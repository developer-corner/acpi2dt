/******************************************************************************
 *
 * Name: acwin.h - OS specific defines, etc.
 *
 *****************************************************************************/

/******************************************************************************
 *
 * Copyright (c) 1999 - 2026, Intel Corp.
 * SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0-only
 *
 *****************************************************************************/

#ifndef __ACWIN_H__
#define __ACWIN_H__

#include <io.h>

#define ACPI_USE_STANDARD_HEADERS
#define ACPI_USE_SYSTEM_CLIBRARY

 /* Note: do not include any C library headers here */

 /*
 * Note: MSVC project files should define ACPI_DEBUGGER and ACPI_DISASSEMBLER
 * as appropriate to enable editor functions like "Find all references".
 * The editor isn't smart enough to dig through the include files to find
 * out if these are actually defined.
 */

 /* Eliminate warnings for "old" (non-secure) versions of clib functions */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

 /* Eliminate warnings for POSIX clib function names (open, write, etc.) */

#ifndef _CRT_NONSTDC_NO_DEPRECATE
#define _CRT_NONSTDC_NO_DEPRECATE
#endif

 /*  Disable these warnings under Windows and MSVC since C4201: nameless 
  *  structs/unions are needed as ACPI event-handling under Linux/GCC breaks
  *  with named and nested structs/unions while C4068: unknown pragma
  *  complains of GCC pragmas in use to disable dangling ptr warnings
  */

#pragma warning( disable: 4201)
#pragma warning( disable: 4068)
#pragma warning( disable: 4214)
#pragma warning( push)

#define ACPI_MACHINE_WIDTH      32
#define ACPI_USE_NATIVE_DIVIDE
#define ACPI_USE_NATIVE_MATH64

#ifdef ACPI_DEFINE_ALTERNATE_TYPES
/*
 * Types used only in (Linux) translated source, defined here to enable
 * cross-platform compilation (i.e., generate the Linux code on Windows,
 * for test purposes only)
 */
typedef int                             s32;
typedef unsigned char                   u8;
typedef unsigned short                  u16;
typedef unsigned int                    u32;
typedef COMPILER_DEPENDENT_UINT64       u64;
#endif

/*
 * Map low I/O functions for MS. This allows us to disable MS language
 * extensions for maximum portability.
 */
#define open            _open
#define read            _read
#define write           _write
#define close           _close
#define stat            _stat
#define fstat           _fstat
#define mkdir           _mkdir
#define fileno          _fileno
#define isatty          _isatty

#if _MSC_VER <= 1200 /* Versions below VC++ 6 */
#define vsnprintf       _vsnprintf
#endif
#define O_RDONLY        _O_RDONLY
#define O_BINARY        _O_BINARY
#define O_CREAT         _O_CREAT
#define O_WRONLY        _O_WRONLY
#define O_TRUNC         _O_TRUNC
#define S_IREAD         _S_IREAD
#define S_IWRITE        _S_IWRITE
#define S_IFDIR         _S_IFDIR
#if _MSC_VER < 1900
#define snprintf        _snprintf
#endif


/*
 * Handle platform- and compiler-specific assembly language differences.
 *
 * Notes:
 * 1) Interrupt 3 is used to break into a debugger
 * 2) Interrupts are turned off during ACPI register setup
 */

/*! [Begin] no source code translation  */

#if defined(_M_ARM64)

#include <intrin.h>

#ifdef ACPI_APPLICATION
#define ACPI_FLUSH_CPU_CACHE()
#else
/*
 * No ARM64 equivalent of WBINVD. Cache cleaning before sleep is done by
 * firmware via PSCI. Use a full-system data barrier so all prior stores
 * have completed before the sleep register is written.
 */
#define ACPI_FLUSH_CPU_CACHE()  __dsb(_ARM64_BARRIER_SY)
#endif

#else /* x86 */

#ifdef ACPI_APPLICATION
#define ACPI_FLUSH_CPU_CACHE()
#else
#define ACPI_FLUSH_CPU_CACHE()  __asm {WBINVD}
#endif

#endif

