/* SPDX-License-Identifier: MIT */
/**
 * @file   acpi_extractor.c
 * @author Ingo A. Kubbilun
 * @brief  ACPI table acquisition without ACPICA (pure Windows API), SMBIOS,
 *         MADT evaluation and SoC / board identity detection
 *
 * Sources of ACPI tables (in this order):
 *   1. a dump folder given by --acpi-dir (offline mode, e.g. acpidump output)
 *   2. EnumSystemFirmwareTables / GetSystemFirmwareTable('ACPI')
 *   3. HKLM\\HARDWARE\\ACPI\\<SIG\>\\<OEM\>\\<TABLE\>\\<REV\> (adds SSDT instances the
 *      firmware table API cannot return)
 *
 * [MIT license] - see main.c
 */

#include "acpi_extractor.h"

#pragma pack(push, 1)
/** @brief common header of ACPI system description tables */
typedef struct {
  char     signature[4];  /**< table signature */
  uint32_t length;  /**< table length incl. header */
  uint8_t  revision;  /**< table revision */
  uint8_t  checksum;  /**< checksum byte */
  char     oem_id[6];  /**< OEM ID */
  char     oem_table_id[8];  /**< OEM table ID */
  uint32_t oem_revision;  /**< OEM revision */
  char     creator_id[4];  /**< ASL compiler vendor ID */
  uint32_t creator_revision;  /**< ASL compiler revision */
} acpi_sdt_header_t;
#pragma pack(pop)

/* ======================================================================== */
/* table set                                                                */
/* ======================================================================== */

/**
 * @brief Checks whether 4 bytes form a plausible ACPI signature.
 *
 * @param s  signature
 * @return true if all characters are alphanumeric or '_'
 */
static bool sig_valid(const char *s)
{
  for (int i = 0; i < 4; i++)
    if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' ||
          (s[i] >= 'a' && s[i] <= 'z')))
      return false;
  return true;
}

/**
 * @brief Checks whether an identical table is already in the set.
 *
 * @param set  table set
 * @param buf  table bytes
 * @param len  table length
 * @return true for a duplicate
 */
static bool table_is_duplicate(const acpi_table_set_t *set, const uint8_t *buf, uint32_t len)
{
  for (int i = 0; i < set->count; i++)
    if (set->t[i].len == len && 0 == memcmp(set->t[i].data, buf, len))
      return true;
  return false;
}

/**
 * @brief Validates a table and adds a copy to the set (duplicates are ignored).
 *
 * @param set     table set
 * @param buf     table bytes
 * @param len     length of the buffer
 * @param origin  description of the source
 * @return true if the table was added
 */
static bool add_table(acpi_table_set_t *set, const uint8_t *buf, uint32_t len, const char *origin)
{
  const acpi_sdt_header_t *hdr = (const acpi_sdt_header_t *)buf;
  acpi_table_t             t;

  if (!buf || len < 4)
    return false;
  /* FACS has a shorter header; everything else has the 36-byte SDT header */
  if (len < sizeof(acpi_sdt_header_t) && memcmp(buf, "FACS", 4) != 0)
    return false;
  if (!sig_valid((const char *)buf))
    return false;
  if (len >= sizeof(acpi_sdt_header_t) && hdr->length > len)
    return false;
  if (len >= sizeof(acpi_sdt_header_t) && hdr->length >= sizeof(acpi_sdt_header_t))
    len = hdr->length;
  if (table_is_duplicate(set, buf, len))
    return false;

  memset(&t, 0, sizeof(t));
  memcpy(t.sig, buf, 4);
  if (len >= sizeof(acpi_sdt_header_t))
  {
    memcpy(t.oem_id, hdr->oem_id, 6);
    memcpy(t.oem_table_id, hdr->oem_table_id, 8);
    t.oem_revision = hdr->oem_revision;
    for (int i = 5; i >= 0 && (t.oem_id[i] == ' ' || t.oem_id[i] == 0); i--) t.oem_id[i] = 0;
    for (int i = 7; i >= 0 && (t.oem_table_id[i] == ' ' || t.oem_table_id[i] == 0); i--) t.oem_table_id[i] = 0;
  }
  t.data = xmalloc(len);
  memcpy(t.data, buf, len);
  t.len = len;
  for (int i = 0; i < set->count; i++)
    if (0 == memcmp(set->t[i].sig, t.sig, 4))
      t.instance++;
  str_copy(t.origin, sizeof(t.origin), origin);
  {
    char lower[5];
    memcpy(lower, t.sig, 5);
    str_lower(lower);
    if (t.instance)
      snprintf(t.file_name, sizeof(t.file_name), "%s%d.dat", lower, t.instance);
    else
      snprintf(t.file_name, sizeof(t.file_name), "%s.dat", lower);
  }
  VEC_PUSH(set->t, set->count, set->cap, t);
  return true;
}

