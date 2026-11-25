/* SPDX-License-Identifier: MIT */
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

/** @brief a firmware-like file in the DriverStore */
typedef struct {
  char      *path;  /**< full path */
  char      *name;  /**< file name */
  char      *pkg;            /**< driver package folder name */
  int64_t    size;  /**< file size */
  bool       present_pkg;    /**< belongs to a driver of a present device */
  char       format[32];     /**< "ELF64 AArch64", "ELF32 Hexagon", "MBN"... */
} fw_file_t;

/** @brief a driver package (DriverStore folder) of a present device */
typedef struct {
  char       inf[64];        /**< published name (oemNN.inf) */
  char       original_inf[128];  /**< original INF file name */
  char       dir[A2D_MAX_PATH];  /**< DriverStore folder */
  char       provider[128];  /**< provider */
  char       cls[64];  /**< setup class */
  char       driver_ver[96];  /**< DriverVer */
  strlist_t  devices;        /**< friendly names of devices using it */
  strlist_t  firmware;       /**< firmware-like files listed by the INF */
} drv_pkg_t;

/** @brief driver packages and firmware index */
typedef struct {
  drv_pkg_t *p;  /**< driver packages */
  int np;  /**< number of packages */
  int cap_p;  /**< capacity of p */
  fw_file_t *f;  /**< firmware files */
  int nf;  /**< number of firmware files */
  int cap_f;  /**< capacity of f */
  bool       full_indexed;  /**< the complete DriverStore is indexed */
  strmap_t   seen;           /**< lower-case paths already indexed */
} drv_db_t;

/** @brief one planned firmware copy */
typedef struct {
  char       target_rel[256]; /**< relative to the firmware output folder */
  char       source[A2D_MAX_PATH];  /**< source file */
  char       reason[256];  /**< why the file is copied */
  bool       copied;  /**< copy succeeded */
  int64_t    size;  /**< file size */
  char       sha256[65];  /**< SHA-256 digest (hex) */
} fw_copy_t;

/** @brief list of planned firmware copies */
typedef struct {
  fw_copy_t *c;  /**< copy jobs */
  int count;  /**< number of jobs */
  int cap;  /**< capacity of c */
} fw_plan_t;

/**
 * @brief Scans the driver packages of present devices: INF metadata and firmware files; writes firmware_scan.json.
 *
 * @param cfg  configuration
 * @param dm   Device Manager data (may be NULL)
 * @param db   receives the database
 * @return number of firmware files
 */
int  drivers_scan(const a2d_config_t *cfg, const dm_set_t *dm, drv_db_t *db);
/**
 * @brief Frees the driver/firmware database.
 *
 * @param db  database
 */
void drivers_free(drv_db_t *db);
/**
 * @brief Finds a driver package by DriverStore folder.
 *
 * @param db   database (may be NULL)
 * @param dir  folder
 * @return package or NULL
 */
const drv_pkg_t *drivers_pkg_by_dir(const drv_db_t *db, const char *dir);

/**
 * @brief Resolves a firmware file name in the DriverStore, preferring SoC-specific variants.
 *
 * @param db           database (full index built on demand)
 * @param wanted_name  wanted file name
 * @param soc          SoC (variant preference)
 * @param why          receives a description
 * @param whycap       size of why
 * @return file or NULL
 */
const fw_file_t *fw_resolve(drv_db_t *db, const char *wanted_name, const char *soc, char *why, size_t whycap);
/**
 * @brief Finds firmware files by prefix and suffix.
 *
 * @param db      database
 * @param prefix  name prefix (case-insensitive)
 * @param suffix  name suffix (case-insensitive)
 * @param out     receives the files
 * @param max     capacity of out
 * @return number of files
 */
int  fw_find_all(drv_db_t *db, const char *prefix, const char *suffix, const fw_file_t **out, int max);

/**
 * @brief Adds a copy job (duplicates by target are ignored).
 *
 * @param plan        copy plan
 * @param target_rel  target path relative to the firmware folder
 * @param source      source file
 * @param reason      reason
 */
void fw_plan_add(fw_plan_t *plan, const char *target_rel, const char *source, const char *reason);
/**
 * @brief Copies the planned files and writes FIRMWARE-MANIFEST.txt with SHA-256 digests.
 *
 * @param cfg   configuration
 * @param plan  copy plan (status updated)
 * @return number of copied files
 */
int  fw_plan_execute(const a2d_config_t *cfg, fw_plan_t *plan);  /* copies + FIRMWARE-MANIFEST.txt */
/**
 * @brief Frees a copy plan.
 *
 * @param plan  copy plan
 */
void fw_plan_free(fw_plan_t *plan);

#endif /* A2D_INF_FIRMWARE_SCANNER_H */
