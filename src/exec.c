/* Statement execution: CREATE TABLE / INSERT / SELECT / UPDATE / DELETE /
 * CREATE VECTOR INDEX and the transaction control statements. */
#define _GNU_SOURCE
#include "mvdb.h"

#include <errno.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  db_t *db;
  arena *a;
  outbuf *body;
  int64_t affected;
  bool ann_mode;
  char *err_code;
  size_t err_code_sz;
  char *err_msg;
  size_t err_msg_sz;
  bool failed;
} exectx;

static bool fail(exectx *c, const char *code, const char *fmt, ...) {
  if (c->failed) return false;
  c->failed = true;
  snprintf(c->err_code, c->err_code_sz, "%s", code);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(c->err_msg, c->err_msg_sz, fmt, ap);
  va_end(ap);
  return false;
}

/* ------------------------------------------------------------- helpers */

static int find_column(table_t *t, const char *name) {
  for (int i = 0; i < t->ncols; i++) {
    if (strcasecmp(t->cols[i].name, name) == 0) return i;
  }
  return -1;
}

/* UTF-8 byte order is the same as unicode code point order. */
static int cmp_code_points(const char *a, uint32_t alen, const char *b, uint32_t blen) {
  uint32_t n = alen < blen ? alen : blen;
  if (n) {
    int r = memcmp(a, b, n);
    if (r != 0) return r < 0 ? -1 : 1;
  }
  if (alen == blen) return 0;
  return alen < blen ? -1 : 1;
}

static bool row_matches_except(table_t *t, uint64_t rowid, const cond *c, const cond *skip) {
  for (; c; c = c->next) {
    if (c == skip) continue;
    int col = c->col;
    if (col < 0) return false;
    if (t->cols[col].type == CT_BIGINT) {
      int64_t v = t->ints[col][rowid];
      switch (c->op) {
        case CMP_EQ: if (v != c->ival) return false; break;
        case CMP_LT: if (!(v < c->ival)) return false; break;
        case CMP_LE: if (!(v <= c->ival)) return false; break;
        case CMP_GT: if (!(v > c->ival)) return false; break;
        case CMP_GE: if (!(v >= c->ival)) return false; break;
      }
    } else if (t->cols[col].type == CT_VARCHAR) {
      if (!c->sval) return false;
      const char *s = t->strs[col][rowid];
      uint32_t n = t->strlens[col][rowid];
      int cmp = cmp_code_points(s ? s : "", n, c->sval, (uint32_t)c->slen);
      switch (c->op) {
        case CMP_EQ: if (cmp != 0) return false; break;
        case CMP_LT: if (!(cmp < 0)) return false; break;
        case CMP_LE: if (!(cmp <= 0)) return false; break;
        case CMP_GT: if (!(cmp > 0)) return false; break;
        case CMP_GE: if (!(cmp >= 0)) return false; break;
      }
    }
  }
  return true;
}

static bool row_matches(table_t *t, uint64_t rowid, const cond *c) {
  return row_matches_except(t, rowid, c, NULL);
}

/* ------------------------------------------------------------ top-k heap */

typedef struct {
  float key;      /* distance, negated for a descending order */
  uint64_t pk;
  uint64_t rowid;
} topk_ent;

static bool topk_worse(const topk_ent *a, const topk_ent *b) {
  if (a->key != b->key) return a->key > b->key;
  return a->pk > b->pk;
}

/* Keeps the K best entries; the root of the heap is the worst one. */
static void topk_push(topk_ent *heap, int *n, int cap, const topk_ent *e) {
  if (*n < cap) {
    int i = (*n)++;
    heap[i] = *e;
    while (i > 0) {
      int p = (i - 1) / 2;
      if (!topk_worse(&heap[i], &heap[p])) break;
      topk_ent t = heap[i];
      heap[i] = heap[p];
      heap[p] = t;
      i = p;
    }
    return;
  }
  if (!topk_worse(&heap[0], e)) return;
  heap[0] = *e;
  int i = 0;
  for (;;) {
    int l = 2 * i + 1, r = l + 1, big = i;
    if (l < cap && topk_worse(&heap[l], &heap[big])) big = l;
    if (r < cap && topk_worse(&heap[r], &heap[big])) big = r;
    if (big == i) break;
    topk_ent t = heap[i];
    heap[i] = heap[big];
    heap[big] = t;
    i = big;
  }
}

