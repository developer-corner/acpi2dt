/* SPDX-License-Identifier: MIT */
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
/** exclude rarely used Windows headers */
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/** acpi2dt version */
#define A2D_VERSION_STRING      "v0.3"
/** maximum path length used by acpi2dt */
#define A2D_MAX_PATH            1024

/** ANSI: reset colors (escape sequences are stripped if the console is not a TTY) */
#define CTRL_RESET              "\033[0;0;0m"
/** ANSI: red */
#define CTRL_RED                "\033[1;31m"
/** ANSI: green */
#define CTRL_GREEN              "\033[1;32m"
/** ANSI: yellow */
#define CTRL_YELLOW             "\033[1;33m"
/** ANSI: blue */
#define CTRL_BLUE               "\033[1;34m"
/** ANSI: magenta */
#define CTRL_MAGENTA            "\033[1;35m"
/** ANSI: cyan */
#define CTRL_CYAN               "\033[1;36m"

/**< ------------------------------------------------------------------------ */
/* configuration                                                            */
/* ------------------------------------------------------------------------ */

/** @brief command line configuration of acpi2dt */
typedef struct {
  /* positional arguments */
  const char   *output_dts;            /**< output DTS file (draft) */
  const char   *output_fw_dir;         /**< output firmware tree (mirrors /lib/firmware) */
  const char   *kernel_dts_dir;        /**< Linux kernel arch/arm64/boot/dts[/qcom] */
  const char  **similar_dts;           /**< similar DTS files, [0] = highest priority */
  int           num_similar;  /**< number of entries in similar_dts */

  /* options */
  char          work_dir[A2D_MAX_PATH];/**< intermediate artifacts (ACPI dumps, JSON) */
  const char   *acpi_dir;              /**< read ACPI tables from a folder (offline) */
  const char   *iasl_exe;              /**< external iasl / AslCompiler.exe (optional) */
  bool          no_iasl;               /**< no .dsl disassembly at all */
  bool          no_acpica;             /**< no embedded AML interpreter / disassembler */
  bool          no_dtc;                /**< no in-process DTB compilation of the draft */
  const char   *soc;                   /**< override SoC, e.g. "x1p42100" */
  const char   *model;                 /**< override root "model" */
  const char   *compatible;            /**< override board compatible */
  const char   *vendor_dir;            /**< override firmware vendor dir ("ASUSTeK") */
  const char   *board_slug;            /**< override firmware board dir ("vivobook-s16") */
  bool          no_devmgr;  /**< do not query the Device Manager */
  bool          no_drivers;  /**< do not scan driver packages / firmware */
  bool          no_fw_copy;  /**< resolve firmware but do not copy it */
  bool          full_driverstore;      /**< index whole DriverStore\\FileRepository */
  bool          verbose;  /**< verbose output (-v) */
} a2d_config_t;

/* ------------------------------------------------------------------------ */
/* console output (util.c)                                                  */
/* ------------------------------------------------------------------------ */

/**
 * @brief Enables UTF-8 output and ANSI colors if stdout and stderr are consoles; installs the Ctrl-C handler.
 */
void  console_init(void);
/**
 * @brief Restores the original console mode (colors off).
 */
void  console_restore(void);
/**
 * @brief printf-like output that strips ANSI color sequences when colors are not active.
 *
 * @param f    output stream (stdout, stderr or a file)
 * @param fmt  printf format
 * @param ...  format arguments
 */
void  FPRINTF(FILE *f, const char *fmt, ...);
/**
 * @brief Enables or disables log_verbose() output.
 *
 * @param v  true to print verbose messages
 */
void  log_set_verbose(bool v);
/**
 * @brief Prints a step headline ("==> ...").
 *
 * @param fmt  printf format
 * @param ...  format arguments
 */
void  log_step(const char *fmt, ...);
/**
 * @brief Prints an informational line.
 *
 * @param fmt  printf format
 * @param ...  format arguments
 */
void  log_info(const char *fmt, ...);
/**
 * @brief Prints a success line ("[ok] ...").
 *
 * @param fmt  printf format
 * @param ...  format arguments
 */
void  log_ok(const char *fmt, ...);
/**
 * @brief Prints a warning line ("[warn] ...").
 *
 * @param fmt  printf format
 * @param ...  format arguments
 */
