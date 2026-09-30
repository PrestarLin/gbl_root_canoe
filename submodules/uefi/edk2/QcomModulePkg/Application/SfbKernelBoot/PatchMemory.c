/*
 * Synthesize the platform DRAM map from the firmware's own GetMemoryMap
 * and write it into a device tree's /memory node -- the loader-side half
 * of what ABL does while loading a kernel.
 *
 * The mainline device trees this application loads carry the layout in a
 * fixed-size reg property (336 bytes, 21 regions, 84 cells); a tree built
 * straight from the source has a placeholder there and the kernel that
 * boots from it sees no memory at all -- no console, no watchdog report,
 * just a black screen. The map itself must describe the machine it boots
 * on: the vendor map captured from a 12G unit has ~30MB of ghost pages on
 * a 16G unit (regions past the end of RAM or inside holes), and handing
 * those to the page allocator ends in a synchronous external abort. So
 * instead of carrying any captured map, this reads the UEFI memory map of
 * the running machine and rebuilds the layout from it:
 *
 *   - keep the RAM-bearing descriptor types (Loader, BootServices,
 *     RuntimeServices, Conventional, ACPI reclaim/NVS) and drop Reserved,
 *     Unusable and MMIO -- carve-outs stay holes, the way the vendor map
 *     leaves them;
 *   - clip to the two DRAM windows this platform uses (low 2-4G, high
 *     32-64G); anything else never reaches the device tree;
 *   - sort and coalesce contiguous ranges;
 *   - validate: 1..21 regions, at least 8 GB -- anything else is refused
 *     with EFI_NOT_READY and the tree keeps its own map.
 *
 * The update is in place and fixed size, like PatchChosen's: the tree
 * sits in the device-tree slot of the reserved span, where the structure
 * block cannot grow. A tree whose reg is not exactly the slot's size
 * cannot be repaired here and is refused with a message rather than
 * booted into the black screen it would otherwise produce.
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

/* The fixed-size reg slot: 21 regions, 4 cells each, 4 bytes per cell. */
#define REG_BYTES       336
#define MAX_REGIONS     21

/* Ceiling for collected sub-ranges before merging; well above 21. */
#define MAX_CANDIDATES  512

/* Sanity floor: neither supported capacity (12G/16G) boots with less. */
#define MIN_TOTAL       (8ULL * 1024 * 1024 * 1024)

/* The two DRAM windows this platform's memory lives in. */
#define LO_START        0x0000000080000000ULL   /* 2G  */
#define LO_END          0x0000000100000000ULL   /* 4G  */
#define HI_START        0x0000000800000000ULL   /* 32G */
#define HI_END          0x0000001000000000ULL   /* 64G */
#define MAX_TOTAL       ((LO_END - LO_START) + (HI_END - HI_START))

typedef struct {
  UINT64  Start;
  UINT64  End;
} Region;

STATIC Region  mCandidates[MAX_CANDIDATES];
STATIC UINT32  mCells[MAX_REGIONS * 4];

/*
 * RAM-bearing descriptor types. Reserved is deliberately absent: on this
 * platform it carries the carve-outs the vendor map leaves as holes, and
 * declaring them usable is exactly the ghost-page failure this replaces.
 */
STATIC
BOOLEAN
IsRamType (
  IN UINT32  Type
  )
{
  switch (Type) {
  case EfiLoaderCode:
  case EfiLoaderData:
  case EfiBootServicesCode:
  case EfiBootServicesData:
  case EfiRuntimeServicesCode:
  case EfiRuntimeServicesData:
  case EfiConventionalMemory:
  case EfiACPIReclaimMemory:
  case EfiACPIMemoryNVS:
    return TRUE;
  default:
    return FALSE;
  }
}

/*
 * Build mCells from the descriptor array. EFI_NOT_READY means the map
 * cannot be trusted as a platform layout; the caller then leaves the
 * device tree's own reg in place.
 */
