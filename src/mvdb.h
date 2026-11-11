/* MiniVectorDB - shared declarations.
 *
 * Single process, single client, at most one explicit transaction.
 * Everything the process writes lives under the data directory (/data in the
 * evaluation container).  No third party dependency is used: the JSON codec,
 * the SQL parser, the storage engine and the vector index are all local.
 */
#ifndef MVDB_H
#define MVDB_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ arena */

/* Request scoped bump allocator.  All parser/executor temporaries live in
 * one arena that is reset after every request, which removes the need for
 * per-object free() and makes leaks structurally impossible. */
typedef struct arena_chunk arena_chunk;
typedef struct {
  arena_chunk *head;
  size_t total;
} arena;

void arena_init(arena *a);
void *arena_alloc(arena *a, size_t n);
void *arena_calloc(arena *a, size_t n);
char *arena_strndup(arena *a, const char *s, size_t n);
char *arena_printf(arena *a, const char *fmt, ...);
void arena_reset(arena *a);

/* ------------------------------------------------------------------- json */

/* Minimal JSON reader/writer.  Only what the protocol needs: objects,
 * strings (with \u escapes), numbers (int64 exact), booleans, null. */

typedef enum { JSON_NULL, JSON_BOOL, JSON_INT, JSON_DOUBLE, JSON_STRING, JSON_ARRAY, JSON_OBJECT } json_type;

typedef struct json_value json_value;
struct json_value {
  json_type type;
  bool bval;
  int64_t ival;
  double dval;
  char *sval;     /* JSON_STRING: NUL terminated, UTF-8 */
  size_t slen;
  /* object / array members */
  char **keys;
  json_value **vals;
  size_t count;
};

/* Parses one JSON document.  Returns NULL on error. */
json_value *json_parse(arena *a, const char *text, size_t len);
const json_value *json_get(const json_value *obj, const char *key);

/* Growable output buffer used to build one response line. */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
  bool oom;
} outbuf;

void ob_init(outbuf *b);
void ob_free(outbuf *b);
void ob_putc(outbuf *b, char c);
void ob_puts(outbuf *b, const char *s);
void ob_putn(outbuf *b, const char *s, size_t n);
void ob_printf(outbuf *b, const char *fmt, ...);
void ob_json_string(outbuf *b, const char *s, size_t n);
void ob_json_int(outbuf *b, int64_t v);
void ob_json_double(outbuf *b, double v);
void ob_json_null(outbuf *b);

/* ------------------------------------------------------------------- sql */

typedef enum { CT_BIGINT = 1, CT_VARCHAR = 2, CT_VECTOR = 3 } coltype_t;

typedef struct {
  char name[64];
  coltype_t type;
  int vlen;    /* VARCHAR: maximum length in unicode code points */
  int dim;     /* VECTOR: dimension */
  bool pk;
} column_t;

typedef enum { EX_COL = 1, EX_L2, EX_INT, EX_STR, EX_VEC } expr_kind;

typedef struct expr {
  expr_kind kind;
  int col;            /* EX_COL: column index */
  char colname[64];   /* column name before semantic analysis */
  float *vec;         /* EX_L2 query vector / EX_VEC literal value */
  int veclen;
  int64_t ival;       /* EX_INT */
  char *sval;         /* EX_STR */
  int slen;
  char alias[64];     /* explicit AS alias */
  bool has_alias;
  bool invalid;       /* the literal itself is illegal (NaN/Inf/overflow) */
  char invalid_msg[96];
} expr;

typedef enum { CMP_EQ = 1, CMP_LT, CMP_LE, CMP_GT, CMP_GE } cmpop_t;

typedef struct cond {
  int col;
  char colname[64];   /* column name before semantic analysis */
  cmpop_t op;
  int64_t ival;
  double fval;
  bool is_float;
  char *sval;         /* owned by the request arena */
  int slen;
  struct cond *next;  /* AND chain */
} cond;

typedef enum {
  ST_CREATE_TABLE = 1, ST_INSERT, ST_SELECT, ST_UPDATE, ST_DELETE,
  ST_CREATE_INDEX, ST_BEGIN, ST_COMMIT, ST_ROLLBACK
} stmt_type;

