/* IVF-Flat vector index.
 *
 * The index partitions the vectors into nlist clusters and stores, for every
 * cluster, the row ids of its members.  A query computes the distance to the
 * centroids, takes the nprobe closest clusters and scans only their
 * members, so a query touches a fraction of the table.
 *
 * The index is a derived structure: the exact mode always scans the table
 * and the index can be rebuilt from the persisted rows.  It is persisted as a
 * base file (the centroids and the initial postings) plus an append only
 * increment log, so that a restart does not have to rebuild it.
 */
#include "mvdb.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IVF_MAGIC 0x4d564446u /* "MVDF" */
#define IVF_VERSION 1u

typedef struct {
  uint64_t *rows;
  size_t n;
  size_t cap;
} ivf_list_t;

struct ivf {
  bool built;
  int dim;
  int nlist;
  int nprobe;
  float *centroids;    /* nlist * dim */
  ivf_list_t *lists;   /* nlist posting lists */
  uint32_t *cluster_of; /* the cluster of every row id */
  uint64_t cluster_of_len;
  int log_fd;
  char base_path[256];
  char log_path[256];
  uint64_t rows_indexed;
};

/* Removes a row id from a posting list. */
static void list_remove(ivf_list_t *l, uint64_t rowid) {
  for (size_t i = 0; i < l->n; i++) {
    if (l->rows[i] == rowid) {
      memmove(&l->rows[i], &l->rows[i + 1], (l->n - i - 1) * sizeof(uint64_t));
      l->n--;
      return;
    }
  }
}

static void list_push(ivf_list_t *l, uint64_t rowid) {
  if (l->n == l->cap) {
    size_t cap = l->cap ? l->cap * 2 : 64;
    uint64_t *p = (uint64_t *)realloc(l->rows, cap * sizeof(uint64_t));
    if (!p) return;
    l->rows = p;
    l->cap = cap;
  }
  l->rows[l->n++] = rowid;
}

/* ------------------------------------------------------- k-means build */

/* Picks nlist initial centroids from the given vectors. */
static void ivf_init_centroids(ivf_t *ix, const float **vecs, const uint64_t *rows,
                                size_t n, uint32_t seed) {
  int nlist = ix->nlist;
  uint32_t s = seed ? seed : 1;
  for (int c = 0; c < nlist; c++) {
    /* a deterministic stride walk over the rows */
    s = s * 1664525u + 1013904223u;
    size_t pick = n ? (size_t)((s >> 8) % n) : 0;
    uint64_t r = rows[pick];
    const float *v = vecs[r];
    if (!v) continue;
    memcpy(ix->centroids + (size_t)c * ix->dim, v, (size_t)ix->dim * sizeof(float));
  }
}

/* Assigns every vector to its closest centroid and recomputes the centroids.
 * Returns the number of vectors that changed their cluster. */
static size_t ivf_iterate(ivf_t *ix, const float **vecs, const uint64_t *rows, size_t n,
                          uint32_t *cluster_of) {
  int nlist = ix->nlist;
  int dim = ix->dim;
  double *sum = (double *)calloc((size_t)nlist * dim, sizeof(double));
  uint64_t *count = (uint64_t *)calloc((size_t)nlist, sizeof(uint64_t));
  if (!sum || !count) { free(sum); free(count); return 0; }
  size_t changed = 0;
  for (size_t i = 0; i < n; i++) {
    uint64_t r = rows[i];
    const float *v = vecs[r];
    if (!v) continue;
    int best = 0;
    float bestd = vec_l2(v, ix->centroids, dim);
    for (int c = 1; c < nlist; c++) {
      float d = vec_l2(v, ix->centroids + (size_t)c * dim, dim);
      if (d < bestd) { bestd = d; best = c; }
    }
    if (cluster_of && cluster_of[r] != (uint32_t)best) changed++;
    if (cluster_of) cluster_of[r] = (uint32_t)best;
    double *acc = sum + (size_t)best * dim;
    for (int d = 0; d < dim; d++) acc[d] += v[d];
    count[best]++;
  }
  for (int c = 0; c < nlist; c++) {
    if (!count[c]) continue;
    float *cent = ix->centroids + (size_t)c * dim;
    for (int d = 0; d < dim; d++) cent[d] = (float)(sum[(size_t)c * dim + d] / (double)count[c]);
  }
  free(sum);
  free(count);
  return changed;
}