static int topk_cmp(const void *pa, const void *pb) {
  const topk_ent *a = (const topk_ent *)pa;
  const topk_ent *b = (const topk_ent *)pb;
  if (a->key != b->key) return a->key < b->key ? -1 : 1;
  if (a->pk != b->pk) return a->pk < b->pk ? -1 : 1;
  return 0;
}

/* --------------------------------------------------------------- output */

static void emit_slot(outbuf *body, table_t *t, stmt *st, int k, uint64_t rowid) {
  if (st->star) {
    if (t->cols[k].type == CT_BIGINT) ob_json_int(body, t->ints[k][rowid]);
    else if (t->cols[k].type == CT_VARCHAR) {
      const char *s = t->strs[k][rowid];
      ob_json_string(body, s ? s : "", t->strlens[k][rowid]);
    } else {
      ob_json_null(body);
    }
    return;
  }
  expr *e = &st->items[k];
  if (e->kind == EX_L2) {
    const float *v = t->vecs[e->col][rowid];
    ob_json_double(body, v ? vec_l2(v, e->vec, t->dim) : 0.0);
    return;
  }
  if (t->cols[e->col].type == CT_BIGINT) {
    ob_json_int(body, t->ints[e->col][rowid]);
  } else if (t->cols[e->col].type == CT_VARCHAR) {
    const char *s = t->strs[e->col][rowid];
    ob_json_string(body, s ? s : "", t->strlens[e->col][rowid]);
  } else {
    ob_json_null(body);
  }
}

/* ---------------------------------------------------------- transactions */

/* Undoes the changes recorded between two undo positions. */
static void undo_range(db_t *db, size_t from, size_t to) {
  for (size_t i = to; i > from; i--) {
    undo_entry *e = &db->undo[i - 1];
    table_t *t = NULL;
    for (int k = 0; k < db->ntables; k++) {
      if (db->tables[k]->id == e->table_id) { t = db->tables[k]; break; }
    }
    if (!t || e->rowid >= t->nrows) continue;
    uint64_t rowid = e->rowid;
    if (e->ctype == CT_BIGINT) {
      t->ints[e->col][rowid] = e->old_int;
    } else if (e->ctype == CT_VARCHAR) {
      t->strs[e->col][rowid] = e->old_str;
      t->strlens[e->col][rowid] = e->old_strlen;
    } else if (e->ctype == CT_VECTOR) {
      t->vecs[e->col][rowid] = e->old_vec;
      t->vec_slot[rowid] = e->old_slot;
    }
    if (!e->existed) {
      if (t->alive[rowid]) {
        t->alive[rowid] = 0;
        t->nlive--;
        if (t->pk_idx >= 0) store_remove_pk(t, t->pk[rowid]);
      }
    }
  }
}

/* Drops the tables that the failed statement created. */
static void remove_ddl_tables(db_t *db, int from) {
  for (int i = db->ddl_n - 1; i >= from; i--) {
    for (int k = db->ntables - 1; k >= 0; k--) {
      if (db->tables[k]->id != db->ddl_ids[i]) continue;
      table_t *t = db->tables[k];
      if (t->vec_fd >= 0) close(t->vec_fd);
      free(t->hash_key);
      free(t->hash_val);
      for (int c = 0; c < t->ncols; c++) {
        if (t->cols[c].type == CT_BIGINT) free(t->ints[c]);
        else if (t->cols[c].type == CT_VARCHAR) { free(t->strs[c]); free(t->strlens[c]); }
        else if (t->cols[c].type == CT_VECTOR) free(t->vecs[c]);
      }
      free(t->ints);
      free(t->strs);
      free(t->strlens);
      free(t->vecs);
      free(t->pk);
      free(t->alive);
      for (int m = k; m + 1 < db->ntables; m++) db->tables[m] = db->tables[m + 1];
      db->ntables--;
      break;
    }
  }
  db->ddl_n = from;
}

/* ------------------------------------------------------------- statements */

