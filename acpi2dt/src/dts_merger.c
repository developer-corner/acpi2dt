/**
 * @file   dts_merger.c
 * @author Ingo A. Kubbilun
 * @brief  drafts the board device tree from all information sources
 *
 * Strategy
 * --------
 * 1. The SoC include (.dtsi) of this machine is determined from the kernel
 *    DTS folder (e.g. x1p42100 -> purwa.dtsi). SoC-specific top-level
 *    /delete-node/ statements are learned from the kernel's other boards of
 *    the same SoC (majority vote).
 * 2. A "base tree" is built from the SoC/PMIC include files. Every ACPI device
 *    with an MMIO resource is matched against the DT nodes by unit address
 *    (ACPI \_SB.I2C1 @ 0xb80000  <->  i2c0: i2c@b80000).
 * 3. The primary similar DTS is the template. It is re-emitted node by node
 *    (comments, labels, raw values preserved) and annotated:
 *      - controllers: matched ACPI device + Device Manager state
 *      - bus children (reg): ACPI consumer at the same bus address; GPIO
 *        interrupt / GPIO pins are cross-checked (Qualcomm "virtual GPIO"
 *        numbers are decoded via the GIO0 interrupt list, qcom,pdc-ranges and
 *        the TLMM PDC wake map)
 *      - pinctrl states and gpio properties: ACPI users of the TLMM pin
 *      - firmware-name: resolved in the DriverStore, copied, path rewritten
 *      - model / compatible: from SMBIOS
 *    Board-family .dtsi includes of the template (e.g. x1-asus-zenbook-a14.dtsi)
 *    are inlined, the SoC include is replaced if the template is for another
 *    SoC.
 * 4. Secondary similar DTS files contribute nodes the template lacks: as real
 *    nodes if ACPI confirms them (and all labels resolve), otherwise as
 *    commented-out suggestions.
 * 5. ACPI devices on I2C/SPI/UART buses that no DTS covers are added
 *    (HID-over-I2C as real nodes, everything else as commented templates).
 * 6. An appendix lists the ACPI namespace, PCI devices, problem devices,
 *    firmware and SoC nodes that ACPI uses but the draft leaves disabled.
 *
 * [MIT license] - see main.c
 */

#include "dts_merger.h"
#include "dts_parser.h"
#include <time.h>

/* ======================================================================== */
/* static knowledge                                                         */
/* ======================================================================== */

typedef struct {
  uint16_t gpio;
  uint16_t pdc;
} wake_map_t;

/* drivers/pinctrl/qcom/pinctrl-x1e80100.c: x1e80100_pdc_map[] (also used by
   purwa / X1P42100, which reuses "qcom,x1e80100-tlmm") */
static const wake_map_t g_x1e80100_wake[] = {
  {0, 72}, {2, 70}, {3, 71}, {6, 123}, {7, 67}, {11, 85}, {13, 86}, {15, 68}, {18, 122}, {19, 69}, {21, 158},
  {23, 143}, {24, 126}, {26, 129}, {27, 144}, {28, 77}, {29, 78}, {30, 92}, {31, 159}, {32, 145}, {33, 115},
  {34, 130}, {35, 146}, {36, 147}, {38, 113}, {39, 80}, {43, 148}, {47, 149}, {51, 79}, {53, 89}, {55, 81},
  {59, 87}, {64, 90}, {65, 106}, {66, 142}, {67, 88}, {68, 151}, {71, 91}, {75, 152}, {79, 153}, {80, 125},
  {81, 128}, {83, 154}, {84, 137}, {85, 155}, {87, 156}, {91, 157}, {92, 138}, {93, 139}, {94, 140}, {95, 141},
  {113, 84}, {121, 73}, {123, 74}, {125, 75}, {129, 76}, {131, 82}, {134, 83}, {141, 93}, {144, 94}, {145, 95},
  {147, 96}, {148, 97}, {150, 102}, {151, 103}, {153, 104}, {154, 100}, {156, 105}, {157, 107}, {163, 98},
  {166, 112}, {172, 99}, {175, 114}, {181, 101}, {184, 116}, {193, 117}, {196, 108}, {203, 133}, {208, 134},
  {212, 120}, {213, 150}, {214, 121}, {215, 118}, {217, 109}, {219, 119}, {220, 110}, {221, 111}, {222, 124},
  {224, 131}, {225, 132}, {228, 135}, {230, 136}, {232, 162},
};

typedef struct {
  const char *soc;
  const char *codename;
  const char *legacy_dtsi;   /* SoC dtsi name in older kernels */
} soc_family_t;

static const soc_family_t g_soc_families[] = {
  { "x1e80100", "hamoa", "x1e80100.dtsi" },
  { "x1e78100", "hamoa", "x1e80100.dtsi" },
  { "x1e001de", "hamoa", "x1e80100.dtsi" },
  { "x1e84100", "hamoa", "x1e80100.dtsi" },
  { "x1p64100", "hamoa", "x1e80100.dtsi" },
  { "x1p66100", "hamoa", "x1e80100.dtsi" },
  { "x1p42100", "purwa", "x1p42100.dtsi" },
  { "x1p46100", "purwa", "x1p42100.dtsi" },
  { "sc8280xp", "sc8280xp", "sc8280xp.dtsi" },
};

typedef struct {
  const char *id;
  const char *compatible;
} hid_compat_t;

static const hid_compat_t g_hid_compat[] = {
  { "PNP0C50",  "hid-over-i2c" },
  { "ACPI0C50", "hid-over-i2c" },
};

/* firmware wanted by well-known nodes (used without template) */
typedef struct {
  const char *label;
  const char *files[2];
} fw_hint_t;

static const fw_hint_t g_fw_hints[] = {
  { "remoteproc_adsp", { "qcadsp8380.mbn", "adsp_dtbs.elf" } },
  { "remoteproc_cdsp", { "qccdsp8380.mbn", "cdsp_dtbs.elf" } },
  { "gpu_zap_shader",  { "qcdxkmsuc8380.mbn", NULL } },
};

/* ======================================================================== */
/* generator state                                                          */
/* ======================================================================== */

typedef struct {
  dt_top_t   *t;
  const char *file;
  int         marker;      /* 0 item, 1 begin-inline, 2 end-inline */
  const char *inc_name;
  bool        replaced_soc;
} flat_t;

typedef struct {
  flat_t *v;
  int     count, cap;
} flatlist_t;

typedef struct {
  uint64_t   addr;
  dt_node_t *node;
} unit_t;

typedef struct {
  a2d_ctx_t   *x;
  dts_stats_t *st;
  const char  *soc;
  char         dirbuf[3][A2D_MAX_PATH];
  const char  *dirs[3];
  int          ndirs;
  char         qcom_dir[A2D_MAX_PATH];
  char         soc_dtsi[64];
  char         pmics_dtsi[64];
  strlist_t    soc_deletes;
  int          soc_boards;

  dt_file_t   *tmpl;
  flatlist_t   tflat;
  char         tmpl_soc[32];
  dt_file_t   *sec[16];
  flatlist_t   sflat[16];
  int          nsec;

  dt_tree_t    base;
  dt_tree_t    full;
  unit_t      *units;
  int          nunits, capunits;

  /* GPIO decoding */
  acpi_dev_t  *gio;
  int          ngpios;
  uint32_t     pdc[32][3];
  int          npdc;
  const wake_map_t *wm;
  int          nwm;
  strlist_t    tlmm_users[512];

  /* ACPI <-> DT */
  dt_node_t  **acpi_node;    /* per ACPI device */
  bool        *covered;      /* consumer emitted somewhere */
  uint8_t     *src_kind;     /* per ACPI device: bit0 I2C src, bit1 SPI src, bit2 UART src */

  strlist_t    top_refs;     /* top-level refs present in the draft */
  strlist_t    notes;        /* summary notes */
  strbuf_t     out;
} gen_t;

typedef struct {
  dt_node_t       *target;      /* node in base tree (may be NULL) */
  acpi_dev_t      *acpi;        /* ACPI device matched by MMIO     */
  acpi_dev_t      *consumer;    /* ACPI consumer (bus child)       */
  acpi_dev_t      *bus_ctrl;    /* ACPI controller of this bus node */
  acpi_res_kind_t  bus_kind;
  bool             bus_child;   /* parent is a bus with ACPI ctrl  */
  acpi_dev_t      *parent_ctrl;
  acpi_res_kind_t  parent_kind;
  bool             in_tlmm;
  bool             is_root;
  bool             missing_ref;
  const dt_node_t *sec[16];     /* counterparts in secondary files */
  const char      *origin;      /* e.g. "[SIMILAR2] taken over from x.dts" */
} ectx_t;

static void emit_node(gen_t *g, strbuf_t *sb, const dt_node_t *n, int depth, ectx_t *ec);

/* ======================================================================== */
/* small helpers                                                            */
/* ======================================================================== */

static const soc_family_t *soc_family(const char *soc)
{
  for (size_t i = 0; i < sizeof(g_soc_families) / sizeof(g_soc_families[0]); i++)
    if (soc && 0 == _stricmp(g_soc_families[i].soc, soc))
      return &g_soc_families[i];
  return NULL;
}

static bool is_soc_dtsi_name(const char *name)
{
  char base[64];
  str_copy(base, sizeof(base), path_basename(name));
  if (!str_ends_with_ci(base, ".dtsi"))
    return false;
  base[strlen(base) - 5] = 0;
  for (size_t i = 0; i < sizeof(g_soc_families) / sizeof(g_soc_families[0]); i++)
  {
    char legacy[64];
    str_copy(legacy, sizeof(legacy), g_soc_families[i].legacy_dtsi);
    legacy[strlen(legacy) - 5] = 0;
    if (!_stricmp(base, g_soc_families[i].soc) || !_stricmp(base, g_soc_families[i].codename) ||
        !_stricmp(base, legacy))
      return true;
  }
  return false;
}

/* board-family include of another board, e.g. "x1-asus-zenbook-a14.dtsi" */
static bool is_board_family_dtsi(const char *name)
{
  const char *b = path_basename(name);
  if (!str_ends_with_ci(b, ".dtsi") || str_contains_ci(b, "pmic") || is_soc_dtsi_name(b))
    return false;
  if (str_starts_with_ci(b, "x1-"))
    return true;
  for (size_t i = 0; i < sizeof(g_soc_families) / sizeof(g_soc_families[0]); i++)
  {
    char p1[48], p2[48];
    snprintf(p1, sizeof(p1), "%s-", g_soc_families[i].soc);
    snprintf(p2, sizeof(p2), "%s-", g_soc_families[i].codename);
    if (str_starts_with_ci(b, p1) || str_starts_with_ci(b, p2))
      return true;
  }
  return false;
}

static void emit_lines_comment(strbuf_t *sb, int depth, const strlist_t *lines)
{
  if (!lines->count)
    return;
  if (lines->count == 1 && strlen(lines->v[0]) < 90)
  {
    sb_indent(sb, depth);
    sb_puts(sb, "/* ");
    sb_put_comment_safe(sb, lines->v[0]);
    sb_puts(sb, " */\n");
    return;
  }
  sb_indent(sb, depth);
  sb_puts(sb, "/*\n");
  for (int i = 0; i < lines->count; i++)
  {
    sb_indent(sb, depth);
    sb_puts(sb, " * ");
    sb_put_comment_safe(sb, lines->v[i]);
    sb_putc(sb, '\n');
  }
  sb_indent(sb, depth);
  sb_puts(sb, " */\n");
}

static void addf(strlist_t *sl, const char *fmt, ...)
{
  char    buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  sl_add(sl, buf);
}

static void emit_raw_comment(strbuf_t *sb, int depth, const char *c)
{
  if (!c)
    return;
  sb_indent(sb, depth);
  sb_puts(sb, c);
  sb_putc(sb, '\n');
}