const acpi_table_t *acpi_find_table(const acpi_table_set_t *set, const char *sig, int instance)
{
  for (int i = 0; i < set->count; i++)
    if (0 == memcmp(set->t[i].sig, sig, 4) && set->t[i].instance == instance)
      return &set->t[i];
  return NULL;
}

void acpi_free_tables(acpi_table_set_t *set)
{
  for (int i = 0; i < set->count; i++)
    free(set->t[i].data);
  free(set->t);
  memset(set, 0, sizeof(*set));
}

/* ======================================================================== */
/* acquisition                                                              */
/* ======================================================================== */

/**
 * @brief Reads all tables offered by EnumSystemFirmwareTables('ACPI').
 *
 * @param set  table set
 */
static void collect_from_firmware_api(acpi_table_set_t *set)
{
  UINT   needed = EnumSystemFirmwareTables('ACPI', NULL, 0);
  DWORD *ids;
  UINT   count;

  if (0 == needed)
  {
    log_warn("EnumSystemFirmwareTables('ACPI') failed (error %lu)", GetLastError());
    return;
  }
  ids = xmalloc(needed);
  count = EnumSystemFirmwareTables('ACPI', ids, needed) / sizeof(DWORD);
  for (UINT i = 0; i < count; ++i)
  {
    UINT sz = GetSystemFirmwareTable('ACPI', ids[i], NULL, 0);
    if (sz > 0)
    {
      uint8_t *buf = xmalloc(sz);
      if (GetSystemFirmwareTable('ACPI', ids[i], buf, sz) == sz)
        add_table(set, buf, sz, "GetSystemFirmwareTable");
      free(buf);
    }
  }
  free(ids);
}

/**
 * @brief Adds REG_BINARY tables below a registry key (recursive).
 *
 * @param set    table set
 * @param key    registry key
 * @param depth  recursion depth
 */
static void collect_registry_values(acpi_table_set_t *set, HKEY key, int depth)
{
  wchar_t name[256];
  DWORD   idx;

  /* values at this level */
  for (idx = 0;; idx++)
  {
    DWORD    nlen = 256, type = 0, dlen = 0;
    LSTATUS  st = RegEnumValueW(key, idx, name, &nlen, NULL, &type, NULL, &dlen);
    if (st != ERROR_SUCCESS)
      break;
    if (type == REG_BINARY && dlen >= sizeof(acpi_sdt_header_t))
    {
      uint8_t *buf = xmalloc(dlen);
      DWORD    l2 = dlen;
      if (ERROR_SUCCESS == RegQueryValueExW(key, name, NULL, &type, buf, &l2))
        add_table(set, buf, l2, "registry HKLM\\HARDWARE\\ACPI");
      free(buf);
    }
  }
  if (depth >= 4)
    return;
  for (idx = 0;; idx++)
  {
    DWORD nlen = 256;
    HKEY  sub;
    if (ERROR_SUCCESS != RegEnumKeyExW(key, idx, name, &nlen, NULL, NULL, NULL, NULL))
      break;
    if (ERROR_SUCCESS == RegOpenKeyExW(key, name, 0, KEY_READ, &sub))
    {
      collect_registry_values(set, sub, depth + 1);
      RegCloseKey(sub);
    }
  }
}