static bool exec_create_table(exectx *c, stmt *st) {
  db_t *db = c->db;
  if (db_find_table(db, st->table)) {
    return fail(c, "duplicate_table", "table '%s' already exists", st->table);
  }
  if (st->ncols == 0) return fail(c, "unsupported_sql", "a table needs at least one column");
  if (st->pk_idx < 0) {
    return fail(c, "unsupported_sql", "a table needs a BIGINT primary key");
  }
  table_t *t = store_create_table(db, st->table, st->cols, st->ncols, st->pk_idx, st->vec_idx);
  if (!t) return fail(c, "io_error", "cannot create table '%s'", st->table);
  return true;
}

static bool exec_create_index(exectx *c, stmt *st) {
  db_t *db = c->db;
  table_t *t = db_find_table(db, st->table);
  if (!t) return fail(c, "no_such_table", "unknown table '%s'", st->table);
  int col = find_column(t, st->cols[0].name);
  if (col < 0) return fail(c, "no_such_column", "unknown column '%s'", st->cols[0].name);
  if (t->cols[col].type != CT_VECTOR) {
    return fail(c, "unsupported_sql", "'%s' is not a vector column", st->cols[0].name);
  }
  t->vec_idx = col;
  t->dim = t->cols[col].dim;
  if (!ivf_is_built(t->ivf)) {
    if (!ivf_build(db, t, col, 20261110u)) {
      return fail(c, "io_error", "cannot build the vector index");
    }
  }
  return true;
}

/* Converts a literal into a column value. */
static bool literal_to_cell(exectx *c, table_t *t, int col, const expr *e, cell_value *out) {
  memset(out, 0, sizeof(*out));
  switch (t->cols[col].type) {
    case CT_BIGINT:
      if (e->kind == EX_INT) {
        out->ival = e->ival;
      } else if (e->kind == EX_STR) {
        char *endp = NULL;
        errno = 0;
        long long v = strtoll(e->sval ? e->sval : "0", &endp, 10);
        if (errno != 0 || !endp || *endp != '\0') {
          return fail(c, "invalid_value", "cannot convert '%s' to BIGINT", e->sval ? e->sval : "");
        }
        out->ival = (int64_t)v;
      } else {
        return fail(c, "invalid_value", "value type does not match column '%s'", t->cols[col].name);
      }
      return true;
    case CT_VARCHAR:
      if (e->kind == EX_STR) {
        out->sval = e->sval ? e->sval : (char *)"";
        out->slen = (uint32_t)e->slen;
      } else if (e->kind == EX_INT) {
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)e->ival);
        out->sval = arena_strndup(c->a, tmp, (size_t)n);
        out->slen = (uint32_t)n;
      } else {
        return fail(c, "invalid_value", "value type does not match column '%s'", t->cols[col].name);
      }
      return true;
    case CT_VECTOR:
      if (e->kind != EX_VEC) {
        return fail(c, "invalid_value", "value type does not match column '%s'", t->cols[col].name);
      }
      if (e->invalid) return fail(c, "invalid_vector", "%s", e->invalid_msg);
      if (e->veclen != t->cols[col].dim) {
        return fail(c, "dimension_mismatch", "expected %d dimensions, got %d", t->cols[col].dim, e->veclen);
      }
      out->vec = e->vec;
      return true;
  }
  return fail(c, "unsupported_sql", "unsupported column type");
}

