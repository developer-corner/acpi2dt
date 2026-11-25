/* SPDX-License-Identifier: MIT */
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

/** @brief one present Device Manager device */
typedef struct {
  char       instance_id[256];  /**< device instance ID */
  char       name[256];           /**< friendly name or device description */
  char       cls[64];             /**< setup class */
  strlist_t  hwids;  /**< hardware IDs */
  strlist_t  cids;  /**< compatible IDs */
  char       inf[64];             /**< "oem42.inf" */
  char       inf_section[128];  /**< INF install section */
  char       provider[128];  /**< driver provider */
  char       driver_version[64];  /**< driver version */
  char       service[64];  /**< driver service name */
  char       location[256];  /**< location information */
  char       bios_name[192];      /**< canonical ACPI path "\\_SB.TCPD" if ACPI-enumerated */
  char       driver_store_dir[A2D_MAX_PATH]; /**< DriverStore\\FileRepository\\<pkg\> */
  ULONG      status;  /**< DN_* status flags */
  ULONG      problem;             /**< CM_PROB_*, 0 = OK */
  uint64_t   mem_base[8];  /**< MMIO range starts */
  uint64_t   mem_end[8];  /**< MMIO range ends */
  int        nmem;  /**< number of MMIO ranges */
  uint32_t   irqs[8];  /**< interrupts */
  int        nirq;  /**< number of interrupts */
  /* Qualcomm PEP / camera drivers: applied resource binaries
     (Device Parameters\Resources\BinaryPath, ScfgBinaryPath) */
  char       res_bin[A2D_MAX_PATH];  /**< Resources\\BinaryPath (power resources) */
  char       scfg_bin[A2D_MAX_PATH];  /**< Resources\\ScfgBinaryPath (sensor configuration) */
} dm_dev_t;

/** @brief a monitor known to Windows, decoded from its EDID */
typedef struct {
  char       instance_id[192];  /**< device instance ID */
  bool       present;  /**< currently connected/active */
  char       pnp_id[4];           /**< EDID manufacturer, e.g. "BOE" */
  uint16_t   product;             /**< EDID product code */
  char       name[16];            /**< descriptor 0xFC (monitor name) */
  char       text[2][16];         /**< descriptors 0xFE (unspecified text) */
  int width_cm;  /**< image width in cm */
  int height_cm;  /**< image height in cm */
  double     diagonal_in;  /**< diagonal in inches */
  bool       digital;  /**< digital input */
  int        interface_type;      /**< 5 = DisplayPort (eDP for internal panels) */
  bool       internal;            /**< heuristic: notebook panel */
} dm_monitor_t;

/** @brief Device Manager devices and monitors */
typedef struct {
  dm_dev_t     *d;  /**< present devices */
  int           count;  /**< number of devices */
  int           cap;  /**< capacity of d */
  dm_monitor_t *mon;  /**< monitors */
  int           nmon;  /**< number of monitors */
  int           capmon;  /**< capacity of mon */
} dm_set_t;

/**
 * @brief Enumerates all present devices and all known monitors; writes hardware_inventory.json.
 *
 * @param cfg  configuration
 * @param set  receives the devices
 * @return number of devices
 */
int  devmgr_collect(const a2d_config_t *cfg, dm_set_t *set);
/**
 * @brief Frees the device and monitor lists.
 *
 * @param set  Device Manager data
 */
void devmgr_free(dm_set_t *set);

/**
 * @brief Finds the present device bound to an ACPI namespace path.
 *
 * @param set        Device Manager data (may be NULL)
 * @param acpi_path  canonical ACPI path
 * @return the device or NULL
 */
const dm_dev_t *devmgr_find_by_acpi_path(const dm_set_t *set, const char *acpi_path);
/**
 * @brief Finds the first device having a hardware ID that contains a substring.
 *
 * @param set     Device Manager data (may be NULL)
 * @param substr  substring (case-insensitive)
 * @return the device or NULL
 */
const dm_dev_t *devmgr_find_by_hwid_substr(const dm_set_t *set, const char *substr);
/**
 * @brief Returns a short status text (OK, problem code, not present).
 *
 * @param d  device (may be NULL)
 * @return text, valid until the next call
 */
const char     *devmgr_status_text(const dm_dev_t *d);
/**
 * @brief Finds a present PCI device by vendor and device ID.
 *
 * @param set  Device Manager data (may be NULL)
 * @param ven  PCI vendor ID
 * @param dev  PCI device ID
 * @return the device or NULL
 */
const dm_dev_t *devmgr_find_pci(const dm_set_t *set, uint16_t ven, uint16_t dev);
/**
 * @brief Returns the best candidate for the internal panel (eDP preferred, then present).
 *
 * @param set  Device Manager data (may be NULL)
 * @return the monitor or NULL
 */
const dm_monitor_t *devmgr_internal_panel(const dm_set_t *set);

#endif /* A2D_DEVMGR_EXPLORER_H */
