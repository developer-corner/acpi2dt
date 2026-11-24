/**
 * @file   dts_parser.c
 * @author Ingo A. Kubbilun
 * @brief  DTS/DTSI parser preserving comments, blank lines, labels and raw
 *         property values; merged tree builder (includes + overrides)
 *
 * The parser does not run the C preprocessor: macros inside property values
 * stay untouched (raw text), #include "..." of .dtsi files is followed by the
 * tree builder, #include <dt-bindings/...> is ignored.
 *
 * [MIT license] - see main.c
 */

#include "dts_parser.h"

/* ======================================================================== */
/* lexer                                                                    */
/* ======================================================================== */

typedef struct {
  const char *s;
  size_t      pos;
  size_t      len;
  int         line;
  const char *path;
  int         errors;
} lex_t;

static int lpeek(lex_t *lx, size_t o) { return lx->pos + o < lx->len ? (unsigned char)lx->s[lx->pos + o] : 0; }

static void ladv(lex_t *lx, size_t n)
{
  while (n-- && lx->pos < lx->len)
  {
    if (lx->s[lx->pos] == '\n')
      lx->line++;
    lx->pos++;
  }
}

static bool at_line_start(lex_t *lx)
{
  size_t p = lx->pos;
  while (p > 0 && (lx->s[p - 1] == ' ' || lx->s[p - 1] == '\t'))
    p--;
  return p == 0 || lx->s[p - 1] == '\n';
}

static bool is_preproc(lex_t *lx)
{
  static const char *kw[] = { "include", "define", "if", "ifdef", "ifndef", "else", "elif", "endif", "undef",
                              "error", "warning", "pragma" };
  if (lpeek(lx, 0) != '#' || !at_line_start(lx))
    return false;
  for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++)
  {
    size_t n = strlen(kw[i]);
    if (lx->pos + 1 + n <= lx->len && 0 == strncmp(lx->s + lx->pos + 1, kw[i], n))
    {
      int c = lpeek(lx, 1 + n);
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0 || c == '<' || c == '"' || c == '(')
        return true;
    }
  }
  return false;
}

/* skips whitespace and comments; comments are appended to cm (one per line);
   *blank becomes true if an empty line precedes the next token */
static void skip_ws(lex_t *lx, strbuf_t *cm, bool *blank)
{
  int nl = 0;
  bool seen_comment = false;

  while (lx->pos < lx->len)
  {
    int c = lpeek(lx, 0);
    if (c == '\n')
    {
      nl++;
      if (nl >= 2 && !seen_comment && blank)
        *blank = true;
      ladv(lx, 1);
    }
    else if (c == ' ' || c == '\t' || c == '\r' || c == '\f')
      ladv(lx, 1);
    else if (c == '/' && lpeek(lx, 1) == '/')
    {
      size_t st = lx->pos;
      while (lx->pos < lx->len && lx->s[lx->pos] != '\n')
        lx->pos++;
      if (cm)
      {
        if (cm->len)
          sb_putc(cm, '\n');
        sb_putn(cm, lx->s + st, lx->pos - st);
      }
      seen_comment = true;
      nl = 0;
    }
    else if (c == '/' && lpeek(lx, 1) == '*')
    {
      size_t st = lx->pos;
      ladv(lx, 2);
      while (lx->pos < lx->len && !(lpeek(lx, 0) == '*' && lpeek(lx, 1) == '/'))
        ladv(lx, 1);
      ladv(lx, 2);
      if (cm)
      {
        if (cm->len)
          sb_putc(cm, '\n');
        sb_putn(cm, lx->s + st, lx->pos - st);
      }
      seen_comment = true;
      nl = 0;
    }
    else
      break;
  }
}

static bool is_name_ch(int c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ',' || c == '.' ||
         c == '_' || c == '+' || c == '*' || c == '#' || c == '?' || c == '@' || c == '-';
}

static char *read_name(lex_t *lx)
{
  size_t st = lx->pos;
  while (lx->pos < lx->len && is_name_ch(lpeek(lx, 0)))
    lx->pos++;
  if (lx->pos == st)
    return NULL;
  return xstrndup(lx->s + st, lx->pos - st);
}

