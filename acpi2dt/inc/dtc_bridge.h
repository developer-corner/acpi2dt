/* SPDX-License-Identifier: MIT */
/**
 * @file   dtc_bridge.h
 * @author Ingo A. Kubbilun
 * @brief  in-process device tree compiler (embedded dtc) for the draft
 *
 * [MIT license] - see main.c (the embedded dtc itself is GPL-2.0-or-later)
 */

#ifndef A2D_DTC_BRIDGE_H
#define A2D_DTC_BRIDGE_H

#include "common.h"

/** result of an in-process DTS -> DTB compilation */
typedef struct {
  bool      preprocessed;     /**< dts_preprocess() succeeded                 */
  bool      compiled;         /**< dtc produced a DTB                          */
  int       exit_code;        /**< dtc exit code (0 = success)                 */
  int       warnings;         /**< number of dtc warnings                      */
  int       errors;           /**< number of dtc / preprocessor errors         */
  int64_t   dtb_size;         /**< size of the DTB in bytes                    */
  strlist_t messages;         /**< preprocessor errors and dtc output lines    */
} dtc_result_t;

/**
 * @brief preprocesses and compiles a DTS file to a DTB with the embedded dtc
 *
 * Runs the built-in preprocessor (dts_cpp.c) with the kernel include
 * directories, writes the preprocessed source to @p pre_path and calls dtc
 * ("-I dts -O dtb") in-process. dtc's diagnostics go to @p log_path.
 *
 * @param dts_path     DTS file (draft)
 * @param inc_dirs     include directories (dts/qcom, dts, kernel include, ...)
 * @param ninc         number of include directories
 * @param pre_path     file for the preprocessed source
 * @param dtb_path     output DTB
 * @param log_path     file receiving dtc's messages
 * @param res          result (call dtc_result_free() afterwards)
 * @return true if a DTB was written
 */
bool dtc_compile(const char *dts_path, const char **inc_dirs, int ninc, const char *pre_path, const char *dtb_path,
                 const char *log_path, dtc_result_t *res);

/**
 * @brief frees the message list of a result
 *
 * @param res  result of dtc_compile()
 */
void dtc_result_free(dtc_result_t *res);

#endif /* A2D_DTC_BRIDGE_H */
