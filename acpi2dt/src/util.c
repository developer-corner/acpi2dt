/**
 * @file   util.c
 * @author Ingo A. Kubbilun
 * @brief  console, memory, string, path, file and process helpers
 *
 * [MIT license] - see main.c
 */

#include "common.h"
#include <io.h>
#include <bcrypt.h>

/* ======================================================================== */
/* console                                                                  */
/* ======================================================================== */

static int    g_colors      = 0;
static DWORD  g_con_mode    = 0;
static HANDLE g_con         = INVALID_HANDLE_VALUE;
static bool   g_verbose     = false;

static BOOL WINAPI ctrl_handler(DWORD type)
{
  if (CTRL_C_EVENT == type || CTRL_BREAK_EVENT == type)
    console_restore();
  return FALSE;
}

void console_init(void)
{
  int fd_out = _fileno(stdout);
  int fd_err = _fileno(stderr);

  SetConsoleOutputCP(CP_UTF8);

  if (fd_out >= 0 && _isatty(fd_out) && fd_err >= 0 && _isatty(fd_err))
  {
    g_con = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(g_con, &g_con_mode))
    {
      SetConsoleMode(g_con, g_con_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
      g_colors = 1;
    }
  }
  SetConsoleCtrlHandler(ctrl_handler, TRUE);
}

void console_restore(void)
{
  if (g_colors && INVALID_HANDLE_VALUE != g_con)
  {
    SetConsoleMode(g_con, g_con_mode);
    g_colors = 0;
  }
}

static void vfprintf_colored(FILE *f, const char *fmt, va_list ap)
{
  char      stackbuf[2048];
  char     *buffer = stackbuf;
  int       n;
  va_list   ap2;
  size_t    src = 0, dst = 0;

  va_copy(ap2, ap);
  n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
  if (n >= (int)sizeof(stackbuf))
  {
    buffer = xmalloc((size_t)n + 1);
    vsnprintf(buffer, (size_t)n + 1, fmt, ap2);
  }
  va_end(ap2);
  if (n < 0)
    return;

  if (0 == g_colors || (stdout != f && stderr != f))
  {
    /* strip ESC [ ... m sequences */
    while (0 != buffer[src])
    {
      if ('\033' == buffer[src] && '[' == buffer[src + 1])
      {
        src += 2;
        while (buffer[src] && 'm' != buffer[src])
          src++;
        if (buffer[src])
          src++;
      }
      else
        buffer[dst++] = buffer[src++];
    }
    buffer[dst] = 0;
  }

  fputs(buffer, f);
  if (buffer != stackbuf)
    free(buffer);
}

void FPRINTF(FILE *f, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vfprintf_colored(f, fmt, ap);
  va_end(ap);
}

void log_set_verbose(bool v) { g_verbose = v; }

#define LOG_IMPL(stream, prefix)               \
  va_list ap;                                  \
  FPRINTF(stream, "%s", prefix);               \
  va_start(ap, fmt);                           \
  vfprintf_colored(stream, fmt, ap);           \
  va_end(ap);                                  \
  FPRINTF(stream, "\n");

void log_step(const char *fmt, ...)    { LOG_IMPL(stdout, "\n" CTRL_CYAN "==> " CTRL_RESET) }
void log_info(const char *fmt, ...)    { LOG_IMPL(stdout, "    ") }
void log_ok(const char *fmt, ...)      { LOG_IMPL(stdout, CTRL_GREEN "    [ok] " CTRL_RESET) }
void log_warn(const char *fmt, ...)    { LOG_IMPL(stdout, CTRL_YELLOW "    [warn] " CTRL_RESET) }
void log_err(const char *fmt, ...)     { LOG_IMPL(stderr, CTRL_RED "    [error] " CTRL_RESET) }
void log_verbose(const char *fmt, ...)
{
  if (!g_verbose)
    return;
  {
    LOG_IMPL(stdout, CTRL_BLUE "    [v] " CTRL_RESET)
  }
}

/* ======================================================================== */
/* memory                                                                   */
/* ======================================================================== */

static void oom(void)
{
  fputs("acpi2dt: out of memory\n", stderr);
  console_restore();
  exit(2);
}

