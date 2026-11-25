/* SPDX-License-Identifier: MIT */
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

/** @brief kind of a decoded ACPI resource */
typedef enum {
  RES_MEM = 1,       /**< Memory32Fixed, Memory32, Q/D/WordAddress (memory) */
  RES_IO,            /**< IO, FixedIO, address descriptors (I/O) */
  RES_IRQ,           /**< Interrupt / IRQ (GSIV) */
  RES_GPIO_INT,      /**< GpioInt */
  RES_GPIO_IO,       /**< GpioIo */
  RES_I2C,           /**< I2cSerialBusV2 */
  RES_SPI,           /**< SpiSerialBusV2 */
  RES_UART,          /**< UartSerialBusV2 */
  RES_CSI2,          /**< Camera serial bus */
  RES_PIN,           /**< PinFunction / PinConfig / PinGroup */
} acpi_res_kind_t;

/** GPIO: default pull configuration */
#define GPIO_PULL_DEFAULT  0
/** GPIO: pull-up */
#define GPIO_PULL_UP       1
/** GPIO: pull-down */
#define GPIO_PULL_DOWN     2
/** GPIO: no pull */
#define GPIO_PULL_NONE     3

/** @brief one decoded ACPI resource (_CRS, _AEI or field connection) */
typedef struct {
  acpi_res_kind_t kind;  /**< resource kind */
  uint64_t        base;          /**< MEM / IO */
  uint64_t        length;  /**< MEM / IO: length */
  uint32_t        irq;           /**< IRQ: GSIV */
  bool            edge;          /**< IRQ / GPIO_INT */
  bool            active_low;    /**< IRQ / GPIO_INT (GPIO_INT: polarity==1) */
  bool            active_both;   /**< GPIO_INT polarity==2 */
  bool            shared;  /**< shared interrupt / GPIO */
  bool            wake;  /**< wake capable */
  uint16_t        pins[16];      /**< GPIO */
  int             npins;  /**< number of entries in pins */
  int             pull;          /**< GPIO_PULL_* */
  uint16_t        debounce;      /**< GPIO (1/100 ms) */
  int             io_restrict;   /**< GpioIo: 0 none, 1 input, 2 output, 3 preserve */
  uint32_t        addr;          /**< I2C slave address / SPI chip select */
  uint32_t        speed;         /**< I2C/SPI Hz, UART baud */
  bool            addr10;        /**< I2C 10-bit */
  uint8_t         spi_mode;      /**< SPI: (CPOL<<1)|CPHA */
  uint8_t         spi_bits;  /**< SPI: data bit length */
  bool            cs_active_high;  /**< SPI: chip select active high */
  char            source[64];    /**< resource source (controller path) */
  int             alt;           /**< index of the resource template (method alternatives) */
  int             origin;        /**< RES_ORIGIN_* */
} acpi_res_t;

/** resource from _CRS */
#define RES_ORIGIN_CRS    0      /**< _CRS */
#define RES_ORIGIN_AEI    1      /**< _AEI (ACPI event GPIOs) */
#define RES_ORIGIN_FIELD  2      /**< OperationRegion Field Connection() */

/** @brief one ACPI Device() object with identification, status and resources */
typedef struct acpi_dev {
  char         path[192];        /**< "\\_SB.I2C1.TCPD" (trailing '_' of segments stripped) */
  char         table[16];        /**< origin table file name */
  strlist_t    hids;             /**< static value or all candidate returns */
  bool         hid_is_method;  /**< _HID is a method (value may depend on SKU) */
  strlist_t    cids;  /**< _CID values */
  char         uid[32];  /**< _UID value */
  bool         has_uid;  /**< _UID present */
  uint64_t     adr;  /**< _ADR value */
  bool         has_adr;  /**< _ADR present */
  char         str[128];         /**< _STR (Unicode) or _DDN */
  int          sta;              /**< -1 unknown/dynamic, otherwise static value */
  acpi_res_t  *res;  /**< decoded resources */
  int          nres;  /**< number of resources */
  int          rescap;  /**< capacity of res */
  int          crs_alternatives; /**< number of distinct resource templates in _CRS */
  bool         crs_is_method;  /**< _CRS is a method */
  int          hid_descr_addr;   /**< from HID-over-I2C _DSM, -1 if none */
  strlist_t    deps;             /**< _DEP */
  bool         has_dsd;  /**< _DSD present */
  int          parent;           /**< index of enclosing device, -1 if none */
  const uint8_t *rom_body;       /**< _ROM method body (Qualcomm GPU: panel XML) */
  uint32_t     rom_len;  /**< length of rom_body */
  /* results of the ACPICA interpreter (acpica_host.c), A2D_EVAL_* */
  int          eval_hid;  /**< A2D_EVAL_* result of the _HID evaluation */
  int          eval_sta;  /**< A2D_EVAL_* result of the _STA evaluation */
  int          eval_crs;  /**< A2D_EVAL_* result of the _CRS evaluation */
  char         hid_eval[16];     /**< _HID value returned by the interpreter */
} acpi_dev_t;

