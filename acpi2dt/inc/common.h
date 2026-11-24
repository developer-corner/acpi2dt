/**
 * @file   common.h
 * @author Ingo A. Kubbilun
 * @brief  shared definitions, configuration and utility helpers of acpi2dt
 *
 * [MIT license] - see main.c
 */

#ifndef A2D_COMMON_H
#define A2D_COMMON_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define A2D_VERSION_STRING      "v0.2"
#define A2D_MAX_PATH            1024

/* ANSI escape sequences (stripped automatically if the console is not a TTY) */
#define CTRL_RESET              "\033[0;0;0m"
#define CTRL_RED                "\033[1;31m"
#define CTRL_GREEN              "\033[1;32m"
#define CTRL_YELLOW             "\033[1;33m"
#define CTRL_BLUE               "\033[1;34m"
#define CTRL_MAGENTA            "\033[1;35m"
#define CTRL_CYAN               "\033[1;36m"

/* ------------------------------------------------------------------------ */
/* configuration                                                            */
/* ------------------------------------------------------------------------ */

typedef struct {
  /* positional arguments */
  const char   *output_dts;            /* output DTS file (draft)                     */
  const char   *output_fw_dir;         /* output firmware tree (mirrors /lib/firmware)*/
  const char   *kernel_dts_dir;        /* Linux kernel arch/arm64/boot/dts[/qcom]     */
  const char  **similar_dts;           /* similar DTS files, [0] = highest priority   */
  int           num_similar;

  /* options */
  char          work_dir[A2D_MAX_PATH];/* intermediate artifacts (ACPI dumps, JSON)   */
  const char   *acpi_dir;              /* read ACPI tables from a folder (offline)    */
  const char   *iasl_exe;              /* ACPICA iasl / AslCompiler.exe               */
  bool          no_iasl;
  const char   *soc;                   /* override SoC, e.g. "x1p42100"               */
  const char   *model;                 /* override root "model"                       */
  const char   *compatible;            /* override board compatible                   */
  const char   *vendor_dir;            /* override firmware vendor dir ("ASUSTeK")    */
  const char   *board_slug;            /* override firmware board dir ("vivobook-s16")*/
  bool          no_devmgr;
  bool          no_drivers;
  bool          no_fw_copy;
  bool          full_driverstore;      /* index whole DriverStore\FileRepository      */
  bool          verbose;
} a2d_config_t;

/* ------------------------------------------------------------------------ */
/* console output (util.c)                                                  */
/* ------------------------------------------------------------------------ */

void  console_init(void);
void  console_restore(void);
void  FPRINTF(FILE *f, const char *fmt, ...);
void  log_set_verbose(bool v);
void  log_step(const char *fmt, ...);
void  log_info(const char *fmt, ...);
void  log_ok(const char *fmt, ...);
void  log_warn(const char *fmt, ...);
void  log_err(const char *fmt, ...);
void  log_verbose(const char *fmt, ...);

/* ------------------------------------------------------------------------ */
/* memory helpers (abort on OOM)                                            */
/* ------------------------------------------------------------------------ */

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

#define VEC_PUSH(arr, cnt, cap, val)                                         \
  do {                                                                       \
    if ((cnt) >= (cap)) {                                                    \
      (cap) = (cap) ? (cap) * 2 : 16;                                        \
      (arr) = xrealloc((arr), (size_t)(cap) * sizeof(*(arr)));               \
    }                                                                        \
    (arr)[(cnt)++] = (val);                                                  \
  } while (0)

/* ------------------------------------------------------------------------ */
/* growing string buffer                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
  char   *buf;
  size_t  len;
  size_t  cap;
} strbuf_t;

void  sb_init(strbuf_t *sb);
void  sb_free(strbuf_t *sb);
void  sb_putc(strbuf_t *sb, char c);
void  sb_putn(strbuf_t *sb, const char *s, size_t n);
void  sb_puts(strbuf_t *sb, const char *s);
void  sb_printf(strbuf_t *sb, const char *fmt, ...);
void  sb_vprintf(strbuf_t *sb, const char *fmt, va_list ap);
void  sb_indent(strbuf_t *sb, int depth);
char *sb_detach(strbuf_t *sb);
void  sb_clear(strbuf_t *sb);
/* appends text but breaks any comment terminator so it can live inside a block comment */
void  sb_put_comment_safe(strbuf_t *sb, const char *s);

/* ------------------------------------------------------------------------ */
/* string list                                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
  char **v;
  int    count;
  int    cap;
} strlist_t;

void  sl_add(strlist_t *sl, const char *s);
bool  sl_add_unique(strlist_t *sl, const char *s);     /* case-insensitive */
bool  sl_contains(const strlist_t *sl, const char *s); /* case-insensitive */
void  sl_free(strlist_t *sl);

/* ------------------------------------------------------------------------ */
/* simple string -> pointer hash map (case-sensitive keys)                  */
/* ------------------------------------------------------------------------ */

typedef struct {
  char  **keys;
  void  **vals;
  size_t  cap;
  size_t  count;
} strmap_t;

void   sm_put(strmap_t *m, const char *key, void *val);
void  *sm_get(const strmap_t *m, const char *key);
void   sm_free(strmap_t *m);

/* ------------------------------------------------------------------------ */
/* string / path / file helpers                                             */
/* ------------------------------------------------------------------------ */

wchar_t *utf8_to_wide(const char *s);                    /* malloc'ed */
char    *wide_to_utf8(const wchar_t *ws);                /* malloc'ed */
void     wide_to_utf8_buf(const wchar_t *ws, char *out, size_t cap);

bool     str_starts_with(const char *s, const char *prefix);
bool     str_starts_with_ci(const char *s, const char *prefix);
bool     str_ends_with_ci(const char *s, const char *suffix);
bool     str_contains_ci(const char *hay, const char *needle);
char    *str_trim(char *s);                              /* in place */
void     str_lower(char *s);
void     str_upper(char *s);
void     str_copy(char *dst, size_t cap, const char *src);

void     path_join(char *out, size_t cap, const char *a, const char *b);
const char *path_basename(const char *p);
void     path_dirname(const char *p, char *out, size_t cap);
void     path_normalize_slashes(char *p);                /* '/' -> '\\'          */
bool     file_exists(const char *path);
bool     dir_exists(const char *path);
bool     mkdir_p(const char *path);
uint8_t *read_file(const char *path, size_t *out_len);   /* NUL-terminated */
bool     write_file(const char *path, const void *data, size_t len);
bool     copy_file(const char *src, const char *dst);
int64_t  file_size(const char *path);
bool     sha256_file(const char *path, char out_hex[65]);
int      run_process(const char *cmdline, const char *cwd, DWORD timeout_ms);

/* directory walk: callback returns false to stop descending/iterating */
typedef bool (*walk_cb_t)(const char *full_path, const char *name, bool is_dir, void *ctx);
void     walk_dir(const char *dir, bool recursive, walk_cb_t cb, void *ctx);

/* JSON output helper */
void     json_escape_fp(FILE *fp, const char *s);

#endif /* A2D_COMMON_H */
