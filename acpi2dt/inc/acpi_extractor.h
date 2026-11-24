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

typedef struct {
  char      sig[5];
  char      oem_id[7];
  char      oem_table_id[9];
  uint32_t  oem_revision;
  uint8_t  *data;
  uint32_t  len;
  int       instance;          /* 0 for the first table of a signature, 1.. for more */
  char      file_name[64];     /* name of the .dat dump inside <work>\acpi           */
  char      origin[64];        /* "firmware API", "registry", "file <name>"          */
} acpi_table_t;

typedef struct {
  acpi_table_t *t;
  int           count;
  int           cap;
} acpi_table_set_t;

typedef struct {
  char  manufacturer[128];     /* SMBIOS type 1 */
  char  product[128];
  char  version[64];
  char  sku[64];
  char  family[128];
  char  board_vendor[128];     /* SMBIOS type 2 */
  char  board_product[64];
  char  bios_version[64];      /* SMBIOS type 0 */
  char  cpu_name[128];         /* registry ProcessorNameString */
  int   madt_gicc_total;
  int   madt_gicc_enabled;
  char  soc[32];               /* e.g. "x1p42100" */
  char  soc_reason[160];
  /* derived identity for the DTS */
  char  vendor_prefix[32];     /* "asus"          */
  char  vendor_dir[64];        /* "ASUSTeK"       */
  char  board_slug[96];        /* "vivobook-s16"  */
  char  model[192];            /* "ASUS Vivobook S16 (S3607QA)" */
  char  compatible[192];       /* "asus,vivobook-s16-s3607qa"   */
} sysinfo_t;

/* acquire all tables; dumps them to <work>\acpi and optionally disassembles
   them with iasl; returns number of tables (0 on failure) */
int  acpi_collect_tables(const a2d_config_t *cfg, acpi_table_set_t *set);
const acpi_table_t *acpi_find_table(const acpi_table_set_t *set, const char *sig, int instance);
void acpi_free_tables(acpi_table_set_t *set);

/* SMBIOS, CPU name, MADT, SoC detection, derived identity */
void sysinfo_collect(const a2d_config_t *cfg, const acpi_table_set_t *set, sysinfo_t *si);

#endif /* A2D_ACPI_EXTRACTOR_H */
