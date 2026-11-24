#ifndef WOA_COMMON_H
#define WOA_COMMON_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WOA_MAX_PATH 1024

typedef struct {
    wchar_t output_dir[WOA_MAX_PATH];
    wchar_t iasl_exe[WOA_MAX_PATH];
    wchar_t dtc_exe[WOA_MAX_PATH];
    wchar_t primary_dts[WOA_MAX_PATH];
    wchar_t secondary_dts[WOA_MAX_PATH];
    bool scan_driver_store;
    bool extract_fw_strings;
} woa_config_t;

static inline void woa_json_escape_fp(FILE* fp, const char* s) {
    fputc('"', fp);
    if (s) {
        for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
            if (*p == '"') fputs("\\\"", fp);
            else if (*p == '\\') fputs("\\\\", fp);
            else if (*p == '\n') fputs("\\n", fp);
            else if (*p == '\r') fputs("\\r", fp);
            else if (*p == '\t') fputs("\\t", fp);
            else if (*p >= 32 && *p < 127) fputc(*p, fp);
        }
    }
    fputc('"', fp);
}

static inline void woa_wide_to_utf8(const wchar_t* ws, char* out, size_t out_cap) {
    if (!ws || !out || out_cap == 0) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, out, (int)out_cap, NULL, NULL);
    if (n <= 0) out[0] = '\0';
}

#endif /* WOA_COMMON_H */
