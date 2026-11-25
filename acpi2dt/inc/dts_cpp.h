/* SPDX-License-Identifier: MIT */
/**
 * @file   dts_cpp.h
 * @author Ingo A. Kubbilun
 * @brief  minimal C preprocessor for device tree sources
 *
 * Kernel DTS files are run through "cpp -nostdinc -undef -D__DTS__
 * -x assembler-with-cpp" before dtc. This module implements the subset that
 * device tree sources use, so that acpi2dt can compile its draft without an
 * external preprocessor.
 *
 * [MIT license] - see main.c
 */

#ifndef A2D_DTS_CPP_H
#define A2D_DTS_CPP_H

#include "common.h"

/**
 * @brief preprocesses a DTS file
 *
 * Supported: \#include "..." and \<...\> (search order: directory of the
 * including file, then @p inc_dirs), object-like and function-like macros
 * (including variadic macros, # and ##), \#undef, \#if / \#ifdef / \#ifndef /
 * \#elif / \#else / \#endif with integer expressions and defined(), \#error,
 * comment removal and cpp line markers for dtc's source positions.
 * __DTS__ is predefined.
 *
 * Include files that cannot be read (e.g. kernel symlinks checked out on
 * Windows) are looked up via known fallbacks, e.g.
 * dt-bindings/input/linux-event-codes.h -> uapi/linux/input-event-codes.h.
 *
 * @param in_path   DTS file to preprocess
 * @param inc_dirs  include search directories
 * @param ninc      number of entries in @p inc_dirs
 * @param out       receives the preprocessed text
 * @param errors    receives error messages (file:line: text)
 * @return true if no error occurred
 */
bool dts_preprocess(const char *in_path, const char **inc_dirs, int ninc, strbuf_t *out, strlist_t *errors);

#endif /* A2D_DTS_CPP_H */
