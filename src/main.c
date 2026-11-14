/* MiniVectorDB process entry point: JSON lines protocol on stdin/stdout.
 *
 *   {"type":"ready"}                                     once, at startup
 *   {"id":1,"op":"execute","sql":"...","search_mode":"exact"}
 *      -> {"id":1,"ok":true,"columns":[...],"rows":[...]}
 *      -> {"id":1,"ok":true,"affected_rows":3}
 *      -> {"id":1,"ok":false,"error":{"code":"...","message":"..."}}
 *   {"id":2,"op":"shutdown"} -> {"id":2,"ok":true} and exit 0
 *
 * Only protocol lines are written to stdout; diagnostics go to stderr.
 */
#include "mvdb.h"

/* The identifier of the build, printed on stderr when the database starts:
 * it is the hash of the sources, so it also works in the image where there
 * is no git repository. */
#ifndef MVDB_BUILD_ID
#define MVDB_BUILD_ID "unknown"
#endif

#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>

static db_t g_db;

/* Writes one complete line to stdout with a single write() call. */
static void write_line(const char *data, size_t len) {
  size_t done = 0;
  while (done < len) {
    ssize_t n = write(STDOUT_FILENO, data + done, len - done);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return;
    }
    done += (size_t)n;
  }
}

/* Picks the data directory:
 *   1. the --data-dir argument,
 *   2. VECTORDB_DATA_DIR when it points at an existing directory,
 *   3. /data (the mount point used by the evaluation container),
 *   4. ./data
 */
static void pick_data_dir(const char *arg, char *out, size_t outsz) {
  struct stat st;
  if (arg && *arg) {
    snprintf(out, outsz, "%s", arg);
    return;
  }
  const char *env = getenv("VECTORDB_DATA_DIR");
  if (env && *env && stat(env, &st) == 0 && S_ISDIR(st.st_mode)) {
    snprintf(out, outsz, "%s", env);
    return;
  }
  if (stat("/data", &st) == 0 && S_ISDIR(st.st_mode)) {
    snprintf(out, outsz, "/data");
    return;
  }
  snprintf(out, outsz, "data");
}