/**
 * @brief Adds the tables stored below HKLM\\HARDWARE\\ACPI (additional SSDTs).
 *
 * @param set  table set
 */
static void collect_from_registry(acpi_table_set_t *set)
{
  HKEY key;
  if (ERROR_SUCCESS == RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\ACPI", 0, KEY_READ, &key))
  {
    collect_registry_values(set, key, 0);
    RegCloseKey(key);
  }
}

/** @brief context of the dump folder walk */
typedef struct {
  acpi_table_set_t *set;  /**< receives the tables */
} folder_ctx_t;

/**
 * @brief walk_dir() callback adding every table file of a dump folder.
 *
 * @param full    full path
 * @param name    file name
 * @param is_dir  true for a directory
 * @param vctx    folder_ctx_t
 * @return false for directories (no recursion), true otherwise
 */
static bool folder_cb(const char *full, const char *name, bool is_dir, void *vctx)
{
  folder_ctx_t *ctx = vctx;
  size_t        len;
  uint8_t      *buf;
  char          origin[96];

  if (is_dir)
    return false;
  if (str_ends_with_ci(name, ".dsl") || str_ends_with_ci(name, ".txt") || str_ends_with_ci(name, ".md"))
    return true;
  buf = read_file(full, &len);
  if (buf && len >= 4 && len < (64u << 20))
  {
    snprintf(origin, sizeof(origin), "file %s", name);
    if (!add_table(ctx->set, buf, (uint32_t)len, origin))
      log_verbose("skipping %s (not an ACPI table or duplicate)", name);
  }
  free(buf);
  return true;
}

/**
 * @brief Returns the external iasl given by --iasl.
 *
 * @param cfg  configuration
 * @param out  output buffer
 * @param cap  size of the output buffer
 * @return out, or NULL if no external iasl is to be used
 */
static const char *find_iasl(const a2d_config_t *cfg, char *out, size_t cap)
{
  if (cfg->no_iasl)
    return NULL;
  if (cfg->iasl_exe)
  {
    if (file_exists(cfg->iasl_exe))
    {
      str_copy(out, cap, cfg->iasl_exe);
      return out;
    }
    log_warn("--iasl %s does not exist", cfg->iasl_exe);
    return NULL;
  }
  /* without --iasl the embedded ACPICA disassembler is used (acpica_host.c) */
  return NULL;
}

int acpi_collect_tables(const a2d_config_t *cfg, acpi_table_set_t *set)
{
  char acpi_dir[A2D_MAX_PATH];
  char iasl[A2D_MAX_PATH];

  memset(set, 0, sizeof(*set));
  if (cfg->acpi_dir)
  {
    folder_ctx_t ctx = { set };
    if (!dir_exists(cfg->acpi_dir))
    {
      log_err("ACPI dump folder %s does not exist", cfg->acpi_dir);
      return 0;
    }
    walk_dir(cfg->acpi_dir, false, folder_cb, &ctx);
    log_info("read %d ACPI tables from folder %s", set->count, cfg->acpi_dir);
  }
  else
  {
    collect_from_firmware_api(set);
    {
      int before = set->count;
      collect_from_registry(set);
      if (set->count > before)
        log_info("registry HKLM\\HARDWARE\\ACPI contributed %d additional table(s)", set->count - before);
    }
    log_info("read %d ACPI tables from the running machine", set->count);
  }

  if (!acpi_find_table(set, "DSDT", 0))
    log_warn("no DSDT found - device discovery from ACPI will be empty");

  /* dump */
  path_join(acpi_dir, sizeof(acpi_dir), cfg->work_dir, "acpi");
  mkdir_p(acpi_dir);
  for (int i = 0; i < set->count; i++)
  {
    char p[A2D_MAX_PATH];
    path_join(p, sizeof(p), acpi_dir, set->t[i].file_name);
    if (!write_file(p, set->t[i].data, set->t[i].len))
      log_warn("cannot write %s", p);
    log_verbose("%-4s %-12s OEM=%-6s TableID=%-8s %7u bytes (%s)", set->t[i].sig, set->t[i].file_name,
                set->t[i].oem_id, set->t[i].oem_table_id, set->t[i].len, set->t[i].origin);
  }

  /* optional disassembly for the human reviewer */
  if (find_iasl(cfg, iasl, sizeof(iasl)))
  {
    int ok = 0;
    for (int i = 0; i < set->count; i++)
    {
      char cmd[A2D_MAX_PATH * 2];
      if (0 != memcmp(set->t[i].sig, "DSDT", 4) && 0 != memcmp(set->t[i].sig, "SSDT", 4))
        continue;
      snprintf(cmd, sizeof(cmd), "\"%s\" -d \"%s\"", iasl, set->t[i].file_name);
      if (0 == run_process(cmd, acpi_dir, 180000))
        ok++;
    }
    log_info("iasl (%s) disassembled %d DSDT/SSDT table(s) to %s\\*.dsl", iasl, ok, acpi_dir);
  }
  else
    log_verbose("no iasl/AslCompiler.exe found - skipping .dsl disassembly (use --iasl)");

  return set->count;
}

/* ======================================================================== */
/* SMBIOS                                                                   */
/* ======================================================================== */

/**
 * @brief Returns string number idx of an SMBIOS structure.
 *
 * @param s    start of the string set
 * @param end  end of the SMBIOS data
 * @param idx  1-based string index (0: none)
 * @return pointer to a static buffer (8 rotating buffers)
 */
static const char *smbios_string(const uint8_t *s, const uint8_t *end, uint8_t idx)
{
  static char out[8][192];
  static int  slot = 0;
  char       *o = out[slot = (slot + 1) & 7];

  o[0] = 0;
  if (0 == idx)
    return o;
  while (s < end && *s)
  {
    if (--idx == 0)
    {
      size_t n = 0;
      while (s + n < end && s[n] && n < 191)
        n++;
      memcpy(o, s, n);
      o[n] = 0;
      return str_trim(o);
    }
    while (s < end && *s)
      s++;
    s++;
  }
  return o;
}

/**
 * @brief Reads SMBIOS types 0, 1 and 2 (BIOS, system, board).
 *
 * @param si  receives the strings
 */
static void parse_smbios(sysinfo_t *si)
{
  UINT     sz = GetSystemFirmwareTable('RSMB', 0, NULL, 0);
  uint8_t *buf;
  uint8_t *p, *end;

  if (0 == sz)
  {
    log_warn("SMBIOS (RSMB) not available");
    return;
  }
  buf = xmalloc(sz);
  if (GetSystemFirmwareTable('RSMB', 0, buf, sz) != sz || sz < 8)
  {
    free(buf);
    return;
  }
  p = buf + 8;
  end = buf + 8 + *(uint32_t *)(buf + 4);
  if (end > buf + sz)
    end = buf + sz;

  while (p + 4 <= end)
  {
    uint8_t        type = p[0];
    uint8_t        len = p[1];
    const uint8_t *strs = p + len;
    const uint8_t *next = strs;

    if (len < 4 || p + len > end)
      break;
    while (next + 1 < end && !(next[0] == 0 && next[1] == 0))
      next++;
    next += 2;

    switch (type)
    {
      case 0:
        if (len > 5) str_copy(si->bios_version, sizeof(si->bios_version), smbios_string(strs, end, p[5]));
        break;
      case 1:
        if (len > 4) str_copy(si->manufacturer, sizeof(si->manufacturer), smbios_string(strs, end, p[4]));
        if (len > 5) str_copy(si->product, sizeof(si->product), smbios_string(strs, end, p[5]));
        if (len > 6) str_copy(si->version, sizeof(si->version), smbios_string(strs, end, p[6]));
        if (len > 0x19) str_copy(si->sku, sizeof(si->sku), smbios_string(strs, end, p[0x19]));
        if (len > 0x1A) str_copy(si->family, sizeof(si->family), smbios_string(strs, end, p[0x1A]));
        break;
      case 2:
        if (!si->board_product[0])
        {
          if (len > 4) str_copy(si->board_vendor, sizeof(si->board_vendor), smbios_string(strs, end, p[4]));
          if (len > 5) str_copy(si->board_product, sizeof(si->board_product), smbios_string(strs, end, p[5]));
        }
        break;
      case 127:
        p = end;
        continue;
      default:
        break;
    }
    p = (uint8_t *)next;
  }
  free(buf);
}

/* ======================================================================== */
/* MADT / CPU / SoC                                                         */
/* ======================================================================== */

/**
 * @brief Counts the GIC CPU interfaces of the MADT.
 *
 * @param set  ACPI tables
 * @param si   receives total and enabled counts
 */
static void parse_madt(const acpi_table_set_t *set, sysinfo_t *si)
{
  const acpi_table_t *t = acpi_find_table(set, "APIC", 0);
  uint32_t            off = 44;

  if (!t)
    return;
  while (off + 2 <= t->len)
  {
    uint8_t type = t->data[off];
    uint8_t len = t->data[off + 1];
    if (len < 2 || off + len > t->len)
      break;
    if (type == 0x0B && len >= 16)
    {
      uint32_t flags = *(const uint32_t *)(t->data + off + 12);
      si->madt_gicc_total++;
      if (flags & 1)
        si->madt_gicc_enabled++;
    }
    off += len;
  }
}

/**
 * @brief Reads ProcessorNameString of CPU 0 from the registry.
 *
 * @param si  receives the CPU name
 */
static void read_cpu_name(sysinfo_t *si)
{
  HKEY    key;
  wchar_t buf[256];
  DWORD   len = sizeof(buf), type = 0;

  if (ERROR_SUCCESS == RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                     0, KEY_READ, &key))
  {
    if (ERROR_SUCCESS == RegQueryValueExW(key, L"ProcessorNameString", NULL, &type, (LPBYTE)buf, &len) &&
        type == REG_SZ)
    {
      buf[255] = 0;
      wide_to_utf8_buf(buf, si->cpu_name, sizeof(si->cpu_name));
      str_trim(si->cpu_name);
    }
    RegCloseKey(key);
  }
}

