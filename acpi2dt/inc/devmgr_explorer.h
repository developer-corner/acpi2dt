/**
 * @file   devmgr_explorer.h
 * @author Ingo A. Kubbilun
 * @brief  Windows Device Manager (SetupAPI / CfgMgr32) enumeration
 *
 * [MIT license] - see main.c
 */

#ifndef A2D_DEVMGR_EXPLORER_H
#define A2D_DEVMGR_EXPLORER_H

#include "common.h"

typedef struct {
  char       instance_id[256];
  char       name[256];           /* friendly name or device description         */
  char       cls[64];             /* setup class                                 */
  strlist_t  hwids;
  strlist_t  cids;
  char       inf[64];             /* "oem42.inf"                                 */
  char       inf_section[128];
  char       provider[128];
  char       driver_version[64];
  char       service[64];
  char       location[256];
  char       bios_name[192];      /* canonical ACPI path "\_SB.TCPD" if ACPI-enumerated */
  char       driver_store_dir[A2D_MAX_PATH]; /* DriverStore\FileRepository\<pkg> */
  ULONG      status;
  ULONG      problem;             /* CM_PROB_*, 0 = OK                           */
  uint64_t   mem_base[8];
  uint64_t   mem_end[8];
  int        nmem;
  uint32_t   irqs[8];
  int        nirq;
} dm_dev_t;

typedef struct {
  dm_dev_t *d;
  int       count;
  int       cap;
} dm_set_t;

/* enumerates all present devices and writes <work>\hardware_inventory.json */
int  devmgr_collect(const a2d_config_t *cfg, dm_set_t *set);
void devmgr_free(dm_set_t *set);

const dm_dev_t *devmgr_find_by_acpi_path(const dm_set_t *set, const char *acpi_path);
/* matches "ACPI\HID" hardware IDs, e.g. hid = "QCOM0C10" (case-insensitive) */
const dm_dev_t *devmgr_find_by_hwid_substr(const dm_set_t *set, const char *substr);
const char     *devmgr_status_text(const dm_dev_t *d);

#endif /* A2D_DEVMGR_EXPLORER_H */