#ifdef _DEBUG
#define ACPI_SIMPLE_RETURN_MACROS
#endif

/*! [End] no source code translation !*/

/******************************************************************************
 *
 * ARM64 (AArch64) Global Lock acquire/release for acwin.h
 *
 * Semantics are identical to the x86 __asm versions:
 *
 *   Acquire:  new = (old & ~3) | OWNED | (old_owned ? PENDING : 0)
 *             Acq = ((UINT8) new < 3) ? 0xFF : 0     (0xFF = acquired)
 *             FacsPtr == NULL -> Acq = 0xFF
 *
 *   Release:  new = old & ~3
 *             Pnd = old & PENDING                    (1 = waiter pending)
 *             FacsPtr == NULL -> Pnd = 0
 *
 * Bit 0 = Pending, bit 1 = Owned (ACPI spec, FACS Global Lock).
 *
 * NOTE: This header is pulled in via acenv.h *before* actypes.h/actbl.h,
 * so the helpers use only builtin C types and the macros resolve
 * (FacsPtr)->GlobalLock at the point of use, just like the x86
 * "[ecx].GlobalLock" form does.
 *
 *****************************************************************************/

#if defined(_M_ARM64) || defined(__aarch64__)

#if defined(_MSC_VER) && !defined(__clang__)

 /*
  * MSVC for ARM64 has no inline assembler (__asm is x86-only),
  * so use the interlocked intrinsics. These compile to LDAXR/STXR
  * (or CASA/CASL with /arch:armv8.1 and later).
  */
#include <intrin.h>

static __forceinline unsigned char
AcpiArm64AcquireGlobalLock(
  volatile long* Lock)
{
  unsigned long           Old;
  unsigned long           New;

  do
  {
    Old = (unsigned long)__iso_volatile_load32((volatile __int32*)Lock);
    New = (Old & ~3UL) | 2UL | ((Old >> 1) & 1UL);
  } while ((unsigned long)_InterlockedCompareExchange_acq(
    Lock, (long)New, (long)Old) != Old);

  return (((unsigned char)New < 3) ? 0xFF : 0);
}

static __forceinline unsigned char
AcpiArm64ReleaseGlobalLock(
  volatile long* Lock)
{
  unsigned long           Old;
  unsigned long           New;

  do
  {
    Old = (unsigned long)__iso_volatile_load32((volatile __int32*)Lock);
    New = Old & ~3UL;
  } while ((unsigned long)_InterlockedCompareExchange_rel(
    Lock, (long)New, (long)Old) != Old);

  return ((unsigned char)(Old & 1));
}

#define ACPI_ACQUIRE_GLOBAL_LOCK(FacsPtr, Acq) \
    ((Acq) = (FacsPtr) ? AcpiArm64AcquireGlobalLock ( \
        (volatile long *) &(FacsPtr)->GlobalLock) : 0xFF)

#define ACPI_RELEASE_GLOBAL_LOCK(FacsPtr, Pnd) \
    ((Pnd) = (FacsPtr) ? AcpiArm64ReleaseGlobalLock ( \
        (volatile long *) &(FacsPtr)->GlobalLock) : 0)

#else /* GCC / Clang (incl. clang-cl): real AArch64 inline assembly */

static __inline__ __attribute__((always_inline)) unsigned char
AcpiArm64AcquireGlobalLock(
  unsigned int* Lock)
{
  unsigned int            Old;
  unsigned int            New;
  unsigned int            Tmp;

  __asm__ __volatile__(
    "1: ldaxr   %w[old], %[lock]            \n" /* load-acquire exclusive    */
    "   ubfx    %w[tmp], %w[old], #1, #1    \n" /* tmp = old Owned bit       */
    "   and     %w[new], %w[old], #0xFFFFFFFC\n" /* clear Owned + Pending    */
    "   orr     %w[new], %w[new], #2        \n" /* set Owned                 */
    "   orr     %w[new], %w[new], %w[tmp]   \n" /* Pending = was owned       */
    "   stxr    %w[tmp], %w[new], %[lock]   \n" /* try to store              */
    "   cbnz    %w[tmp], 1b                 \n" /* lost reservation: retry   */
    "   and     %w[tmp], %w[new], #0xFF     \n" /* like "cmp dl, 3"          */
    "   cmp     %w[tmp], #3                 \n"
    "   csetm   %w[tmp], lo                 \n" /* like "sbb eax, eax"       */
    : [old] "=&r" (Old), [new] "=&r" (New), [tmp] "=&r" (Tmp),
    [lock] "+Q" (*Lock)
    :
    : "cc", "memory");

  return ((unsigned char)Tmp);               /* 0xFF = acquired */
}