/** @brief CPU name token -> SoC name */
typedef struct {
  const char *token;   /**< normalized (upper-case alnum) substring of the CPU name */
  const char *soc;  /**< SoC name */
} soc_map_t;

/** CPU name tokens -> SoC */
static const soc_map_t g_soc_map[] = {
  { "X1E84100", "x1e84100" },
  { "X1E80100", "x1e80100" },
  { "X1E78100", "x1e78100" },
  { "X1E001DE", "x1e001de" },
  { "X1P66100", "x1p66100" },
  { "X1P64100", "x1p64100" },
  { "X1P46100", "x1p46100" },
  { "X1P42100", "x1p42100" },
  { "X126100",  "x1p42100" },   /* "Snapdragon X X1-26-100" = Purwa / X1P42100 class */
  { "X186100",  "x1e80100" },
  { "X185100",  "x1e80100" },
  { "SC8280XP", "sc8280xp" },
  { "8CXGEN3",  "sc8280xp" },
  { "SC8180X",  "sc8180x"  },
  { "8CXGEN2",  "sc8180x"  },
  { "7CXGEN3",  "sc8280xp" },
};

/**
 * @brief Determines the SoC from the CPU name, falling back to the MADT core count.
 *
 * @param cfg  configuration (--soc)
 * @param si   system information (soc, soc_reason updated)
 */