void *xmalloc(size_t n)            { void *p = malloc(n ? n : 1); if (!p) oom(); return p; }
void *xcalloc(size_t n, size_t sz) { void *p = calloc(n ? n : 1, sz ? sz : 1); if (!p) oom(); return p; }
void *xrealloc(void *p, size_t n)  { void *q = realloc(p, n ? n : 1); if (!q) oom(); return q; }
char *xstrdup(const char *s)       { return xstrndup(s ? s : "", strlen(s ? s : "")); }
char *xstrndup(const char *s, size_t n)
{
  char *p = xmalloc(n + 1);
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}

/* ======================================================================== */
/* string buffer                                                            */
/* ======================================================================== */

void sb_init(strbuf_t *sb) { sb->buf = NULL; sb->len = 0; sb->cap = 0; }
void sb_free(strbuf_t *sb) { free(sb->buf); sb_init(sb); }
void sb_clear(strbuf_t *sb) { sb->len = 0; if (sb->buf) sb->buf[0] = 0; }

static void sb_grow(strbuf_t *sb, size_t extra)
{
  if (sb->len + extra + 1 > sb->cap)
  {
    size_t nc = sb->cap ? sb->cap : 256;
    while (nc < sb->len + extra + 1)
      nc *= 2;
    sb->buf = xrealloc(sb->buf, nc);
    sb->cap = nc;
  }
}

void sb_putn(strbuf_t *sb, const char *s, size_t n)
{
  sb_grow(sb, n);
  memcpy(sb->buf + sb->len, s, n);
  sb->len += n;
  sb->buf[sb->len] = 0;
}

void sb_putc(strbuf_t *sb, char c) { sb_putn(sb, &c, 1); }
void sb_puts(strbuf_t *sb, const char *s) { if (s) sb_putn(sb, s, strlen(s)); }

void sb_vprintf(strbuf_t *sb, const char *fmt, va_list ap)
{
  va_list ap2;
  int     n;

  va_copy(ap2, ap);
  n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (n <= 0)
    return;
  sb_grow(sb, (size_t)n);
  vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap);
  sb->len += (size_t)n;
}

void sb_printf(strbuf_t *sb, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  sb_vprintf(sb, fmt, ap);
  va_end(ap);
}

void sb_indent(strbuf_t *sb, int depth)
{
  for (int i = 0; i < depth; i++)
    sb_putc(sb, '\t');
}

char *sb_detach(strbuf_t *sb)
{
  char *p = sb->buf ? sb->buf : xstrdup("");
  sb_init(sb);
  return p;
}

void sb_put_comment_safe(strbuf_t *sb, const char *s)
{
  if (!s)
    return;
  for (; *s; s++)
  {
    sb_putc(sb, *s);
    if ('*' == s[0] && '/' == s[1])
      sb_putc(sb, ' ');
    else if ('/' == s[0] && '*' == s[1])
      sb_putc(sb, ' ');
  }
}

/* ======================================================================== */
/* string list                                                              */
/* ======================================================================== */

void sl_add(strlist_t *sl, const char *s)
{
  char *d = xstrdup(s);
  VEC_PUSH(sl->v, sl->count, sl->cap, d);
}

bool sl_contains(const strlist_t *sl, const char *s)
{
  for (int i = 0; i < sl->count; i++)
    if (0 == _stricmp(sl->v[i], s))
      return true;
  return false;
}

bool sl_add_unique(strlist_t *sl, const char *s)
{
  if (!s || !*s || sl_contains(sl, s))
    return false;
  sl_add(sl, s);
  return true;
}

void sl_free(strlist_t *sl)
{
  for (int i = 0; i < sl->count; i++)
    free(sl->v[i]);
  free(sl->v);
  memset(sl, 0, sizeof(*sl));
}

/* ======================================================================== */
/* hash map                                                                 */
/* ======================================================================== */

static uint64_t fnv1a(const char *s)
{
  uint64_t h = 1469598103934665603ULL;
  while (*s)
  {
    h ^= (uint8_t)*s++;
    h *= 1099511628211ULL;
  }
  return h;
}

static void sm_rehash(strmap_t *m, size_t ncap)
{
  char  **ok = m->keys;
  void  **ov = m->vals;
  size_t  oc = m->cap;

  m->keys  = xcalloc(ncap, sizeof(char *));
  m->vals  = xcalloc(ncap, sizeof(void *));
  m->cap   = ncap;
  m->count = 0;
  for (size_t i = 0; i < oc; i++)
  {
    if (ok[i])
    {
      size_t j = (size_t)(fnv1a(ok[i]) & (ncap - 1));
      while (m->keys[j])
        j = (j + 1) & (ncap - 1);
      m->keys[j] = ok[i];
      m->vals[j] = ov[i];
      m->count++;
    }
  }
  free(ok);
  free(ov);
}

