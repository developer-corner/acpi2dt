/* SPDX-License-Identifier: MIT */
/**
 * @file   dts_merger.h
 * @author Ingo A. Kubbilun
 * @brief  drafts the device tree: re-emits the primary similar DTS (template)
 *         annotated with evidence from ACPI / Device Manager / DriverStore,
 *         merges additional nodes from secondary similar DTS files and adds
 *         ACPI-only devices; also works without any similar DTS
 *
 * [MIT license] - see main.c
 */

#ifndef A2D_DTS_MERGER_H
#define A2D_DTS_MERGER_H

#include "common.h"
#include "acpi_extractor.h"
#include "aml_parser.h"
#include "devmgr_explorer.h"
#include "inf_firmware_scanner.h"

/** @brief all information sources handed to the DTS generator */
typedef struct {
  const a2d_config_t     *cfg;  /**< configuration */
  const acpi_table_set_t *tables;  /**< ACPI tables */
  const sysinfo_t        *si;  /**< machine identity */
  acpi_ns_t              *ns;  /**< ACPI devices */
  dm_set_t               *dm;  /**< Device Manager data (NULL with --no-devmgr) */
  drv_db_t               *drv;  /**< driver/firmware database (NULL with --no-drivers) */
  fw_plan_t              *fw;  /**< firmware copy plan */
} a2d_ctx_t;

/** @brief statistics of the DTS generator */
typedef struct {
  int confirmed;        /**< template nodes confirmed by ACPI */
  int not_in_acpi;      /**< template bus devices without ACPI counterpart */
  int mismatches;       /**< GPIO / address mismatches */
  int added_acpi;       /**< nodes generated from ACPI */
  int added_similar;    /**< nodes taken over from secondary similar DTS */
  int commented;        /**< commented-out suggestions */
  int warnings;         /**< unresolved labels etc. */
  int firmware;         /**< firmware-name entries resolved */
  bool dtb_built;       /**< embedded dtc produced a DTB */
  int  dtb_warnings;    /**< dtc warnings */
  int  dtb_errors;      /**< preprocessor / dtc errors */
  char dtb_path[A2D_MAX_PATH];  /**< path of the compiled DTB */
} dts_stats_t;

/**
 * @brief Drafts the device tree: template re-emission with annotations, merges, ACPI additions, appendix, DTB compilation.
 *
 * @param x      all information sources
 * @param stats  receives statistics
 * @return 0 on success, -1 if the output could not be written
 */
int dts_generate(a2d_ctx_t *x, dts_stats_t *stats);

#endif /* A2D_DTS_MERGER_H */
