/**
 * @file   devmgr_explorer.c
 * @author Ingo A. Kubbilun
 * @brief  enumerates hardware via the Windows Device Manager (SetupAPI,
 *         CfgMgr32) and links every device to its ACPI namespace path
 *         (DEVPKEY_Device_BiosDeviceName) and its DriverStore package
 *
 * [MIT license] - see main.c
 */

#include "devmgr_explorer.h"
#include "aml_parser.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <devguid.h>
#include <math.h>

static bool query_prop(HDEVINFO devs, SP_DEVINFO_DATA *info, const DEVPROPKEY *key, wchar_t *out, DWORD cap_bytes)
{
  DEVPROPTYPE ptype = 0;
  DWORD       req = 0;
  ZeroMemory(out, cap_bytes);
  if (!SetupDiGetDevicePropertyW(devs, info, key, &ptype, (PBYTE)out, cap_bytes - sizeof(wchar_t) * 2, &req, 0))
    return false;
  return ptype == DEVPROP_TYPE_STRING || ptype == DEVPROP_TYPE_STRING_LIST;
}

static void prop_utf8(HDEVINFO devs, SP_DEVINFO_DATA *info, const DEVPROPKEY *key, char *out, size_t cap)
{
  wchar_t w[1024];
  out[0] = 0;
  if (query_prop(devs, info, key, w, sizeof(w)))
    wide_to_utf8_buf(w, out, cap);
}

static void prop_multi(HDEVINFO devs, SP_DEVINFO_DATA *info, const DEVPROPKEY *key, strlist_t *sl)
{
  wchar_t w[4096];
  if (!query_prop(devs, info, key, w, sizeof(w)))
    return;
  for (const wchar_t *p = w; *p; p += wcslen(p) + 1)
  {
    char *s = wide_to_utf8(p);
    sl_add_unique(sl, s);
    free(s);
  }
}

static void collect_resources(dm_dev_t *d, DEVINST inst)
{
  LOG_CONF   lc = 0;
  RES_DES    rd, next;
  RESOURCEID rid;

  if (CM_Get_First_Log_Conf(&lc, inst, ALLOC_LOG_CONF) != CR_SUCCESS &&
      CM_Get_First_Log_Conf(&lc, inst, BOOT_LOG_CONF) != CR_SUCCESS)
    return;

  rd = (RES_DES)lc;
  while (CM_Get_Next_Res_Des(&next, rd, ResType_All, &rid, 0) == CR_SUCCESS)
  {
    ULONG sz = 0;
    if (rd != (RES_DES)lc)
      CM_Free_Res_Des_Handle(rd);
    rd = next;
    if (CM_Get_Res_Des_Data_Size(&sz, rd, 0) == CR_SUCCESS && sz > 0)
    {
      BYTE *data = xmalloc(sz);
      if (CM_Get_Res_Des_Data(rd, data, sz, 0) == CR_SUCCESS)
      {
        if (rid == ResType_Mem && sz >= sizeof(MEM_DES) && d->nmem < 8)
        {
          const MEM_DES *m = (const MEM_DES *)data;
          d->mem_base[d->nmem] = m->MD_Alloc_Base;
          d->mem_end[d->nmem] = m->MD_Alloc_End;
          d->nmem++;
        }
        else if (rid == ResType_MemLarge && sz >= sizeof(MEM_LARGE_DES) && d->nmem < 8)
        {
          const MEM_LARGE_DES *m = (const MEM_LARGE_DES *)data;
          d->mem_base[d->nmem] = m->MLD_Alloc_Base;
          d->mem_end[d->nmem] = m->MLD_Alloc_End;
          d->nmem++;
        }
        else if (rid == ResType_IRQ && sz >= sizeof(IRQ_DES) && d->nirq < 8)
        {
          const IRQ_DES *q = (const IRQ_DES *)data;
          d->irqs[d->nirq++] = (uint32_t)q->IRQD_Alloc_Num;
        }
      }
      free(data);
    }
  }
  if (rd != (RES_DES)lc)
    CM_Free_Res_Des_Handle(rd);
  CM_Free_Log_Conf_Handle(lc);
}