static __inline__ __attribute__((always_inline)) unsigned char
AcpiArm64ReleaseGlobalLock(
  unsigned int* Lock)
{
  unsigned int            Old;
  unsigned int            New;
  unsigned int            Tmp;

  __asm__ __volatile__(
    "1: ldxr    %w[old], %[lock]            \n" /* load exclusive            */
    "   and     %w[new], %w[old], #0xFFFFFFFC\n" /* clear Owned + Pending    */
    "   stlxr   %w[tmp], %w[new], %[lock]   \n" /* store-release exclusive   */
    "   cbnz    %w[tmp], 1b                 \n"
    "   and     %w[old], %w[old], #1        \n" /* return old Pending bit    */
    : [old] "=&r" (Old), [new] "=&r" (New), [tmp] "=&r" (Tmp),
    [lock] "+Q" (*Lock)
    :
    : "memory");

  return ((unsigned char)Old);
}

#define ACPI_ACQUIRE_GLOBAL_LOCK(FacsPtr, Acq) \
    ((Acq) = (FacsPtr) ? AcpiArm64AcquireGlobalLock ( \
        (unsigned int *) &(FacsPtr)->GlobalLock) : 0xFF)

#define ACPI_RELEASE_GLOBAL_LOCK(FacsPtr, Pnd) \
    ((Pnd) = (FacsPtr) ? AcpiArm64ReleaseGlobalLock ( \
        (unsigned int *) &(FacsPtr)->GlobalLock) : 0)

#endif /* _MSC_VER */

#else

/*
 * Global Lock acquire/release code
 *
 * Note: Handles case where the FACS pointer is null
 */
#define ACPI_ACQUIRE_GLOBAL_LOCK(FacsPtr, Acq)  __asm \
{                                                   \
        __asm mov           eax, 0xFF               \
        __asm mov           ecx, FacsPtr            \
        __asm or            ecx, ecx                \
        __asm jz            exit_acq                \
        __asm lea           ecx, [ecx].GlobalLock   \
                                                    \
        __asm acq10:                                \
        __asm mov           eax, [ecx]              \
        __asm mov           edx, eax                \
        __asm and           edx, 0xFFFFFFFE         \
        __asm bts           edx, 1                  \
        __asm adc           edx, 0                  \
        __asm lock cmpxchg  dword ptr [ecx], edx    \
        __asm jnz           acq10                   \
                                                    \
        __asm cmp           dl, 3                   \
        __asm sbb           eax, eax                \
                                                    \
        __asm exit_acq:                             \
        __asm mov           Acq, al                 \
}

#define ACPI_RELEASE_GLOBAL_LOCK(FacsPtr, Pnd) __asm \
{                                                   \
        __asm xor           eax, eax                \
        __asm mov           ecx, FacsPtr            \
        __asm or            ecx, ecx                \
        __asm jz            exit_rel                \
        __asm lea           ecx, [ecx].GlobalLock   \
                                                    \
        __asm Rel10:                                \
        __asm mov           eax, [ecx]              \
        __asm mov           edx, eax                \
        __asm and           edx, 0xFFFFFFFC         \
        __asm lock cmpxchg  dword ptr [ecx], edx    \
        __asm jnz           Rel10                   \
                                                    \
        __asm cmp           dl, 3                   \
        __asm and           eax, 1                  \
                                                    \
        __asm exit_rel:                             \
        __asm mov           Pnd, al                 \
}

#endif /* _M_ARM64 || __aarch64__ */

#endif /* __ACWIN_H__ */