/* prefixes every line of src with indentation + "// " */
static void comment_out(strbuf_t *dst, const char *src, int depth)
{
  const char *p = src;
  while (p && *p)
  {
    const char *e = strchr(p, '\n');
    size_t      n = e ? (size_t)(e - p) : strlen(p);
    size_t      k = 0;
    while (k < n && p[k] == '\t')
      k++;
    sb_indent(dst, depth);
    sb_puts(dst, "// ");
    if (k > (size_t)depth)
      for (size_t i = (size_t)depth; i < k; i++)
        sb_putc(dst, '\t');
    sb_putn(dst, p + k, n - k);
    sb_putc(dst, '\n');
    p = e ? e + 1 : NULL;
  }
}

static const char *node_title(const dt_node_t *n)
{
  return n->ref ? n->ref : (n->name ? n->name : "?");
}

static void base_ref(const dt_node_t *n, char *out, size_t cap)
{
  if (n->labels.count)
    snprintf(out, cap, "&%s", n->labels.v[0]);
  else
  {
    char p[512];
    dt_node_path(n, p, sizeof(p));
    snprintf(out, cap, "&{%s}", p);
  }
}

static const dm_dev_t *dm_for(gen_t *g, const acpi_dev_t *a)
{
  return a ? devmgr_find_by_acpi_path(g->x->dm, a->path) : NULL;
}

static const char *irq_type_name(const acpi_res_t *r)
{
  if (r->edge)
    return r->active_both ? "IRQ_TYPE_EDGE_BOTH" : (r->active_low ? "IRQ_TYPE_EDGE_FALLING" : "IRQ_TYPE_EDGE_RISING");
  return r->active_low ? "IRQ_TYPE_LEVEL_LOW" : "IRQ_TYPE_LEVEL_HIGH";
}

/* ======================================================================== */
/* kernel folder: SoC include, learned delete-nodes                         */
/* ======================================================================== */

typedef struct {
  gen_t    *g;
  strmap_t  counts;
  strlist_t keys;
} harvest_t;

static void harvest_file(harvest_t *h, const char *path, strlist_t *seen, int depth)
{
  dt_file_t *f = dt_parse_file(path);
  if (!f)
    return;
  for (dt_top_t *t = f->items; t; t = t->next)
  {
    if (t->kind == TOP_DELETE_NODE && t->ref)
      sl_add_unique(seen, t->ref);
    else if (t->kind == TOP_INCLUDE && t->include && !t->sys_include && depth < 3 &&
             is_board_family_dtsi(t->include))
    {
      char p[A2D_MAX_PATH];
      if (dt_resolve_include(t->include, path, h->g->dirs, h->g->ndirs, p, sizeof(p)))
        harvest_file(h, p, seen, depth + 1);
    }
  }
}

static bool harvest_cb(const char *full, const char *name, bool is_dir, void *vctx)
{
  harvest_t *h = vctx;
  char       prefix[48];
  strlist_t  seen;

  if (is_dir)
    return false;
  snprintf(prefix, sizeof(prefix), "%s-", h->g->soc);
  if (!str_starts_with_ci(name, prefix) || !str_ends_with_ci(name, ".dts"))
    return true;
  memset(&seen, 0, sizeof(seen));
  harvest_file(h, full, &seen, 0);
  h->g->soc_boards++;
  for (int i = 0; i < seen.count; i++)
  {
    intptr_t c = (intptr_t)sm_get(&h->counts, seen.v[i]);
    if (!c)
      sl_add(&h->keys, seen.v[i]);
    sm_put(&h->counts, seen.v[i], (void *)(c + 1));
  }
  sl_free(&seen);
  return true;
}

static void setup_kernel(gen_t *g)
{
  const char         *k = g->x->cfg->kernel_dts_dir;
  const soc_family_t *fam = soc_family(g->soc);
  char                p[A2D_MAX_PATH];

  path_join(g->qcom_dir, sizeof(g->qcom_dir), k, "qcom");
  if (!dir_exists(g->qcom_dir))
    str_copy(g->qcom_dir, sizeof(g->qcom_dir), k);
  str_copy(g->dirbuf[0], A2D_MAX_PATH, g->qcom_dir);
  str_copy(g->dirbuf[1], A2D_MAX_PATH, k);
  path_dirname(k, g->dirbuf[2], A2D_MAX_PATH);
  for (int i = 0; i < 3; i++)
    g->dirs[i] = g->dirbuf[i];
  g->ndirs = 3;

  /* SoC include */
  {
    char cands[3][64];
    int  nc = 0;
    if (fam)
      snprintf(cands[nc++], 64, "%s.dtsi", fam->codename);
    snprintf(cands[nc++], 64, "%s.dtsi", g->soc);
    if (fam)
      str_copy(cands[nc++], 64, fam->legacy_dtsi);
    for (int i = 0; i < nc && !g->soc_dtsi[0]; i++)
    {
      path_join(p, sizeof(p), g->qcom_dir, cands[i]);
      if (file_exists(p))
        str_copy(g->soc_dtsi, sizeof(g->soc_dtsi), cands[i]);
    }
    if (!g->soc_dtsi[0])
    {
      log_warn("no SoC include for %s found in %s", g->soc, g->qcom_dir);
      snprintf(g->soc_dtsi, sizeof(g->soc_dtsi), "%s.dtsi", g->soc);
    }
    else
      log_info("SoC include for %s: %s", g->soc, g->soc_dtsi);
  }
  /* PMIC include */
  {
    char cands[2][64];
    int  nc = 0;
    if (fam)
      snprintf(cands[nc++], 64, "%s-pmics.dtsi", fam->codename);
    snprintf(cands[nc++], 64, "%s-pmics.dtsi", g->soc);
    if (fam && !strcmp(fam->codename, "hamoa"))
      str_copy(cands[nc - 1], 64, "x1e80100-pmics.dtsi");
    for (int i = 0; i < nc && !g->pmics_dtsi[0]; i++)
    {
      path_join(p, sizeof(p), g->qcom_dir, cands[i]);
      if (file_exists(p))
        str_copy(g->pmics_dtsi, sizeof(g->pmics_dtsi), cands[i]);
    }
    if (!g->pmics_dtsi[0] && fam && !strcmp(fam->codename, "purwa"))
    {
      path_join(p, sizeof(p), g->qcom_dir, "hamoa-pmics.dtsi");
      if (file_exists(p))
        str_copy(g->pmics_dtsi, sizeof(g->pmics_dtsi), "hamoa-pmics.dtsi");
      else
      {
        path_join(p, sizeof(p), g->qcom_dir, "x1e80100-pmics.dtsi");
        if (file_exists(p))
          str_copy(g->pmics_dtsi, sizeof(g->pmics_dtsi), "x1e80100-pmics.dtsi");
      }
    }
  }

  /* learn SoC-specific /delete-node/ statements from the kernel boards */
  {
    harvest_t h;
    memset(&h, 0, sizeof(h));
    h.g = g;
    walk_dir(g->qcom_dir, false, harvest_cb, &h);
    for (int i = 0; i < h.keys.count; i++)
    {
      intptr_t c = (intptr_t)sm_get(&h.counts, h.keys.v[i]);
      if (g->soc_boards > 0 && c * 2 > g->soc_boards)
        sl_add(&g->soc_deletes, h.keys.v[i]);
    }
    if (g->soc_deletes.count)
      log_info("learned %d SoC-specific /delete-node/ statement(s) from %d kernel %s boards", g->soc_deletes.count,
               g->soc_boards, g->soc);
    sm_free(&h.counts);
    sl_free(&h.keys);
  }
}

/* ======================================================================== */
/* template flattening                                                      */
/* ======================================================================== */

static void flatten(gen_t *g, dt_file_t *f, flatlist_t *fl, int depth)
{
  for (dt_top_t *t = f->items; t; t = t->next)
  {
    flat_t e;
    memset(&e, 0, sizeof(e));
    e.t = t;
    e.file = f->path;
    if (t->kind == TOP_INCLUDE && t->include && !t->sys_include && depth < 4)
    {
      if (is_board_family_dtsi(t->include))
      {
        char       p[A2D_MAX_PATH];
        dt_file_t *inc;
        if (dt_resolve_include(t->include, f->path, g->dirs, g->ndirs, p, sizeof(p)) &&
            (inc = dt_parse_file(p)) != NULL)
        {
          flat_t b = e;
          b.marker = 1;
          b.inc_name = t->include;
          VEC_PUSH(fl->v, fl->count, fl->cap, b);
          flatten(g, inc, fl, depth + 1);
          b.marker = 2;
          VEC_PUSH(fl->v, fl->count, fl->cap, b);
          continue;
        }
        log_warn("%s includes board file \"%s\", which is neither next to it nor in %s: the board content of this "
                 "similar DTS is MISSING - use a similar DTS from the same kernel tree",
                 path_basename(f->path), t->include, g->qcom_dir);
        addf(&g->notes, "board include \"%s\" of %s not found - its nodes are not part of this draft", t->include,
             path_basename(f->path));
      }
      if (is_soc_dtsi_name(t->include) && 0 != _stricmp(path_basename(t->include), g->soc_dtsi))
        e.replaced_soc = true;
    }
    VEC_PUSH(fl->v, fl->count, fl->cap, e);
  }
}

static const char *template_soc(const flatlist_t *fl, char *out, size_t cap)
{
  out[0] = 0;
  for (int i = 0; i < fl->count; i++)
  {
    const dt_top_t *t = fl->v[i].t;
    if (fl->v[i].marker || t->kind != TOP_NODE || t->node->ref)
      continue;
    {
      const char *c = dt_prop_value(t->node, "compatible");
      strlist_t   sl;
      memset(&sl, 0, sizeof(sl));
      dt_value_strings(c, &sl);
      for (int k = 0; k < sl.count; k++)
        if (str_starts_with(sl.v[k], "qcom,"))
          str_copy(out, cap, sl.v[k] + 5);
      sl_free(&sl);
    }
  }
  return out;
}

static dt_node_t *flat_find_ref(const flatlist_t *fl, const char *ref)
{
  for (int i = 0; i < fl->count; i++)
  {
    const dt_top_t *t = fl->v[i].t;
    if (!fl->v[i].marker && t->kind == TOP_NODE && t->node->ref && 0 == strcmp(t->node->ref, ref))
      return t->node;
  }
  return NULL;
}

static dt_node_t *flat_root(const flatlist_t *fl)
{
  for (int i = 0; i < fl->count; i++)
  {
    const dt_top_t *t = fl->v[i].t;
    if (!fl->v[i].marker && t->kind == TOP_NODE && !t->node->ref)
      return t->node;
  }
  return NULL;
}

/* ======================================================================== */
/* trees and matching                                                       */
/* ======================================================================== */

static void build_trees(gen_t *g)
{
  strlist_t incs;
  memset(&incs, 0, sizeof(incs));

  if (g->tmpl)
  {
    for (int i = 0; i < g->tflat.count; i++)
    {
      const flat_t *e = &g->tflat.v[i];
      if (e->marker || e->t->kind != TOP_INCLUDE || !e->t->include || e->t->sys_include)
        continue;
      sl_add_unique(&incs, e->replaced_soc ? g->soc_dtsi : e->t->include);
    }
  }
  if (!incs.count || !sl_contains(&incs, g->soc_dtsi))
  {
    /* template without direct SoC include (or no template) */
    bool has_soc = false;
    for (int i = 0; i < incs.count; i++)
      if (is_soc_dtsi_name(incs.v[i]))
        has_soc = true;
    if (!has_soc)
      sl_add_unique(&incs, g->soc_dtsi);
  }
  if (!g->tmpl && g->pmics_dtsi[0])
    sl_add_unique(&incs, g->pmics_dtsi);

  dt_tree_init(&g->base);
  dt_tree_init(&g->full);
  for (int i = 0; i < incs.count; i++)
  {
    char p[A2D_MAX_PATH];
    if (!dt_resolve_include(incs.v[i], g->tmpl ? g->tmpl->path : NULL, g->dirs, g->ndirs, p, sizeof(p)))
    {
      log_warn("include \"%s\" not found in %s", incs.v[i], g->qcom_dir);
      addf(&g->notes, "include \"%s\" could not be resolved in %s", incs.v[i], g->qcom_dir);
      continue;
    }
    dt_tree_add_file(&g->base, p, g->dirs, g->ndirs);
    dt_tree_add_file(&g->full, p, g->dirs, g->ndirs);
  }
  /* full tree = base + learned deletes + template board nodes */
  for (int i = 0; i < g->soc_deletes.count; i++)
  {
    dt_node_t *v = dt_tree_ref(&g->full, g->soc_deletes.v[i]);
    if (v && v->parent)
      v->deleted = true;
  }
  if (g->tmpl)
  {
    for (int i = 0; i < g->tflat.count; i++)
    {
      dt_file_t one;
      if (g->tflat.v[i].marker || g->tflat.v[i].t->kind == TOP_INCLUDE)
        continue;
      memset(&one, 0, sizeof(one));
      one.path = (char *)g->tflat.v[i].file;
      one.items = g->tflat.v[i].t;
      {
        dt_top_t *saved = one.items->next;
        one.items->next = NULL;
        dt_tree_add_parsed(&g->full, &one, g->dirs, g->ndirs, false);
        one.items->next = saved;
      }
    }
  }
  log_info("base tree: %d file(s), %d labels", g->base.files.count, (int)g->base.labels.count);
  for (int i = 0; i < g->base.missing.count; i++)
    log_verbose("unresolved include in SoC files: %s", g->base.missing.v[i]);
  sl_free(&incs);
}

