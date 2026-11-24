/**
 * @file   aml_parser.c
 * @author Ingo A. Kubbilun
 * @brief  static AML namespace walker (no AML interpreter needed)
 *
 * AML is executable byte code; a complete evaluation needs an interpreter
 * (ACPICA). For drafting a device tree, the static structure is sufficient:
 *
 *   - Scope / Device / ThermalZone / PowerResource / Processor objects are
 *     walked recursively (If/Else/While bodies included), using PkgLength
 *     boundaries so that a parse problem stays local to one object.
 *   - Name() objects of devices (_HID, _CID, _UID, _ADR, _STR, _DDN, _STA,
 *     _CRS, _DEP) are evaluated as constant data objects.
 *   - Method() objects of devices are evaluated heuristically:
 *       _HID/_CID  : all HID-like string returns become candidates
 *       _CRS       : every Buffer that is a valid resource template
 *       _STA       : "Return (constant)" bodies
 *       _DSM       : HID-over-I2C descriptor address
 *   - method argument counts are learned in a first pass so that method
 *     invocations inside predicates can be skipped correctly.
 *
 * [MIT license] - see main.c
 */

#include "aml_parser.h"

#define MAX_DEPTH 200

typedef struct {
  bool root;
  int  up;
  int  nseg;
  char seg[32][5];
} aml_name_t;

typedef struct {
  const uint8_t *base;
  uint32_t       len;
  acpi_ns_t     *ns;
  strmap_t      *methods;     /* last NameSeg -> (argc + 1) */
  const char    *table;
  int            pass;
  int            depth;
} pctx_t;

/* ======================================================================== */
/* helpers                                                                  */
/* ======================================================================== */

