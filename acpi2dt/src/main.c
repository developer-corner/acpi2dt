/* SPDX-License-Identifier: MIT */
/**
 * @file   main.c
 * @author Ingo A. Kubbilun
 * @brief  main function of the ACPI to Device Tree 'draft' tool
 *
 * This program runs on MS Windows 11 on ARM64. It pulls in all available
 * hardware sources:
 * (a) Windows driver store inside C:\\Windows\\System32; driver inf files,
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

#include "common.h"
#include "acpi_extractor.h"
#include "aml_parser.h"
#include "devmgr_explorer.h"
#include "inf_firmware_scanner.h"
#include "dts_merger.h"
#include "acpica_host.h"

/**
 * @brief Prints the usage.
 *
 * @param progname  program name
 */
static void ShowHelp(const char *progname)
{
  FPRINTF(stdout, CTRL_YELLOW "usage:" CTRL_RESET " %s [options] <output dts file> <output firmware folder> <input linux kernel dts folder>\n", progname);
  FPRINTF(stdout, CTRL_YELLOW "------" CTRL_RESET " [<similar DTS file>][<similar DTS file>]...\n\n");

  FPRINTF(stdout, "  - scans Windows device manager, enumerates binary firmwares, reads ACPI tables\n");
  FPRINTF(stdout, "  - creates a Device Tree proposal (output DTS file)\n");
  FPRINTF(stdout, "  - creates a binary firmware folder structure (under /lib/firmware in target Linux)\n");
  FPRINTF(stdout, "  - uses files in <input linux kernel dts folder> (arch/arm64/boot/dts or .../dts/qcom)\n");
  FPRINTF(stdout, "  - optionally, you MAY specify one or more existing DTS files of 'similar' notebooks\n");
  FPRINTF(stdout, "    => the first similar DTS file has the highest priority, next files have lower\n");
  FPRINTF(stdout, "       priorities. These files are scanned and checked for additional Device Tree components.\n\n");

  FPRINTF(stdout, CTRL_YELLOW "options:" CTRL_RESET "\n");
  FPRINTF(stdout, "  --work-dir <dir>      intermediate files (default: <output dts>.acpi2dt)\n");
  FPRINTF(stdout, "  --acpi-dir <dir>      read ACPI tables from a dump folder instead of this machine\n");
  FPRINTF(stdout, "  --iasl <exe>          use an external iasl / AslCompiler.exe for the .dsl files\n");
  FPRINTF(stdout, "                        (default: embedded ACPICA disassembler)\n");
  FPRINTF(stdout, "  --no-iasl             do not disassemble the ACPI tables\n");
  FPRINTF(stdout, "  --no-acpica           no embedded AML interpreter (static AML analysis only)\n");
  FPRINTF(stdout, "  --no-dtc              do not compile the draft to a DTB (embedded dtc)\n");
  FPRINTF(stdout, "  --soc <name>          override SoC detection, e.g. x1p42100, x1e80100\n");
  FPRINTF(stdout, "  --model <string>      override root 'model'\n");
  FPRINTF(stdout, "  --compatible <str>    override board compatible, e.g. asus,vivobook-s16-s3607qa\n");
  FPRINTF(stdout, "  --vendor-dir <name>   firmware vendor directory (e.g. ASUSTeK)\n");
  FPRINTF(stdout, "  --board-slug <name>   firmware board directory (e.g. vivobook-s16)\n");
  FPRINTF(stdout, "  --no-devmgr           do not query the Windows Device Manager\n");
  FPRINTF(stdout, "  --no-drivers          do not scan driver packages / firmware\n");
  FPRINTF(stdout, "  --no-fw-copy          resolve firmware but do not copy it\n");
  FPRINTF(stdout, "  --full-driverstore    index the complete DriverStore up front\n");
  FPRINTF(stdout, "  -v, --verbose         verbose output\n");
  FPRINTF(stdout, "  -h, --help            this help\n\n");

  FPRINTF(stdout, CTRL_YELLOW "example:" CTRL_RESET "\n");
  FPRINTF(stdout, "  %s s16-draft.dts fw D:\\linux\\arch\\arm64\\boot\\dts x1e80100-asus-vivobook-s15.dts x1p42100-asus-zenbook-a14.dts\n\n", progname);
}