static void detect_soc(const a2d_config_t *cfg, sysinfo_t *si)
{
  char norm[128];
  int  n = 0;

  if (cfg->soc)
  {
    str_copy(si->soc, sizeof(si->soc), cfg->soc);
    str_lower(si->soc);
    snprintf(si->soc_reason, sizeof(si->soc_reason), "command line --soc");
    return;
  }
  for (const char *p = si->cpu_name; *p && n < (int)sizeof(norm) - 1; p++)
  {
    char c = *p;
    if (c >= 'a' && c <= 'z')
      c = (char)(c - 32);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
      norm[n++] = c;
  }
  norm[n] = 0;
  for (size_t i = 0; i < sizeof(g_soc_map) / sizeof(g_soc_map[0]); i++)
  {
    if (strstr(norm, g_soc_map[i].token))
    {
      str_copy(si->soc, sizeof(si->soc), g_soc_map[i].soc);
      snprintf(si->soc_reason, sizeof(si->soc_reason), "CPU name \"%s\"", si->cpu_name);
      return;
    }
  }
  /* fall back to the number of enabled cores in the MADT (Snapdragon X family) */
  switch (si->madt_gicc_enabled)
  {
    case 8:  str_copy(si->soc, sizeof(si->soc), "x1p42100"); break;
    case 10: str_copy(si->soc, sizeof(si->soc), "x1p64100"); break;
    case 12: str_copy(si->soc, sizeof(si->soc), "x1e80100"); break;
    default: break;
  }
  if (si->soc[0])
    snprintf(si->soc_reason, sizeof(si->soc_reason), "ACPI MADT: %d enabled GICC entries (guess)",
             si->madt_gicc_enabled);
}