/* Device Parameters\Resources: BinaryPath / ScfgBinaryPath (Qualcomm drivers) */
static void read_resource_binaries(HDEVINFO devs, SP_DEVINFO_DATA *info, dm_dev_t *d)
{
  HKEY key = SetupDiOpenDevRegKey(devs, info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
  HKEY res;

  if (key == INVALID_HANDLE_VALUE)
    return;
  if (ERROR_SUCCESS == RegOpenKeyExW(key, L"Resources", 0, KEY_READ, &res))
  {
    static const wchar_t *names[2] = { L"BinaryPath", L"ScfgBinaryPath" };
    char                 *outs[2] = { d->res_bin, d->scfg_bin };
    for (int i = 0; i < 2; i++)
    {
      wchar_t buf[A2D_MAX_PATH];
      DWORD   len = sizeof(buf) - sizeof(wchar_t), type = 0;
      if (ERROR_SUCCESS == RegQueryValueExW(res, names[i], NULL, &type, (LPBYTE)buf, &len) &&
          (type == REG_SZ || type == REG_EXPAND_SZ))
      {
        buf[len / sizeof(wchar_t)] = 0;
        wide_to_utf8_buf(buf, outs[i], A2D_MAX_PATH);
      }
    }
    RegCloseKey(res);
  }
  RegCloseKey(key);
}

static void find_driver_store_dir(dm_dev_t *d)
{
  wchar_t  inf_path[MAX_PATH], ret[A2D_MAX_PATH];
  char     tmp[A2D_MAX_PATH], dir[A2D_MAX_PATH];
  wchar_t *winf;
  DWORD    req = 0;

  if (!d->inf[0])
    return;
  winf = utf8_to_wide(d->inf);
  GetWindowsDirectoryW(inf_path, MAX_PATH);
  wcsncat(inf_path, L"\\INF\\", MAX_PATH - wcslen(inf_path) - 1);
  wcsncat(inf_path, winf, MAX_PATH - wcslen(inf_path) - 1);
  free(winf);
  if (SetupGetInfDriverStoreLocationW(inf_path, NULL, NULL, ret, A2D_MAX_PATH, &req))
  {
    wide_to_utf8_buf(ret, tmp, sizeof(tmp));
    path_dirname(tmp, dir, sizeof(dir));
    str_copy(d->driver_store_dir, sizeof(d->driver_store_dir), dir);
  }
}

/* ======================================================================== */
/* monitors / EDID                                                          */
/* ======================================================================== */

/* text of an EDID display descriptor; empty if it holds binary data (some vendors do) */
static void edid_text(const uint8_t *d, char *out, size_t cap)
{
  size_t n = 0;
  for (int i = 5; i < 18 && n < cap - 1; i++)
  {
    if (d[i] == 0x0A || d[i] == 0)
      break;
    if (d[i] < 0x20 || d[i] >= 0x7F)
    {
      n = 0;
      break;
    }
    out[n++] = (char)d[i];
  }
  while (n && out[n - 1] == ' ')
    n--;
  out[n] = 0;
}

static bool decode_edid(const uint8_t *e, size_t len, dm_monitor_t *m)
{
  static const uint8_t hdr[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
  static const char   *panel_makers[] = { "BOE", "SDC", "AUO", "LGD", "CMN", "IVO", "CSO", "SHP", "TMA",
                                          "NCP", "STA", "MNC", "TL0", "MSF", "ATN", "CSW", "KDB", "HKC" };
  uint16_t             v;
  int                  ntext = 0;

  if (len < 128 || memcmp(e, hdr, 8))
    return false;
  v = (uint16_t)((e[8] << 8) | e[9]);
  m->pnp_id[0] = (char)('@' + ((v >> 10) & 0x1F));
  m->pnp_id[1] = (char)('@' + ((v >> 5) & 0x1F));
  m->pnp_id[2] = (char)('@' + (v & 0x1F));
  m->pnp_id[3] = 0;
  m->product = (uint16_t)(e[10] | (e[11] << 8));
  m->digital = (e[20] & 0x80) != 0;
  m->interface_type = e[20] & 0x0F;
  m->width_cm = e[21];
  m->height_cm = e[22];
  m->diagonal_in = (m->width_cm && m->height_cm)
                     ? sqrt((double)(m->width_cm * m->width_cm + m->height_cm * m->height_cm)) / 2.54
                     : 0.0;
  for (int d = 54; d + 18 <= 126; d += 18)
  {
    const uint8_t *p = e + d;
    if (p[0] || p[1])
      continue; /* detailed timing */
    if (p[3] == 0xFC)
      edid_text(p, m->name, sizeof(m->name));
    else if (p[3] == 0xFE && ntext < 2)
      edid_text(p, m->text[ntext++], sizeof(m->text[0]));
  }
  m->internal = m->digital && m->interface_type == 5 && m->diagonal_in > 0.0 && m->diagonal_in < 20.0;
  if (!m->internal && m->diagonal_in > 0.0 && m->diagonal_in < 20.0)
    for (size_t i = 0; i < sizeof(panel_makers) / sizeof(panel_makers[0]); i++)
      if (!strcmp(m->pnp_id, panel_makers[i]))
        m->internal = true;
  return true;
}

static void collect_monitors(dm_set_t *set)
{
  HDEVINFO        devs = SetupDiGetClassDevsW(&GUID_DEVCLASS_MONITOR, NULL, NULL, 0); /* also non-present */
  SP_DEVINFO_DATA info;

  if (devs == INVALID_HANDLE_VALUE)
    return;
  ZeroMemory(&info, sizeof(info));
  info.cbSize = sizeof(info);
  for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &info); ++i)
  {
    dm_monitor_t m;
    wchar_t      winst[512];
    HKEY         key;
    ULONG        st = 0, pr = 0;

    memset(&m, 0, sizeof(m));
    if (SetupDiGetDeviceInstanceIdW(devs, &info, winst, 512, NULL))
      wide_to_utf8_buf(winst, m.instance_id, sizeof(m.instance_id));
    m.present = CM_Get_DevNode_Status(&st, &pr, info.DevInst, 0) == CR_SUCCESS;
    key = SetupDiOpenDevRegKey(devs, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (key != INVALID_HANDLE_VALUE)
    {
      uint8_t edid[512];
      DWORD   len = sizeof(edid), type = 0;
      if (ERROR_SUCCESS == RegQueryValueExW(key, L"EDID", NULL, &type, edid, &len) && type == REG_BINARY &&
          decode_edid(edid, len, &m))
        VEC_PUSH(set->mon, set->nmon, set->capmon, m);
      RegCloseKey(key);
    }
  }
  SetupDiDestroyDeviceInfoList(devs);
}

const dm_monitor_t *devmgr_internal_panel(const dm_set_t *set)
{
  const dm_monitor_t *best = NULL;
  if (!set)
    return NULL;
  for (int i = 0; i < set->nmon; i++)
  {
    const dm_monitor_t *m = &set->mon[i];
    if (!m->internal)
      continue;
    /* prefer eDP, then a present one */
    if (!best || (m->interface_type == 5 && best->interface_type != 5) || (m->present && !best->present))
      best = m;
  }
  return best;
}

const dm_dev_t *devmgr_find_pci(const dm_set_t *set, uint16_t ven, uint16_t dev)
{
  char id[32];
  if (!set)
    return NULL;
  snprintf(id, sizeof(id), "PCI\\VEN_%04X&DEV_%04X", ven, dev);
  for (int i = 0; i < set->count; i++)
    if (str_starts_with_ci(set->d[i].instance_id, id))
      return &set->d[i];
  return NULL;
}

static void write_json(const a2d_config_t *cfg, const dm_set_t *set)
{
  char  path[A2D_MAX_PATH];
  FILE *fp;
  wchar_t *wp;

  path_join(path, sizeof(path), cfg->work_dir, "hardware_inventory.json");
  wp = utf8_to_wide(path);
  fp = _wfopen(wp, L"wb");
  free(wp);
  if (!fp)
  {
    log_warn("cannot write %s", path);
    return;
  }
  fputs("{\n  \"devices\": [\n", fp);
  for (int i = 0; i < set->count; i++)
  {
    const dm_dev_t *d = &set->d[i];
    fputs(i ? ",\n    { " : "    { ", fp);
    fputs("\"instanceId\": ", fp);        json_escape_fp(fp, d->instance_id);
    fputs(", \"friendlyName\": ", fp);    json_escape_fp(fp, d->name);
    fputs(", \"className\": ", fp);       json_escape_fp(fp, d->cls);
    fputs(", \"hardwareIds\": [", fp);
    for (int k = 0; k < d->hwids.count; k++) { if (k) fputs(", ", fp); json_escape_fp(fp, d->hwids.v[k]); }
    fputs("], \"compatibleIds\": [", fp);
    for (int k = 0; k < d->cids.count; k++) { if (k) fputs(", ", fp); json_escape_fp(fp, d->cids.v[k]); }
    fputs("], \"driverInf\": ", fp);      json_escape_fp(fp, d->inf);
    fputs(", \"driverProvider\": ", fp);  json_escape_fp(fp, d->provider);
    fputs(", \"driverVersion\": ", fp);   json_escape_fp(fp, d->driver_version);
    fputs(", \"service\": ", fp);         json_escape_fp(fp, d->service);
    fputs(", \"driverStore\": ", fp);     json_escape_fp(fp, d->driver_store_dir);
    fputs(", \"locationInfo\": ", fp);    json_escape_fp(fp, d->location);
    fputs(", \"biosDeviceName\": ", fp);  json_escape_fp(fp, d->bios_name);
    if (d->res_bin[0] || d->scfg_bin[0])
    {
      fputs(", \"resourceBinary\": ", fp);  json_escape_fp(fp, d->res_bin);
      fputs(", \"scfgBinary\": ", fp);      json_escape_fp(fp, d->scfg_bin);
    }
    fprintf(fp, ", \"problem\": %lu, \"status\": ", (unsigned long)d->problem);
    json_escape_fp(fp, devmgr_status_text(d));
    fputs(", \"mmioRanges\": [", fp);
    for (int k = 0; k < d->nmem; k++)
      fprintf(fp, "%s\"0x%llx-0x%llx\"", k ? ", " : "", (unsigned long long)d->mem_base[k],
              (unsigned long long)d->mem_end[k]);
    fputs("], \"irqs\": [", fp);
    for (int k = 0; k < d->nirq; k++)
      fprintf(fp, "%s%u", k ? ", " : "", d->irqs[k]);
    fputs("] }", fp);
  }
  fputs("\n  ],\n  \"monitors\": [\n", fp);
  for (int i = 0; i < set->nmon; i++)
  {
    const dm_monitor_t *m = &set->mon[i];
    fputs(i ? ",\n    { " : "    { ", fp);
    fputs("\"instanceId\": ", fp); json_escape_fp(fp, m->instance_id);
    fputs(", \"edidVendor\": ", fp); json_escape_fp(fp, m->pnp_id);
    fprintf(fp, ", \"edidProduct\": \"0x%04x\", \"name\": ", m->product);
    json_escape_fp(fp, m->name);
    fputs(", \"text\": [", fp); json_escape_fp(fp, m->text[0]); fputs(", ", fp); json_escape_fp(fp, m->text[1]);
    fprintf(fp, "], \"sizeCm\": [%d, %d], \"diagonalInch\": %.1f, \"displayPort\": %s, \"present\": %s, "
                "\"internal\": %s }",
            m->width_cm, m->height_cm, m->diagonal_in, m->interface_type == 5 ? "true" : "false",
            m->present ? "true" : "false", m->internal ? "true" : "false");
  }
  fputs("\n  ]\n}\n", fp);
  fclose(fp);
}

int devmgr_collect(const a2d_config_t *cfg, dm_set_t *set)
{
  HDEVINFO        devs;
  SP_DEVINFO_DATA info;

  memset(set, 0, sizeof(*set));
  devs = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
  if (devs == INVALID_HANDLE_VALUE)
  {
    log_err("SetupDiGetClassDevsW failed (%lu)", GetLastError());
    return 0;
  }

  ZeroMemory(&info, sizeof(info));
  info.cbSize = sizeof(info);
  for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &info); ++i)
  {
    dm_dev_t d;
    wchar_t  winst[512];
    char     bios[256];
    ULONG    st = 0, pr = 0;

    memset(&d, 0, sizeof(d));
    if (SetupDiGetDeviceInstanceIdW(devs, &info, winst, 512, NULL))
      wide_to_utf8_buf(winst, d.instance_id, sizeof(d.instance_id));
    prop_utf8(devs, &info, &DEVPKEY_Device_FriendlyName, d.name, sizeof(d.name));
    if (!d.name[0])
      prop_utf8(devs, &info, &DEVPKEY_Device_DeviceDesc, d.name, sizeof(d.name));
    prop_utf8(devs, &info, &DEVPKEY_Device_Class, d.cls, sizeof(d.cls));
    prop_multi(devs, &info, &DEVPKEY_Device_HardwareIds, &d.hwids);
    prop_multi(devs, &info, &DEVPKEY_Device_CompatibleIds, &d.cids);
    prop_utf8(devs, &info, &DEVPKEY_Device_DriverInfPath, d.inf, sizeof(d.inf));
    prop_utf8(devs, &info, &DEVPKEY_Device_DriverInfSection, d.inf_section, sizeof(d.inf_section));
    prop_utf8(devs, &info, &DEVPKEY_Device_DriverProvider, d.provider, sizeof(d.provider));
    prop_utf8(devs, &info, &DEVPKEY_Device_DriverVersion, d.driver_version, sizeof(d.driver_version));
    prop_utf8(devs, &info, &DEVPKEY_Device_Service, d.service, sizeof(d.service));
    prop_utf8(devs, &info, &DEVPKEY_Device_LocationInfo, d.location, sizeof(d.location));
    prop_utf8(devs, &info, &DEVPKEY_Device_BiosDeviceName, bios, sizeof(bios));
    if (bios[0] == '\\')
      aml_canon_path(bios, d.bios_name, sizeof(d.bios_name));
    if (CM_Get_DevNode_Status(&st, &pr, info.DevInst, 0) == CR_SUCCESS)
    {
      d.status = st;
      d.problem = (st & DN_HAS_PROBLEM) ? pr : 0;
    }
    collect_resources(&d, info.DevInst);
    read_resource_binaries(devs, &info, &d);
    if (!cfg->no_drivers)
      find_driver_store_dir(&d);
    VEC_PUSH(set->d, set->count, set->cap, d);
  }
  SetupDiDestroyDeviceInfoList(devs);

  collect_monitors(set);
  write_json(cfg, set);
  return set->count;
}