/**
 * @brief Parses the command line.
 *
 * @param argc  argument count
 * @param argv  argument vector
 * @param cfg   receives the configuration
 * @return false if the usage is to be shown
 */
static bool parse_args(int argc, char *argv[], a2d_config_t *cfg)
{
  const char **pos = calloc((size_t)argc + 1, sizeof(char *));
  int          npos = 0;

  if (!pos)
    return false;
  for (int i = 1; i < argc; i++)
  {
    const char *a = argv[i];
/** fails parse_args() if option a has no argument */
#define NEEDS_ARG()                                                          \
  if (i + 1 >= argc)                                                         \
  {                                                                          \
    log_err("option %s needs an argument", a);                               \
    free(pos);                                                               \
    return false;                                                            \
  }
    if (!strcmp(a, "-h") || !strcmp(a, "--help"))
    {
      free(pos);
      return false;
    }
    else if (!strcmp(a, "-v") || !strcmp(a, "--verbose"))
      cfg->verbose = true;
    else if (!strcmp(a, "--work-dir"))       { NEEDS_ARG(); str_copy(cfg->work_dir, sizeof(cfg->work_dir), argv[++i]); }
    else if (!strcmp(a, "--acpi-dir"))       { NEEDS_ARG(); cfg->acpi_dir = argv[++i]; }
    else if (!strcmp(a, "--iasl"))           { NEEDS_ARG(); cfg->iasl_exe = argv[++i]; }
    else if (!strcmp(a, "--no-iasl"))        cfg->no_iasl = true;
    else if (!strcmp(a, "--no-acpica"))      cfg->no_acpica = true;
    else if (!strcmp(a, "--no-dtc"))         cfg->no_dtc = true;
    else if (!strcmp(a, "--soc"))            { NEEDS_ARG(); cfg->soc = argv[++i]; }
    else if (!strcmp(a, "--model"))          { NEEDS_ARG(); cfg->model = argv[++i]; }
    else if (!strcmp(a, "--compatible"))     { NEEDS_ARG(); cfg->compatible = argv[++i]; }
    else if (!strcmp(a, "--vendor-dir"))     { NEEDS_ARG(); cfg->vendor_dir = argv[++i]; }
    else if (!strcmp(a, "--board-slug"))     { NEEDS_ARG(); cfg->board_slug = argv[++i]; }
    else if (!strcmp(a, "--no-devmgr"))      cfg->no_devmgr = true;
    else if (!strcmp(a, "--no-drivers"))     cfg->no_drivers = true;
    else if (!strcmp(a, "--no-fw-copy"))     cfg->no_fw_copy = true;
    else if (!strcmp(a, "--full-driverstore")) cfg->full_driverstore = true;
    else if (a[0] == '-' && a[1] == '-')
    {
      log_err("unknown option %s", a);
      free(pos);
      return false;
    }
    else
      pos[npos++] = a;
#undef NEEDS_ARG
  }
  if (npos < 3)
  {
    free(pos);
    return false;
  }
  cfg->output_dts = pos[0];
  cfg->output_fw_dir = pos[1];
  cfg->kernel_dts_dir = pos[2];
  cfg->similar_dts = pos + 3;      /* argv strings stay valid; array leaks intentionally */
  cfg->num_similar = npos - 3;
  if (!cfg->work_dir[0])
  {
    char base[A2D_MAX_PATH];
    char *dot;
    str_copy(base, sizeof(base), cfg->output_dts);
    dot = strrchr(base, '.');
    if (dot && !strpbrk(dot, "\\/"))
      *dot = 0;
    snprintf(cfg->work_dir, sizeof(cfg->work_dir), "%s.acpi2dt", base);
  }
  return true;
}

/**
 * @brief acpi2dt entry point: collects all sources, drafts the DTS, copies firmware, prints a summary.
 *
 * @param argc  argument count
 * @param argv  argument vector
 * @return 0 on success, 1 on error
 */
