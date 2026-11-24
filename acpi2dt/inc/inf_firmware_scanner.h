/**
 * @file   inf_firmware_scanner.h
 * @author Ingo A. Kubbilun
 * @brief  driver package (.inf) and firmware blob scanner (DriverStore),
 *         firmware resolution and copy plan (Linux /lib/firmware layout)
 *
 * [MIT license] - see main.c
 */

#ifndef A2D_INF_FIRMWARE_SCANNER_H
#define A2D_INF_FIRMWARE_SCANNER_H

#include "common.h"
#include "devmgr_explorer.h"

typedef struct {
  char      *path;
  char      *name;
  char      *pkg;            /* driver package folder name                */
  int64_t    size;
  bool       present_pkg;    /* belongs to a driver of a present device   */
  char       format[32];     /* "ELF64 AArch64", "ELF32 Hexagon", "MBN"... */
} fw_file_t;

typedef struct {
  char       inf[64];        /* published name (oemNN.inf)                */
  char       original_inf[128];
  char       dir[A2D_MAX_PATH];
  char       provider[128];
  char       cls[64];
  char       driver_ver[96];
  strlist_t  devices;        /* friendly names of devices using it        */
  strlist_t  firmware;       /* firmware-like files listed by the INF     */
} drv_pkg_t;

typedef struct {
  drv_pkg_t *p;
  int        np, cap_p;
  fw_file_t *f;
  int        nf, cap_f;
  bool       full_indexed;
  strmap_t   seen;           /* lower-case paths already indexed          */
} drv_db_t;

typedef struct {
  char       target_rel[256]; /* relative to the firmware output folder   */
  char       source[A2D_MAX_PATH];
  char       reason[256];
  bool       copied;
  int64_t    size;
  char       sha256[65];
} fw_copy_t;

typedef struct {
  fw_copy_t *c;
  int        count, cap;
} fw_plan_t;

int  drivers_scan(const a2d_config_t *cfg, const dm_set_t *dm, drv_db_t *db);
void drivers_free(drv_db_t *db);
const drv_pkg_t *drivers_pkg_by_dir(const drv_db_t *db, const char *dir);

/* resolves a firmware file wanted by a (similar) DTS on this machine.
   soc is used to prefer SoC-specific variants; why receives a description */
const fw_file_t *fw_resolve(drv_db_t *db, const char *wanted_name, const char *soc, char *why, size_t whycap);
/* all files whose name matches prefix*suffix (case-insensitive) */
int  fw_find_all(drv_db_t *db, const char *prefix, const char *suffix, const fw_file_t **out, int max);

void fw_plan_add(fw_plan_t *plan, const char *target_rel, const char *source, const char *reason);
int  fw_plan_execute(const a2d_config_t *cfg, fw_plan_t *plan);  /* copies + FIRMWARE-MANIFEST.txt */
void fw_plan_free(fw_plan_t *plan);

#endif /* A2D_INF_FIRMWARE_SCANNER_H */
