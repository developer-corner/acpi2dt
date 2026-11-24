#include "devmgr_explorer.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>

static void print_multi_sz_json(FILE* fp, const wchar_t* buf) {
    fputc('[', fp);
    if (buf) {
        const wchar_t* p = buf;
        bool first = true;
        while (*p) {
            char utf8[512];
            woa_wide_to_utf8(p, utf8, sizeof(utf8));
            if (!first) fputs(", ", fp);
            woa_json_escape_fp(fp, utf8);
            first = false;
            p += wcslen(p) + 1;
        }
    }
    fputc(']', fp);
}

static bool query_dev_prop_str(HDEVINFO devs, SP_DEVINFO_DATA* info, const DEVPROPKEY* key, wchar_t* out, DWORD cap_bytes) {
    DEVPROPTYPE ptype = 0;
    DWORD req = 0;
    ZeroMemory(out, cap_bytes);
    return SetupDiGetDevicePropertyW(devs, info, key, &ptype, (PBYTE)out, cap_bytes, &req, 0) != FALSE;
}

static void emit_cm_resources(FILE* fp, DEVINST dev_inst) {
    LOG_CONF log_conf = 0;
    if (CM_Get_First_Log_Conf(&log_conf, dev_inst, ALLOC_LOG_CONF) != CR_SUCCESS) {
        if (CM_Get_First_Log_Conf(&log_conf, dev_inst, BOOT_LOG_CONF) != CR_SUCCESS) {
            fputs("\"mmioRanges\": [], \"irqs\": []", fp);
            return;
        }
    }

    char mmio_buf[2048] = "";
    char irq_buf[512] = "";
    bool first_mmio = true;
    bool first_irq = true;

    RES_DES rd = log_conf;
    RES_DES next_rd = 0;
    RESOURCEID res_id = 0;

    while (CM_Get_Next_Res_Des(&next_rd, rd, ResType_All, &res_id, 0) == CR_SUCCESS) {
        if (rd != log_conf) CM_Free_Res_Des_Handle(rd);
        rd = next_rd;

        ULONG sz = 0;
        if (CM_Get_Res_Des_Data_Size(&sz, rd, 0) == CR_SUCCESS && sz > 0) {
            BYTE* data = (BYTE*)malloc(sz);
            if (data && CM_Get_Res_Des_Data(rd, data, sz, 0) == CR_SUCCESS) {
                if (res_id == ResType_Mem && sz >= sizeof(MEM_RESOURCE)) {
                    const MEM_RESOURCE* mem = (const MEM_RESOURCE*)data;
                    char item[128];
                    _snprintf(item, sizeof(item), "%s\"0x%llx-0x%llx\"",
                              first_mmio ? "" : ", ",
                              (unsigned long long)mem->MEM_Header.MD_Alloc_Base,
                              (unsigned long long)mem->MEM_Header.MD_Alloc_End);
                    strncat(mmio_buf, item, sizeof(mmio_buf) - strlen(mmio_buf) - 1);
                    first_mmio = false;
                } else if (res_id == ResType_IRQ && sz >= sizeof(IRQ_RESOURCE)) {
                    const IRQ_RESOURCE* irq = (const IRQ_RESOURCE*)data;
                    char item[32];
                    _snprintf(item, sizeof(item), "%s%u",
                              first_irq ? "" : ", ",
                              (unsigned)irq->IRQ_Header.IRQD_Alloc_Num);
                    strncat(irq_buf, item, sizeof(irq_buf) - strlen(irq_buf) - 1);
                    first_irq = false;
                }
            }
            free(data);
        }
    }
    if (rd != log_conf) CM_Free_Res_Des_Handle(rd);
    CM_Free_Log_Conf_Handle(log_conf);

    fprintf(fp, "\"mmioRanges\": [%s], \"irqs\": [%s]", mmio_buf, irq_buf);
}