int main(int argc, char *argv[])
{
  int               rc = 1;
  a2d_config_t      cfg;
  acpi_table_set_t  tables;
  sysinfo_t         si;
  acpi_ns_t         ns;
  dm_set_t          dm;
  drv_db_t          drv;
  fw_plan_t         fw;
  a2d_ctx_t         ctx;
  dts_stats_t       st;

  memset(&cfg, 0, sizeof(cfg));
  memset(&dm, 0, sizeof(dm));
  memset(&drv, 0, sizeof(drv));
  memset(&fw, 0, sizeof(fw));
  memset(&tables, 0, sizeof(tables));
  memset(&ns, 0, sizeof(ns));

  console_init();

  FPRINTF(stdout, CTRL_CYAN "ACPI2DT Device Tree draft tool " CTRL_GREEN A2D_VERSION_STRING CTRL_RESET "\n\n");

  if (!parse_args(argc, argv, &cfg))
  {
    ShowHelp(argv[0]);
    goto DoExit;
  }
  log_set_verbose(cfg.verbose);

  if (!dir_exists(cfg.kernel_dts_dir))
  {
    log_err("Linux kernel DTS folder %s does not exist", cfg.kernel_dts_dir);
    goto DoExit;
  }
  for (int i = 0; i < cfg.num_similar; i++)
    if (!file_exists(cfg.similar_dts[i]))
    {
      log_err("similar DTS file %s does not exist", cfg.similar_dts[i]);
      goto DoExit;
    }
  if (!mkdir_p(cfg.work_dir))
  {
    log_err("cannot create work folder %s", cfg.work_dir);
    goto DoExit;
  }

  /* 1. ACPI */
  log_step("reading ACPI tables");
  if (0 == acpi_collect_tables(&cfg, &tables))
    log_warn("no ACPI tables - the draft will be based on DTS files only");

  /* 2. machine identity */
  log_step("identifying the machine (SMBIOS, CPU, MADT)");
  sysinfo_collect(&cfg, &tables, &si);
  log_info("machine : %s / %s (board %s, BIOS %s)", si.manufacturer, si.product, si.board_product, si.bios_version);
  log_info("CPU     : %s, %d/%d cores enabled", si.cpu_name, si.madt_gicc_enabled, si.madt_gicc_total);
  if (si.soc[0])
    log_info("SoC     : " CTRL_GREEN "%s" CTRL_RESET " (%s)", si.soc, si.soc_reason);
  else
    log_warn("SoC could not be determined - use --soc");
  log_info("DT      : model \"%s\", compatible \"%s\"", si.model, si.compatible);
  log_info("firmware: qcom/%s/%s/%s/", si.soc, si.vendor_dir, si.board_slug);

  /* 3. AML namespace */
  log_step("parsing the ACPI namespace (DSDT/SSDT)");
  aml_parse_all(&tables, &ns);
  {
    int with_hid = 0, with_res = 0;
    for (int i = 0; i < ns.count; i++)
    {
      if (ns.d[i].hids.count)
        with_hid++;
      if (ns.d[i].nres)
        with_res++;
    }
    log_info("%d devices (%d with _HID, %d with decoded _CRS resources)", ns.count, with_hid, with_res);
    if (ns.parse_errors)
      log_warn("%d AML block(s) could not be parsed completely (use -v)", ns.parse_errors);
  }

  /* 3b. embedded ACPICA: exact evaluation where possible, .dsl output */
  if (!cfg.no_acpica && tables.count)
  {
    char log[A2D_MAX_PATH], dsl_dir[A2D_MAX_PATH];
    log_step("evaluating AML with the embedded ACPICA %s (simulated hardware)", acpica_version());
    path_join(log, sizeof(log), cfg.work_dir, "acpica.log");
    if (acpica_load(&tables, log))
    {
      acpica_stats_t as;
      acpica_evaluate(&ns, &as);
      log_info("%d devices: %d objects evaluated exactly, %d depend on firmware data, %d failed", as.devices,
               as.exact, as.firmware, as.failed);
      log_info("_HID determined for %d devices, _CRS replaced by the evaluated result for %d", as.hid_updated,
               as.crs_updated);
      if (!cfg.no_iasl && !cfg.iasl_exe)
      {
        int n;
        path_join(dsl_dir, sizeof(dsl_dir), cfg.work_dir, "acpi");
        n = acpica_disassemble(&tables, dsl_dir);
        log_info("disassembled %d DSDT/SSDT table(s) to %s\\*.dsl", n, dsl_dir);
      }
    }
    else
      log_warn("ACPICA could not load the tables (see %s) - static analysis only", log);
    acpica_shutdown();
  }

  /* 4. Device Manager */
  if (!cfg.no_devmgr)
  {
    int linked = 0;
    log_step("enumerating the Windows Device Manager");
    devmgr_collect(&cfg, &dm);
    for (int i = 0; i < dm.count; i++)
      if (dm.d[i].bios_name[0] && aml_find_device(&ns, dm.d[i].bios_name))
        linked++;
    log_info("%d present devices, %d linked to ACPI namespace objects", dm.count, linked);
    {
      const dm_monitor_t *p = devmgr_internal_panel(&dm);
      if (p)
        log_info("internal panel (EDID): %s 0x%04x \"%s\" %.1f\"%s", p->pnp_id, p->product,
                 p->name[0] ? p->name : p->text[0], p->diagonal_in, p->present ? "" : " (currently not active)");
      else
        log_info("internal panel: not found among %d known monitors", dm.nmon);
    }
    if (cfg.acpi_dir)
      log_warn("ACPI tables come from %s: Device Manager data is from THIS machine", cfg.acpi_dir);
  }

  /* 5. drivers / firmware */
  if (!cfg.no_drivers)
  {
    log_step("scanning driver packages and firmware (DriverStore)");
    drivers_scan(&cfg, cfg.no_devmgr ? NULL : &dm, &drv);
  }

  /* 6. DTS */
  log_step("drafting the device tree");
  memset(&ctx, 0, sizeof(ctx));
  ctx.cfg = &cfg;
  ctx.tables = &tables;
  ctx.si = &si;
  ctx.ns = &ns;
  ctx.dm = cfg.no_devmgr ? NULL : &dm;
  ctx.drv = cfg.no_drivers ? NULL : &drv;
  ctx.fw = &fw;
  if (0 != dts_generate(&ctx, &st))
    goto Cleanup;

  /* 7. firmware */
  log_step("firmware");
  if (fw.count)
  {
    int copied = fw_plan_execute(&cfg, &fw);
    if (cfg.no_fw_copy)
      log_info("%d firmware file(s) resolved (not copied, --no-fw-copy)", fw.count);
    else
      log_info("%d of %d firmware file(s) copied to %s", copied, fw.count, cfg.output_fw_dir);
  }
  else
    log_info("no firmware resolved");

  /* summary */
  log_step("summary");
  log_ok("draft written to " CTRL_GREEN "%s" CTRL_RESET, cfg.output_dts);
  log_info("confirmed by ACPI        : %d", st.confirmed);
  if (st.not_in_acpi)
    log_warn("template devices missing in ACPI : %d (marked WARNING)", st.not_in_acpi);
  if (st.mismatches)
    log_warn("GPIO / property mismatches       : %d (marked WARNING)", st.mismatches);
  if (st.warnings)
    log_warn("unresolved references            : %d", st.warnings);
  log_info("added from ACPI          : %d", st.added_acpi);
  log_info("added from similar DTS   : %d", st.added_similar);
  log_info("commented suggestions    : %d", st.commented);
  log_info("firmware-name resolved   : %d", st.firmware);
  if (st.dtb_built)
    log_ok("DTB compiled (embedded dtc): " CTRL_GREEN "%s" CTRL_RESET " (%d dtc warnings)", st.dtb_path,
           st.dtb_warnings);
  else if (!cfg.no_dtc)
    log_warn("DTB NOT compiled: %d error(s) - see %s\\dtc.log", st.dtb_errors, cfg.work_dir);
  log_info("intermediate files       : %s", cfg.work_dir);
  FPRINTF(stdout, "\n" CTRL_YELLOW "Review the draft carefully - search for WARNING, CHECK and TODO." CTRL_RESET "\n");

  // END OF MAIN, set result code to zero (0) now:

  rc = 0;

Cleanup:
  fw_plan_free(&fw);
  drivers_free(&drv);
  devmgr_free(&dm);
  aml_free(&ns);
  acpi_free_tables(&tables);

DoExit:

  console_restore();

  return rc;
}
