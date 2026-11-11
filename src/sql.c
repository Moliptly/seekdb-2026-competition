/* SQL subset: lexer, recursive descent parser, AST construction.
 *
 * Supported statements (the contest subset):
 *   CREATE TABLE t (coldef, ...)
 *   CREATE VECTOR INDEX idx ON t (vec_col)
 *   INSERT INTO t [(cols)] VALUES (...), (...)
 *   SELECT [* | expr [AS alias], ...] FROM t [WHERE cond] [ORDER BY ...] [LIMIT n]
 *   UPDATE t SET col = value, ... [WHERE cond]
 *   DELETE FROM t [WHERE cond]
 *   BEGIN / COMMIT / ROLLBACK
 *
 * Anything else fails the parse and is reported as unsupported_sql.
 */
#include "mvdb.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>

typedef enum {
  T_EOF = 0, T_IDENT, T_INT, T_FLOAT, T_STRING, T_VECLIT,
  T_LPAREN, T_RPAREN, T_COMMA, T_SEMI, T_STAR, T_DOT,
  T_EQ, T_LT, T_LE, T_GT, T_GE, T_NE,
  T_CREATE, T_TABLE, T_INSERT, T_INTO, T_VALUES, T_SELECT, T_FROM, T_WHERE,
  T_AND, T_OR, T_ORDER, T_BY, T_ASC, T_DESC, T_LIMIT, T_UPDATE, T_SET,
  T_DELETE, T_BEGIN, T_COMMIT, T_ROLLBACK, T_INDEX, T_VECTOR, T_ON,
  T_PRIMARY, T_KEY, T_BIGINT, T_VARCHAR, T_AS, T_L2, T_NULL, T_IS, T_NOT
} tok_kind;

typedef struct {
  tok_kind kind;
  const char *text;
  size_t len;
  int64_t ival;
  double dval;
  char *sval;
  int slen;
  char *vraw;
  int vrawlen;
} token;

typedef struct {
  arena *a;
  const char *p;
  const char *end;
  token cur;
  char err[192];
  bool failed;
} parser;

static const struct { const char *name; tok_kind kind; } keywords[] = {
  {"create", T_CREATE}, {"table", T_TABLE}, {"insert", T_INSERT}, {"into", T_INTO},
  {"values", T_VALUES}, {"select", T_SELECT}, {"from", T_FROM}, {"where", T_WHERE},
  {"and", T_AND}, {"or", T_OR}, {"order", T_ORDER}, {"by", T_BY}, {"asc", T_ASC},
  {"desc", T_DESC}, {"limit", T_LIMIT}, {"update", T_UPDATE}, {"set", T_SET},
  {"delete", T_DELETE}, {"begin", T_BEGIN}, {"commit", T_COMMIT}, {"rollback", T_ROLLBACK},
  {"index", T_INDEX}, {"vector", T_VECTOR}, {"on", T_ON}, {"primary", T_PRIMARY},
  {"key", T_KEY}, {"bigint", T_BIGINT}, {"varchar", T_VARCHAR}, {"as", T_AS},
  {"l2_distance", T_L2}, {"null", T_NULL}, {"is", T_IS}, {"not", T_NOT},
  {NULL, 0}
};

static void perr(parser *ps, const char *fmt, ...) {
  if (ps->failed) return;
  ps->failed = true;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ps->err, sizeof(ps->err), fmt, ap);
  va_end(ap);
}

static bool ident_start(unsigned char c) {
  return isalpha(c) || c == '_' || c == '$' || c >= 0x80;
}
static bool ident_char(unsigned char c) {
  return isalnum(c) || c == '_' || c == '$' || c >= 0x80;
}

static void lex_string(parser *ps, char quote) {
  ps->p++;
  outbuf b;
  ob_init(&b);
  while (ps->p < ps->end && *ps->p != quote) {
    if (*ps->p == '\\' && ps->p + 1 < ps->end) {
      ps->p++;
      char e = *ps->p++;
      switch (e) {
        case 'n': ob_putc(&b, '\n'); break;
        case 't': ob_putc(&b, '\t'); break;
        case 'r': ob_putc(&b, '\r'); break;
        case '0': ob_putc(&b, '\0'); break;
        case '\\': ob_putc(&b, '\\'); break;
        case '\'': ob_putc(&b, '\''); break;
        case '"': ob_putc(&b, '"'); break;
        default: ob_putc(&b, e); break;
      }
      continue;
    }
    if (*ps->p == quote && ps->p + 1 < ps->end && ps->p[1] == quote) {
      ob_putc(&b, quote);
      ps->p += 2;
      continue;
    }
    ob_putc(&b, *ps->p++);
  }
  if (ps->p >= ps->end) {
    ob_free(&b);
    perr(ps, "unterminated string literal");
    return;
  }
  ps->p++;
  ps->cur.kind = T_STRING;
  ps->cur.sval = arena_strndup(ps->a, b.data ? b.data : "", b.len);
  ps->cur.slen = (int)b.len;
  ob_free(&b);
}

