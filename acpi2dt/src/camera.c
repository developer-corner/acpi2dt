/**
 * @file   camera.c
 * @author Ingo A. Kubbilun
 * @brief  camera sensors from the Qualcomm Windows camera driver configuration
 *
 * The Qualcomm camera sensor drivers (qccamfrontsensor8380, qccamauxsensor8380,
 * ...) are generic; the board-specific data lives in OEM extension packages
 * and is referenced per device in the registry:
 *
 *   ...\Enum\ACPI\<id>\<inst>\Device Parameters\Resources
 *       BinaryPath      CAMF_RES_*.bin   power sequence (regulators, MCLK, GPIOs)
 *       ScfgBinaryPath  SCFG_*.bin       sensor module, I2C address, chip ID
 *
 * Both files use the Qualcomm "AeoB" container: "AeoB", u32 size, u32 version,
 * then TLV records (u16 type, u16 length): type 0 = integer, 1 = string,
 * 3 = container (children follow).
 *
 * [MIT license] - see main.c
 */

#include "camera.h"

typedef struct {
  bool     is_str;
  uint64_t v;
  char     s[96];
} tok_t;

typedef struct {
  tok_t *t;
  int    count, cap;
} toklist_t;

static bool aeob_tokens(const char *path, toklist_t *tl)
{
  size_t   len;
  uint8_t *d = read_file(path, &len);
  size_t   off = 12;

  memset(tl, 0, sizeof(*tl));
  if (!d || len < 16 || memcmp(d, "AeoB", 4))
  {
    free(d);
    return false;
  }
  while (off + 4 <= len)
  {
    uint16_t type = (uint16_t)(d[off] | (d[off + 1] << 8));
    uint16_t l = (uint16_t)(d[off + 2] | (d[off + 3] << 8));
    tok_t    t;

    off += 4;
    if (type == 3)
      continue; /* container: its children follow */
    if (off + l > len)
      break;
    memset(&t, 0, sizeof(t));
    if (type == 0)
    {
      for (int i = 0; i < l && i < 8; i++)
        t.v |= (uint64_t)d[off + i] << (8 * i);
      VEC_PUSH(tl->t, tl->count, tl->cap, t);
    }
    else if (type == 1)
    {
      size_t n = 0;
      while (n < l && n < sizeof(t.s) - 1 && d[off + n])
      {
        t.s[n] = (char)d[off + n];
        n++;
      }
      t.is_str = true;
      VEC_PUSH(tl->t, tl->count, tl->cap, t);
    }
    off += l;
  }
  free(d);
  return true;
}

static const char *g_sensor_prefixes[][2] = {
  { "ov", "ovti" }, { "imx", "sony" }, { "hm", "himax" }, { "gc", "galaxycore" },
  { "s5k", "samsung" }, { "hi", "hynix" }, { "og", "ovti" }, { "sc", "smartsens" },
};

const char *camera_vendor_prefix(const char *sensor)
{
  for (size_t i = 0; i < sizeof(g_sensor_prefixes) / sizeof(g_sensor_prefixes[0]); i++)
    if (str_starts_with_ci(sensor, g_sensor_prefixes[i][0]))
      return g_sensor_prefixes[i][1];
  return NULL;
}

/* "com.qti.sensormodule.Shinetech_ov08x_1.bin" -> "ov08x" */
static void sensor_from_module(const char *module, char *out, size_t cap)
{
  char        tmp[96], *tok, *ctx = NULL;
  const char *p = module;

  out[0] = 0;
  if (str_starts_with(p, "com.qti.sensormodule."))
    p += strlen("com.qti.sensormodule.");
  str_copy(tmp, sizeof(tmp), p);
  if (str_ends_with_ci(tmp, ".bin"))
    tmp[strlen(tmp) - 4] = 0;
  for (tok = strtok_s(tmp, "_", &ctx); tok; tok = strtok_s(NULL, "_", &ctx))
  {
    if (camera_vendor_prefix(tok) && strpbrk(tok, "0123456789"))
    {
      str_copy(out, cap, tok);
      break;
    }
  }
  if (!out[0])
  {
    str_copy(tmp, sizeof(tmp), p);
    if (str_ends_with_ci(tmp, ".bin"))
      tmp[strlen(tmp) - 4] = 0;
    str_copy(out, cap, tmp);
  }
  str_lower(out);
}