typedef struct {
  stmt_type type;

  /* CREATE TABLE */
  char table[64];
  column_t cols[16];
  int ncols;
  int pk_idx;
  int vec_idx;

  /* INSERT / UPDATE / SELECT table */
  int tab;
  /* INSERT: explicit column list, -1 entries mean "positional" */
  int *icols;
  char **icolnames;
  int nicols;
  /* values: flat array of nrows * ncols_per_row expressions */
  expr **vals;
  int nrows;
  int nvalcols;

  /* SELECT */
  expr *items;        /* nitems entries, the first nout are projected */
  int nitems;
  int nout;
  bool star;
  cond *where;
  int order_kind;     /* 0 none, 1 by table column, 2 by distance expression */
  int order_col;      /* order_kind == 1 */
  char order_colname[64];
  int order_item;     /* order_kind == 2: index into items */
  bool order_desc;
  bool has_limit;
  int64_t limit;
  int topk_item;      /* item index holding the ordered distance, -1 when none */

  /* UPDATE */
  int *set_cols;
  char **set_colnames;
  expr **set_vals;
  int nset;

  /* CREATE VECTOR INDEX */
  char index_name[64];
} stmt;

stmt *sql_parse(arena *a, const char *text, size_t len, char **err);

/* ------------------------------------------------------------------ store */

/* The IVF-Flat index of a table (opaque, implemented in ivf.c). */
typedef struct ivf ivf_t;

/* Equality posting index for one ordinary column: maps a value to the list
 * of row ids that carried it.  It is built on demand and maintained on every
 * write, so a filtered TopK only has to look at the matching rows. */
typedef struct {
  bool built;
  uint32_t mask;
  uint32_t used;
  char **keys;
  uint32_t *keylens;
  uint64_t *heads;    /* the first row id of each list */
  uint64_t *nexts;    /* the next row id, indexed by row id */
  uint64_t nexts_cap;
} eqidx_t;

typedef struct {
  uint64_t id;
  char name[64];
  int ncols;
  column_t cols[16];
  int pk_idx;     /* -1 when the table has no primary key */
  int vec_idx;    /* -1 when the table has no vector column */
  int dim;

  uint64_t next_rowid;

  /* Column oriented in-memory store.  Row i is a physical rowid. */
  uint64_t nrows;      /* allocated slots */
  uint64_t nlive;
  uint64_t *pk;        /* [nrows] primary key value */
  uint8_t *alive;      /* [nrows] */
  int64_t **ints;      /* [ncols][nrows] for CT_BIGINT */
  char ***strs;        /* [ncols][nrows] for CT_VARCHAR */
  uint32_t **strlens;
  float ***vecs;       /* [ncols][nrows] for CT_VECTOR (dim floats each) */

  /* equality posting indexes, one per ordinary column */
  eqidx_t *eqidx;

  /* the approximate vector index, created by CREATE VECTOR INDEX */
  ivf_t *ivf;

  /* primary key hash map: pk -> rowid */
  uint64_t *hash_key;
  uint64_t *hash_val;
  uint32_t hash_mask;
  uint32_t hash_used;

  /* Vector payload pool.  Every row version gets its own slot so that a
   * vector is written exactly once and the payload stays append only: a
   * transaction that never commits can therefore never change a value. */
  int vec_fd;
  char vec_path[256];
  bool vec_cached;      /* the vectors of this table are held in memory */
  uint64_t vec_bytes;   /* the memory used by the cached vectors */
  /* a small read cache over the payload pool, used when the vectors are not
   * cached in memory (a table that grew beyond the memory budget) */
  unsigned char *pool_buf;
  size_t pool_buf_cap;
  uint64_t pool_buf_slot;   /* the first slot in the cache */
  size_t pool_buf_slots;
  uint64_t *vec_slot;    /* [nrows] pool slot of the current vector */
  uint64_t vec_next_slot;
  uint64_t vec_pool_rows;  /* slots physically present in the pool file */
} table_t;

/* One old column value, used to undo a change when a transaction is rolled
 * back or when a statement fails. */
typedef struct {
  uint64_t table_id;
  uint64_t rowid;
  int col;
  int ctype;
  bool existed;       /* the row did not exist before this change */
  int64_t old_int;
  char *old_str;
  uint32_t old_strlen;
  float *old_vec;     /* dim floats, private copy */
  uint64_t old_slot;  /* previous vector pool slot */
} undo_entry;

/* A row touched by the open transaction; serialised at commit time. */
typedef struct {
  uint64_t table_id;
  uint64_t rowid;
  uint8_t kind;   /* 0 insert, 1 update, 2 delete */
} dirty_row;

/* One scanned row with its squared distance. */
typedef struct {
  uint64_t rowid;
  float sq;
} dres_t;