static void lex_veclit(parser *ps) {
  ps->p++;
  const char *s = ps->p;
  while (ps->p < ps->end && *ps->p != ']') ps->p++;
  if (ps->p >= ps->end) {
    perr(ps, "unterminated vector literal");
    return;
  }
  ps->cur.kind = T_VECLIT;
  ps->cur.vraw = arena_strndup(ps->a, s, (size_t)(ps->p - s));
  ps->cur.vrawlen = (int)(ps->p - s);
  ps->p++;
}


/* A fast decimal parser.  The vectors of a batch insert are the bulk of the
 * SQL text, and strtod is far slower than a plain digit loop.  Returns 0 on
 * success, 1 when the text is not a number and 2 when it has an exponent
 * (then strtod is used). */
static int fast_atof(const char *p, size_t n, double *out) {
  size_t i = 0;
  bool neg = false;
  if (i < n && (p[i] == '-' || p[i] == '+')) {
    neg = (p[i] == '-');
    i++;
  }
  size_t digits = 0;
  double v = 0.0;
  while (i < n && p[i] >= '0' && p[i] <= '9') {
    v = v * 10.0 + (double)(p[i] - '0');
    i++;
    digits++;
  }
  if (i < n && p[i] == '.') {
    i++;
    double scale = 0.1;
    while (i < n && p[i] >= '0' && p[i] <= '9') {
      v += (double)(p[i] - '0') * scale;
      scale *= 0.1;
      i++;
      digits++;
    }
  }
  if (digits == 0) return 1;
  if (i < n && (p[i] == 'e' || p[i] == 'E')) return 2;
  if (i != n) return 1;
  *out = neg ? -v : v;
  return 0;
}

/* Parses a comma separated list of numbers into float32.  Values that are
 * not finite or that do not fit into float32 are flagged instead of failing
 * the parse, so that the executor can report them as invalid vector values.
 * The text may or may not be wrapped in brackets. */
static bool parse_vec_body(parser *ps, const char *text, size_t len, float **out, int *out_dim,
                           bool *invalid, char *invalid_msg, size_t invalid_msg_sz) {
  const char *s = text;
  size_t n = len;
  while (n > 0 && isspace((unsigned char)s[0])) { s++; n--; }
  while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
  if (n >= 2 && s[0] == '[' && s[n - 1] == ']') { s++; n -= 2; }
  int cap = 16, cnt = 0;
  float *vals = (float *)arena_alloc(ps->a, (size_t)cap * sizeof(float));
  if (!vals) { perr(ps, "out of memory"); return false; }
  *invalid = false;
  size_t i = 0;
  while (i < n) {
    while (i < n && (isspace((unsigned char)s[i]) || s[i] == ',')) i++;
    if (i >= n) break;
    const char *start = s + i;
    while (i < n && s[i] != ',' && !isspace((unsigned char)s[i])) i++;
    size_t len2 = (size_t)(s + i - start);
    if (len2 == 0) continue;
    char tmp[64];
    if (len2 >= sizeof(tmp)) { perr(ps, "vector element too long"); return false; }
    memcpy(tmp, start, len2);
    tmp[len2] = '\0';
    /* the non finite spellings are accepted by the grammar and rejected by
     * the semantic check, so that they produce invalid_vector */
    if (len2 == 3 && strncasecmp(tmp, "nan", 3) == 0) {
      if (!*invalid) {
        *invalid = true;
        snprintf(invalid_msg, invalid_msg_sz, "vector element is not finite");
      }
      continue;
    }
    if ((len2 == 3 && strncasecmp(tmp, "inf", 3) == 0) ||
        (len2 == 8 && strncasecmp(tmp, "infinity", 8) == 0)) {
      if (!*invalid) {
        *invalid = true;
        snprintf(invalid_msg, invalid_msg_sz, "vector element is not finite");
      }
      continue;
    }
    double d;
    int st = fast_atof(tmp, len2, &d);
    if (st == 2) {
      errno = 0;
      char *endp = NULL;
      d = strtod(tmp, &endp);
      if (!endp || endp == tmp || *endp != '\0') {
        perr(ps, "invalid vector element '%s'", tmp);
        return false;
      }
    } else if (st != 0) {
      perr(ps, "invalid vector element '%s'", tmp);
      return false;
    }
    if (!isfinite(d)) {
      if (!*invalid) {
        *invalid = true;
        snprintf(invalid_msg, invalid_msg_sz, "vector element is not finite");
      }
    } else if (fabs(d) > 3.4028234663852886e38) {
      if (!*invalid) {
        *invalid = true;
        snprintf(invalid_msg, invalid_msg_sz, "vector element overflows float32");
      }
    }
    if (cnt == cap) {
      cap *= 2;
      float *nv = (float *)arena_alloc(ps->a, (size_t)cap * sizeof(float));
      if (!nv) { perr(ps, "out of memory"); return false; }
      memcpy(nv, vals, (size_t)cnt * sizeof(float));
      vals = nv;
    }
    vals[cnt++] = (float)d;
  }
  *out = vals;
  *out_dim = cnt;
  return true;
}