/* ======================================================================== */
/* board identity                                                           */
/* ======================================================================== */

/** @brief SMBIOS manufacturer -> DT vendor prefix and firmware directory */
typedef struct {
  const char *match;   /**< case-insensitive prefix of SMBIOS manufacturer */
  const char *prefix;  /**< DT vendor prefix */
  const char *dir;     /**< firmware vendor directory as used upstream */
} vendor_map_t;

/** SMBIOS manufacturers -> DT vendor prefix / firmware directory */
static const vendor_map_t g_vendors[] = {
  { "ASUS",      "asus",      "ASUSTeK"   },
  { "LENOVO",    "lenovo",    "LENOVO"    },
  { "HP",        "hp",        "hp"        },
  { "Hewlett",   "hp",        "hp"        },
  { "Dell",      "dell",      "dell"      },
  { "Microsoft", "microsoft", "microsoft" },
  { "SAMSUNG",   "samsung",   "SAMSUNG"   },
  { "Acer",      "acer",      "acer"      },
  { "HUAWEI",    "huawei",    "HUAWEI"    },
  { "HONOR",     "honor",     "HONOR"     },
  { "MEDION",    "medion",    "medion"    },
  { "Qualcomm",  "qcom",      "qcom"      },
};

/**
 * @brief Checks whether a string consists of decimal digits only.
 *
 * @param s  string
 * @return true for a non-empty decimal number
 */
static bool is_number(const char *s)
{
  if (!*s)
    return false;
  for (; *s; s++)
    if (*s < '0' || *s > '9')
      return false;
  return true;
}

/**
 * @brief Derives DT vendor prefix, firmware directories, model and compatible from SMBIOS.
 *
 * @param cfg  configuration (overrides)
 * @param si   system information (updated)
 */
