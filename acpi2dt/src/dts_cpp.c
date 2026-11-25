/* SPDX-License-Identifier: MIT */
/**
 * @file   dts_cpp.c
 * @author Ingo A. Kubbilun
 * @brief  minimal C preprocessor for device tree sources (see dts_cpp.h)
 *
 * Processing per file:
 *   1. comments are replaced by blanks (newlines are kept, strings respected)
 *   2. physical lines ending with a backslash are joined to logical lines
 *   3. directives (\#include, \#define, \#if, ...) are executed; a '#' line that
 *      is not a known directive (e.g. "\#address-cells = <1>;") is text
 *   4. text lines of active conditional sections are macro-expanded
 *
 * Line markers ("# \<line\> "\<file\>"") keep dtc's error positions correct.
 *
 * [MIT license] - see main.c
 */

#include "dts_cpp.h"

#define CPP_MAX_DEPTH   48     /**< maximum include nesting            */
#define CPP_MAX_EXPAND  64     /**< maximum macro expansion nesting    */
#define CPP_MAX_COND    128    /**< maximum \#if nesting per file       */

/** one macro definition */
typedef struct {
  bool   func;        /**< function-like macro                         */
  bool   variadic;    /**< last parameter is "..." (__VA_ARGS__)       */
  int    nparams;     /**< number of named parameters                  */
  char **params;      /**< parameter names                             */
  char  *body;        /**< replacement text                            */
  bool   busy;        /**< currently being expanded (no recursion)     */
} macro_t;

/** preprocessor state */
typedef struct {
  strmap_t     macros;  /**< name -> macro_t*                          */
  const char **dirs;    /**< include search directories                */
  int          ndirs;  /**< number of include directories */
  strbuf_t    *out;     /**< output text                               */
  strlist_t   *errors;  /**< error messages                            */
  bool         ok;      /**< false after the first error               */
} cpp_t;

/** one conditional (\#if) level */
typedef struct {
  bool parent_active;   /**< enclosing section is active               */
  bool taken;           /**< a branch of this \#if was already active   */
  bool active;          /**< current branch is active                  */
} cond_t;

static void expand(cpp_t *c, const char *in, strbuf_t *out, int depth);

/* ======================================================================== */
/* helpers                                                                  */
/* ======================================================================== */

/**
 * @brief Checks for a valid first character of a C identifier.
 *
 * @param ch  character
 * @return true for letters and '_'
 */