static void next_token(parser *ps) {
  memset(&ps->cur, 0, sizeof(ps->cur));
  while (ps->p < ps->end && isspace((unsigned char)*ps->p)) ps->p++;
  if (ps->p >= ps->end) { ps->cur.kind = T_EOF; return; }
  char c = *ps->p;

  if (ident_start((unsigned char)c)) {
    const char *s = ps->p;
    while (ps->p < ps->end && ident_char((unsigned char)*ps->p)) ps->p++;
    size_t n = (size_t)(ps->p - s);
    ps->cur.text = s;
    ps->cur.len = n;
    ps->cur.kind = T_IDENT;
    for (int i = 0; keywords[i].name; i++) {
      if (strlen(keywords[i].name) == n && strncasecmp(keywords[i].name, s, n) == 0) {
        ps->cur.kind = keywords[i].kind;
        break;
      }
    }
    return;
  }
  if (isdigit((unsigned char)c) ||
      (c == '-' && ps->p + 1 < ps->end && isdigit((unsigned char)ps->p[1])) ||
      (c == '.' && ps->p + 1 < ps->end && isdigit((unsigned char)ps->p[1]))) {
    const char *s = ps->p;
    bool isint = true;
    if (*ps->p == '-') ps->p++;
    while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) ps->p++;
    if (ps->p < ps->end && *ps->p == '.') {
      isint = false;
      ps->p++;
      while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
      const char *save = ps->p;
      ps->p++;
      if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ps->p++;
      if (ps->p < ps->end && isdigit((unsigned char)*ps->p)) {
        isint = false;
        while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) ps->p++;
      } else {
        ps->p = save;
      }
    }
    size_t n = (size_t)(ps->p - s);
    char tmp[64];
    if (n >= sizeof(tmp)) { perr(ps, "numeric literal too long"); return; }
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    ps->cur.text = s;
    ps->cur.len = n;
    if (isint) {
      errno = 0;
      char *endp = NULL;
      long long v = strtoll(tmp, &endp, 10);
      if (errno == 0 && endp && *endp == '\0') {
        ps->cur.kind = T_INT;
        ps->cur.ival = (int64_t)v;
        return;
      }
    }
    ps->cur.kind = T_FLOAT;
    ps->cur.dval = strtod(tmp, NULL);
    return;
  }
  if (c == '\'' || c == '"') { lex_string(ps, c); return; }
  if (c == '[') { lex_veclit(ps); return; }
  ps->p++;
  switch (c) {
    case '(': ps->cur.kind = T_LPAREN; return;
    case ')': ps->cur.kind = T_RPAREN; return;
    case ',': ps->cur.kind = T_COMMA; return;
    case ';': ps->cur.kind = T_SEMI; return;
    case '*': ps->cur.kind = T_STAR; return;
    case '.': ps->cur.kind = T_DOT; return;
    case '=': ps->cur.kind = T_EQ; return;
    case '<':
      if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = T_LE; }
      else if (ps->p < ps->end && *ps->p == '>') { ps->p++; ps->cur.kind = T_NE; }
      else ps->cur.kind = T_LT;
      return;
    case '>':
      if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = T_GE; }
      else ps->cur.kind = T_GT;
      return;
    case '!':
      if (ps->p < ps->end && *ps->p == '=') { ps->p++; ps->cur.kind = T_NE; return; }
      perr(ps, "unsupported operator '!'");
      return;
    default:
      perr(ps, "unexpected character '%c'", c);
      return;
  }
}

static bool accept_tok(parser *ps, tok_kind k) {
  if (ps->cur.kind == k) { next_token(ps); return true; }
  return false;
}

