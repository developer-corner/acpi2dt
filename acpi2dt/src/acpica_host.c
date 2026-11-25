/* SPDX-License-Identifier: MIT */
/**
 * @file   acpica_host.c
 * @author Ingo A. Kubbilun
 * @brief  embedded ACPICA: AML interpreter and disassembler for the
 *         extracted ACPI tables
 *
 * The ACPICA core (third_party/acpica) is built in the acpiexec
 * configuration (ACPI_EXEC_APP: application mode, interpreter, debugger and
 * disassembler). This module is the host:
 *
 *  - provides the three upcalls of the core into acpiexec as stubs
 *  - provides AcpiOsGetRootPointer(): RSDP/XSDT built around the tables in
 *    memory (the Windows OSL maps "physical" addresses 1:1)
 *  - installs simulated operation region handlers for every address space
 *    before the tables are loaded, so that AML never touches hardware;
 *    every access is counted to tell exact results from results that depend
 *    on firmware data
 *  - evaluates _HID/_CID/_UID/_STA/_CRS and the HID-over-I2C _DSM
 *  - disassembles DSDT/SSDT tables with AcpiDmDisassemble()
 *
 * [MIT license] - see main.c (ACPICA: BSD-3-Clause OR GPL-2.0-only)
 */

#include "acpi.h"
#include "accommon.h"
#include "acparser.h"
#include "acdispat.h"
#include "acdisasm.h"
#include "acnamesp.h"
#include "actables.h"
#include "aecommon.h"
#include "amlresrc.h"

#include "acpica_host.h"

/* ======================================================================== */
/* upcalls of the ACPICA core into acpiexec (ACPI_EXEC_APP)                 */
/* ======================================================================== */

/**
 * @brief acpiexec init-file lookup (upcall of the ACPICA core); not used by acpi2dt.
 *
 * @param Pathname  object path
 * @param ObjDesc   receives the object
 * @return AE_NOT_FOUND
 */
ACPI_STATUS AeLookupInitFileEntry(char *Pathname, ACPI_OPERAND_OBJECT **ObjDesc)
{
  ACPI_UNUSED_VAR(Pathname);
  ACPI_UNUSED_VAR(ObjDesc);
  return (AE_NOT_FOUND);
}

/**
 * @brief acpiexec object overrides (upcall of the ACPICA core); not used by acpi2dt.
 */
void AeDoObjectOverrides(void)
{
}

/**
 * @brief iasl map-file hook called by the GPIO resource disassembler; not used by acpi2dt.
 *
 * @param Op          parse op
 * @param Resource    resource descriptor
 * @param PinCount    number of pins
 * @param PinList     pin list
 * @param DeviceName  device name
 */
void MpSaveGpioInfo(ACPI_PARSE_OBJECT *Op, AML_RESOURCE *Resource, UINT32 PinCount, UINT16 *PinList,
                    char *DeviceName)
{
  ACPI_UNUSED_VAR(Op);
  ACPI_UNUSED_VAR(Resource);
  ACPI_UNUSED_VAR(PinCount);
  ACPI_UNUSED_VAR(PinList);
  ACPI_UNUSED_VAR(DeviceName);
}

/**
 * @brief iasl map-file hook called by the serial bus resource disassembler; not used by acpi2dt.
 *
 * @param Op          parse op
 * @param Resource    resource descriptor
 * @param DeviceName  device name
 */
void MpSaveSerialInfo(ACPI_PARSE_OBJECT *Op, AML_RESOURCE *Resource, char *DeviceName)
{
  ACPI_UNUSED_VAR(Op);
  ACPI_UNUSED_VAR(Resource);
  ACPI_UNUSED_VAR(DeviceName);
}

/**
 * @brief acpiexec table override (upcall of the ACPICA Windows OSL); tables are never replaced.
 *
 * @param ExistingTable  table about to be installed
 * @param NewTable       receives the replacement (always NULL)
 */
void AeTableOverride(ACPI_TABLE_HEADER *ExistingTable, ACPI_TABLE_HEADER **NewTable)
{
  ACPI_UNUSED_VAR(ExistingTable);
  *NewTable = NULL;
}

/* ======================================================================== */
/* state                                                                    */
/* ======================================================================== */

static ACPI_TABLE_RSDP  g_rsdp;          /**< RSDP pointing to g_xsdt            */
static ACPI_TABLE_XSDT *g_xsdt;          /**< XSDT built around the tables       */
static uint8_t         *g_fadt;          /**< FADT copy with DSDT pointers       */
static FILE            *g_log;           /**< ACPICA output                      */
static bool             g_loaded;        /**< namespace loaded                   */
static volatile long    g_touched;       /**< operation region accesses          */

