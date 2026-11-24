/**
 * @file   main.c
 * @author Ingo A. Kubbilun
 * @brief  main function of the ACPI to Device Tree 'draft' tool
 *
 * This program runs on MS Windows 11 on ARM64. It pulls in all available
 * hardware sources:
 * (a) Windows driver store inside C:\Windows\System32; driver inf files,
 *     binary firmwares, etc.
 * (b) Windows device manager information
 * (c) optionally, one or more existing device trees of 'similar' ARM64 notebooks
 *     as primary, secondary, tertiary, etc. device trees for information extraction
 *     of these
 * (d) the ACPI tables available on the host running this tool (main table is DSDT)
 * 
 * It then 'drafts' a device tree for the current notebook, which has to be manually
 * edited afterwards by a human being, an AI, or both.
 * 
 * [MIT license]
 *
 * Copyright (c) 2026 Ingo A. Kubbilun
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * claim: "flat ASN.1 processing was y'day"
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <io.h>
#include <Windows.h>

#define VERSION_STRING          "v0.1"

#define CTRL_RESET              "\033[0;0;0m"
#define CTRL_RED                "\033[1;31m"
#define CTRL_GREEN              "\033[1;32m"
#define CTRL_YELLOW             "\033[1;33m"
#define CTRL_BLUE               "\033[1;34m"
#define CTRL_MAGENTA            "\033[1;35m"
#define CTRL_CYAN               "\033[1;36m"

static int    reset_console = 0;
static DWORD  dwMode = 0;
static HANDLE hConsole = INVALID_HANDLE_VALUE;

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
  switch (fdwCtrlType)
  {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
      if (0 != reset_console)
      {
        if (INVALID_HANDLE_VALUE != hConsole)
        {
          dwMode &= ~ENABLE_VIRTUAL_TERMINAL_PROCESSING;
          SetConsoleMode(hConsole, dwMode);
          reset_console = 0;
        }
      }
      break;

    default:
      break;
  }

  return FALSE;
}

void FPRINTF(FILE* f, const char* fmt, ...)
{
  va_list         ap;
  char            buffer[2048];
  uint32_t        src = 0, dst = 0;

  va_start(ap, fmt);
  (void)vsnprintf(buffer, sizeof(buffer), fmt, ap);
  va_end(ap);

  if (0 == reset_console || (stdout != f && stderr != f)) // no colors
  {
    while (0 != buffer[src])
    {
      if (('\033' != buffer[src]) || ('[' != buffer[src + 1]))
        buffer[dst++] = buffer[src++];
      else
        src += ('0' == buffer[src + 2]) ? 8 : 7;
    }

    buffer[dst] = 0x00;
  }

  fprintf(f, "%s", buffer);
}

static void ShowHelp(const char *progname)
{
  FPRINTF(stdout, CTRL_YELLOW "usage:" CTRL_RESET " %s <output dts file> <output firmware folder> <input linux kernel dts folder>\n", progname);
  FPRINTF(stdout, CTRL_YELLOW "------" CTRL_RESET " [<similar DTS file>][<similar DTS file>]...\n\n");

  FPRINTF(stdout, "  - scans Windows device manager, enumerates binary firmwares, reads ACPI tables\n");
  FPRINTF(stdout, "  - creates a Device Tree proposal (output DTS file)\n");
  FPRINTF(stdout, "  - creates a binary firmware folder structure (under /lib/firmware in target Linux)\n");
  FPRINTF(stdout, "  - uses files in <input linux kernel dts folder>\n");
  FPRINTF(stdout, "  - optionally, you MAY specify one or more existing DTS files of 'similar' notebooks\n");
  FPRINTF(stdout, "    => the first similar DTS file has the highest priority, next files have lower\n");
  FPRINTF(stdout, "       priorities. These files are scanned and checked for additional Device Tree components.\n\n");
}

int main(int argc, char* argv[])
{
  int         fd_out          = _fileno(stdout);
  int         fd_err          = _fileno(stderr);
  int         rc              = 1;
  bool        stdout_is_tty   = (fd_out >= 0) && (_isatty(fd_out) != 0);
  bool        stderr_is_tty   = (fd_err >= 0) && (_isatty(fd_err) != 0);
  const char *output_dts_file;
  const char *output_fw_folder;
  const char *input_linux_kernel_dts_folder;

  // only enable colored text output if stdout/stderr not redirected to file

  if (stdout_is_tty && stderr_is_tty)
  {
    hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(hConsole, &dwMode))
    {
      dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
      SetConsoleMode(hConsole, dwMode);
      reset_console = 1;
    }
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
  }

  FPRINTF(stdout, CTRL_CYAN "ACPI2DT Device Tree draft tool " CTRL_GREEN VERSION_STRING CTRL_RESET "\n\n");

  if (argc < 4)
  {
    ShowHelp(argv[0]);
    goto DoExit;
  }

  output_dts_file = argv[1];
  output_fw_folder = argv[2];
  input_linux_kernel_dts_folder = argv[3];

  // argv[4], argv[5], ... contain optional DTS files of 'similar' ARM64 notebooks

  // Claude: source code missing here




  // END OF MAIN, set result code to zero (0) now:

  rc = 0;

DoExit:

  if (0 != reset_console)
  {
    dwMode &= ~ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hConsole, dwMode);
  }

  SetConsoleCtrlHandler(CtrlHandler, FALSE);

  return rc;
}

#if 0

OLD MAIN WAS:

woa_config_t cfg;
ZeroMemory(&cfg, sizeof(cfg));
wcscpy(cfg.output_dir, L"C:\\woa-extract");
wcscpy(cfg.iasl_exe, L"C:\\src\\acpica\\iasl.exe");
wcscpy(cfg.dtc_exe, L"C:\\src\\dtc\\dtc.exe");
cfg.scan_driver_store = true;
cfg.extract_fw_strings = true;

for (int i = 1; i < argc; ++i) {
  if (wcscmp(argv[i], L"--out") == 0 && i + 1 < argc) {
    wcsncpy(cfg.output_dir, argv[++i], WOA_MAX_PATH - 1);
  }
  else if (wcscmp(argv[i], L"--iasl") == 0 && i + 1 < argc) {
    wcsncpy(cfg.iasl_exe, argv[++i], WOA_MAX_PATH - 1);
  }
  else if (wcscmp(argv[i], L"--dtc") == 0 && i + 1 < argc) {
    wcsncpy(cfg.dtc_exe, argv[++i], WOA_MAX_PATH - 1);
  }
  else if (wcscmp(argv[i], L"--primary") == 0 && i + 1 < argc) {
    wcsncpy(cfg.primary_dts, argv[++i], WOA_MAX_PATH - 1);
  }
  else if (wcscmp(argv[i], L"--secondary") == 0 && i + 1 < argc) {
    wcsncpy(cfg.secondary_dts, argv[++i], WOA_MAX_PATH - 1);
  }
}

woa_extract_acpi_tables(&cfg);
woa_explore_device_manager(&cfg);
woa_scan_drivers_and_firmware(&cfg);
woa_merge_and_validate_dts(&cfg);

wprintf(L"\n[Done] Artifacts written to: %s\n", cfg.output_dir);

#endif



