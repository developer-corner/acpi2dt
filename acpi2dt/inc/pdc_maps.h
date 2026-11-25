/* SPDX-License-Identifier: MIT */
/**
 * @file   pdc_maps.h
 * @author Ingo A. Kubbilun
 * @brief  Qualcomm TLMM -> PDC wake-up maps (data in pdc_maps.c, GPL-2.0-only)
 *
 * [MIT license] - see main.c
 */

#ifndef A2D_PDC_MAPS_H
#define A2D_PDC_MAPS_H

#include <stdint.h>

/** @brief TLMM GPIO -> PDC pin (wake-up capable interrupt) */
typedef struct {
  uint16_t gpio;  /**< TLMM GPIO */
  uint16_t pdc;   /**< PDC pin */
} wake_map_t;

/**
 * @brief returns the PDC wake map of a TLMM
 *
 * @param tlmm_compatible  compatible property (raw) of the TLMM node
 * @param count            receives the number of entries
 * @return the map or NULL if the TLMM is not known
 */
const wake_map_t *pdc_wake_map(const char *tlmm_compatible, int *count);

#endif /* A2D_PDC_MAPS_H */
