/* SPDX-License-Identifier: MIT */
/**
 * @file   acpica_host.h
 * @author Ingo A. Kubbilun
 * @brief  embedded ACPICA: AML interpreter and disassembler for the
 *         extracted ACPI tables (no hardware access)
 *
 * [MIT license] - see main.c (the embedded ACPICA is used under its
 * BSD-3-Clause OR GPL-2.0-only license, see third_party/acpica)
 */

#ifndef A2D_ACPICA_HOST_H
#define A2D_ACPICA_HOST_H

#include "common.h"
#include "acpi_extractor.h"
#include "aml_parser.h"

/** statistics of acpica_evaluate() */
typedef struct {
  int devices;      /**< devices looked at                                    */
  int exact;        /**< objects evaluated without firmware data             */
  int firmware;     /**< objects whose result depends on firmware data       */
  int failed;       /**< evaluation errors                                   */
  int crs_updated;  /**< devices whose _CRS was replaced by the evaluated one */
  int hid_updated;  /**< devices whose _HID was determined exactly           */
} acpica_stats_t;

/**
 * @brief initializes ACPICA and loads the tables from memory
 *
 * Builds RSDP/XSDT around the tables (FADT patched to point to the DSDT),
 * installs simulated operation region handlers for all address spaces
 * (reads return 0 or the last value written; nothing touches hardware) and
 * loads the namespace. ACPICA's own messages go to @p log_path.
 *
 * @param set       ACPI tables (DSDT, SSDTs, FADT, ...)
 * @param log_path  file for ACPICA output
 * @return true if the namespace was loaded
 */
bool acpica_load(const acpi_table_set_t *set, const char *log_path);

/**
 * @brief evaluates _HID, _CID, _UID, _STA and _CRS of every device
 *
 * A result is "exact" if the evaluation did not access any operation region
 * (i.e. it does not depend on firmware data that only exists on the running
 * machine). Exact results replace the static heuristics of aml_parser.c;
 * results that depend on simulated firmware data are recorded but the
 * static candidates are kept.
 *
 * @param ns  namespace from aml_parse_all(), updated in place
 * @param st  statistics
 */
void acpica_evaluate(acpi_ns_t *ns, acpica_stats_t *st);

/**
 * @brief disassembles all DSDT/SSDT tables to ASL with ACPICA's disassembler
 *
 * The output is meant for reading; External() declarations and other
 * compiler-side refinements of "iasl -d" are not produced.
 *
 * @param set      ACPI tables
 * @param out_dir  folder for the .dsl files (named like the .dat dumps)
 * @return number of tables disassembled
 */
int acpica_disassemble(const acpi_table_set_t *set, const char *out_dir);

/** @brief shuts ACPICA down and closes the log */
void acpica_shutdown(void);

/**
 * @brief ACPICA version as string
 *
 * @return version, e.g. "20260930"
 */
const char *acpica_version(void);

#endif /* A2D_ACPICA_HOST_H */