static bool exec_insert(exectx *c, stmt *st) {
  db_t *db = c->db;
  table_t *t = db_find_table(db, st->table);
  if (!t) return fail(c, "no_such_table", "unknown table '%s'", st->table);

  int map[16];
  int nmap = 0;
  if (st->nicols > 0) {
    for (int i = 0; i < st->nicols; i++) {
      int col = find_column(t, st->icolnames[i]);
      if (col < 0) return fail(c, "no_such_column", "unknown column '%s'", st->icolnames[i]);
      map[nmap++] = col;
    }
  } else {
    if (st->nvalcols > t->ncols) {
      return fail(c, "unsupported_sql", "too many values for table '%s'", st->table);
    }
    for (int i = 0; i < st->nvalcols; i++) map[nmap++] = i;
  }
  if (st->nvalcols != nmap) {
    return fail(c, "unsupported_sql", "value count does not match the column list");
  }

  /* Validate every row before touching the table. */
  cell_value *rows = (cell_value *)arena_calloc(c->a, (size_t)(st->nrows + 1) * nmap * sizeof(cell_value));
  if (!rows) return fail(c, "internal_error", "out of memory");
  for (int r = 0; r < st->nrows; r++) {
    for (int k = 0; k < nmap; k++) {
      if (!literal_to_cell(c, t, map[k], st->vals[r * st->nvalcols + k], &rows[r * nmap + k])) {
        return false;
      }
    }
  }
  if (t->pk_idx >= 0) {
    int pkpos = -1;
    for (int k = 0; k < nmap; k++) {
      if (map[k] == t->pk_idx) pkpos = k;
    }
    if (pkpos < 0) return fail(c, "unsupported_sql", "the primary key must be given");
    for (int r = 0; r < st->nrows; r++) {
      int64_t pk = rows[r * nmap + pkpos].ival;
      uint64_t found;
      if (store_lookup_pk(t, pk, &found)) {
        return fail(c, "duplicate_key", "duplicate primary key %lld", (long long)pk);
      }
      for (int p = 0; p < r; p++) {
        if (rows[p * nmap + pkpos].ival == pk) {
          return fail(c, "duplicate_key", "duplicate primary key %lld", (long long)pk);
        }
      }
    }
  }

  for (int r = 0; r < st->nrows; r++) {
    cell_value full[16];
    memset(full, 0, sizeof(full));
    for (int k = 0; k < nmap; k++) full[map[k]] = rows[r * nmap + k];
    store_insert_row(db, t, full);
  }
  c->affected = st->nrows;
  return true;
}

static bool exec_update(exectx *c, stmt *st) {
  db_t *db = c->db;
  table_t *t = db_find_table(db, st->table);
  if (!t) return fail(c, "no_such_table", "unknown table '%s'", st->table);
  int setcol[16];
  bool mask[16];
  memset(mask, 0, sizeof(mask));
  for (int i = 0; i < st->nset; i++) {
    int col = find_column(t, st->set_colnames[i]);
    if (col < 0) return fail(c, "no_such_column", "unknown column '%s'", st->set_colnames[i]);
    if (col == t->pk_idx) {
      /* updating the primary key is not part of the required semantics and
       * has no defined behaviour, so it is refused instead of silently
       * doing something else */
      return fail(c, "unsupported_sql", "updating the primary key is not supported");
    }
    setcol[i] = col;
    mask[col] = true;
  }
  for (cond *q = st->where; q; q = q->next) {
    int col = find_column(t, q->colname);
    if (col < 0) return fail(c, "no_such_column", "unknown column '%s'", q->colname);
    q->col = col;
  }
  uint64_t *rows = (uint64_t *)arena_alloc(c->a, (size_t)(t->nrows + 1) * sizeof(uint64_t));
  if (!rows) return fail(c, "internal_error", "out of memory");
  size_t nrows = 0;
  for (uint64_t r = 0; r < t->nrows; r++) {
    if (!t->alive[r]) continue;
    if (!row_matches(t, r, st->where)) continue;
    rows[nrows++] = r;
  }
  size_t nval = (size_t)(nrows + 1) * (size_t)(st->nset > 0 ? st->nset : 1);
  cell_value *vals = (cell_value *)arena_calloc(c->a, nval * sizeof(cell_value));
  if (!vals) return fail(c, "internal_error", "out of memory");
  for (size_t i = 0; i < nrows; i++) {
    for (int k = 0; k < st->nset; k++) {
      if (!literal_to_cell(c, t, setcol[k], st->set_vals[k], &vals[i * st->nset + k])) return false;
    }
  }
  for (size_t i = 0; i < nrows; i++) {
    cell_value full[16];
    bool fmask[16];
    memset(full, 0, sizeof(full));
    memset(fmask, 0, sizeof(fmask));
    for (int k = 0; k < st->nset; k++) {
      full[setcol[k]] = vals[i * st->nset + k];
      fmask[setcol[k]] = true;
    }
    store_update_row(db, t, rows[i], full, fmask);
  }
  c->affected = (int64_t)nrows;
  return true;
}