static bool expect_tok(parser *ps, tok_kind k, const char *what) {
  if (ps->cur.kind == k) { next_token(ps); return true; }
  perr(ps, "expected %s", what);
  return false;
}

static bool copy_ident(parser *ps, char *dst, size_t dstsz) {
  if (ps->cur.kind != T_IDENT) { perr(ps, "expected an identifier"); return false; }
  if (ps->cur.len >= dstsz) { perr(ps, "identifier too long"); return false; }
  memcpy(dst, ps->cur.text, ps->cur.len);
  dst[ps->cur.len] = '\0';
  next_token(ps);
  return true;
}

/* Parses a vector literal token into float32. */
static bool parse_vec_literal(parser *ps, float **out, int *out_dim, bool *invalid,
                              char *invalid_msg, size_t invalid_msg_sz) {
  *invalid = false;
  if (!parse_vec_body(ps, ps->cur.vraw, (size_t)ps->cur.vrawlen, out, out_dim, invalid,
                      invalid_msg, invalid_msg_sz)) {
    return false;
  }
  next_token(ps);
  return true;
}

/* Parses a vector given as a quoted string, e.g. '[0,2]'. */
static bool parse_vec_string(parser *ps, const char *text, size_t len, float **out, int *out_dim,
                             bool *invalid, char *invalid_msg, size_t invalid_msg_sz) {
  *invalid = false;
  return parse_vec_body(ps, text, len, out, out_dim, invalid, invalid_msg, invalid_msg_sz);
}

/* expr := IDENT | L2_DISTANCE ( IDENT , veclit ) */
static expr *parse_expr(parser *ps) {
  expr *e = (expr *)arena_calloc(ps->a, sizeof(expr));
  if (!e) { perr(ps, "out of memory"); return NULL; }
  e->col = -1;
  if (ps->cur.kind == T_L2) {
    next_token(ps);
    if (!expect_tok(ps, T_LPAREN, "'('")) return NULL;
    if (ps->cur.kind != T_IDENT) { perr(ps, "expected a vector column"); return NULL; }
    if (!copy_ident(ps, e->colname, sizeof(e->colname))) return NULL;
    if (!expect_tok(ps, T_COMMA, "','")) return NULL;
    if (ps->cur.kind == T_VECLIT) {
      if (!parse_vec_literal(ps, &e->vec, &e->veclen, &e->invalid, e->invalid_msg,
                             sizeof(e->invalid_msg))) return NULL;
    } else if (ps->cur.kind == T_STRING) {
      if (!parse_vec_string(ps, ps->cur.sval, (size_t)ps->cur.slen, &e->vec, &e->veclen,
                            &e->invalid, e->invalid_msg, sizeof(e->invalid_msg))) return NULL;
      next_token(ps);
    } else {
      perr(ps, "expected a vector literal");
      return NULL;
    }
    if (!expect_tok(ps, T_RPAREN, "')'")) return NULL;
    e->kind = EX_L2;
    return e;
  }
  if (ps->cur.kind == T_IDENT) {
    if (!copy_ident(ps, e->colname, sizeof(e->colname))) return NULL;
    e->kind = EX_COL;
    return e;
  }
  perr(ps, "unsupported expression");
  return NULL;
}

/* cond := comparison (AND comparison)* */
static cond *parse_cond(parser *ps) {
  cond *head = NULL, *tail = NULL;
  for (;;) {
    cond *c = (cond *)arena_calloc(ps->a, sizeof(cond));
    if (!c) { perr(ps, "out of memory"); return NULL; }
    c->col = -1;
    if (ps->cur.kind != T_IDENT) { perr(ps, "expected a column in WHERE"); return NULL; }
    if (!copy_ident(ps, c->colname, sizeof(c->colname))) return NULL;
    tok_kind op = ps->cur.kind;
    if (op != T_EQ && op != T_LT && op != T_LE && op != T_GT && op != T_GE) {
      perr(ps, "unsupported comparison operator");
      return NULL;
    }
    next_token(ps);
    if (ps->cur.kind == T_INT) {
      c->ival = ps->cur.ival;
      next_token(ps);
    } else if (ps->cur.kind == T_FLOAT) {
      c->is_float = true;
      c->fval = ps->cur.dval;
      c->ival = (int64_t)c->fval;
      next_token(ps);
    } else if (ps->cur.kind == T_STRING) {
      c->sval = ps->cur.sval;
      c->slen = ps->cur.slen;
      next_token(ps);
    } else if (ps->cur.kind == T_IDENT) {
      c->sval = arena_strndup(ps->a, ps->cur.text, ps->cur.len);
      c->slen = (int)ps->cur.len;
      next_token(ps);
    } else {
      perr(ps, "unsupported WHERE value");
      return NULL;
    }
    switch (op) {
      case T_EQ: c->op = CMP_EQ; break;
      case T_LT: c->op = CMP_LT; break;
      case T_LE: c->op = CMP_LE; break;
      case T_GT: c->op = CMP_GT; break;
      default: c->op = CMP_GE; break;
    }
    if (tail) tail->next = c; else head = c;
    tail = c;
    if (ps->cur.kind == T_AND) { next_token(ps); continue; }
    break;
  }
  return head;
}