/* Builds the index for a table. */
bool ivf_build(db_t *db, table_t *t, int col, uint32_t seed) {
  if (col < 0 || col >= t->ncols || t->cols[col].type != CT_VECTOR) return false;
  int dim = t->cols[col].dim;
  if (dim <= 0) return false;

  /* the live rows */
  uint64_t *rows = (uint64_t *)malloc((size_t)(t->nrows + 1) * sizeof(uint64_t));
  if (!rows) return false;
  size_t n = 0;
  for (uint64_t r = 0; r < t->nrows; r++) {
    if (t->alive[r] && t->vecs[col][r]) rows[n++] = r;
  }
  const float **vecs = (const float **)t->vecs[col];
  if (!vecs) { free(rows); return false; }

  int nlist = (int)(sqrt((double)n) + 0.5);
  if (nlist < 1) nlist = 1;
  if (nlist > 4096) nlist = 4096;
  if (nlist > (int)n) nlist = (int)n;
  if (nlist < 1) { free(rows); return false; }

  ivf_t *ix = (ivf_t *)calloc(1, sizeof(struct ivf));
  if (!ix) { free(rows); return false; }
  ix->dim = dim;
  ix->nlist = nlist;
  ix->centroids = (float *)malloc((size_t)nlist * dim * sizeof(float));
  ix->lists = (ivf_list_t *)calloc((size_t)nlist, sizeof(ivf_list_t));
  if (!ix->centroids || !ix->lists) { free(rows); free(ix); return false; }

  uint32_t *cluster_of = (uint32_t *)calloc((size_t)t->nrows, sizeof(uint32_t));
  if (!cluster_of) { free(rows); free(ix); return false; }
  for (uint32_t i = 0; i < t->nrows; i++) cluster_of[i] = UINT32_MAX;

  ivf_init_centroids(ix, vecs, rows, n, seed);
  for (int it = 0; it < 6; it++) {
    size_t changed = ivf_iterate(ix, vecs, rows, n, cluster_of);
    if (changed == 0 && it > 0) break;
  }
  ix->cluster_of = (uint32_t *)malloc((size_t)(t->nrows + 1) * sizeof(uint32_t));
  ix->cluster_of_len = (uint64_t)t->nrows + 1;
  if (ix->cluster_of) {
    for (uint64_t i = 0; i < ix->cluster_of_len; i++) ix->cluster_of[i] = UINT32_MAX;
  }
  for (size_t i = 0; i < n; i++) {
    uint64_t r = rows[i];
    if (cluster_of[r] != UINT32_MAX) {
      list_push(&ix->lists[cluster_of[r]], r);
      if (ix->cluster_of) ix->cluster_of[r] = cluster_of[r];
    }
  }
  free(cluster_of);
  free(rows);
  ix->built = true;
  ix->rows_indexed = n;

  snprintf(ix->base_path, sizeof(ix->base_path), "%s/idx_%llu.ivf", db->data_dir,
           (unsigned long long)t->id);
  snprintf(ix->log_path, sizeof(ix->log_path), "%s/idx_%llu.log", db->data_dir,
           (unsigned long long)t->id);
  ix->log_fd = open(ix->log_path, O_RDWR | O_CREAT | O_APPEND, 0644);
  t->ivf = ix;
  /* pick the probe count from the data itself */
  ivf_calibrate(ix, (const float *const *)vecs, t->alive, t->nrows, 10, 32, 0.98);
  ivf_save_base(db, t, ix);
  return true;
}