static bool exec_delete(exectx *c, stmt *st) {
  db_t *db = c->db;
  table_t *t = db_find_table(db, st->table);
  if (!t) return fail(c, "no_such_table", "unknown table '%s'", st->table);
  for (cond *q = st->where; q; q = q->next) {
    int col = find_column(t, q->colname);
    if (col < 0) return fail(c, "no_such_column", "unknown column '%s'", q->colname);
    q->col = col;
  }
  uint64_t *rows = (uint64_t *)arena_alloc(c->a, (size_t)(t->nrows + 1) * sizeof(uint64_t));
  if (!rows) return fail(c, "internal_error", "out of memory");
  size_t nrows = 0;
  for (uint64_t r = 0; r < t->nrows; r++) {
    if (!t->alive[r]) continue;
    if (!row_matches(t, r, st->where)) continue;
    rows[nrows++] = r;
  }
  for (size_t i = 0; i < nrows; i++) store_delete_row(db, t, rows[i]);
  c->affected = (int64_t)nrows;
  return true;
}

/* --------------------------------------------------------------- select */

typedef struct {
  table_t *t;
  int col;
  bool desc;
} sortctx;

static int sort_rows_cmp(const void *pa, const void *pb, void *arg) {
  const uint64_t *ra = (const uint64_t *)pa;
  const uint64_t *rb = (const uint64_t *)pb;
  const sortctx *sc = (const sortctx *)arg;
  table_t *t = sc->t;
  int col = sc->col;
  int cmp;
  if (t->cols[col].type == CT_BIGINT) {
    int64_t a = t->ints[col][*ra], b = t->ints[col][*rb];
    cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
  } else {
    cmp = cmp_code_points(t->strs[col][*ra] ? t->strs[col][*ra] : "", t->strlens[col][*ra],
                          t->strs[col][*rb] ? t->strs[col][*rb] : "", t->strlens[col][*rb]);
  }
  if (cmp == 0) cmp = (t->pk[*ra] < t->pk[*rb]) ? -1 : (t->pk[*ra] > t->pk[*rb]) ? 1 : 0;
  return sc->desc ? -cmp : cmp;
}

