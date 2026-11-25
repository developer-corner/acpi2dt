/* SPDX-License-Identifier: MIT */
/**
 * @file   acpi_extractor.h
 * @author Ingo A. Kubbilun
 * @brief  ACPI table acquisition (live machine or dump folder), SMBIOS / MADT
 *         system information and SoC detection
 *
 * [MIT license] - see main.c
 */

#ifndef A2D_ACPI_EXTRACTOR_H
#define A2D_ACPI_EXTRACTOR_H

#include "common.h"

/** @brief one ACPI table (copy of the firmware data) */
typedef struct {
  char      sig[5];  /**< signature (NUL-terminated) */
  char      oem_id[7];  /**< OEM ID (NUL-terminated) */
  char      oem_table_id[9];  /**< OEM table ID (NUL-terminated) */
  uint32_t  oem_revision;  /**< OEM revision */
  uint8_t  *data;  /**< table bytes */
  uint32_t  len;  /**< table length */
  int       instance;          /**< 0 for the first table of a signature, 1.. for more */
  char      file_name[64];     /**< name of the .dat dump inside \<work\>\\acpi */
  char      origin[64];        /**< "firmware API", "registry", "file \<name\>" */
} acpi_table_t;

/** @brief all acquired ACPI tables */
typedef struct {
  acpi_table_t *t;  /**< tables */
  int           count;  /**< number of tables */
  int           cap;  /**< capacity of t */
} acpi_table_set_t;

/** @brief machine identity: SMBIOS, CPU, MADT, SoC and the derived DT / firmware names */
typedef struct {
  char  manufacturer[128];     /**< SMBIOS type 1 */
  char  product[128];  /**< product name */
  char  version[64];  /**< product version */
  char  sku[64];  /**< SKU number */
  char  family[128];  /**< product family */
  char  board_vendor[128];     /**< SMBIOS type 2 */
  char  board_product[64];  /**< board product */
  char  bios_version[64];      /**< SMBIOS type 0 */
  char  cpu_name[128];         /**< registry ProcessorNameString */
  int   madt_gicc_total;  /**< GIC CPU interfaces in the MADT */
  int   madt_gicc_enabled;  /**< enabled GIC CPU interfaces */
  char  soc[32];               /**< e.g. "x1p42100" */
  char  soc_reason[160];  /**< how the SoC was determined */
  /* derived identity for the DTS */
  char  vendor_prefix[32];     /**< "asus" */
  char  vendor_dir[64];        /**< "ASUSTeK" */
  char  board_slug[96];        /**< "vivobook-s16" */
  char  model[192];            /**< "ASUS Vivobook S16 (S3607QA)" */
  char  compatible[192];       /**< "asus,vivobook-s16-s3607qa" */
} sysinfo_t;

/**
 * @brief Acquires all ACPI tables (dump folder or running machine), writes them to \<work\>\\acpi and optionally disassembles them with an external iasl.
 *
 * @param cfg  configuration
 * @param set  receives the tables
 * @return number of tables, 0 on failure
 */
int  acpi_collect_tables(const a2d_config_t *cfg, acpi_table_set_t *set);
/**
 * @brief Looks up a table by signature and instance.
 *
 * @param set       table set
 * @param sig       4-character signature
 * @param instance  0 for the first table of that signature, 1.. for more
 * @return the table or NULL
 */
const acpi_table_t *acpi_find_table(const acpi_table_set_t *set, const char *sig, int instance);
/**
 * @brief Frees all tables of a table set.
 *
 * @param set  table set
 */
void acpi_free_tables(acpi_table_set_t *set);

/**
 * @brief Collects SMBIOS, CPU name and MADT data, detects the SoC and derives model, compatible and firmware directories.
 *
 * @param cfg  configuration (overrides)
 * @param set  ACPI tables (MADT)
 * @param si   receives the system information
 */
void sysinfo_collect(const a2d_config_t *cfg, const acpi_table_set_t *set, sysinfo_t *si);

#endif /* A2D_ACPI_EXTRACTOR_H */