/* reads "&label" or "&{/path}" */
static char *read_ref(lex_t *lx)
{
  size_t st = lx->pos;
  if (lpeek(lx, 0) != '&')
    return NULL;
  lx->pos++;
  if (lpeek(lx, 0) == '{')
  {
    while (lx->pos < lx->len && lpeek(lx, 0) != '}')
      ladv(lx, 1);
    lx->pos++;
  }
  else
    while (lx->pos < lx->len && is_name_ch(lpeek(lx, 0)))
      lx->pos++;
  return xstrndup(lx->s + st, lx->pos - st);
}

/* raw text up to (excluding) ';' at top nesting level; consumes ';' */
static char *read_value(lex_t *lx)
{
  size_t st = lx->pos, en;
  while (lx->pos < lx->len)
  {
    int c = lpeek(lx, 0);
    if (c == '"')
    {
      ladv(lx, 1);
      while (lx->pos < lx->len && lpeek(lx, 0) != '"')
      {
        if (lpeek(lx, 0) == '\\')
          ladv(lx, 1);
        ladv(lx, 1);
      }
      ladv(lx, 1);
    }
    else if (c == '/' && lpeek(lx, 1) == '*')
    {
      ladv(lx, 2);
      while (lx->pos < lx->len && !(lpeek(lx, 0) == '*' && lpeek(lx, 1) == '/'))
        ladv(lx, 1);
      ladv(lx, 2);
    }
    else if (c == '/' && lpeek(lx, 1) == '/')
    {
      while (lx->pos < lx->len && lpeek(lx, 0) != '\n')
        ladv(lx, 1);
    }
    else if (c == ';' || c == '}')
      break;
    else
      ladv(lx, 1);
  }
  en = lx->pos;
  if (lpeek(lx, 0) == ';')
    ladv(lx, 1);
  {
    char *v = xstrndup(lx->s + st, en - st);
    char *t = str_trim(v);
    char *r = xstrdup(t);
    free(v);
    return r;
  }
}

static char *read_line_raw(lex_t *lx)
{
  size_t st = lx->pos;
  while (lx->pos < lx->len)
  {
    if (lpeek(lx, 0) == '\\' && (lpeek(lx, 1) == '\n' || (lpeek(lx, 1) == '\r' && lpeek(lx, 2) == '\n')))
    {
      ladv(lx, lpeek(lx, 1) == '\r' ? 3 : 2);
      continue;
    }
    if (lpeek(lx, 0) == '\n')
      break;
    ladv(lx, 1);
  }
  {
    char *v = xstrndup(lx->s + st, lx->pos - st);
    size_t n = strlen(v);
    while (n && (v[n - 1] == '\r' || v[n - 1] == ' '))
      v[--n] = 0;
    return v;
  }
}

static bool lmatch(lex_t *lx, const char *kw)
{
  size_t n = strlen(kw);
  if (lx->pos + n <= lx->len && 0 == strncmp(lx->s + lx->pos, kw, n))
  {
    ladv(lx, n);
    return true;
  }
  return false;
}

static void lerror(lex_t *lx, const char *what)
{
  lx->errors++;
  log_verbose("%s:%d: DTS parse problem: %s", lx->path, lx->line, what);
  /* recover: skip to next ';' */
  while (lx->pos < lx->len && lpeek(lx, 0) != ';' && lpeek(lx, 0) != '}')
    ladv(lx, 1);
  if (lpeek(lx, 0) == ';')
    ladv(lx, 1);
}

/* ======================================================================== */
/* node construction                                                        */
/* ======================================================================== */

dt_node_t *dt_node_new(const char *name)
{
  dt_node_t *n = xcalloc(1, sizeof(*n));
  if (name)
    n->name = xstrdup(name);
  return n;
}

static dt_prop_t *append_prop(dt_node_t *n, dt_prop_t *p)
{
  if (n->props_tail)
    n->props_tail->next = p;
  else
    n->props = p;
  n->props_tail = p;
  return p;
}

dt_prop_t *dt_node_add_prop(dt_node_t *n, const char *name, const char *value, const char *comment)
{
  dt_prop_t *p = xcalloc(1, sizeof(*p));
  p->name = xstrdup(name);
  p->value = value ? xstrdup(value) : NULL;
  p->comment = comment ? xstrdup(comment) : NULL;
  return append_prop(n, p);
}

void dt_node_add_child(dt_node_t *parent, dt_node_t *child)
{
  child->parent = parent;
  child->next = NULL;
  if (parent->child_tail)
    parent->child_tail->next = child;
  else
    parent->children = child;
  parent->child_tail = child;
}