STATIC
EFI_STATUS
SynthesizeMap (
  IN  CONST EFI_MEMORY_DESCRIPTOR  *Map,
  IN  UINTN                        MapSize,
  IN  UINTN                        DescSize,
  OUT UINTN                        *CountOut,
  OUT UINT64                       *TotalOut
  )
{
  UINTN    Count;
  UINTN    Index;
  UINTN    N = 0;
  EFI_STATUS Status;

  if (Map == NULL || DescSize < sizeof (EFI_MEMORY_DESCRIPTOR) ||
      MapSize < DescSize) {
    return EFI_NOT_READY;
  }

  Count = MapSize / DescSize;
  for (Index = 0; Index < Count; Index++) {
    CONST EFI_MEMORY_DESCRIPTOR  *D;
    UINT64                       Start;
    UINT64                       End;
    UINTN                        W;

    D = (CONST EFI_MEMORY_DESCRIPTOR *)((CONST UINT8 *)Map + Index * DescSize);
    if (!IsRamType (D->Type) || D->NumberOfPages == 0) {
      continue;
    }
    if (D->NumberOfPages > (0xffffffffffffffffULL >> 12)) {
      continue;
    }
    Start = D->PhysicalStart;
    End   = Start + (D->NumberOfPages << 12);
    if (End < Start) {
      continue;
    }
    for (W = 0; W < 2; W++) {
      UINT64  Ws   = (W == 0) ? LO_START : HI_START;
      UINT64  We   = (W == 0) ? LO_END : HI_END;
      UINT64  Lo   = (Start > Ws) ? Start : Ws;
      UINT64  Hi   = (End < We) ? End : We;

      if (Hi <= Lo) {
        continue;
      }
      if (N >= MAX_CANDIDATES) {
        return EFI_NOT_READY;
      }
      mCandidates[N].Start = Lo;
      mCandidates[N].End   = Hi;
      N++;
    }
  }

  if (N == 0) {
    return EFI_NOT_READY;
  }

  /* Insertion sort by start address; N is small. */
  for (Index = 1; Index < N; Index++) {
    Region  R = mCandidates[Index];
    UINTN   J = Index;

    while (J > 0 && mCandidates[J - 1].Start > R.Start) {
      mCandidates[J] = mCandidates[J - 1];
      J--;
    }
    mCandidates[J] = R;
  }

  /* Coalesce contiguous ranges, never bridging a hole. */
  {
    UINT64  OStart[MAX_REGIONS + 1];
    UINT64  OEnd[MAX_REGIONS + 1];
    UINTN   Out = 0;
    UINT64  Total = 0;

    for (Index = 0; Index < N; Index++) {
      if (Out > 0 && mCandidates[Index].Start <= OEnd[Out - 1]) {
        if (mCandidates[Index].End > OEnd[Out - 1]) {
          OEnd[Out - 1] = mCandidates[Index].End;
        }
        continue;
      }
      if (Out > MAX_REGIONS) {
        return EFI_NOT_READY;
      }
      OStart[Out] = mCandidates[Index].Start;
      OEnd[Out]   = mCandidates[Index].End;
      Out++;
    }

    if (Out == 0 || Out > MAX_REGIONS) {
      return EFI_NOT_READY;
    }

    for (Index = 0; Index < Out; Index++) {
      UINT64  S = OStart[Index];
      UINT64  L = OEnd[Index] - OStart[Index];

      Total += L;
      mCells[Index * 4]     = (UINT32)(S >> 32);
      mCells[Index * 4 + 1] = (UINT32)(S & 0xffffffffULL);
      mCells[Index * 4 + 2] = (UINT32)(L >> 32);
      mCells[Index * 4 + 3] = (UINT32)(L & 0xffffffffULL);
    }

    if (Total < MIN_TOTAL || Total > MAX_TOTAL) {
      return EFI_NOT_READY;
    }

    *CountOut = Out;
    *TotalOut = Total;
    Status    = EFI_SUCCESS;
    return Status;
  }
}

