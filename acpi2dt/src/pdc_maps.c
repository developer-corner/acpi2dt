// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file   pdc_maps.c
 * @brief  Qualcomm TLMM -> PDC wake-up maps
 *
 * The tables are taken from the Linux kernel pinctrl drivers
 * (drivers/pinctrl/qcom/pinctrl-x1e80100.c, x1e80100_pdc_map[]):
 *
 *   Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 *   (pinctrl-x1e80100.c)
 *
 * This file is therefore licensed under GPL-2.0-only, like its source.
 * acpi2dt uses the maps to decode Qualcomm "virtual" ACPI GPIO numbers:
 * GIO0 interrupt -> GIC SPI -> PDC pin (qcom,pdc-ranges) -> TLMM GPIO.
 */

#include <string.h>
#include "pdc_maps.h"

/** TLMM PDC wake map of X1E80100 (also used by X1P42100 / purwa, which reuses "qcom,x1e80100-tlmm") */
static const wake_map_t g_x1e80100_wake[] = {
  {0, 72}, {2, 70}, {3, 71}, {6, 123}, {7, 67}, {11, 85}, {13, 86}, {15, 68}, {18, 122}, {19, 69}, {21, 158},
  {23, 143}, {24, 126}, {26, 129}, {27, 144}, {28, 77}, {29, 78}, {30, 92}, {31, 159}, {32, 145}, {33, 115},
  {34, 130}, {35, 146}, {36, 147}, {38, 113}, {39, 80}, {43, 148}, {47, 149}, {51, 79}, {53, 89}, {55, 81},
  {59, 87}, {64, 90}, {65, 106}, {66, 142}, {67, 88}, {68, 151}, {71, 91}, {75, 152}, {79, 153}, {80, 125},
  {81, 128}, {83, 154}, {84, 137}, {85, 155}, {87, 156}, {91, 157}, {92, 138}, {93, 139}, {94, 140}, {95, 141},
  {113, 84}, {121, 73}, {123, 74}, {125, 75}, {129, 76}, {131, 82}, {134, 83}, {141, 93}, {144, 94}, {145, 95},
  {147, 96}, {148, 97}, {150, 102}, {151, 103}, {153, 104}, {154, 100}, {156, 105}, {157, 107}, {163, 98},
  {166, 112}, {172, 99}, {175, 114}, {181, 101}, {184, 116}, {193, 117}, {196, 108}, {203, 133}, {208, 134},
  {212, 120}, {213, 150}, {214, 121}, {215, 118}, {217, 109}, {219, 119}, {220, 110}, {221, 111}, {222, 124},
  {224, 131}, {225, 132}, {228, 135}, {230, 136}, {232, 162},
};

const wake_map_t *pdc_wake_map(const char *tlmm_compatible, int *count)
{
  *count = 0;
  if (tlmm_compatible && strstr(tlmm_compatible, "x1e80100-tlmm"))
  {
    *count = (int)(sizeof(g_x1e80100_wake) / sizeof(g_x1e80100_wake[0]));
    return g_x1e80100_wake;
  }
  return NULL;
}