dt_node_t *dt_node_clone(const dt_node_t *n)
{
  dt_node_t *c = dt_node_new(n->name);
  c->ref = n->ref ? xstrdup(n->ref) : NULL;
  for (int i = 0; i < n->labels.count; i++)
    sl_add(&c->labels, n->labels.v[i]);
  c->comment = n->comment ? xstrdup(n->comment) : NULL;
  c->tail_comment = n->tail_comment ? xstrdup(n->tail_comment) : NULL;
  c->blank_before = n->blank_before;
  c->omit_if_no_ref = n->omit_if_no_ref;
  c->file = n->file;
  c->line = n->line;
  for (const dt_prop_t *p = n->props; p; p = p->next)
  {
    dt_prop_t *q = dt_node_add_prop(c, p->name, p->value, p->comment);
    q->blank_before = p->blank_before;
    q->directive = p->directive;
    q->line = p->line;
  }
  for (const dt_node_t *ch = n->children; ch; ch = ch->next)
    dt_node_add_child(c, dt_node_clone(ch));
  return c;
}

/* ======================================================================== */
/* parser                                                                   */
/* ======================================================================== */

static void parse_body(lex_t *lx, dt_node_t *node, int depth)
{
  for (;;)
  {
    strbuf_t   cm;
    bool       blank = false;
    strlist_t  labels;
    char      *name;
    bool       omit = false;

    sb_init(&cm);
    memset(&labels, 0, sizeof(labels));
    skip_ws(lx, &cm, &blank);
    if (lx->pos >= lx->len)
    {
      sb_free(&cm);
      lx->errors++;
      return;
    }
    if (lpeek(lx, 0) == '}')
    {
      ladv(lx, 1);
      skip_ws(lx, NULL, NULL);
      if (lpeek(lx, 0) == ';')
        ladv(lx, 1);
      node->tail_comment = cm.len ? sb_detach(&cm) : NULL;
      sb_free(&cm);
      return;
    }
    if (is_preproc(lx))
    {
      dt_prop_t *p = xcalloc(1, sizeof(*p));
      p->line = lx->line;
      p->name = read_line_raw(lx);
      p->directive = DT_DIR_PREPROC;
      p->comment = cm.len ? sb_detach(&cm) : NULL;
      p->blank_before = blank;
      append_prop(node, p);
      sb_free(&cm);
      continue;
    }
    if (lmatch(lx, "/delete-property/") || lmatch(lx, "/delete-node/"))
    {
      bool       is_prop = lx->s[lx->pos - 2] == 'y';
      dt_prop_t *p = xcalloc(1, sizeof(*p));
      skip_ws(lx, NULL, NULL);
      p->line = lx->line;
      p->name = lpeek(lx, 0) == '&' ? read_ref(lx) : read_name(lx);
      if (!p->name)
        p->name = xstrdup("?");
      p->directive = is_prop ? DT_DIR_DEL_PROP : DT_DIR_DEL_NODE;
      p->comment = cm.len ? sb_detach(&cm) : NULL;
      p->blank_before = blank;
      skip_ws(lx, NULL, NULL);
      if (lpeek(lx, 0) == ';')
        ladv(lx, 1);
      append_prop(node, p);
      sb_free(&cm);
      continue;
    }
    if (lmatch(lx, "/omit-if-no-ref/"))
    {
      omit = true;
      skip_ws(lx, NULL, NULL);
    }

    /* labels and name */
    for (;;)
    {
      size_t save = lx->pos;
      int    sline = lx->line;
      name = read_name(lx);
      if (!name)
        break;
      if (lpeek(lx, 0) == ':' && lpeek(lx, 1) != ':')
      {
        ladv(lx, 1);
        sl_add(&labels, name);
        free(name);
        name = NULL;
        skip_ws(lx, NULL, NULL);
        continue;
      }
      (void)save;
      (void)sline;
      break;
    }
    if (!name)
    {
      sl_free(&labels);
      sb_free(&cm);
      lerror(lx, "name expected");
      continue;
    }
    skip_ws(lx, NULL, NULL);
    if (lpeek(lx, 0) == '{')
    {
      dt_node_t *ch = dt_node_new(NULL);
      ch->name = name;
      ch->labels = labels;
      ch->comment = cm.len ? sb_detach(&cm) : NULL;
      ch->blank_before = blank;
      ch->omit_if_no_ref = omit;
      ch->file = lx->path;
      ch->line = lx->line;
      ladv(lx, 1);
      dt_node_add_child(node, ch);
      if (depth < 64)
        parse_body(lx, ch, depth + 1);
    }
    else
    {
      dt_prop_t *p = xcalloc(1, sizeof(*p));
      p->name = name;
      p->line = lx->line;
      p->comment = cm.len ? sb_detach(&cm) : NULL;
      p->blank_before = blank;
      if (lpeek(lx, 0) == '=')
      {
        ladv(lx, 1);
        skip_ws(lx, NULL, NULL);
        p->value = read_value(lx);
      }
      else if (lpeek(lx, 0) == ';')
        ladv(lx, 1);
      else
        lerror(lx, "';' or '=' expected");
      append_prop(node, p);
      sl_free(&labels);
    }
    sb_free(&cm);
  }
}