void  log_warn(const char *fmt, ...);
/**
 * @brief Prints an error line to stderr ("[error] ...").
 *
 * @param fmt  printf format
 * @param ...  format arguments
 */
void  log_err(const char *fmt, ...);
/**
 * @brief Prints a line only in verbose mode (-v).
 *
 * @param fmt  printf format
 * @param ...  format arguments
 */
void  log_verbose(const char *fmt, ...);

/* ------------------------------------------------------------------------ */
/* memory helpers (abort on OOM)                                            */
/* ------------------------------------------------------------------------ */

/**
 * @brief malloc() that terminates acpi2dt on out-of-memory.
 *
 * @param n  number of bytes (0 allocates 1 byte)
 * @return pointer to the allocated memory, never NULL
 */
void *xmalloc(size_t n);
/**
 * @brief calloc() that terminates acpi2dt on out-of-memory.
 *
 * @param n   number of elements
 * @param sz  element size
 * @return pointer to zeroed memory, never NULL
 */
void *xcalloc(size_t n, size_t sz);
/**
 * @brief realloc() that terminates acpi2dt on out-of-memory.
 *
 * @param p  memory to resize (may be NULL)
 * @param n  new size in bytes
 * @return pointer to the resized memory, never NULL
 */
void *xrealloc(void *p, size_t n);
/**
 * @brief Duplicates a string (NULL is treated as "").
 *
 * @param s  string to copy
 * @return newly allocated copy
 */
char *xstrdup(const char *s);
/**
 * @brief Duplicates the first n characters of a string.
 *
 * @param s  source characters
 * @param n  number of characters to copy
 * @return newly allocated, NUL-terminated copy
 */
char *xstrndup(const char *s, size_t n);

/** appends val to the growing array arr (count cnt, capacity cap); aborts on out-of-memory */
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

/** @brief growing, NUL-terminated string buffer */
typedef struct {
  char   *buf;  /**< text (NUL-terminated, NULL while empty) */
  size_t  len;  /**< text length */
  size_t  cap;  /**< allocated size */
} strbuf_t;

/**
 * @brief Initializes an empty string buffer.
 *
 * @param sb  string buffer
 */
void  sb_init(strbuf_t *sb);
/**
 * @brief Frees the memory of a string buffer and re-initializes it.
 *
 * @param sb  string buffer
 */
void  sb_free(strbuf_t *sb);
/**
 * @brief Appends one character.
 *
 * @param sb  string buffer
 * @param c   character to append
 */
void  sb_putc(strbuf_t *sb, char c);
/**
 * @brief Appends n bytes.
 *
 * @param sb  string buffer
 * @param s   bytes to append
 * @param n   number of bytes
 */
void  sb_putn(strbuf_t *sb, const char *s, size_t n);
/**
 * @brief Appends a NUL-terminated string (NULL is ignored).
 *
 * @param sb  string buffer
 * @param s   string to append
 */
void  sb_puts(strbuf_t *sb, const char *s);
/**
 * @brief Appends printf-formatted text.
 *
 * @param sb   string buffer
 * @param fmt  printf format
 * @param ...  format arguments
 */
void  sb_printf(strbuf_t *sb, const char *fmt, ...);
/**
 * @brief Appends vprintf-formatted text.
 *
 * @param sb   string buffer
 * @param fmt  printf format
 * @param ap   argument list
 */
void  sb_vprintf(strbuf_t *sb, const char *fmt, va_list ap);
/**
 * @brief Appends tab characters.
 *
 * @param sb     string buffer
 * @param depth  number of tabs
 */
void  sb_indent(strbuf_t *sb, int depth);
/**
 * @brief Takes over the buffer contents; the string buffer is empty afterwards.
 *
 * @param sb  string buffer
 * @return the NUL-terminated contents (caller frees), never NULL
 */
char *sb_detach(strbuf_t *sb);
/**
 * @brief Empties the buffer without freeing its memory.
 *
 * @param sb  string buffer
 */
void  sb_clear(strbuf_t *sb);
/**
 * @brief Appends text that is placed inside a C block comment; "*""/" and "/""*" are broken up.
 *
 * @param sb  string buffer
 * @param s   text to append
 */
void  sb_put_comment_safe(strbuf_t *sb, const char *s);

