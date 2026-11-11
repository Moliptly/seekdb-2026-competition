/* Vector primitives.  The distance is the plain euclidean distance between
 * the stored float32 values and the query vector. */
#include "mvdb.h"

#include <math.h>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

/* Scalar reference implementation. */
static float l2_scalar(const float *a, const float *b, int dim) {
  float sum = 0.0f;
  for (int i = 0; i < dim; i++) {
    float d = a[i] - b[i];
    sum += d * d;
  }
  return sqrtf(sum);
}

#if defined(__x86_64__)
__attribute__((target("avx2,fma"))) static float l2_avx2(const float *a, const float *b, int dim) {
  __m256 acc = _mm256_setzero_ps();
  int i = 0;
  for (; i + 8 <= dim; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    __m256 d = _mm256_sub_ps(va, vb);
    acc = _mm256_fmadd_ps(d, d, acc);
  }
  float sum = 0.0f;
  float tmp[8];
  _mm256_storeu_ps(tmp, acc);
  for (int k = 0; k < 8; k++) sum += tmp[k];
  for (; i < dim; i++) {
    float d = a[i] - b[i];
    sum += d * d;
  }
  return sqrtf(sum);
}

static int have_avx2 = -1;

float vec_l2(const float *a, const float *b, int dim) {
  if (have_avx2 < 0) have_avx2 = __builtin_cpu_supports("avx2") ? 1 : 0;
  if (have_avx2) return l2_avx2(a, b, dim);
  return l2_scalar(a, b, dim);
}
#else
float vec_l2(const float *a, const float *b, int dim) { return l2_scalar(a, b, dim); }
#endif

/* Computes the distance from one query vector to n stored vectors that are
 * stored with a fixed stride. */
void vec_l2_batch(const float *base, int dim, const float *q, int n, float *out) {
  for (int i = 0; i < n; i++) out[i] = vec_l2(base + (size_t)i * dim, q, dim);
}

/* ------------------------------------------------------- batched scanning */

#if defined(__x86_64__)
__attribute__((target("avx2,fma"))) static float l2_sq_avx2(const float *a, const float *b, int dim) {
  __m256 acc = _mm256_setzero_ps();
  int i = 0;
  for (; i + 8 <= dim; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    __m256 d = _mm256_sub_ps(va, vb);
    acc = _mm256_fmadd_ps(d, d, acc);
  }
  float tmp[8];
  _mm256_storeu_ps(tmp, acc);
  float sum = tmp[0] + tmp[1] + tmp[2] + tmp[3] + tmp[4] + tmp[5] + tmp[6] + tmp[7];
  for (; i < dim; i++) {
    float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}

/* The candidate rows of a filtered query are scattered over the table, so
 * the vectors are prefetched a few rows ahead: the memory latency is then
 * overlapped instead of being paid row by row. */
#define MVDB_PREFETCH 16

__attribute__((target("avx2,fma"))) static size_t scan_sq_avx2(const float *const *vecs,
                                                               const uint8_t *alive,
                                                               const uint64_t *rows, size_t nrows,
                                                               const float *q, int dim, dres_t *out) {
  size_t m = 0;
  for (size_t i = 0; i < nrows; i++) {
    if (i + MVDB_PREFETCH < nrows) {
      uint64_t rn = rows ? rows[i + MVDB_PREFETCH] : (uint64_t)(i + MVDB_PREFETCH);
      const float *vn = vecs[rn];
      if (vn) __builtin_prefetch(vn, 0, 1);
    }
    uint64_t r = rows ? rows[i] : (uint64_t)i;
    if (alive && !alive[r]) continue;
    const float *v = vecs[r];
    if (!v) continue;
    out[m].rowid = r;
    out[m].sq = l2_sq_avx2(v, q, dim);
    m++;
  }
  return m;
}
#endif

static float l2_sq_scalar(const float *a, const float *b, int dim) {
  float sum = 0.0f;
  for (int i = 0; i < dim; i++) {
    float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}

static size_t scan_sq_scalar(const float *const *vecs, const uint8_t *alive,
                             const uint64_t *rows, size_t nrows, const float *q, int dim,
                             dres_t *out) {
  size_t m = 0;
  for (size_t i = 0; i < nrows; i++) {
    if (i + MVDB_PREFETCH < nrows) {
      uint64_t rn = rows ? rows[i + MVDB_PREFETCH] : (uint64_t)(i + MVDB_PREFETCH);
      const float *vn = vecs[rn];
      if (vn) __builtin_prefetch(vn, 0, 1);
    }
    uint64_t r = rows ? rows[i] : (uint64_t)i;
    if (alive && !alive[r]) continue;
    const float *v = vecs[r];
    if (!v) continue;
    out[m].rowid = r;
    out[m].sq = l2_sq_scalar(v, q, dim);
    m++;
  }
  return m;
}

float vec_l2_sq(const float *a, const float *b, int dim) {
#if defined(__x86_64__)
  if (have_avx2) return l2_sq_avx2(a, b, dim);
#endif
  return l2_sq_scalar(a, b, dim);
}

size_t vec_scan_sq(const float *const *vecs, const uint8_t *alive, const uint64_t *rows,
                   size_t nrows, const float *q, int dim, dres_t *out) {
#if defined(__x86_64__)
  if (have_avx2) return scan_sq_avx2(vecs, alive, rows, nrows, q, dim, out);
#endif
  return scan_sq_scalar(vecs, alive, rows, nrows, q, dim, out);
}