static bool is_lead_char(uint8_t c) { return (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_name_char(uint8_t c) { return is_lead_char(c) || (c >= '0' && c <= '9'); }

static bool parse_pkglen(const uint8_t **pp, const uint8_t *end, const uint8_t **pkg_end)
{
  const uint8_t *p = *pp;
  uint32_t       len;
  int            n;

  if (p >= end)
    return false;
  n = p[0] >> 6;
  if (p + 1 + n > end)
    return false;
  if (0 == n)
    len = p[0] & 0x3F;
  else
  {
    len = p[0] & 0x0F;
    for (int i = 0; i < n; i++)
      len |= (uint32_t)p[1 + i] << (4 + 8 * i);
  }
  if (p + len > end || len < (uint32_t)(1 + n))
    return false;
  *pkg_end = p + len;
  *pp = p + 1 + n;
  return true;
}

static bool parse_nameseg(const uint8_t **pp, const uint8_t *end, char out[5])
{
  const uint8_t *p = *pp;
  if (p + 4 > end || !is_lead_char(p[0]))
    return false;
  for (int i = 0; i < 4; i++)
  {
    if (!is_name_char(p[i]))
      return false;
    out[i] = (char)p[i];
  }
  out[4] = 0;
  *pp = p + 4;
  return true;
}

static bool is_namestring_start(uint8_t c)
{
  return c == 0x5C || c == 0x5E || c == 0x2E || c == 0x2F || is_lead_char(c);
}

static bool parse_namestring(const uint8_t **pp, const uint8_t *end, aml_name_t *n)
{
  const uint8_t *p = *pp;
  int            cnt = 1;

  memset(n, 0, sizeof(*n));
  if (p < end && *p == 0x5C)
  {
    n->root = true;
    p++;
  }
  else
  {
    while (p < end && *p == 0x5E)
    {
      n->up++;
      p++;
    }
  }
  if (p >= end)
    return false;
  if (*p == 0x00)
  {
    *pp = p + 1;
    return true; /* NullName */
  }
  if (*p == 0x2E)
  {
    cnt = 2;
    p++;
  }
  else if (*p == 0x2F)
  {
    if (p + 2 > end)
      return false;
    cnt = p[1];
    p += 2;
    if (cnt > 32)
      return false;
  }
  for (int i = 0; i < cnt; i++)
    if (!parse_nameseg(&p, end, n->seg[n->nseg++]))
      return false;
  *pp = p;
  return true;
}

static void seg_strip(const char *seg, char *out)
{
  int n = 4;
  memcpy(out, seg, 4);
  while (n > 1 && out[n - 1] == '_')
    n--;
  out[n] = 0;
}

/* scope is a canonical path ("\" or "\_SB.I2C1") */
static void resolve_name(const char *scope, const aml_name_t *n, char *out, size_t cap)
{
  char segs[64][5];
  int  ns = 0;

  if (!n->root)
  {
    const char *s = scope;
    if (*s == '\\')
      s++;
    while (*s && ns < 64)
    {
      int k = 0;
      while (*s && *s != '.' && k < 4)
        segs[ns][k++] = *s++;
      segs[ns][k] = 0;
      ns++;
      if (*s == '.')
        s++;
    }
    ns -= n->up;
    if (ns < 0)
      ns = 0;
  }
  for (int i = 0; i < n->nseg && ns < 64; i++)
    seg_strip(n->seg[i], segs[ns++]);

  {
    strbuf_t sb;
    sb_init(&sb);
    sb_putc(&sb, '\\');
    for (int i = 0; i < ns; i++)
    {
      if (i)
        sb_putc(&sb, '.');
      sb_puts(&sb, segs[i]);
    }
    str_copy(out, cap, sb.buf);
    sb_free(&sb);
  }
}

void aml_canon_path(const char *in, char *out, size_t cap)
{
  strbuf_t sb;
  sb_init(&sb);
  while (*in == ' ')
    in++;
  if (*in == '\\')
  {
    sb_putc(&sb, '\\');
    in++;
  }
  while (*in)
  {
    char seg[8];
    int  k = 0;
    while (*in && *in != '.' && k < 7)
    {
      char c = *in++;
      if (c >= 'a' && c <= 'z')
        c = (char)(c - 32);
      seg[k++] = c;
    }
    while (*in && *in != '.')
      in++;
    while (k > 1 && seg[k - 1] == '_')
      k--;
    seg[k] = 0;
    sb_puts(&sb, seg);
    if (*in == '.')
    {
      sb_putc(&sb, '.');
      in++;
    }
  }
  str_copy(out, cap, sb.buf ? sb.buf : "");
  sb_free(&sb);
}

static void eisa_decode(uint32_t v, char out[16])
{
  uint32_t s = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
  snprintf(out, 16, "%c%c%c%04X", (char)(((s >> 26) & 0x1F) + 0x40), (char)(((s >> 21) & 0x1F) + 0x40),
           (char)(((s >> 16) & 0x1F) + 0x40), (unsigned)(s & 0xFFFF));
}

static bool looks_like_hid(const char *s)
{
  size_t n = strlen(s);
  if (n < 7 || n > 9)
    return false;
  for (size_t i = 0; i < n; i++)
    if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
      return false;
  /* at least one digit */
  for (size_t i = 0; i < n; i++)
    if (s[i] >= '0' && s[i] <= '9')
      return true;
  return false;
}

/* ======================================================================== */
/* term skipping                                                            */
/* ======================================================================== */

static const char *g_op1[256];
static const char *g_op2[256];
static bool        g_ops_init = false;

static void init_ops(void)
{
  static const struct { int op; const char *a; } t1[] = {
    {0x00,""},{0x01,""},{0xFF,""},{0x06,"NN"},{0x08,"NT"},{0x0A,"B"},{0x0B,"W"},{0x0C,"D"},{0x0E,"Q"},
    {0x15,"NBB"},
    {0x60,""},{0x61,""},{0x62,""},{0x63,""},{0x64,""},{0x65,""},{0x66,""},{0x67,""},
    {0x68,""},{0x69,""},{0x6A,""},{0x6B,""},{0x6C,""},{0x6D,""},{0x6E,""},
    {0x70,"TS"},{0x71,"S"},{0x72,"TTS"},{0x73,"TTS"},{0x74,"TTS"},{0x75,"S"},{0x76,"S"},{0x77,"TTS"},
    {0x78,"TTSS"},{0x79,"TTS"},{0x7A,"TTS"},{0x7B,"TTS"},{0x7C,"TTS"},{0x7D,"TTS"},{0x7E,"TTS"},
    {0x7F,"TTS"},{0x80,"TS"},{0x81,"TS"},{0x82,"TS"},{0x83,"T"},{0x84,"TTS"},{0x85,"TTS"},{0x86,"ST"},
    {0x87,"S"},{0x88,"TTS"},{0x89,"TBTBTT"},{0x8A,"TTN"},{0x8B,"TTN"},{0x8C,"TTN"},{0x8D,"TTN"},
    {0x8E,"S"},{0x8F,"TTN"},{0x90,"TT"},{0x91,"TT"},{0x92,"T"},{0x93,"TT"},{0x94,"TT"},{0x95,"TT"},
    {0x96,"TS"},{0x97,"TS"},{0x98,"TS"},{0x99,"TS"},{0x9C,"TTS"},{0x9D,"TS"},{0x9E,"TTTS"},{0x9F,""},
    {0xA3,""},{0xA4,"T"},{0xA5,""},{0xCC,""},
  };
  static const struct { int op; const char *a; } t2[] = {
    {0x01,"NB"},{0x02,"N"},{0x12,"SS"},{0x13,"TTTN"},{0x1F,"TTTTTT"},{0x20,"NS"},{0x21,"T"},{0x22,"T"},
    {0x23,"SW"},{0x24,"S"},{0x25,"ST"},{0x26,"S"},{0x27,"S"},{0x28,"TS"},{0x29,"TS"},{0x2A,"S"},
    {0x30,""},{0x31,""},{0x32,"BDT"},{0x33,""},{0x80,"NBTT"},{0x88,"NTTT"},
  };
  for (size_t i = 0; i < sizeof(t1) / sizeof(t1[0]); i++)
    g_op1[t1[i].op] = t1[i].a;
  for (size_t i = 0; i < sizeof(t2) / sizeof(t2[0]); i++)
    g_op2[t2[i].op] = t2[i].a;
  g_ops_init = true;
}

static bool skip_term(pctx_t *c, const uint8_t **pp, const uint8_t *end);

static bool skip_supername(pctx_t *c, const uint8_t **pp, const uint8_t *end)
{
  const uint8_t *p = *pp;
  aml_name_t     n;

  if (p >= end)
    return false;
  if (*p == 0x00)
  {
    *pp = p + 1;
    return true;
  }
  if (is_namestring_start(*p))
  {
    if (!parse_namestring(&p, end, &n))
      return false;
    *pp = p;
    return true;
  }
  return skip_term(c, pp, end);
}

static bool skip_args(pctx_t *c, const char *args, const uint8_t **pp, const uint8_t *end)
{
  const uint8_t *p = *pp;
  aml_name_t     n;

  for (; *args; args++)
  {
    switch (*args)
    {
      case 'T': if (!skip_term(c, &p, end)) return false; break;
      case 'S': if (!skip_supername(c, &p, end)) return false; break;
      case 'N': if (!parse_namestring(&p, end, &n)) return false; break;
      case 'B': p += 1; break;
      case 'W': p += 2; break;
      case 'D': p += 4; break;
      case 'Q': p += 8; break;
      default: return false;
    }
    if (p > end)
      return false;
  }
  *pp = p;
  return true;
}

static bool skip_term(pctx_t *c, const uint8_t **pp, const uint8_t *end)
{
  const uint8_t *p = *pp;
  const uint8_t *pend;
  uint8_t        op;
  bool           ok = false;

  if (p >= end || c->depth > MAX_DEPTH)
    return false;
  c->depth++;
  op = *p;

  if (op == 0x0D) /* string */
  {
    p++;
    while (p < end && *p)
      p++;
    if (p < end)
    {
      p++;
      ok = true;
    }
  }
  else if (op == 0x10 || op == 0x11 || op == 0x12 || op == 0x13 || op == 0x14 || op == 0xA0 || op == 0xA1 ||
           op == 0xA2)
  {
    p++;
    if (parse_pkglen(&p, end, &pend))
    {
      p = pend;
      ok = true;
    }
  }
  else if (op == 0x5B)
  {
    uint8_t op2 = (p + 1 < end) ? p[1] : 0xFF;
    if (op2 == 0x81 || op2 == 0x82 || op2 == 0x83 || op2 == 0x84 || op2 == 0x85 || op2 == 0x86 || op2 == 0x87)
    {
      p += 2;
      if (parse_pkglen(&p, end, &pend))
      {
        p = pend;
        ok = true;
      }
    }
    else if (g_op2[op2])
    {
      p += 2;
      ok = skip_args(c, g_op2[op2], &p, end);
    }
  }
  else if (is_namestring_start(op))
  {
    aml_name_t n;
    if (parse_namestring(&p, end, &n))
    {
      ok = true;
      if (n.nseg > 0)
      {
        intptr_t argc1 = (intptr_t)sm_get(c->methods, n.seg[n.nseg - 1]);
        for (intptr_t i = 1; ok && i < argc1; i++)
          ok = skip_term(c, &p, end);
      }
    }
  }
  else if (g_op1[op])
  {
    p++;
    ok = skip_args(c, g_op1[op], &p, end);
  }

  c->depth--;
  if (ok && p <= end)
  {
    *pp = p;
    return true;
  }
  return false;
}

/* ======================================================================== */
/* constant data objects                                                    */
/* ======================================================================== */

typedef enum { V_NONE, V_INT, V_STR, V_BUF, V_PKG } vtype_t;

typedef struct {
  vtype_t        type;
  uint64_t       i;
  char           s[256];
  const uint8_t *buf;
  uint32_t       blen;
  const uint8_t *pkg;      /* first element */
  const uint8_t *pkg_end;
  int            pkg_count;
} aml_val_t;

static bool eval_const(pctx_t *c, const uint8_t **pp, const uint8_t *end, aml_val_t *v)
{
  const uint8_t *p = *pp;
  const uint8_t *pend;

  memset(v, 0, sizeof(*v));
  if (p >= end)
    return false;
  switch (*p)
  {
    case 0x00: v->type = V_INT; v->i = 0; *pp = p + 1; return true;
    case 0x01: v->type = V_INT; v->i = 1; *pp = p + 1; return true;
    case 0xFF: v->type = V_INT; v->i = ~0ULL; *pp = p + 1; return true;
    case 0x0A: if (p + 2 > end) return false; v->type = V_INT; v->i = p[1]; *pp = p + 2; return true;
    case 0x0B: if (p + 3 > end) return false; v->type = V_INT; v->i = *(const uint16_t *)(p + 1); *pp = p + 3; return true;
    case 0x0C: if (p + 5 > end) return false; v->type = V_INT; v->i = *(const uint32_t *)(p + 1); *pp = p + 5; return true;
    case 0x0E: if (p + 9 > end) return false; v->type = V_INT; v->i = *(const uint64_t *)(p + 1); *pp = p + 9; return true;
    case 0x0D:
    {
      size_t n = 0;
      p++;
      while (p < end && *p)
      {
        if (n < sizeof(v->s) - 1)
          v->s[n++] = (char)*p;
        p++;
      }
      if (p >= end)
        return false;
      v->s[n] = 0;
      v->type = V_STR;
      *pp = p + 1;
      return true;
    }
    case 0x11:
    {
      aml_val_t sz;
      p++;
      if (!parse_pkglen(&p, end, &pend))
        return false;
      if (!eval_const(c, &p, pend, &sz) || sz.type != V_INT)
      {
        *pp = pend;
        return true; /* dynamic size: keep V_NONE */
      }
      v->type = V_BUF;
      v->buf = p;
      v->blen = (uint32_t)(pend - p);
      *pp = pend;
      return true;
    }
    case 0x12:
    case 0x13:
    {
      uint8_t op = *p++;
      if (!parse_pkglen(&p, end, &pend))
        return false;
      if (op == 0x12)
      {
        if (p >= pend)
          return false;
        v->pkg_count = *p++;
      }
      else
      {
        aml_val_t n;
        if (!eval_const(c, &p, pend, &n))
        {
          *pp = pend;
          return true;
        }
        v->pkg_count = (int)n.i;
      }
      v->type = V_PKG;
      v->pkg = p;
      v->pkg_end = pend;
      *pp = pend;
      return true;
    }
    default:
      if (skip_term(c, &p, end))
      {
        *pp = p;
        return true;
      }
      return false;
  }
}

/* ======================================================================== */
/* resource templates                                                       */
/* ======================================================================== */

static void res_source(const uint8_t *desc, uint32_t dlen, uint32_t off, char *out, size_t cap)
{
  size_t n = 0;
  out[0] = 0;
  if (off >= dlen)
    return;
  while (off + n < dlen && desc[off + n] && n < cap - 1)
  {
    out[n] = (char)desc[off + n];
    n++;
  }
  out[n] = 0;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

/* decodes a resource template; returns number of resources or -1 if invalid */
static int decode_template(const uint8_t *b, uint32_t len, acpi_res_t **out, int *nout, int *cap, int alt)
{
  uint32_t off = 0;
  int      found = 0;
  bool     end_seen = false;
  int      start_n = *nout;

  while (off < len)
  {
    uint8_t     tag = b[off];
    uint32_t    dlen, hdr;
    const uint8_t *d;
    acpi_res_t  r;

    memset(&r, 0, sizeof(r));
    r.alt = alt;
    if (tag & 0x80)
    {
      if (off + 3 > len)
        goto bad;
      dlen = rd16(b + off + 1);
      hdr = 3;
    }
    else
    {
      dlen = tag & 7;
      hdr = 1;
    }
    if (off + hdr + dlen > len)
      goto bad;
    d = b + off; /* descriptor start incl. tag */

    if (!(tag & 0x80))
    {
      uint8_t type = (tag >> 3) & 0x0F;
      switch (type)
      {
        case 0x0F: /* end tag */
          end_seen = true;
          break;
        case 0x04: /* IRQ */
          if (dlen >= 2)
          {
            uint16_t mask = rd16(d + 1);
            for (int i = 0; i < 16; i++)
              if (mask & (1u << i))
              {
                acpi_res_t ri = r;
                ri.kind = RES_IRQ;
                ri.irq = (uint32_t)i;
                if (dlen >= 3)
                {
                  ri.edge = d[3] & 1;
                  ri.active_low = (d[3] >> 3) & 1;
                }
                VEC_PUSH(*out, *nout, *cap, ri);
                found++;
              }
          }
          break;
        case 0x08: /* IO */
          if (dlen >= 7)
          {
            r.kind = RES_IO;
            r.base = rd16(d + 2);
            r.length = d[7];
            VEC_PUSH(*out, *nout, *cap, r);
            found++;
          }
          break;
        case 0x09: /* fixed IO */
          if (dlen >= 3)
          {
            r.kind = RES_IO;
            r.base = rd16(d + 1) & 0x3FF;
            r.length = d[3];
            VEC_PUSH(*out, *nout, *cap, r);
            found++;
          }
          break;
        case 0x05: case 0x06: case 0x07: case 0x0A: case 0x0E:
          break; /* DMA, start/end dependent, fixed DMA, vendor */
        default:
          goto bad;
      }
      off += hdr + dlen;
      if (end_seen)
        break;
      continue;
    }

    switch (tag & 0x7F)
    {
      case 0x01: /* Memory24 */
        if (dlen >= 9)
        {
          r.kind = RES_MEM;
          r.base = (uint64_t)rd16(d + 4) << 8;
          r.length = (uint64_t)rd16(d + 10) << 8;
          VEC_PUSH(*out, *nout, *cap, r);
          found++;
        }
        break;
      case 0x05: /* Memory32 */
        if (dlen >= 17)
        {
          r.kind = RES_MEM;
          r.base = rd32(d + 4);
          r.length = rd32(d + 16);
          VEC_PUSH(*out, *nout, *cap, r);
          found++;
        }
        break;
      case 0x06: /* Memory32Fixed */
        if (dlen >= 9)
        {
          r.kind = RES_MEM;
          r.base = rd32(d + 4);
          r.length = rd32(d + 8);
          VEC_PUSH(*out, *nout, *cap, r);
          found++;
        }
        break;
      case 0x07: /* DWordAddress */
      case 0x08: /* WordAddress */
      case 0x0A: /* QWordAddress */
      case 0x0B: /* ExtendedAddress */
      {
        uint8_t rtype = d[3];
        int     w = ((tag & 0x7F) == 0x07) ? 4 : ((tag & 0x7F) == 0x08) ? 2 : 8;
        uint32_t base_off = ((tag & 0x7F) == 0x0B) ? 8 : 6;
        uint32_t need = base_off + (uint32_t)w * 5;
        if (dlen + hdr >= need)
        {
          const uint8_t *q = d + base_off;
          uint64_t       mn, ln;
          mn = (w == 2) ? rd16(q + 2) : (w == 4) ? rd32(q + 4) : rd64(q + 8);
          ln = (w == 2) ? rd16(q + 8) : (w == 4) ? rd32(q + 16) : rd64(q + 32);
          if (rtype == 0 || rtype == 1)
          {
            r.kind = rtype == 0 ? RES_MEM : RES_IO;
            r.base = mn;
            r.length = ln;
            VEC_PUSH(*out, *nout, *cap, r);
            found++;
          }
        }
        break;
      }
      case 0x09: /* Extended Interrupt */
        if (dlen >= 2)
        {
          uint8_t flags = d[3];
          uint8_t cnt = d[4];
          for (uint8_t i = 0; i < cnt && 5u + 4u * (i + 1u) <= dlen + hdr; i++)
          {
            acpi_res_t ri = r;
            ri.kind = RES_IRQ;
            ri.irq = rd32(d + 5 + 4 * i);
            ri.edge = (flags >> 1) & 1;
            ri.active_low = (flags >> 2) & 1;
            ri.shared = (flags >> 3) & 1;
            ri.wake = (flags >> 4) & 1;
            VEC_PUSH(*out, *nout, *cap, ri);
            found++;
          }
        }
        break;
      case 0x0C: /* GPIO connection */
        if (dlen >= 20)
        {
          uint8_t  ctype = d[4];
          uint16_t flags = rd16(d + 7);
          uint16_t pin_off = rd16(d + 14);
          uint16_t src_off = rd16(d + 17);
          uint32_t total = hdr + dlen;

          r.kind = ctype == 0 ? RES_GPIO_INT : RES_GPIO_IO;
          if (ctype == 0)
          {
            r.edge = flags & 1;
            r.active_low = ((flags >> 1) & 3) == 1;
            r.active_both = ((flags >> 1) & 3) == 2;
            r.shared = (flags >> 3) & 1;
            r.wake = (flags >> 4) & 1;
          }
          else
          {
            r.io_restrict = flags & 3;
            r.shared = (flags >> 3) & 1;
          }
          r.pull = d[9] <= 3 ? d[9] : GPIO_PULL_DEFAULT;
          r.debounce = rd16(d + 12);
          for (uint32_t o = pin_off; o + 2 <= src_off && o + 2 <= total && r.npins < 16; o += 2)
            r.pins[r.npins++] = rd16(d + o);
          res_source(d, total, src_off, r.source, sizeof(r.source));
          VEC_PUSH(*out, *nout, *cap, r);
          found++;
        }
        break;
      case 0x0E: /* generic serial bus */
        if (dlen >= 9)
        {
          uint8_t  btype = d[5];
          uint16_t tflags = rd16(d + 7);
          uint16_t tlen = rd16(d + 10);
          uint32_t total = hdr + dlen;
          const uint8_t *td = d + 12;

          if (12u + tlen > total)
            goto bad;
          switch (btype)
          {
            case 1:
              r.kind = RES_I2C;
              r.addr10 = tflags & 1;
              if (tlen >= 6)
              {
                r.speed = rd32(td);
                r.addr = rd16(td + 4);
              }
              break;
            case 2:
              r.kind = RES_SPI;
              r.cs_active_high = (tflags >> 1) & 1;
              if (tlen >= 9)
              {
                r.speed = rd32(td);
                r.spi_bits = td[4];
                r.spi_mode = (uint8_t)(((td[6] & 1) << 1) | (td[5] & 1));
                r.addr = rd16(td + 7);
              }
              break;
            case 3:
              r.kind = RES_UART;
              if (tlen >= 4)
                r.speed = rd32(td);
              break;
            default:
              r.kind = RES_CSI2;
              break;
          }
          res_source(d, total, 12u + tlen, r.source, sizeof(r.source));
          VEC_PUSH(*out, *nout, *cap, r);
          found++;
        }
        break;
      case 0x0D: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
        r.kind = RES_PIN; /* pin function / config / group: not decoded */
        break;
      case 0x02: case 0x04: case 0x15:
        break; /* generic register, vendor, ... */
      default:
        goto bad;
    }
    off += hdr + dlen;
  }
  if (!end_seen)
    goto bad;
  return found;

bad:
  *nout = start_n; /* roll back */
  return -1;
}

static bool templates_equal(const acpi_dev_t *d, int start_a, int na, int start_b, int nb)
{
  if (na != nb)
    return false;
  for (int i = 0; i < na; i++)
  {
    acpi_res_t a = d->res[start_a + i], b = d->res[start_b + i];
    a.alt = b.alt = 0;
    if (memcmp(&a, &b, sizeof(a)))
      return false;
  }
  return true;
}

static void add_template(acpi_dev_t *d, const uint8_t *buf, uint32_t len)
{
  int start = d->nres;
  int n = decode_template(buf, len, &d->res, &d->nres, &d->rescap, d->crs_alternatives);
  if (n < 0)
    return;
  /* dedupe against previous alternatives */
  {
    int s = 0;
    for (int alt = 0; alt < d->crs_alternatives; alt++)
    {
      int cnt = 0;
      while (s + cnt < start && d->res[s + cnt].alt == alt)
        cnt++;
      if (templates_equal(d, s, cnt, start, n))
      {
        d->nres = start;
        return;
      }
      s += cnt;
    }
  }
  d->crs_alternatives++;
}

/* GPIO references outside _CRS (_AEI, field connections) */
static void add_extra_template(acpi_dev_t *d, const uint8_t *buf, uint32_t len, int origin)
{
  int start = d->nres;
  if (decode_template(buf, len, &d->res, &d->nres, &d->rescap, 0) < 0)
    return;
  for (int i = start; i < d->nres; i++)
  {
    bool dup = false;
    d->res[i].origin = origin;
    d->res[i].alt = 0;
    for (int k = 0; k < start && !dup; k++)
      dup = 0 == memcmp(&d->res[k], &d->res[i], sizeof(acpi_res_t));
    if (dup || (d->res[i].kind != RES_GPIO_INT && d->res[i].kind != RES_GPIO_IO))
    {
      memmove(&d->res[i], &d->res[i + 1], (size_t)(d->nres - i - 1) * sizeof(acpi_res_t));
      d->nres--;
      i--;
    }
  }
}

static void scan_buffers(pctx_t *c, acpi_dev_t *d, const uint8_t *body, const uint8_t *end, int origin)
{
  for (const uint8_t *p = body; p < end; p++)
  {
    const uint8_t *q = p + 1, *pend;
    aml_val_t      sz;
    if (*p != 0x11 || !parse_pkglen(&q, end, &pend))
      continue;
    if (!eval_const(c, &q, pend, &sz) || sz.type != V_INT)
      continue;
    if (pend - q >= 2 && pend[-2] == 0x79)
    {
      int before = d->nres;
      add_extra_template(d, q, (uint32_t)(pend - q), origin);
      if (d->nres != before)
        p = pend - 1;
    }
  }
}

/* ======================================================================== */
/* device objects                                                           */
/* ======================================================================== */

static void add_id(strlist_t *sl, const aml_val_t *v)
{
  if (v->type == V_STR)
    sl_add_unique(sl, v->s);
  else if (v->type == V_INT)
  {
    char id[16];
    eisa_decode((uint32_t)v->i, id);
    sl_add_unique(sl, id);
  }
}

static void decode_unicode(const aml_val_t *v, char *out, size_t cap)
{
  size_t n = 0;
  if (v->type == V_STR)
  {
    str_copy(out, cap, v->s);
    return;
  }
  if (v->type != V_BUF)
    return;
  for (uint32_t i = 0; i + 1 < v->blen && n < cap - 1; i += 2)
  {
    uint16_t ch = rd16(v->buf + i);
    if (!ch)
      break;
    out[n++] = (ch < 0x80) ? (char)ch : '?';
  }
  out[n] = 0;
}

static void handle_name(pctx_t *c, acpi_dev_t *d, const char *seg, const uint8_t **pp, const uint8_t *end)
{
  aml_val_t v;
  if (!eval_const(c, pp, end, &v))
  {
    c->ns->parse_errors++;
    return;
  }

  if (!strcmp(seg, "_HID"))
    add_id(&d->hids, &v);
  else if (!strcmp(seg, "_CID"))
  {
    if (v.type == V_PKG)
    {
      const uint8_t *p = v.pkg;
      for (int i = 0; i < v.pkg_count && p < v.pkg_end; i++)
      {
        aml_val_t e;
        if (!eval_const(c, &p, v.pkg_end, &e))
          break;
        add_id(&d->cids, &e);
      }
    }
    else
      add_id(&d->cids, &v);
  }
  else if (!strcmp(seg, "_UID"))
  {
    d->has_uid = true;
    if (v.type == V_INT)
      snprintf(d->uid, sizeof(d->uid), "%llu", (unsigned long long)v.i);
    else if (v.type == V_STR)
      str_copy(d->uid, sizeof(d->uid), v.s);
  }
  else if (!strcmp(seg, "_ADR") && v.type == V_INT)
  {
    d->has_adr = true;
    d->adr = v.i;
  }
  else if (!strcmp(seg, "_STR") || (!strcmp(seg, "_DDN") && !d->str[0]))
    decode_unicode(&v, d->str, sizeof(d->str));
  else if (!strcmp(seg, "_STA") && v.type == V_INT)
    d->sta = (int)v.i;
  else if (!strcmp(seg, "_CRS") && v.type == V_BUF)
    add_template(d, v.buf, v.blen);
  else if (!strcmp(seg, "_DSD"))
    d->has_dsd = true;
  else if (!strcmp(seg, "_DEP") && v.type == V_PKG)
  {
    const uint8_t *p = v.pkg;
    while (p < v.pkg_end)
    {
      aml_name_t n;
      if (is_namestring_start(*p) && parse_namestring(&p, v.pkg_end, &n))
      {
        char path[192];
        resolve_name(d->path, &n, path, sizeof(path));
        sl_add_unique(&d->deps, path);
      }
      else if (!skip_term(c, &p, v.pkg_end))
        break;
    }
  }
}

static const uint8_t g_hid_i2c_uuid[16] = { 0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45,
                                           0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE };

static void scan_strings(const uint8_t *p, const uint8_t *end, strlist_t *sl)
{
  for (; p < end; p++)
  {
    if (*p == 0x0D)
    {
      char           s[16];
      size_t         n = 0;
      const uint8_t *q = p + 1;
      while (q < end && *q && n < sizeof(s) - 1)
        s[n++] = (char)*q++;
      s[n] = 0;
      if (q < end && *q == 0 && looks_like_hid(s))
        sl_add_unique(sl, s);
    }
    else if (*p == 0xA4 && p + 5 < end && p[1] == 0x0C)
    {
      char     id[16];
      uint32_t v = rd32(p + 2);
      eisa_decode(v, id);
      if (looks_like_hid(id) && id[0] >= 'A' && id[1] >= 'A' && id[2] >= 'A')
        sl_add_unique(sl, id);
    }
  }
}

static bool return_const(pctx_t *c, const uint8_t *p, const uint8_t *end, uint64_t *out)
{
  aml_val_t v;
  if (p >= end || *p != 0xA4)
    return false;
  p++;
  if (!eval_const(c, &p, end, &v) || v.type != V_INT)
    return false;
  *out = v.i;
  return p == end;
}

static void handle_method(pctx_t *c, acpi_dev_t *d, const char *seg, const uint8_t *body, const uint8_t *end)
{
  if (!strcmp(seg, "_HID"))
  {
    d->hid_is_method = true;
    scan_strings(body, end, &d->hids);
  }
  else if (!strcmp(seg, "_CID"))
    scan_strings(body, end, &d->cids);
  else if (!strcmp(seg, "_CRS"))
  {
    d->crs_is_method = true;
    for (const uint8_t *p = body; p < end; p++)
    {
      if (*p == 0x11)
      {
        const uint8_t *q = p + 1, *pend;
        aml_val_t      sz;
        if (!parse_pkglen(&q, end, &pend))
          continue;
        if (!eval_const(c, &q, pend, &sz) || sz.type != V_INT)
          continue;
        if (pend - q >= 2 && pend[-2] == 0x79)
        {
          int before = d->nres;
          add_template(d, q, (uint32_t)(pend - q));
          if (d->nres != before)
            p = pend - 1;
        }
      }
    }
  }
  else if (!strcmp(seg, "_AEI"))
    scan_buffers(c, d, body, end, RES_ORIGIN_AEI);
  else if (!strcmp(seg, "_ROM"))
  {
    d->rom_body = body;
    d->rom_len = (uint32_t)(end - body);
  }
  else if (!strcmp(seg, "_STA"))
  {
    uint64_t v;
    d->sta = return_const(c, body, end, &v) ? (int)v : -1;
  }
  else if (!strcmp(seg, "_UID") || !strcmp(seg, "_ADR"))
  {
    uint64_t v;
    if (return_const(c, body, end, &v))
    {
      if (seg[1] == 'U')
      {
        d->has_uid = true;
        snprintf(d->uid, sizeof(d->uid), "%llu", (unsigned long long)v);
      }
      else
      {
        d->has_adr = true;
        d->adr = v;
      }
    }
  }
  else if (!strcmp(seg, "_DSM"))
  {
    for (const uint8_t *p = body; p + 16 <= end; p++)
    {
      if (0 == memcmp(p, g_hid_i2c_uuid, 16))
      {
        for (const uint8_t *q = p + 16; q + 1 < end; q++)
        {
          if (*q == 0xA4)
          {
            const uint8_t *r = q + 1;
            aml_val_t      v;
            if (*r == 0x11 || *r == 0x12)
              continue;
            if (eval_const(c, &r, end, &v) && v.type == V_INT && v.i < 0x10000)
            {
              d->hid_descr_addr = (int)v.i;
              break;
            }
          }
        }
        break;
      }
    }
  }
  else if (!strcmp(seg, "_DSD"))
    d->has_dsd = true;
}

static int create_device(pctx_t *c, const char *path, int parent)
{
  acpi_dev_t d;
  acpi_dev_t *existing = aml_find_device(c->ns, path);

  if (existing)
    return (int)(existing - c->ns->d);
  memset(&d, 0, sizeof(d));
  str_copy(d.path, sizeof(d.path), path);
  str_copy(d.table, sizeof(d.table), c->table);
  d.sta = 0x0F; /* default if no _STA */
  d.hid_descr_addr = -1;
  d.parent = parent;
  VEC_PUSH(c->ns->d, c->ns->count, c->ns->cap, d);
  return c->ns->count - 1;
}

/* ======================================================================== */
/* namespace walk                                                           */
/* ======================================================================== */

static void walk(pctx_t *c, const uint8_t *p, const uint8_t *end, const char *scope, int dev)
{
  while (p < end)
  {
    uint8_t        op = *p;
    const uint8_t *pend;
    aml_name_t     n;
    char           path[192];

    if (op == 0x10) /* Scope */
    {
      p++;
      if (!parse_pkglen(&p, end, &pend) || !parse_namestring(&p, pend, &n))
        goto error;
      resolve_name(scope, &n, path, sizeof(path));
      {
        acpi_dev_t *d = aml_find_device(c->ns, path);
        walk(c, p, pend, path, d ? (int)(d - c->ns->d) : -1);
      }
      p = pend;
      continue;
    }
    if (op == 0x5B && p + 1 < end &&
        (p[1] == 0x82 || p[1] == 0x83 || p[1] == 0x84 || p[1] == 0x85))
    {
      uint8_t op2 = p[1];
      int     ndev = -1;
      p += 2;
      if (!parse_pkglen(&p, end, &pend) || !parse_namestring(&p, pend, &n))
        goto error;
      resolve_name(scope, &n, path, sizeof(path));
      if (op2 == 0x84)
        p += 3;
      else if (op2 == 0x83)
        p += 6;
      if (op2 == 0x82 && c->pass == 2)
        ndev = create_device(c, path, dev);
      if (p <= pend)
        walk(c, p, pend, path, ndev);
      p = pend;
      continue;
    }
    if (op == 0x14) /* Method */
    {
      uint8_t flags;
      p++;
      if (!parse_pkglen(&p, end, &pend) || !parse_namestring(&p, pend, &n) || p >= pend)
        goto error;
      flags = *p++;
      if (n.nseg > 0 && !sm_get(c->methods, n.seg[n.nseg - 1]))
        sm_put(c->methods, n.seg[n.nseg - 1], (void *)(intptr_t)((flags & 7) + 1));
      if (c->pass == 2 && dev >= 0 && !n.root && n.up == 0 && n.nseg == 1)
        handle_method(c, &c->ns->d[dev], n.seg[0], p, pend);
      p = pend;
      continue;
    }
    if (op == 0x08) /* Name */
    {
      p++;
      if (!parse_namestring(&p, end, &n))
        goto error;
      if (c->pass == 2 && dev >= 0 && !n.root && n.up == 0 && n.nseg == 1)
      {
        const uint8_t *before = p;
        handle_name(c, &c->ns->d[dev], n.seg[0], &p, end);
        if (p == before && !skip_term(c, &p, end))
          goto error;
      }
      else if (!skip_term(c, &p, end))
        goto error;
      continue;
    }
    if (op == 0x5B && p + 1 < end && (p[1] == 0x81 || p[1] == 0x86 || p[1] == 0x87)) /* Field variants */
    {
      const uint8_t *body;
      p += 2;
      if (!parse_pkglen(&p, end, &pend))
        goto error;
      body = p;
      if (c->pass == 2 && dev >= 0)
        scan_buffers(c, &c->ns->d[dev], body, pend, RES_ORIGIN_FIELD);
      p = pend;
      continue;
    }
    if (op == 0xA0 || op == 0xA2) /* If / While */
    {
      p++;
      if (!parse_pkglen(&p, end, &pend))
        goto error;
      if (skip_term(c, &p, pend))
        walk(c, p, pend, scope, dev);
      p = pend;
      continue;
    }
    if (op == 0xA1) /* Else */
    {
      p++;
      if (!parse_pkglen(&p, end, &pend))
        goto error;
      walk(c, p, pend, scope, dev);
      p = pend;
      continue;
    }
    if (!skip_term(c, &p, end))
      goto error;
  }
  return;

error:
  if (c->pass == 2)
  {
    c->ns->parse_errors++;
    log_verbose("AML parse problem in %s at offset 0x%x (scope %s) - skipping rest of block", c->table,
                (unsigned)(p - c->base), scope);
  }
}

static void resolve_sources(acpi_ns_t *ns)
{
  for (int i = 0; i < ns->count; i++)
  {
    acpi_dev_t *d = &ns->d[i];
    for (int r = 0; r < d->nres; r++)
    {
      char tmp[192];
      if (!d->res[r].source[0])
        continue;
      if (d->res[r].source[0] == '\\')
        aml_canon_path(d->res[r].source, tmp, sizeof(tmp));
      else
      {
        /* relative: resolve against the parent scope of the device */
        char scope[192], joined[256];
        const char *dot = strrchr(d->path, '.');
        size_t      n = dot ? (size_t)(dot - d->path) : 1;
        memcpy(scope, d->path, n);
        scope[n] = 0;
        snprintf(joined, sizeof(joined), "%s.%s", scope, d->res[r].source);
        aml_canon_path(joined, tmp, sizeof(tmp));
      }
      str_copy(d->res[r].source, sizeof(d->res[r].source), tmp);
    }
  }
}

void aml_parse_all(const acpi_table_set_t *set, acpi_ns_t *ns)
{
  strmap_t methods;
  memset(&methods, 0, sizeof(methods));
  memset(ns, 0, sizeof(*ns));
  if (!g_ops_init)
    init_ops();

  for (int pass = 1; pass <= 2; pass++)
  {
    for (int i = 0; i < set->count; i++)
    {
      const acpi_table_t *t = &set->t[i];
      pctx_t              c;
      if (memcmp(t->sig, "DSDT", 4) && memcmp(t->sig, "SSDT", 4))
        continue;
      if (t->len <= 36)
        continue;
      memset(&c, 0, sizeof(c));
      c.base = t->data;
      c.len = t->len;
      c.ns = ns;
      c.methods = &methods;
      c.table = t->file_name;
      c.pass = pass;
      walk(&c, t->data + 36, t->data + t->len, "\\", -1);
    }
  }
  resolve_sources(ns);
  sm_free(&methods);
}

void aml_free(acpi_ns_t *ns)
{
  for (int i = 0; i < ns->count; i++)
  {
    sl_free(&ns->d[i].hids);
    sl_free(&ns->d[i].cids);
    sl_free(&ns->d[i].deps);
    free(ns->d[i].res);
  }
  free(ns->d);
  memset(ns, 0, sizeof(*ns));
}

/* ======================================================================== */
/* Qualcomm GPU _ROM panel configuration                                    */
/* ======================================================================== */

/* "Store (<seg>, Local2)" = 0x70 NameSeg 0x62 */
static bool store_to_local2(const uint8_t *p, const uint8_t *end, char seg[5])
{
  const uint8_t *q = p + 1;
  if (p + 6 > end || p[0] != 0x70 || !parse_nameseg(&q, end, seg))
    return false;
  return q < end && *q == 0x62;
}

bool aml_rom_panel_xml(const acpi_dev_t *d, uint32_t panel_id, char bufname[5], const char **xml, uint32_t *xml_len,
                       bool *is_default)
{
  const uint8_t *body = d->rom_body, *end;
  bool           found = false;
  pctx_t         c;

  if (!body || d->rom_len < 16)
    return false;
  end = body + d->rom_len;
  memset(&c, 0, sizeof(c));
  if (!g_ops_init)
    init_ops();
  *is_default = false;

  /* 1. the panel ID compared as a constant, followed by Store (PCxx, Local2) */
  for (const uint8_t *p = body; p + 5 <= end && !found; p++)
  {
    bool hit = (p[0] == 0x0C && rd32(p + 1) == panel_id) ||
               (panel_id <= 0xFFFF && p[0] == 0x0B && rd16(p + 1) == panel_id);
    if (!hit)
      continue;
    for (const uint8_t *q = p + 3; q < p + 32 && q < end; q++)
      if (store_to_local2(q, end, bufname))
      {
        found = true;
        break;
      }
  }
  /* 2. default: the last Store (xxxx, Local2) in the method (Else branch) */
  if (!found)
  {
    for (const uint8_t *p = body; p < end; p++)
    {
      char seg[5];
      if (store_to_local2(p, end, seg))
      {
        memcpy(bufname, seg, 5);
        found = true;
      }
    }
    if (!found)
      return false;
    *is_default = true;
  }

  /* 3. Name (<bufname>, Buffer (n) {...}) inside the method */
  for (const uint8_t *p = body; p + 6 < end; p++)
  {
    const uint8_t *q, *pend;
    char           seg[5];
    aml_val_t      sz;
    if (p[0] != 0x08)
      continue;
    q = p + 1;
    if (!parse_nameseg(&q, end, seg) || strcmp(seg, bufname) || q >= end || *q != 0x11)
      continue;
    q++;
    if (!parse_pkglen(&q, end, &pend) || !eval_const(&c, &q, pend, &sz))
      return false;
    *xml = (const char *)q;
    *xml_len = (uint32_t)(pend - q);
    while (*xml_len && (*xml)[*xml_len - 1] == 0)
      (*xml_len)--;
    return true;
  }
  return false;
}

/* ======================================================================== */
/* queries                                                                  */
/* ======================================================================== */

acpi_dev_t *aml_find_device(const acpi_ns_t *ns, const char *path)
{
  for (int i = 0; i < ns->count; i++)
    if (0 == _stricmp(ns->d[i].path, path))
      return &ns->d[i];
  return NULL;
}

acpi_dev_t *aml_find_by_mmio(const acpi_ns_t *ns, uint64_t addr, bool exact_only)
{
  acpi_dev_t *contain = NULL;
  for (int i = 0; i < ns->count; i++)
  {
    acpi_dev_t *d = &ns->d[i];
    for (int r = 0; r < d->nres; r++)
    {
      if (d->res[r].kind != RES_MEM || !d->res[r].length)
        continue;
      if (d->res[r].base == addr && d->hids.count)
        return d;
      if (!contain && !exact_only && d->hids.count && addr > d->res[r].base &&
          addr < d->res[r].base + d->res[r].length)
        contain = d;
    }
  }
  return contain;
}

bool aml_dev_has_id(const acpi_dev_t *d, const char *id)
{
  return sl_contains(&d->hids, id) || sl_contains(&d->cids, id);
}

const char *aml_dev_primary_hid(const acpi_dev_t *d)
{
  return d->hids.count ? d->hids.v[0] : (d->cids.count ? d->cids.v[0] : "");
}

const char *aml_res_kind_name(acpi_res_kind_t k)
{
  switch (k)
  {
    case RES_MEM:      return "Memory";
    case RES_IO:       return "IO";
    case RES_IRQ:      return "Interrupt";
    case RES_GPIO_INT: return "GpioInt";
    case RES_GPIO_IO:  return "GpioIo";
    case RES_I2C:      return "I2cSerialBus";
    case RES_SPI:      return "SpiSerialBus";
    case RES_UART:     return "UartSerialBus";
    case RES_CSI2:     return "CameraSerialBus";
    case RES_PIN:      return "Pin*";
    default:           return "?";
  }
}

static const char *pull_name(int p)
{
  switch (p)
  {
    case GPIO_PULL_UP:   return "PullUp";
    case GPIO_PULL_DOWN: return "PullDown";
    case GPIO_PULL_NONE: return "PullNone";
    default:             return "PullDefault";
  }
}

void aml_res_describe(const acpi_res_t *r, strbuf_t *sb)
{
  switch (r->kind)
  {
    case RES_MEM:
    case RES_IO:
      sb_printf(sb, "%s 0x%llx+0x%llx", aml_res_kind_name(r->kind), (unsigned long long)r->base,
                (unsigned long long)r->length);
      break;
    case RES_IRQ:
      sb_printf(sb, "Interrupt GSIV %u (%s, %s%s%s)", r->irq, r->edge ? "Edge" : "Level",
                r->active_low ? "ActiveLow" : "ActiveHigh", r->shared ? ", Shared" : "", r->wake ? ", Wake" : "");
      break;
    case RES_GPIO_INT:
    case RES_GPIO_IO:
      sb_printf(sb, "%s %s pin", aml_res_kind_name(r->kind), r->source);
      for (int i = 0; i < r->npins; i++)
        sb_printf(sb, "%s%u", i ? "," : " ", r->pins[i]);
      if (r->kind == RES_GPIO_INT)
        sb_printf(sb, " (%s, %s, %s%s)", r->edge ? "Edge" : "Level",
                  r->active_both ? "ActiveBoth" : (r->active_low ? "ActiveLow" : "ActiveHigh"), pull_name(r->pull),
                  r->wake ? ", Wake" : "");
      else
        sb_printf(sb, " (%s, %s)", pull_name(r->pull),
                  r->io_restrict == 1 ? "Input" : r->io_restrict == 2 ? "Output" : "InOut");
      break;
    case RES_I2C:
      sb_printf(sb, "I2C %s addr 0x%02x @ %u Hz%s", r->source, r->addr, r->speed, r->addr10 ? " (10-bit)" : "");
      break;
    case RES_SPI:
      sb_printf(sb, "SPI %s cs %u @ %u Hz mode %u", r->source, r->addr, r->speed, r->spi_mode);
      break;
    case RES_UART:
      sb_printf(sb, "UART %s @ %u baud", r->source, r->speed);
      break;
    case RES_CSI2:
      sb_printf(sb, "CSI2 %s", r->source);
      break;
    default:
      sb_puts(sb, aml_res_kind_name(r->kind));
      break;
  }
  if (r->alt)
    sb_printf(sb, " [alt %d]", r->alt);
  if (r->origin == RES_ORIGIN_AEI)
    sb_puts(sb, " [_AEI event]");
  else if (r->origin == RES_ORIGIN_FIELD)
    sb_puts(sb, " [OpRegion field Connection]");
}