/* ------------------------------------------------------------------------ */
/* string list                                                              */
/* ------------------------------------------------------------------------ */

/** @brief list of strings (owned copies) */
typedef struct {
  char **v;  /**< strings */
  int    count;  /**< number of strings */
  int    cap;  /**< capacity of v */
} strlist_t;

/**
 * @brief Appends a copy of a string to a string list.
 *
 * @param sl  string list
 * @param s   string to add
 */
void  sl_add(strlist_t *sl, const char *s);
/**
 * @brief Appends a string if it is not empty and not yet contained (case-insensitive).
 *
 * @param sl  string list
 * @param s   string to add
 * @return true if the string was added
 */
bool  sl_add_unique(strlist_t *sl, const char *s);     /* case-insensitive */
/**
 * @brief Checks whether a string list contains a string (case-insensitive).
 *
 * @param sl  string list
 * @param s   string to look for
 * @return true if found
 */
bool  sl_contains(const strlist_t *sl, const char *s); /* case-insensitive */
/**
 * @brief Frees all strings of a string list and the list itself.
 *
 * @param sl  string list
 */
void  sl_free(strlist_t *sl);

/* ------------------------------------------------------------------------ */
/* simple string -> pointer hash map (case-sensitive keys)                  */
/* ------------------------------------------------------------------------ */

/** @brief string -> pointer hash map (open addressing) */
typedef struct {
  char  **keys;  /**< key slots (NULL: free) */
  void  **vals;  /**< value slots */
  size_t  cap;  /**< number of slots (power of two) */
  size_t  count;  /**< number of keys */
} strmap_t;

/**
 * @brief Inserts or replaces a value in a string map.
 *
 * @param m    map
 * @param key  key (copied)
 * @param val  value
 */
void   sm_put(strmap_t *m, const char *key, void *val);
/**
 * @brief Looks up a key in a string map.
 *
 * @param m    map
 * @param key  key
 * @return the value or NULL
 */
void  *sm_get(const strmap_t *m, const char *key);
/**
 * @brief Frees the keys and the table of a string map (values are not freed).
 *
 * @param m  map
 */
void   sm_free(strmap_t *m);

/* ------------------------------------------------------------------------ */
/* string / path / file helpers                                             */
/* ------------------------------------------------------------------------ */

/**
 * @brief Converts UTF-8 to UTF-16 for the Windows wide-character APIs.
 *
 * @param s  UTF-8 string (NULL is treated as "")
 * @return newly allocated wide string
 */
wchar_t *utf8_to_wide(const char *s);                    /* malloc'ed */
/**
 * @brief Converts a UTF-16 string to UTF-8.
 *
 * @param ws  wide string (NULL is treated as L"")
 * @return newly allocated UTF-8 string
 */
char    *wide_to_utf8(const wchar_t *ws);                /* malloc'ed */
/**
 * @brief Converts a UTF-16 string to UTF-8 into a fixed buffer.
 *
 * @param ws   wide string
 * @param out  output buffer
 * @param cap  size of the output buffer
 */
void     wide_to_utf8_buf(const wchar_t *ws, char *out, size_t cap);

/**
 * @brief Checks a string prefix (case-sensitive).
 *
 * @param s       string
 * @param prefix  prefix
 * @return true if s starts with prefix
 */
bool     str_starts_with(const char *s, const char *prefix);
/**
 * @brief Checks a string prefix (case-insensitive).
 *
 * @param s       string
 * @param prefix  prefix
 * @return true if s starts with prefix
 */
bool     str_starts_with_ci(const char *s, const char *prefix);
/**
 * @brief Checks a string suffix (case-insensitive).
 *
 * @param s       string
 * @param suffix  suffix
 * @return true if s ends with suffix
 */
bool     str_ends_with_ci(const char *s, const char *suffix);
/**
 * @brief Checks whether a string contains a substring (case-insensitive).
 *
 * @param hay     string to search in
 * @param needle  substring
 * @return true if found
 */
bool     str_contains_ci(const char *hay, const char *needle);
/**
 * @brief Removes leading and trailing whitespace in place.
 *
 * @param s  string (modified)
 * @return pointer to the first non-blank character
 */