/** simulated storage of one region location */
typedef struct {
  uint8_t  space;  /**< address space ID */
  uint64_t addr;  /**< address */
  uint64_t value;  /**< last written value */
} simcell_t;

static simcell_t *g_cells;               /**< values written by AML              */
static int        g_ncells;              /**< number of entries in g_cells       */
static int        g_capcells;            /**< capacity of g_cells                */

/**
 * @brief Returns the RSDP built around the tables in memory (ACPI_USE_NATIVE_RSDP_POINTER).
 *
 * @return address of the RSDP
 */
ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void)
{
  return ACPI_PTR_TO_PHYSADDR(&g_rsdp);
}

const char *acpica_version(void)
{
  static char v[16];
  snprintf(v, sizeof(v), "%08X", (unsigned)ACPI_CA_VERSION);
  return v;
}

/* ======================================================================== */
/* simulated operation regions                                              */
/* ======================================================================== */

/**
 * @brief Checks whether a region space transfers buffers instead of integers.
 *
 * @param space  address space ID
 * @return true for SMBus, IPMI, GenericSerialBus, PCC, PRM, FFixedHW
 */
static bool buffer_space(UINT8 space)
{
  return space == ACPI_ADR_SPACE_SMBUS || space == ACPI_ADR_SPACE_IPMI || space == ACPI_ADR_SPACE_GSBUS ||
         space == ACPI_ADR_SPACE_PLATFORM_COMM || space == ACPI_ADR_SPACE_PLATFORM_RT ||
         space == ACPI_ADR_SPACE_FIXED_HARDWARE;
}

/**
 * @brief Simulated operation region handler: reads return the last written value or 0; counts accesses.
 *
 * @param Function        ACPI_READ / ACPI_WRITE
 * @param Address         region address
 * @param BitWidth        access width in bits
 * @param Value           value to write / receives the value read
 * @param HandlerContext  address space ID (cast)
 * @param RegionContext   region context
 * @return AE_OK
 */
static ACPI_STATUS region_handler(UINT32 Function, ACPI_PHYSICAL_ADDRESS Address, UINT32 BitWidth, UINT64 *Value,
                                  void *HandlerContext, void *RegionContext)
{
  UINT8    space = (UINT8)(ACPI_SIZE)HandlerContext;
  uint64_t mask = BitWidth >= 64 ? ~0ULL : ((1ULL << BitWidth) - 1);

  ACPI_UNUSED_VAR(RegionContext);
  g_touched++;
  if (buffer_space(space) || !Value)
    return (AE_OK); /* buffer protocols: leave the buffer as it is */

  for (int i = 0; i < g_ncells; i++)
  {
    if (g_cells[i].space == space && g_cells[i].addr == Address)
    {
      if ((Function & ACPI_IO_MASK) == ACPI_READ)
        *Value = g_cells[i].value & mask;
      else
        g_cells[i].value = *Value & mask;
      return (AE_OK);
    }
  }
  if ((Function & ACPI_IO_MASK) == ACPI_READ)
    *Value = 0;
  else
  {
    simcell_t c = { space, Address, *Value & mask };
    VEC_PUSH(g_cells, g_ncells, g_capcells, c);
  }
  return (AE_OK);
}

/**
 * @brief Region setup handler: no per-region context needed.
 *
 * @param RegionHandle    region handle
 * @param Function        ACPI_REGION_ACTIVATE / DEACTIVATE
 * @param HandlerContext  handler context
 * @param RegionContext   receives the region context
 * @return AE_OK
 */
static ACPI_STATUS region_setup(ACPI_HANDLE RegionHandle, UINT32 Function, void *HandlerContext, void **RegionContext)
{
  ACPI_UNUSED_VAR(RegionHandle);
  *RegionContext = (Function == ACPI_REGION_DEACTIVATE) ? NULL : HandlerContext;
  return (AE_OK);
}

/**
 * @brief Installs the simulated handler for all standard and OEM address spaces.
 */
static void install_handlers(void)
{
  static const UINT8 spaces[] = {
    ACPI_ADR_SPACE_SYSTEM_MEMORY, ACPI_ADR_SPACE_SYSTEM_IO, ACPI_ADR_SPACE_PCI_CONFIG, ACPI_ADR_SPACE_EC,
    ACPI_ADR_SPACE_SMBUS, ACPI_ADR_SPACE_CMOS, ACPI_ADR_SPACE_PCI_BAR_TARGET, ACPI_ADR_SPACE_IPMI,
    ACPI_ADR_SPACE_GPIO, ACPI_ADR_SPACE_GSBUS, ACPI_ADR_SPACE_PLATFORM_COMM, ACPI_ADR_SPACE_PLATFORM_RT,
    ACPI_ADR_SPACE_FIXED_HARDWARE,
  };
  for (size_t i = 0; i < sizeof(spaces); i++)
    AcpiInstallAddressSpaceHandler(ACPI_ROOT_OBJECT, spaces[i], region_handler, region_setup,
                                   (void *)(ACPI_SIZE)spaces[i]);
  /* OEM defined spaces (0x80..0xFE) */
  for (unsigned s = 0x80; s < 0xFF; s++)
    AcpiInstallAddressSpaceHandler(ACPI_ROOT_OBJECT, (ACPI_ADR_SPACE_TYPE)s, region_handler, region_setup,
                                   (void *)(ACPI_SIZE)s);
}

