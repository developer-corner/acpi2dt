#include "acpi_extractor.h"

#pragma pack(push, 1)
typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    char     creator_id[4];
    uint32_t creator_revision;
} acpi_sdt_header_t;
#pragma pack(pop)

static void sig_to_u32_le(const char sig[4], DWORD* out_id) {
    /* EnumSystemFirmwareTables / GetSystemFirmwareTable expects big-endian 4CC */
    *out_id = ((DWORD)(uint8_t)sig[0] << 24) |
              ((DWORD)(uint8_t)sig[1] << 16) |
              ((DWORD)(uint8_t)sig[2] << 8)  |
              ((DWORD)(uint8_t)sig[3]);
}

static int run_iasl_disassemble(const wchar_t* iasl_exe, const wchar_t* dat_path, const wchar_t* dsl_dir) {
    wchar_t cmd[WOA_MAX_PATH * 3];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    _snwprintf(cmd, sizeof(cmd) / sizeof(cmd[0]),
        L"\"%s\" -d \"%s\"", iasl_exe, dat_path);

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dsl_dir, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;
}

static void dump_table_to_disk(const woa_config_t* cfg, const uint8_t* buf, DWORD size, const char* tag, int index) {
    if (!buf || size < sizeof(acpi_sdt_header_t)) return;
    const acpi_sdt_header_t* hdr = (const acpi_sdt_header_t*)buf;

    char oem[7] = {0};
    char tid[9] = {0};
    memcpy(oem, hdr->oem_id, 6);
    memcpy(tid, hdr->oem_table_id, 8);

    wchar_t dat_path[WOA_MAX_PATH];
    wchar_t wtag[16];
    MultiByteToWideChar(CP_UTF8, 0, tag, -1, wtag, 16);
    _snwprintf(dat_path, WOA_MAX_PATH, L"%s\\acpi\\%s_%d.dat", cfg->output_dir, wtag, index);

    FILE* fp = _wfopen(dat_path, L"wb");
    if (fp) {
        fwrite(buf, 1, size, fp);
        fclose(fp);
        printf("[ACPI] Extracted %-4s (%u bytes, OEM=%s, TableID=%s)\n", tag, (unsigned)size, oem, tid);

        if (wcslen(cfg->iasl_exe) > 0 &&
            (strcmp(tag, "DSDT") == 0 || strcmp(tag, "SSDT") == 0)) {
            wchar_t acpi_dir[WOA_MAX_PATH];
            _snwprintf(acpi_dir, WOA_MAX_PATH, L"%s\\acpi", cfg->output_dir);
            if (run_iasl_disassemble(cfg->iasl_exe, dat_path, acpi_dir) == 0) {
                printf("[ACPI] Disassembled %s_%d.dat -> .dsl via ACPICA iasl\n", tag, index);
            }
        }
    }
}

static void dump_registry_table(const woa_config_t* cfg, const wchar_t* subkey, const char* tag) {
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return;
    }

    wchar_t child_name[256];
    DWORD idx = 0;
    int dump_idx = 0;
    while (1) {
        DWORD name_len = 256;
        if (RegEnumKeyExW(hKey, idx++, child_name, &name_len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;

        HKEY hSub = NULL;
        if (RegOpenKeyExW(hKey, child_name, 0, KEY_READ, &hSub) == ERROR_SUCCESS) {
            /* Walk deeper if OEM/TableID subkeys exist */
            wchar_t val_name[256];
            DWORD v_idx = 0;
            while (1) {
                DWORD vn_len = 256;
                DWORD type = 0;
                DWORD data_len = 0;
                if (RegEnumValueW(hSub, v_idx++, val_name, &vn_len, NULL, &type, NULL, &data_len) != ERROR_SUCCESS) break;
                if (type == REG_BINARY && data_len >= sizeof(acpi_sdt_header_t)) {
                    uint8_t* buf = (uint8_t*)malloc(data_len);
                    if (buf) {
                        if (RegQueryValueExW(hSub, val_name, NULL, &type, buf, &data_len) == ERROR_SUCCESS) {
                            dump_table_to_disk(cfg, buf, data_len, tag, dump_idx++);
                        }
                        free(buf);
                    }
                }
            }
            RegCloseKey(hSub);
        }
    }
    RegCloseKey(hKey);
}

int woa_extract_acpi_tables(const woa_config_t* cfg) {
    wchar_t acpi_dir[WOA_MAX_PATH];
    _snwprintf(acpi_dir, WOA_MAX_PATH, L"%s\\acpi", cfg->output_dir);
    CreateDirectoryW(cfg->output_dir, NULL);
    CreateDirectoryW(acpi_dir, NULL);

    /* 1. Enumerate firmware tables via GetSystemFirmwareTable('ACPI', ...) */
    UINT needed = EnumSystemFirmwareTables('ACPI', NULL, 0);
    if (needed > 0) {
        DWORD* table_ids = (DWORD*)malloc(needed);
        if (table_ids) {
            UINT count = EnumSystemFirmwareTables('ACPI', table_ids, needed) / sizeof(DWORD);
            for (UINT i = 0; i < count; ++i) {
                DWORD tid = table_ids[i];
                char sig[5];
                sig[0] = (char)(tid & 0xFF);
                sig[1] = (char)((tid >> 8) & 0xFF);
                sig[2] = (char)((tid >> 16) & 0xFF);
                sig[3] = (char)((tid >> 24) & 0xFF);
                sig[4] = '\0';

                /* Convert signature ordering for GetSystemFirmwareTable */
                DWORD be_id = 0;
                sig_to_u32_le(sig, &be_id);
                (void)be_id;

                UINT tbl_sz = GetSystemFirmwareTable('ACPI', tid, NULL, 0);
                if (tbl_sz > 0) {
                    uint8_t* buf = (uint8_t*)malloc(tbl_sz);
                    if (buf) {
                        if (GetSystemFirmwareTable('ACPI', tid, buf, tbl_sz) == tbl_sz) {
                            dump_table_to_disk(cfg, buf, tbl_sz, sig, (int)i);
                        }
                        free(buf);
                    }
                }
            }
            free(table_ids);
        }
    }

    /* 2. DSDT and SSDTs are reliably exposed under HKLM\HARDWARE\ACPI on Windows 11 ARM64 */
    dump_registry_table(cfg, L"HARDWARE\\ACPI\\DSDT", "DSDT");
    dump_registry_table(cfg, L"HARDWARE\\ACPI\\FADT", "FACP");
    dump_registry_table(cfg, L"HARDWARE\\ACPI\\SSDT", "SSDT");

    return 0;
}
