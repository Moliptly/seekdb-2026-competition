/* Storage engine: append only transaction log, per table vector payload
 * files, in memory row store and the transaction machinery.
 *
 * Durability contract (matches the contest rules):
 *   - a statement that has answered "ok" is durable on disk;
 *   - a transaction without a commit frame is never applied, so it can not
 *     partially survive a crash;
 *   - the log is written and fsync'ed before the commit reply is produced.
 */
#include "mvdb.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FRAME_HDR 8      /* u32 len + u32 crc */
#define LOG_TXN_BEGIN 0x01
#define LOG_TXN_COMMIT 0x02
#define LOG_TXN_ABORT 0x03
#define LOG_CREATE_TABLE 0x10
#define LOG_INSERT_ROW 0x11
#define LOG_UPDATE_ROW 0x12
#define LOG_DELETE_ROW 0x13

/* --------------------------------------------------------------- crc32 */

static uint32_t crc_table[256];
static bool crc_ready = false;

static void crc_init(void) {
  if (crc_ready) return;
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    crc_table[i] = c;
  }
  crc_ready = true;
}

static uint32_t crc32_buf(const void *data, size_t len) {
  crc_init();
  const unsigned char *p = (const unsigned char *)data;
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------- little helpers */

static void put_u8(outbuf *b, uint8_t v) { ob_putc(b, (char)v); }
static void put_u16(outbuf *b, uint16_t v) {
  unsigned char t[2] = {(unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF)};
  ob_putn(b, (const char *)t, 2);
}
static void put_u32(outbuf *b, uint32_t v) {
  unsigned char t[4];
  for (int i = 0; i < 4; i++) t[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
  ob_putn(b, (const char *)t, 4);
}
static void put_u64(outbuf *b, uint64_t v) {
  unsigned char t[8];
  for (int i = 0; i < 8; i++) t[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
  ob_putn(b, (const char *)t, 8);
}
static void put_i64(outbuf *b, int64_t v) { put_u64(b, (uint64_t)v); }

static uint32_t get_u32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t get_u16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint64_t get_u64(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}
static int64_t get_i64(const unsigned char *p) { return (int64_t)get_u64(p); }

/* ------------------------------------------------------------- row store */

static void table_init_columns(table_t *t) {
  t->ints = (int64_t **)calloc((size_t)t->ncols, sizeof(int64_t *));
  t->strs = (char ***)calloc((size_t)t->ncols, sizeof(char **));
  t->strlens = (uint32_t **)calloc((size_t)t->ncols, sizeof(uint32_t *));
  t->vecs = (float ***)calloc((size_t)t->ncols, sizeof(float *));
  t->eqidx = (eqidx_t *)calloc((size_t)t->ncols, sizeof(eqidx_t));
}

static void table_alloc_rows(table_t *t, uint64_t need) {
  if (need <= t->nrows) return;
  uint64_t cap = t->nrows ? t->nrows : 64;
  while (cap < need) cap *= 2;
  t->pk = (uint64_t *)realloc(t->pk, (size_t)cap * sizeof(uint64_t));
  t->alive = (uint8_t *)realloc(t->alive, (size_t)cap);
  if (!t->pk || !t->alive) return;
  memset(t->alive + t->nrows, 0, (size_t)(cap - t->nrows));
  for (int c = 0; c < t->ncols; c++) {
    if (t->cols[c].type == CT_BIGINT) {
      t->ints[c] = (int64_t *)realloc(t->ints[c], (size_t)cap * sizeof(int64_t));
      if (t->ints[c]) memset(t->ints[c] + t->nrows, 0, (size_t)(cap - t->nrows) * sizeof(int64_t));
    } else if (t->cols[c].type == CT_VARCHAR) {
      t->strs[c] = (char **)realloc(t->strs[c], (size_t)cap * sizeof(char *));
      t->strlens[c] = (uint32_t *)realloc(t->strlens[c], (size_t)cap * sizeof(uint32_t));
      if (t->strs[c]) {
        memset(t->strs[c] + t->nrows, 0, (size_t)(cap - t->nrows) * sizeof(char *));
        memset(t->strlens[c] + t->nrows, 0, (size_t)(cap - t->nrows) * sizeof(uint32_t));
      }
    } else if (t->cols[c].type == CT_VECTOR) {
      t->vecs[c] = (float **)realloc(t->vecs[c], (size_t)cap * sizeof(float *));
      if (t->vecs[c]) memset(t->vecs[c] + t->nrows, 0, (size_t)(cap - t->nrows) * sizeof(float *));
      t->vec_slot = (uint64_t *)realloc(t->vec_slot, (size_t)cap * sizeof(uint64_t));
      if (t->vec_slot) memset(t->vec_slot + t->nrows, 0, (size_t)(cap - t->nrows) * sizeof(uint64_t));
    }
  }
  t->nrows = cap;
}

/* Primary key hash map (open addressing, power of two capacity). */
static void pkmap_init(table_t *t) {
  t->hash_mask = 1023;
  t->hash_key = (uint64_t *)calloc(t->hash_mask + 1, sizeof(uint64_t));
  t->hash_val = (uint64_t *)calloc(t->hash_mask + 1, sizeof(uint64_t));
  t->hash_used = 0;
}

static uint64_t hash_u64(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

static bool pkmap_get(table_t *t, uint64_t key, uint64_t *val) {
  if (!t->hash_key) return false;
  uint32_t i = (uint32_t)(hash_u64(key) & t->hash_mask);
  for (;;) {
    if (t->hash_key[i] == 0) return false;
    if (t->hash_key[i] == key) { *val = t->hash_val[i]; return true; }
    i = (i + 1) & t->hash_mask;
  }
}

static void pkmap_put(table_t *t, uint64_t key, uint64_t val);

static void pkmap_grow(table_t *t) {
  uint32_t old_mask = t->hash_mask;
  uint64_t *old_key = t->hash_key;
  uint64_t *old_val = t->hash_val;
  t->hash_mask = old_mask * 2 + 1;
  t->hash_key = (uint64_t *)calloc(t->hash_mask + 1, sizeof(uint64_t));
  t->hash_val = (uint64_t *)calloc(t->hash_mask + 1, sizeof(uint64_t));
  t->hash_used = 0;
  for (uint32_t i = 0; i <= old_mask; i++) {
    if (old_key[i]) pkmap_put(t, old_key[i], old_val[i]);
  }
  free(old_key);
  free(old_val);
}

static void pkmap_put(table_t *t, uint64_t key, uint64_t val) {
  if (!t->hash_key) pkmap_init(t);
  if ((t->hash_used + 1) * 10 > (t->hash_mask + 1) * 7) pkmap_grow(t);
  uint32_t i = (uint32_t)(hash_u64(key) & t->hash_mask);
  for (;;) {
    if (t->hash_key[i] == 0) {
      t->hash_key[i] = key;
      t->hash_val[i] = val;
      t->hash_used++;
      return;
    }
    if (t->hash_key[i] == key) { t->hash_val[i] = val; return; }
    i = (i + 1) & t->hash_mask;
  }
}

static void pkmap_del(table_t *t, uint64_t key) {
  if (!t->hash_key) return;
  uint32_t i = (uint32_t)(hash_u64(key) & t->hash_mask);
  for (;;) {
    if (t->hash_key[i] == 0) return;
    if (t->hash_key[i] == key) {
      t->hash_key[i] = 0;
      t->hash_val[i] = 0;
      uint32_t j = (i + 1) & t->hash_mask;
      while (t->hash_key[j]) {
        uint64_t k = t->hash_key[j], v = t->hash_val[j];
        t->hash_key[j] = 0;
        t->hash_val[j] = 0;
        t->hash_used--;
        pkmap_put(t, k, v);
        j = (j + 1) & t->hash_mask;
      }
      t->hash_used--;
      return;
    }
    i = (i + 1) & t->hash_mask;
  }
}

/* ------------------------------------------------------------ table api */

table_t *db_find_table(db_t *db, const char *name) {
  for (int i = 0; i < db->ntables; i++) {
    if (strcasecmp(db->tables[i]->name, name) == 0) return db->tables[i];
  }
  return NULL;
}

static table_t *table_new(db_t *db, uint64_t id, const char *name, const column_t *cols,
                          int ncols, int pk_idx, int vec_idx) {
  if (db->ntables >= 32) return NULL;
  table_t *t = (table_t *)arena_calloc(&db->arena, sizeof(table_t));
  if (!t) return NULL;
  t->id = id;
  snprintf(t->name, sizeof(t->name), "%s", name);
  t->ncols = ncols;
  for (int i = 0; i < ncols; i++) t->cols[i] = cols[i];
  t->pk_idx = pk_idx;
  t->vec_idx = vec_idx;
  t->dim = (vec_idx >= 0) ? cols[vec_idx].dim : 0;
  t->vec_fd = -1;
  t->vec_cached = true;
  table_init_columns(t);
  pkmap_init(t);
  db->tables[db->ntables++] = t;
  return t;
}

/* The vector payload file holds the float32 values with a fixed stride:
 * rowid * dim * 4 bytes.  Keeping the payload out of the log means every
 * vector byte is written exactly once. */
static int table_open_vector_file(db_t *db, table_t *t) {
  if (t->vec_idx < 0) return 0;
  snprintf(t->vec_path, sizeof(t->vec_path), "%s/vec_%llu.dat", db->data_dir,
           (unsigned long long)t->id);
  int fd = open(t->vec_path, O_RDWR | O_CREAT, 0644);
  if (fd < 0) return -1;
  t->vec_fd = fd;
  /* The number of slots the pool holds decides how much of it can be read at
   * once; after a restart it is the size of the file. */
  struct stat st;
  if (fstat(fd, &st) == 0 && st.st_size > 0) {
    size_t bytes = (size_t)t->dim * sizeof(float);
    if (bytes) t->vec_pool_rows = (uint64_t)(st.st_size / bytes);
  }
  return 0;
}

/* Reads a vector from the payload pool, using a small read cache so that a
 * sequential scan does not issue one system call per row. */
bool table_pool_read(table_t *t, uint64_t slot, float *dst) {
  if (t->vec_fd < 0) return false;
  size_t bytes = (size_t)t->dim * sizeof(float);
  if (!t->pool_buf) {
    t->pool_buf_cap = 1u << 20;
    t->pool_buf = (unsigned char *)malloc(t->pool_buf_cap);
    t->pool_buf_slots = 0;
    if (!t->pool_buf) { t->pool_buf_cap = 0; return false; }
  }
  size_t slots_per_buf = t->pool_buf_cap / bytes;
  if (slots_per_buf == 0) slots_per_buf = 1;
  if (slot >= t->pool_buf_slot && slot < t->pool_buf_slot + t->pool_buf_slots) {
    memcpy(dst, t->pool_buf + (size_t)(slot - t->pool_buf_slot) * bytes, bytes);
    return true;
  }
  uint64_t start = (slot / slots_per_buf) * slots_per_buf;
  size_t want = slots_per_buf;
  if (start + want > t->vec_pool_rows) want = (size_t)(t->vec_pool_rows - start);
  if (want == 0) return false;
  size_t nbytes = want * bytes;
  size_t got = 0;
  while (got < nbytes) {
    ssize_t n = pread(t->vec_fd, t->pool_buf + got, nbytes - got,
                      (off_t)((start * (uint64_t)bytes) + got));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      break;
    }
    got += (size_t)n;
  }
  if (got < bytes) return false;
  t->pool_buf_slot = start;
  t->pool_buf_slots = got / bytes;
  memcpy(dst, t->pool_buf + (size_t)(slot - start) * bytes, bytes);
  return true;
}

/* Returns the vector of a row, from the memory cache or from the pool. */
float *table_row_vector(table_t *t, int vcol, uint64_t rowid) {
  if (vcol < 0 || vcol >= t->ncols) return NULL;
  const float *v = t->vecs[vcol] ? t->vecs[vcol][rowid] : NULL;
  if (v) return (float *)v;
  if (rowid >= t->nrows) return NULL;
  if (!t->vec_buf) {
    t->vec_buf = (float *)malloc((size_t)t->dim * sizeof(float));
    if (!t->vec_buf) return NULL;
  }
  if (!table_pool_read(t, t->vec_slot[rowid], t->vec_buf)) return NULL;
  return t->vec_buf;
}

/* The parameters of a scan, shared by the workers. */
typedef struct {
  const float *const *vecs;
  const uint8_t *alive;
  const uint64_t *rows;   /* NULL: all the rows of the table */
  uint64_t row_begin;    /* the first row id when rows is NULL */
  const float *q;
  int dim;
  table_t *table;        /* set when the vectors are read from the pool */
  int vcol;
} scan_ctx_t;

/* Scans the candidate range [begin, end). */
static size_t scan_range_run(void *ctxv, size_t begin, size_t end, dres_t *out, size_t cap) {
  scan_ctx_t *ctx = (scan_ctx_t *)ctxv;
  if (ctx->table) {
    /* the vectors are not cached in memory: they come from the payload pool */
    size_t m = 0;
    for (size_t i = begin; i < end && m < cap; i++) {
      uint64_t r = ctx->rows ? ctx->rows[i] : (ctx->row_begin + i);
      if (ctx->alive && !ctx->alive[r]) continue;
      const float *v = table_row_vector(ctx->table, ctx->vcol, r);
      if (!v) continue;
      out[m].rowid = r;
      out[m].sq = vec_l2_sq(v, ctx->q, ctx->dim);
      m++;
    }
    return m;
  }
  const uint64_t *rows = ctx->rows ? ctx->rows + begin : NULL;
  size_t nrows = end - begin;
  uint64_t row_begin = ctx->rows ? 0 : ctx->row_begin + begin;
  return vec_scan_sq_range((const float *const *)ctx->vecs, ctx->alive, rows, nrows, row_begin,
                           row_begin + nrows, ctx->q, ctx->dim, out);
}

/* The scan of the query path.  A table that caches its vectors uses the
 * vectorised batch scan, a table that exceeded the memory budget reads the
 * vectors from the payload pool instead.  The work is split over the
 * workers of the pool; the result does not depend on the split. */
size_t table_scan_sq(table_t *t, int vcol, const uint8_t *alive, const uint64_t *rows,
                      size_t nrows, const float *q, int dim, dres_t *out) {
  scan_ctx_t ctx;
  ctx.vecs = (const float *const *)t->vecs[vcol];
  ctx.alive = alive;
  ctx.rows = rows;
  ctx.row_begin = 0;
  ctx.q = q;
  ctx.dim = dim;
  ctx.table = t->vec_cached ? NULL : t;
  ctx.vcol = vcol;
  /* The scan is limited by the memory bandwidth, not by the core: a
   * measurement with one to eight threads showed 12.6 to 13.8 GB/s, so the
   * work is not split over workers. */
  return scan_range_run(&ctx, 0, nrows, out, nrows);
}

static bool table_write_vector(table_t *t, uint64_t slot, const float *vec) {
  if (t->vec_fd < 0) return true;
  size_t bytes = (size_t)t->dim * sizeof(float);
  off_t off = (off_t)(slot * (uint64_t)bytes);
  size_t done = 0;
  while (done < bytes) {
    ssize_t n = pwrite(t->vec_fd, (const char *)vec + done, bytes - done, off + (off_t)done);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    done += (size_t)n;
  }
  if (slot + 1 > t->vec_pool_rows) t->vec_pool_rows = slot + 1;
  return true;
}

/* Reads a vector from the pool. */
static bool table_read_vector(table_t *t, uint64_t slot, float *dst) {
  if (t->vec_fd < 0) return false;
  size_t bytes = (size_t)t->dim * sizeof(float);
  size_t done = 0;
  while (done < bytes) {
    ssize_t n = pread(t->vec_fd, (char *)dst + done, bytes - done,
                      (off_t)(slot * (uint64_t)bytes) + (off_t)done);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    done += (size_t)n;
  }
  return true;
}

/* ------------------------------------------------------------- log write */

static bool log_append(db_t *db, const void *payload, size_t len, uint8_t type) {
  unsigned char hdr[FRAME_HDR + 1];
  uint32_t crc = crc32_buf(payload, len);
  hdr[0] = (unsigned char)(len & 0xFF);
  hdr[1] = (unsigned char)((len >> 8) & 0xFF);
  hdr[2] = (unsigned char)((len >> 16) & 0xFF);
  hdr[3] = (unsigned char)((len >> 24) & 0xFF);
  hdr[4] = (unsigned char)(crc & 0xFF);
  hdr[5] = (unsigned char)((crc >> 8) & 0xFF);
  hdr[6] = (unsigned char)((crc >> 16) & 0xFF);
  hdr[7] = (unsigned char)((crc >> 24) & 0xFF);
  hdr[8] = type;
  size_t done = 0;
  size_t hlen = FRAME_HDR + 1;
  while (done < hlen) {
    ssize_t n = write(db->log_fd, (const char *)hdr + done, hlen - done);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    done += (size_t)n;
  }
  done = 0;
  while (done < len) {
    ssize_t n = write(db->log_fd, (const char *)payload + done, len - done);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    done += (size_t)n;
  }
  db->log_size += hlen + len;
  return true;
}

static bool log_sync(db_t *db) {
  if (fdatasync(db->log_fd) != 0) {
    if (errno == EINVAL) return fsync(db->log_fd) == 0;
    return false;
  }
  return true;
}

/* ------------------------------------------------------------ transactions */

static void txn_reset(db_t *db) {
  db->undo_n = 0;
  db->dirty_n = 0;
  db->ddl_n = 0;
  db->txn_active = false;
  db->in_statement = false;
}

static void undo_push(db_t *db, const undo_entry *e) {
  if (db->undo_n == db->undo_cap) {
    size_t cap = db->undo_cap ? db->undo_cap * 2 : 256;
    undo_entry *n = (undo_entry *)realloc(db->undo, cap * sizeof(undo_entry));
    if (!n) { db->fatal = true; return; }
    db->undo = n;
    db->undo_cap = cap;
  }
  db->undo[db->undo_n++] = *e;
}

static void dirty_push(db_t *db, uint64_t table_id, uint64_t rowid, uint8_t kind) {
  for (size_t i = 0; i < db->dirty_n; i++) {
    if (db->dirty[i].table_id == table_id && db->dirty[i].rowid == rowid) {
      /* an insert that later becomes a delete still needs a delete frame */
      if (kind == 2) db->dirty[i].kind = 2;
      return;
    }
  }
  if (db->dirty_n == db->dirty_cap) {
    size_t cap = db->dirty_cap ? db->dirty_cap * 2 : 256;
    dirty_row *n = (dirty_row *)realloc(db->dirty, cap * sizeof(dirty_row));
    if (!n) { db->fatal = true; return; }
    db->dirty = n;
    db->dirty_cap = cap;
  }
  db->dirty[db->dirty_n].table_id = table_id;
  db->dirty[db->dirty_n].rowid = rowid;
  db->dirty[db->dirty_n].kind = kind;
  db->dirty_n++;
}

static bool log_create_table_frame(db_t *db, table_t *t);

static table_t *table_by_id(db_t *db, uint64_t id) {
  for (int i = 0; i < db->ntables; i++) {
    if (db->tables[i]->id == id) return db->tables[i];
  }
  return NULL;
}

/* Rolls the open transaction back to the state before it started. */
void txn_rollback(db_t *db) {
  /* tables created by the transaction disappear again */
  for (int i = 0; i < db->ddl_n; i++) {
    for (int k = db->ntables - 1; k >= 0; k--) {
      if (db->tables[k]->id == db->ddl_ids[i]) {
        if (db->tables[k]->vec_fd >= 0) close(db->tables[k]->vec_fd);
        free(db->tables[k]->hash_key);
        free(db->tables[k]->hash_val);
        for (int c = 0; c < db->tables[k]->ncols; c++) {
          if (db->tables[k]->cols[c].type == CT_BIGINT) free(db->tables[k]->ints[c]);
          else if (db->tables[k]->cols[c].type == CT_VARCHAR) { free(db->tables[k]->strs[c]); free(db->tables[k]->strlens[c]); }
          else if (db->tables[k]->cols[c].type == CT_VECTOR) free(db->tables[k]->vecs[c]);
        }
        free(db->tables[k]->ints);
        free(db->tables[k]->strs);
        free(db->tables[k]->strlens);
        free(db->tables[k]->vecs);
        free(db->tables[k]->pk);
        free(db->tables[k]->alive);
        for (int m = k; m + 1 < db->ntables; m++) db->tables[m] = db->tables[m + 1];
        db->ntables--;
        break;
      }
    }
  }
  for (size_t i = db->undo_n; i > 0; i--) {
    undo_entry *e = &db->undo[i - 1];
    table_t *t = table_by_id(db, e->table_id);
    if (!t) continue;
    uint64_t rowid = e->rowid;
    if (rowid >= t->nrows) continue;
    if (e->ctype == CT_BIGINT) {
      /* The row leaves the list of the value it has now and joins the list of
       * the value it gets back, so that the index follows the rollback. */
      eqidx_detach(t, e->col, rowid);
      t->ints[e->col][rowid] = e->old_int;
      eqidx_add(t, e->col, rowid);
    } else if (e->ctype == CT_VARCHAR) {
      eqidx_detach(t, e->col, rowid);
      t->strs[e->col][rowid] = e->old_str;
      t->strlens[e->col][rowid] = e->old_strlen;
      eqidx_add(t, e->col, rowid);
    } else if (e->ctype == CT_VECTOR) {
      t->vecs[e->col][rowid] = e->old_vec;
      t->vec_slot[rowid] = e->old_slot;
    }
    if (e->deleted) {
      /* the row was removed by this transaction: it comes back */
      if (!t->alive[rowid]) {
        t->alive[rowid] = 1;
        t->nlive++;
        if (t->pk_idx >= 0) pkmap_put(t, t->pk[rowid], rowid);
      }
    } else if (!e->existed) {
      /* the row was created by this transaction: remove it again */
      if (t->alive[rowid]) {
        t->alive[rowid] = 0;
        t->nlive--;
        if (t->pk_idx >= 0) pkmap_del(t, t->pk[rowid]);
      }
    } else if (t->alive[rowid] && t->pk_idx >= 0) {
      pkmap_put(t, t->pk[rowid], rowid);
    }
  }
  txn_reset(db);
}

/* Serialises one row (metadata only, vectors live in the payload file). */
static bool log_row_frame(db_t *db, table_t *t, uint64_t rowid, uint8_t kind) {
  /* the buffer is reused across the rows of a transaction: one allocation per
   * row would show up in the profile of the write path */
  outbuf *bp = &db->frame_buf;
  ob_free(bp);
  ob_init(bp);
  outbuf *b = bp;
  put_u64(b, db->txn_id);
  put_u64(b, t->id);
  put_u64(b, rowid);
  if (kind == 2) {
    bool ok = !b->oom && log_append(db, b->data ? b->data : "", b->len, LOG_DELETE_ROW);
    ob_free(bp);
    return ok;
  }
  put_i64(b, (int64_t)t->pk[rowid]);
  if (t->vec_idx >= 0) put_u64(b, t->vec_slot[rowid]);
  uint16_t ncols = 0;
  for (int c = 0; c < t->ncols; c++) {
    if (t->cols[c].type == CT_VECTOR) continue;
    ncols++;
  }
  put_u16(b, ncols);
  for (int c = 0; c < t->ncols; c++) {
    if (t->cols[c].type == CT_VECTOR) continue;
    put_u8(b, (uint8_t)t->cols[c].type);
    if (t->cols[c].type == CT_BIGINT) {
      put_i64(b, t->ints[c][rowid]);
    } else {
      uint32_t len = t->strlens[c][rowid];
      put_u32(b, len);
      if (len) ob_putn(b, t->strs[c][rowid], len);
    }
  }
  bool ok = !b->oom &&
            log_append(db, b->data ? b->data : "", b->len,
                       kind == 0 ? LOG_INSERT_ROW : LOG_UPDATE_ROW);
  ob_free(bp);
  return ok;
}

/* Writes the pending transaction to the log and makes it durable. */
bool txn_commit(db_t *db) {
  bool ok = true;
  /* 1. the vector payloads were written when the rows were applied; they
   * must be durable before the commit frame */
  int synced_fd = -1;
  for (size_t i = 0; i < db->dirty_n && ok; i++) {
    dirty_row *d = &db->dirty[i];
    table_t *t = table_by_id(db, d->table_id);
    if (!t || t->vec_idx < 0) continue;
    if (d->kind == 2) continue;
    if (!t->alive[d->rowid]) continue;
    if (synced_fd < 0) synced_fd = t->vec_fd;
  }
  if (ok && synced_fd >= 0) {
    if (fdatasync(synced_fd) != 0 && errno != EINVAL) ok = false;
  }
  /* 2. table definitions created by this transaction */
  for (int i = 0; i < db->ddl_n && ok; i++) {
    table_t *t = table_by_id(db, db->ddl_ids[i]);
    if (!t) continue;
    if (!log_create_table_frame(db, t)) { ok = false; break; }
  }
  /* 2b. row frames */
  for (size_t i = 0; i < db->dirty_n && ok; i++) {
    dirty_row *d = &db->dirty[i];
    table_t *t = table_by_id(db, d->table_id);
    if (!t) continue;
    if (d->kind == 3) continue; /* ddl marker, no row frame */
    if (!log_row_frame(db, t, d->rowid, d->kind)) { ok = false; break; }
  }
  /* 3. commit frame + durability */
  if (ok) {
    unsigned char tid[8];
    for (int i = 0; i < 8; i++) tid[i] = (unsigned char)((db->txn_id >> (8 * i)) & 0xFF);
    if (!log_append(db, tid, 8, LOG_TXN_COMMIT)) ok = false;
  }
  if (ok && !log_sync(db)) ok = false;
  if (!ok) db->fatal = true;
  txn_reset(db);
  return ok;
}

/* --------------------------------------------------------------- recovery */

typedef struct {
  uint8_t type;
  uint32_t len;
  unsigned char *data;
} pending_frame;

typedef struct {
  int fd;
  unsigned char buf[65536];
  size_t len, pos;
} lreader;

static bool lr_fill(lreader *r) {
  if (r->pos < r->len) return true;
  ssize_t n = read(r->fd, r->buf, sizeof(r->buf));
  if (n <= 0) return false;
  r->pos = 0;
  r->len = (size_t)n;
  return true;
}

static bool lr_read(lreader *r, void *dst, size_t n) {
  unsigned char *p = (unsigned char *)dst;
  size_t got = 0;
  while (got < n) {
    if (!lr_fill(r)) return false;
    size_t avail = r->len - r->pos;
    size_t take = (n - got < avail) ? (n - got) : avail;
    memcpy(p + got, r->buf + r->pos, take);
    r->pos += take;
    got += take;
  }
  return true;
}

/* Applies one recovered row frame. */
static void recover_row(db_t *db, uint8_t type, const unsigned char *p, size_t len) {
  size_t off = 0;
  if (len < 24) return;
  uint64_t txn_id = get_u64(p + off); off += 8;
  uint64_t table_id = get_u64(p + off); off += 8;
  uint64_t rowid = get_u64(p + off); off += 8;
  (void)txn_id;
  table_t *t = table_by_id(db, table_id);
  if (!t) return;
  table_alloc_rows(t, rowid + 1);
  if (type == LOG_DELETE_ROW) {
    if (rowid < t->nrows && t->alive[rowid]) {
      t->alive[rowid] = 0;
      t->nlive--;
      if (t->pk_idx >= 0) pkmap_del(t, t->pk[rowid]);
    }
    return;
  }
  if (off + 8 > len) return;
  int64_t pk = get_i64(p + off); off += 8;
  uint64_t slot = 0;
  if (t->vec_idx >= 0) {
    if (off + 8 > len) return;
    slot = get_u64(p + off);
    off += 8;
  }
  if (off + 2 > len) return;
  uint16_t ncols = get_u16(p + off); off += 2;
  int col = 0;
  for (uint16_t i = 0; i < ncols; i++) {
    if (off + 1 > len) return;
    uint8_t ct = p[off++];
    if (ct == CT_BIGINT) {
      if (off + 8 > len) return;
      int64_t v = get_i64(p + off); off += 8;
      while (col < t->ncols && t->cols[col].type == CT_VECTOR) col++;
      if (col < t->ncols) t->ints[col][rowid] = v;
      col++;
    } else {
      if (off + 4 > len) return;
      uint32_t slen = get_u32(p + off); off += 4;
      if (off + slen > len) return;
      while (col < t->ncols && t->cols[col].type == CT_VECTOR) col++;
      if (col < t->ncols) {
        char *copy = (char *)arena_strndup(&db->arena, (const char *)p + off, slen);
        t->strs[col][rowid] = copy;
        t->strlens[col][rowid] = slen;
      }
      off += slen;
      col++;
    }
  }
  bool was_alive = rowid < t->nrows ? t->alive[rowid] : 0;
  t->pk[rowid] = (uint64_t)pk;
  if (t->vec_idx >= 0) {
    t->vec_slot[rowid] = slot;
    if (slot + 1 > t->vec_next_slot) t->vec_next_slot = slot + 1;
    if (slot + 1 > t->vec_pool_rows) t->vec_pool_rows = slot + 1;
    size_t bytes = (size_t)t->dim * sizeof(float);
    if (db->vec_cache_bytes + bytes > db->vec_cache_limit) {
      t->vec_cached = false;
    } else {
      float *v = (float *)arena_alloc(&db->arena, bytes);
      if (v && table_read_vector(t, slot, v)) {
        t->vecs[t->vec_idx][rowid] = v;
        db->vec_cache_bytes += bytes;
        t->vec_bytes += bytes;
      }
    }
  }
  if (!was_alive) {
    t->alive[rowid] = 1;
    t->nlive++;
  }
  if (t->pk_idx >= 0) pkmap_put(t, (uint64_t)pk, rowid);
  if (rowid + 1 > t->next_rowid) t->next_rowid = rowid + 1;
}

static void recover_create_table(db_t *db, const unsigned char *p, size_t len) {
  size_t off = 0;
  if (len < 16) return;
  off += 8; /* transaction id */
  uint64_t id = get_u64(p + off); off += 8;
  if (off + 2 > len) return;
  uint16_t nlen = get_u16(p + off); off += 2;
  if (off + nlen > len) return;
  char name[64];
  size_t cp = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
  memcpy(name, p + off, cp);
  name[cp] = '\0';
  off += nlen;
  if (off + 2 > len) return;
  uint16_t ncols = get_u16(p + off); off += 2;
  column_t cols[16];
  int pk_idx = -1, vec_idx = -1;
  for (uint16_t i = 0; i < ncols && i < 16; i++) {
    if (off + 1 > len) return;
    uint8_t ct = p[off++];
    if (off + 2 > len) return;
    uint16_t clen = get_u16(p + off); off += 2;
    if (off + clen > len) return;
    char cname[64];
    size_t c2 = clen < sizeof(cname) - 1 ? clen : sizeof(cname) - 1;
    memcpy(cname, p + off, c2);
    cname[c2] = '\0';
    off += clen;
    if (off + 5 > len) return;
    int32_t vlen = (int32_t)get_u32(p + off); off += 4;
    uint8_t flags = p[off++];
    memset(&cols[i], 0, sizeof(cols[i]));
    snprintf(cols[i].name, sizeof(cols[i].name), "%s", cname);
    cols[i].type = (coltype_t)ct;
    cols[i].vlen = vlen;
    cols[i].pk = (flags & 1) != 0;
    if (cols[i].type == CT_VECTOR) { cols[i].dim = vlen; vec_idx = (int)i; }
    if (cols[i].pk) pk_idx = (int)i;
  }
  if (db_find_table(db, name)) return;
  table_t *t = table_new(db, id, name, cols, ncols < 16 ? ncols : 16, pk_idx, vec_idx);
  if (t) table_open_vector_file(db, t);
}

/* Rebuilds the state from the log.  Only committed transactions are applied.
 * The frames of one transaction are written as one contiguous batch followed
 * by its commit frame, so an uncommitted tail is simply discarded. */
static bool db_recover(db_t *db) {
  lreader r;
  r.fd = db->log_fd;
  r.len = 0;
  r.pos = 0;
  pending_frame *pend = NULL;
  size_t pend_n = 0, pend_cap = 0;
  uint64_t pend_txn = 0;
  bool have_pend = false;
  bool ok = true;
  for (;;) {
    unsigned char hdr[FRAME_HDR + 1];
    if (!lr_read(&r, hdr, FRAME_HDR + 1)) break;   /* clean or torn end of file */
    uint32_t len = get_u32(hdr);
    uint32_t crc = get_u32(hdr + 4);
    uint8_t type = hdr[8];
    if (len > (256u << 20)) { ok = false; break; }
    unsigned char *payload = (unsigned char *)malloc(len ? len : 1);
    if (!payload) { ok = false; break; }
    if (len && !lr_read(&r, payload, len)) { free(payload); break; }
    if (crc32_buf(payload, len) != crc) { free(payload); break; }

    if (type == LOG_TXN_COMMIT) {
      uint64_t txn = len >= 8 ? get_u64(payload) : 0;
      if (have_pend && pend_txn == txn) {
        for (size_t i = 0; i < pend_n; i++) {
          if (pend[i].type == LOG_CREATE_TABLE) recover_create_table(db, pend[i].data, pend[i].len);
          else recover_row(db, pend[i].type, pend[i].data, pend[i].len);
          free(pend[i].data);
        }
      } else {
        for (size_t i = 0; i < pend_n; i++) free(pend[i].data);
      }
      pend_n = 0;
      have_pend = false;
      free(payload);
      continue;
    }
    if (type == LOG_TXN_ABORT || type == LOG_TXN_BEGIN) {
      for (size_t i = 0; i < pend_n; i++) free(pend[i].data);
      pend_n = 0;
      have_pend = false;
      free(payload);
      continue;
    }
    uint64_t txn = len >= 8 ? get_u64(payload) : 0;
    if (have_pend && pend_n > 0 && txn != pend_txn) {
      /* a different transaction starts: the previous one never committed */
      for (size_t i = 0; i < pend_n; i++) free(pend[i].data);
      pend_n = 0;
    }
    pend_txn = txn;
    have_pend = true;
    if (pend_n == pend_cap) {
      size_t cap = pend_cap ? pend_cap * 2 : 64;
      pending_frame *np = (pending_frame *)realloc(pend, cap * sizeof(pending_frame));
      if (!np) { free(payload); ok = false; break; }
      pend = np;
      pend_cap = cap;
    }
    pend[pend_n].type = type;
    pend[pend_n].len = len;
    pend[pend_n].data = payload;
    pend_n++;
  }
  for (size_t i = 0; i < pend_n; i++) free(pend[i].data);
  free(pend);
  return ok;
}

/* ------------------------------------------------------------------ open */

int db_open(db_t *db, const char *data_dir) {
  memset(db, 0, sizeof(*db));
  arena_init(&db->arena);
  snprintf(db->data_dir, sizeof(db->data_dir), "%s", data_dir);
  mkdir(data_dir, 0755);
  snprintf(db->log_path, sizeof(db->log_path), "%s/heap.log", data_dir);
  db->log_fd = open(db->log_path, O_RDWR | O_CREAT | O_APPEND, 0644);
  if (db->log_fd < 0) return -1;
  struct stat lst;
  if (fstat(db->log_fd, &lst) == 0 && lst.st_size > 0) db->log_size = (uint64_t)lst.st_size;
  db->next_txn = 1;
  db->vec_cache_limit = 256ull * 1024 * 1024;
  {
    /* the budget can be lowered for the local tests of the large data path */
    const char *env = getenv("MVDB_VEC_CACHE_MB");
    if (env && *env) {
      long mb = strtol(env, NULL, 10);
      if (mb > 0) db->vec_cache_limit = (uint64_t)mb * 1024 * 1024;
    }
  }
  if (!db_recover(db)) {
    /* A damaged tail is tolerated: the log is truncated to the last good
     * frame boundary so that new writes stay append only. */
    fprintf(stderr, "[mvdb] log recovery stopped early, truncating tail\n");
  }
  /* The vector index of a table is restored before the database reports
   * itself ready: a query that arrives right after the handshake has to
   * find the index of the recovered rows. */
  for (int i = 0; i < db->ntables; i++) {
    table_t *t = db->tables[i];
    if (t->vec_idx >= 0 && !ivf_load(db, t)) {
      fprintf(stderr, "[mvdb] no vector index for table %llu\n", (unsigned long long)t->id);
    }
  }
  return 0;
}

void db_close(db_t *db) {
  if (db->log_fd >= 0) {
    log_sync(db);
    close(db->log_fd);
    db->log_fd = -1;
  }
  for (int i = 0; i < db->ntables; i++) {
    table_t *t = db->tables[i];
    if (t->vec_fd >= 0) { close(t->vec_fd); t->vec_fd = -1; }
  }
}

/* ------------------------------------------------------------ row writes */

static void record_undo(db_t *db, table_t *t, uint64_t rowid, int col, bool existed,
                        bool deleted) {
  undo_entry e;
  memset(&e, 0, sizeof(e));
  e.table_id = t->id;
  e.rowid = rowid;
  e.col = col;
  e.ctype = t->cols[col].type;
  e.existed = existed;
  e.deleted = deleted;
  if (e.ctype == CT_BIGINT) {
    e.old_int = existed ? t->ints[col][rowid] : 0;
  } else if (e.ctype == CT_VARCHAR) {
    e.old_str = existed ? t->strs[col][rowid] : NULL;
    e.old_strlen = existed ? t->strlens[col][rowid] : 0;
  } else if (e.ctype == CT_VECTOR) {
    /* The slot of the payload has to be restored as well: the rollback puts
     * the row back to the vector it had before, and the log has to name that
     * vector again. */
    e.old_slot = existed ? t->vec_slot[rowid] : 0;
    if (existed && t->vecs[col][rowid]) {
      size_t bytes = (size_t)t->dim * sizeof(float);
      e.old_vec = (float *)arena_alloc(&db->arena, bytes);
      if (e.old_vec) memcpy(e.old_vec, t->vecs[col][rowid], bytes);
    }
  }
  undo_push(db, &e);
}

/* Inserts a row.  The caller has validated the values already. */
uint64_t store_insert_row(db_t *db, table_t *t, const cell_value *cells) {
  uint64_t rowid = t->next_rowid++;
  table_alloc_rows(t, rowid + 1);
  for (int c = 0; c < t->ncols; c++) {
    record_undo(db, t, rowid, c, false, false);
    if (t->cols[c].type == CT_BIGINT) {
      t->ints[c][rowid] = cells[c].ival;
    } else if (t->cols[c].type == CT_VARCHAR) {
      t->strs[c][rowid] = arena_strndup(&db->arena, cells[c].sval ? cells[c].sval : "", cells[c].slen);
      t->strlens[c][rowid] = cells[c].slen;
    } else if (t->cols[c].type == CT_VECTOR) {
      size_t bytes = (size_t)t->dim * sizeof(float);
      uint64_t slot = t->vec_next_slot++;
      t->vec_slot[rowid] = slot;
      if (t->vec_cached) {
        if (db->vec_cache_bytes + bytes > db->vec_cache_limit) {
          /* the memory budget is exhausted: this table stops caching its
           * vectors in memory and reads them from the payload pool */
          t->vec_cached = false;
        } else {
          float *copy = (float *)arena_alloc(&db->arena, bytes);
          if (copy && cells[c].vec) memcpy(copy, cells[c].vec, bytes);
          t->vecs[c][rowid] = copy;
          db->vec_cache_bytes += bytes;
          t->vec_bytes += bytes;
        }
      }
      /* the payload is written as soon as the row is applied; the slot stays
       * unreferenced until the transaction commits, so an uncommitted change
       * can never become visible */
      if (cells[c].vec) table_write_vector(t, slot, cells[c].vec);
    }
  }
  t->pk[rowid] = (uint64_t)cells[t->pk_idx >= 0 ? t->pk_idx : 0].ival;
  t->alive[rowid] = 1;
  t->nlive++;
  if (t->pk_idx >= 0) pkmap_put(t, t->pk[rowid], rowid);
  for (int c = 0; c < t->ncols; c++) {
    if (t->cols[c].type == CT_VARCHAR || t->cols[c].type == CT_BIGINT) eqidx_add(t, c, rowid);
  }
  if (ivf_is_built(t->ivf) && t->vec_idx >= 0) ivf_add(t, rowid);
  dirty_push(db, t->id, rowid, 0);
  return rowid;
}

/* Updates the columns selected by mask. */
bool store_update_row(db_t *db, table_t *t, uint64_t rowid, const cell_value *cells,
                      const bool *mask) {
  for (int c = 0; c < t->ncols; c++) {
    if (!mask[c]) continue;
    record_undo(db, t, rowid, c, true, false);
    /* The row leaves the list of its old value before the value changes:
     * a row that stays in the list of the old value would be found twice
     * and the chain of the list would be broken. */
    if (t->cols[c].type == CT_VARCHAR || t->cols[c].type == CT_BIGINT) {
      eqidx_detach(t, c, rowid);
    }
    if (t->cols[c].type == CT_BIGINT) {
      t->ints[c][rowid] = cells[c].ival;
    } else if (t->cols[c].type == CT_VARCHAR) {
      t->strs[c][rowid] = arena_strndup(&db->arena, cells[c].sval ? cells[c].sval : "", cells[c].slen);
      t->strlens[c][rowid] = cells[c].slen;
    } else if (t->cols[c].type == CT_VECTOR) {
      size_t bytes = (size_t)t->dim * sizeof(float);
      uint64_t slot = t->vec_next_slot++;
      t->vec_slot[rowid] = slot;
      if (t->vec_cached) {
        float *copy = (float *)arena_alloc(&db->arena, bytes);
        if (copy && cells[c].vec) memcpy(copy, cells[c].vec, bytes);
        t->vecs[c][rowid] = copy;
        db->vec_cache_bytes += bytes;
        t->vec_bytes += bytes;
      }
      if (cells[c].vec) table_write_vector(t, slot, cells[c].vec);
    }
  }
  for (int c = 0; c < t->ncols; c++) {
    if (!mask[c]) continue;
    if (t->cols[c].type == CT_VARCHAR || t->cols[c].type == CT_BIGINT) eqidx_add(t, c, rowid);
  }
  if (ivf_is_built(t->ivf) && t->vec_idx >= 0 && mask[t->vec_idx]) ivf_add(t, rowid);
  dirty_push(db, t->id, rowid, 1);
  return true;
}

bool store_delete_row(db_t *db, table_t *t, uint64_t rowid) {
  /* The row was removed by this statement: the rollback has to bring it
   * back.  The flag is set on the first column, the rollback restores the
   * row once. */
  for (int c = 0; c < t->ncols; c++) record_undo(db, t, rowid, c, true, c == 0);
  t->alive[rowid] = 0;
  t->nlive--;
  if (t->pk_idx >= 0) pkmap_del(t, t->pk[rowid]);
  dirty_push(db, t->id, rowid, 2);
  return true;
}

/* Creates a table.  The definition is written to the log when the current
 * transaction commits, so a rollback removes the table completely. */
table_t *store_create_table(db_t *db, const char *name, const column_t *cols, int ncols,
                             int pk_idx, int vec_idx) {
  if (db_find_table(db, name)) return NULL;
  uint64_t id = 1;
  for (int i = 0; i < db->ntables; i++) {
    if (db->tables[i]->id >= id) id = db->tables[i]->id + 1;
  }
  table_t *t = table_new(db, id, name, cols, ncols, pk_idx, vec_idx);
  if (!t) return NULL;
  if (table_open_vector_file(db, t) != 0) return NULL;
  if (db->ddl_n < 8) db->ddl_ids[db->ddl_n++] = id;
  return t;
}

/* Serialises a table definition into a create table frame. */
static bool log_create_table_frame(db_t *db, table_t *t) {
  outbuf b;
  ob_init(&b);
  put_u64(&b, db->txn_id);
  put_u64(&b, t->id);
  put_u16(&b, (uint16_t)strlen(t->name));
  ob_putn(&b, t->name, strlen(t->name));
  put_u16(&b, (uint16_t)t->ncols);
  for (int i = 0; i < t->ncols; i++) {
    put_u8(&b, (uint8_t)t->cols[i].type);
    put_u16(&b, (uint16_t)strlen(t->cols[i].name));
    ob_putn(&b, t->cols[i].name, strlen(t->cols[i].name));
    put_u32(&b, (uint32_t)(t->cols[i].type == CT_VECTOR ? t->cols[i].dim : t->cols[i].vlen));
    put_u8(&b, t->cols[i].pk ? 1 : 0);
  }
  bool ok = !b.oom && log_append(db, b.data ? b.data : "", b.len, LOG_CREATE_TABLE);
  ob_free(&b);
  return ok;
}

/* Removes a primary key from the hash map. */
void store_remove_pk(table_t *t, uint64_t pk) { pkmap_del(t, pk); }
void store_insert_pk(table_t *t, uint64_t pk, uint64_t rowid) { pkmap_put(t, pk, rowid); }

/* Looks up a row by primary key. */
bool store_lookup_pk(table_t *t, int64_t pk, uint64_t *rowid) {
  if (t->pk_idx < 0) return false;
  return pkmap_get(t, (uint64_t)pk, rowid);
}