int woa_explore_device_manager(const woa_config_t* cfg) {
    HDEVINFO devs = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[DevMgr] SetupDiGetClassDevsW failed (%lu)\n", GetLastError());
        return -1;
    }

    wchar_t out_path[WOA_MAX_PATH];
    _snwprintf(out_path, WOA_MAX_PATH, L"%s\\hardware_inventory.json", cfg->output_dir);
    FILE* fp = _wfopen(out_path, L"wb");
    if (!fp) {
        SetupDiDestroyDeviceInfoList(devs);
        return -1;
    }

    fputs("{\n  \"devices\": [\n", fp);

    SP_DEVINFO_DATA info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);

    DWORD count = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &info); ++i) {
        wchar_t w_inst[512] = L"";
        wchar_t w_name[512] = L"";
        wchar_t w_cls[256] = L"";
        wchar_t w_hw[2048] = L"";
        wchar_t w_compat[2048] = L"";
        wchar_t w_inf[256] = L"";
        wchar_t w_ver[128] = L"";
        wchar_t w_loc[512] = L"";
        wchar_t w_bios[512] = L"";

        SetupDiGetDeviceInstanceIdW(devs, &info, w_inst, 512, NULL);
        if (!query_dev_prop_str(devs, &info, &DEVPKEY_Device_FriendlyName, w_name, sizeof(w_name))) {
            query_dev_prop_str(devs, &info, &DEVPKEY_Device_DeviceDesc, w_name, sizeof(w_name));
        }
        query_dev_prop_str(devs, &info, &DEVPKEY_Device_Class, w_cls, sizeof(w_cls));
        query_dev_prop_str(devs, &info, &DEVPKEY_Device_HardwareIds, w_hw, sizeof(w_hw));
        query_dev_prop_str(devs, &info, &DEVPKEY_Device_CompatibleIds, w_compat, sizeof(w_compat));
        query_dev_prop_str(devs, &info, &DEVPKEY_Device_DriverInfPath, w_inf, sizeof(w_inf));
        query_dev_prop_str(devs, &info, &DEVPKEY_Device_DriverVersion, w_ver, sizeof(w_ver));
        query_dev_prop_str(devs, &info, &DEVPKEY_Device_LocationInfo, w_loc, sizeof(w_loc));
        query_dev_prop_str(devs, &info, &DEVPKEY_Device_BiosDeviceName, w_bios, sizeof(w_bios));

        char inst[512], name[512], cls[256], inf[256], ver[128], loc[512], bios[512];
        woa_wide_to_utf8(w_inst, inst, sizeof(inst));
        woa_wide_to_utf8(w_name, name, sizeof(name));
        woa_wide_to_utf8(w_cls, cls, sizeof(cls));
        woa_wide_to_utf8(w_inf, inf, sizeof(inf));
        woa_wide_to_utf8(w_ver, ver, sizeof(ver));
        woa_wide_to_utf8(w_loc, loc, sizeof(loc));
        woa_wide_to_utf8(w_bios, bios, sizeof(bios));

        if (count > 0) fputs(",\n", fp);
        fputs("    { ", fp);
        fputs("\"instanceId\": ", fp); woa_json_escape_fp(fp, inst);
        fputs(", \"friendlyName\": ", fp); woa_json_escape_fp(fp, name);
        fputs(", \"className\": ", fp); woa_json_escape_fp(fp, cls);
        fputs(", \"hardwareIds\": ", fp); print_multi_sz_json(fp, w_hw);
        fputs(", \"compatibleIds\": ", fp); print_multi_sz_json(fp, w_compat);
        fputs(", \"driverInf\": ", fp); woa_json_escape_fp(fp, inf);
        fputs(", \"driverVersion\": ", fp); woa_json_escape_fp(fp, ver);
        fputs(", \"locationInfo\": ", fp); woa_json_escape_fp(fp, loc);
        fputs(", \"biosDeviceName\": ", fp); woa_json_escape_fp(fp, bios);
        fputs(", \"status\": \"OK\", ", fp);
        emit_cm_resources(fp, info.DevInst);
        fputs(" }", fp);
        count++;
    }

    fputs("\n  ]\n}\n", fp);
    fclose(fp);
    SetupDiDestroyDeviceInfoList(devs);
    printf("[DevMgr] Exported %lu PnP device records to hardware_inventory.json\n", count);
    return 0;
}