typedef struct {
  arena arena;          /* long lived allocations (catalog, string values) */
  table_t *tables[32];
  int ntables;

  /* storage */
  char data_dir[512];
  int log_fd;
  char log_path[512];
  uint64_t log_size;
  uint64_t next_txn;

  /* transaction state */
  bool txn_active;
  bool in_statement;    /* an implicit transaction is running */
  uint64_t txn_id;

  undo_entry *undo;
  size_t undo_n, undo_cap;
  dirty_row *dirty;
  size_t dirty_n, dirty_cap;

  /* the memory budget for the cached vectors */
  uint64_t vec_cache_limit;
  uint64_t vec_cache_bytes;

  /* reusable scratch buffers for the query path; they are kept between
   * requests so that a query does not allocate (and fault in) a fresh
   * hundred kilobyte buffer every time. */
  uint64_t *scratch_rows;
  size_t scratch_rows_cap;
  dres_t *scratch_res;
  size_t scratch_res_cap;

  /* tables created by the open transaction (removed again on rollback) */
  uint64_t ddl_ids[8];
  int ddl_n;

  bool fatal;
} db_t;

int db_open(db_t *db, const char *data_dir);
void db_close(db_t *db);

/* A single column value prepared by the executor. */
typedef struct {
  int64_t ival;
  char *sval;
  uint32_t slen;
  float *vec;   /* dim floats for a vector column */
} cell_value;

table_t *db_find_table(db_t *db, const char *name);
bool table_pool_read(table_t *t, uint64_t slot, float *dst);
size_t table_scan_sq(table_t *t, int vcol, const uint8_t *alive, const uint64_t *rows,
                      size_t nrows, const float *q, int dim, dres_t *out);
bool table_row_vector(table_t *t, int vcol, uint64_t rowid, float *dst);
uint64_t store_insert_row(db_t *db, table_t *t, const cell_value *cells);
bool store_update_row(db_t *db, table_t *t, uint64_t rowid, const cell_value *cells,
                      const bool *mask);
bool store_delete_row(db_t *db, table_t *t, uint64_t rowid);
table_t *store_create_table(db_t *db, const char *name, const column_t *cols, int ncols,
                             int pk_idx, int vec_idx);
bool store_lookup_pk(table_t *t, int64_t pk, uint64_t *rowid);

/* equality posting index */
void eqidx_build(table_t *t, int col);
void eqidx_add(table_t *t, int col, uint64_t rowid);
uint64_t eqidx_head(table_t *t, int col, const char *value, size_t len);
uint64_t eqidx_next(table_t *t, int col, uint64_t rowid);
void eqidx_free(table_t *t);

/* IVF-Flat index */
bool ivf_build(db_t *db, table_t *t, int col, uint32_t seed);
bool ivf_load(db_t *db, table_t *t);
bool ivf_add(table_t *t, uint64_t rowid);
bool ivf_save_base(db_t *db, table_t *t, ivf_t *ix);
size_t ivf_search(ivf_t *ix, const float *const *vecs, const uint8_t *alive, const float *q,
                  int dim, int nprobe, size_t out_cap, dres_t *out);
bool ivf_is_built(ivf_t *ix);
double ivf_calibrate(ivf_t *ix, const float *const *vecs, const uint8_t *alive, uint64_t nrows,
                     int k, int samples, double target);
int ivf_nprobe(ivf_t *ix);
int ivf_nlist(ivf_t *ix);
bool ivf_set_nprobe(ivf_t *ix, int nprobe);
void ivf_free(ivf_t *ix);
void store_remove_pk(table_t *t, uint64_t pk);
bool txn_commit(db_t *db);
void txn_rollback(db_t *db);

/* --------------------------------------------------------------- execute */

typedef struct {
  outbuf *out;      /* response body (columns / rows / affected_rows) */
  arena *a;         /* request arena */
} exec_ctx;

/* Executes one statement.  Returns true when the statement succeeded.  On
 * failure err_code/err_msg describe the problem and nothing was applied. */
bool exec_statement(db_t *db, arena *a, stmt *st, const char *search_mode,
                    outbuf *body, char *err_code, size_t err_code_sz,
                    char *err_msg, size_t err_msg_sz);

/* --------------------------------------------------------------- vectors */

float vec_l2(const float *a, const float *b, int dim);
float vec_l2_sq(const float *a, const float *b, int dim);

/* Computes the squared distance of every candidate row.  rows == NULL scans
 * all the rows of the table; dead rows are skipped. */
size_t vec_scan_sq(const float *const *vecs, const uint8_t *alive, const uint64_t *rows,
                   size_t nrows, const float *q, int dim, dres_t *out);
void vec_l2_batch(const float *base, int dim, const float *q, int n, float *out);

/* Parses "[1,2,3]" into float32.  Rejects NaN/Inf/out-of-range values.
 * Returns false and sets *err on failure. */
bool vec_parse(arena *a, const char *text, size_t len, float **out, int *out_dim,
               char *err, size_t err_sz);

#endif /* MVDB_H */
