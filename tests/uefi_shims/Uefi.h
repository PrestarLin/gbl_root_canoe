/*
 * Host-side stand-in for MdePkg's Uefi.h: just enough of the type and
 * status-code surface for PatchMemory.c to compile and run as a normal
 * host process under tests/run.sh. The production file itself is compiled
 * unmodified; only the headers it names resolve here instead of MdePkg.
 */
#ifndef HOST_UEFI_H
#define HOST_UEFI_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef size_t    UINTN;
typedef ptrdiff_t INTN;
typedef char      CHAR8;
typedef wchar_t   CHAR16;
typedef uint8_t   BOOLEAN;
typedef void      VOID;

typedef UINTN EFI_STATUS;

#define IN
#define OUT
#define OPTIONAL
#define CONST const
#define STATIC static
#ifndef NULL
#define NULL ((void *)0)
#endif

#define EFIERR(a) (0x8000000000000000ULL | (a))
#define EFI_ERROR(a) (((INTN)(a)) < 0)

#define TRUE  ((BOOLEAN)(1 == 1))
#define FALSE ((BOOLEAN)(0 == 1))

/* MdePkg/Base.h formula, verbatim. */
#define ALIGN_VALUE(Value, Alignment) ((Value) + (((Alignment) - (Value)) & ((Alignment) - 1)))

#define EFI_SUCCESS            0
#define EFI_BUFFER_TOO_SMALL   EFIERR (5)
#define EFI_VOLUME_CORRUPTED   EFIERR (9)
#define EFI_LOAD_ERROR         EFIERR (1)
#define EFI_NOT_FOUND          EFIERR (14)
#define EFI_OUT_OF_RESOURCES   EFIERR (9)
/* MdePkg/Base.h: EFI_NOT_READY = EFIERR(12). */
#define EFI_NOT_READY          EFIERR (12)

/* MdePkg/Base.h. */
#define EFI_PAGE_SIZE  4096

typedef UINT64 EFI_PHYSICAL_ADDRESS;
typedef UINT64 EFI_VIRTUAL_ADDRESS;

/*
 * MdePkg/UefiMultiPhase.h's EFI_MEMORY_DESCRIPTOR, layout verbatim: the
 * firmware hands these out with GetMemoryMap's DescSize stride, which may
 * be larger than sizeof here.
 */
typedef struct {
  UINT32                 Type;
  EFI_PHYSICAL_ADDRESS   PhysicalStart;
  EFI_VIRTUAL_ADDRESS    VirtualStart;
  UINT64                 NumberOfPages;
  UINT64                 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* MdePkg/UefiMultiPhase.h memory type values, verbatim. */
#define EfiReservedMemoryType         0
#define EfiLoaderCode                 1
#define EfiLoaderData                 2
#define EfiBootServicesCode           3
#define EfiBootServicesData           4
#define EfiRuntimeServicesCode        5
#define EfiRuntimeServicesData        6
#define EfiConventionalMemory         7
#define EfiUnusableMemory             8
#define EfiACPIReclaimMemory          9
#define EfiACPIMemoryNVS              10
#define EfiMemoryMappedIO             11
#define EfiMemoryMappedIOPortSpace    12
#define EfiPalCode                    13
#define EfiPersistentMemory           14
#define EfiUnacceptedMemoryType       15

#endif /* HOST_UEFI_H */