/* ======================================================================== */
/* tables                                                                   */
/* ======================================================================== */

/**
 * @brief Computes an ACPI checksum byte (sum of all bytes becomes 0).
 *
 * @param p  data
 * @param n  length
 * @return checksum byte
 */
static UINT8 checksum(const void *p, size_t n)
{
  const UINT8 *b = p;
  UINT8        s = 0;
  while (n--)
    s = (UINT8)(s + *b++);
  return (UINT8)(0 - s);
}

/**
 * @brief Builds RSDP, XSDT and a FADT copy pointing to the in-memory DSDT.
 *
 * @param set  ACPI tables
 * @return false if DSDT or FADT is missing
 */
static bool build_root_tables(const acpi_table_set_t *set)
{
  const acpi_table_t *dsdt = acpi_find_table(set, "DSDT", 0);
  const acpi_table_t *fadt = acpi_find_table(set, "FACP", 0);
  int                 n = 1; /* FADT */
  size_t              xsdt_len;
  ACPI_TABLE_FADT    *f;
  int                 k = 1;

  if (!dsdt || !fadt)
    return false;
  for (int i = 0; i < set->count; i++)
    if (memcmp(set->t[i].sig, "DSDT", 4) && memcmp(set->t[i].sig, "FACP", 4) && memcmp(set->t[i].sig, "FACS", 4) &&
        set->t[i].len >= sizeof(ACPI_TABLE_HEADER))
      n++;

  /* FADT copy pointing to the DSDT (large enough for all FADT fields) */
  g_fadt = xcalloc(1, fadt->len > sizeof(ACPI_TABLE_FADT) ? fadt->len : sizeof(ACPI_TABLE_FADT));
  memcpy(g_fadt, fadt->data, fadt->len);
  f = (ACPI_TABLE_FADT *)g_fadt;
  if (fadt->len >= ACPI_FADT_OFFSET(XDsdt) + 8)
    f->XDsdt = ACPI_PTR_TO_PHYSADDR(dsdt->data);
  if ((ACPI_SIZE)dsdt->data <= 0xFFFFFFFF)
    f->Dsdt = (UINT32)(ACPI_SIZE)dsdt->data;
  else
    f->Dsdt = 0;
  f->Facs = 0;
  if (fadt->len >= ACPI_FADT_OFFSET(XFacs) + 8)
    f->XFacs = 0;
  f->Header.Checksum = 0;
  f->Header.Checksum = checksum(f, fadt->len);

  /* XSDT */
  xsdt_len = sizeof(ACPI_TABLE_HEADER) + (size_t)n * sizeof(UINT64);
  g_xsdt = xcalloc(1, xsdt_len);
  memcpy(g_xsdt->Header.Signature, ACPI_SIG_XSDT, 4);
  g_xsdt->Header.Length = (UINT32)xsdt_len;
  g_xsdt->Header.Revision = 1;
  memcpy(g_xsdt->Header.OemId, dsdt->data + 10, 6);
  memcpy(g_xsdt->Header.OemTableId, dsdt->data + 16, 8);
  g_xsdt->TableOffsetEntry[0] = ACPI_PTR_TO_PHYSADDR(g_fadt);
  for (int i = 0; i < set->count; i++)
    if (memcmp(set->t[i].sig, "DSDT", 4) && memcmp(set->t[i].sig, "FACP", 4) && memcmp(set->t[i].sig, "FACS", 4) &&
        set->t[i].len >= sizeof(ACPI_TABLE_HEADER))
      g_xsdt->TableOffsetEntry[k++] = ACPI_PTR_TO_PHYSADDR(set->t[i].data);
  g_xsdt->Header.Checksum = checksum(g_xsdt, xsdt_len);

  /* RSDP */
  memset(&g_rsdp, 0, sizeof(g_rsdp));
  ACPI_MAKE_RSDP_SIG(g_rsdp.Signature);
  memcpy(g_rsdp.OemId, "A2DT  ", 6);
  g_rsdp.Revision = 2;
  g_rsdp.Length = sizeof(ACPI_TABLE_RSDP);
  g_rsdp.XsdtPhysicalAddress = ACPI_PTR_TO_PHYSADDR(g_xsdt);
  g_rsdp.Checksum = checksum(&g_rsdp, ACPI_RSDP_CHECKSUM_LENGTH);
  g_rsdp.ExtendedChecksum = checksum(&g_rsdp, ACPI_RSDP_XCHECKSUM_LENGTH);
  return true;
}

