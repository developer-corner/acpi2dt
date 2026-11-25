/* SPDX-License-Identifier: MIT */
/**
 * @file   camera.h
 * @author Ingo A. Kubbilun
 * @brief  camera sensors from the Qualcomm Windows camera driver configuration
 *         (AeoB resource / sensor-config binaries referenced by the devices'
 *         Device Parameters\\Resources registry values)
 *
 * [MIT license] - see main.c
 */

#ifndef A2D_CAMERA_H
#define A2D_CAMERA_H

#include "common.h"
#include "devmgr_explorer.h"

/** @brief a PMIC regulator vote of a camera power sequence */
typedef struct {
  char      name[48];          /**< "PPP_RESOURCE_ID_LDO7_B" */
  char      pmic;              /**< 'b' */
  int       ldo;               /**< 7 */
  uint32_t  uv;                /**< 2800000 */
} cam_vreg_t;

/** @brief a TLMM GPIO step of a camera power sequence */
typedef struct {
  int       pin;  /**< TLMM pin */
  int       v[5];              /**< raw TLMM config values */
} cam_gpio_t;

/** @brief a camera sensor described by the Windows camera driver configuration */
typedef struct {
  char        acpi_path[64];   /**< "\\_SB.CAMF" */
  char        dev_name[256];   /**< Device Manager name */
  char        res_file[A2D_MAX_PATH];  /**< power sequence file (CAM*_RES_*.bin) */
  char        scfg_file[A2D_MAX_PATH];  /**< sensor configuration file (SCFG_*.bin) */

  /* sensor configuration (SCFG) */
  char        module[96];      /**< "com.qti.sensormodule.ov02c10.bin" */
  char        tuning[96];  /**< tuning file name */
  char        sensor[48];      /**< "ov02c10" */
  uint16_t    slave8;          /**< 8-bit I2C address */
  uint16_t    cci_info;        /**< upper half of the address word */
  uint16_t    chip_id;  /**< chip ID value */
  uint16_t    chip_id_reg;  /**< chip ID register */
  bool        has_scfg;  /**< sensor configuration decoded */

  /* power-on sequence (D0) */
  bool        has_res;  /**< power sequence decoded */
  char        mclk[32];        /**< "cam_cc_mclk4_clk" */
  uint32_t    mclk_hz;  /**< MCLK rate in Hz */
  cam_vreg_t  vreg[4];  /**< regulator votes of the power-on sequence */
  int         nvreg;  /**< number of entries in vreg */
  cam_gpio_t  gpio[8];  /**< TLMM GPIO steps of the power-on sequence */
  int         ngpio;  /**< number of entries in gpio */
  int         reset_pin;       /**< -1 if none */
  bool        reset_active_low;  /**< reset is released by driving the pin high */
  int         delay_after;     /**< last DELAY in D0 */
  strlist_t   steps;           /**< human readable D0 sequence */
} cam_sensor_t;

/** @brief all camera sensors */
typedef struct {
  cam_sensor_t *s;  /**< sensors */
  int count;  /**< number of sensors */
  int cap;  /**< capacity of s */
} cam_set_t;

/**
 * @brief Collects the camera sensors of all devices with a Qualcomm sensor configuration.
 *
 * @param dm   Device Manager data
 * @param set  receives the sensors
 * @return number of sensors
 */
int  camera_collect(const dm_set_t *dm, cam_set_t *set);
/**
 * @brief Frees a sensor list.
 *
 * @param set  sensor list
 */
void camera_free(cam_set_t *set);
/**
 * @brief Returns the DT vendor prefix of a sensor name.
 *
 * @param sensor  sensor name, e.g. "ov02c10"
 * @return prefix, e.g. "ovti", or NULL
 */
const char *camera_vendor_prefix(const char *sensor);

#endif /* A2D_CAMERA_H */
