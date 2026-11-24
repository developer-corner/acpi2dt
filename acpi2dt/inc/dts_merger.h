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

typedef struct {
  const a2d_config_t     *cfg;
  const acpi_table_set_t *tables;
  const sysinfo_t        *si;
  acpi_ns_t              *ns;
  dm_set_t               *dm;
  drv_db_t               *drv;
  fw_plan_t              *fw;
} a2d_ctx_t;

typedef struct {
  int confirmed;        /* template nodes confirmed by ACPI                  */
  int not_in_acpi;      /* template bus devices without ACPI counterpart      */
  int mismatches;       /* GPIO / address mismatches                          */
  int added_acpi;       /* nodes generated from ACPI                          */
  int added_similar;    /* nodes taken over from secondary similar DTS        */
  int commented;        /* commented-out suggestions                          */
  int warnings;         /* unresolved labels etc.                             */
  int firmware;         /* firmware-name entries resolved                     */
} dts_stats_t;

int dts_generate(a2d_ctx_t *x, dts_stats_t *stats);

#endif /* A2D_DTS_MERGER_H */