void devmgr_free(dm_set_t *set)
{
  for (int i = 0; i < set->count; i++)
  {
    sl_free(&set->d[i].hwids);
    sl_free(&set->d[i].cids);
  }
  free(set->d);
  free(set->mon);
  memset(set, 0, sizeof(*set));
}

const dm_dev_t *devmgr_find_by_acpi_path(const dm_set_t *set, const char *acpi_path)
{
  if (!set || !acpi_path || !*acpi_path)
    return NULL;
  for (int i = 0; i < set->count; i++)
    if (set->d[i].bios_name[0] && 0 == _stricmp(set->d[i].bios_name, acpi_path))
      return &set->d[i];
  return NULL;
}

const dm_dev_t *devmgr_find_by_hwid_substr(const dm_set_t *set, const char *substr)
{
  if (!set)
    return NULL;
  for (int i = 0; i < set->count; i++)
    for (int k = 0; k < set->d[i].hwids.count; k++)
      if (str_contains_ci(set->d[i].hwids.v[k], substr))
        return &set->d[i];
  return NULL;
}

const char *devmgr_status_text(const dm_dev_t *d)
{
  static char buf[64];
  if (!d)
    return "not present";
  if (0 == d->problem)
    return (d->status & DN_STARTED) ? "OK" : "present, not started";
  switch (d->problem)
  {
    case CM_PROB_NOT_CONFIGURED:  return "problem 1: not configured";
    case CM_PROB_FAILED_START:    return "problem 10: failed to start";
    case CM_PROB_DISABLED:        return "problem 22: disabled";
    case CM_PROB_FAILED_INSTALL:  return "problem 28: no driver";
    case CM_PROB_HARDWARE_DISABLED: return "problem 29: disabled by firmware";
    default:
      snprintf(buf, sizeof(buf), "problem %lu", (unsigned long)d->problem);
      return buf;
  }
}