bool acpica_load(const acpi_table_set_t *set, const char *log_path)
{
  ACPI_STATUS st;

  AcpiDbgLevel = 0;
  AcpiDbgLayer = 0;
  AcpiGbl_MaxLoopIterations = 2; /* seconds per While loop (hardware polling) */

  st = AcpiInitializeSubsystem();
  g_log = fopen(log_path, "w");
  if (g_log)
    AcpiOsRedirectOutput(g_log);
  if (ACPI_FAILURE(st))
    return false;
  if (!build_root_tables(set))
  {
    AcpiOsPrintf("acpi2dt: DSDT or FADT missing\n");
    return false;
  }
  st = AcpiInitializeTables(NULL, 64, TRUE);
  if (ACPI_FAILURE(st))
  {
    AcpiOsPrintf("acpi2dt: AcpiInitializeTables: %s\n", AcpiFormatException(st));
    return false;
  }
  install_handlers();
  st = AcpiLoadTables();
  if (ACPI_FAILURE(st))
  {
    AcpiOsPrintf("acpi2dt: AcpiLoadTables: %s\n", AcpiFormatException(st));
    return false;
  }
  st = AcpiEnableSubsystem(ACPI_NO_ACPI_ENABLE | ACPI_NO_HARDWARE_INIT | ACPI_NO_EVENT_INIT | ACPI_NO_FACS_INIT);
  if (ACPI_FAILURE(st))
    AcpiOsPrintf("acpi2dt: AcpiEnableSubsystem: %s\n", AcpiFormatException(st));
  st = AcpiInitializeObjects(ACPI_NO_DEVICE_INIT);
  if (ACPI_FAILURE(st))
    AcpiOsPrintf("acpi2dt: AcpiInitializeObjects: %s\n", AcpiFormatException(st));
  g_loaded = true;
  return true;
}

void acpica_shutdown(void)
{
  if (g_loaded)
    AcpiTerminate();
  g_loaded = false;
  AcpiOsRedirectOutput(stdout);
  if (g_log)
    fclose(g_log);
  g_log = NULL;
  free(g_cells);
  g_cells = NULL;
  g_ncells = g_capcells = 0;
  free(g_xsdt);
  free(g_fadt);
  g_xsdt = NULL;
  g_fadt = NULL;
}

/* ======================================================================== */
/* evaluation                                                               */
/* ======================================================================== */

/**
 * @brief Decodes an EISA ID integer to its 7-character form.
 *
 * @param v    EISA ID as returned by ACPICA
 * @param out  output buffer (16 bytes)
 */
