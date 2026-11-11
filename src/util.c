/* Arena allocator and JSON codec. */
#include "mvdb.h"

#include <errno.h>
#include <math.h>

/* ------------------------------------------------------------------ arena */

struct arena_chunk {
  arena_chunk *next;
  size_t used;
  size_t cap;
  char data[];
};

#define ARENA_CHUNK (64 * 1024)

void arena_init(arena *a) {
  a->head = NULL;
  a->total = 0;
}

static arena_chunk *arena_new_chunk(size_t need) {
  size_t cap = ARENA_CHUNK;
  if (need + sizeof(arena_chunk) > cap) cap = need + sizeof(arena_chunk);
  arena_chunk *c = (arena_chunk *)malloc(sizeof(arena_chunk) + cap);
  if (!c) return NULL;
  c->next = NULL;
  c->used = 0;
  c->cap = cap;
  return c;
}

void *arena_alloc(arena *a, size_t n) {
  n = (n + 15) & ~(size_t)15; /* keep 16 byte alignment */
  if (!a->head || a->head->used + n > a->head->cap) {
    arena_chunk *c = arena_new_chunk(n);
    if (!c) return NULL;
    c->next = a->head;
    a->head = c;
  }
  void *p = a->head->data + a->head->used;
  a->head->used += n;
  a->total += n;
  return p;
}

void *arena_calloc(arena *a, size_t n) {
  void *p = arena_alloc(a, n);
  if (p) memset(p, 0, n);
  return p;
}

char *arena_strndup(arena *a, const char *s, size_t n) {
  char *p = (char *)arena_alloc(a, n + 1);
  if (!p) return NULL;
  if (n) memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

char *arena_printf(arena *a, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) { va_end(ap2); return NULL; }
  char *p = (char *)arena_alloc(a, (size_t)n + 1);
  if (p) vsnprintf(p, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  return p;
}

void arena_reset(arena *a) {
  arena_chunk *c = a->head;
  while (c) {
    arena_chunk *next = c->next;
    free(c);
    c = next;
  }
  a->head = NULL;
  a->total = 0;
}

/* ------------------------------------------------------------------ outbuf */

void ob_init(outbuf *b) {
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
  b->oom = false;
}

void ob_free(outbuf *b) {
  free(b->data);
  ob_init(b);
}

static bool ob_reserve(outbuf *b, size_t extra) {
  if (b->oom) return false;
  if (b->len + extra + 1 <= b->cap) return true;
  size_t cap = b->cap ? b->cap * 2 : 4096;
  while (cap < b->len + extra + 1) cap *= 2;
  char *p = (char *)realloc(b->data, cap);
  if (!p) { b->oom = true; return false; }
  b->data = p;
  b->cap = cap;
  return true;
}

void ob_putc(outbuf *b, char c) {
  if (!ob_reserve(b, 1)) return;
  b->data[b->len++] = c;
  b->data[b->len] = '\0';
}

void ob_putn(outbuf *b, const char *s, size_t n) {
  if (!ob_reserve(b, n)) return;
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

void ob_puts(outbuf *b, const char *s) { ob_putn(b, s, strlen(s)); }

void ob_printf(outbuf *b, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) { va_end(ap2); return; }
  if (!ob_reserve(b, (size_t)n + 1)) { va_end(ap2); return; }
  vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  b->len += (size_t)n;
}

/* Emits a JSON string literal.  UTF-8 bytes are passed through unchanged so
 * that Chinese text round trips exactly; only the characters JSON requires to
 * be escaped are escaped. */
void ob_json_string(outbuf *b, const char *s, size_t n) {
  ob_putc(b, '"');
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"': ob_puts(b, "\\\""); break;
      case '\\': ob_puts(b, "\\\\"); break;
      case '\b': ob_puts(b, "\\b"); break;
      case '\f': ob_puts(b, "\\f"); break;
      case '\n': ob_puts(b, "\\n"); break;
      case '\r': ob_puts(b, "\\r"); break;
      case '\t': ob_puts(b, "\\t"); break;
      default:
        if (c < 0x20) ob_printf(b, "\\u%04x", c);
        else ob_putc(b, (char)c);
    }
  }
  ob_putc(b, '"');
}

void ob_json_int(outbuf *b, int64_t v) { ob_printf(b, "%lld", (long long)v); }

void ob_json_double(outbuf *b, double v) {
  if (!isfinite(v)) { ob_puts(b, "null"); return; }
  char tmp[40];
  snprintf(tmp, sizeof(tmp), "%.17g", v);
  /* Make sure the value reads back as a JSON number with a fractional part. */
  bool has = false;
  for (char *p = tmp; *p; p++) {
    if (*p == '.' || *p == 'e' || *p == 'E') { has = true; break; }
  }
  ob_puts(b, tmp);
  if (!has) ob_puts(b, ".0");
}