/** not evaluated / object absent */
#define A2D_EVAL_NONE      0     /**< not evaluated / object absent */
#define A2D_EVAL_EXACT     1     /**< evaluated without touching firmware data */
#define A2D_EVAL_FIRMWARE  2     /**< result depends on firmware data (simulated) */
#define A2D_EVAL_FAILED    3     /**< evaluation failed */

/** @brief all ACPI devices of DSDT and SSDTs */
typedef struct {
  acpi_dev_t *d;  /**< devices */
  int         count;  /**< number of devices */
  int         cap;  /**< capacity of d */
  int         parse_errors;  /**< AML blocks that could not be parsed completely */
} acpi_ns_t;

/**
 * @brief Parses DSDT and all SSDTs into a device list (two passes: method argument counts, then devices).
 *
 * @param set  ACPI tables
 * @param ns   receives the devices
 */
void aml_parse_all(const acpi_table_set_t *set, acpi_ns_t *ns);
/**
 * @brief Frees a device list.
 *
 * @param ns  device list
 */
void aml_free(acpi_ns_t *ns);

/**
 * @brief Looks up a device by canonical path.
 *
 * @param ns    device list
 * @param path  path, e.g. "\\_SB.I2C1"
 * @return the device or NULL
 */
acpi_dev_t *aml_find_device(const acpi_ns_t *ns, const char *path);
/**
 * @brief Finds the device owning an MMIO address.
 *
 * @param ns          device list
 * @param addr        physical address
 * @param exact_only  only accept a resource starting exactly at addr
 * @return the device (exact match preferred) or NULL
 */
acpi_dev_t *aml_find_by_mmio(const acpi_ns_t *ns, uint64_t addr, bool exact_only);
/**
 * @brief Checks whether a device has an ID as _HID or _CID.
 *
 * @param d   device
 * @param id  ID, e.g. "PNP0C50"
 * @return true if present
 */
bool        aml_dev_has_id(const acpi_dev_t *d, const char *id);   /* HID or CID */
/**
 * @brief Returns the first _HID (or _CID) of a device.
 *
 * @param d  device
 * @return the ID or ""
 */
const char *aml_dev_primary_hid(const acpi_dev_t *d);

/**
 * @brief Selects the panel configuration XML of a Qualcomm GPU _ROM method for a panel ID.
 *
 * @param d           device with _ROM method
 * @param panel_id    panel ID (EDID manufacturer word byte-swapped << 16 | product code)
 * @param bufname     receives the AML buffer name
 * @param xml         receives a pointer to the XML text
 * @param xml_len     receives the XML length
 * @param is_default  set if the default branch was used
 * @return true if an XML buffer was found
 */
bool        aml_rom_panel_xml(const acpi_dev_t *d, uint32_t panel_id, char bufname[5], const char **xml,
                              uint32_t *xml_len, bool *is_default);

/**
 * @brief Canonicalizes an ACPI path: upper case, trailing '_' of segments removed.
 *
 * @param in   path as found in AML or Windows
 * @param out  output buffer
 * @param cap  size of the output buffer
 */
void        aml_canon_path(const char *in, char *out, size_t cap);

/**
 * @brief Appends a one-line description of a resource.
 *
 * @param r   resource
 * @param sb  string buffer
 */
void        aml_res_describe(const acpi_res_t *r, strbuf_t *sb);
/**
 * @brief Returns the ASL name of a resource kind.
 *
 * @param k  resource kind
 * @return name, e.g. "GpioInt"
 */
const char *aml_res_kind_name(acpi_res_kind_t k);

#endif /* A2D_AML_PARSER_H */