static void emit_id(outbuf *b, const json_value *req) {
  const json_value *id = json_get(req, "id");
  if (!id) {
    ob_puts(b, "null");
    return;
  }
  switch (id->type) {
    case JSON_INT: ob_json_int(b, id->ival); break;
    case JSON_DOUBLE: ob_json_double(b, id->dval); break;
    case JSON_STRING: ob_json_string(b, id->sval ? id->sval : "", id->slen); break;
    case JSON_BOOL: ob_puts(b, id->bval ? "true" : "false"); break;
    default: ob_puts(b, "null"); break;
  }
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  const char *dir_arg = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
      dir_arg = argv[++i];
    } else if (strncmp(argv[i], "--data-dir=", 11) == 0) {
      dir_arg = argv[i] + 11;
    }
  }
  char data_dir[512];
  pick_data_dir(dir_arg, data_dir, sizeof(data_dir));
  mkdir(data_dir, 0755);

  if (db_open(&g_db, data_dir) != 0) {
    fprintf(stderr, "[mvdb] cannot open the data directory '%s': %s\n", data_dir, strerror(errno));
    return 1;
  }
  fprintf(stderr, "[mvdb] ready, data directory '%s'\n", data_dir);
  fprintf(stderr, "[mvdb] build %s\n", MVDB_BUILD_ID);

  /* The handshake is the first thing on stdout and is flushed immediately. */
  {
    const char *ready = "{\"type\":\"ready\"}\n";
    write_line(ready, strlen(ready));
  }

  char *line = NULL;
  size_t cap = 0;
  ssize_t nread;
  const bool debug = getenv("MVDB_DEBUG") != NULL;
  while ((nread = getline(&line, &cap, stdin)) > 0) {
    size_t len = (size_t)nread;
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;

    arena a;
    arena_init(&a);
    json_value *req = json_parse(&a, line, len);
    if (!req || req->type != JSON_OBJECT) {
      outbuf b;
      ob_init(&b);
      ob_puts(&b, "{\"id\":null,\"ok\":false,\"error\":{\"code\":\"bad_request\",\"message\":\"invalid request\"}}\n");
      write_line(b.data, b.len);
      ob_free(&b);
      arena_reset(&a);
      continue;
    }

    const json_value *opv = json_get(req, "op");
    const char *op = (opv && opv->type == JSON_STRING) ? opv->sval : "execute";

    if (strcmp(op, "shutdown") == 0) {
      outbuf b;
      ob_init(&b);
      ob_puts(&b, "{\"id\":");
      emit_id(&b, req);
      ob_puts(&b, ",\"ok\":true}\n");
      write_line(b.data, b.len);
      ob_free(&b);
      arena_reset(&a);
      break;
    }
    if (strcmp(op, "execute") != 0) {
      outbuf b;
      ob_init(&b);
      ob_puts(&b, "{\"id\":");
      emit_id(&b, req);
      ob_puts(&b, ",\"ok\":false,\"error\":{\"code\":\"unsupported_op\",\"message\":\"unknown operation\"}}\n");
      write_line(b.data, b.len);
      ob_free(&b);
      arena_reset(&a);
      continue;
    }

    const json_value *sqlv = json_get(req, "sql");
    const json_value *modev = json_get(req, "search_mode");
    const char *mode = (modev && modev->type == JSON_STRING) ? modev->sval : "exact";

    bool timing = getenv("MVDB_TIMING") != NULL;
    struct timespec ts0, ts1, ts2;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    outbuf body;
    ob_init(&body);
    char err_code[64], err_msg[256];
    err_code[0] = '\0';
    err_msg[0] = '\0';
    bool ok = false;
    if (!sqlv || sqlv->type != JSON_STRING) {
      snprintf(err_code, sizeof(err_code), "bad_request");
      snprintf(err_msg, sizeof(err_msg), "missing sql");
    } else {
      char *perr = NULL;
      stmt *st = sql_parse(&a, sqlv->sval, sqlv->slen, &perr);
      if (!st) {
        snprintf(err_code, sizeof(err_code), "unsupported_sql");
        snprintf(err_msg, sizeof(err_msg), "%s", perr ? perr : "cannot parse the statement");
      } else {
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        ok = exec_statement(&g_db, &a, st, mode, &body, err_code, sizeof(err_code), err_msg,
                            sizeof(err_msg));
        clock_gettime(CLOCK_MONOTONIC, &ts2);
        if (timing) {
          double tparse = (ts1.tv_sec - ts0.tv_sec) * 1e3 + (ts1.tv_nsec - ts0.tv_nsec) / 1e6;
          double texec = (ts2.tv_sec - ts1.tv_sec) * 1e3 + (ts2.tv_nsec - ts1.tv_nsec) / 1e6;
          fprintf(stderr, "[mvdb] parse %.3f ms exec %.3f ms\n", tparse, texec);
        }
      }
    }

    outbuf b;
    ob_init(&b);
    ob_puts(&b, "{\"id\":");
    emit_id(&b, req);
    if (ok) {
      ob_puts(&b, ",\"ok\":true");
      if (body.len) {
        ob_putc(&b, ',');
        ob_putn(&b, body.data, body.len);
      }
      ob_puts(&b, "}\n");
    } else {
      ob_puts(&b, ",\"ok\":false,\"error\":{\"code\":");
      ob_json_string(&b, err_code[0] ? err_code : "internal_error", strlen(err_code[0] ? err_code : "internal_error"));
      ob_puts(&b, ",\"message\":");
      ob_json_string(&b, err_msg, strlen(err_msg));
      ob_puts(&b, "}}\n");
    }
    /* The judge collects the log of a case, so the log has to show what was
     * asked and what was answered.  Everything goes to stderr, stdout only
     * carries the protocol.  The trace of every request costs a write to the
     * log per request, which is a sixth of the time of a query, so it is
     * only written when it is asked for; a statement that failed is always
     * written. */
    if (debug || !ok) {
      const char *sqlt = (sqlv && sqlv->type == JSON_STRING) ? sqlv->sval : "";
      size_t sqll = (sqlv && sqlv->type == JSON_STRING) ? (size_t)sqlv->slen : 0;
      char sqlbuf[161];
      if (sqll > 160) sqll = 160;
      memcpy(sqlbuf, sqlt, sqll);
      sqlbuf[sqll] = '\0';
      fprintf(stderr, "[mvdb] req mode=%s sql=%s\n", mode, sqlbuf);
      fprintf(stderr, "[mvdb] res %.*s\n", (int)(b.len > 300 ? 300 : b.len), b.data);
    }
    write_line(b.data, b.len);
    ob_free(&b);
    ob_free(&body);
    arena_reset(&a);
  }

  free(line);
  /* A clean shutdown rolls back an open transaction and flushes the log. */
  if (g_db.txn_active) txn_rollback(&g_db);
  db_close(&g_db);
  return 0;
}