/* A value in an INSERT tuple or an UPDATE SET clause. */
static expr *parse_value(parser *ps) {
  expr *e = (expr *)arena_calloc(ps->a, sizeof(expr));
  if (!e) { perr(ps, "out of memory"); return NULL; }
  e->col = -1;
  if (ps->cur.kind == T_INT) {
    e->kind = EX_INT;
    e->ival = ps->cur.ival;
    next_token(ps);
    return e;
  }
  if (ps->cur.kind == T_FLOAT) {
    e->kind = EX_INT;
    e->ival = (int64_t)ps->cur.dval;
    next_token(ps);
    return e;
  }
  if (ps->cur.kind == T_STRING) {
    /* a quoted vector literal such as '[0,2]' is a vector value */
    if (ps->cur.slen >= 2 && ps->cur.sval[0] == '[') {
      e->kind = EX_VEC;
      if (!parse_vec_string(ps, ps->cur.sval, (size_t)ps->cur.slen, &e->vec, &e->veclen,
                            &e->invalid, e->invalid_msg, sizeof(e->invalid_msg))) return NULL;
      next_token(ps);
      return e;
    }
    e->kind = EX_STR;
    e->sval = ps->cur.sval;
    e->slen = ps->cur.slen;
    next_token(ps);
    return e;
  }
  if (ps->cur.kind == T_VECLIT) {
    e->kind = EX_VEC;
    if (!parse_vec_literal(ps, &e->vec, &e->veclen, &e->invalid, e->invalid_msg,
                           sizeof(e->invalid_msg))) return NULL;
    return e;
  }
  if (ps->cur.kind == T_IDENT) {
    e->kind = EX_STR;
    e->sval = arena_strndup(ps->a, ps->cur.text, ps->cur.len);
    e->slen = (int)ps->cur.len;
    next_token(ps);
    return e;
  }
  perr(ps, "unsupported value");
  return NULL;
}

static bool parse_create_table(parser *ps, stmt *st) {
  st->type = ST_CREATE_TABLE;
  if (!expect_tok(ps, T_TABLE, "'TABLE'")) return false;
  if (!copy_ident(ps, st->table, sizeof(st->table))) return false;
  if (!expect_tok(ps, T_LPAREN, "'('")) return false;
  int n = 0;
  for (;;) {
    if (n >= 16) { perr(ps, "too many columns"); return false; }
    column_t *col = &st->cols[n];
    memset(col, 0, sizeof(*col));
    if (!copy_ident(ps, col->name, sizeof(col->name))) return false;
    if (ps->cur.kind == T_BIGINT) {
      col->type = CT_BIGINT;
      next_token(ps);
    } else if (ps->cur.kind == T_VARCHAR) {
      col->type = CT_VARCHAR;
      next_token(ps);
      if (!expect_tok(ps, T_LPAREN, "'('")) return false;
      if (ps->cur.kind != T_INT) { perr(ps, "expected a VARCHAR length"); return false; }
      if (ps->cur.ival < 0 || ps->cur.ival > 65535) {
        perr(ps, "the VARCHAR length is out of range");
        return false;
      }
      col->vlen = (int)ps->cur.ival;
      next_token(ps);
      if (!expect_tok(ps, T_RPAREN, "')'")) return false;
    } else if (ps->cur.kind == T_VECTOR) {
      col->type = CT_VECTOR;
      next_token(ps);
      if (!expect_tok(ps, T_LPAREN, "'('")) return false;
      if (ps->cur.kind != T_INT) { perr(ps, "expected a VECTOR dimension"); return false; }
      if (ps->cur.ival <= 0 || ps->cur.ival > 65535) {
        perr(ps, "the vector dimension is out of range");
        return false;
      }
      col->dim = (int)ps->cur.ival;
      next_token(ps);
      if (!expect_tok(ps, T_RPAREN, "')'")) return false;
    } else {
      perr(ps, "unsupported column type");
      return false;
    }
    if (ps->cur.kind == T_PRIMARY) {
      next_token(ps);
      if (!expect_tok(ps, T_KEY, "'KEY'")) return false;
      col->pk = true;
      if (col->type != CT_BIGINT) { perr(ps, "only BIGINT primary keys are supported"); return false; }
    }
    n++;
    if (accept_tok(ps, T_COMMA)) continue;
    break;
  }
  if (!expect_tok(ps, T_RPAREN, "')'")) return false;
  st->ncols = n;
  st->pk_idx = -1;
  st->vec_idx = -1;
  for (int i = 0; i < n; i++) {
    if (st->cols[i].pk) {
      if (st->pk_idx >= 0) { perr(ps, "only one primary key is allowed"); return false; }
      st->pk_idx = i;
    }
    if (st->cols[i].type == CT_VECTOR) {
      if (st->vec_idx >= 0) { perr(ps, "only one vector column is allowed"); return false; }
      st->vec_idx = i;
    }
  }
  return true;
}