static void index_units(gen_t *g, dt_node_t *n)
{
  uint64_t a;
  for (dt_node_t *c = n->children; c; c = c->next)
  {
    if (dt_unit_address(c, &a) && a >= 0x10000)
    {
      unit_t u = { a, c };
      VEC_PUSH(g->units, g->nunits, g->capunits, u);
    }
    index_units(g, c);
  }
}

static const char *kind_basename(acpi_res_kind_t k)
{
  return k == RES_I2C ? "i2c" : k == RES_SPI ? "spi" : k == RES_UART ? "serial" : NULL;
}

static acpi_res_kind_t node_bus_kind(const dt_node_t *n)
{
  char b[64];
  if (!n)
    return 0;
  dt_node_basename(n, b, sizeof(b));
  if (!strcmp(b, "i2c"))
    return RES_I2C;
  if (!strcmp(b, "spi"))
    return RES_SPI;
  if (!strcmp(b, "serial"))
    return RES_UART;
  return 0;
}

static void match_acpi(gen_t *g)
{
  acpi_ns_t *ns = g->x->ns;

  g->acpi_node = xcalloc((size_t)ns->count + 1, sizeof(dt_node_t *));
  g->covered = xcalloc((size_t)ns->count + 1, sizeof(bool));
  g->src_kind = xcalloc((size_t)ns->count + 1, 1);
  index_units(g, g->base.root);

  for (int i = 0; i < ns->count; i++)
    for (int r = 0; r < ns->d[i].nres; r++)
    {
      acpi_res_t *res = &ns->d[i].res[r];
      acpi_dev_t *s;
      if (res->kind != RES_I2C && res->kind != RES_SPI && res->kind != RES_UART)
        continue;
      if ((s = aml_find_device(ns, res->source)) != NULL)
        g->src_kind[s - ns->d] |= res->kind == RES_I2C ? 1 : res->kind == RES_SPI ? 2 : 4;
    }

  for (int i = 0; i < ns->count; i++)
  {
    acpi_dev_t *d = &ns->d[i];
    const char *seg = strrchr(d->path, '.');
    const char *want = NULL;

    seg = seg ? seg + 1 : d->path;
    if (g->src_kind[i] & 1)
      want = "i2c";
    else if (g->src_kind[i] & 2)
      want = "spi";
    else if (g->src_kind[i] & 4)
      want = "serial";
    else
    {
      const dm_dev_t *m = dm_for(g, d);
      const char     *nm = m ? m->name : "";
      if (str_starts_with_ci(seg, "I2C") || str_contains_ci(nm, "I2C") || str_contains_ci(d->str, "I2C"))
        want = "i2c";
      else if (str_starts_with_ci(seg, "SP") || str_contains_ci(nm, "SPI") || str_contains_ci(d->str, "SPI"))
        want = "spi";
      else if (str_starts_with_ci(seg, "UAR") || str_starts_with_ci(seg, "UR") || str_contains_ci(nm, "UART") ||
               str_contains_ci(d->str, "UART"))
        want = "serial";
    }

    for (int r = 0; r < d->nres && !g->acpi_node[i]; r++)
    {
      dt_node_t *best = NULL;
      int        best_score = -1;
      if (d->res[r].kind != RES_MEM || !d->hids.count)
        continue;
      for (int u = 0; u < g->nunits; u++)
      {
        int  score = 0;
        char b[64];
        if (g->units[u].addr != d->res[r].base)
          continue;
        dt_node_basename(g->units[u].node, b, sizeof(b));
        if (want && !strcmp(b, want))
          score += 10;
        if (g->units[u].node->labels.count)
          score += 2;
        if (score > best_score)
        {
          best = g->units[u].node;
          best_score = score;
        }
      }
      g->acpi_node[i] = best;
    }
  }
}

static acpi_dev_t *acpi_for_node(gen_t *g, const dt_node_t *n)
{
  if (!n)
    return NULL;
  for (int i = 0; i < g->x->ns->count; i++)
    if (g->acpi_node[i] == n)
      return &g->x->ns->d[i];
  return NULL;
}

static const acpi_res_t *dev_bus_res(const acpi_dev_t *d, acpi_res_kind_t kind, const char *src)
{
  for (int r = 0; r < d->nres; r++)
    if (d->res[r].kind == kind && (!src || 0 == _stricmp(d->res[r].source, src)))
      return &d->res[r];
  return NULL;
}

static acpi_dev_t *consumer_at(gen_t *g, const acpi_dev_t *ctrl, acpi_res_kind_t kind, uint32_t addr,
                               bool any_addr)
{
  for (int i = 0; i < g->x->ns->count; i++)
  {
    acpi_dev_t *d = &g->x->ns->d[i];
    for (int r = 0; r < d->nres; r++)
      if (d->res[r].kind == kind && 0 == _stricmp(d->res[r].source, ctrl->path) &&
          (any_addr || d->res[r].addr == addr))
        return d;
  }
  return NULL;
}

/* ======================================================================== */
/* GPIO decoding                                                            */
/* ======================================================================== */

static void setup_gpio(gen_t *g)
{
  dt_node_t *tlmm = dt_tree_label(&g->base, "tlmm");
  dt_node_t *pdc = dt_tree_label(&g->base, "pdc");
  uint64_t   a;
  strlist_t  cells;

  memset(&cells, 0, sizeof(cells));
  if (tlmm && dt_unit_address(tlmm, &a))
    g->gio = aml_find_by_mmio(g->x->ns, a, true);
  if (tlmm)
  {
    if (dt_value_cells(dt_prop_value(tlmm, "gpio-ranges"), &cells) >= 4)
      g->ngpios = atoi(cells.v[3]);
    sl_free(&cells);
    if (str_contains_ci(dt_prop_value(tlmm, "compatible"), "x1e80100-tlmm"))
    {
      g->wm = g_x1e80100_wake;
      g->nwm = (int)(sizeof(g_x1e80100_wake) / sizeof(g_x1e80100_wake[0]));
    }
  }
  if (pdc)
  {
    int n = dt_value_cells(dt_prop_value(pdc, "qcom,pdc-ranges"), &cells);
    for (int i = 0; i + 2 < n && g->npdc < 32; i += 3)
    {
      g->pdc[g->npdc][0] = (uint32_t)strtoul(cells.v[i], NULL, 0);
      g->pdc[g->npdc][1] = (uint32_t)strtoul(cells.v[i + 1], NULL, 0);
      g->pdc[g->npdc][2] = (uint32_t)strtoul(cells.v[i + 2], NULL, 0);
      g->npdc++;
    }
    sl_free(&cells);
  }
  if (g->gio)
    log_info("TLMM: ACPI %s, %d GPIOs, %d PDC ranges, %s wake map", g->gio->path, g->ngpios, g->npdc,
             g->wm ? "x1e80100" : "no");
  else
    log_warn("TLMM GPIO controller not found in ACPI - GPIO numbers cannot be verified");
}

/* returns the TLMM pin of an ACPI GPIO pin, -1 if not decodable */
static int tlmm_decode(gen_t *g, const char *source, uint16_t pin, char *how, size_t cap)
{
  int limit = g->ngpios ? g->ngpios : 200;

  if (how)
    how[0] = 0;
  if (!g->gio || _stricmp(source, g->gio->path))
  {
    if (how)
      snprintf(how, cap, "%s pin %u (not the TLMM)", source, pin);
    return -1;
  }
  if (pin < limit)
  {
    if (how)
      snprintf(how, cap, "TLMM %u", pin);
    return pin;
  }
  if (pin % 64 == 0)
  {
    int idx = pin / 64, k = 0;
    for (int r = 0; r < g->gio->nres; r++)
    {
      const acpi_res_t *res = &g->gio->res[r];
      if (res->kind != RES_IRQ || res->alt)
        continue;
      if (k++ == idx)
      {
        uint32_t spi = res->irq - 32;
        for (int p = 0; p < g->npdc; p++)
        {
          if (spi >= g->pdc[p][1] && spi < g->pdc[p][1] + g->pdc[p][2])
          {
            uint32_t pdcpin = g->pdc[p][0] + (spi - g->pdc[p][1]);
            for (int w = 0; w < g->nwm; w++)
              if (g->wm[w].pdc == pdcpin)
              {
                if (how)
                  snprintf(how, cap, "virtual pin %u = %s interrupt #%d, GSIV %u -> PDC %u -> TLMM %u", pin,
                           g->gio->path, idx, res->irq, pdcpin, g->wm[w].gpio);
                return g->wm[w].gpio;
              }
            if (how)
              snprintf(how, cap, "virtual pin %u -> GSIV %u -> PDC %u (no TLMM wake mapping)", pin, res->irq, pdcpin);
            return -1;
          }
        }
        if (how)
          snprintf(how, cap, "virtual pin %u -> GSIV %u (not in qcom,pdc-ranges)", pin, res->irq);
        return -1;
      }
    }
  }
  if (how)
    snprintf(how, cap, "virtual pin %u (not decodable)", pin);
  return -1;
}

static void build_tlmm_users(gen_t *g)
{
  for (int i = 0; i < g->x->ns->count; i++)
  {
    acpi_dev_t *d = &g->x->ns->d[i];
    for (int r = 0; r < d->nres; r++)
    {
      const acpi_res_t *res = &d->res[r];
      if (res->kind != RES_GPIO_INT && res->kind != RES_GPIO_IO)
        continue;
      for (int p = 0; p < res->npins; p++)
      {
        int  t = tlmm_decode(g, res->source, res->pins[p], NULL, 0);
        char line[256];
        if (t < 0 || t >= 512)
          continue;
        snprintf(line, sizeof(line), "%s%s of %s (%s)%s", res->kind == RES_GPIO_INT ? "GpioInt" : "GpioIo",
                 res->origin == RES_ORIGIN_AEI ? " (_AEI event)"
                 : res->origin == RES_ORIGIN_FIELD ? " (OpRegion field)" : "",
                 d->path, aml_dev_primary_hid(d), d->sta == 0 ? " [_STA=0]" : "");
        sl_add_unique(&g->tlmm_users[t], line);
      }
    }
  }
}

static int tlmm_pins_in_value(const char *raw, int *pins, int max)
{
  strlist_t cells;
  int       n = 0;
  memset(&cells, 0, sizeof(cells));
  dt_value_cells(raw, &cells);
  for (int i = 0; i + 1 < cells.count && n < max; i++)
    if (!strcmp(cells.v[i], "&tlmm"))
    {
      char *end;
      long  v = strtol(cells.v[i + 1], &end, 0);
      if (*end == 0)
        pins[n++] = (int)v;
    }
  sl_free(&cells);
  return n;
}

/* ======================================================================== */
/* descriptions                                                             */
/* ======================================================================== */