static void add_top(dt_file_t *f, dt_top_t *t)
{
  if (f->tail)
    f->tail->next = t;
  else
    f->items = t;
  f->tail = t;
}

static void parse_include_target(dt_top_t *t)
{
  const char *s = t->text;
  const char *a, *b;
  if ((a = strchr(s, '"')) != NULL && (b = strchr(a + 1, '"')) != NULL)
  {
    t->include = xstrndup(a + 1, (size_t)(b - a - 1));
    t->sys_include = false;
  }
  else if ((a = strchr(s, '<')) != NULL && (b = strchr(a + 1, '>')) != NULL)
  {
    t->include = xstrndup(a + 1, (size_t)(b - a - 1));
    t->sys_include = true;
  }
}

dt_file_t *dt_parse_text(const char *path, const char *text)
{
  dt_file_t *f = xcalloc(1, sizeof(*f));
  lex_t      lx;

  f->path = xstrdup(path);
  memset(&lx, 0, sizeof(lx));
  lx.s = text;
  lx.len = strlen(text);
  lx.line = 1;
  lx.path = f->path;

  for (;;)
  {
    strbuf_t  cm;
    bool      blank = false;
    dt_top_t *t;

    sb_init(&cm);
    skip_ws(&lx, &cm, &blank);
    if (lx.pos >= lx.len)
    {
      f->trailing_comment = cm.len ? sb_detach(&cm) : NULL;
      sb_free(&cm);
      break;
    }
    t = xcalloc(1, sizeof(*t));
    t->comment = cm.len ? sb_detach(&cm) : NULL;
    t->blank_before = blank;
    sb_free(&cm);

    if (is_preproc(&lx))
    {
      t->text = read_line_raw(&lx);
      if (str_starts_with(t->text, "#include"))
      {
        t->kind = TOP_INCLUDE;
        parse_include_target(t);
      }
      else
        t->kind = TOP_PREPROC;
      add_top(f, t);
      continue;
    }
    if (lpeek(&lx, 0) == '/' && lpeek(&lx, 1) != ' ' && lpeek(&lx, 1) != '{' && lpeek(&lx, 1) != '\t' &&
        lpeek(&lx, 1) != '\n' && lpeek(&lx, 1) != '\r')
    {
      if (lmatch(&lx, "/include/"))
      {
        char *rest = read_line_raw(&lx);
        t->kind = TOP_INCLUDE;
        t->text = xmalloc(strlen(rest) + 16);
        sprintf(t->text, "/include/%s", rest);
        free(rest);
        parse_include_target(t);
        add_top(f, t);
        continue;
      }
      if (lmatch(&lx, "/delete-node/"))
      {
        skip_ws(&lx, NULL, NULL);
        t->kind = TOP_DELETE_NODE;
        t->ref = read_ref(&lx);
        if (!t->ref)
          t->ref = read_name(&lx);
        skip_ws(&lx, NULL, NULL);
        if (lpeek(&lx, 0) == ';')
          ladv(&lx, 1);
        add_top(f, t);
        continue;
      }
      {
        /* /dts-v1/; /plugin/; /memreserve/ a b; /omit-if-no-ref/ &x; */
        char *v = read_value(&lx);
        t->kind = TOP_DIRECTIVE;
        t->text = xmalloc(strlen(v) + 2);
        sprintf(t->text, "%s;", v);
        free(v);
        add_top(f, t);
        continue;
      }
    }

    /* labels before "/ {" are possible but rare */
    {
      strlist_t labels;
      memset(&labels, 0, sizeof(labels));
      while (is_name_ch(lpeek(&lx, 0)))
      {
        char *l = read_name(&lx);
        if (lpeek(&lx, 0) == ':')
        {
          ladv(&lx, 1);
          sl_add(&labels, l);
        }
        free(l);
        skip_ws(&lx, NULL, NULL);
      }

      if (lpeek(&lx, 0) == '/')
      {
        ladv(&lx, 1);
        skip_ws(&lx, NULL, NULL);
        if (lpeek(&lx, 0) == '{')
        {
          ladv(&lx, 1);
          t->kind = TOP_NODE;
          t->node = dt_node_new("/");
          t->node->labels = labels;
          t->node->file = f->path;
          t->node->line = lx.line;
          parse_body(&lx, t->node, 0);
          add_top(f, t);
          continue;
        }
      }
      else if (lpeek(&lx, 0) == '&')
      {
        char *ref = read_ref(&lx);
        skip_ws(&lx, NULL, NULL);
        if (lpeek(&lx, 0) == '{')
        {
          ladv(&lx, 1);
          t->kind = TOP_NODE;
          t->node = dt_node_new(NULL);
          t->node->ref = ref;
          t->node->labels = labels;
          t->node->file = f->path;
          t->node->line = lx.line;
          parse_body(&lx, t->node, 0);
          add_top(f, t);
          continue;
        }
        free(ref);
      }
      sl_free(&labels);
    }
    free(t->comment);
    free(t);
    lerror(&lx, "unexpected top-level token");
  }
  f->errors = lx.errors;
  return f;
}