static void eisa_to_string(UINT64 v, char out[16])
{
  UINT32 s = (UINT32)(((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF));
  snprintf(out, 16, "%c%c%c%04X", (char)(((s >> 26) & 0x1F) + 0x40), (char)(((s >> 21) & 0x1F) + 0x40),
           (char)(((s >> 16) & 0x1F) + 0x40), (unsigned)(s & 0xFFFF));
}

/**
 * @brief Evaluates an object and counts operation region accesses during the call.
 *
 * @param h        device handle
 * @param name     object name, e.g. "_HID"
 * @param args     arguments (may be NULL)
 * @param out      receives the result (free with AcpiOsFree)
 * @param touched  receives the number of region accesses
 * @return ACPICA status
 */
static ACPI_STATUS eval(ACPI_HANDLE h, const char *name, ACPI_OBJECT_LIST *args, ACPI_BUFFER *out, long *touched)
{
  ACPI_STATUS st;
  out->Length = ACPI_ALLOCATE_BUFFER;
  out->Pointer = NULL;
  g_touched = 0;
  st = AcpiEvaluateObject(h, (char *)name, args, out);
  *touched = g_touched;
  return st;
}

/**
 * @brief Converts an ID object (string or EISA integer) to a string.
 *
 * @param o    ACPI object
 * @param out  output buffer
 * @param cap  size of the output buffer
 * @return true for a usable ID
 */
static bool id_string(const ACPI_OBJECT *o, char *out, size_t cap)
{
  if (o->Type == ACPI_TYPE_STRING && o->String.Pointer)
  {
    str_copy(out, cap, o->String.Pointer);
    return true;
  }
  if (o->Type == ACPI_TYPE_INTEGER)
  {
    char id[16];
    eisa_to_string(o->Integer.Value, id);
    str_copy(out, cap, id);
    return true;
  }
  return false;
}

/**
 * @brief Canonicalizes a resource source name relative to the device's parent scope.
 *
 * @param d    device
 * @param src  resource source from ACPICA (may be NULL)
 * @param out  output buffer
 * @param cap  size of the output buffer
 */
static void canon_source(const acpi_dev_t *d, const char *src, char *out, size_t cap)
{
  if (!src || !*src)
  {
    out[0] = 0;
    return;
  }
  if (src[0] == '\\')
    aml_canon_path(src, out, cap);
  else
  {
    char        joined[256];
    const char *dot = strrchr(d->path, '.');
    int         n = dot ? (int)(dot - d->path) : 1;
    snprintf(joined, sizeof(joined), "%.*s.%s", n, d->path, src);
    aml_canon_path(joined, out, cap);
  }
}

/** context of the resource walk */
typedef struct {
  const acpi_dev_t *dev;  /**< device whose resources are converted */
  acpi_res_t       *res;  /**< converted resources */
  int nres;  /**< number of resources */
  int cap;  /**< capacity of res */
} reswalk_t;

/**
 * @brief Appends a converted _CRS resource.
 *
 * @param w  walk context
 * @param r  resource
 */
static void push_res(reswalk_t *w, acpi_res_t *r)
{
  r->origin = RES_ORIGIN_CRS;
  VEC_PUSH(w->res, w->nres, w->cap, *r);
}

/**
 * @brief AcpiWalkResourceBuffer() callback converting ACPICA resources to acpi_res_t.
 *
 * @param rs   ACPICA resource
 * @param ctx  reswalk_t
 * @return AE_OK
 */
static ACPI_STATUS res_cb(ACPI_RESOURCE *rs, void *ctx)
{
  reswalk_t *w = ctx;
  acpi_res_t r;

  memset(&r, 0, sizeof(r));
  switch (rs->Type)
  {
    case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
      r.kind = RES_MEM;
      r.base = rs->Data.FixedMemory32.Address;
      r.length = rs->Data.FixedMemory32.AddressLength;
      push_res(w, &r);
      break;
    case ACPI_RESOURCE_TYPE_MEMORY32:
      r.kind = RES_MEM;
      r.base = rs->Data.Memory32.Minimum;
      r.length = rs->Data.Memory32.AddressLength;
      push_res(w, &r);
      break;
    case ACPI_RESOURCE_TYPE_ADDRESS16:
    case ACPI_RESOURCE_TYPE_ADDRESS32:
    case ACPI_RESOURCE_TYPE_ADDRESS64:
    case ACPI_RESOURCE_TYPE_EXTENDED_ADDRESS64:
    {
      ACPI_RESOURCE_ADDRESS64 a;
      if (ACPI_SUCCESS(AcpiResourceToAddress64(rs, &a)) &&
          (a.ResourceType == ACPI_MEMORY_RANGE || a.ResourceType == ACPI_IO_RANGE))
      {
        r.kind = a.ResourceType == ACPI_MEMORY_RANGE ? RES_MEM : RES_IO;
        r.base = a.Address.Minimum;
        r.length = a.Address.AddressLength;
        push_res(w, &r);
      }
      break;
    }
    case ACPI_RESOURCE_TYPE_IRQ:
      for (int i = 0; i < rs->Data.Irq.InterruptCount; i++)
      {
        acpi_res_t q = r;
        q.kind = RES_IRQ;
        q.irq = rs->Data.Irq.Interrupts[i];
        q.edge = rs->Data.Irq.Triggering == ACPI_EDGE_SENSITIVE;
        q.active_low = rs->Data.Irq.Polarity == ACPI_ACTIVE_LOW;
        q.shared = rs->Data.Irq.Shareable == ACPI_SHARED;
        q.wake = rs->Data.Irq.WakeCapable == ACPI_WAKE_CAPABLE;
        push_res(w, &q);
      }
      break;
    case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
      for (int i = 0; i < rs->Data.ExtendedIrq.InterruptCount; i++)
      {
        acpi_res_t q = r;
        q.kind = RES_IRQ;
        q.irq = rs->Data.ExtendedIrq.Interrupts[i];
        q.edge = rs->Data.ExtendedIrq.Triggering == ACPI_EDGE_SENSITIVE;
        q.active_low = rs->Data.ExtendedIrq.Polarity == ACPI_ACTIVE_LOW;
        q.shared = rs->Data.ExtendedIrq.Shareable == ACPI_SHARED;
        q.wake = rs->Data.ExtendedIrq.WakeCapable == ACPI_WAKE_CAPABLE;
        push_res(w, &q);
      }
      break;
    case ACPI_RESOURCE_TYPE_GPIO:
    {
      const ACPI_RESOURCE_GPIO *g = &rs->Data.Gpio;
      r.kind = g->ConnectionType == ACPI_RESOURCE_GPIO_TYPE_INT ? RES_GPIO_INT : RES_GPIO_IO;
      r.edge = g->Triggering == ACPI_EDGE_SENSITIVE;
      r.active_low = g->Polarity == ACPI_ACTIVE_LOW;
      r.active_both = g->Polarity == ACPI_ACTIVE_BOTH;
      r.shared = g->Shareable == ACPI_SHARED;
      r.wake = g->WakeCapable == ACPI_WAKE_CAPABLE;
      r.pull = g->PinConfig <= 3 ? g->PinConfig : GPIO_PULL_DEFAULT;
      r.debounce = g->DebounceTimeout;
      r.io_restrict = g->IoRestriction;
      for (int i = 0; i < g->PinTableLength && r.npins < 16; i++)
        r.pins[r.npins++] = g->PinTable[i];
      canon_source(w->dev, g->ResourceSource.StringPtr, r.source, sizeof(r.source));
      push_res(w, &r);
      break;
    }
    case ACPI_RESOURCE_TYPE_SERIAL_BUS:
    {
      const ACPI_RESOURCE_COMMON_SERIALBUS *c = &rs->Data.CommonSerialBus;
      canon_source(w->dev, c->ResourceSource.StringPtr, r.source, sizeof(r.source));
      switch (c->Type)
      {
        case ACPI_RESOURCE_SERIAL_TYPE_I2C:
          r.kind = RES_I2C;
          r.addr = rs->Data.I2cSerialBus.SlaveAddress;
          r.speed = rs->Data.I2cSerialBus.ConnectionSpeed;
          r.addr10 = rs->Data.I2cSerialBus.AccessMode == ACPI_I2C_10BIT_MODE;
          break;
        case ACPI_RESOURCE_SERIAL_TYPE_SPI:
          r.kind = RES_SPI;
          r.addr = rs->Data.SpiSerialBus.DeviceSelection;
          r.speed = rs->Data.SpiSerialBus.ConnectionSpeed;
          r.spi_bits = rs->Data.SpiSerialBus.DataBitLength;
          r.spi_mode = (uint8_t)((rs->Data.SpiSerialBus.ClockPolarity << 1) | rs->Data.SpiSerialBus.ClockPhase);
          r.cs_active_high = rs->Data.SpiSerialBus.DevicePolarity == ACPI_SPI_ACTIVE_HIGH;
          break;
        case ACPI_RESOURCE_SERIAL_TYPE_UART:
          r.kind = RES_UART;
          r.speed = rs->Data.UartSerialBus.DefaultBaudRate;
          break;
        default:
          r.kind = RES_CSI2;
          break;
      }
      push_res(w, &r);
      break;
    }
    default:
      break;
  }
  return (AE_OK);
}

/**
 * @brief Replaces the _CRS resources of a device, keeping _AEI / field connections.
 *
 * @param d  device
 * @param w  evaluated resources
 */
static void replace_crs(acpi_dev_t *d, reswalk_t *w)
{
  acpi_res_t *nr = NULL;
  int         n = 0, cap = 0;
  for (int i = 0; i < d->nres; i++)
    if (d->res[i].origin != RES_ORIGIN_CRS)
      VEC_PUSH(nr, n, cap, d->res[i]);
  for (int i = 0; i < w->nres; i++)
    VEC_PUSH(nr, n, cap, w->res[i]);
  free(d->res);
  d->res = nr;
  d->nres = n;
  d->rescap = cap;
  d->crs_alternatives = w->nres ? 1 : 0;
}

/** _DSM UUID of HID-over-I2C (3cdff6f7-4267-4555-ad05-b30a3d8938de) in AML byte order */
static const UINT8 g_hid_i2c_uuid[16] = { 0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45,
                                         0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE };

/**
 * @brief Evaluates _HID, _CID, _UID, _STA, _CRS and the HID-over-I2C _DSM of a device.
 *
 * @param d   device (updated)
 * @param st  statistics (updated)
 */
static void eval_device(acpi_dev_t *d, acpica_stats_t *st)
{
  ACPI_HANDLE h;
  ACPI_BUFFER b;
  long        touched;

  if (ACPI_FAILURE(AcpiGetHandle(NULL, d->path, &h)))
    return;
  st->devices++;

  /* _HID */
  if (ACPI_SUCCESS(eval(h, "_HID", NULL, &b, &touched)) && b.Pointer)
  {
    char id[32];
    if (id_string(b.Pointer, id, sizeof(id)))
    {
      str_copy(d->hid_eval, sizeof(d->hid_eval), id);
      if (touched)
      {
        d->eval_hid = A2D_EVAL_FIRMWARE;
        st->firmware++;
      }
      else
      {
        /* exact: put the value first */
        strlist_t nl;
        memset(&nl, 0, sizeof(nl));
        sl_add(&nl, id);
        for (int i = 0; i < d->hids.count; i++)
          sl_add_unique(&nl, d->hids.v[i]);
        sl_free(&d->hids);
        d->hids = nl;
        d->eval_hid = A2D_EVAL_EXACT;
        st->exact++;
        st->hid_updated++;
      }
    }
    AcpiOsFree(b.Pointer);
  }

  /* _CID */
  if (ACPI_SUCCESS(eval(h, "_CID", NULL, &b, &touched)) && b.Pointer)
  {
    const ACPI_OBJECT *o = b.Pointer;
    char               id[32];
    if (!touched)
    {
      if (o->Type == ACPI_TYPE_PACKAGE)
      {
        for (UINT32 i = 0; i < o->Package.Count; i++)
          if (id_string(&o->Package.Elements[i], id, sizeof(id)))
            sl_add_unique(&d->cids, id);
      }
      else if (id_string(o, id, sizeof(id)))
        sl_add_unique(&d->cids, id);
    }
    AcpiOsFree(b.Pointer);
  }

  /* _UID */
  if (!d->has_uid && ACPI_SUCCESS(eval(h, "_UID", NULL, &b, &touched)) && b.Pointer)
  {
    const ACPI_OBJECT *o = b.Pointer;
    if (!touched && o->Type == ACPI_TYPE_INTEGER)
    {
      snprintf(d->uid, sizeof(d->uid), "%llu", (unsigned long long)o->Integer.Value);
      d->has_uid = true;
    }
    else if (!touched && o->Type == ACPI_TYPE_STRING)
    {
      str_copy(d->uid, sizeof(d->uid), o->String.Pointer);
      d->has_uid = true;
    }
    AcpiOsFree(b.Pointer);
  }

  /* _STA */
  if (ACPI_SUCCESS(eval(h, "_STA", NULL, &b, &touched)) && b.Pointer)
  {
    const ACPI_OBJECT *o = b.Pointer;
    if (o->Type == ACPI_TYPE_INTEGER)
    {
      if (touched)
      {
        d->eval_sta = A2D_EVAL_FIRMWARE;
        st->firmware++;
      }
      else
      {
        d->sta = (int)o->Integer.Value;
        d->eval_sta = A2D_EVAL_EXACT;
        st->exact++;
      }
    }
    AcpiOsFree(b.Pointer);
  }

  /* _CRS */
  {
    reswalk_t w;
    memset(&w, 0, sizeof(w));
    w.dev = d;
    b.Length = ACPI_ALLOCATE_BUFFER;
    b.Pointer = NULL;
    g_touched = 0;
    if (ACPI_SUCCESS(AcpiGetCurrentResources(h, &b)) && b.Pointer)
    {
      touched = g_touched;
      AcpiWalkResourceBuffer(&b, res_cb, &w);
      if (!touched)
      {
        replace_crs(d, &w);
        d->eval_crs = A2D_EVAL_EXACT;
        st->exact++;
        st->crs_updated++;
      }
      else
      {
        bool static_crs = false;
        for (int i = 0; i < d->nres; i++)
          if (d->res[i].origin == RES_ORIGIN_CRS)
            static_crs = true;
        if (!static_crs)
          replace_crs(d, &w); /* better than nothing */
        d->eval_crs = A2D_EVAL_FIRMWARE;
        st->firmware++;
      }
      AcpiOsFree(b.Pointer);
    }
    else if (d->crs_is_method)
    {
      d->eval_crs = A2D_EVAL_FAILED;
      st->failed++;
    }
    free(w.res);
  }

  /* HID-over-I2C descriptor address: _DSM (uuid, 1, 1, {}) */
  if (aml_dev_has_id(d, "PNP0C50") || aml_dev_has_id(d, "ACPI0C50"))
  {
    ACPI_OBJECT      a[4];
    ACPI_OBJECT_LIST l;
    a[0].Type = ACPI_TYPE_BUFFER;
    a[0].Buffer.Length = 16;
    a[0].Buffer.Pointer = (UINT8 *)g_hid_i2c_uuid;
    a[1].Type = ACPI_TYPE_INTEGER;
    a[1].Integer.Value = 1;
    a[2].Type = ACPI_TYPE_INTEGER;
    a[2].Integer.Value = 1;
    a[3].Type = ACPI_TYPE_PACKAGE;
    a[3].Package.Count = 0;
    a[3].Package.Elements = NULL;
    l.Count = 4;
    l.Pointer = a;
    if (ACPI_SUCCESS(eval(h, "_DSM", &l, &b, &touched)) && b.Pointer)
    {
      const ACPI_OBJECT *o = b.Pointer;
      if (o->Type == ACPI_TYPE_INTEGER && !touched)
        d->hid_descr_addr = (int)o->Integer.Value;
      AcpiOsFree(b.Pointer);
    }
  }
}

void acpica_evaluate(acpi_ns_t *ns, acpica_stats_t *st)
{
  memset(st, 0, sizeof(*st));
  if (!g_loaded)
    return;
  for (int i = 0; i < ns->count; i++)
    eval_device(&ns->d[i], st);
}

/* ======================================================================== */
/* disassembly                                                              */
/* ======================================================================== */

/**
 * @brief Disassembles a DSDT/SSDT with AcpiDmDisassemble() into a .dsl file.
 *
 * @param t     table
 * @param path  output path
 * @return true if the table was parsed completely
 */
static bool disassemble_table(const acpi_table_t *t, const char *path)
{
  const ACPI_TABLE_HEADER *hdr = (const ACPI_TABLE_HEADER *)t->data;
  UINT8                   *aml = t->data + sizeof(ACPI_TABLE_HEADER);
  UINT32                   len = t->len - (UINT32)sizeof(ACPI_TABLE_HEADER);
  ACPI_PARSE_OBJECT       *root;
  ACPI_WALK_STATE         *walk;
  ACPI_STATUS              st;
  FILE                    *f = fopen(path, "w");

  if (!f)
    return false;
  AcpiOsRedirectOutput(f);
  AcpiOsPrintf("/*\n * %4.4s disassembled in-process by acpi2dt with the ACPICA %08X disassembler\n",
               hdr->Signature, (unsigned)ACPI_CA_VERSION);
  AcpiOsPrintf(" * (for reading - External() declarations of \"iasl -d\" are not generated)\n */\n");
  AcpiOsPrintf("DefinitionBlock (\"\", \"%4.4s\", %u, \"%.6s\", \"%.8s\", 0x%08X)\n{\n", hdr->Signature,
               hdr->Revision, hdr->OemId, hdr->OemTableId, hdr->OemRevision);

  AcpiUtSetIntegerWidth(hdr->Revision);
  AcpiGbl_DmOpt_Verbose = FALSE; /* no "(Path ...)" debugger annotations */
  AcpiOsPrintf("/* acpi2dt: messages of the ACPICA parse pass (if any):\n");
  root = AcpiPsCreateScopeOp(aml);
  walk = root ? AcpiDsCreateWalkState(0, root, NULL, NULL) : NULL;
  st = walk ? AcpiDsInitAmlWalk(walk, root, NULL, aml, len, NULL, ACPI_IMODE_LOAD_PASS1) : AE_NO_MEMORY;
  if (ACPI_SUCCESS(st))
  {
    walk->ParseFlags &= ~ACPI_PARSE_DELETE_TREE;
    /* the namespace is already loaded: reuse its nodes (also gives every
       method call its argument count, incl. forward references) */
    walk->NamespaceOverride = TRUE;
    st = AcpiPsParseAml(walk);
    /* method bodies, packages and buffers are deferred in the load pass */
    if (ACPI_SUCCESS(st))
      st = AcpiDmParseDeferredOps(root);
  }
  AcpiOsPrintf("*/\n");
  if (root)
  {
    AcpiDmDisassemble(NULL, root, ACPI_UINT32_MAX);
    AcpiPsDeleteParseTree(root);
  }
  AcpiOsPrintf("}\n");
  if (ACPI_FAILURE(st))
    AcpiOsPrintf("/* acpi2dt: parse ended with %s - output may be incomplete */\n", AcpiFormatException(st));
  AcpiOsRedirectOutput(g_log ? g_log : stdout);
  fclose(f);
  return ACPI_SUCCESS(st);
}

int acpica_disassemble(const acpi_table_set_t *set, const char *out_dir)
{
  int n = 0;
  if (!g_loaded)
    return 0;
  for (int i = 0; i < set->count; i++)
  {
    char name[64], path[A2D_MAX_PATH], *dot;
    if (memcmp(set->t[i].sig, "DSDT", 4) && memcmp(set->t[i].sig, "SSDT", 4))
      continue;
    str_copy(name, sizeof(name), set->t[i].file_name);
    dot = strrchr(name, '.');
    if (dot)
      strcpy(dot, ".dsl");
    path_join(path, sizeof(path), out_dir, name);
    if (disassemble_table(&set->t[i], path))
      n++;
  }
  return n;
}