static void derive_identity(const a2d_config_t *cfg, sysinfo_t *si)
{
  char        first_word[64] = "";
  char        src[192];
  char       *tok, *ctx = NULL;
  char        toks[16][64];
  int         nt = 0;
  strbuf_t    slug, model;

  /* vendor */
  sscanf(si->manufacturer, "%63s", first_word);
  str_copy(si->vendor_prefix, sizeof(si->vendor_prefix), first_word);
  str_lower(si->vendor_prefix);
  str_copy(si->vendor_dir, sizeof(si->vendor_dir), first_word);
  for (size_t i = 0; i < sizeof(g_vendors) / sizeof(g_vendors[0]); i++)
  {
    if (str_starts_with_ci(si->manufacturer, g_vendors[i].match))
    {
      str_copy(si->vendor_prefix, sizeof(si->vendor_prefix), g_vendors[i].prefix);
      str_copy(si->vendor_dir, sizeof(si->vendor_dir), g_vendors[i].dir);
      break;
    }
  }
  if (!si->vendor_prefix[0])
    str_copy(si->vendor_prefix, sizeof(si->vendor_prefix), "unknown");
  if (!si->vendor_dir[0])
    str_copy(si->vendor_dir, sizeof(si->vendor_dir), "unknown");

  /* tokens of the family (fallback: product) with letter+number merging:
     "ASUS Vivobook S 16" -> ASUS, Vivobook, S16 */
  str_copy(src, sizeof(src), si->family[0] ? si->family : si->product);
  for (tok = strtok_s(src, " \t", &ctx); tok && nt < 16; tok = strtok_s(NULL, " \t", &ctx))
  {
    if (strchr(tok, '_'))
      continue;                                    /* "S3607QA_S3607QA" */
    if (si->board_product[0] && 0 == _stricmp(tok, si->board_product))
      continue;
    if (nt > 0 && is_number(tok) && strlen(toks[nt - 1]) == 1)
    {
      strncat(toks[nt - 1], tok, 60);              /* S + 16 -> S16 */
      continue;
    }
    str_copy(toks[nt++], 64, tok);
  }

  sb_init(&slug);
  sb_init(&model);
  for (int i = 0; i < nt; i++)
  {
    bool is_vendor = 0 == _stricmp(toks[i], first_word) || 0 == _stricmp(toks[i], si->vendor_prefix) ||
                     0 == _stricmp(toks[i], si->vendor_dir);
    if (model.len)
      sb_putc(&model, ' ');
    sb_puts(&model, toks[i]);
    if (is_vendor)
      continue;
    if (slug.len)
      sb_putc(&slug, '-');
    sb_puts(&slug, toks[i]);
  }
  if (si->board_product[0])
    sb_printf(&model, " (%s)", si->board_product);

  str_copy(si->board_slug, sizeof(si->board_slug), slug.buf ? slug.buf : "board");
  str_lower(si->board_slug);
  str_copy(si->model, sizeof(si->model), model.buf ? model.buf : "Unknown board");
  if (si->board_product[0])
    snprintf(si->compatible, sizeof(si->compatible), "%s,%s-%s", si->vendor_prefix, si->board_slug,
             si->board_product);
  else
    snprintf(si->compatible, sizeof(si->compatible), "%s,%s", si->vendor_prefix, si->board_slug);
  str_lower(si->compatible);
  sb_free(&slug);
  sb_free(&model);

  if (cfg->vendor_dir) str_copy(si->vendor_dir, sizeof(si->vendor_dir), cfg->vendor_dir);
  if (cfg->board_slug) str_copy(si->board_slug, sizeof(si->board_slug), cfg->board_slug);
  if (cfg->model)      str_copy(si->model, sizeof(si->model), cfg->model);
  if (cfg->compatible) str_copy(si->compatible, sizeof(si->compatible), cfg->compatible);
}

void sysinfo_collect(const a2d_config_t *cfg, const acpi_table_set_t *set, sysinfo_t *si)
{
  memset(si, 0, sizeof(*si));
  parse_smbios(si);
  read_cpu_name(si);
  parse_madt(set, si);
  detect_soc(cfg, si);
  derive_identity(cfg, si);
}