static void parse_scfg(cam_sensor_t *s)
{
  toklist_t tl;
  int       nint = 0, nstr = 0;
  uint64_t  ints[4] = { 0 };

  if (!s->scfg_file[0] || !aeob_tokens(s->scfg_file, &tl))
    return;
  for (int i = 0; i < tl.count; i++)
  {
    if (tl.t[i].is_str)
    {
      if (nstr == 0)
        str_copy(s->module, sizeof(s->module), tl.t[i].s);
      else if (nstr == 1)
        str_copy(s->tuning, sizeof(s->tuning), tl.t[i].s);
      nstr++;
    }
    else if (nint < 4)
      ints[nint++] = tl.t[i].v;
  }
  if (nstr >= 1 && nint >= 2)
  {
    s->has_scfg = true;
    s->slave8 = (uint16_t)(ints[0] & 0xFFFF);
    s->cci_info = (uint16_t)(ints[0] >> 16);
    s->chip_id = (uint16_t)(ints[1] >> 16);
    s->chip_id_reg = (uint16_t)(ints[1] & 0xFFFF);
    sensor_from_module(s->module, s->sensor, sizeof(s->sensor));
  }
  free(tl.t);
}

static bool is_keyword(const tok_t *t)
{
  static const char *kw[] = { "DEVICE", "COMPONENT", "FSTATE", "DSTATE", "NPARESOURCE", "CLOCK", "FOOTSWITCH",
                              "TLMMGPIO", "PMICVREGVOTE", "DELAY", "PSTATE", "BUSARB", "PSTATE_SET",
                              "PSTATE_ADJUST" };
  if (!t->is_str)
    return false;
  for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++)
    if (!strcmp(t->s, kw[i]))
      return true;
  return false;
}