void sm_put(strmap_t *m, const char *key, void *val)
{
  size_t j;

  if ((m->count + 1) * 2 > m->cap)
    sm_rehash(m, m->cap ? m->cap * 2 : 64);
  j = (size_t)(fnv1a(key) & (m->cap - 1));
  while (m->keys[j])
  {
    if (0 == strcmp(m->keys[j], key))
    {
      m->vals[j] = val;
      return;
    }
    j = (j + 1) & (m->cap - 1);
  }
  m->keys[j] = xstrdup(key);
  m->vals[j] = val;
  m->count++;
}

void *sm_get(const strmap_t *m, const char *key)
{
  size_t j;
  if (!m->cap || !key)
    return NULL;
  j = (size_t)(fnv1a(key) & (m->cap - 1));
  while (m->keys[j])
  {
    if (0 == strcmp(m->keys[j], key))
      return m->vals[j];
    j = (j + 1) & (m->cap - 1);
  }
  return NULL;
}

void sm_free(strmap_t *m)
{
  for (size_t i = 0; i < m->cap; i++)
    free(m->keys[i]);
  free(m->keys);
  free(m->vals);
  memset(m, 0, sizeof(*m));
}

/* ======================================================================== */
/* strings                                                                  */
/* ======================================================================== */

wchar_t *utf8_to_wide(const char *s)
{
  int      n;
  wchar_t *w;

  if (!s)
    s = "";
  n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (n <= 0)
  {
    w = xmalloc(sizeof(wchar_t));
    w[0] = 0;
    return w;
  }
  w = xmalloc((size_t)n * sizeof(wchar_t));
  MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}

char *wide_to_utf8(const wchar_t *ws)
{
  int   n;
  char *s;

  if (!ws)
    ws = L"";
  n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
  if (n <= 0)
    return xstrdup("");
  s = xmalloc((size_t)n);
  WideCharToMultiByte(CP_UTF8, 0, ws, -1, s, n, NULL, NULL);
  return s;
}

void wide_to_utf8_buf(const wchar_t *ws, char *out, size_t cap)
{
  if (!out || !cap)
    return;
  out[0] = 0;
  if (!ws)
    return;
  if (WideCharToMultiByte(CP_UTF8, 0, ws, -1, out, (int)cap, NULL, NULL) <= 0)
    out[0] = 0;
  out[cap - 1] = 0;
}

bool str_starts_with(const char *s, const char *prefix)
{
  return s && prefix && 0 == strncmp(s, prefix, strlen(prefix));
}

bool str_starts_with_ci(const char *s, const char *prefix)
{
  return s && prefix && 0 == _strnicmp(s, prefix, strlen(prefix));
}

bool str_ends_with_ci(const char *s, const char *suffix)
{
  size_t sl, xl;
  if (!s || !suffix)
    return false;
  sl = strlen(s);
  xl = strlen(suffix);
  return sl >= xl && 0 == _stricmp(s + sl - xl, suffix);
}

bool str_contains_ci(const char *hay, const char *needle)
{
  size_t nl;
  if (!hay || !needle)
    return false;
  nl = strlen(needle);
  if (!nl)
    return true;
  for (; *hay; hay++)
    if (0 == _strnicmp(hay, needle, nl))
      return true;
  return false;
}

char *str_trim(char *s)
{
  char *e;
  if (!s)
    return s;
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
    s++;
  e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
    *--e = 0;
  return s;
}

void str_lower(char *s) { for (; s && *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32); }
void str_upper(char *s) { for (; s && *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32); }

void str_copy(char *dst, size_t cap, const char *src)
{
  if (!cap)
    return;
  if (!src)
    src = "";
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = 0;
}

/* ======================================================================== */
/* paths & files                                                            */
/* ======================================================================== */

void path_join(char *out, size_t cap, const char *a, const char *b)
{
  size_t al;
  char   tmp[A2D_MAX_PATH];

  str_copy(tmp, sizeof(tmp), a ? a : "");
  al = strlen(tmp);
  if (al && tmp[al - 1] != '\\' && tmp[al - 1] != '/')
    snprintf(out, cap, "%s\\%s", tmp, b ? b : "");
  else
    snprintf(out, cap, "%s%s", tmp, b ? b : "");
}

