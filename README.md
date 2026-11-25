# ACPI to Device Tree semi-automatic tool for MS Windows 11 on ARM64

`acpi2dt` runs on a Windows 11 ARM64 notebook (Snapdragon X / X Elite / X Plus,
8cx) and **drafts** a Linux device tree (DTS) for that machine. The draft is a
proposal for a human (or an AI) to review: every node carries comments that
name the source of the information and the result of cross-checks.

## Information sources

| Tag | Source |
|---|---|
| `[ACPI]` | ACPI tables of the running machine (`GetSystemFirmwareTable`, `HKLM\HARDWARE\ACPI`), parsed by a built-in static AML walker: `_HID/_CID/_UID/_ADR/_STA/_STR`, `_CRS` resource templates (MMIO, interrupts, GpioInt/GpioIo, I2C/SPI/UART), `_AEI`, OperationRegion `Connection()` fields, HID-over-I2C `_DSM` |
| `[DevMgr]` | Windows Device Manager (SetupAPI/CfgMgr32), linked to ACPI by `BiosDeviceName` |
| `[INF]` / `[FW]` | driver packages in the DriverStore; firmware is resolved, copied into a `/lib/firmware` layout and listed with SHA-256 in `FIRMWARE-MANIFEST.txt` |
| `[SMBIOS]` | `model` / `compatible`, firmware vendor and board directory |
| `[EDID]` | EDID of the monitors known to Windows (also non-present ones): the internal eDP panel |
| `[CAMERA]` | Qualcomm camera driver configuration applied per device (`Device Parameters\Resources`): sensor model, I2C address and chip ID (`SCFG_*.bin`), power-on sequence with regulators, MCLK and reset GPIO (`CAM*_RES_*.bin`, "AeoB" format) |
| `[KERNEL]` | SoC and PMIC `.dtsi` of the Linux kernel DTS folder; SoC-specific `/delete-node/` statements are learned from the kernel's other boards of the same SoC |
| `[TEMPLATE]` / `[SIMILARn]` | optional DTS files of similar notebooks (first = template) |

What the tool does with them:

* matches ACPI devices to SoC DT nodes by MMIO address (`\_SB.I2C1 @ 0xb80000` ↔ `i2c0: i2c@b80000`)
* decodes Qualcomm virtual GPIO numbers (GIO0 interrupt list → `qcom,pdc-ranges` → PDC wake map → TLMM pin)
* re-emits the template node by node and checks I2C/SPI/UART children, interrupt and GPIO pins and `hid-descr-addr` against ACPI (`OK` / `CHECK` / `WARNING`)
* reads the panel configuration of the Qualcomm GPU driver from ACPI (`\_SB.GPU0._ROM` XML, selected by the panel ID derived from the EDID): backlight type, PMIC PWM frequency → `pwm-backlight` `pwms`, eDP DPCD override; checks panel / `pwm-backlight` nodes and suggests a backlight node if the draft lacks one
* checks `pciVVVV,DDDD` compatibles against the present PCI devices, Qualcomm `qcom,wcnNNNN-*` compatibles against the Wi-Fi/BT chip actually fitted, and panel compatibles against the internal panel's EDID
* replaces the SoC include if the template is for another SoC, inlines board `.dtsi` files of the template
* rewrites `firmware-name` to `qcom/<soc>/<vendor>/<board>/…` and prefers SoC-specific blobs (e.g. `qcdxkmsucpurwa.mbn` on X1P42100)
* takes over nodes from secondary similar DTS files when ACPI confirms them, otherwise adds them as commented-out suggestions
* adds ACPI-only bus devices (HID-over-I2C as real nodes, others as commented templates)
* appends an inventory: ACPI namespace with decoded resources, PCI / problem devices, firmware, SoC nodes that ACPI uses but the draft leaves disabled
* evaluates `_HID/_CID/_UID/_STA/_CRS` and the HID-over-I2C `_DSM` with the **embedded ACPICA interpreter**; operation regions are simulated (no hardware access), so a result counts as exact only if it did not read firmware data (`[ACPICA]` annotations)
* disassembles DSDT/SSDT to `.dsl` with the **embedded ACPICA disassembler** (no iasl needed)
* compiles the draft to `<output>.dtb` with the **embedded dtc** and a built-in DTS preprocessor (same result as cpp + dtc in the kernel build)

ACPI does not describe clocks, regulators, pin configuration or power
sequencing, so a template DTS of a similar notebook gives by far the best draft.

## Usage

```
acpi2dt [options] <output dts> <output firmware folder> <linux kernel dts folder> [<similar dts> ...]
```

Example (ASUS Vivobook S16, S15 as template, Zenbook A14 as secondary source):