void ob_json_null(outbuf *b) { ob_puts(b, "null"); }

/* ------------------------------------------------------------------- json */

typedef struct {
  const char *p;
  const char *end;
  arena *a;
  bool ok;
} jparse;

static json_value *jvalue(jparse *j);

static void jskip(jparse *j) {
  while (j->p < j->end) {
    char c = *j->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->p++;
    else break;
  }
}

static json_value *jnew(jparse *j, json_type t) {
  json_value *v = (json_value *)arena_calloc(j->a, sizeof(json_value));
  if (!v) { j->ok = false; return NULL; }
  v->type = t;
  return v;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void utf8_encode(outbuf *b, uint32_t cp) {
  if (cp < 0x80) {
    ob_putc(b, (char)cp);
  } else if (cp < 0x800) {
    ob_putc(b, (char)(0xC0 | (cp >> 6)));
    ob_putc(b, (char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    ob_putc(b, (char)(0xE0 | (cp >> 12)));
    ob_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    ob_putc(b, (char)(0x80 | (cp & 0x3F)));
  } else {
    ob_putc(b, (char)(0xF0 | (cp >> 18)));
    ob_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
    ob_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    ob_putc(b, (char)(0x80 | (cp & 0x3F)));
  }
}

static bool jstring(jparse *j, char **out, size_t *out_len) {
  if (j->p >= j->end || *j->p != '"') return false;
  j->p++;
  outbuf b;
  ob_init(&b);
  /* Fast path: a string without escapes is copied in one go.  The SQL of a
   * batch insert is a hundred kilobyte long, so this matters. */
  {
    const char *q = j->p;
    const char *esc = memchr(q, '\\', (size_t)(j->end - q));
    const char *quote = memchr(q, '"', (size_t)(j->end - q));
    if (quote && (!esc || esc > quote)) {
      size_t n = (size_t)(quote - q);
      *out = arena_strndup(j->a, q, n);
      *out_len = n;
      j->p = quote + 1;
      return *out != NULL;
    }
  }
  while (j->p < j->end) {
    unsigned char c = (unsigned char)*j->p;
    if (c == '"') {
      j->p++;
      *out = arena_strndup(j->a, b.data ? b.data : "", b.len);
      *out_len = b.len;
      ob_free(&b);
      return *out != NULL;
    }
    if (c == '\\') {
      j->p++;
      if (j->p >= j->end) break;
      char e = *j->p++;
      switch (e) {
        case '"': ob_putc(&b, '"'); break;
        case '\\': ob_putc(&b, '\\'); break;
        case '/': ob_putc(&b, '/'); break;
        case 'b': ob_putc(&b, '\b'); break;
        case 'f': ob_putc(&b, '\f'); break;
        case 'n': ob_putc(&b, '\n'); break;
        case 'r': ob_putc(&b, '\r'); break;
        case 't': ob_putc(&b, '\t'); break;
        case 'u': {
          if (j->end - j->p < 4) { ob_free(&b); return false; }
          uint32_t cp = 0;
          for (int i = 0; i < 4; i++) {
            int h = hexval(j->p[i]);
            if (h < 0) { ob_free(&b); return false; }
            cp = (cp << 4) | (uint32_t)h;
          }
          j->p += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && j->end - j->p >= 6 &&
              j->p[0] == '\\' && j->p[1] == 'u') {
            uint32_t lo = 0;
            bool good = true;
            for (int i = 0; i < 4; i++) {
              int h = hexval(j->p[2 + i]);
              if (h < 0) { good = false; break; }
              lo = (lo << 4) | (uint32_t)h;
            }
            if (good && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              j->p += 6;
            }
          }
          utf8_encode(&b, cp);
          break;
        }
        default: ob_free(&b); return false;
      }
      continue;
    }
    ob_putc(&b, (char)c);
    j->p++;
  }
  ob_free(&b);
  return false;
}

static json_value *jvalue(jparse *j) {
  jskip(j);
  if (j->p >= j->end) { j->ok = false; return NULL; }
  char c = *j->p;
  if (c == '{') {
    j->p++;
    json_value *v = jnew(j, JSON_OBJECT);
    if (!v) return NULL;
    size_t cap = 8;
    v->keys = (char **)arena_alloc(j->a, cap * sizeof(char *));
    v->vals = (json_value **)arena_alloc(j->a, cap * sizeof(json_value *));
    if (!v->keys || !v->vals) { j->ok = false; return NULL; }
    jskip(j);
    if (j->p < j->end && *j->p == '}') { j->p++; return v; }
    for (;;) {
      jskip(j);
      char *key = NULL;
      size_t klen = 0;
      if (!jstring(j, &key, &klen)) { j->ok = false; return NULL; }
      jskip(j);
      if (j->p >= j->end || *j->p != ':') { j->ok = false; return NULL; }
      j->p++;
      json_value *val = jvalue(j);
      if (!j->ok) return NULL;
      if (v->count == cap) {
        size_t ncap = cap * 2;
        char **nk = (char **)arena_alloc(j->a, ncap * sizeof(char *));
        json_value **nv = (json_value **)arena_alloc(j->a, ncap * sizeof(json_value *));
        if (!nk || !nv) { j->ok = false; return NULL; }
        memcpy(nk, v->keys, cap * sizeof(char *));
        memcpy(nv, v->vals, cap * sizeof(json_value *));
        v->keys = nk;
        v->vals = nv;
        cap = ncap;
      }
      v->keys[v->count] = key;
      v->vals[v->count] = val;
      v->count++;
      jskip(j);
      if (j->p < j->end && *j->p == ',') { j->p++; continue; }
      if (j->p < j->end && *j->p == '}') { j->p++; break; }
      j->ok = false;
      return NULL;
    }
    return v;
  }
  if (c == '[') {
    j->p++;
    json_value *v = jnew(j, JSON_ARRAY);
    if (!v) return NULL;
    size_t cap = 8;
    v->vals = (json_value **)arena_alloc(j->a, cap * sizeof(json_value *));
    if (!v->vals) { j->ok = false; return NULL; }
    jskip(j);
    if (j->p < j->end && *j->p == ']') { j->p++; return v; }
    for (;;) {
      json_value *val = jvalue(j);
      if (!j->ok) return NULL;
      if (v->count == cap) {
        size_t ncap = cap * 2;
        json_value **nv = (json_value **)arena_alloc(j->a, ncap * sizeof(json_value *));
        if (!nv) { j->ok = false; return NULL; }
        memcpy(nv, v->vals, cap * sizeof(json_value *));
        v->vals = nv;
        cap = ncap;
      }
      v->vals[v->count++] = val;
      jskip(j);
      if (j->p < j->end && *j->p == ',') { j->p++; continue; }
      if (j->p < j->end && *j->p == ']') { j->p++; break; }
      j->ok = false;
      return NULL;
    }
    return v;
  }
  if (c == '"') {
    json_value *v = jnew(j, JSON_STRING);
    if (!v) return NULL;
    if (!jstring(j, &v->sval, &v->slen)) { j->ok = false; return NULL; }
    return v;
  }
  if ((size_t)(j->end - j->p) >= 4 && memcmp(j->p, "true", 4) == 0) {
    j->p += 4;
    json_value *v = jnew(j, JSON_BOOL);
    if (v) v->bval = true;
    return v;
  }
  if ((size_t)(j->end - j->p) >= 5 && memcmp(j->p, "false", 5) == 0) {
    j->p += 5;
    json_value *v = jnew(j, JSON_BOOL);
    if (v) v->bval = false;
    return v;
  }
  if ((size_t)(j->end - j->p) >= 4 && memcmp(j->p, "null", 4) == 0) {
    j->p += 4;
    return jnew(j, JSON_NULL);
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    const char *start = j->p;
    bool isint = true;
    if (*j->p == '-') j->p++;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    if (j->p < j->end && *j->p == '.') {
      isint = false;
      j->p++;
      while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
      isint = false;
      j->p++;
      if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
      while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    }
    size_t n = (size_t)(j->p - start);
    char tmp[64];
    if (n >= sizeof(tmp)) { j->ok = false; return NULL; }
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    if (isint) {
      errno = 0;
      char *endp = NULL;
      long long v = strtoll(tmp, &endp, 10);
      if (errno == 0 && endp && *endp == '\0') {
        json_value *jv = jnew(j, JSON_INT);
        if (jv) jv->ival = (int64_t)v;
        return jv;
      }
    }
    json_value *jv = jnew(j, JSON_DOUBLE);
    if (jv) jv->dval = strtod(tmp, NULL);
    return jv;
  }
  j->ok = false;
  return NULL;
}

json_value *json_parse(arena *a, const char *text, size_t len) {
  jparse j;
  j.p = text;
  j.end = text + len;
  j.a = a;
  j.ok = true;
  json_value *v = jvalue(&j);
  if (!j.ok) return NULL;
  return v;
}

const json_value *json_get(const json_value *obj, const char *key) {
  if (!obj || obj->type != JSON_OBJECT) return NULL;
  for (size_t i = 0; i < obj->count; i++) {
    if (obj->keys[i] && strcmp(obj->keys[i], key) == 0) return obj->vals[i];
  }
  return NULL;
}