EFI_STATUS
PatchMemory (
  IN OUT UINT8                        *Dtb,
  IN     UINTN                         Size,
  IN     CONST EFI_MEMORY_DESCRIPTOR   *Map,
  IN     UINTN                         MapSize,
  IN     UINTN                         DescSize,
  OUT    UINTN                        *RegionCount OPTIONAL,
  OUT    UINT64                       *TotalBytes OPTIONAL
  )
{
  UINT32      TotalSize;
  UINT32      OffStruct;
  UINT32      SizeStruct;
  UINT32      OffStrings;
  UINT32      Magic;
  UINT32      Tag;
  UINT32      PropLen;
  UINT32      NameOff;
  UINTN       Pos;
  UINTN       Depth;
  UINTN       RegPos = 0;
  BOOLEAN     InMemory = FALSE;
  BOOLEAN     HaveReg = FALSE;
  UINTN       Count = 0;
  UINT64      Total = 0;
  UINTN       Index;
  EFI_STATUS  Status;

  CopyMem (&Magic, Dtb + 0x00, 4);
  CopyMem (&TotalSize, Dtb + 0x04, 4);
  CopyMem (&OffStruct, Dtb + 0x08, 4);
  CopyMem (&OffStrings, Dtb + 0x0C, 4);
  CopyMem (&SizeStruct, Dtb + 0x24, 4);
  Magic      = SwapBytes32 (Magic);
  TotalSize  = SwapBytes32 (TotalSize);
  OffStruct  = SwapBytes32 (OffStruct);
  OffStrings = SwapBytes32 (OffStrings);
  SizeStruct = SwapBytes32 (SizeStruct);

  if (Magic != FDT_MAGIC) {
    Print (L"PatchMemory: not a device tree (magic %x)\n", Magic);
    return EFI_VOLUME_CORRUPTED;
  }
  if (TotalSize > Size || OffStruct + SizeStruct > Size) {
    Print (L"PatchMemory: device tree header describes %x bytes of "
           L"structure at %x, beyond the file (%lx)\n",
           SizeStruct, OffStruct, (UINT64)Size);
    return EFI_VOLUME_CORRUPTED;
  }

  /*
   * Walk to the depth-1 memory node's reg first: structural refusals come
   * before synthesis, so a tree that cannot be repaired is reported as
   * such even when the synthesized map would have been fine.
   */
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
          if (PropLen != REG_BYTES) {
            Print (L"PatchMemory: /memory reg holds %u bytes, the platform "
                   L"map needs %u; this device tree cannot be repaired "
                   L"in place\n", PropLen, (UINT32)REG_BYTES);
            return EFI_BUFFER_TOO_SMALL;
          }
          if (Pos + PropLen > OffStruct + SizeStruct) {
            Print (L"PatchMemory: /memory reg runs past the structure "
                   L"block\n");
            return EFI_VOLUME_CORRUPTED;
          }
          RegPos  = Pos;
          HaveReg = TRUE;
        }
      }
      Pos += ALIGN_VALUE (PropLen, 4);
      break;

    case FDT_NOP:
      break;

    case FDT_END:
      if (!HaveReg) {
        Print (L"PatchMemory: device tree has no /memory reg to fill in\n");
        return EFI_NOT_FOUND;
      }
      Pos = OffStruct + SizeStruct;   /* leave the loop */
      break;

    default:
      Print (L"PatchMemory: unexpected FDT token %x at %lx\n",
             Tag, (UINT64)(Pos - 4));
      return EFI_VOLUME_CORRUPTED;
    }
  }
  if (!HaveReg) {
    Print (L"PatchMemory: ran off the end of the device tree structure\n");
    return EFI_VOLUME_CORRUPTED;
  }

  Status = SynthesizeMap (Map, MapSize, DescSize, &Count, &Total);
  if (Status != EFI_SUCCESS) {
    Print (L"PatchMemory: memory map synthesis not ready; keeping the "
           L"device tree's own map\n");
    return EFI_NOT_READY;
  }

  for (Index = 0; Index < REG_BYTES / 4; Index++) {
    UINT32  Value = (Index < Count * 4) ? mCells[Index] : 0;
    UINT32  Be32  = SwapBytes32 (Value);

    CopyMem (Dtb + RegPos + Index * 4, &Be32, sizeof (Be32));
  }

  Print (L"PatchMemory: /memory filled: %lu regions, %lu bytes\n",
         (UINT64)Count, Total);
  if (RegionCount != NULL) {
    *RegionCount = Count;
  }
  if (TotalBytes != NULL) {
    *TotalBytes = Total;
  }
  return EFI_SUCCESS;
}