static bool exec_select(exectx *c, stmt *st) {
  struct timespec q0, q1, q2, q3;
  db_t *db = c->db;
  table_t *t = db_find_table(db, st->table);
  if (!t) return fail(c, "no_such_table", "unknown table '%s'", st->table);

  for (int i = 0; i < st->nitems; i++) {
    expr *e = &st->items[i];
    if (e->kind == EX_COL) {
      int col = find_column(t, e->colname);
      if (col < 0) return fail(c, "no_such_column", "unknown column '%s'", e->colname);
      e->col = col;
    } else if (e->kind == EX_L2) {
      int col = find_column(t, e->colname);
      if (col < 0) return fail(c, "no_such_column", "unknown column '%s'", e->colname);
      if (t->cols[col].type != CT_VECTOR) {
        return fail(c, "unsupported_sql", "'%s' is not a vector column", e->colname);
      }
      e->col = col;
      if (e->invalid) return fail(c, "invalid_vector", "%s", e->invalid_msg);
      if (e->veclen != t->cols[col].dim) {
        return fail(c, "dimension_mismatch", "expected %d dimensions, got %d", t->cols[col].dim, e->veclen);
      }
    }
  }
  for (cond *q = st->where; q; q = q->next) {
    int col = find_column(t, q->colname);
    if (col < 0) return fail(c, "no_such_column", "unknown column '%s'", q->colname);
    q->col = col;
  }
  if (st->order_kind == 1) {
    int col = find_column(t, st->order_colname);
    if (col < 0) return fail(c, "no_such_column", "unknown column '%s'", st->order_colname);
    st->order_col = col;
  }

  const char *names[32];
  int nout = 0;
  if (st->star) {
    for (int i = 0; i < t->ncols && nout < 32; i++) names[nout++] = t->cols[i].name;
  } else {
    for (int i = 0; i < st->nout && nout < 32; i++) {
      expr *e = &st->items[i];
      if (e->has_alias) names[nout++] = e->alias;
      else if (e->kind == EX_L2) names[nout++] = "l2_distance";
      else names[nout++] = t->cols[e->col].name;
    }
  }

  /* Phase 1: the candidate rows.
   * Without a filter every live row is a candidate.  With an equality
   * filter the posting index of that column provides the candidates
   * directly, which keeps a filtered TopK independent of the table size. */
  clock_gettime(CLOCK_MONOTONIC, &q0);
  uint64_t *cand = NULL;
  size_t ncand = 0;
  bool use_all = false;
  bool verify = false;
  const cond *eq = NULL;
  if (!st->where) {
    use_all = true;
  } else {
    eq = NULL;
    for (const cond *q = st->where; q; q = q->next) {
      if (q->op != CMP_EQ) continue;
      coltype_t ct = t->cols[q->col].type;
      if (ct == CT_VARCHAR || ct == CT_BIGINT) { eq = q; break; }
    }
    if ((size_t)t->nrows + 1 > db->scratch_rows_cap) {
      uint64_t *p = (uint64_t *)realloc(db->scratch_rows,
                                        ((size_t)t->nrows + 1) * sizeof(uint64_t));
      if (!p) return fail(c, "internal_error", "out of memory");
      db->scratch_rows = p;
      db->scratch_rows_cap = (size_t)t->nrows + 1;
    }
    cand = db->scratch_rows;
    if (eq) {
      char keybuf[32];
      const char *key;
      size_t klen;
      if (t->cols[eq->col].type == CT_BIGINT) {
        int n = snprintf(keybuf, sizeof(keybuf), "%lld", (long long)eq->ival);
        key = keybuf;
        klen = (size_t)n;
      } else {
        key = eq->sval ? eq->sval : "";
        klen = (size_t)eq->slen;
      }
      eqidx_build(t, eq->col);
      uint64_t head = eqidx_head(t, eq->col, key, klen);
      if (getenv("MVDB_DEBUG")) fprintf(stderr, "[mvdb] eq index col=%d key=%s head=%llu\n", eq->col, key, (unsigned long long)head);
      for (uint64_t r = head; r != UINT64_MAX; r = eqidx_next(t, eq->col, r)) cand[ncand++] = r;
      verify = true;
    } else {
      for (uint64_t r = 0; r < t->nrows; r++) {
        if (!t->alive[r]) continue;
        if (!row_matches(t, r, st->where)) continue;
        cand[ncand++] = r;
      }
    }
    if (verify) {
      /* the equality conjunct is already satisfied by the posting list */
      size_t k = 0;
      for (size_t i = 0; i < ncand; i++) {
        uint64_t r = cand[i];
        if (!t->alive[r]) continue;
        if (!row_matches_except(t, r, st->where, eq)) continue;
        cand[k++] = r;
      }
      ncand = k;
    }
  }

  outbuf *body = c->body;
  ob_puts(body, "\"columns\":[");
  for (int i = 0; i < nout; i++) {
    if (i) ob_putc(body, ',');
    ob_json_string(body, names[i], strlen(names[i]));
  }
  ob_puts(body, "],\"rows\":[");

  int64_t limit = st->has_limit ? st->limit : -1;
  bool first = true;

  if (st->topk_item >= 0) {
    expr *de = &st->items[st->topk_item];
    int vcol = de->col;
    int dim = t->dim;
    /* In the approximate mode an unfiltered TopK is answered from the index:
     * only the rows of the probed clusters are scanned. */
    int nprobe = ivf_is_built(t->ivf) ? ivf_nprobe(t->ivf) : 0;
    /* When the calibration had to probe almost every cluster the index does
     * not pay off and the exact scan is used instead. */
    bool use_ivf = c->ann_mode && ivf_is_built(t->ivf) && !st->where && nprobe > 0 &&
                   nprobe * 10 < ivf_nlist(t->ivf) * 9;
    if (use_ivf) {
      size_t cap_rows = (size_t)t->nrows + 1;
      if (cap_rows > db->scratch_res_cap) {
        dres_t *p = (dres_t *)realloc(db->scratch_res, cap_rows * sizeof(dres_t));
        if (!p) return fail(c, "internal_error", "out of memory");
        db->scratch_res = p;
        db->scratch_res_cap = cap_rows;
      }
      dres_t *res = db->scratch_res;
      size_t nres = ivf_search(t->ivf, (const float *const *)t->vecs[vcol], t->alive, de->vec,
                               dim, nprobe, cap_rows, res);
      int cap = 4096;
      if (limit > 0 && limit < cap) cap = (int)limit;
      topk_ent *heap = (topk_ent *)arena_alloc(c->a, (size_t)cap * sizeof(topk_ent));
      if (!heap) return fail(c, "internal_error", "out of memory");
      int n = 0;
      bool desc = st->order_desc;
      for (size_t i = 0; i < nres; i++) {
        topk_ent e;
        e.key = desc ? -res[i].sq : res[i].sq;
        e.pk = t->pk[res[i].rowid];
        e.rowid = res[i].rowid;
        topk_push(heap, &n, cap, &e);
      }
      qsort(heap, (size_t)n, sizeof(topk_ent), topk_cmp);
      for (int i = 0; i < n; i++) {
        if (limit > 0 && i >= limit) break;
        if (!first) ob_putc(body, ',');
        first = false;
        ob_putc(body, '[');
        for (int k = 0; k < nout; k++) {
          if (k) ob_putc(body, ',');
          emit_slot(body, t, st, k, heap[i].rowid);
        }
        ob_putc(body, ']');
      }
      ob_puts(body, "]");
      return true;
    }
    size_t res_cap = use_all ? (size_t)t->nrows : ncand;
    if (res_cap + 1 > db->scratch_res_cap) {
      dres_t *p = (dres_t *)realloc(db->scratch_res, (res_cap + 1) * sizeof(dres_t));
      if (!p) return fail(c, "internal_error", "out of memory");
      db->scratch_res = p;
      db->scratch_res_cap = res_cap + 1;
    }
    dres_t *res = db->scratch_res;
    clock_gettime(CLOCK_MONOTONIC, &q1);
    size_t nres = vec_scan_sq((const float *const *)t->vecs[vcol], t->alive, use_all ? NULL : cand,
                              use_all ? (size_t)t->nrows : ncand, de->vec, dim, res);
    clock_gettime(CLOCK_MONOTONIC, &q1);
    if (getenv("MVDB_DEBUG"))
      fprintf(stderr, "[mvdb] topk: use_all=%d ncand=%zu nres=%zu nrows=%llu\n", (int)use_all, ncand,
              nres, (unsigned long long)t->nrows);
    int cap = 4096;
    if (limit > 0 && limit < cap) cap = (int)limit;
    topk_ent *heap = (topk_ent *)arena_alloc(c->a, (size_t)cap * sizeof(topk_ent));
    if (!heap) return fail(c, "internal_error", "out of memory");
    int n = 0;
    bool desc = st->order_desc;
    for (size_t i = 0; i < nres; i++) {
      topk_ent e;
      e.key = desc ? -res[i].sq : res[i].sq;
      e.pk = t->pk[res[i].rowid];
      e.rowid = res[i].rowid;
      topk_push(heap, &n, cap, &e);
    }
    qsort(heap, (size_t)n, sizeof(topk_ent), topk_cmp);
    clock_gettime(CLOCK_MONOTONIC, &q2);
    for (int i = 0; i < n; i++) {
      if (limit > 0 && i >= limit) break;
      if (!first) ob_putc(body, ',');
      first = false;
      ob_putc(body, '[');
      for (int k = 0; k < nout; k++) {
        if (k) ob_putc(body, ',');
        emit_slot(body, t, st, k, heap[i].rowid);
      }
      ob_putc(body, ']');
    }
  } else {
    uint64_t *rows = cand;
    size_t nrows = ncand;
    if (use_all) {
      if ((size_t)t->nrows + 1 > db->scratch_rows_cap) {
        uint64_t *p = (uint64_t *)realloc(db->scratch_rows,
                                          ((size_t)t->nrows + 1) * sizeof(uint64_t));
        if (!p) return fail(c, "internal_error", "out of memory");
        db->scratch_rows = p;
        db->scratch_rows_cap = (size_t)t->nrows + 1;
      }
      rows = db->scratch_rows;
      for (uint64_t r = 0; r < t->nrows; r++) {
        if (!t->alive[r]) continue;
        rows[nrows++] = r;
      }
    }
    if (st->order_kind == 1 && nrows > 1) {
      sortctx sc;
      sc.t = t;
      sc.col = st->order_col;
      sc.desc = st->order_desc;
      qsort_r(rows, nrows, sizeof(uint64_t), sort_rows_cmp, &sc);
    }
    for (size_t i = 0; i < nrows; i++) {
      if (limit > 0 && (int64_t)i >= limit) break;
      if (!first) ob_putc(body, ',');
      first = false;
      ob_putc(body, '[');
      for (int k = 0; k < nout; k++) {
        if (k) ob_putc(body, ',');
        emit_slot(body, t, st, k, rows[i]);
      }
      ob_putc(body, ']');
    }
  }
  ob_puts(body, "]");
  if (getenv("MVDB_DEBUG")) {
    clock_gettime(CLOCK_MONOTONIC, &q3);
    fprintf(stderr, "[mvdb] phases cand %.3f scan %.3f topk %.3f out %.3f ms\n",
            (q0.tv_sec - q3.tv_sec) * 1e3 + (q0.tv_nsec - q3.tv_nsec) / 1e6,
            (q1.tv_sec - q0.tv_sec) * 1e3 + (q1.tv_nsec - q0.tv_nsec) / 1e6,
            (q2.tv_sec - q1.tv_sec) * 1e3 + (q2.tv_nsec - q1.tv_nsec) / 1e6,
            (q3.tv_sec - q2.tv_sec) * 1e3 + (q3.tv_nsec - q2.tv_nsec) / 1e6);
  }
  return true;
}