const char *path_basename(const char *p)
{
  const char *b = p;
  for (; p && *p; p++)
    if (*p == '\\' || *p == '/')
      b = p + 1;
  return b;
}

void path_dirname(const char *p, char *out, size_t cap)
{
  const char *b = path_basename(p);
  size_t      n = (size_t)(b - p);

  if (n > 0)
    n--; /* strip separator */
  if (n == 0 && (p[0] == '\\' || p[0] == '/'))
    n = 1;
  if (n >= cap)
    n = cap - 1;
  if (0 == n)
  {
    str_copy(out, cap, ".");
    return;
  }
  memcpy(out, p, n);
  out[n] = 0;
}

void path_normalize_slashes(char *p)
{
  for (; p && *p; p++)
    if (*p == '/')
      *p = '\\';
}

static DWORD get_attr(const char *path)
{
  wchar_t *w = utf8_to_wide(path);
  DWORD    a = GetFileAttributesW(w);
  free(w);
  return a;
}

bool file_exists(const char *path)
{
  DWORD a = get_attr(path);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const char *path)
{
  DWORD a = get_attr(path);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool mkdir_p(const char *path)
{
  char tmp[A2D_MAX_PATH];
  size_t n;

  str_copy(tmp, sizeof(tmp), path);
  path_normalize_slashes(tmp);
  n = strlen(tmp);
  while (n > 0 && tmp[n - 1] == '\\')
    tmp[--n] = 0;
  if (!n)
    return false;

  for (size_t i = 1; i <= n; i++)
  {
    if (tmp[i] == '\\' || tmp[i] == 0)
    {
      char c = tmp[i];
      tmp[i] = 0;
      if (!(i == 2 && tmp[1] == ':') && !dir_exists(tmp))
      {
        wchar_t *w = utf8_to_wide(tmp);
        BOOL     ok = CreateDirectoryW(w, NULL);
        DWORD    e = GetLastError();
        free(w);
        if (!ok && e != ERROR_ALREADY_EXISTS)
          return false;
      }
      tmp[i] = c;
    }
  }
  return true;
}

static FILE *fopen_utf8(const char *path, const char *mode)
{
  wchar_t *wp = utf8_to_wide(path);
  wchar_t *wm = utf8_to_wide(mode);
  FILE    *f = _wfopen(wp, wm);
  free(wp);
  free(wm);
  return f;
}

uint8_t *read_file(const char *path, size_t *out_len)
{
  FILE    *f = fopen_utf8(path, "rb");
  uint8_t *buf;
  long     sz;
  size_t   rd;

  if (out_len)
    *out_len = 0;
  if (!f)
    return NULL;
  if (0 != _fseeki64(f, 0, SEEK_END))
  {
    fclose(f);
    return NULL;
  }
  sz = (long)_ftelli64(f);
  _fseeki64(f, 0, SEEK_SET);
  if (sz < 0)
  {
    fclose(f);
    return NULL;
  }
  buf = xmalloc((size_t)sz + 2);
  rd = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[rd] = 0;
  buf[rd + 1] = 0;
  if (out_len)
    *out_len = rd;
  return buf;
}

bool write_file(const char *path, const void *data, size_t len)
{
  FILE *f = fopen_utf8(path, "wb");
  bool  ok;
  if (!f)
    return false;
  ok = fwrite(data, 1, len, f) == len;
  ok = (0 == fclose(f)) && ok;
  return ok;
}

bool copy_file(const char *src, const char *dst)
{
  wchar_t *ws = utf8_to_wide(src);
  wchar_t *wd = utf8_to_wide(dst);
  BOOL     ok = CopyFileW(ws, wd, FALSE);
  free(ws);
  free(wd);
  return ok != FALSE;
}

int64_t file_size(const char *path)
{
  WIN32_FILE_ATTRIBUTE_DATA fad;
  wchar_t *w = utf8_to_wide(path);
  BOOL     ok = GetFileAttributesExW(w, GetFileExInfoStandard, &fad);
  free(w);
  if (!ok)
    return -1;
  return ((int64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

bool sha256_file(const char *path, char out_hex[65])
{
  BCRYPT_ALG_HANDLE  alg = NULL;
  BCRYPT_HASH_HANDLE h = NULL;
  uint8_t            digest[32];
  uint8_t           *buf;
  FILE              *f;
  size_t             n;
  bool               ok = false;

  out_hex[0] = 0;
  f = fopen_utf8(path, "rb");
  if (!f)
    return false;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
    goto done;
  if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) < 0)
    goto done;
  buf = xmalloc(1 << 20);
  while ((n = fread(buf, 1, 1 << 20, f)) > 0)
    BCryptHashData(h, buf, (ULONG)n, 0);
  free(buf);
  if (BCryptFinishHash(h, digest, sizeof(digest), 0) >= 0)
  {
    for (int i = 0; i < 32; i++)
      sprintf(out_hex + i * 2, "%02x", digest[i]);
    ok = true;
  }
done:
  if (h)
    BCryptDestroyHash(h);
  if (alg)
    BCryptCloseAlgorithmProvider(alg, 0);
  fclose(f);
  return ok;
}

int run_process(const char *cmdline, const char *cwd, DWORD timeout_ms)
{
  STARTUPINFOW        si;
  PROCESS_INFORMATION pi;
  wchar_t            *wcmd = utf8_to_wide(cmdline);
  wchar_t            *wcwd = cwd ? utf8_to_wide(cwd) : NULL;
  DWORD               code = (DWORD)-1;
  HANDLE              hnul;
  SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };

  hnul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, NULL);

  ZeroMemory(&si, sizeof(si));
  ZeroMemory(&pi, sizeof(pi));
  si.cb = sizeof(si);
  if (hnul != INVALID_HANDLE_VALUE)
  {
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = NULL;
    si.hStdOutput = hnul;
    si.hStdError  = hnul;
  }

  if (CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, wcwd, &si, &pi))
  {
    if (WAIT_TIMEOUT == WaitForSingleObject(pi.hProcess, timeout_ms))
    {
      TerminateProcess(pi.hProcess, 1);
      code = (DWORD)-2;
    }
    else
      GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }
  if (hnul != INVALID_HANDLE_VALUE)
    CloseHandle(hnul);
  free(wcmd);
  free(wcwd);
  return (int)code;
}