```
acpi2dt s16-draft.dts fw D:\linux\arch\arm64\boot\dts ^
        D:\linux\arch\arm64\boot\dts\qcom\x1e80100-asus-vivobook-s15.dts ^
        D:\linux\arch\arm64\boot\dts\qcom\x1p42100-asus-zenbook-a14.dts
```

Use similar DTS files **from the same kernel tree** as the kernel DTS folder:
newer kernels move board content into `.dtsi` files that must be resolvable.

Important options (`acpi2dt --help` lists all):

| Option | Meaning |
|---|---|
| `--acpi-dir <dir>` | read ACPI tables from a dump folder (offline, e.g. other machine) |
| `--iasl <exe>` | use an external `iasl`/`AslCompiler.exe` for the `.dsl` files instead of the embedded disassembler |
| `--no-iasl` | no `.dsl` disassembly |
| `--no-acpica` | no embedded AML interpreter (static AML analysis only) |
| `--no-dtc` | do not compile the draft to a DTB |
| `--soc <name>` | override SoC detection (`x1p42100`, `x1e80100`, …) |
| `--model`, `--compatible`, `--vendor-dir`, `--board-slug` | override the identity derived from SMBIOS |
| `--no-devmgr`, `--no-drivers`, `--no-fw-copy` | skip sources / copying |
| `-v` | verbose |

Output: the draft `<output>.dts`, the compiled `<output>.dtb` and the firmware
folder. Intermediate files go to `<output dts>.acpi2dt\`: raw ACPI tables
(`acpi\*.dat`) and their disassembly (`acpi\*.dsl`), `acpica.log`,
`hardware_inventory.json` (Device Manager, monitors), `firmware_scan.json`
(driver packages and firmware), `panel_<buf>.xml` (GPU panel configuration),
`draft.preprocessed.dts` and `dtc.log`.

Firmware copied from the Windows DriverStore is usually not redistributable;
use it only on the machine it came from.

## Checking a draft

acpi2dt compiles the draft itself (`<output>.dtb`, messages in `dtc.log`). To
check an edited draft by hand, it has to go through the C preprocessor before
`dtc`, like in the kernel build:

```
clang -E -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I <dts>\qcom -I <dts> -I <linux>\include -o pre.dts draft.dts
dtc -I dts -O dtb -o draft.dtb pre.dts
```

(Kernel trees checked out on Windows may contain unreadable symlinks, e.g.
`include/dt-bindings/input/linux-event-codes.h`; acpi2dt's preprocessor falls
back to `include/uapi/linux/input-event-codes.h`.)

## Building

Visual Studio 2026 (v145 toolset), solution `acpi2dt.slnx`, platform ARM64.
The project `acpi2dt` is self-contained: ACPICA and dtc are compiled into
`acpi2dt.exe` from `acpi2dt/third_party` (see `third_party/README.md`); the
only libraries are those of the Windows SDK (`setupapi`, `cfgmgr32`,
`advapi32`, `bcrypt`).

## Documentation

The sources are documented for doxygen:

```
cd acpi2dt
doxygen Doxyfile        # -> doxygen/html/index.html
```

## License

acpi2dt's own source files are MIT licensed; `src/pdc_maps.c` (data from the
Linux kernel) is GPL-2.0-only. The embedded third-party code is ACPICA
(BSD-3-Clause OR GPL-2.0-only) and dtc (GPL-2.0-or-later). The `acpi2dt.exe`
binary as a whole is therefore distributed under **GPL-2.0-only**. Details:
`acpi2dt/LICENSE`, license texts in `acpi2dt/LICENSES/`.

## Source layout

| File | Purpose |
|---|---|
| `src/main.c` | command line, orchestration, summary |
| `src/acpi_extractor.c` | ACPI table acquisition, SMBIOS, MADT, SoC / board identity |
| `src/aml_parser.c` | static AML namespace walker and resource template decoder |
| `src/devmgr_explorer.c` | Device Manager enumeration |
| `src/inf_firmware_scanner.c` | driver packages, INF parsing, firmware resolution and copy |
| `src/dts_parser.c` | DTS/DTSI parser (keeps comments) and merged tree builder |
| `src/dts_merger.c` | draft generation, annotations, appendix |
| `src/camera.c` | camera sensors from the Windows camera configuration (AeoB files) |
| `src/acpica_host.c` | embedded ACPICA: table loading, simulated regions, evaluation, disassembly |
| `src/dts_cpp.c` | built-in C preprocessor for DTS files |
| `src/dtc_bridge.c` | embedded dtc: in-process DTS -> DTB compilation |
| `src/pdc_maps.c` | Qualcomm TLMM -> PDC wake maps (GPL-2.0-only, from Linux) |
| `src/util.c` | console, strings, paths, files, hashing |
| `third_party/acpica`, `third_party/dtc` | embedded ACPICA and dtc (see `third_party/README.md`) |