dt_file_t *dt_parse_file(const char *path)
{
  size_t     len;
  char      *txt = (char *)read_file(path, &len);
  dt_file_t *f;

  if (!txt)
    return NULL;
  f = dt_parse_text(path, txt);
  free(txt);
  return f;
}

/* ======================================================================== */
/* merged tree                                                              */
/* ======================================================================== */

void dt_tree_init(dt_tree_t *t)
{
  memset(t, 0, sizeof(*t));
  t->root = dt_node_new("/");
}

static void register_labels(dt_tree_t *t, dt_node_t *n)
{
  for (int i = 0; i < n->labels.count; i++)
    sm_put(&t->labels, n->labels.v[i], n);
}

static void unlink_child(dt_node_t *parent, dt_node_t *child)
{
  dt_node_t *prev = NULL;
  for (dt_node_t *c = parent->children; c; prev = c, c = c->next)
  {
    if (c == child)
    {
      if (prev)
        prev->next = c->next;
      else
        parent->children = c->next;
      if (parent->child_tail == c)
        parent->child_tail = prev;
      c->deleted = true;
      return;
    }
  }
}

static void remove_prop(dt_node_t *n, const char *name)
{
  dt_prop_t *prev = NULL;
  for (dt_prop_t *p = n->props; p; prev = p, p = p->next)
  {
    if (!p->directive && 0 == strcmp(p->name, name))
    {
      if (prev)
        prev->next = p->next;
      else
        n->props = p->next;
      if (n->props_tail == p)
        n->props_tail = prev;
      return;
    }
  }
}

static void merge_into(dt_tree_t *t, dt_node_t *dst, const dt_node_t *src)
{
  for (int i = 0; i < src->labels.count; i++)
    sl_add_unique(&dst->labels, src->labels.v[i]);
  register_labels(t, dst);
  if (!dst->file)
  {
    dst->file = src->file;
    dst->line = src->line;
  }

  for (const dt_prop_t *p = src->props; p; p = p->next)
  {
    if (p->directive == DT_DIR_PREPROC)
      continue;
    if (p->directive == DT_DIR_DEL_PROP)
    {
      remove_prop(dst, p->name);
      continue;
    }
    if (p->directive == DT_DIR_DEL_NODE)
    {
      dt_node_t *victim = p->name[0] == '&' ? dt_tree_ref(t, p->name) : dt_child(dst, p->name);
      if (victim && victim->parent)
        unlink_child(victim->parent, victim);
      continue;
    }
    {
      dt_prop_t *e = dt_prop(dst, p->name);
      if (e)
      {
        free(e->value);
        e->value = p->value ? xstrdup(p->value) : NULL;
      }
      else
      {
        dt_prop_t *q = dt_node_add_prop(dst, p->name, p->value, NULL);
        q->line = p->line;
      }
    }
  }
  for (const dt_node_t *c = src->children; c; c = c->next)
  {
    dt_node_t *d = dt_child(dst, c->name);
    if (!d)
    {
      d = dt_node_new(c->name);
      d->file = c->file;
      d->line = c->line;
      dt_node_add_child(dst, d);
    }
    merge_into(t, d, c);
  }
}

