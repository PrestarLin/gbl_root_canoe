/*
 * Fill in the memory node the way ABL does while loading a kernel.
 *
 * ABL rewrites /memory with the platform's real DRAM layout before it
 * branches to the kernel; the mainline device trees this application loads
 * carry the layout in a fixed-size reg property, but a tree built straight
 * from the source has a placeholder there and the kernel that boots from it
 * sees no memory at all -- no console, no watchdog report, just a black
 * screen. This is the loader-side half of ABL's job: overwrite the reg
 * value with the captured platform map.
 *
 * The update is in place and fixed size, like PatchChosen's: the tree sits
 * in the device-tree slot of the reserved span, where the structure block
 * cannot grow. A tree whose reg is not exactly the map's size cannot be
 * repaired here and is refused with a message rather than booted into the
 * black screen it would otherwise produce.
 *
 * The map itself: the DRAM layout the vendor bootloader writes on this
 * platform, captured from a known-good device tree (21 regions, 84 cells).
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include "PatchMemory.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>

/* The same structure tokens SfbKernelBoot.c walks; FDT format, spec 0.4. */
#define FDT_BEGIN_NODE  0x00000001U
#define FDT_END_NODE    0x00000002U
#define FDT_PROP        0x00000003U
#define FDT_NOP         0x00000004U
#define FDT_END         0x00000009U
#define FDT_MAGIC       0xd00dfeedU

STATIC CONST UINT32  mMap[84] = {
  0x00000000, 0x81960000, 0x00000000, 0x000a0000, 0x00000000, 0x81a60000,
  0x00000000, 0x001a0000, 0x00000000, 0x81cf4000, 0x00000000, 0x0000c000,
  0x00000000, 0x82478000, 0x00000000, 0x00028000, 0x00000000, 0xc4800000,
  0x00000000, 0x13000000, 0x00000000, 0xd79c0000, 0x00000000, 0x00040000,
  0x00000000, 0xd7c00000, 0x00000000, 0x00400000, 0x00000000, 0xd8800000,
  0x00000000, 0x00000000, 0x00000000, 0xe34a0000, 0x00000000, 0x1c360000,
  0x00000008, 0x80000000, 0x00000000, 0x2f8fe000, 0x00000008, 0xb0000000,
  0x00000000, 0x08100000, 0x00000009, 0x80000000, 0x00000001, 0x80000000,
  0x00000008, 0xc0000000, 0x00000000, 0xc0000000, 0x00000000, 0x82600000,
  0x00000000, 0x00100000, 0x00000000, 0x82800000, 0x00000000, 0x02200000,
  0x00000000, 0x8a980000, 0x00000000, 0x00080000, 0x00000000, 0x9959c000,
  0x00000000, 0x00064000, 0x00000000, 0x9ce80000, 0x00000000, 0x02e00000,
  0x00000000, 0x9ffe0000, 0x00000000, 0x00070000, 0x00000000, 0xa0910000,
  0x00000000, 0x00a70000, 0x00000000, 0xa6400000, 0x00000000, 0x1d9d0000,
};

EFI_STATUS
PatchMemory (
  IN OUT UINT8  *Dtb,
  IN     UINTN   Size
  )
{
  UINT32    TotalSize;
  UINT32    OffStruct;
  UINT32    SizeStruct;
  UINT32    OffStrings;
  UINTN     Pos;
  UINTN     Depth;
  UINT32    Tag;
  UINT32    PropLen;
  UINT32    NameOff;
  BOOLEAN   InMemory = FALSE;
  UINTN     Patched  = 0;
  UINTN     Index;
  UINT32    Magic;
  UINT32    Be32;

  CopyMem (&Magic,     Dtb + 0x00, 4);
  CopyMem (&TotalSize,  Dtb + 0x04, 4);
  CopyMem (&OffStruct,  Dtb + 0x08, 4);
  CopyMem (&SizeStruct, Dtb + 0x24, 4);
  CopyMem (&OffStrings, Dtb + 0x0C, 4);
  Magic      = SwapBytes32 (Magic);
  TotalSize  = SwapBytes32 (TotalSize);
  OffStruct  = SwapBytes32 (OffStruct);
  SizeStruct = SwapBytes32 (SizeStruct);
  OffStrings = SwapBytes32 (OffStrings);

  if (Magic != FDT_MAGIC) {
    Print (L"SfbKernelBoot: not a device tree (magic %x)\n", Magic);
    return EFI_VOLUME_CORRUPTED;
  }

  if (TotalSize > Size || OffStruct + SizeStruct > Size) {
    Print (L"SfbKernelBoot: device tree header describes %x bytes of "
           L"structure at %x, beyond the file (%lx)\n",
           SizeStruct, OffStruct, (UINT64)Size);
    return EFI_VOLUME_CORRUPTED;
  }

  Pos   = OffStruct;
  Depth = 0;

  while (Pos + 4 <= OffStruct + SizeStruct) {
    CopyMem (&Tag, Dtb + Pos, 4);
    Tag = SwapBytes32 (Tag);
    Pos += 4;

    switch (Tag) {
    case FDT_BEGIN_NODE:
      {
        CONST CHAR8  *Name = (CONST CHAR8 *)(Dtb + Pos);
        UINTN        NameLen = AsciiStrLen (Name) + 1;

        /*
         * The memory node sits at depth 1 under the root and may carry a
         * unit address, so compare the stem only.
         */
        if (Depth == 1 && AsciiStrnCmp (Name, "memory", 6) == 0 &&
            (Name[6] == '\0' || Name[6] == '@')) {
          InMemory = TRUE;
        }
        Depth++;
        Pos += ALIGN_VALUE (NameLen, 4);
      }
      break;

    case FDT_END_NODE:
      if (Depth == 2) {
        InMemory = FALSE;
      }
      if (Depth > 0) {
        Depth--;
      }
      break;

    case FDT_PROP:
      CopyMem (&PropLen, Dtb + Pos, 4);
      CopyMem (&NameOff, Dtb + Pos + 4, 4);
      PropLen = SwapBytes32 (PropLen);
      NameOff = SwapBytes32 (NameOff);
      Pos += 8;

      if (InMemory && OffStrings + NameOff < Size) {
        CONST CHAR8  *PropName = (CONST CHAR8 *)(Dtb + OffStrings + NameOff);

        if (AsciiStrCmp (PropName, "reg") == 0) {
          if (PropLen != sizeof (mMap)) {
            Print (L"SfbKernelBoot: /memory reg holds %u bytes, the platform "
                   L"map needs %u; this device tree cannot be repaired "
                   L"in place\n", PropLen, (UINT32)sizeof (mMap));
            return EFI_BUFFER_TOO_SMALL;
          }
          for (Index = 0; Index < 84; Index++) {
            Be32 = SwapBytes32 (mMap[Index]);
            CopyMem (Dtb + Pos + Index * 4, &Be32, sizeof (Be32));
          }
          Patched++;
        }
      }
      Pos += ALIGN_VALUE (PropLen, 4);
      break;

    case FDT_NOP:
      break;

    case FDT_END:
      if (Patched == 0) {
        Print (L"SfbKernelBoot: device tree has no /memory reg to fill in\n");
        return EFI_NOT_FOUND;
      }
      return EFI_SUCCESS;

    default:
      Print (L"SfbKernelBoot: unexpected FDT token %x at %lx\n",
             Tag, (UINT64)(Pos - 4));
      return EFI_VOLUME_CORRUPTED;
    }
  }

  Print (L"SfbKernelBoot: ran off the end of the device tree structure\n");
  return EFI_VOLUME_CORRUPTED;
}