static void describe_acpi(gen_t *g, const acpi_dev_t *d, strlist_t *lines, bool with_resources)
{
  strbuf_t sb;
  sb_init(&sb);
  sb_printf(&sb, "[ACPI] %s", d->path);
  if (d->hids.count)
  {
    sb_puts(&sb, " HID ");
    for (int i = 0; i < d->hids.count; i++)
      sb_printf(&sb, "%s%s", i ? "|" : "", d->hids.v[i]);
    if (d->hid_is_method && d->hids.count > 1)
      sb_puts(&sb, " (_HID method, value depends on SKU)");
  }
  if (d->cids.count)
  {
    sb_puts(&sb, " CID ");
    for (int i = 0; i < d->cids.count; i++)
      sb_printf(&sb, "%s%s", i ? "|" : "", d->cids.v[i]);
  }
  if (d->has_uid)
    sb_printf(&sb, " UID %s", d->uid);
  if (d->str[0])
    sb_printf(&sb, " \"%s\"", d->str);
  if (d->sta == 0)
    sb_puts(&sb, " _STA=0 (DISABLED by firmware)");
  else if (d->sta < 0)
    sb_puts(&sb, " _STA=dynamic");
  sb_printf(&sb, " (%s)", d->table);
  sl_add(lines, sb.buf);
  sb_clear(&sb);

  if (with_resources)
  {
    for (int r = 0; r < d->nres; r++)
    {
      const acpi_res_t *res = &d->res[r];
      sb_clear(&sb);
      sb_puts(&sb, "[ACPI]   ");
      aml_res_describe(res, &sb);
      if (res->kind == RES_GPIO_INT || res->kind == RES_GPIO_IO)
      {
        for (int p = 0; p < res->npins; p++)
        {
          char how[256];
          int  t = tlmm_decode(g, res->source, res->pins[p], how, sizeof(how));
          if (t >= 0 && res->pins[p] != t)
            sb_printf(&sb, " => %s", how);
        }
      }
      sl_add(lines, sb.buf);
    }
    if (d->crs_alternatives > 1)
      addf(lines, "[ACPI]   NOTE: _CRS is a method with %d alternative resource templates", d->crs_alternatives);
    if (d->hid_descr_addr >= 0)
      addf(lines, "[ACPI]   _DSM HID descriptor address: 0x%x", d->hid_descr_addr);
    if (d->deps.count)
    {
      sb_clear(&sb);
      sb_puts(&sb, "[ACPI]   _DEP:");
      for (int i = 0; i < d->deps.count; i++)
        sb_printf(&sb, " %s", d->deps.v[i]);
      sl_add(lines, sb.buf);
    }
  }

  {
    const dm_dev_t *m = dm_for(g, d);
    if (m)
    {
      addf(lines, "[DevMgr] \"%s\" class %s, %s, driver %s%s%s%s", m->name, m->cls[0] ? m->cls : "-",
           devmgr_status_text(m), m->inf[0] ? m->inf : "none", m->provider[0] ? " (" : "", m->provider,
           m->provider[0] ? ")" : "");
      if (m->driver_store_dir[0] && g->x->drv)
      {
        const drv_pkg_t *p = drivers_pkg_by_dir(g->x->drv, m->driver_store_dir);
        if (p)
        {
          sb_clear(&sb);
          sb_printf(&sb, "[INF] %s  %s", p->original_inf, p->driver_ver);
          if (p->firmware.count)
          {
            sb_puts(&sb, "  firmware:");
            for (int i = 0; i < p->firmware.count && i < 8; i++)
              sb_printf(&sb, " %s", p->firmware.v[i]);
            if (p->firmware.count > 8)
              sb_printf(&sb, " (+%d)", p->firmware.count - 8);
          }
          sl_add(lines, sb.buf);
        }
      }
    }
    else if (g->x->dm && g->x->dm->count)
      sl_add(lines, "[DevMgr] no present device with this ACPI path");
  }
  sb_free(&sb);
}

/* ======================================================================== */
/* annotations                                                              */
/* ======================================================================== */

static void check_consumer_props(gen_t *g, const dt_node_t *n, const acpi_dev_t *c, strlist_t *lines, bool *mismatch)
{
  int             irq_tlmm = -1;
  const acpi_res_t *irq_res = NULL;
  int             io_pins[16];
  int             nio = 0;

  for (int r = 0; r < c->nres; r++)
  {
    const acpi_res_t *res = &c->res[r];
    for (int p = 0; p < res->npins; p++)
    {
      int t = tlmm_decode(g, res->source, res->pins[p], NULL, 0);
      if (t < 0)
        continue;
      if (res->kind == RES_GPIO_INT && irq_tlmm < 0)
      {
        irq_tlmm = t;
        irq_res = res;
      }
      else if (res->kind == RES_GPIO_IO && nio < 16)
        io_pins[nio++] = t;
    }
  }

  for (const dt_prop_t *p = n->props; p; p = p->next)
  {
    int pins[8], np;
    if (p->directive || !p->value)
      continue;
    np = tlmm_pins_in_value(p->value, pins, 8);
    if (!np)
      continue;
    if (!strcmp(p->name, "interrupts-extended") || !strcmp(p->name, "interrupts"))
    {
      if (irq_tlmm < 0)
        addf(lines, "CHECK: %s uses TLMM %d, ACPI has no decodable GpioInt for this device", p->name, pins[0]);
      else if (pins[0] == irq_tlmm)
        addf(lines, "OK: %s TLMM %d matches ACPI GpioInt", p->name, pins[0]);
      else
      {
        addf(lines, "WARNING: %s uses TLMM %d but ACPI GpioInt decodes to TLMM %d", p->name, pins[0], irq_tlmm);
        *mismatch = true;
      }
    }
    else if (str_ends_with_ci(p->name, "gpios") || !strcmp(p->name, "gpios"))
    {
      bool found = false;
      for (int i = 0; i < nio; i++)
        if (io_pins[i] == pins[0])
          found = true;
      if (found)
        addf(lines, "OK: %s TLMM %d is a GpioIo of this ACPI device", p->name, pins[0]);
      else if (nio)
      {
        strbuf_t sb;
        sb_init(&sb);
        sb_printf(&sb, "CHECK: %s uses TLMM %d; ACPI GpioIo pins of this device:", p->name, pins[0]);
        for (int i = 0; i < nio; i++)
          sb_printf(&sb, " %d", io_pins[i]);
        sl_add(lines, sb.buf);
        sb_free(&sb);
      }
    }
  }
  if (irq_tlmm >= 0 && !dt_prop(n, "interrupts-extended") && !dt_prop(n, "interrupts"))
    addf(lines, "NOTE: ACPI GpioInt -> interrupts-extended = <&tlmm %d %s>;", irq_tlmm, irq_type_name(irq_res));
  if (c->hid_descr_addr >= 0)
  {
    uint64_t v;
    const char *hd = dt_prop_value(n, "hid-descr-addr");
    if (hd && dt_value_first_u64(hd, &v))
    {
      if ((int)v == c->hid_descr_addr)
        addf(lines, "OK: hid-descr-addr 0x%x matches ACPI _DSM", c->hid_descr_addr);
      else
      {
        addf(lines, "WARNING: hid-descr-addr 0x%llx, ACPI _DSM returns 0x%x", (unsigned long long)v,
             c->hid_descr_addr);
        *mismatch = true;
      }
    }
  }
}

static void annotate_node(gen_t *g, const dt_node_t *n, ectx_t *ec, strlist_t *lines)
{
  if (ec->origin)
    sl_add(lines, ec->origin);
  if (ec->missing_ref)
  {
    addf(lines, "WARNING: %s is not defined by the included SoC/PMIC files - remove or fix", n->ref);
    g->st->warnings++;
  }
  if (ec->acpi && !ec->bus_child)
  {
    describe_acpi(g, ec->acpi, lines, false);
    if (ec->acpi->sta == 0)
      sl_add(lines, "WARNING: ACPI _STA returns 0: firmware hides this device on this machine");
    g->st->confirmed++;
  }
  else if (ec->target && !ec->bus_child && node_bus_kind(ec->target) && ec->target->parent)
  {
    uint64_t a;
    if (dt_unit_address(ec->target, &a))
      addf(lines, "[ACPI] no device with MMIO base 0x%llx: this bus is not used by Windows on this machine",
           (unsigned long long)a);
  }

  if (ec->bus_child)
  {
    uint64_t reg = 0;
    bool     has_reg = dt_value_first_u64(dt_prop_value(n, "reg"), &reg);
    if (ec->consumer)
    {
      bool mismatch = false;
      describe_acpi(g, ec->consumer, lines, true);
      check_consumer_props(g, n, ec->consumer, lines, &mismatch);
      if (mismatch)
        g->st->mismatches++;
      else
        g->st->confirmed++;
    }
    else if (has_reg || ec->parent_kind == RES_UART)
    {
      acpi_dev_t *elsewhere = NULL;
      if (has_reg)
      {
        for (int i = 0; i < g->x->ns->count && !elsewhere; i++)
        {
          const acpi_res_t *r = dev_bus_res(&g->x->ns->d[i], ec->parent_kind, NULL);
          if (r && r->addr == (uint32_t)reg)
            elsewhere = &g->x->ns->d[i];
        }
        addf(lines, "WARNING: no ACPI device at %s address 0x%llx on %s on this machine - verify or remove",
             ec->parent_kind == RES_I2C ? "I2C" : "SPI chip-select", (unsigned long long)reg, ec->parent_ctrl->path);
        if (elsewhere)
        {
          const acpi_res_t *r = dev_bus_res(elsewhere, ec->parent_kind, NULL);
          acpi_dev_t       *ctl = aml_find_device(g->x->ns, r->source);
          dt_node_t        *dn = ctl ? g->acpi_node[ctl - g->x->ns->d] : NULL;
          char              ref[256] = "?";
          if (dn)
            base_ref(dn, ref, sizeof(ref));
          addf(lines, "NOTE: ACPI has %s (%s) at this address on %s (%s)", elsewhere->path,
               aml_dev_primary_hid(elsewhere), r->source, ref);
        }
      }
      else
        addf(lines, "WARNING: no ACPI device uses %s as UART on this machine", ec->parent_ctrl->path);
      g->st->not_in_acpi++;
    }
  }

  if (ec->in_tlmm)
  {
    strlist_t pins;
    memset(&pins, 0, sizeof(pins));
    dt_value_strings(dt_prop_value(n, "pins"), &pins);
    for (int i = 0; i < pins.count; i++)
    {
      int t;
      if (!str_starts_with(pins.v[i], "gpio"))
        continue;
      t = atoi(pins.v[i] + 4);
      if (t < 0 || t >= 512)
        continue;
      if (g->tlmm_users[t].count)
        for (int k = 0; k < g->tlmm_users[t].count; k++)
          addf(lines, "[ACPI] TLMM %d: %s", t, g->tlmm_users[t].v[k]);
      else if (g->gio)
        addf(lines, "[ACPI] TLMM %d: not referenced by any ACPI device", t);
    }
    sl_free(&pins);
  }
}

/* ======================================================================== */
/* firmware-name                                                            */
/* ======================================================================== */

static char *rewrite_firmware(gen_t *g, const dt_node_t *n, const char *value, int depth, strlist_t *lines)
{
  strlist_t   names;
  strbuf_t    sb;
  const sysinfo_t *si = g->x->si;

  memset(&names, 0, sizeof(names));
  dt_value_strings(value, &names);
  if (!names.count || !g->x->drv)
  {
    sl_free(&names);
    return NULL;
  }
  sb_init(&sb);
  for (int i = 0; i < names.count; i++)
  {
    char             why[512], rel[256];
    const char      *base = path_basename(names.v[i]);
    const fw_file_t *f = fw_resolve(g->x->drv, base, g->soc, why, sizeof(why));
    if (f)
    {
      char reason[256];
      snprintf(rel, sizeof(rel), "qcom/%s/%s/%s/%s", g->soc, si->vendor_dir, si->board_slug, f->name);
      snprintf(reason, sizeof(reason), "firmware-name of %s", node_title(n));
      fw_plan_add(g->x->fw, rel, f->path, reason);
      addf(lines, "[FW] %s: %s -> /lib/firmware/%s", base, why, rel);
      g->st->firmware++;
    }
    else
    {
      str_copy(rel, sizeof(rel), names.v[i]);
      addf(lines, "[FW] WARNING: %s (template path kept; maybe from linux-firmware)", why);
    }
    if (i)
    {
      sb_puts(&sb, ",\n");
      sb_indent(&sb, depth + 4);
    }
    sb_printf(&sb, "\"%s\"", rel);
  }
  sl_free(&names);
  return sb_detach(&sb);
}