static bool is_id_start(int ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_'; }
/**
 * @brief Checks for a valid character of a C identifier.
 *
 * @param ch  character
 * @return true for letters, digits and '_'
 */
static bool is_id_char(int ch)  { return is_id_start(ch) || (ch >= '0' && ch <= '9'); }

/**
 * @brief Records a preprocessor error "file:line: message".
 *
 * @param c     preprocessor state
 * @param file  file name
 * @param line  line number
 * @param fmt   printf format
 * @param ...   format arguments
 */
static void cpp_error(cpp_t *c, const char *file, int line, const char *fmt, ...)
{
  char    msg[512], full[768];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  snprintf(full, sizeof(full), "%s:%d: %s", file, line, msg);
  sl_add(c->errors, full);
  c->ok = false;
}

/**
 * @brief Replaces comments by blanks, keeping newlines and string literals.
 *
 * @param s  text (modified)
 */
static void strip_comments(char *s)
{
  enum { NORMAL, STR, CHR, LINE, BLOCK } st = NORMAL;
  for (char *p = s; *p; p++)
  {
    switch (st)
    {
      case NORMAL:
        if (p[0] == '/' && p[1] == '/')      { st = LINE; p[0] = p[1] = ' '; p++; }
        else if (p[0] == '/' && p[1] == '*') { st = BLOCK; p[0] = p[1] = ' '; p++; }
        else if (*p == '"')                  st = STR;
        else if (*p == '\'')                 st = CHR;
        break;
      case STR:
        if (*p == '\\' && p[1] && p[1] != '\n') p++;
        else if (*p == '"' || *p == '\n')     st = NORMAL;
        break;
      case CHR:
        if (*p == '\\' && p[1] && p[1] != '\n') p++;
        else if (*p == '\'' || *p == '\n')    st = NORMAL;
        break;
      case LINE:
        if (*p == '\n') st = NORMAL;
        else            *p = ' ';
        break;
      case BLOCK:
        if (p[0] == '*' && p[1] == '/') { p[0] = p[1] = ' '; p++; st = NORMAL; }
        else if (*p != '\n')            *p = ' ';
        break;
    }
  }
}

/**
 * @brief Skips blanks and tabs (not newlines).
 *
 * @param p  text position
 * @return first non-blank position
 */
static const char *skip_ws(const char *p)
{
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\f' || *p == '\v')
    p++;
  return p;
}

/**
 * @brief Reads a C identifier.
 *
 * @param p    text position
 * @param out  output buffer
 * @param cap  size of the output buffer
 * @return position behind the identifier
 */
static const char *read_ident(const char *p, char *out, size_t cap)
{
  size_t n = 0;
  while (is_id_char((unsigned char)*p))
  {
    if (n < cap - 1)
      out[n++] = *p;
    p++;
  }
  out[n] = 0;
  return p;
}

/**
 * @brief Copies a string or character literal including its quotes.
 *
 * @param p    position of the opening quote
 * @param out  output buffer
 * @return position behind the literal
 */
static const char *copy_literal(const char *p, strbuf_t *out)
{
  char q = *p;
  sb_putc(out, *p++);
  while (*p && *p != q && *p != '\n')
  {
    if (*p == '\\' && p[1])
      sb_putc(out, *p++);
    sb_putc(out, *p++);
  }
  if (*p == q)
    sb_putc(out, *p++);
  return p;
}

/**
 * @brief Frees a macro definition.
 *
 * @param m  macro (may be NULL)
 */
static void free_macro(macro_t *m)
{
  if (!m)
    return;
  for (int i = 0; i < m->nparams; i++)
    free(m->params[i]);
  free(m->params);
  free(m->body);
  free(m);
}

/* ======================================================================== */
/* #define / #undef                                                         */
/* ======================================================================== */

/**
 * @brief Handles \#define (object-like and function-like, variadic).
 *
 * @param c     preprocessor state
 * @param def   text behind "\#define"
 * @param file  file name (for errors)
 * @param line  line number (for errors)
 */
static void define_macro(cpp_t *c, const char *def, const char *file, int line)
{
  char        name[128];
  const char *p = read_ident(skip_ws(def), name, sizeof(name));
  macro_t    *m;

  if (!name[0])
  {
    cpp_error(c, file, line, "#define without macro name");
    return;
  }
  m = xcalloc(1, sizeof(*m));
  if (*p == '(') /* function-like only if '(' directly follows the name */
  {
    m->func = true;
    p++;
    for (;;)
    {
      char pname[128];
      p = skip_ws(p);
      if (*p == ')')
      {
        p++;
        break;
      }
      if (p[0] == '.' && p[1] == '.' && p[2] == '.')
      {
        m->variadic = true;
        p = skip_ws(p + 3);
        if (*p == ')')
          p++;
        break;
      }
      p = read_ident(p, pname, sizeof(pname));
      if (!pname[0])
      {
        cpp_error(c, file, line, "bad parameter list of macro %s", name);
        free_macro(m);
        return;
      }
      m->params = xrealloc(m->params, sizeof(char *) * (size_t)(m->nparams + 1));
      m->params[m->nparams++] = xstrdup(pname);
      p = skip_ws(p);
      if (p[0] == '.' && p[1] == '.' && p[2] == '.') /* GNU named variadic */
      {
        m->variadic = true;
        p = skip_ws(p + 3);
      }
      if (*p == ',')
        p++;
    }
  }
  {
    char *body = xstrdup(skip_ws(p));
    m->body = xstrdup(str_trim(body));
    free(body);
  }
  free_macro(sm_get(&c->macros, name));
  sm_put(&c->macros, name, m);
}

/**
 * @brief Handles \#undef.
 *
 * @param c     preprocessor state
 * @param rest  text behind "\#undef"
 */
static void undef_macro(cpp_t *c, const char *rest)
{
  char name[128];
  read_ident(skip_ws(rest), name, sizeof(name));
  if (name[0] && sm_get(&c->macros, name))
  {
    free_macro(sm_get(&c->macros, name));
    sm_put(&c->macros, name, NULL);
  }
}

/* ======================================================================== */
/* macro expansion                                                          */
/* ======================================================================== */

/**
 * @brief Splits the arguments of a function-like macro call.
 *
 * @param p     position of '('
 * @param args  receives the raw arguments
 * @return position behind ')', NULL if unterminated
 */
static const char *collect_args(const char *p, strlist_t *args)
{
  int      level = 0;
  strbuf_t cur;

  sb_init(&cur);
  p++; /* '(' */
  while (*p)
  {
    if (*p == '"' || *p == '\'')
    {
      p = copy_literal(p, &cur);
      continue;
    }
    if (*p == '(')
      level++;
    else if (*p == ')')
    {
      if (level == 0)
      {
        char *a = xstrdup(cur.buf ? cur.buf : "");
        sl_add(args, str_trim(a));
        free(a);
        sb_free(&cur);
        return p + 1;
      }
      level--;
    }
    else if (*p == ',' && level == 0)
    {
      char *a = xstrdup(cur.buf ? cur.buf : "");
      sl_add(args, str_trim(a));
      free(a);
      sb_clear(&cur);
      p++;
      continue;
    }
    sb_putc(&cur, *p++);
  }
  sb_free(&cur);
  return NULL; /* unterminated */
}

/**
 * @brief Returns the raw argument for a parameter name (or __VA_ARGS__).
 *
 * @param m      macro
 * @param args   raw arguments of the call
 * @param ident  identifier from the macro body
 * @param raw    receives the argument text
 * @return true if ident is a parameter
 */
static bool param_arg(const macro_t *m, const strlist_t *args, const char *ident, strbuf_t *raw)
{
  for (int i = 0; i < m->nparams; i++)
    if (!strcmp(m->params[i], ident))
    {
      sb_puts(raw, i < args->count ? args->v[i] : "");
      return true;
    }
  if (m->variadic && !strcmp(ident, "__VA_ARGS__"))
  {
    for (int i = m->nparams; i < args->count; i++)
    {
      if (i > m->nparams)
        sb_puts(raw, ", ");
      sb_puts(raw, args->v[i]);
    }
    return true;
  }
  return false;
}

/**
 * @brief Substitutes the arguments into the body of a function-like macro (#, ## and argument pre-expansion).
 *
 * @param c      preprocessor state
 * @param m      macro
 * @param args   raw arguments
 * @param res    receives the substituted body
 * @param depth  expansion depth
 */
static void substitute(cpp_t *c, const macro_t *m, const strlist_t *args, strbuf_t *res, int depth)
{
  const char *p = m->body;
  bool        paste_next = false;

  while (*p)
  {
    if (p[0] == '#' && p[1] == '#')
    {
      while (res->len && (res->buf[res->len - 1] == ' ' || res->buf[res->len - 1] == '\t'))
        res->buf[--res->len] = 0;
      p = skip_ws(p + 2);
      paste_next = true;
      continue;
    }
    if (*p == '#')
    {
      char        id[128];
      const char *q = read_ident(skip_ws(p + 1), id, sizeof(id));
      strbuf_t    raw;
      sb_init(&raw);
      if (id[0] && param_arg(m, args, id, &raw))
      {
        sb_putc(res, '"');
        for (const char *s = raw.buf ? raw.buf : ""; *s; s++)
        {
          if (*s == '"' || *s == '\\')
            sb_putc(res, '\\');
          sb_putc(res, *s);
        }
        sb_putc(res, '"');
        sb_free(&raw);
        p = q;
        paste_next = false;
        continue;
      }
      sb_free(&raw);
    }
    if (*p == '"' || *p == '\'')
    {
      p = copy_literal(p, res);
      paste_next = false;
      continue;
    }
    if (is_id_start((unsigned char)*p))
    {
      char        id[128];
      const char *q = read_ident(p, id, sizeof(id));
      strbuf_t    raw;
      sb_init(&raw);
      if (param_arg(m, args, id, &raw))
      {
        const char *after = skip_ws(q);
        bool        raw_use = paste_next || (after[0] == '#' && after[1] == '#');
        if (raw_use)
          sb_puts(res, raw.buf ? raw.buf : "");
        else
          expand(c, raw.buf ? raw.buf : "", res, depth + 1);
      }
      else
        sb_puts(res, id);
      sb_free(&raw);
      p = q;
      paste_next = false;
      continue;
    }
    if (*p != ' ' && *p != '\t')
      paste_next = false;
    sb_putc(res, *p++);
  }
}

/**
 * @brief Macro-expands a text (string literals and numbers are copied unchanged).
 *
 * @param c      preprocessor state
 * @param in     input text
 * @param out    output buffer
 * @param depth  expansion depth
 */
static void expand(cpp_t *c, const char *in, strbuf_t *out, int depth)
{
  const char *p = in;

  if (depth > CPP_MAX_EXPAND)
  {
    sb_puts(out, in);
    return;
  }
  while (*p)
  {
    if (*p == '"' || *p == '\'')
    {
      p = copy_literal(p, out);
      continue;
    }
    if (*p >= '0' && *p <= '9') /* pp-number: never a macro name */
    {
      while (is_id_char((unsigned char)*p) || *p == '.')
        sb_putc(out, *p++);
      continue;
    }
    if (is_id_start((unsigned char)*p))
    {
      char        id[128];
      const char *q = read_ident(p, id, sizeof(id));
      macro_t    *m = sm_get(&c->macros, id);

      if (!m || m->busy)
      {
        sb_puts(out, id);
        p = q;
        continue;
      }
      if (!m->func)
      {
        m->busy = true;
        expand(c, m->body, out, depth + 1);
        m->busy = false;
        p = q;
        continue;
      }
      {
        const char *paren = skip_ws(q);
        strlist_t   args;
        strbuf_t    res;
        const char *end;

        if (*paren != '(')
        {
          sb_puts(out, id); /* function-like macro name without arguments */
          p = q;
          continue;
        }
        memset(&args, 0, sizeof(args));
        end = collect_args(paren, &args);
        if (!end)
        {
          sb_puts(out, id);
          p = q;
          sl_free(&args);
          continue;
        }
        sb_init(&res);
        substitute(c, m, &args, &res, depth);
        m->busy = true;
        expand(c, res.buf ? res.buf : "", out, depth + 1);
        m->busy = false;
        sb_free(&res);
        sl_free(&args);
        p = end;
        continue;
      }
    }
    sb_putc(out, *p++);
  }
}

/* ======================================================================== */
/* #if expressions                                                          */
/* ======================================================================== */

/** @brief state of the \#if expression parser */
typedef struct {
  const char *p;  /**< current position */
  bool        err;  /**< syntax error found */
} expr_t;

static long long ex_cond(expr_t *e);

/**
 * @brief Skips blanks in a \#if expression.
 *
 * @param e  expression state
 */
static void ex_ws(expr_t *e) { e->p = skip_ws(e->p); }

/**
 * @brief Parses a primary \#if expression: number, character, identifier (0) or parenthesized expression.
 *
 * @param e  expression state
 * @return value
 */
static long long ex_primary(expr_t *e)
{
  ex_ws(e);
  if (*e->p == '(')
  {
    long long v;
    e->p++;
    v = ex_cond(e);
    ex_ws(e);
    if (*e->p == ')')
      e->p++;
    else
      e->err = true;
    return v;
  }
  if (*e->p == '\'')
  {
    long long v = (unsigned char)e->p[1];
    if (e->p[1] == '\\')
    {
      v = e->p[2] == 'n' ? '\n' : e->p[2] == 't' ? '\t' : e->p[2] == '0' ? 0 : (unsigned char)e->p[2];
      e->p++;
    }
    e->p += 2;
    if (*e->p == '\'')
      e->p++;
    return v;
  }
  if (*e->p >= '0' && *e->p <= '9')
  {
    char     *end;
    long long v = (long long)strtoull(e->p, &end, 0);
    e->p = end;
    while (*e->p == 'u' || *e->p == 'U' || *e->p == 'l' || *e->p == 'L')
      e->p++;
    return v;
  }
  if (is_id_start((unsigned char)*e->p))
  {
    while (is_id_char((unsigned char)*e->p))
      e->p++;
    return 0; /* undefined identifiers evaluate to 0 */
  }
  e->err = true;
  return 0;
}

/**
 * @brief Parses unary operators ! ~ - +.
 *
 * @param e  expression state
 * @return value
 */
static long long ex_unary(expr_t *e)
{
  ex_ws(e);
  if (*e->p == '!') { e->p++; return !ex_unary(e); }
  if (*e->p == '~') { e->p++; return ~ex_unary(e); }
  if (*e->p == '-') { e->p++; return -ex_unary(e); }
  if (*e->p == '+') { e->p++; return ex_unary(e); }
  return ex_primary(e);
}

/**
 * @brief Parses * / %.
 *
 * @param e  expression state
 * @return value
 */
static long long ex_mul(expr_t *e)
{
  long long v = ex_unary(e);
  for (;;)
  {
    ex_ws(e);
    if (*e->p == '*')      { e->p++; v *= ex_unary(e); }
    else if (*e->p == '/') { long long r; e->p++; r = ex_unary(e); v = r ? v / r : 0; }
    else if (*e->p == '%') { long long r; e->p++; r = ex_unary(e); v = r ? v % r : 0; }
    else return v;
  }
}

/**
 * @brief Parses + -.
 *
 * @param e  expression state
 * @return value
 */
static long long ex_add(expr_t *e)
{
  long long v = ex_mul(e);
  for (;;)
  {
    ex_ws(e);
    if (*e->p == '+')      { e->p++; v += ex_mul(e); }
    else if (*e->p == '-') { e->p++; v -= ex_mul(e); }
    else return v;
  }
}

/**
 * @brief Parses << >>.
 *
 * @param e  expression state
 * @return value
 */
static long long ex_shift(expr_t *e)
{
  long long v = ex_add(e);
  for (;;)
  {
    ex_ws(e);
    if (!strncmp(e->p, "<<", 2))      { e->p += 2; v <<= ex_add(e); }
    else if (!strncmp(e->p, ">>", 2)) { e->p += 2; v >>= ex_add(e); }
    else return v;
  }
}

/**
 * @brief Parses < <= > >=.
 *
 * @param e  expression state
 * @return value
 */
static long long ex_rel(expr_t *e)
{
  long long v = ex_shift(e);
  for (;;)
  {
    ex_ws(e);
    if (!strncmp(e->p, "<=", 2))                       { e->p += 2; v = v <= ex_shift(e); }
    else if (!strncmp(e->p, ">=", 2))                  { e->p += 2; v = v >= ex_shift(e); }
    else if (*e->p == '<' && e->p[1] != '<')           { e->p++; v = v < ex_shift(e); }
    else if (*e->p == '>' && e->p[1] != '>')           { e->p++; v = v > ex_shift(e); }
    else return v;
  }
}

/**
 * @brief Parses == !=.
 *
 * @param e  expression state
 * @return value
 */
static long long ex_eq(expr_t *e)
{
  long long v = ex_rel(e);
  for (;;)
  {
    ex_ws(e);
    if (!strncmp(e->p, "==", 2))      { e->p += 2; v = v == ex_rel(e); }
    else if (!strncmp(e->p, "!=", 2)) { e->p += 2; v = v != ex_rel(e); }
    else return v;
  }
}

/**
 * @brief Parses bitwise &.
 *
 * @param e  expression state
 * @return value
 */
static long long ex_band(expr_t *e)
{
  long long v = ex_eq(e);
  for (;;)
  {
    ex_ws(e);
    if (*e->p == '&' && e->p[1] != '&') { e->p++; v &= ex_eq(e); }
    else return v;
  }
}

/**
 * @brief Parses ^.
 *
 * @param e  expression state
 * @return value
 */
static long long ex_xor(expr_t *e)
{
  long long v = ex_band(e);
  for (;;)
  {
    ex_ws(e);
    if (*e->p == '^') { e->p++; v ^= ex_band(e); }
    else return v;
  }
}

/**
 * @brief Parses bitwise |.
 *
 * @param e  expression state
 * @return value
 */
static long long ex_bor(expr_t *e)
{
  long long v = ex_xor(e);
  for (;;)
  {
    ex_ws(e);
    if (*e->p == '|' && e->p[1] != '|') { e->p++; v |= ex_xor(e); }
    else return v;
  }
}

/**
 * @brief Parses &&.
 *
 * @param e  expression state
 * @return value
 */
static long long ex_land(expr_t *e)
{
  long long v = ex_bor(e);
  for (;;)
  {
    ex_ws(e);
    if (!strncmp(e->p, "&&", 2)) { long long r; e->p += 2; r = ex_bor(e); v = v && r; }
    else return v;
  }
}

/**
 * @brief Parses ||.
 *
 * @param e  expression state
 * @return value
 */
static long long ex_lor(expr_t *e)
{
  long long v = ex_land(e);
  for (;;)
  {
    ex_ws(e);
    if (!strncmp(e->p, "||", 2)) { long long r; e->p += 2; r = ex_land(e); v = v || r; }
    else return v;
  }
}

/**
 * @brief Parses a complete \#if expression including ?:.
 *
 * @param e  expression state
 * @return value
 */
static long long ex_cond(expr_t *e)
{
  long long v = ex_lor(e);
  ex_ws(e);
  if (*e->p == '?')
  {
    long long a, b;
    e->p++;
    a = ex_cond(e);
    ex_ws(e);
    if (*e->p == ':')
      e->p++;
    else
      e->err = true;
    b = ex_cond(e);
    return v ? a : b;
  }
  return v;
}

/**
 * @brief Evaluates a \#if / \#elif expression (defined(), macro expansion, integer arithmetic).
 *
 * @param c     preprocessor state
 * @param expr  expression text
 * @param file  file name (for errors)
 * @param line  line number (for errors)
 * @return true if the expression is non-zero
 */
static bool eval_if(cpp_t *c, const char *expr, const char *file, int line)
{
  strbuf_t    pre, ex;
  const char *p = expr;
  expr_t      e;
  long long   v;

  /* 1. defined X / defined(X) */
  sb_init(&pre);
  while (*p)
  {
    if (is_id_start((unsigned char)*p))
    {
      char id[128];
      p = read_ident(p, id, sizeof(id));
      if (!strcmp(id, "defined"))
      {
        bool paren;
        p = skip_ws(p);
        paren = *p == '(';
        if (paren)
          p = skip_ws(p + 1);
        p = read_ident(p, id, sizeof(id));
        if (paren)
        {
          p = skip_ws(p);
          if (*p == ')')
            p++;
        }
        sb_puts(&pre, sm_get(&c->macros, id) ? " 1 " : " 0 ");
      }
      else
        sb_puts(&pre, id);
      continue;
    }
    sb_putc(&pre, *p++);
  }
  /* 2. macro expansion, 3. evaluation */
  sb_init(&ex);
  expand(c, pre.buf ? pre.buf : "", &ex, 0);
  e.p = ex.buf ? ex.buf : "0";
  e.err = false;
  v = ex_cond(&e);
  ex_ws(&e);
  if (e.err || *e.p)
    cpp_error(c, file, line, "cannot evaluate #if expression \"%s\"", expr);
  sb_free(&pre);
  sb_free(&ex);
  return v != 0;
}

/* ======================================================================== */
/* files                                                                    */
/* ======================================================================== */

/**
 * @brief Writes a cpp line marker (# line "file").
 *
 * @param c     preprocessor state
 * @param file  file name
 * @param line  line number
 */
static void line_marker(cpp_t *c, const char *file, int line)
{
  sb_printf(c->out, "# %d \"", line);
  for (const char *p = file; *p; p++)
    sb_putc(c->out, *p == '\\' ? '/' : *p);
  sb_puts(c->out, "\"\n");
}

/**
 * @brief Locates and reads an include file, applying fallbacks for unreadable symlinks.
 *
 * @param c         preprocessor state
 * @param name      file name from the directive
 * @param from_dir  directory of the including file
 * @param quoted    true for "..." includes
 * @param found     receives the path found
 * @param cap       size of found
 * @return file contents (caller frees) or NULL
 */
static char *read_include(cpp_t *c, const char *name, const char *from_dir, bool quoted, char *found, size_t cap)
{
  static const char *fallbacks[][2] = {
    { "linux-event-codes.h", "uapi/linux/input-event-codes.h" },
  };
  for (int pass = 0; pass < 2; pass++)
  {
    const char *want = name;
    if (pass == 1)
    {
      want = NULL;
      for (size_t f = 0; f < sizeof(fallbacks) / sizeof(fallbacks[0]); f++)
        if (!strcmp(path_basename(name), fallbacks[f][0]))
          want = fallbacks[f][1];
      if (!want)
        break;
    }
    for (int i = (quoted && pass == 0) ? -1 : 0; i < c->ndirs; i++)
    {
      size_t len;
      char  *txt;
      path_join(found, cap, i < 0 ? from_dir : c->dirs[i], want);
      path_normalize_slashes(found);
      txt = (char *)read_file(found, &len);
      if (txt && len > 0)
        return txt;
      free(txt);
    }
  }
  return NULL;
}

/**
 * @brief Preprocesses one file (recursive for includes).
 *
 * @param c        preprocessor state
 * @param path     file path (for markers and includes)
 * @param text_in  file contents
 * @param depth    include nesting depth
 */
static void process_file(cpp_t *c, const char *path, const char *text_in, int depth)
{
  char     dir[A2D_MAX_PATH];
  char    *text = xstrdup(text_in);
  char    *p = text;
  int      line = 1;
  int      expected = 1;
  cond_t   cond[CPP_MAX_COND];
  int      ncond = 0;
  strbuf_t logical;

  path_dirname(path, dir, sizeof(dir));
  strip_comments(text);
  sb_init(&logical);
  line_marker(c, path, 1);

  while (*p)
  {
    int         start_line = line;
    const char *s;
    bool        active = ncond == 0 || cond[ncond - 1].active;

    /* logical line: join backslash-newline continuations */
    sb_clear(&logical);
    for (;;)
    {
      char  *nl = strchr(p, '\n');
      size_t n = nl ? (size_t)(nl - p) : strlen(p);
      size_t m = n;
      while (m && p[m - 1] == '\r')
        m--;
      if (m && p[m - 1] == '\\')
      {
        sb_putn(&logical, p, m - 1);
        p = nl ? nl + 1 : p + n;
        line++;
        if (!nl)
          break;
        continue;
      }
      sb_putn(&logical, p, m);
      p = nl ? nl + 1 : p + n;
      line++;
      break;
    }
    s = skip_ws(logical.buf ? logical.buf : "");

    if (*s == '#')
    {
      char        word[32];
      const char *rest = read_ident(skip_ws(s + 1), word, sizeof(word));
      bool        known = true;

      if (!strcmp(word, "ifdef") || !strcmp(word, "ifndef"))
      {
        char id[128];
        bool def;
        read_ident(skip_ws(rest), id, sizeof(id));
        def = sm_get(&c->macros, id) != NULL;
        if (ncond < CPP_MAX_COND)
        {
          cond_t *k = &cond[ncond++];
          k->parent_active = active;
          k->active = active && (word[2] == 'd' ? def : !def);
          k->taken = k->active;
        }
      }
      else if (!strcmp(word, "if"))
      {
        if (ncond < CPP_MAX_COND)
        {
          cond_t *k = &cond[ncond++];
          k->parent_active = active;
          k->active = active && eval_if(c, rest, path, start_line);
          k->taken = k->active;
        }
      }
      else if (!strcmp(word, "elif"))
      {
        if (!ncond)
          cpp_error(c, path, start_line, "#elif without #if");
        else
        {
          cond_t *k = &cond[ncond - 1];
          if (k->taken || !k->parent_active)
            k->active = false;
          else
            k->taken = k->active = eval_if(c, rest, path, start_line);
        }
      }
      else if (!strcmp(word, "else"))
      {
        if (!ncond)
          cpp_error(c, path, start_line, "#else without #if");
        else
        {
          cond_t *k = &cond[ncond - 1];
          k->active = k->parent_active && !k->taken;
          k->taken = true;
        }
      }
      else if (!strcmp(word, "endif"))
      {
        if (!ncond)
          cpp_error(c, path, start_line, "#endif without #if");
        else
          ncond--;
      }
      else if (!active)
        known = strcmp(word, "include") == 0 || strcmp(word, "define") == 0 || strcmp(word, "undef") == 0 ||
                strcmp(word, "error") == 0 || strcmp(word, "warning") == 0 || strcmp(word, "pragma") == 0 ||
                strcmp(word, "line") == 0 || word[0] == 0;
      else if (!strcmp(word, "define"))
        define_macro(c, rest, path, start_line);
      else if (!strcmp(word, "undef"))
        undef_macro(c, rest);
      else if (!strcmp(word, "include"))
      {
        const char *q = skip_ws(rest);
        strbuf_t    ex;
        sb_init(&ex);
        if (*q != '"' && *q != '<')
        {
          expand(c, q, &ex, 0); /* computed include */
          q = skip_ws(ex.buf ? ex.buf : "");
        }
        if (*q == '"' || *q == '<')
        {
          char        close = *q == '"' ? '"' : '>';
          const char *e = strchr(q + 1, close);
          if (!e)
            cpp_error(c, path, start_line, "malformed #include");
          else if (depth >= CPP_MAX_DEPTH)
            cpp_error(c, path, start_line, "#include nested too deeply");
          else
          {
            char  name[A2D_MAX_PATH], found[A2D_MAX_PATH];
            char *txt;
            size_t n = (size_t)(e - q - 1);
            if (n >= sizeof(name))
              n = sizeof(name) - 1;
            memcpy(name, q + 1, n);
            name[n] = 0;
            txt = read_include(c, name, dir, close == '"', found, sizeof(found));
            if (!txt)
              cpp_error(c, path, start_line, "include file \"%s\" not found", name);
            else
            {
              process_file(c, found, txt, depth + 1);
              free(txt);
            }
          }
        }
        else
          cpp_error(c, path, start_line, "malformed #include");
        sb_free(&ex);
        expected = -1; /* force a line marker */
      }
      else if (!strcmp(word, "error"))
        cpp_error(c, path, start_line, "#error %s", skip_ws(rest));
      else if (!strcmp(word, "warning") || !strcmp(word, "pragma") || !strcmp(word, "line"))
        ;
      else if (!word[0] && (*skip_ws(s + 1) >= '0' && *skip_ws(s + 1) <= '9'))
        ; /* line marker in the input: ignore */
      else
        known = false; /* e.g. "#address-cells = <1>;" */

      if (known)
        continue;
    }

    if (!active)
      continue;
    if (start_line != expected)
      line_marker(c, path, start_line);
    expand(c, logical.buf ? logical.buf : "", c->out, 0);
    sb_putc(c->out, '\n');
    /* next line number dtc expects; joined continuation lines need a marker */
    expected = (line - start_line > 1) ? -1 : line;
  }
  if (ncond)
    cpp_error(c, path, line, "unterminated #if");
  sb_free(&logical);
  free(text);
}

bool dts_preprocess(const char *in_path, const char **inc_dirs, int ninc, strbuf_t *out, strlist_t *errors)
{
  cpp_t  c;
  size_t len;
  char  *txt = (char *)read_file(in_path, &len);

  memset(&c, 0, sizeof(c));
  c.dirs = inc_dirs;
  c.ndirs = ninc;
  c.out = out;
  c.errors = errors;
  c.ok = true;
  if (!txt)
  {
    char msg[A2D_MAX_PATH + 32];
    snprintf(msg, sizeof(msg), "cannot read %s", in_path);
    sl_add(errors, msg);
    return false;
  }
  define_macro(&c, "__DTS__ 1", in_path, 0);
  process_file(&c, in_path, txt, 0);
  free(txt);
  for (size_t i = 0; i < c.macros.cap; i++)
    if (c.macros.keys[i])
      free_macro(c.macros.vals[i]);
  sm_free(&c.macros);
  return c.ok;
}
