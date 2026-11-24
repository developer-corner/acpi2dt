#include "inf_firmware_scanner.h"

static bool ends_with_ci(const wchar_t* str, const wchar_t* suffix) {
    size_t sl = wcslen(str);
    size_t xl = wcslen(suffix);
    if (sl < xl) return false;
    return _wcsicmp(str + sl - xl, suffix) == 0;
}

static bool is_firmware_blob(const wchar_t* name) {
    return ends_with_ci(name, L".mbn") || ends_with_ci(name, L".elf") ||
           ends_with_ci(name, L".bin") || ends_with_ci(name, L".mdt") ||
           ends_with_ci(name, L".fw")  || ends_with_ci(name, L".tlv");
}

static void inspect_elf_or_fw_file(FILE* out_fp, const wchar_t* path, const wchar_t* file_name, bool extract_strings, int* fw_count) {
    FILE* f = _wfopen(path, L"rb");
    if (!f) return;

    uint8_t hdr[64];
    size_t n = fread(hdr, 1, sizeof(hdr), f);
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);

    const char* fmt = "Raw Binary";
    char machine[64] = "Unknown";
    if (n >= 20 && hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
        fmt = (hdr[4] == 2) ? "ELF64" : "ELF32";
        uint16_t e_mach = (uint16_t)(hdr[18] | (hdr[19] << 8));
        if (e_mach == 183) strcpy(machine, "AArch64");
        else if (e_mach == 164) strcpy(machine, "Qualcomm Hexagon DSP");
        else if (e_mach == 40) strcpy(machine, "ARM32");
        else _snprintf(machine, sizeof(machine), "ELF(%u)", (unsigned)e_mach);
    } else if (ends_with_ci(file_name, L".mbn")) {
        fmt = "MBN";
    }

    char utf_name[256];
    char utf_path[WOA_MAX_PATH];
    woa_wide_to_utf8(file_name, utf_name, sizeof(utf_name));
    woa_wide_to_utf8(path, utf_path, sizeof(utf_path));

    if (*fw_count > 0) fputs(",\n", out_fp);
    fputs("    { ", out_fp);
    fputs("\"fileName\": ", out_fp); woa_json_escape_fp(out_fp, utf_name);
    fputs(", \"path\": ", out_fp); woa_json_escape_fp(out_fp, utf_path);
    fprintf(out_fp, ", \"sizeBytes\": %ld", fsize);
    fputs(", \"format\": ", out_fp); woa_json_escape_fp(out_fp, fmt);
    fputs(", \"machine\": ", out_fp); woa_json_escape_fp(out_fp, machine);

    /* Extract hardware hint strings from first 256 KB */
    fputs(", \"hints\": [", out_fp);
    if (extract_strings) {
        fseek(f, 0, SEEK_SET);
        size_t cap = 262144;
        uint8_t* buf = (uint8_t*)malloc(cap);
        if (buf) {
            size_t rd = fread(buf, 1, cap, f);
            char tok[128];
            size_t tlen = 0;
            int emitted = 0;
            for (size_t i = 0; i < rd && emitted < 8; ++i) {
                uint8_t c = buf[i];
                if (c >= 32 && c < 127) {
                    if (tlen < sizeof(tok) - 1) tok[tlen++] = (char)c;
                } else {
                    if (tlen >= 8) {
                        tok[tlen] = '\0';
                        if (strstr(tok, "qcom") || strstr(tok, "QCOM") || strstr(tok, "adsp") ||
                            strstr(tok, "cdsp") || strstr(tok, "wcn") || strstr(tok, "x1e")) {
                            if (emitted > 0) fputs(", ", out_fp);
                            woa_json_escape_fp(out_fp, tok);
                            emitted++;
                        }
                    }
                    tlen = 0;
                }
            }
            free(buf);
        }
    }
    fputs("] }", out_fp);
    (*fw_count)++;
    fclose(f);
}

static void walk_driver_store(const wchar_t* dir, FILE* fw_fp, bool extract_strings, int* inf_count, int* fw_count, const wchar_t* inf_copy_dir) {
    wchar_t pattern[WOA_MAX_PATH];
    _snwprintf(pattern, WOA_MAX_PATH, L"%s\\*", dir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t full[WOA_MAX_PATH];
        _snwprintf(full, WOA_MAX_PATH, L"%s\\%s", dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walk_driver_store(full, fw_fp, extract_strings, inf_count, fw_count, inf_copy_dir);
        } else if (ends_with_ci(fd.cFileName, L".inf")) {
            wchar_t dst[WOA_MAX_PATH];
            _snwprintf(dst, WOA_MAX_PATH, L"%s\\%s", inf_copy_dir, fd.cFileName);
            CopyFileW(full, dst, FALSE);
            (*inf_count)++;
        } else if (is_firmware_blob(fd.cFileName)) {
            inspect_elf_or_fw_file(fw_fp, full, fd.cFileName, extract_strings, fw_count);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

int woa_scan_drivers_and_firmware(const woa_config_t* cfg) {
    if (!cfg->scan_driver_store) return 0;

    wchar_t inf_dir[WOA_MAX_PATH];
    wchar_t json_path[WOA_MAX_PATH];
    _snwprintf(inf_dir, WOA_MAX_PATH, L"%s\\inf_collected", cfg->output_dir);
    _snwprintf(json_path, WOA_MAX_PATH, L"%s\\firmware_scan.json", cfg->output_dir);
    CreateDirectoryW(inf_dir, NULL);

    FILE* fw_fp = _wfopen(json_path, L"wb");
    if (!fw_fp) return -1;

    fputs("{\n  \"firmwareArtifacts\": [\n", fw_fp);

    int inf_count = 0;
    int fw_count = 0;
    walk_driver_store(L"C:\\Windows\\System32\\DriverStore\\FileRepository",
                      fw_fp, cfg->extract_fw_strings, &inf_count, &fw_count, inf_dir);

    fputs("\n  ]\n}\n", fw_fp);
    fclose(fw_fp);

    printf("[DriverStore] Collected %d .inf files and indexed %d firmware binaries\n", inf_count, fw_count);
    return 0;
}