/* ======================================================================== */
/* label resolution checks for taken-over nodes                             */
/* ======================================================================== */

static void collect_labels_defined(const dt_node_t *n, strlist_t *out)
{
  for (int i = 0; i < n->labels.count; i++)
    sl_add_unique(out, n->labels.v[i]);
  for (const dt_node_t *c = n->children; c; c = c->next)
    collect_labels_defined(c, out);
}

static void collect_refs_used(const dt_node_t *n, strlist_t *out)
{
  for (const dt_prop_t *p = n->props; p; p = p->next)
  {
    const char *s = p->value;
    while (s && (s = strchr(s, '&')) != NULL)
    {
      const char *e = ++s;
      if (*e == '{')
        continue;
      while (*e && ((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') || (*e >= '0' && *e <= '9') || *e == '_'))
        e++;
      if (e > s)
      {
        char *l = xstrndup(s, (size_t)(e - s));
        sl_add_unique(out, l);
        free(l);
      }
      s = e;
    }
  }
  for (const dt_node_t *c = n->children; c; c = c->next)
    collect_refs_used(c, out);
}

static bool refs_resolvable(gen_t *g, const dt_node_t *n, strbuf_t *missing)
{
  strlist_t def, used;
  bool      ok = true;
  memset(&def, 0, sizeof(def));
  memset(&used, 0, sizeof(used));
  collect_labels_defined(n, &def);
  collect_refs_used(n, &used);
  for (int i = 0; i < used.count; i++)
  {
    if (sl_contains(&def, used.v[i]) || dt_tree_label(&g->full, used.v[i]))
      continue;
    ok = false;
    if (missing)
      sb_printf(missing, " &%s", used.v[i]);
  }
  sl_free(&def);
  sl_free(&used);
  return ok;
}

/* ======================================================================== */
/* ACPI-derived candidate nodes                                             */
/* ======================================================================== */

static const char *compat_for(const acpi_dev_t *d)
{
  for (size_t i = 0; i < sizeof(g_hid_compat) / sizeof(g_hid_compat[0]); i++)
    if (aml_dev_has_id(d, g_hid_compat[i].id))
      return g_hid_compat[i].compatible;
  return NULL;
}

static void candidate_name(gen_t *g, const acpi_dev_t *d, char *out, size_t cap)
{
  const dm_dev_t *m = dm_for(g, d);
  const char     *nm = m ? m->name : "";
  const char     *seg = strrchr(d->path, '.');

  seg = seg ? seg + 1 : d->path;
  if (str_contains_ci(nm, "touch pad") || str_contains_ci(nm, "touchpad") || str_starts_with_ci(seg, "TCP"))
    str_copy(out, cap, "touchpad");
  else if (str_contains_ci(nm, "keyboard") || str_starts_with_ci(seg, "KB") || str_contains_ci(seg, "KB"))
    str_copy(out, cap, "keyboard");
  else if (str_contains_ci(nm, "touch screen") || str_contains_ci(nm, "touchscreen") || str_starts_with_ci(seg, "TSC"))
    str_copy(out, cap, "touchscreen");
  else if (compat_for(d))
    str_copy(out, cap, "hid");
  else
  {
    str_copy(out, cap, seg);
    str_lower(out);
  }
}

static void emit_candidate(gen_t *g, strbuf_t *sb, int depth, acpi_dev_t *d, acpi_dev_t *ctrl, acpi_res_kind_t kind)
{
  const acpi_res_t *bus = dev_bus_res(d, kind, ctrl->path);
  const char       *compat = compat_for(d);
  const acpi_res_t *irq = NULL;
  int               irq_tlmm = -1;
  strlist_t         lines;
  strbuf_t          node;
  char              name[64];
  bool              real;

  memset(&lines, 0, sizeof(lines));
  sb_init(&node);
  for (int r = 0; r < d->nres && irq_tlmm < 0; r++)
    if (d->res[r].kind == RES_GPIO_INT && d->res[r].npins)
    {
      irq = &d->res[r];
      irq_tlmm = tlmm_decode(g, irq->source, irq->pins[0], NULL, 0);
    }

  candidate_name(g, d, name, sizeof(name));
  real = compat && kind == RES_I2C && irq_tlmm >= 0 && d->sta != 0;

  sl_add(&lines, real ? "[ACPI-DERIVED] generated from ACPI (no similar DTS describes this device)"
                      : "[ACPI-DERIVED] suggestion (commented out): no binding known for this ACPI ID");
  describe_acpi(g, d, &lines, true);
  if (real)
    sl_add(&lines, "TODO: pinctrl-0 (interrupt pin config) and supplies are not described by ACPI");

  if (kind == RES_UART)
  {
    sb_indent(&node, depth);
    sb_printf(&node, "%s {\n", name);
    sb_indent(&node, depth + 1);
    sb_printf(&node, "compatible = \"TODO\";\t/* ACPI %s, e.g. bluetooth: \"qcom,wcn6855-bt\" */\n",
              aml_dev_primary_hid(d));
    if (bus && bus->speed)
    {
      sb_indent(&node, depth + 1);
      sb_printf(&node, "max-speed = <%u>;\n", bus->speed);
    }
  }
  else
  {
    sb_indent(&node, depth);
    sb_printf(&node, "%s@%x {\n", name, bus ? bus->addr : 0);
    sb_indent(&node, depth + 1);
    if (compat)
      sb_printf(&node, "compatible = \"%s\";\n", compat);
    else
      sb_printf(&node, "compatible = \"TODO\";\t/* ACPI %s */\n", aml_dev_primary_hid(d));
    sb_indent(&node, depth + 1);
    sb_printf(&node, "reg = <0x%x>;\n", bus ? bus->addr : 0);
    if (kind == RES_SPI && bus)
    {
      sb_indent(&node, depth + 1);
      sb_printf(&node, "spi-max-frequency = <%u>;\n", bus->speed);
    }
    if (compat && !strcmp(compat, "hid-over-i2c"))
    {
      sb_putc(&node, '\n');
      sb_indent(&node, depth + 1);
      if (d->hid_descr_addr >= 0)
        sb_printf(&node, "hid-descr-addr = <0x%x>;\n", d->hid_descr_addr);
      else
        sb_puts(&node, "hid-descr-addr = <0x1>;\t/* TODO: not found in ACPI _DSM */\n");
    }
  }
  if (irq_tlmm >= 0)
  {
    sb_indent(&node, depth + 1);
    sb_printf(&node, "interrupts-extended = <&tlmm %d %s>;\n", irq_tlmm,
              (compat && !strcmp(compat, "hid-over-i2c")) ? "IRQ_TYPE_LEVEL_LOW" : irq_type_name(irq));
  }
  for (int r = 0; r < d->nres; r++)
  {
    const acpi_res_t *res = &d->res[r];
    if (res->kind != RES_GPIO_IO)
      continue;
    for (int p = 0; p < res->npins; p++)
    {
      int t = tlmm_decode(g, res->source, res->pins[p], NULL, 0);
      sb_indent(&node, depth + 1);
      if (t >= 0)
        sb_printf(&node, "todo-gpios = <&tlmm %d GPIO_ACTIVE_HIGH>;\t/* ACPI GpioIo: function/polarity unknown */\n",
                  t);
      else
        sb_printf(&node, "/* ACPI GpioIo %s pin %u */\n", res->source, res->pins[p]);
    }
  }
  if (irq && irq->wake)
  {
    sb_putc(&node, '\n');
    sb_indent(&node, depth + 1);
    sb_puts(&node, "wakeup-source;\n");
  }
  sb_indent(&node, depth);
  sb_puts(&node, "};\n");

  sb_putc(sb, '\n');
  emit_lines_comment(sb, depth, &lines);
  if (real)
  {
    sb_puts(sb, node.buf);
    g->st->added_acpi++;
  }
  else
  {
    comment_out(sb, node.buf, depth);
    g->st->commented++;
  }
  sl_free(&lines);
  sb_free(&node);
}

/* emits all not yet covered consumers of ctrl */
static void emit_candidates_for(gen_t *g, strbuf_t *sb, int depth, acpi_dev_t *ctrl, acpi_res_kind_t kind)
{
  for (int i = 0; i < g->x->ns->count; i++)
  {
    acpi_dev_t *d = &g->x->ns->d[i];
    if (g->covered[i] || !dev_bus_res(d, kind, ctrl->path))
      continue;
    g->covered[i] = true;
    emit_candidate(g, sb, depth, d, ctrl, kind);
  }
}

/* ======================================================================== */
/* node emission                                                            */
/* ======================================================================== */

static void child_ctx(gen_t *g, const ectx_t *pc, const dt_node_t *c, ectx_t *cc)
{
  memset(cc, 0, sizeof(*cc));
  cc->target = pc->target ? dt_child(pc->target, c->name) : NULL;
  if (cc->target && !strchr(c->name, '@') && strchr(cc->target->name, '@'))
    cc->target = NULL; /* do not match "name" against "name@addr" loosely */
  cc->in_tlmm = pc->in_tlmm;
  if (pc->bus_ctrl)
  {
    uint64_t reg;
    cc->bus_child = true;
    cc->parent_ctrl = pc->bus_ctrl;
    cc->parent_kind = pc->bus_kind;
    if (pc->bus_kind == RES_UART)
      cc->consumer = consumer_at(g, pc->bus_ctrl, RES_UART, 0, true);
    else if (dt_value_first_u64(dt_prop_value(c, "reg"), &reg))
      cc->consumer = consumer_at(g, pc->bus_ctrl, pc->bus_kind, (uint32_t)reg, false);
    else
      cc->bus_child = false; /* e.g. "ports" without reg */
  }
  else
  {
    cc->acpi = cc->target ? acpi_for_node(g, cc->target) : NULL;
    if (cc->acpi && node_bus_kind(cc->target))
    {
      cc->bus_ctrl = cc->acpi;
      cc->bus_kind = node_bus_kind(cc->target);
    }
  }
}

static void mark_covered_tree(gen_t *g, const dt_node_t *n, const ectx_t *ec)
{
  if (ec->consumer)
    g->covered[ec->consumer - g->x->ns->d] = true;
  for (const dt_node_t *c = n->children; c; c = c->next)
  {
    ectx_t cc;
    child_ctx(g, ec, c, &cc);
    mark_covered_tree(g, c, &cc);
  }
}

static void emit_secondary_extras(gen_t *g, strbuf_t *sb, const dt_node_t *n, int depth, ectx_t *ec)
{
  strlist_t done;
  memset(&done, 0, sizeof(done));

  for (int s = 0; s < g->nsec; s++)
  {
    const dt_node_t *sn = ec->sec[s];
    if (!sn)
      continue;

    /* properties the template lacks */
    {
      strlist_t lines;
      memset(&lines, 0, sizeof(lines));
      for (const dt_prop_t *p = sn->props; p; p = p->next)
      {
        if (p->directive || dt_prop(n, p->name))
          continue;
        if (p->value)
          addf(&lines, "%s = %s;", p->name, p->value);
        else
          addf(&lines, "%s;", p->name);
      }
      if (lines.count)
      {
        strbuf_t tmp;
        sb_init(&tmp);
        sb_putc(sb, '\n');
        sb_indent(sb, depth + 1);
        sb_printf(sb, "/* [SIMILAR%d] %s additionally sets (not taken over): */\n", s + 2,
                  path_basename(g->sec[s]->path));
        for (int i = 0; i < lines.count; i++)
        {
          sb_indent(&tmp, depth + 1);
          sb_puts(&tmp, lines.v[i]);
          sb_putc(&tmp, '\n');
        }
        comment_out(sb, tmp.buf, depth + 1);
        sb_free(&tmp);
        g->st->commented++;
      }
      sl_free(&lines);
    }

    for (const dt_node_t *sc = sn->children; sc; sc = sc->next)
    {
      bool     exists = false;
      ectx_t   cc;
      strbuf_t missing, tmp;
      char     origin[512];
      bool     real;

      for (const dt_node_t *c = n->children; c; c = c->next)
        if (!strcmp(c->name, sc->name))
          exists = true;
      if (exists || sl_contains(&done, sc->name))
        continue;
      sl_add(&done, sc->name);

      child_ctx(g, ec, sc, &cc);
      sb_init(&missing);
      real = cc.bus_child && cc.consumer && refs_resolvable(g, sc, &missing);
      if (cc.bus_child && cc.consumer && !real)
        snprintf(origin, sizeof(origin), "[SIMILAR%d] from %s, confirmed by ACPI but unresolved labels:%s", s + 2,
                 path_basename(g->sec[s]->path), missing.buf ? missing.buf : "");
      else if (real)
        snprintf(origin, sizeof(origin), "[SIMILAR%d] taken over from %s (confirmed by ACPI)", s + 2,
                 path_basename(g->sec[s]->path));
      else
        snprintf(origin, sizeof(origin), "[SIMILAR%d] node only in %s - review%s", s + 2,
                 path_basename(g->sec[s]->path), cc.bus_child ? " (NOT present in ACPI)" : "");
      sb_free(&missing);
      cc.origin = origin;

      sb_putc(sb, '\n');
      if (real)
      {
        mark_covered_tree(g, sc, &cc);
        emit_node(g, sb, sc, depth + 1, &cc);
        g->st->added_similar++;
      }
      else
      {
        strlist_t one;
        memset(&one, 0, sizeof(one));
        sl_add(&one, origin);
        emit_lines_comment(sb, depth + 1, &one);
        sl_free(&one);
        cc.origin = NULL;
        sb_init(&tmp);
        emit_node(g, &tmp, sc, depth + 1, &cc);
        comment_out(sb, tmp.buf, depth + 1);
        sb_free(&tmp);
        g->st->commented++;
      }
    }
  }
  sl_free(&done);
}

static void emit_prop(gen_t *g, strbuf_t *sb, const dt_node_t *n, const dt_prop_t *p, int depth, ectx_t *ec)
{
  strlist_t lines;
  char     *newval = NULL;

  memset(&lines, 0, sizeof(lines));
  if (p->directive == DT_DIR_PREPROC)
  {
    emit_raw_comment(sb, depth, p->comment);
    sb_puts(sb, p->name);
    sb_putc(sb, '\n');
    return;
  }
  emit_raw_comment(sb, depth, p->comment);
  if (p->directive)
  {
    sb_indent(sb, depth);
    sb_printf(sb, "%s %s;\n", p->directive == DT_DIR_DEL_PROP ? "/delete-property/" : "/delete-node/", p->name);
    return;
  }

  if (ec->is_root && !strcmp(p->name, "model"))
  {
    newval = xmalloc(strlen(g->x->si->model) + 3);
    sprintf(newval, "\"%s\"", g->x->si->model);
    addf(&lines, "[SMBIOS] \"%s\" / \"%s\" / board \"%s\"; template: %s", g->x->si->manufacturer,
         g->x->si->family[0] ? g->x->si->family : g->x->si->product, g->x->si->board_product, p->value);
  }
  else if (ec->is_root && !strcmp(p->name, "compatible"))
  {
    newval = xmalloc(strlen(g->x->si->compatible) + strlen(g->soc) + 32);
    sprintf(newval, "\"%s\", \"qcom,%s\"", g->x->si->compatible, g->soc);
    addf(&lines, "[SMBIOS] derived board compatible (verify the vendor naming); template: %s", p->value);
  }
  else if (!strcmp(p->name, "firmware-name") && p->value)
    newval = rewrite_firmware(g, n, p->value, depth, &lines);
  else if (!ec->bus_child && !ec->in_tlmm && p->value &&
           (!strcmp(p->name, "gpios") || str_ends_with_ci(p->name, "-gpios") ||
            !strcmp(p->name, "interrupts-extended")))
  {
    int pins[8];
    int np = tlmm_pins_in_value(p->value, pins, 8);
    for (int i = 0; i < np; i++)
    {
      if (pins[i] < 0 || pins[i] >= 512)
        continue;
      if (g->tlmm_users[pins[i]].count)
        for (int k = 0; k < g->tlmm_users[pins[i]].count; k++)
          addf(&lines, "[ACPI] TLMM %d: %s", pins[i], g->tlmm_users[pins[i]].v[k]);
      else if (g->gio)
        addf(&lines, "[ACPI] TLMM %d: not referenced by any ACPI device", pins[i]);
    }
  }

  emit_lines_comment(sb, depth, &lines);
  sl_free(&lines);
  sb_indent(sb, depth);
  if (newval || p->value)
    sb_printf(sb, "%s = %s;\n", p->name, newval ? newval : p->value);
  else
    sb_printf(sb, "%s;\n", p->name);
  free(newval);
}

static void emit_node(gen_t *g, strbuf_t *sb, const dt_node_t *n, int depth, ectx_t *ec)
{
  strlist_t lines;
  bool      first = true;

  memset(&lines, 0, sizeof(lines));
  emit_raw_comment(sb, depth, n->comment);
  annotate_node(g, n, ec, &lines);
  emit_lines_comment(sb, depth, &lines);
  sl_free(&lines);

  sb_indent(sb, depth);
  if (n->omit_if_no_ref)
    sb_puts(sb, "/omit-if-no-ref/ ");
  for (int i = 0; i < n->labels.count; i++)
    sb_printf(sb, "%s: ", n->labels.v[i]);
  sb_printf(sb, "%s {\n", n->ref ? n->ref : n->name);

  for (const dt_prop_t *p = n->props; p; p = p->next)
  {
    if (p->blank_before && !first)
      sb_putc(sb, '\n');
    emit_prop(g, sb, n, p, depth + 1, ec);
    first = false;
  }

  for (const dt_node_t *c = n->children; c; c = c->next)
  {
    ectx_t cc;
    child_ctx(g, ec, c, &cc);
    for (int s = 0; s < g->nsec; s++)
    {
      const dt_node_t *sp = ec->sec[s];
      cc.sec[s] = NULL;
      if (sp)
        for (const dt_node_t *x = sp->children; x; x = x->next)
          if (!strcmp(x->name, c->name))
            cc.sec[s] = x;
    }
    if (!first)
      sb_putc(sb, '\n');
    emit_node(g, sb, c, depth + 1, &cc);
    first = false;
  }

  emit_secondary_extras(g, sb, n, depth, ec);

  if (ec->bus_ctrl)
    emit_candidates_for(g, sb, depth + 1, ec->bus_ctrl, ec->bus_kind);

  if (n->tail_comment)
    emit_raw_comment(sb, depth + 1, n->tail_comment);
  sb_indent(sb, depth);
  sb_puts(sb, "};\n");
}

/* top-level node of the template (root or &ref) */
static void top_ctx(gen_t *g, const dt_node_t *n, ectx_t *ec)
{
  memset(ec, 0, sizeof(*ec));
  if (n->ref)
  {
    ec->target = dt_tree_ref(&g->base, n->ref);
    if (!ec->target && !dt_tree_ref(&g->full, n->ref))
      ec->missing_ref = true;
    if (0 == strcmp(n->ref, "&tlmm"))
      ec->in_tlmm = true;
    ec->acpi = acpi_for_node(g, ec->target);
    if (ec->acpi && node_bus_kind(ec->target))
    {
      ec->bus_ctrl = ec->acpi;
      ec->bus_kind = node_bus_kind(ec->target);
    }
    for (int s = 0; s < g->nsec; s++)
      ec->sec[s] = flat_find_ref(&g->sflat[s], n->ref);
  }
  else
  {
    ec->target = g->base.root;
    ec->is_root = true;
    for (int s = 0; s < g->nsec; s++)
      ec->sec[s] = flat_root(&g->sflat[s]);
  }
}

/* ======================================================================== */
/* header, appendix                                                         */
/* ======================================================================== */

static void emit_header(gen_t *g)
{
  strbuf_t        *sb = &g->out;
  const sysinfo_t *si = g->x->si;
  time_t           now = time(NULL);
  struct tm        tmv;
  char             date[32];
  const acpi_table_t *dsdt = acpi_find_table(g->x->tables, "DSDT", 0);

  localtime_s(&tmv, &now);
  strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &tmv);

  sb_puts(sb, "// SPDX-License-Identifier: BSD-3-Clause\n/*\n");
  sb_printf(sb, " * %s\n", si->model);
  sb_puts(sb, " *\n");
  sb_printf(sb, " * DRAFT generated by acpi2dt %s on %s.\n", A2D_VERSION_STRING, date);
  sb_puts(sb, " * This file is a PROPOSAL: review every node before using it. ACPI does not\n");
  sb_puts(sb, " * describe clocks, regulators, pin configuration or power sequencing.\n");
  sb_puts(sb, " *\n");
  sb_puts(sb, " * Machine\n");
  sb_printf(sb, " *   SMBIOS     : %s / %s / family \"%s\" / board %s / BIOS %s\n", si->manufacturer, si->product,
            si->family, si->board_product, si->bios_version);
  sb_printf(sb, " *   CPU        : %s (MADT: %d of %d GIC CPU interfaces enabled)\n", si->cpu_name,
            si->madt_gicc_enabled, si->madt_gicc_total);
  sb_printf(sb, " *   SoC        : %s (%s), include %s\n", g->soc, si->soc_reason, g->soc_dtsi);
  sb_puts(sb, " *\n * Information sources and annotation tags\n");
  sb_printf(sb, " *   [ACPI]     %d ACPI tables, DSDT OEM \"%s\" \"%s\" rev 0x%x; %d devices parsed\n",
            g->x->tables->count, dsdt ? dsdt->oem_id : "-", dsdt ? dsdt->oem_table_id : "-",
            dsdt ? dsdt->oem_revision : 0, g->x->ns->count);
  sb_printf(sb, " *   [DevMgr]   Windows Device Manager: %d present devices\n", g->x->dm ? g->x->dm->count : 0);
  sb_printf(sb, " *   [INF]      %d driver packages (DriverStore) of present devices\n", g->x->drv ? g->x->drv->np : 0);
  sb_printf(sb, " *   [FW]       firmware copied to %s (see FIRMWARE-MANIFEST.txt)\n", g->x->cfg->output_fw_dir);
  sb_printf(sb, " *   [SMBIOS]   model / compatible\n");
  sb_printf(sb, " *   [KERNEL]   %s\n", g->x->cfg->kernel_dts_dir);
  if (g->tmpl)
    sb_printf(sb, " *   [TEMPLATE] %s (primary similar DTS, SoC %s)\n", g->tmpl->path,
              g->tmpl_soc[0] ? g->tmpl_soc : "?");
  for (int s = 0; s < g->nsec; s++)
    sb_printf(sb, " *   [SIMILAR%d] %s\n", s + 2, g->sec[s]->path);
  sb_puts(sb, " *   [ACPI-DERIVED] node generated from ACPI only\n");
  sb_puts(sb, " *   OK / CHECK / WARNING / TODO: result of cross-checks, items for the reviewer\n");
  sb_puts(sb, " *\n * Intermediate data (ACPI dumps, .dsl, JSON inventories): ");
  sb_puts(sb, g->x->cfg->work_dir);
  sb_puts(sb, "\n */\n");
}

static bool is_spdx_comment_line(const char *c)
{
  return str_starts_with(c, "// SPDX");
}

static void emit_template_comment(strbuf_t *sb, const char *c)
{
  /* drop the SPDX line of the template (our header has one) */
  if (!c)
    return;
  if (is_spdx_comment_line(c))
  {
    const char *nl = strchr(c, '\n');
    if (!nl)
      return;
    c = nl + 1;
  }
  if (*c)
  {
    sb_puts(sb, c);
    sb_putc(sb, '\n');
  }
}

static void emit_appendix(gen_t *g)
{
  strbuf_t   *sb = &g->out;
  acpi_ns_t  *ns = g->x->ns;
  strbuf_t    body;

  sb_init(&body);
  sb_puts(&body, "\n==================================================================\n");
  sb_puts(&body, "APPENDIX A: SoC nodes backed by an ACPI device but disabled in this draft\n");
  sb_puts(&body, "==================================================================\n");
  {
    int n = 0;
    for (int i = 0; i < ns->count; i++)
    {
      dt_node_t      *dn = g->acpi_node[i];
      const dm_dev_t *m;
      char            ref[256];
      if (!dn || ns->d[i].sta == 0 || dt_node_enabled(dn))
        continue;
      base_ref(dn, ref, sizeof(ref));
      if (sl_contains(&g->top_refs, ref))
        continue;
      m = dm_for(g, &ns->d[i]);
      sb_printf(&body, "%-28s %-22s %-10s %s%s%s\n", ref, ns->d[i].path, aml_dev_primary_hid(&ns->d[i]),
                m ? m->name : "", m ? " - " : "", m ? devmgr_status_text(m) : "");
      n++;
    }
    if (!n)
      sb_puts(&body, "(none)\n");
  }

  sb_puts(&body, "\n==================================================================\n");
  sb_puts(&body, "APPENDIX B: ACPI devices (DSDT/SSDT) with decoded resources\n");
  sb_puts(&body, "==================================================================\n");
  for (int i = 0; i < ns->count; i++)
  {
    strlist_t lines;
    char      ref[256] = "";
    if (!ns->d[i].hids.count && !ns->d[i].cids.count && !ns->d[i].has_adr)
      continue;
    memset(&lines, 0, sizeof(lines));
    describe_acpi(g, &ns->d[i], &lines, true);
    if (g->acpi_node[i])
      base_ref(g->acpi_node[i], ref, sizeof(ref));
    sb_putc(&body, '\n');
    for (int k = 0; k < lines.count; k++)
    {
      sb_puts(&body, lines.v[k]);
      if (k == 0 && ref[0])
        sb_printf(&body, "  <=> DT %s", ref);
      sb_putc(&body, '\n');
    }
    sl_free(&lines);
  }

  if (g->x->dm && g->x->dm->count)
  {
    sb_puts(&body, "\n==================================================================\n");
    sb_puts(&body, "APPENDIX C: Device Manager - PCI devices and devices with problems\n");
    sb_puts(&body, "==================================================================\n");
    for (int i = 0; i < g->x->dm->count; i++)
    {
      const dm_dev_t *d = &g->x->dm->d[i];
      if (!str_starts_with_ci(d->instance_id, "PCI\\") && 0 == d->problem)
        continue;
      sb_printf(&body, "%-60s %s [%s] %s\n", d->instance_id, d->name, devmgr_status_text(d), d->inf);
    }
  }

  if (g->x->fw && g->x->fw->count)
  {
    sb_puts(&body, "\n==================================================================\n");
    sb_puts(&body, "APPENDIX D: firmware taken from the Windows DriverStore\n");
    sb_puts(&body, "==================================================================\n");
    for (int i = 0; i < g->x->fw->count; i++)
      sb_printf(&body, "/lib/firmware/%s\n    <- %s\n    (%s)\n", g->x->fw->c[i].target_rel, g->x->fw->c[i].source,
                g->x->fw->c[i].reason);
  }

  if (g->notes.count)
  {
    sb_puts(&body, "\n==================================================================\n");
    sb_puts(&body, "APPENDIX E: notes\n");
    sb_puts(&body, "==================================================================\n");
    for (int i = 0; i < g->notes.count; i++)
      sb_printf(&body, "- %s\n", g->notes.v[i]);
  }

  sb_puts(sb, "\n/*");
  {
    const char *p = body.buf;
    while (p && *p)
    {
      const char *e = strchr(p, '\n');
      size_t      n = e ? (size_t)(e - p) : strlen(p);
      char       *line = xstrndup(p, n);
      sb_puts(sb, n ? "\n * " : "\n *");
      sb_put_comment_safe(sb, line);
      free(line);
      p = e ? e + 1 : NULL;
    }
  }
  sb_puts(sb, "\n */\n");
  sb_free(&body);
}

/* ======================================================================== */
/* sections                                                                 */
/* ======================================================================== */

static void emit_soc_deletes(gen_t *g, const strlist_t *already)
{
  bool any = false;
  for (int i = 0; i < g->soc_deletes.count; i++)
  {
    if (sl_contains(already, g->soc_deletes.v[i]))
      continue;
    if (!any)
    {
      sb_printf(&g->out, "\n/* [KERNEL] found in the majority of the %d kernel %s board files */\n", g->soc_boards,
                g->soc);
      any = true;
    }
    sb_printf(&g->out, "/delete-node/ %s;\n", g->soc_deletes.v[i]);
  }
}

static void emit_template(gen_t *g)
{
  strlist_t deletes;
  bool      deletes_done = false;
  int       last_pre = -1;

  memset(&deletes, 0, sizeof(deletes));
  for (int i = 0; i < g->tflat.count; i++)
  {
    const flat_t *e = &g->tflat.v[i];
    if (!e->marker && e->t->kind == TOP_DELETE_NODE && e->t->ref)
      sl_add_unique(&deletes, e->t->ref);
    if (!e->marker && e->t->kind != TOP_NODE)
      last_pre = i;
    if (!e->marker && e->t->kind == TOP_NODE)
      break;
  }

  for (int i = 0; i < g->tflat.count; i++)
  {
    const flat_t   *e = &g->tflat.v[i];
    const dt_top_t *t = e->t;

    if (e->marker == 1)
    {
      sb_printf(&g->out, "\n/* ---- begin of inlined board include \"%s\" (from template) ---- */\n", e->inc_name);
      continue;
    }
    if (e->marker == 2)
    {
      sb_printf(&g->out, "\n/* ---- end of inlined \"%s\" ---- */\n", e->inc_name);
      continue;
    }

    if (t->blank_before || t->kind == TOP_NODE)
      sb_putc(&g->out, '\n');
    if (i == 0)
      emit_template_comment(&g->out, t->comment);
    else
      emit_raw_comment(&g->out, 0, t->comment);

    switch (t->kind)
    {
      case TOP_DIRECTIVE:
        if (strstr(t->text, "/dts-v1/") && i != 0 && e->file != g->tmpl->path)
          break; /* inlined files must not repeat /dts-v1/ */
        sb_printf(&g->out, "%s\n", t->text);
        break;
      case TOP_PREPROC:
        sb_printf(&g->out, "%s\n", t->text);
        break;
      case TOP_INCLUDE:
        if (e->replaced_soc)
        {
          sb_printf(&g->out, "/* [KERNEL] template SoC include \"%s\" replaced for %s */\n", t->include, g->soc);
          sb_printf(&g->out, "#include \"%s\"\n", g->soc_dtsi);
        }
        else
          sb_printf(&g->out, "%s\n", t->text);
        break;
      case TOP_DELETE_NODE:
        if (!dt_tree_ref(&g->base, t->ref))
        {
          sb_printf(&g->out, "/* WARNING: %s is not defined by the SoC/PMIC includes */\n", t->ref);
          g->st->warnings++;
        }
        sb_printf(&g->out, "/delete-node/ %s;\n", t->ref);
        break;
      case TOP_NODE:
      {
        ectx_t ec;
        top_ctx(g, t->node, &ec);
        if (t->node->ref)
          sl_add_unique(&g->top_refs, t->node->ref);
        emit_node(g, &g->out, t->node, 0, &ec);
        break;
      }
    }
    if (i == last_pre && !deletes_done)
    {
      emit_soc_deletes(g, &deletes);
      deletes_done = true;
    }
  }
  if (!deletes_done)
    emit_soc_deletes(g, &deletes);
  if (g->tmpl->trailing_comment)
    emit_raw_comment(&g->out, 0, g->tmpl->trailing_comment);
  sl_free(&deletes);
}

static void emit_secondary_top(gen_t *g)
{
  bool header = false;
  for (int s = 0; s < g->nsec; s++)
  {
    for (int i = 0; i < g->sflat[s].count; i++)
    {
      const flat_t *e = &g->sflat[s].v[i];
      ectx_t        ec;
      strbuf_t      missing, tmp;
      char          origin[512];
      bool          real;

      if (e->marker || e->t->kind != TOP_NODE || !e->t->node->ref || sl_contains(&g->top_refs, e->t->node->ref))
        continue;
      sl_add(&g->top_refs, e->t->node->ref);
      if (!header)
      {
        sb_puts(&g->out, "\n/* ================================================================== */\n");
        sb_puts(&g->out, "/* nodes of the secondary similar DTS files that the template lacks      */\n");
        sb_puts(&g->out, "/* ================================================================== */\n");
        header = true;
      }
      top_ctx(g, e->t->node, &ec);
      for (int k = 0; k < 16; k++)
        ec.sec[k] = NULL;
      sb_init(&missing);
      real = ec.acpi && ec.acpi->sta != 0 && !ec.missing_ref && refs_resolvable(g, e->t->node, &missing);
      if (real && g->x->dm && g->x->dm->count)
      {
        const dm_dev_t *m = dm_for(g, ec.acpi);
        real = m && m->problem == 0;
      }
      if (real && ec.bus_ctrl)
      {
        /* a bus block is only taken over if all addressed children exist in ACPI */
        for (const dt_node_t *c = e->t->node->children; c && real; c = c->next)
        {
          uint64_t reg;
          if (ec.bus_kind != RES_UART && dt_value_first_u64(dt_prop_value(c, "reg"), &reg) &&
              !consumer_at(g, ec.bus_ctrl, ec.bus_kind, (uint32_t)reg, false))
          {
            real = false;
            sb_printf(&missing, " (child %s not present in ACPI)", c->name);
          }
        }
      }
      snprintf(origin, sizeof(origin), "[SIMILAR%d] %s from %s%s%s", s + 2, real ? "taken over" : "suggestion",
               path_basename(g->sec[s]->path), missing.len ? " - not taken over:" : "",
               missing.buf ? missing.buf : "");
      sb_free(&missing);
      ec.origin = origin;
      sb_putc(&g->out, '\n');
      if (real)
      {
        mark_covered_tree(g, e->t->node, &ec);
        emit_node(g, &g->out, e->t->node, 0, &ec);
        g->st->added_similar++;
      }
      else
      {
        sb_init(&tmp);
        emit_node(g, &tmp, e->t->node, 0, &ec);
        comment_out(&g->out, tmp.buf, 0);
        sb_free(&tmp);
        g->st->commented++;
      }
    }
  }
}

static void emit_acpi_buses(gen_t *g)
{
  acpi_ns_t *ns = g->x->ns;
  bool       header = false;

  for (int i = 0; i < ns->count; i++)
  {
    acpi_dev_t     *ctrl = &ns->d[i];
    acpi_res_kind_t kind = (g->src_kind[i] & 1) ? RES_I2C : (g->src_kind[i] & 2) ? RES_SPI
                                                       : (g->src_kind[i] & 4) ? RES_UART : 0;
    bool            pending = false;
    dt_node_t      *dn = g->acpi_node[i];
    char            ref[256];
    strlist_t       lines;
    uint32_t        maxspeed = 0;

    if (!kind)
      continue;
    for (int k = 0; k < ns->count; k++)
    {
      const acpi_res_t *r = dev_bus_res(&ns->d[k], kind, ctrl->path);
      if (r && !g->covered[k])
      {
        pending = true;
        if (r->speed > maxspeed)
          maxspeed = r->speed;
      }
    }
    if (!pending)
      continue;
    if (!header)
    {
      sb_puts(&g->out, "\n/* ================================================================== */\n");
      sb_puts(&g->out, "/* devices found only in ACPI                                           */\n");
      sb_puts(&g->out, "/* ================================================================== */\n");
      header = true;
    }
    memset(&lines, 0, sizeof(lines));
    sl_add(&lines, "[ACPI-DERIVED] bus controller with ACPI devices that no similar DTS describes");
    describe_acpi(g, ctrl, &lines, false);
    sb_putc(&g->out, '\n');
    if (!dn)
    {
      sl_add(&lines, "WARNING: no DT node with this MMIO base in the SoC include - devices listed for reference");
      emit_lines_comment(&g->out, 0, &lines);
      sb_puts(&g->out, "/*\n");
      for (int k = 0; k < ns->count; k++)
        if (!g->covered[k] && dev_bus_res(&ns->d[k], kind, ctrl->path))
        {
          strlist_t dl;
          memset(&dl, 0, sizeof(dl));
          g->covered[k] = true;
          describe_acpi(g, &ns->d[k], &dl, true);
          for (int l = 0; l < dl.count; l++)
          {
            sb_puts(&g->out, " * ");
            sb_put_comment_safe(&g->out, dl.v[l]);
            sb_putc(&g->out, '\n');
          }
          sl_free(&dl);
        }
      sb_puts(&g->out, " */\n");
      sl_free(&lines);
      continue;
    }
    base_ref(dn, ref, sizeof(ref));
    emit_lines_comment(&g->out, 0, &lines);
    sl_free(&lines);
    sb_printf(&g->out, "%s {\n", ref);
    if (kind == RES_I2C && maxspeed)
      sb_printf(&g->out, "\tclock-frequency = <%u>;\n", maxspeed > 1000000 ? 1000000 : maxspeed);
    sb_puts(&g->out, "\tstatus = \"okay\";\n");
    emit_candidates_for(g, &g->out, 1, ctrl, kind);
    sb_puts(&g->out, "};\n");
    sl_add_unique(&g->top_refs, ref);
  }
}

/* without template: skeleton built from SoC include, ACPI and DriverStore */
static void emit_skeleton(gen_t *g)
{
  strbuf_t        *sb = &g->out;
  const sysinfo_t *si = g->x->si;
  strlist_t        none;

  memset(&none, 0, sizeof(none));
  sb_puts(sb, "\n/dts-v1/;\n\n");
  sb_puts(sb, "#include <dt-bindings/gpio/gpio.h>\n");
  sb_puts(sb, "#include <dt-bindings/input/gpio-keys.h>\n");
  sb_puts(sb, "#include <dt-bindings/interrupt-controller/irq.h>\n");
  sb_puts(sb, "#include <dt-bindings/regulator/qcom,rpmh-regulator.h>\n\n");
  sb_printf(sb, "#include \"%s\"\n", g->soc_dtsi);
  if (g->pmics_dtsi[0])
    sb_printf(sb, "#include \"%s\"\n", g->pmics_dtsi);
  emit_soc_deletes(g, &none);

  sb_puts(sb, "\n/ {\n");
  sb_printf(sb, "\t/* [SMBIOS] %s / %s / board %s */\n", si->manufacturer, si->family[0] ? si->family : si->product,
            si->board_product);
  sb_printf(sb, "\tmodel = \"%s\";\n", si->model);
  sb_printf(sb, "\tcompatible = \"%s\", \"qcom,%s\";\n", si->compatible, g->soc);
  sb_puts(sb, "\tchassis-type = \"laptop\";\t/* TODO: verify */\n\n");
  sb_puts(sb, "\t/*\n");
  sb_puts(sb, "\t * TODO: aliases, chosen, fixed regulators, pmic-glink (USB-C / battery),\n");
  sb_puts(sb, "\t * sound, panel / backlight, gpio-keys (lid switch) - none of these are\n");
  sb_puts(sb, "\t * described by ACPI in a form usable for DT; use a similar DTS file.\n");
  sb_puts(sb, "\t */\n");
  sb_puts(sb, "};\n");

  /* firmware of well-known nodes */
  if (g->x->drv)
  {
    for (size_t h = 0; h < sizeof(g_fw_hints) / sizeof(g_fw_hints[0]); h++)
    {
      char      ref[64];
      strbuf_t  val;
      strlist_t lines;
      bool      ok = true;

      snprintf(ref, sizeof(ref), "&%s", g_fw_hints[h].label);
      if (!dt_tree_ref(&g->base, ref))
        continue;
      memset(&lines, 0, sizeof(lines));
      sb_init(&val);
      for (int k = 0; k < 2 && g_fw_hints[h].files[k]; k++)
      {
        char             why[512], rel[256], reason[128];
        const fw_file_t *f = fw_resolve(g->x->drv, g_fw_hints[h].files[k], g->soc, why, sizeof(why));
        if (!f)
        {
          ok = false;
          break;
        }
        snprintf(rel, sizeof(rel), "qcom/%s/%s/%s/%s", g->soc, si->vendor_dir, si->board_slug, f->name);
        snprintf(reason, sizeof(reason), "firmware-name of %s", ref);
        fw_plan_add(g->x->fw, rel, f->path, reason);
        addf(&lines, "[FW] %s", why);
        sb_printf(&val, "%s\"%s\"", k ? ",\n\t\t\t\t" : "", rel);
        g->st->firmware++;
      }
      if (ok)
      {
        sb_putc(sb, '\n');
        emit_lines_comment(sb, 0, &lines);
        sb_printf(sb, "%s {\n\tfirmware-name = %s;\n", ref, val.buf);
        if (strcmp(g_fw_hints[h].label, "gpu_zap_shader"))
          sb_puts(sb, "\tstatus = \"okay\";\n");
        sb_puts(sb, "};\n");
        sl_add_unique(&g->top_refs, ref);
      }
      sl_free(&lines);
      sb_free(&val);
    }
  }

  /* enable SoC nodes backed by active ACPI devices */
  for (int i = 0; i < g->x->ns->count; i++)
  {
    acpi_dev_t     *d = &g->x->ns->d[i];
    dt_node_t      *dn = g->acpi_node[i];
    const dm_dev_t *m = dm_for(g, d);
    char            ref[256];
    strlist_t       lines;

    if (!dn || d->sta == 0 || dt_node_enabled(dn) || (g->src_kind[i] & 7))
      continue;
    if (g->x->dm && g->x->dm->count && (!m || m->problem))
      continue;
    base_ref(dn, ref, sizeof(ref));
    if (sl_contains(&g->top_refs, ref))
      continue;
    memset(&lines, 0, sizeof(lines));
    describe_acpi(g, d, &lines, false);
    sl_add(&lines, "TODO: supplies / PHY / pinctrl properties required by the binding");
    sb_putc(sb, '\n');
    emit_lines_comment(sb, 0, &lines);
    sb_printf(sb, "%s {\n\tstatus = \"okay\";\n};\n", ref);
    sl_add_unique(&g->top_refs, ref);
    g->st->added_acpi++;
    sl_free(&lines);
  }
}

/* ======================================================================== */
/* Wi-Fi board file and other extras                                        */
/* ======================================================================== */

static void plan_extra_firmware(gen_t *g)
{
  const sysinfo_t *si = g->x->si;
  if (!g->x->drv || !g->x->dm)
    return;

  /* WCN685x (ath11k) / WCN785x (ath12k) board data */
  {
    const dm_dev_t *wl = devmgr_find_by_hwid_substr(g->x->dm, "VEN_17CB&DEV_1103");
    const char     *target = "ath11k/WCN6855/hw2.1/board.bin";
    if (!wl)
    {
      wl = devmgr_find_by_hwid_substr(g->x->dm, "VEN_17CB&DEV_1107");
      target = "ath12k/WCN7850/hw2.0/board.bin";
    }
    if (wl)
    {
      const fw_file_t *c[64];
      int              n = fw_find_all(g->x->drv, "bdwlan", "", c, 64);
      const fw_file_t *best = NULL;
      for (int i = 0; i < n; i++)
      {
        if (si->board_product[0] && str_contains_ci(c[i]->name, si->board_product))
        {
          best = c[i];
          break;
        }
      }
      if (best)
      {
        char reason[256];
        snprintf(reason, sizeof(reason), "Wi-Fi board data for %s, renamed (verify against the PCI subsystem ID)",
                 wl->name);
        fw_plan_add(g->x->fw, target, best->path, reason);
        addf(&g->notes, "Wi-Fi board file %s -> /lib/firmware/%s", best->name, target);
      }
      else if (n)
        addf(&g->notes, "Wi-Fi (%s): %d bdwlan* board files in the DriverStore, none matches board \"%s\"", wl->name,
             n, si->board_product);
    }
  }

  /* video codec firmware (iris) */
  {
    const fw_file_t *c[8];
    int              n = fw_find_all(g->x->drv, "qcvss", ".mbn", c, 8);
    for (int i = 0; i < n; i++)
      if (c[i]->present_pkg)
      {
        char rel[256];
        snprintf(rel, sizeof(rel), "qcom/%s/%s/%s/%s", g->soc, si->vendor_dir, si->board_slug, c[i]->name);
        fw_plan_add(g->x->fw, rel, c[i]->path, "video codec (iris) firmware, optional");
        break;
      }
  }
}

/* ======================================================================== */
/* entry                                                                    */
/* ======================================================================== */

int dts_generate(a2d_ctx_t *x, dts_stats_t *stats)
{
  gen_t g;

  memset(&g, 0, sizeof(g));
  memset(stats, 0, sizeof(*stats));
  g.x = x;
  g.st = stats;
  g.soc = x->si->soc[0] ? x->si->soc : "x1e80100";
  if (!x->si->soc[0])
    log_warn("SoC unknown - assuming x1e80100 (use --soc)");

  setup_kernel(&g);

  /* similar DTS files */
  for (int i = 0; i < x->cfg->num_similar; i++)
  {
    dt_file_t *f = dt_parse_file(x->cfg->similar_dts[i]);
    if (!f)
    {
      log_err("cannot read similar DTS %s", x->cfg->similar_dts[i]);
      continue;
    }
    if (f->errors)
      log_warn("%s: %d parse problem(s) (use -v for details)", f->path, f->errors);
    if (!g.tmpl)
    {
      g.tmpl = f;
      flatten(&g, f, &g.tflat, 0);
      template_soc(&g.tflat, g.tmpl_soc, sizeof(g.tmpl_soc));
      log_info("template: %s (SoC %s)", f->path, g.tmpl_soc[0] ? g.tmpl_soc : "unknown");
      if (g.tmpl_soc[0] && _stricmp(g.tmpl_soc, g.soc))
      {
        log_warn("template is for %s, this machine is %s: SoC include will be replaced", g.tmpl_soc, g.soc);
        addf(&g.notes, "template SoC %s differs from this machine (%s): check CPU/GPU/thermal differences", g.tmpl_soc,
             g.soc);
      }
    }
    else if (g.nsec < 16)
    {
      g.sec[g.nsec] = f;
      flatten(&g, f, &g.sflat[g.nsec], 0);
      g.nsec++;
    }
  }

  build_trees(&g);
  match_acpi(&g);
  setup_gpio(&g);
  build_tlmm_users(&g);
  {
    int matched = 0;
    for (int i = 0; i < x->ns->count; i++)
      if (g.acpi_node[i])
        matched++;
    log_info("%d ACPI devices matched to SoC DT nodes by MMIO address", matched);
  }

  sb_init(&g.out);
  emit_header(&g);
  if (g.tmpl)
  {
    /* consumers already described by the template */
    for (int i = 0; i < g.tflat.count; i++)
    {
      const flat_t *e = &g.tflat.v[i];
      ectx_t        ec;
      if (e->marker || e->t->kind != TOP_NODE)
        continue;
      top_ctx(&g, e->t->node, &ec);
      mark_covered_tree(&g, e->t->node, &ec);
    }
    emit_template(&g);
    emit_secondary_top(&g);
  }
  else
    emit_skeleton(&g);
  emit_acpi_buses(&g);
  plan_extra_firmware(&g);
  emit_appendix(&g);

  if (!write_file(x->cfg->output_dts, g.out.buf, g.out.len))
  {
    log_err("cannot write %s", x->cfg->output_dts);
    sb_free(&g.out);
    return -1;
  }

  /* self-check: the draft must parse again */
  {
    dt_file_t *chk = dt_parse_text(x->cfg->output_dts, g.out.buf);
    if (chk->errors)
      log_warn("the draft has %d syntax problem(s) - please report", chk->errors);
    else
      log_ok("draft re-parsed without syntax problems");
  }
  sb_free(&g.out);
  return 0;
}