bool dt_resolve_include(const char *name, const char *from_file, const char **dirs, int ndirs, char *out, size_t cap)
{
  char dir[A2D_MAX_PATH];
  if (from_file)
  {
    path_dirname(from_file, dir, sizeof(dir));
    path_join(out, cap, dir, name);
    path_normalize_slashes(out);
    if (file_exists(out))
      return true;
  }
  for (int i = 0; i < ndirs; i++)
  {
    path_join(out, cap, dirs[i], name);
    path_normalize_slashes(out);
    if (file_exists(out))
      return true;
  }
  return false;
}

static void tree_add_parsed_depth(dt_tree_t *t, const dt_file_t *f, const char **dirs, int ndirs, bool follow,
                                  int depth)
{
  sl_add_unique(&t->files, f->path);
  for (const dt_top_t *it = f->items; it; it = it->next)
  {
    switch (it->kind)
    {
      case TOP_INCLUDE:
        if (follow && it->include && !it->sys_include && depth < 16)
        {
          char       p[A2D_MAX_PATH];
          dt_file_t *inc;
          if (!dt_resolve_include(it->include, f->path, dirs, ndirs, p, sizeof(p)))
          {
            sl_add_unique(&t->missing, it->include);
            continue;
          }
          inc = dt_parse_file(p);
          if (!inc)
          {
            sl_add_unique(&t->missing, it->include);
            continue;
          }
          tree_add_parsed_depth(t, inc, dirs, ndirs, follow, depth + 1);
        }
        break;
      case TOP_NODE:
        if (it->node->ref)
        {
          dt_node_t *target = dt_tree_ref(t, it->node->ref);
          if (target)
            merge_into(t, target, it->node);
          else
            log_verbose("%s: reference %s not resolvable", f->path, it->node->ref);
        }
        else
          merge_into(t, t->root, it->node);
        break;
      case TOP_DELETE_NODE:
      {
        dt_node_t *victim = it->ref ? dt_tree_ref(t, it->ref) : NULL;
        if (victim && victim->parent)
          unlink_child(victim->parent, victim);
        break;
      }
      default:
        break;
    }
  }
}

void dt_tree_add_parsed(dt_tree_t *t, const dt_file_t *f, const char **dirs, int ndirs, bool follow_includes)
{
  tree_add_parsed_depth(t, f, dirs, ndirs, follow_includes, 0);
}

bool dt_tree_add_file(dt_tree_t *t, const char *path, const char **dirs, int ndirs)
{
  dt_file_t *f = dt_parse_file(path);
  if (!f)
    return false;
  dt_tree_add_parsed(t, f, dirs, ndirs, true);
  return true;
}

/* ======================================================================== */
/* queries                                                                  */
/* ======================================================================== */

static bool is_live(const dt_node_t *n)
{
  for (; n; n = n->parent)
    if (n->deleted)
      return false;
  return true;
}

dt_node_t *dt_tree_label(const dt_tree_t *t, const char *label)
{
  dt_node_t *n = sm_get(&t->labels, label);
  return (n && is_live(n)) ? n : NULL;
}

dt_node_t *dt_child(const dt_node_t *n, const char *name)
{
  if (!n || !name)
    return NULL;
  for (dt_node_t *c = n->children; c; c = c->next)
    if (c->name && 0 == strcmp(c->name, name))
      return c;
  /* "name" without unit address matches "name@..." only if unique */
  if (!strchr(name, '@'))
  {
    dt_node_t *found = NULL;
    size_t     nl = strlen(name);
    for (dt_node_t *c = n->children; c; c = c->next)
      if (c->name && 0 == strncmp(c->name, name, nl) && c->name[nl] == '@')
      {
        if (found)
          return NULL;
        found = c;
      }
    return found;
  }
  return NULL;
}

dt_node_t *dt_tree_path(const dt_tree_t *t, const char *path)
{
  dt_node_t *n = t->root;
  char       buf[512];
  char      *tok, *ctx = NULL;

  str_copy(buf, sizeof(buf), path);
  for (tok = strtok_s(buf, "/", &ctx); tok && n; tok = strtok_s(NULL, "/", &ctx))
    n = dt_child(n, tok);
  return n;
}