static bool walk_dir_impl(const char *dir, bool recursive, walk_cb_t cb, void *ctx)
{
  char             pattern[A2D_MAX_PATH];
  WIN32_FIND_DATAW fd;
  HANDLE           h;
  wchar_t         *wp;
  bool             cont = true;

  path_join(pattern, sizeof(pattern), dir, "*");
  wp = utf8_to_wide(pattern);
  h = FindFirstFileW(wp, &fd);
  free(wp);
  if (h == INVALID_HANDLE_VALUE)
    return true;

  do
  {
    char name[MAX_PATH * 3];
    char full[A2D_MAX_PATH];
    bool is_dir;

    if (0 == wcscmp(fd.cFileName, L".") || 0 == wcscmp(fd.cFileName, L".."))
      continue;
    wide_to_utf8_buf(fd.cFileName, name, sizeof(name));
    path_join(full, sizeof(full), dir, name);
    is_dir = 0 != (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    if (is_dir && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
      continue; /* do not follow junctions */

    if (!cb(full, name, is_dir, ctx))
    {
      if (!is_dir)
      {
        cont = false;
        break;
      }
      continue; /* callback declined to descend */
    }
    if (is_dir && recursive)
    {
      if (!walk_dir_impl(full, recursive, cb, ctx))
      {
        cont = false;
        break;
      }
    }
  } while (FindNextFileW(h, &fd));

  FindClose(h);
  return cont;
}

void walk_dir(const char *dir, bool recursive, walk_cb_t cb, void *ctx)
{
  walk_dir_impl(dir, recursive, cb, ctx);
}

void json_escape_fp(FILE *fp, const char *s)
{
  fputc('"', fp);
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p)
  {
    switch (*p)
    {
      case '"':  fputs("\\\"", fp); break;
      case '\\': fputs("\\\\", fp); break;
      case '\n': fputs("\\n", fp); break;
      case '\r': fputs("\\r", fp); break;
      case '\t': fputs("\\t", fp); break;
      default:
        if (*p < 0x20)
          fprintf(fp, "\\u%04x", *p);
        else
          fputc(*p, fp); /* UTF-8 passes through */
        break;
    }
  }
  fputc('"', fp);
}