static void parse_res(cam_sensor_t *s)
{
  toklist_t tl;
  int       d0 = -1, d0_end;
  int       first_val[512];

  s->reset_pin = -1;
  if (!s->res_file[0] || !aeob_tokens(s->res_file, &tl))
    return;
  for (int i = 0; i < 512; i++)
    first_val[i] = -1;

  /* device path */
  for (int i = 0; i + 2 < tl.count; i++)
    if (tl.t[i].is_str && !strcmp(tl.t[i].s, "DEVICE") && tl.t[i + 2].is_str)
    {
      str_copy(s->acpi_path, sizeof(s->acpi_path), tl.t[i + 2].s);
      break;
    }
  /* D0 block: "DSTATE" 0 ... next "DSTATE" */
  for (int i = 0; i + 1 < tl.count; i++)
    if (tl.t[i].is_str && !strcmp(tl.t[i].s, "DSTATE") && !tl.t[i + 1].is_str && tl.t[i + 1].v == 0)
    {
      d0 = i + 2;
      break;
    }
  if (d0 < 0)
  {
    free(tl.t);
    return;
  }
  d0_end = tl.count;
  for (int i = d0; i < tl.count; i++)
    if (tl.t[i].is_str && !strcmp(tl.t[i].s, "DSTATE"))
    {
      d0_end = i;
      break;
    }
  s->has_res = true;

  for (int i = d0; i < d0_end; i++)
  {
    const tok_t *k = &tl.t[i];
    int          j = i + 1;
    uint64_t     iv[8];
    int          ni = 0;
    char         name[96] = "";

    if (!is_keyword(k))
      continue;
    if (j < d0_end && tl.t[j].is_str && !is_keyword(&tl.t[j]))
      str_copy(name, sizeof(name), tl.t[j++].s);
    while (j < d0_end && !tl.t[j].is_str && ni < 8)
      iv[ni++] = tl.t[j++].v;

    if (!strcmp(k->s, "PMICVREGVOTE") && s->nvreg < 4 && ni >= 2)
    {
      cam_vreg_t *v = &s->vreg[s->nvreg++];
      const char *ldo = strstr(name, "LDO");
      str_copy(v->name, sizeof(v->name), name);
      v->uv = (uint32_t)iv[1];
      if (ldo)
      {
        v->ldo = atoi(ldo + 3);
        {
          const char *us = strrchr(name, '_');
          v->pmic = us ? (char)(us[1] | 0x20) : '?';
        }
      }
      {
        char line[160];
        snprintf(line, sizeof(line), "regulator %s %.3f V", ldo ? ldo : name, v->uv / 1000000.0);
        sl_add(&s->steps, line);
      }
    }
    else if (!strcmp(k->s, "CLOCK"))
    {
      if (strstr(name, "mclk") && ni >= 2 && iv[1])
      {
        char line[160];
        str_copy(s->mclk, sizeof(s->mclk), name);
        s->mclk_hz = (uint32_t)iv[1];
        snprintf(line, sizeof(line), "clock %s %.1f MHz", name, s->mclk_hz / 1e6);
        sl_add(&s->steps, line);
      }
    }
    else if (!strcmp(k->s, "TLMMGPIO") && ni >= 2)
    {
      int  pin = (int)iv[0];
      char line[160];
      if (s->ngpio < 8)
      {
        cam_gpio_t *g = &s->gpio[s->ngpio++];
        g->pin = pin;
        for (int q = 0; q < 5; q++)
          g->v[q] = (q + 1 < ni) ? (int)iv[q + 1] : 0;
      }
      if (pin >= 0 && pin < 512)
      {
        if (first_val[pin] < 0)
          first_val[pin] = (int)iv[1];
        else if (first_val[pin] != (int)iv[1] && s->reset_pin < 0)
        {
          s->reset_pin = pin;
          s->reset_active_low = first_val[pin] == 0;
        }
      }
      snprintf(line, sizeof(line), "TLMM %d config %d,%d,%d,%d,%d", pin, ni > 1 ? (int)iv[1] : 0,
               ni > 2 ? (int)iv[2] : 0, ni > 3 ? (int)iv[3] : 0, ni > 4 ? (int)iv[4] : 0, ni > 5 ? (int)iv[5] : 0);
      sl_add(&s->steps, line);
    }
    else if (!strcmp(k->s, "DELAY") && ni >= 1)
    {
      char line[64];
      s->delay_after = (int)iv[0];
      snprintf(line, sizeof(line), "delay %d", (int)iv[0]);
      sl_add(&s->steps, line);
    }
    else if (!strcmp(k->s, "FOOTSWITCH") && name[0])
    {
      char line[160];
      snprintf(line, sizeof(line), "power domain %s", name);
      sl_add(&s->steps, line);
    }
  }
  free(tl.t);
}

int camera_collect(const dm_set_t *dm, cam_set_t *set)
{
  memset(set, 0, sizeof(*set));
  if (!dm)
    return 0;
  for (int i = 0; i < dm->count; i++)
  {
    const dm_dev_t *d = &dm->d[i];
    cam_sensor_t    s;

    if (!d->scfg_bin[0] && !(d->res_bin[0] && _stricmp(d->cls, "Camera") == 0))
      continue;
    if (!d->scfg_bin[0])
      continue; /* platform, ISP, ... devices have resources but no sensor */
    memset(&s, 0, sizeof(s));
    s.reset_pin = -1;
    str_copy(s.dev_name, sizeof(s.dev_name), d->name);
    str_copy(s.acpi_path, sizeof(s.acpi_path), d->bios_name);
    str_copy(s.res_file, sizeof(s.res_file), d->res_bin);
    str_copy(s.scfg_file, sizeof(s.scfg_file), d->scfg_bin);
    parse_scfg(&s);
    parse_res(&s);
    if (s.has_scfg || s.has_res)
      VEC_PUSH(set->s, set->count, set->cap, s);
  }
  return set->count;
}

void camera_free(cam_set_t *set)
{
  for (int i = 0; i < set->count; i++)
    sl_free(&set->s[i].steps);
  free(set->s);
  memset(set, 0, sizeof(*set));
}
