/**
 * @file   camera.h
 * @author Ingo A. Kubbilun
 * @brief  camera sensors from the Qualcomm Windows camera driver configuration
 *         (AeoB resource / sensor-config binaries referenced by the devices'
 *         Device Parameters\Resources registry values)
 *
 * [MIT license] - see main.c
 */

#ifndef A2D_CAMERA_H
#define A2D_CAMERA_H

#include "common.h"
#include "devmgr_explorer.h"

typedef struct {
  char      name[48];          /* "PPP_RESOURCE_ID_LDO7_B"         */
  char      pmic;              /* 'b'                               */
  int       ldo;               /* 7                                 */
  uint32_t  uv;                /* 2800000                           */
} cam_vreg_t;

typedef struct {
  int       pin;
  int       v[5];              /* raw TLMM config values            */
} cam_gpio_t;

typedef struct {
  char        acpi_path[64];   /* "\_SB.CAMF"                       */
  char        dev_name[256];   /* Device Manager name               */
  char        res_file[A2D_MAX_PATH];
  char        scfg_file[A2D_MAX_PATH];

  /* sensor configuration (SCFG) */
  char        module[96];      /* "com.qti.sensormodule.ov02c10.bin" */
  char        tuning[96];
  char        sensor[48];      /* "ov02c10"                          */
  uint16_t    slave8;          /* 8-bit I2C address                  */
  uint16_t    cci_info;        /* upper half of the address word     */
  uint16_t    chip_id;
  uint16_t    chip_id_reg;
  bool        has_scfg;

  /* power-on sequence (D0) */
  bool        has_res;
  char        mclk[32];        /* "cam_cc_mclk4_clk"                 */
  uint32_t    mclk_hz;
  cam_vreg_t  vreg[4];
  int         nvreg;
  cam_gpio_t  gpio[8];
  int         ngpio;
  int         reset_pin;       /* -1 if none                         */
  bool        reset_active_low;
  int         delay_after;     /* last DELAY in D0                   */
  strlist_t   steps;           /* human readable D0 sequence         */
} cam_sensor_t;

typedef struct {
  cam_sensor_t *s;
  int           count, cap;
} cam_set_t;

int  camera_collect(const dm_set_t *dm, cam_set_t *set);
void camera_free(cam_set_t *set);
/* "ovti" for ov*, "sony" for imx*, ... ; NULL if unknown */
const char *camera_vendor_prefix(const char *sensor);

#endif /* A2D_CAMERA_H */