/* ------------------------------------------------------------- dispatch */

bool exec_statement(db_t *db, arena *a, stmt *st, const char *search_mode, outbuf *body,
                    char *err_code, size_t err_code_sz, char *err_msg, size_t err_msg_sz) {
  exectx c;
  memset(&c, 0, sizeof(c));
  c.db = db;
  c.a = a;
  c.body = body;
  c.err_code = err_code;
  c.err_code_sz = err_code_sz;
  c.err_msg = err_msg;
  c.err_msg_sz = err_msg_sz;
  c.affected = 0;
  c.ann_mode = search_mode && strcasecmp(search_mode, "ann") == 0;

  /* transaction control is handled outside the implicit transaction logic */
  if (st->type == ST_BEGIN) {
    if (db->txn_active) {
      snprintf(err_code, err_code_sz, "unsupported_sql");
      snprintf(err_msg, err_msg_sz, "nested transactions are not supported");
      return false;
    }
    db->txn_id = db->next_txn++;
    db->txn_active = true;
    return true;
  }
  if (st->type == ST_COMMIT) {
    if (!db->txn_active) return true;
    if (!txn_commit(db)) {
      snprintf(err_code, err_code_sz, "io_error");
      snprintf(err_msg, err_msg_sz, "cannot persist the transaction");
      return false;
    }
    return true;
  }
  if (st->type == ST_ROLLBACK) {
    if (!db->txn_active) return true;
    txn_rollback(db);
    return true;
  }

  /* A read only statement does not open a transaction: it must not force a
   * log flush, which would cost a disk sync per query. */
  if (st->type == ST_SELECT) {
    c.affected = -1;
    bool ok = exec_select(&c, st);
    if (!ok) return false;
    return true;
  }

  bool implicit = !db->txn_active;
  if (implicit) {
    db->txn_id = db->next_txn++;
    db->txn_active = true;
  }
  size_t undo_mark = db->undo_n;
  size_t dirty_mark = db->dirty_n;
  int ddl_mark = db->ddl_n;

  bool ok = true;
  switch (st->type) {
    case ST_CREATE_TABLE: ok = exec_create_table(&c, st); break;
    case ST_CREATE_INDEX: ok = exec_create_index(&c, st); break;
    case ST_INSERT: ok = exec_insert(&c, st); break;
    case ST_UPDATE: ok = exec_update(&c, st); break;
    case ST_DELETE: ok = exec_delete(&c, st); break;
    default: ok = fail(&c, "unsupported_sql", "unsupported statement"); break;
  }

  if (!ok) {
    undo_range(db, undo_mark, db->undo_n);
    db->undo_n = undo_mark;
    db->dirty_n = dirty_mark;
    remove_ddl_tables(db, ddl_mark);
    if (implicit) db->txn_active = false;
    return false;
  }
  if (implicit) {
    if (!txn_commit(db)) {
      snprintf(err_code, err_code_sz, "io_error");
      snprintf(err_msg, err_msg_sz, "cannot persist the transaction");
      return false;
    }
  }
  if (st->type == ST_INSERT || st->type == ST_UPDATE || st->type == ST_DELETE) {
    ob_printf(body, "\"affected_rows\":%lld", (long long)c.affected);
  }
  return true;
}
