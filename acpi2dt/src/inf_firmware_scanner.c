/* SPDX-License-Identifier: MIT */
/**
 * @file   inf_firmware_scanner.c
 * @author Ingo A. Kubbilun
 * @brief  scans the driver packages (DriverStore\\FileRepository) used by the
 *         present devices: INF metadata and firmware blobs (.mbn, .elf, ...);
 *         resolves firmware names wanted by a DTS and copies them into a
 *         /lib/firmware-like folder structure (+ SHA-256 manifest)
 *
 * [MIT license] - see main.c
 */

#include "inf_firmware_scanner.h"

/* ======================================================================== */
/* firmware classification                                                  */
/* ======================================================================== */

/**
 * @brief Checks a file name for firmware extensions (.mbn, .mdt, .bNN, .elf, .bin, ...).
 *
 * @param name  file name
 * @return true for firmware-like files
 */
static bool is_firmware_name(const char *name)
{
  static const char *exts[] = { ".mbn", ".mdt", ".elf", ".bin", ".tlv", ".jsn", ".fw", ".dtb", ".nvm" };
  const char        *dot = strrchr(name, '.');

  if (!dot)
    return false;
  for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
    if (0 == _stricmp(dot, exts[i]))
      return true;
  /* split images: .b00 .. .b99 */
  if (strlen(dot) == 4 && (dot[1] == 'b' || dot[1] == 'B') && dot[2] >= '0' && dot[2] <= '9' && dot[3] >= '0' &&
      dot[3] <= '9')
    return true;
  return false;
}

/**
 * @brief Detects ELF (with machine), MBN or FDT format of a firmware file.
 *
 * @param path  file path
 * @param name  file name
 * @param out   output buffer
 * @param cap   size of the output buffer
 */