dt_node_t *dt_tree_ref(const dt_tree_t *t, const char *ref)
{
  if (!ref)
    return NULL;
  if (ref[0] == '&')
    ref++;
  if (ref[0] == '{')
  {
    char   p[512];
    size_t n = strlen(ref);
    str_copy(p, sizeof(p), ref + 1);
    if (n >= 2)
      p[n - 2] = 0;
    return dt_tree_path(t, p);
  }
  return dt_tree_label(t, ref);
}

dt_prop_t *dt_prop(const dt_node_t *n, const char *name)
{
  if (!n)
    return NULL;
  for (dt_prop_t *p = n->props; p; p = p->next)
    if (!p->directive && 0 == strcmp(p->name, name))
      return p;
  return NULL;
}

const char *dt_prop_value(const dt_node_t *n, const char *name)
{
  dt_prop_t *p = dt_prop(n, name);
  return p ? (p->value ? p->value : "") : NULL;
}

bool dt_unit_address(const dt_node_t *n, uint64_t *addr)
{
  const char *at;
  char       *end;
  if (!n || !n->name || !(at = strchr(n->name, '@')))
    return false;
  *addr = strtoull(at + 1, &end, 16);
  return end != at + 1;
}

void dt_node_path(const dt_node_t *n, char *out, size_t cap)
{
  const dt_node_t *chain[64];
  int              k = 0;
  strbuf_t         sb;

  for (; n && n->parent && k < 64; n = n->parent)
    chain[k++] = n;
  sb_init(&sb);
  if (!k)
    sb_putc(&sb, '/');
  while (k-- > 0)
  {
    sb_putc(&sb, '/');
    sb_puts(&sb, chain[k]->name);
  }
  str_copy(out, cap, sb.buf);
  sb_free(&sb);
}

bool dt_node_enabled(const dt_node_t *n)
{
  const char *st = dt_prop_value(n, "status");
  return !st || strstr(st, "okay") || strstr(st, "ok\"");
}

void dt_node_basename(const dt_node_t *n, char *out, size_t cap)
{
  const char *at;
  str_copy(out, cap, n && n->name ? n->name : "");
  at = strchr(out, '@');
  if (at)
    out[at - out] = 0;
}

/* ======================================================================== */
/* value helpers                                                            */
/* ======================================================================== */

int dt_value_strings(const char *raw, strlist_t *out)
{
  int n = 0;
  if (!raw)
    return 0;
  while (*raw)
  {
    if (*raw == '"')
    {
      strbuf_t sb;
      sb_init(&sb);
      raw++;
      while (*raw && *raw != '"')
      {
        if (*raw == '\\' && raw[1])
          raw++;
        sb_putc(&sb, *raw++);
      }
      if (*raw)
        raw++;
      sl_add(out, sb.buf ? sb.buf : "");
      sb_free(&sb);
      n++;
    }
    else
      raw++;
  }
  return n;
}

int dt_value_cells(const char *raw, strlist_t *out)
{
  int  n = 0;
  bool in = false;
  if (!raw)
    return 0;
  while (*raw)
  {
    if (*raw == '<')
      in = true;
    else if (*raw == '>')
      in = false;
    else if (in && *raw != ' ' && *raw != '\t' && *raw != '\n' && *raw != '\r')
    {
      const char *s = raw;
      int         par = 0;
      while (*raw && (par > 0 || (*raw != ' ' && *raw != '\t' && *raw != '\n' && *raw != '\r' && *raw != '>')))
      {
        if (*raw == '(')
          par++;
        else if (*raw == ')')
          par--;
        raw++;
      }
      {
        char *tok = xstrndup(s, (size_t)(raw - s));
        sl_add(out, tok);
        free(tok);
      }
      n++;
      continue;
    }
    raw++;
  }
  return n;
}

bool dt_value_first_u64(const char *raw, uint64_t *v)
{
  strlist_t cells;
  bool      ok = false;
  memset(&cells, 0, sizeof(cells));
  if (dt_value_cells(raw, &cells) > 0)
  {
    char *end;
    *v = strtoull(cells.v[0], &end, 0);
    ok = end != cells.v[0] && *end == 0;
  }
  sl_free(&cells);
  return ok;
}