static bool parse_create_index(parser *ps, stmt *st) {
  st->type = ST_CREATE_INDEX;
  if (!expect_tok(ps, T_INDEX, "'INDEX'")) return false;
  if (!copy_ident(ps, st->index_name, sizeof(st->index_name))) return false;
  if (!expect_tok(ps, T_ON, "'ON'")) return false;
  if (!copy_ident(ps, st->table, sizeof(st->table))) return false;
  if (!expect_tok(ps, T_LPAREN, "'('")) return false;
  if (ps->cur.kind != T_IDENT) { perr(ps, "expected a vector column"); return false; }
  if (!copy_ident(ps, st->cols[0].name, sizeof(st->cols[0].name))) return false;
  if (!expect_tok(ps, T_RPAREN, "')'")) return false;
  st->ncols = 1;
  st->cols[0].type = CT_VECTOR;
  return true;
}

static bool parse_insert(parser *ps, stmt *st) {
  st->type = ST_INSERT;
  if (!expect_tok(ps, T_INTO, "'INTO'")) return false;
  if (!copy_ident(ps, st->table, sizeof(st->table))) return false;
  int cap = 8, n = 0;
  int *icols = (int *)arena_alloc(ps->a, (size_t)cap * sizeof(int));
  char **icolnames = (char **)arena_alloc(ps->a, (size_t)cap * sizeof(char *));
  if (!icols || !icolnames) { perr(ps, "out of memory"); return false; }
  if (ps->cur.kind == T_LPAREN) {
    next_token(ps);
    for (;;) {
      if (n == cap) {
        cap *= 2;
        int *ni = (int *)arena_alloc(ps->a, (size_t)cap * sizeof(int));
        char **nn = (char **)arena_alloc(ps->a, (size_t)cap * sizeof(char *));
        if (!ni || !nn) { perr(ps, "out of memory"); return false; }
        memcpy(ni, icols, (size_t)n * sizeof(int));
        memcpy(nn, icolnames, (size_t)n * sizeof(char *));
        icols = ni;
        icolnames = nn;
      }
      char name[64];
      if (!copy_ident(ps, name, sizeof(name))) return false;
      icols[n] = -1; /* resolved later against the schema */
      icolnames[n] = arena_strndup(ps->a, name, strlen(name));
      n++;
      if (accept_tok(ps, T_COMMA)) continue;
      break;
    }
    if (!expect_tok(ps, T_RPAREN, "')'")) return false;
  }
  st->icols = icols;
  st->icolnames = icolnames;
  st->nicols = n;
  if (!expect_tok(ps, T_VALUES, "'VALUES'")) return false;

  int vcap = 16, vn = 0;
  expr **vals = (expr **)arena_alloc(ps->a, (size_t)vcap * sizeof(expr *));
  if (!vals) { perr(ps, "out of memory"); return false; }
  int rows = 0, perrow = 0;
  for (;;) {
    if (!expect_tok(ps, T_LPAREN, "'('")) return false;
    int cnt = 0;
    for (;;) {
      if (vn == vcap) {
        vcap *= 2;
        expr **nv = (expr **)arena_alloc(ps->a, (size_t)vcap * sizeof(expr *));
        if (!nv) { perr(ps, "out of memory"); return false; }
        memcpy(nv, vals, (size_t)vn * sizeof(expr *));
        vals = nv;
      }
      expr *v = parse_value(ps);
      if (!v) return false;
      vals[vn++] = v;
      cnt++;
      if (accept_tok(ps, T_COMMA)) continue;
      break;
    }
    if (!expect_tok(ps, T_RPAREN, "')'")) return false;
    if (rows == 0) perrow = cnt;
    else if (cnt != perrow) { perr(ps, "all rows must have the same number of values"); return false; }
    rows++;
    if (accept_tok(ps, T_COMMA)) continue;
    break;
  }
  st->vals = vals;
  st->nrows = rows;
  st->nvalcols = perrow;
  return true;
}

