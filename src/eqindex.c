/* Equality posting index.
 *
 * A filtered TopK must not scan the whole table: the filter is an equality on
 * an ordinary column, so the rows that can match are looked up directly.
 * The index maps a value to the list of row ids that carried it.  It is built
 * on the first filtered query and kept up to date by the write path.
 *
 * Entries are never removed: a stale row id is filtered out by re-checking
 * the predicate, which keeps the maintenance free.
 */
#include "mvdb.h"

static uint64_t hash_bytes(const char *p, size_t n) {
  uint64_t h = 1469598103934665603ULL; /* FNV-1a */
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static void eqidx_rehash(eqidx_t *ix) {
  uint32_t old_mask = ix->mask;
  char **old_keys = ix->keys;
  uint32_t *old_lens = ix->keylens;
  uint64_t *old_heads = ix->heads;
  uint32_t ncap = (old_mask + 1) * 2;
  ix->mask = ncap - 1;
  ix->keys = (char **)calloc(ncap, sizeof(char *));
  ix->keylens = (uint32_t *)calloc(ncap, sizeof(uint32_t));
  ix->heads = (uint64_t *)calloc(ncap, sizeof(uint64_t));
  ix->used = 0;
  for (uint32_t i = 0; i <= old_mask; i++) {
    if (!old_keys[i]) continue;
    uint32_t j = (uint32_t)(hash_bytes(old_keys[i], old_lens[i]) & ix->mask);
    while (ix->keys[j]) j = (j + 1) & ix->mask;
    ix->keys[j] = old_keys[i];
    ix->keylens[j] = old_lens[i];
    ix->heads[j] = old_heads[i];
    ix->used++;
  }
  free(old_keys);
  free(old_lens);
  free(old_heads);
}

/* Finds or creates the bucket of a value. */
static uint32_t eqidx_slot(eqidx_t *ix, const char *value, size_t len, bool create) {
  if (!ix->keys) {
    ix->mask = 1023;
    ix->keys = (char **)calloc(ix->mask + 1, sizeof(char *));
    ix->keylens = (uint32_t *)calloc(ix->mask + 1, sizeof(uint32_t));
    ix->heads = (uint64_t *)calloc(ix->mask + 1, sizeof(uint64_t));
    ix->used = 0;
  }
  uint32_t i = (uint32_t)(hash_bytes(value, len) & ix->mask);
  for (;;) {
    if (!ix->keys[i]) {
      if (!create) return ix->mask;   /* not found */
      if ((ix->used + 1) * 10 > (ix->mask + 1) * 7) {
        eqidx_rehash(ix);
        i = (uint32_t)(hash_bytes(value, len) & ix->mask);
        continue;
      }
      ix->keys[i] = (char *)malloc(len + 1);
      if (!ix->keys[i]) return ix->mask;
      memcpy(ix->keys[i], value, len);
      ix->keys[i][len] = '\0';
      ix->keylens[i] = (uint32_t)len;
      ix->heads[i] = UINT64_MAX;
      ix->used++;
      return i;
    }
    if (ix->keylens[i] == len && memcmp(ix->keys[i], value, len) == 0) return i;
    i = (i + 1) & ix->mask;
  }
}

/* Appends a row id to the list of a value. */
static void eqidx_append(eqidx_t *ix, uint32_t slot, uint64_t rowid) {
  if (rowid >= ix->nexts_cap) {
    uint64_t cap = ix->nexts_cap ? ix->nexts_cap : 64;
    while (cap <= rowid) cap *= 2;
    ix->nexts = (uint64_t *)realloc(ix->nexts, (size_t)cap * sizeof(uint64_t));
    if (!ix->nexts) { ix->nexts_cap = 0; return; }
    for (uint64_t i = ix->nexts_cap; i < cap; i++) ix->nexts[i] = UINT64_MAX;
    ix->nexts_cap = cap;
  }
  ix->nexts[rowid] = ix->heads[slot];
  ix->heads[slot] = rowid;
}

/* The key of a row in a column: the raw bytes for a string column, the
 * decimal representation for an integer column. */
static const char *row_key(table_t *t, int col, uint64_t rowid, char *buf, size_t bufsz,
                           size_t *outlen) {
  if (t->cols[col].type == CT_BIGINT) {
    int n = snprintf(buf, bufsz, "%lld", (long long)t->ints[col][rowid]);
    *outlen = (size_t)n;
    return buf;
  }
  const char *s = t->strs[col][rowid];
  *outlen = t->strlens[col][rowid];
  return s ? s : "";
}

void eqidx_build(table_t *t, int col) {
  if (col < 0 || col >= t->ncols) return;
  if (!t->eqidx) return;
  eqidx_t *ix = &t->eqidx[col];
  if (ix->built) return;
  if (ix->keys) {
    for (uint32_t i = 0; i <= ix->mask; i++) free(ix->keys[i]);
    free(ix->keys);
    free(ix->keylens);
    free(ix->heads);
    ix->keys = NULL;
    ix->keylens = NULL;
    ix->heads = NULL;
    ix->used = 0;
    ix->mask = 0;
  }
  char buf[32];
  for (uint64_t r = 0; r < t->nrows; r++) {
    if (!t->alive[r]) continue;
    size_t len = 0;
    const char *key = row_key(t, col, r, buf, sizeof(buf), &len);
    uint32_t slot = eqidx_slot(ix, key, len, true);
    if (slot == ix->mask && !ix->keys[slot]) continue;
    eqidx_append(ix, slot, r);
  }
  ix->built = true;
}

void eqidx_add(table_t *t, int col, uint64_t rowid) {
  if (col < 0 || col >= t->ncols || !t->eqidx) return;
  eqidx_t *ix = &t->eqidx[col];
  if (!ix->built) return;
  char buf[32];
  size_t len = 0;
  const char *key = row_key(t, col, rowid, buf, sizeof(buf), &len);
  uint32_t slot = eqidx_slot(ix, key, len, true);
  if (slot == ix->mask && !ix->keys[slot]) return;
  eqidx_append(ix, slot, rowid);
}

/* Returns the head of the candidate list for a value; the caller walks it
 * with eqidx_next().  No copy is made. */
uint64_t eqidx_head(table_t *t, int col, const char *value, size_t len) {
  if (col < 0 || col >= t->ncols || !t->eqidx) return UINT64_MAX;
  eqidx_t *ix = &t->eqidx[col];
  if (!ix->built || !ix->keys) return UINT64_MAX;
  uint32_t slot = eqidx_slot(ix, value, len, false);
  if (!ix->keys[slot]) return UINT64_MAX;
  return ix->heads[slot];
}

uint64_t eqidx_next(table_t *t, int col, uint64_t rowid) {
  if (col < 0 || col >= t->ncols || !t->eqidx) return UINT64_MAX;
  eqidx_t *ix = &t->eqidx[col];
  if (rowid >= ix->nexts_cap) return UINT64_MAX;
  return ix->nexts[rowid];
}

void eqidx_free(table_t *t) {
  if (!t->eqidx) return;
  for (int c = 0; c < t->ncols; c++) {
    eqidx_t *ix = &t->eqidx[c];
    if (ix->keys) {
      for (uint32_t i = 0; i <= ix->mask; i++) free(ix->keys[i]);
      free(ix->keys);
      free(ix->keylens);
      free(ix->heads);
    }
    free(ix->nexts);
  }
  free(t->eqidx);
  t->eqidx = NULL;
}