static void detect_format(const char *path, const char *name, char *out, size_t cap)
{
  uint8_t hdr[64];
  size_t  n = 0;
  FILE   *f;
  wchar_t *w = utf8_to_wide(path);

  str_copy(out, cap, "raw");
  f = _wfopen(w, L"rb");
  free(w);
  if (!f)
    return;
  n = fread(hdr, 1, sizeof(hdr), f);
  fclose(f);
  if (n >= 20 && hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F')
  {
    uint16_t m = (uint16_t)(hdr[18] | (hdr[19] << 8));
    const char *mach = m == 183 ? "AArch64" : m == 164 ? "Hexagon" : m == 40 ? "ARM" : m == 243 ? "RISC-V" : "other";
    snprintf(out, cap, "%s %s", hdr[4] == 2 ? "ELF64" : "ELF32", mach);
  }
  else if (str_ends_with_ci(name, ".mbn"))
    str_copy(out, cap, "MBN");
  else if (n >= 4 && hdr[0] == 0xD0 && hdr[1] == 0x0D && hdr[2] == 0xFE && hdr[3] == 0xED)
    str_copy(out, cap, "FDT");
}

/* ======================================================================== */
/* INF parsing                                                              */
/* ======================================================================== */

/**
 * @brief Reads an INF file as UTF-8 (UTF-16 and BOMs handled).
 *
 * @param path  INF path
 * @return newly allocated text or NULL
 */
static char *load_inf_text(const char *path)
{
  size_t   len;
  uint8_t *raw = read_file(path, &len);
  char    *txt;

  if (!raw)
    return NULL;
  if (len >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
  {
    txt = wide_to_utf8((const wchar_t *)(raw + 2)); /* read_file NUL-terminates twice */
    free(raw);
    return txt;
  }
  if (len >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF)
    memmove(raw, raw + 3, len - 2);
  return (char *)raw;
}

/**
 * @brief Expands a %string% token of an INF and removes quotes.
 *
 * @param strings  [Strings] map (lower-case keys)
 * @param in       raw value
 * @param out      output buffer
 * @param cap      size of the output buffer
 */
static void inf_expand(const strmap_t *strings, const char *in, char *out, size_t cap)
{
  char tmp[256];
  str_copy(tmp, sizeof(tmp), in);
  {
    char *s = str_trim(tmp);
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '%' && s[n - 1] == '%')
    {
      char key[128];
      const char *v;
      str_copy(key, sizeof(key), s + 1);
      key[n - 2] = 0;
      str_lower(key);
      v = sm_get(strings, key);
      str_copy(out, cap, v ? v : s);
      return;
    }
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"')
    {
      s[n - 1] = 0;
      s++;
    }
    str_copy(out, cap, s);
  }
}

/**
 * @brief Reads Provider, Class, DriverVer and the firmware files of an INF.
 *
 * @param pkg       package (updated)
 * @param inf_path  INF path
 */
static void parse_inf(drv_pkg_t *pkg, const char *inf_path)
{
  char     *txt = load_inf_text(inf_path);
  char     *line, *ctx = NULL;
  char      section[128] = "";
  strmap_t  strings;
  char      provider_raw[128] = "", class_raw[64] = "";

  if (!txt)
    return;
  memset(&strings, 0, sizeof(strings));

  /* pass 1: [Strings] */
  {
    char *copy = xstrdup(txt);
    for (line = strtok_s(copy, "\r\n", &ctx); line; line = strtok_s(NULL, "\r\n", &ctx))
    {
      char *l = str_trim(line);
      if (*l == ';' || !*l)
        continue;
      if (*l == '[')
      {
        char *e = strchr(l, ']');
        if (e)
          *e = 0;
        str_copy(section, sizeof(section), l + 1);
        str_lower(section);
        continue;
      }
      if (0 == strcmp(section, "strings"))
      {
        char *eq = strchr(l, '=');
        if (eq)
        {
          char *k, *v;
          size_t vl;
          *eq = 0;
          k = str_trim(l);
          v = str_trim(eq + 1);
          vl = strlen(v);
          if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"')
          {
            v[vl - 1] = 0;
            v++;
          }
          str_lower(k);
          if (!sm_get(&strings, k))
            sm_put(&strings, k, xstrdup(v));
        }
      }
    }
    free(copy);
  }

  /* pass 2: [Version], [SourceDisksFiles*] */
  section[0] = 0;
  ctx = NULL;
  for (line = strtok_s(txt, "\r\n", &ctx); line; line = strtok_s(NULL, "\r\n", &ctx))
  {
    char *l = str_trim(line);
    char *semi;
    if (*l == ';' || !*l)
      continue;
    if (*l == '[')
    {
      char *e = strchr(l, ']');
      if (e)
        *e = 0;
      str_copy(section, sizeof(section), l + 1);
      str_lower(section);
      continue;
    }
    semi = strchr(l, ';');
    if (semi)
      *semi = 0;
    if (0 == strcmp(section, "version"))
    {
      char *eq = strchr(l, '=');
      if (!eq)
        continue;
      *eq = 0;
      if (0 == _stricmp(str_trim(l), "Provider"))
        str_copy(provider_raw, sizeof(provider_raw), str_trim(eq + 1));
      else if (0 == _stricmp(str_trim(l), "Class"))
        str_copy(class_raw, sizeof(class_raw), str_trim(eq + 1));
      else if (0 == _stricmp(str_trim(l), "DriverVer"))
        str_copy(pkg->driver_ver, sizeof(pkg->driver_ver), str_trim(eq + 1));
    }
    else if (str_starts_with(section, "sourcedisksfiles"))
    {
      char *eq = strchr(l, '=');
      if (eq)
        *eq = 0;
      l = str_trim(l);
      if (is_firmware_name(l))
        sl_add_unique(&pkg->firmware, l);
    }
  }
  inf_expand(&strings, provider_raw, pkg->provider, sizeof(pkg->provider));
  inf_expand(&strings, class_raw, pkg->cls, sizeof(pkg->cls));

  for (size_t i = 0; i < strings.cap; i++)
    if (strings.keys[i])
      free(strings.vals[i]);
  sm_free(&strings);
  free(txt);
}

/* ======================================================================== */
/* scanning                                                                 */
/* ======================================================================== */

/** @brief context of a firmware scan walk */
typedef struct {
  drv_db_t   *db;  /**< database */
  const char *pkg;  /**< package name (NULL: derive from path) */
  bool        present;  /**< package of a present device */
} scan_ctx_t;

/**
 * @brief Checks and records whether a path is already indexed.
 *
 * @param db    database
 * @param path  file path
 * @return true if it was already indexed
 */
static bool fw_known(drv_db_t *db, const char *path)
{
  char key[A2D_MAX_PATH];
  str_copy(key, sizeof(key), path);
  str_lower(key);
  if (sm_get(&db->seen, key))
    return true;
  sm_put(&db->seen, key, (void *)1);
  return false;
}

/**
 * @brief walk_dir() callback indexing firmware-like files.
 *
 * @param full    full path
 * @param name    file name
 * @param is_dir  true for a directory
 * @param vctx    scan_ctx_t
 * @return always true
 */
static bool scan_cb(const char *full, const char *name, bool is_dir, void *vctx)
{
  scan_ctx_t *ctx = vctx;
  fw_file_t   f;

  if (is_dir)
    return true;
  if (!is_firmware_name(name) || fw_known(ctx->db, full))
    return true;
  memset(&f, 0, sizeof(f));
  f.path = xstrdup(full);
  f.name = xstrdup(name);
  if (ctx->pkg)
    f.pkg = xstrdup(ctx->pkg);
  else
  {
    /* derive package from path: ...\FileRepository\<pkg>\... */
    const char *fr = strstr(full, "FileRepository\\");
    char        pkg[256] = "";
    if (fr)
    {
      const char *s = fr + strlen("FileRepository\\");
      const char *e = strchr(s, '\\');
      size_t      n = e ? (size_t)(e - s) : strlen(s);
      if (n >= sizeof(pkg))
        n = sizeof(pkg) - 1;
      memcpy(pkg, s, n);
      pkg[n] = 0;
    }
    f.pkg = xstrdup(pkg);
  }
  f.size = file_size(full);
  f.present_pkg = ctx->present;
  detect_format(full, name, f.format, sizeof(f.format));
  VEC_PUSH(ctx->db->f, ctx->db->nf, ctx->db->cap_f, f);
  return true;
}

/**
 * @brief Indexes the firmware files of the complete DriverStore (once).
 *
 * @param db  database
 */
static void full_index(drv_db_t *db)
{
  char       repo[A2D_MAX_PATH];
  wchar_t    win[MAX_PATH];
  char       windir[MAX_PATH];
  scan_ctx_t ctx = { db, NULL, false };
  int        before = db->nf;

  if (db->full_indexed)
    return;
  db->full_indexed = true;
  GetWindowsDirectoryW(win, MAX_PATH);
  wide_to_utf8_buf(win, windir, sizeof(windir));
  path_join(repo, sizeof(repo), windir, "System32\\DriverStore\\FileRepository");
  log_info("indexing the complete DriverStore (%s) ...", repo);
  walk_dir(repo, true, scan_cb, &ctx);
  log_info("DriverStore index: %d additional firmware-like files", db->nf - before);
}

/**
 * @brief Writes firmware_scan.json (packages and firmware files) to the work folder.
 *
 * @param cfg  configuration
 * @param db   database
 */
static void write_json(const a2d_config_t *cfg, const drv_db_t *db)
{
  char     path[A2D_MAX_PATH];
  FILE    *fp;
  wchar_t *wp;

  path_join(path, sizeof(path), cfg->work_dir, "firmware_scan.json");
  wp = utf8_to_wide(path);
  fp = _wfopen(wp, L"wb");
  free(wp);
  if (!fp)
    return;
  fputs("{\n  \"driverPackages\": [\n", fp);
  for (int i = 0; i < db->np; i++)
  {
    const drv_pkg_t *p = &db->p[i];
    fputs(i ? ",\n    { " : "    { ", fp);
    fputs("\"inf\": ", fp); json_escape_fp(fp, p->inf);
    fputs(", \"dir\": ", fp); json_escape_fp(fp, p->dir);
    fputs(", \"provider\": ", fp); json_escape_fp(fp, p->provider);
    fputs(", \"class\": ", fp); json_escape_fp(fp, p->cls);
    fputs(", \"driverVer\": ", fp); json_escape_fp(fp, p->driver_ver);
    fputs(", \"devices\": [", fp);
    for (int k = 0; k < p->devices.count; k++) { if (k) fputs(", ", fp); json_escape_fp(fp, p->devices.v[k]); }
    fputs("], \"infFirmware\": [", fp);
    for (int k = 0; k < p->firmware.count; k++) { if (k) fputs(", ", fp); json_escape_fp(fp, p->firmware.v[k]); }
    fputs("] }", fp);
  }
  fputs("\n  ],\n  \"firmwareArtifacts\": [\n", fp);
  for (int i = 0; i < db->nf; i++)
  {
    const fw_file_t *f = &db->f[i];
    fputs(i ? ",\n    { " : "    { ", fp);
    fputs("\"fileName\": ", fp); json_escape_fp(fp, f->name);
    fputs(", \"path\": ", fp); json_escape_fp(fp, f->path);
    fprintf(fp, ", \"sizeBytes\": %lld", (long long)f->size);
    fputs(", \"format\": ", fp); json_escape_fp(fp, f->format);
    fputs(", \"package\": ", fp); json_escape_fp(fp, f->pkg);
    fprintf(fp, ", \"presentDevice\": %s }", f->present_pkg ? "true" : "false");
  }
  fputs("\n  ]\n}\n", fp);
  fclose(fp);
}

int drivers_scan(const a2d_config_t *cfg, const dm_set_t *dm, drv_db_t *db)
{
  memset(db, 0, sizeof(*db));

  for (int i = 0; dm && i < dm->count; i++)
  {
    const dm_dev_t *d = &dm->d[i];
    drv_pkg_t      *pkg = NULL;

    if (!d->driver_store_dir[0])
      continue;
    for (int k = 0; k < db->np; k++)
      if (0 == _stricmp(db->p[k].dir, d->driver_store_dir))
        pkg = &db->p[k];
    if (!pkg)
    {
      drv_pkg_t np;
      char      inf_path[A2D_MAX_PATH];
      memset(&np, 0, sizeof(np));
      str_copy(np.inf, sizeof(np.inf), d->inf);
      str_copy(np.dir, sizeof(np.dir), d->driver_store_dir);
      {
        /* the package folder is named <original.inf>_<arch>_<hash> */
        const char *b = path_basename(np.dir);
        const char *u = strstr(b, ".inf_");
        size_t      n = u ? (size_t)(u - b) + 4 : strlen(b);
        if (n >= sizeof(np.original_inf))
          n = sizeof(np.original_inf) - 1;
        memcpy(np.original_inf, b, n);
        np.original_inf[n] = 0;
      }
      path_join(inf_path, sizeof(inf_path), np.dir, np.original_inf);
      if (file_exists(inf_path))
        parse_inf(&np, inf_path);
      VEC_PUSH(db->p, db->np, db->cap_p, np);
      pkg = &db->p[db->np - 1];
      {
        scan_ctx_t ctx = { db, path_basename(pkg->dir), true };
        walk_dir(pkg->dir, true, scan_cb, &ctx);
      }
    }
    sl_add_unique(&pkg->devices, d->name[0] ? d->name : d->instance_id);
  }
  log_info("%d driver packages of present devices, %d firmware-like files", db->np, db->nf);

  if (cfg->full_driverstore)
    full_index(db);
  write_json(cfg, db);
  return db->nf;
}

void drivers_free(drv_db_t *db)
{
  for (int i = 0; i < db->np; i++)
  {
    sl_free(&db->p[i].devices);
    sl_free(&db->p[i].firmware);
  }
  for (int i = 0; i < db->nf; i++)
  {
    free(db->f[i].path);
    free(db->f[i].name);
    free(db->f[i].pkg);
  }
  free(db->p);
  free(db->f);
  sm_free(&db->seen);
  memset(db, 0, sizeof(*db));
}

const drv_pkg_t *drivers_pkg_by_dir(const drv_db_t *db, const char *dir)
{
  for (int i = 0; db && i < db->np; i++)
    if (0 == _stricmp(db->p[i].dir, dir))
      return &db->p[i];
  return NULL;
}

/* ======================================================================== */
/* firmware resolution                                                      */
/* ======================================================================== */

/**
 * @brief Finds a firmware file by exact name, preferring packages of present devices.
 *
 * @param db    database
 * @param name  file name
 * @return file or NULL
 */
static const fw_file_t *find_exact(const drv_db_t *db, const char *name)
{
  const fw_file_t *best = NULL;
  for (int i = 0; i < db->nf; i++)
    if (0 == _stricmp(db->f[i].name, name))
    {
      if (!best || (db->f[i].present_pkg && !best->present_pkg))
        best = &db->f[i];
    }
  return best;
}

int fw_find_all(drv_db_t *db, const char *prefix, const char *suffix, const fw_file_t **out, int max)
{
  int n = 0;
  for (int i = 0; i < db->nf && n < max; i++)
    if (str_starts_with_ci(db->f[i].name, prefix) && str_ends_with_ci(db->f[i].name, suffix))
      out[n++] = &db->f[i];
  return n;
}

/**
 * @brief Returns the name token that marks SoC-specific firmware variants.
 *
 * @param soc  SoC name
 * @return token, e.g. "purwa" or "8380"
 */
static const char *soc_alias(const char *soc)
{
  if (!soc)
    return "";
  if (str_starts_with(soc, "x1p42100") || str_starts_with(soc, "x1p46100"))
    return "purwa";
  if (str_starts_with(soc, "x1"))
    return "8380";
  if (str_starts_with(soc, "sc8280xp"))
    return "8280";
  return soc;
}

const fw_file_t *fw_resolve(drv_db_t *db, const char *wanted, const char *soc, char *why, size_t whycap)
{
  const fw_file_t *f;
  char             stem[64];
  const char      *dot;
  size_t           n = 0;

  why[0] = 0;
  if (!db->full_indexed)
    full_index(db);
  f = find_exact(db, wanted);

  /* fuzzy: same alphabetic stem and extension, e.g. qcdxkmsuc8380.mbn -> qcdxkmsucpurwa.mbn */
  dot = strrchr(wanted, '.');
  while (wanted[n] && wanted + n != dot && !(wanted[n] >= '0' && wanted[n] <= '9') && n < sizeof(stem) - 1)
  {
    stem[n] = wanted[n];
    n++;
  }
  stem[n] = 0;
  if (n >= 4 && dot)
  {
    const fw_file_t *cand[64];
    int              nc = fw_find_all(db, stem, dot, cand, 64);
    const fw_file_t *best = NULL;
    int              best_score = -1;
    const char      *alias = soc_alias(soc);

    for (int i = 0; i < nc; i++)
    {
      int score = 0;
      if (cand[i]->present_pkg)
        score += 10;
      if (alias[0] && str_contains_ci(cand[i]->name, alias))
        score += 5;
      score -= (int)strlen(cand[i]->name) / 8;
      if (score > best_score)
      {
        best = cand[i];
        best_score = score;
      }
    }
    if (f)
    {
      /* exact name exists - prefer a SoC-specific variant if there is one (qcdxkmsucpurwa.mbn on X1P42100) */
      if (best && best != f && alias[0] && str_contains_ci(best->name, alias) && !str_contains_ci(f->name, alias))
      {
        snprintf(why, whycap, "SoC-specific variant %s preferred over %s (%s package %s)", best->name, f->name,
                 best->present_pkg ? "driver" : "DriverStore", best->pkg);
        return best;
      }
    }
    else if (best)
    {
      snprintf(why, whycap, "%s not found; using SoC/board variant %s (package %s, %d candidate(s) for %s*%s)", wanted,
               best->name, best->pkg, nc, stem, dot);
      return best;
    }
  }
  if (f)
  {
    snprintf(why, whycap, "found in %s %s", f->present_pkg ? "driver package" : "DriverStore package (extension/other INF)",
             f->pkg);
    return f;
  }
  snprintf(why, whycap, "%s not found in the DriverStore", wanted);
  return NULL;
}

/* ======================================================================== */
/* copy plan                                                                */
/* ======================================================================== */

void fw_plan_add(fw_plan_t *plan, const char *target_rel, const char *source, const char *reason)
{
  fw_copy_t c;
  for (int i = 0; i < plan->count; i++)
    if (0 == _stricmp(plan->c[i].target_rel, target_rel))
      return;
  memset(&c, 0, sizeof(c));
  str_copy(c.target_rel, sizeof(c.target_rel), target_rel);
  str_copy(c.source, sizeof(c.source), source);
  str_copy(c.reason, sizeof(c.reason), reason);
  VEC_PUSH(plan->c, plan->count, plan->cap, c);
}

int fw_plan_execute(const a2d_config_t *cfg, fw_plan_t *plan)
{
  strbuf_t man;
  int      ok = 0;
  char     mpath[A2D_MAX_PATH];

  mkdir_p(cfg->output_fw_dir);
  sb_init(&man);
  sb_printf(&man, "acpi2dt %s firmware manifest\n", A2D_VERSION_STRING);
  sb_puts(&man, "=====================================\n\n");
  sb_puts(&man, "Copy the contents of this folder to /lib/firmware/ of the target Linux system.\n");
  sb_puts(&man, "NOTE: files taken from the Windows DriverStore are usually NOT redistributable;\n");
  sb_puts(&man, "      use them only on the machine they were extracted from.\n\n");

  for (int i = 0; i < plan->count; i++)
  {
    fw_copy_t *c = &plan->c[i];
    char       dst[A2D_MAX_PATH], rel[256], dir[A2D_MAX_PATH];

    str_copy(rel, sizeof(rel), c->target_rel);
    path_normalize_slashes(rel);
    path_join(dst, sizeof(dst), cfg->output_fw_dir, rel);
    path_dirname(dst, dir, sizeof(dir));
    if (!cfg->no_fw_copy)
    {
      mkdir_p(dir);
      c->copied = copy_file(c->source, dst);
      if (c->copied)
        ok++;
      else
        log_warn("cannot copy %s -> %s", c->source, dst);
    }
    c->size = file_size(c->source);
    sha256_file(c->source, c->sha256);
    sb_printf(&man, "%s\n  source : %s\n  size   : %lld bytes\n  sha256 : %s\n  reason : %s\n  status : %s\n\n",
              c->target_rel, c->source, (long long)c->size, c->sha256, c->reason,
              cfg->no_fw_copy ? "not copied (--no-fw-copy)" : (c->copied ? "copied" : "COPY FAILED"));
  }
  path_join(mpath, sizeof(mpath), cfg->output_fw_dir, "FIRMWARE-MANIFEST.txt");
  if (!write_file(mpath, man.buf ? man.buf : "", man.len))
    log_warn("cannot write %s", mpath);
  sb_free(&man);
  return ok;
}

void fw_plan_free(fw_plan_t *plan)
{
  free(plan->c);
  memset(plan, 0, sizeof(*plan));
}