static bool parse_select(parser *ps, stmt *st) {
  st->type = ST_SELECT;
  int cap = 8, n = 0;
  expr *items = (expr *)arena_alloc(ps->a, (size_t)cap * sizeof(expr));
  if (!items) { perr(ps, "out of memory"); return false; }
  st->star = false;
  if (ps->cur.kind == T_STAR) {
    next_token(ps);
    st->star = true;
  } else {
    for (;;) {
      if (n == cap) {
        cap *= 2;
        expr *ni = (expr *)arena_alloc(ps->a, (size_t)cap * sizeof(expr));
        if (!ni) { perr(ps, "out of memory"); return false; }
        memcpy(ni, items, (size_t)n * sizeof(expr));
        items = ni;
      }
      expr *e = parse_expr(ps);
      if (!e) return false;
      if (accept_tok(ps, T_AS)) {
        char alias[64];
        if (!copy_ident(ps, alias, sizeof(alias))) return false;
        snprintf(e->alias, sizeof(e->alias), "%s", alias);
        e->has_alias = true;
      }
      items[n++] = *e;
      if (accept_tok(ps, T_COMMA)) continue;
      break;
    }
  }
  st->items = items;
  st->nitems = n;
  st->nout = n;
  st->topk_item = -1;
  st->order_kind = 0;
  st->order_col = -1;
  st->order_item = -1;

  if (!expect_tok(ps, T_FROM, "'FROM'")) return false;
  if (!copy_ident(ps, st->table, sizeof(st->table))) return false;
  if (ps->cur.kind == T_WHERE) {
    next_token(ps);
    st->where = parse_cond(ps);
    if (!st->where) return false;
  }
  if (ps->cur.kind == T_ORDER) {
    next_token(ps);
    if (!expect_tok(ps, T_BY, "'BY'")) return false;
    if (ps->cur.kind == T_IDENT) {
      char name[64];
      if (!copy_ident(ps, name, sizeof(name))) return false;
      /* An output alias wins over a table column so that
       * "ORDER BY distance" sorts by the projected distance. */
      int alias_hit = -1;
      for (int i = 0; i < st->nout; i++) {
        if (items[i].has_alias && strcasecmp(items[i].alias, name) == 0) { alias_hit = i; break; }
      }
      if (alias_hit >= 0) {
        st->order_kind = 2;
        st->order_item = alias_hit;
        if (items[alias_hit].kind == EX_L2) st->topk_item = alias_hit;
      } else {
        st->order_kind = 1;
        st->order_col = -1;
        snprintf(st->order_colname, sizeof(st->order_colname), "%s", name);
      }
    } else if (ps->cur.kind == T_L2) {
      expr *e = parse_expr(ps);
      if (!e) return false;
      /* keep the ordering expression as a hidden extra item */
      expr *ni = (expr *)arena_alloc(ps->a, (size_t)(st->nitems + 1) * sizeof(expr));
      if (!ni) { perr(ps, "out of memory"); return false; }
      memcpy(ni, items, (size_t)st->nitems * sizeof(expr));
      ni[st->nitems] = *e;
      items = ni;
      st->items = items;
      st->order_kind = 2;
      st->order_item = st->nitems;
      st->nitems++;
      st->topk_item = st->order_item;
    } else {
      perr(ps, "unsupported ORDER BY term");
      return false;
    }
    if (accept_tok(ps, T_ASC)) { /* default */ }
    if (accept_tok(ps, T_DESC)) st->order_desc = true;
  }
  if (ps->cur.kind == T_LIMIT) {
    next_token(ps);
    if (ps->cur.kind != T_INT) { perr(ps, "expected a LIMIT count"); return false; }
    st->has_limit = true;
    st->limit = ps->cur.ival;
    next_token(ps);
  }
  return true;
}

