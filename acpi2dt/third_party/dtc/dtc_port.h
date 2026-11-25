/* SPDX-License-Identifier: MIT */
/**
 * @file   dtc_port.h
 * @author Ingo A. Kubbilun
 * @brief  forced include for the embedded dtc sources (acpi2dt build only)
 *
 * - renames dtc symbols that clash with acpi2dt (xstrdup, xstrndup, main)
 * - routes exit() to dtc_exit(): dtc reports fatal errors with exit(),
 *   which inside acpi2dt must return to the caller (see dtc_bridge.c)
 */

#ifndef A2D_DTC_PORT_H
#define A2D_DTC_PORT_H

#include <stdlib.h>   /* declare the real exit() before it is renamed */
#include <stdio.h>

#define xstrdup  dtc_xstrdup
#define xstrndup dtc_xstrndup
#define main     dtc_main
#define exit     dtc_exit

__declspec(noreturn) void dtc_exit(int code);

#endif /* A2D_DTC_PORT_H */
