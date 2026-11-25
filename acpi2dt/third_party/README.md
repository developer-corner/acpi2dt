# Third-party code embedded in acpi2dt

Both components are compiled directly into `acpi2dt.exe` (see the item groups
"acpica" and "dtc" in `acpi2dt.vcxproj`). They are copies of the neighbouring
projects of the solution (`../../acpica`, `../../dtc`), reduced to the files
acpi2dt needs.

## ACPICA (`acpica/`)

| | |
|---|---|
| Origin | Intel ACPICA, version `0x20260930` (copy of `../../acpica`) |
| License | BSD-3-Clause OR GPL-2.0-only (`acpica/LICENSE.*`) |
| Files | the core components, disassembler, debugger and Windows OS layer of the AcpiExec project (`source/components`, `source/common/{acfileio,ahids,ahuuids,cmfsize}.c`, `source/os_specific/service_layers/{oswinxf,oswindir,osgendbg}.c`, `source/include`), plus `source/tools/acpiexec/aecommon.h` (header only) |
| Configuration | `WIN32;__STDC__;_CONSOLE;ACPI_EXEC_APP` (application mode with interpreter, debugger and disassembler) |
| Glue | `../src/acpica_host.c` provides the acpiexec upcalls (`AeLookupInitFileEntry`, `AeDoObjectOverrides`, `AeTableOverride`), the iasl map-file hooks used by the resource disassembler (`MpSaveGpioInfo`, `MpSaveSerialInfo`) and `AcpiOsGetRootPointer` |

Local modification:

* `source/include/platform/acwin.h`: `ACPI_MACHINE_WIDTH` is 64 for `_WIN64`
  targets (x64, ARM64). The original value 32 makes `ACPI_SIZE` and physical
  addresses 32 bits wide while pointers are 64 bits, which truncates heap and
  table addresses above 4 GB. The same fix is applied to the standalone
  `../../acpica` projects of the solution.

## dtc (`dtc/`)

| | |
|---|---|
| Origin | Device Tree Compiler 1.7.2 as shipped in the Linux kernel (`scripts/dtc`), Windows port from `../../dtc` |
| License | GPL-2.0-or-later; `libfdt/` headers GPL-2.0-or-later OR BSD-2-Clause; `getopt.c`/`getopt.h` MIT (musl) |
| Files | compiler sources incl. `dtc.c`, pre-generated `dtc-lexer.lex.c` / `dtc-parser.tab.c`, libfdt headers; `yamltree.c` and the libfdt sources are not needed (`NO_YAML`) |
| Glue | `dtc_port.h` (forced include, MIT): renames `main` -> `dtc_main`, `xstrdup`/`xstrndup` (clash with acpi2dt) and `exit` -> `dtc_exit` (fatal dtc errors return to acpi2dt via `longjmp`, see `../src/dtc_bridge.c`) |

Modifications of the Windows port compared to the kernel sources:

* `checks.c`: MSVC does not accept empty initializer lists, therefore every
  prerequisite array starts with a `NULL` placeholder; `.prereq` points behind
  it (`nm_##_prereqs + 1`). Without the `+ 1` every check dereferenced the
  placeholder (crash on any input) - fixed in this repository.
* `dtc.c`, `fstree.c`, `treesource.c`, `util.h`, `libfdt/fdt_sw.c`: MSVC
  compatibility (`?:` with omitted middle operand, `S_ISDIR`, `strcasecmp`,
  `NORETURN`, directory reading with `FindFirstFile`).
* `unistd.h`, `getopt.c`, `getopt.h`: POSIX shims for Windows.

The output of the embedded dtc was verified to be byte-identical to DTC 1.7.2
built on Linux.