char    *str_trim(char *s);                              /* in place */
/**
 * @brief Converts ASCII letters to lower case in place.
 *
 * @param s  string (modified)
 */
void     str_lower(char *s);
/**
 * @brief Converts ASCII letters to upper case in place.
 *
 * @param s  string (modified)
 */
void     str_upper(char *s);
/**
 * @brief strncpy() that always NUL-terminates.
 *
 * @param dst  destination
 * @param cap  size of the destination
 * @param src  source (NULL copies "")
 */
void     str_copy(char *dst, size_t cap, const char *src);

/**
 * @brief Joins two path components with a backslash.
 *
 * @param out  output buffer
 * @param cap  size of the output buffer
 * @param a    first component
 * @param b    second component
 */
void     path_join(char *out, size_t cap, const char *a, const char *b);
/**
 * @brief Returns the file name part of a path ('/' and '\\' are separators).
 *
 * @param p  path
 * @return pointer into p behind the last separator
 */
const char *path_basename(const char *p);
/**
 * @brief Returns the directory part of a path ("." if there is none).
 *
 * @param p    path
 * @param out  output buffer
 * @param cap  size of the output buffer
 */
void     path_dirname(const char *p, char *out, size_t cap);
/**
 * @brief Replaces '/' by '\\' in place.
 *
 * @param p  path (modified)
 */
void     path_normalize_slashes(char *p);                /* '/' -> '\\'          */
/**
 * @brief Checks whether a regular file exists.
 *
 * @param path  UTF-8 path
 * @return true for an existing file
 */
bool     file_exists(const char *path);
/**
 * @brief Checks whether a directory exists.
 *
 * @param path  UTF-8 path
 * @return true for an existing directory
 */
bool     dir_exists(const char *path);
/**
 * @brief Creates a directory including all missing parents.
 *
 * @param path  UTF-8 path
 * @return true on success or if the directory exists
 */
bool     mkdir_p(const char *path);
/**
 * @brief Reads a whole file into memory; the buffer is NUL-terminated twice (also usable as UTF-16 text).
 *
 * @param path     UTF-8 path
 * @param out_len  receives the file size (may be NULL)
 * @return newly allocated buffer or NULL
 */
uint8_t *read_file(const char *path, size_t *out_len);   /* NUL-terminated */
/**
 * @brief Writes a buffer to a file (truncates an existing file).
 *
 * @param path  UTF-8 path
 * @param data  data
 * @param len   number of bytes
 * @return true on success
 */
bool     write_file(const char *path, const void *data, size_t len);
/**
 * @brief Copies a file (overwrites the destination).
 *
 * @param src  source path
 * @param dst  destination path
 * @return true on success
 */
bool     copy_file(const char *src, const char *dst);
/**
 * @brief Returns the size of a file.
 *
 * @param path  UTF-8 path
 * @return size in bytes or -1
 */
int64_t  file_size(const char *path);
/**
 * @brief Computes the SHA-256 digest of a file (Windows CNG).
 *
 * @param path     UTF-8 path
 * @param out_hex  receives 64 hex digits and a NUL
 * @return true on success
 */
bool     sha256_file(const char *path, char out_hex[65]);
/**
 * @brief Runs a command line without window, stdout/stderr discarded.
 *
 * @param cmdline     complete command line
 * @param cwd         working directory (NULL: current)
 * @param timeout_ms  timeout in milliseconds
 * @return exit code, -1 if it could not be started, -2 on timeout
 */
int      run_process(const char *cmdline, const char *cwd, DWORD timeout_ms);

/** walk_dir() callback: return false to skip a directory or to stop at a file */
typedef bool (*walk_cb_t)(const char *full_path, const char *name, bool is_dir, void *ctx);
/**
 * @brief Walks a directory; reparse points (junctions) are not followed.
 *
 * @param dir        directory
 * @param recursive  descend into subdirectories
 * @param cb         callback; for directories, returning false skips the directory, for files it stops the walk
 * @param ctx        user context
 */
void     walk_dir(const char *dir, bool recursive, walk_cb_t cb, void *ctx);

/**
 * @brief Writes a string as JSON string literal (UTF-8 is passed through).
 *
 * @param fp  output stream
 * @param s   string (NULL writes "")
 */
void     json_escape_fp(FILE *fp, const char *s);

#endif /* A2D_COMMON_H */
