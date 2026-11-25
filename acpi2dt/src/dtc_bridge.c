/* SPDX-License-Identifier: MIT */
/**
 * @file   dtc_bridge.c
 * @author Ingo A. Kubbilun
 * @brief  in-process device tree compiler (embedded dtc)
 *
 * The embedded dtc sources (third_party/dtc) are compiled with
 * third_party/dtc/dtc_port.h as forced include: dtc's main() becomes
 * dtc_main() and every exit() becomes dtc_exit(). This module calls dtc_main()
 * with "-I dts -O dtb -o \<dtb\> \<file\>" - i.e. dtc runs its unmodified
 * pipeline - and turns dtc_exit() into a longjmp() back to the caller, so
 * that fatal dtc errors do not terminate acpi2dt. stderr is redirected into a
 * log file while dtc runs.
 *
 * [MIT license] - see main.c (the embedded dtc itself is GPL-2.0-or-later)
 */

#include "dtc_bridge.h"
#include "dts_cpp.h"
#include <setjmp.h>
#include <io.h>
#include <fcntl.h>

/**
 * @brief dtc's main(), renamed by dtc_port.h
 *
 * @param argc  argument count
 * @param argv  argument vector
 * @return dtc exit code
 */
int dtc_main(int argc, char *argv[]);
extern int optind;                      /**< getopt state of the dtc port        */

static jmp_buf g_jump;                  /**< return point for dtc_exit()         */
static bool    g_active;                /**< dtc_main() is running               */
static int     g_exit_code;             /**< code passed to dtc_exit()           */

/**
 * @brief replacement for exit() inside the embedded dtc
 * @param code exit code requested by dtc
 */
__declspec(noreturn) void dtc_exit(int code)
{
  if (g_active)
  {
    g_exit_code = code;
    longjmp(g_jump, 1);
  }
  exit(code);
}

void dtc_result_free(dtc_result_t *res)
{
  sl_free(&res->messages);
}

/**
 * @brief Counts warnings and errors of a dtc log and keeps its lines.
 *
 * @param log_path  log file
 * @param res       result (messages and counters updated)
 */
static void read_log(const char *log_path, dtc_result_t *res)
{
  size_t len;
  char  *txt = (char *)read_file(log_path, &len);
  char  *line, *ctx = NULL;

  if (!txt)
    return;
  for (line = strtok_s(txt, "\r\n", &ctx); line; line = strtok_s(NULL, "\r\n", &ctx))
  {
    if (strstr(line, "Warning"))
      res->warnings++;
    else if (strstr(line, "ERROR") || strstr(line, "Error"))
      res->errors++;
    sl_add(&res->messages, line);
  }
  free(txt);
}

bool dtc_compile(const char *dts_path, const char **inc_dirs, int ninc, const char *pre_path, const char *dtb_path,
                 const char *log_path, dtc_result_t *res)
{
  strbuf_t pre;
  char    *argv[8];
  int      saved_err, log_fd;
  FILE    *logf;

  memset(res, 0, sizeof(*res));

  /* 1. preprocess */
  sb_init(&pre);
  res->preprocessed = dts_preprocess(dts_path, inc_dirs, ninc, &pre, &res->messages);
  res->errors += res->messages.count;
  if (!write_file(pre_path, pre.buf ? pre.buf : "", pre.len))
  {
    sl_add(&res->messages, "cannot write the preprocessed source");
    sb_free(&pre);
    return false;
  }
  sb_free(&pre);
  if (!res->preprocessed)
    return false;

  /* 2. dtc with stderr redirected to the log file */
  logf = fopen(log_path, "wb");
  if (!logf)
    return false;
  fflush(stdout);
  fflush(stderr);
  saved_err = _dup(_fileno(stderr));
  log_fd = _fileno(logf);
  _dup2(log_fd, _fileno(stderr));

  argv[0] = "dtc";
  argv[1] = "-I";
  argv[2] = "dts";
  argv[3] = "-O";
  argv[4] = "dtb";
  argv[5] = "-o";
  argv[6] = (char *)dtb_path;
  argv[7] = (char *)pre_path;
  optind = 1;
  g_exit_code = 0;
  g_active = true;
  if (setjmp(g_jump) == 0)
    res->exit_code = dtc_main(8, argv);
  else
    res->exit_code = g_exit_code;
  g_active = false;

  fflush(stderr);
  _dup2(saved_err, _fileno(stderr));
  _close(saved_err);
  fclose(logf);

  read_log(log_path, res);
  res->dtb_size = file_size(dtb_path);
  res->compiled = res->exit_code == 0 && res->dtb_size > 0;
  return res->compiled;
}
