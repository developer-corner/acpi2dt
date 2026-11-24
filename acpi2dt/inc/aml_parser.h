/**
 * @file   aml_parser.h
 * @author Ingo A. Kubbilun
 * @brief  static AML namespace walker: extracts Device() objects with their
 *         identification objects and decoded _CRS resource templates
 *
 * [MIT license] - see main.c
 */

#ifndef A2D_AML_PARSER_H
#define A2D_AML_PARSER_H

#include "common.h"
#include "acpi_extractor.h"

typedef enum {
  RES_MEM = 1,       /* Memory32Fixed, Memory32, Q/D/WordAddress (memory) */
  RES_IO,            /* IO, FixedIO, address descriptors (I/O)            */
  RES_IRQ,           /* Interrupt / IRQ (GSIV)                            */
  RES_GPIO_INT,      /* GpioInt                                           */
  RES_GPIO_IO,       /* GpioIo                                            */
  RES_I2C,           /* I2cSerialBusV2                                    */
  RES_SPI,           /* SpiSerialBusV2                                    */
  RES_UART,          /* UartSerialBusV2                                   */
  RES_CSI2,          /* Camera serial bus                                 */
  RES_PIN,           /* PinFunction / PinConfig / PinGroup                */
} acpi_res_kind_t;

#define GPIO_PULL_DEFAULT  0
#define GPIO_PULL_UP       1
#define GPIO_PULL_DOWN     2
#define GPIO_PULL_NONE     3

typedef struct {
  acpi_res_kind_t kind;
  uint64_t        base;          /* MEM / IO                                   */
  uint64_t        length;
  uint32_t        irq;           /* IRQ: GSIV                                  */
  bool            edge;          /* IRQ / GPIO_INT                             */
  bool            active_low;    /* IRQ / GPIO_INT (GPIO_INT: polarity==1)     */
  bool            active_both;   /* GPIO_INT polarity==2                       */
  bool            shared;
  bool            wake;
  uint16_t        pins[16];      /* GPIO                                       */
  int             npins;
  int             pull;          /* GPIO_PULL_*                                */
  uint16_t        debounce;      /* GPIO (1/100 ms)                            */
  int             io_restrict;   /* GpioIo: 0 none, 1 input, 2 output, 3 preserve */
  uint32_t        addr;          /* I2C slave address / SPI chip select        */
  uint32_t        speed;         /* I2C/SPI Hz, UART baud                      */
  bool            addr10;        /* I2C 10-bit                                 */
  uint8_t         spi_mode;      /* SPI: (CPOL<<1)|CPHA                        */
  uint8_t         spi_bits;
  bool            cs_active_high;
  char            source[64];    /* resource source (controller path)          */
  int             alt;           /* index of the resource template (method alternatives) */
  int             origin;        /* RES_ORIGIN_*                               */
} acpi_res_t;

#define RES_ORIGIN_CRS    0      /* _CRS                                       */
#define RES_ORIGIN_AEI    1      /* _AEI (ACPI event GPIOs)                    */
#define RES_ORIGIN_FIELD  2      /* OperationRegion Field Connection()         */

typedef struct acpi_dev {
  char         path[192];        /* "\_SB.I2C1.TCPD" (trailing '_' of segments stripped) */
  char         table[16];        /* origin table file name                     */
  strlist_t    hids;             /* static value or all candidate returns      */
  bool         hid_is_method;
  strlist_t    cids;
  char         uid[32];
  bool         has_uid;
  uint64_t     adr;
  bool         has_adr;
  char         str[128];         /* _STR (Unicode) or _DDN                     */
  int          sta;              /* -1 unknown/dynamic, otherwise static value */
  acpi_res_t  *res;
  int          nres;
  int          rescap;
  int          crs_alternatives; /* number of distinct resource templates in _CRS */
  bool         crs_is_method;
  int          hid_descr_addr;   /* from HID-over-I2C _DSM, -1 if none         */
  strlist_t    deps;             /* _DEP                                       */
  bool         has_dsd;
  int          parent;           /* index of enclosing device, -1 if none      */
  const uint8_t *rom_body;       /* _ROM method body (Qualcomm GPU: panel XML) */
  uint32_t     rom_len;
} acpi_dev_t;

typedef struct {
  acpi_dev_t *d;
  int         count;
  int         cap;
  int         parse_errors;
} acpi_ns_t;

/* parse DSDT + all SSDTs */
void aml_parse_all(const acpi_table_set_t *set, acpi_ns_t *ns);
void aml_free(acpi_ns_t *ns);

acpi_dev_t *aml_find_device(const acpi_ns_t *ns, const char *path);
/* first device having a MEM resource whose base equals addr (exact), else containing addr */
acpi_dev_t *aml_find_by_mmio(const acpi_ns_t *ns, uint64_t addr, bool exact_only);
bool        aml_dev_has_id(const acpi_dev_t *d, const char *id);   /* HID or CID */
const char *aml_dev_primary_hid(const acpi_dev_t *d);

/* Qualcomm GPU _ROM: selects the panel configuration (XML) for a panel ID
   (EDID manufacturer word byte-swapped << 16 | product code). Falls back to
   the default branch if the ID is not listed (*is_default = true).
   xml points into the ACPI table (not NUL-terminated beyond xml_len). */
bool        aml_rom_panel_xml(const acpi_dev_t *d, uint32_t panel_id, char bufname[5], const char **xml,
                              uint32_t *xml_len, bool *is_default);

/* canonical ACPI path: upper case, "\_SB_.I2C1" -> "\_SB.I2C1" */
void        aml_canon_path(const char *in, char *out, size_t cap);

/* human readable one-line resource description */
void        aml_res_describe(const acpi_res_t *r, strbuf_t *sb);
const char *aml_res_kind_name(acpi_res_kind_t k);

#endif /* A2D_AML_PARSER_H */