/* Persists the base file (centroids and postings). */
bool ivf_save_base(db_t *db, table_t *t, ivf_t *ix) {
  char tmp[288];
  snprintf(tmp, sizeof(tmp), "%s.tmp", ix->base_path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  uint32_t hdr[5] = {IVF_MAGIC, IVF_VERSION, (uint32_t)ix->nlist, (uint32_t)ix->dim,
                     (uint32_t)ix->nprobe};
  bool ok = write(fd, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr);
  if (ok) ok = write(fd, ix->centroids, (size_t)ix->nlist * ix->dim * sizeof(float)) ==
                       (ssize_t)((size_t)ix->nlist * ix->dim * sizeof(float));
  for (int c = 0; ok && c < ix->nlist; c++) {
    uint64_t n = ix->lists[c].n;
    ok = write(fd, &n, sizeof(n)) == (ssize_t)sizeof(n);
    if (ok && n) ok = write(fd, ix->lists[c].rows, (size_t)n * sizeof(uint64_t)) ==
                              (ssize_t)((size_t)n * sizeof(uint64_t));
  }
  if (ok) ok = fdatasync(fd) == 0;
  close(fd);
  if (ok) ok = rename(tmp, ix->base_path) == 0;
  (void)db;
  (void)t;
  return ok;
}

/* Appends the rows that entered the index to the increment log. */
bool ivf_log_append(ivf_t *ix, const uint32_t *cluster_of, const uint64_t *rows, size_t n) {
  if (!ix || ix->log_fd < 0 || n == 0) return true;
  size_t bytes = n * (sizeof(uint32_t) + sizeof(uint64_t));
  unsigned char *buf = (unsigned char *)malloc(bytes);
  if (!buf) return false;
  size_t off = 0;
  for (size_t i = 0; i < n; i++) {
    uint32_t c = cluster_of[rows[i]];
    uint64_t r = rows[i];
    memcpy(buf + off, &c, 4);
    memcpy(buf + off + 4, &r, 8);
    off += 12;
  }
  size_t done = 0;
  while (done < bytes) {
    ssize_t w = write(ix->log_fd, buf + done, bytes - done);
    if (w <= 0) {
      if (w < 0 && errno == EINTR) continue;
      free(buf);
      return false;
    }
    done += (size_t)w;
  }
  free(buf);
  return true;
}

/* Adds a row to the index, or moves it when the row is already indexed and
 * its vector changed.  A row is never in two posting lists, so a query can
 * not report it twice. */
bool ivf_add(table_t *t, uint64_t rowid) {
  ivf_t *ix = t->ivf;
  if (!ix || !ix->built) return false;
  int dim = ix->dim;
  const float *v = t->vecs[t->vec_idx][rowid];
  if (!v) return false;
  int best = 0;
  float bestd = vec_l2(v, ix->centroids, dim);
  for (int c = 1; c < ix->nlist; c++) {
    float d = vec_l2(v, ix->centroids + (size_t)c * dim, dim);
    if (d < bestd) { bestd = d; best = c; }
  }
  /* make room for the cluster of this row */
  if (ix->cluster_of && rowid >= ix->cluster_of_len) {
    uint64_t cap = ix->cluster_of_len ? ix->cluster_of_len : 1024;
    while (cap <= rowid) cap *= 2;
    uint32_t *p = (uint32_t *)realloc(ix->cluster_of, (size_t)cap * sizeof(uint32_t));
    if (p) {
      for (uint64_t i = ix->cluster_of_len; i < cap; i++) p[i] = UINT32_MAX;
      ix->cluster_of = p;
      ix->cluster_of_len = cap;
    }
  }
  uint32_t old = (ix->cluster_of && rowid < ix->cluster_of_len) ? ix->cluster_of[rowid]
                                                               : UINT32_MAX;
  if (old == (uint32_t)best) {
    /* the row is already in this cluster: it must not be added twice, or a
     * query would report it twice */
    return true;
  }
  if (old != UINT32_MAX) {
    /* the vector changed and the row belongs to another cluster now */
    list_remove(&ix->lists[old], rowid);
    ix->rows_indexed--;
  }
  list_push(&ix->lists[best], rowid);
  if (ix->cluster_of && rowid < ix->cluster_of_len) ix->cluster_of[rowid] = (uint32_t)best;
  ix->rows_indexed++;
  uint32_t c = (uint32_t)best;
  unsigned char rec[12];
  memcpy(rec, &c, 4);
  memcpy(rec + 4, &rowid, 8);
  if (ix->log_fd >= 0) {
    size_t done = 0;
    while (done < sizeof(rec)) {
      ssize_t w = write(ix->log_fd, rec + done, sizeof(rec) - done);
      if (w <= 0) break;
      done += (size_t)w;
    }
  }
  return true;
}

/* Loads a persisted index.  Returns false when there is none. */
bool ivf_load(db_t *db, table_t *t) {
  char path[256], logp[256];
  snprintf(path, sizeof(path), "%s/idx_%llu.ivf", db->data_dir, (unsigned long long)t->id);
  snprintf(logp, sizeof(logp), "%s/idx_%llu.log", db->data_dir, (unsigned long long)t->id);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  uint32_t hdr[5];
  if (read(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) { close(fd); return false; }
  if (hdr[0] != IVF_MAGIC || hdr[1] != IVF_VERSION) { close(fd); return false; }
  int nlist = (int)hdr[2], dim = (int)hdr[3];
  if (nlist <= 0 || dim <= 0 || nlist > 4096) { close(fd); return false; }
  ivf_t *ix = (ivf_t *)calloc(1, sizeof(struct ivf));
  if (!ix) { close(fd); return false; }
  ix->nlist = nlist;
  ix->dim = dim;
  ix->nprobe = (int)hdr[4];
  if (ix->nprobe < 1 || ix->nprobe > nlist) ix->nprobe = nlist / 8 + 1;
  ix->centroids = (float *)malloc((size_t)nlist * dim * sizeof(float));
  ix->lists = (ivf_list_t *)calloc((size_t)nlist, sizeof(ivf_list_t));
  if (!ix->centroids || !ix->lists) { close(fd); free(ix); return false; }
  size_t cbytes = (size_t)nlist * dim * sizeof(float);
  if (read(fd, ix->centroids, cbytes) != (ssize_t)cbytes) { close(fd); return false; }
  uint64_t total = 0;
  for (int c = 0; c < nlist; c++) {
    uint64_t n = 0;
    if (read(fd, &n, sizeof(n)) != (ssize_t)sizeof(n)) break;
    for (uint64_t i = 0; i < n; i++) {
      uint64_t r = 0;
      if (read(fd, &r, sizeof(r)) != (ssize_t)sizeof(r)) break;
      list_push(&ix->lists[c], r);
    }
    total += n;
  }
  close(fd);
  ix->built = true;
  ix->rows_indexed = total;
  /* the cluster of every row, so that an updated row can be moved */
  size_t max_row = 0;
  for (int c = 0; c < nlist; c++) {
    for (size_t k = 0; k < ix->lists[c].n; k++) {
      if (ix->lists[c].rows[k] > max_row) max_row = (size_t)ix->lists[c].rows[k];
    }
  }
  ix->cluster_of = (uint32_t *)calloc(max_row + 2, sizeof(uint32_t));
  ix->cluster_of_len = max_row + 2;
  if (ix->cluster_of) {
    for (uint32_t i = 0; i < ix->cluster_of_len; i++) ix->cluster_of[i] = UINT32_MAX;
    for (int c = 0; c < nlist; c++) {
      for (size_t k = 0; k < ix->lists[c].n; k++) {
        ix->cluster_of[ix->lists[c].rows[k]] = (uint32_t)c;
      }
    }
  }
  snprintf(ix->base_path, sizeof(ix->base_path), "%s", path);
  snprintf(ix->log_path, sizeof(ix->log_path), "%s", logp);
  ix->log_fd = open(logp, O_RDWR | O_CREAT | O_APPEND, 0644);
  /* replay the increment log */
  if (ix->log_fd >= 0) {
    lseek(ix->log_fd, 0, SEEK_SET);
    unsigned char rec[12];
    while (read(ix->log_fd, rec, sizeof(rec)) == (ssize_t)sizeof(rec)) {
      uint32_t c;
      uint64_t r;
      memcpy(&c, rec, 4);
      memcpy(&r, rec + 4, 8);
      if (c < (uint32_t)nlist) {
        if (ix->cluster_of && r < ix->cluster_of_len && ix->cluster_of[r] != UINT32_MAX &&
            ix->cluster_of[r] != c) {
          list_remove(&ix->lists[ix->cluster_of[r]], r);
          ix->rows_indexed--;
        }
        list_push(&ix->lists[c], r);
        if (ix->cluster_of && r < ix->cluster_of_len) ix->cluster_of[r] = c;
        ix->rows_indexed++;
      }
    }
    lseek(ix->log_fd, 0, SEEK_END);
  }
  t->ivf = ix;
  return true;
}

/* Searches the index.  Returns the number of candidates written to out. */
size_t ivf_search(ivf_t *ix, const float *const *vecs, const uint8_t *alive, const float *q,
                  int dim, int nprobe, size_t out_cap, dres_t *out) {
  if (!ix || !ix->built) return 0;
  int nlist = ix->nlist;
  if (nprobe < 1) nprobe = 1;
  if (nprobe > nlist) nprobe = nlist;
  /* the closest centroids */
  float *cd = (float *)malloc((size_t)nlist * sizeof(float));
  if (!cd) return 0;
  for (int c = 0; c < nlist; c++) cd[c] = vec_l2(q, ix->centroids + (size_t)c * dim, dim);
  /* a partial selection of the nprobe closest centroids */
  int *pick = (int *)malloc((size_t)nlist * sizeof(int));
  if (!pick) { free(cd); return 0; }
  int np = 0;
  for (int c = 0; c < nlist; c++) {
    if (np < nprobe) {
      int i = np++;
      pick[i] = c;
      while (i > 0 && cd[pick[i]] < cd[pick[i - 1]]) {
        int t = pick[i];
        pick[i] = pick[i - 1];
        pick[i - 1] = t;
        i--;
      }
    } else if (cd[c] < cd[pick[np - 1]]) {
      pick[np - 1] = c;
      int i = np - 1;
      while (i > 0 && cd[pick[i]] < cd[pick[i - 1]]) {
        int t = pick[i];
        pick[i] = pick[i - 1];
        pick[i - 1] = t;
        i--;
      }
    }
  }
  size_t m = 0;
  for (int i = 0; i < np; i++) {
    ivf_list_t *l = &ix->lists[pick[i]];
    for (size_t k = 0; k < l->n; k++) {
      uint64_t r = l->rows[k];
      if (alive && !alive[r]) continue;
      if (m >= out_cap) goto done;
      const float *v = vecs[r];
      if (!v) continue;
      out[m].rowid = r;
      out[m].sq = vec_l2_sq(v, q, dim);
      m++;
    }
  }
done:
  free(cd);
  free(pick);
  return m;
}

bool ivf_is_built(ivf_t *ix) { return ix && ix->built; }


/* --------------------------------------------------------- calibration */

/* Picks the number of probed clusters from the data itself.
 *
 * A fixed number of probed clusters cannot work for every distribution: for
 * uniform random vectors in a high dimensional space the true neighbours are
 * spread over the whole table, while for clustered vectors a handful of
 * clusters is enough.  Instead of guessing, the index measures its own
 * recall on a sample of the table and picks the smallest probe count that
 * reaches the target.  The query used for the measurement is one of the
 * stored vectors, so its own row is removed from the exact answer.
 */
typedef struct {
  double recall;
  int nprobe;
} calib_t;

static int exact_topk(const float *const *vecs, const uint8_t *alive, uint64_t nrows,
                      const float *q, int dim, int k, uint64_t *out) {
  int n = 0;
  for (uint64_t r = 0; r < nrows; r++) {
    if (!alive[r] || !vecs[r]) continue;
    float d = vec_l2_sq(vecs[r], q, dim);
    if (n < k) {
      out[n++] = r;
      for (int i = n - 1; i > 0; i--) {
        float di = vec_l2_sq(vecs[out[i]], q, dim);
        float dj = vec_l2_sq(vecs[out[i - 1]], q, dim);
        if (di >= dj) break;
        uint64_t tmp = out[i];
        out[i] = out[i - 1];
        out[i - 1] = tmp;
      }
    } else {
      float worst = vec_l2_sq(vecs[out[n - 1]], q, dim);
      if (d < worst) {
        out[n - 1] = r;
        for (int i = n - 1; i > 0; i--) {
          float di = vec_l2_sq(vecs[out[i]], q, dim);
          float dj = vec_l2_sq(vecs[out[i - 1]], q, dim);
          if (di >= dj) break;
          uint64_t tmp = out[i];
          out[i] = out[i - 1];
          out[i - 1] = tmp;
        }
      }
    }
  }
  return n;
}

double ivf_calibrate(ivf_t *ix, const float *const *vecs, const uint8_t *alive, uint64_t nrows,
                     int k, int samples, double target) {
  if (!ix || !ix->built || nrows == 0) return 1.0;
  if (samples < 4) samples = 4;
  /* a deterministic sample of the row ids */
  uint64_t *probe_rows = (uint64_t *)malloc((size_t)samples * sizeof(uint64_t));
  /* the exact answer holds one row more than the requested top k, so that
   * the probe row itself can be removed from it */
  uint64_t *truth = (uint64_t *)malloc((size_t)(k + 1) * sizeof(uint64_t));
  if (!probe_rows || !truth) { free(probe_rows); free(truth); return 1.0; }
  uint64_t step = nrows / (uint64_t)samples;
  if (step == 0) step = 1;
  size_t ns = 0;
  for (size_t i = 0; i < (size_t)samples; i++) {
    uint64_t r = (uint64_t)i * step;
    if (r >= nrows) break;
    if (!alive[r] || !vecs[r]) continue;
    probe_rows[ns++] = r;
  }
  if (ns == 0) { free(probe_rows); free(truth); return 1.0; }

  /* the exact answer for every probe row, without the row itself */
  int **truth_sets = (int **)calloc(ns, sizeof(int *));
  if (!truth_sets) { free(probe_rows); free(truth); return 1.0; }
  for (size_t i = 0; i < ns; i++) {
    truth_sets[i] = (int *)calloc((size_t)k, sizeof(int));
    int n = exact_topk(vecs, alive, nrows, vecs[probe_rows[i]], ix->dim, k + 1, truth);
    int m = 0;
    for (int j = 0; j < n; j++) {
      if (truth[j] == probe_rows[i]) continue;
      truth_sets[i][m++] = (int)(truth[j] + 1);   /* +1: row id 0 is valid */
      if (m >= k) break;
    }
  }

  double best = 1.0;
  int chosen = ix->nlist;
  dres_t *cand = (dres_t *)malloc((size_t)(nrows + 1) * sizeof(dres_t));
  if (!cand) goto done;
  /* the probe counts to try, always including the full table */
  int probes[16];
  int nprobes = 0;
  for (int p = 1; p <= ix->nlist && nprobes < 15; p = p < 4 ? p + 1 : p * 2) probes[nprobes++] = p;
  if (nprobes == 0 || probes[nprobes - 1] != ix->nlist) probes[nprobes++] = ix->nlist;
  for (int pi = 0; pi < nprobes; pi++) {
    int nprobe = probes[pi];
    double hit = 0.0;
    for (size_t i = 0; i < ns; i++) {
      size_t m = ivf_search(ix, vecs, alive, vecs[probe_rows[i]], ix->dim, nprobe,
                            (size_t)nrows + 1, cand);
      int found = 0;
      for (int j = 0; j < k; j++) {
        if (truth_sets[i][j] == 0) continue;
        for (size_t z = 0; z < m; z++) {
          if (cand[z].rowid + 1 == (uint64_t)truth_sets[i][j]) { found++; break; }
        }
      }
      hit += (double)found / (double)k;
    }
    best = hit / (double)ns;
    if (best >= target) { chosen = nprobe; break; }
  }
  ix->nprobe = chosen;
  fprintf(stderr, "[mvdb] index calibrated: nlist=%d nprobe=%d recall=%.4f\n", ix->nlist, chosen,
          best);
done:
  for (size_t i = 0; i < ns; i++) free(truth_sets[i]);
  free(truth_sets);
  free(cand);
  free(probe_rows);
  free(truth);
  return best;
}

int ivf_nlist(ivf_t *ix) { return ix ? ix->nlist : 0; }

int ivf_nprobe(ivf_t *ix) { return ix ? ix->nprobe : 0; }
bool ivf_set_nprobe(ivf_t *ix, int nprobe) {
  if (!ix) return false;
  if (nprobe < 1) nprobe = 1;
  if (nprobe > ix->nlist) nprobe = ix->nlist;
  ix->nprobe = nprobe;
  return true;
}
void ivf_free(ivf_t *ix) {
  if (!ix) return;
  if (ix->lists) {
    for (int c = 0; c < ix->nlist; c++) free(ix->lists[c].rows);
    free(ix->lists);
  }
  free(ix->centroids);
  free(ix->cluster_of);
  if (ix->log_fd >= 0) close(ix->log_fd);
  free(ix);
}