static bool parse_update(parser *ps, stmt *st) {
  st->type = ST_UPDATE;
  if (!copy_ident(ps, st->table, sizeof(st->table))) return false;
  if (!expect_tok(ps, T_SET, "'SET'")) return false;
  int cap = 8, n = 0;
  int *cols = (int *)arena_alloc(ps->a, (size_t)cap * sizeof(int));
  char **colnames = (char **)arena_alloc(ps->a, (size_t)cap * sizeof(char *));
  expr **vals = (expr **)arena_alloc(ps->a, (size_t)cap * sizeof(expr *));
  if (!cols || !colnames || !vals) { perr(ps, "out of memory"); return false; }
  for (;;) {
    if (n == cap) {
      cap *= 2;
      int *nc = (int *)arena_alloc(ps->a, (size_t)cap * sizeof(int));
      char **nn = (char **)arena_alloc(ps->a, (size_t)cap * sizeof(char *));
      expr **nv = (expr **)arena_alloc(ps->a, (size_t)cap * sizeof(expr *));
      if (!nc || !nn || !nv) { perr(ps, "out of memory"); return false; }
      memcpy(nc, cols, (size_t)n * sizeof(int));
      memcpy(nn, colnames, (size_t)n * sizeof(char *));
      memcpy(nv, vals, (size_t)n * sizeof(expr *));
      cols = nc;
      colnames = nn;
      vals = nv;
    }
    char name[64];
    if (!copy_ident(ps, name, sizeof(name))) return false;
    if (!expect_tok(ps, T_EQ, "'='")) return false;
    expr *v = parse_value(ps);
    if (!v) return false;
    cols[n] = -1;
    colnames[n] = arena_strndup(ps->a, name, strlen(name));
    vals[n] = v;
    n++;
    if (accept_tok(ps, T_COMMA)) continue;
    break;
  }
  st->set_cols = cols;
  st->set_colnames = colnames;
  st->set_vals = vals;
  st->nset = n;
  if (ps->cur.kind == T_WHERE) {
    next_token(ps);
    st->where = parse_cond(ps);
    if (!st->where) return false;
  }
  return true;
}

static bool parse_delete(parser *ps, stmt *st) {
  st->type = ST_DELETE;
  if (!expect_tok(ps, T_FROM, "'FROM'")) return false;
  if (!copy_ident(ps, st->table, sizeof(st->table))) return false;
  if (ps->cur.kind == T_WHERE) {
    next_token(ps);
    st->where = parse_cond(ps);
    if (!st->where) return false;
  }
  return true;
}

stmt *sql_parse(arena *a, const char *text, size_t len, char **err) {
  parser ps;
  memset(&ps, 0, sizeof(ps));
  ps.a = a;
  ps.p = text;
  ps.end = text + len;
  ps.err[0] = '\0';
  next_token(&ps);

  stmt *st = (stmt *)arena_calloc(a, sizeof(stmt));
  if (!st) {
    if (err) *err = arena_strndup(a, "out of memory", 11);
    return NULL;
  }
  st->pk_idx = -1;
  st->vec_idx = -1;
  st->tab = -1;
  st->topk_item = -1;
  st->order_kind = 0;
  st->order_col = -1;
  st->order_item = -1;

  bool ok = false;
  switch (ps.cur.kind) {
    case T_CREATE:
      next_token(&ps);
      if (ps.cur.kind == T_TABLE) ok = parse_create_table(&ps, st);
      else if (ps.cur.kind == T_VECTOR) {
        next_token(&ps);
        ok = parse_create_index(&ps, st);
      } else ok = false;
      break;
    case T_INSERT:
      next_token(&ps);
      ok = parse_insert(&ps, st);
      break;
    case T_SELECT:
      next_token(&ps);
      ok = parse_select(&ps, st);
      break;
    case T_UPDATE:
      next_token(&ps);
      ok = parse_update(&ps, st);
      break;
    case T_DELETE:
      next_token(&ps);
      ok = parse_delete(&ps, st);
      break;
    case T_BEGIN:
      next_token(&ps);
      st->type = ST_BEGIN;
      ok = true;
      break;
    case T_COMMIT:
      next_token(&ps);
      st->type = ST_COMMIT;
      ok = true;
      break;
    case T_ROLLBACK:
      next_token(&ps);
      st->type = ST_ROLLBACK;
      ok = true;
      break;
    default:
      perr(&ps, "unsupported statement");
      break;
  }
  if (ok) {
    /* trailing semicolons are fine */
    while (ps.cur.kind == T_SEMI) next_token(&ps);
    if (ps.cur.kind != T_EOF) { perr(&ps, "unexpected trailing input"); ok = false; }
  }
  if (!ok) {
    if (err) *err = arena_strndup(a, ps.err[0] ? ps.err : "unsupported statement", strlen(ps.err[0] ? ps.err : "unsupported statement"));
    return NULL;
  }
  return st;
}
