/*
 * Host tests for PatchMemory, the loader-side /memory fill that synthesizes
 * the platform DRAM map from the firmware's GetMemoryMap descriptors and
 * writes it into the device tree the way ABL does. Written before the
 * implementation exists: this file is the specification.
 *
 * Contract:
 *
 *   - Structure (same as ABL-side walking): bad magic -> EFI_VOLUME_CORRUPTED,
 *     no depth-1 memory* node -> EFI_NOT_FOUND, reg not exactly 336 bytes
 *     (21 regions, the fixed-size in-place slot) -> EFI_BUFFER_TOO_SMALL.
 *   - Synthesis: walk Map with DescSize stride; keep RAM-bearing types
 *     (Loader/BootServices/RuntimeServices/Conventional/ACPI reclaim+NVS);
 *     drop Reserved, Unusable, MMIO, PalCode; clip to the two DRAM windows
 *     this platform uses (low 2-4G, high 32-64G); sort and coalesce
 *     contiguous ranges.
 *   - Validation: 1..21 regions, total between 8G and the window capacity;
 *     anything else -> EFI_NOT_READY and the buffer stays untouched (the
 *     caller keeps the device tree's own map).
 *   - On success: big-endian cells for each region, remaining cells of the
 *     336-byte slot zero, and nothing outside the reg value changes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Uefi.h>
#include <Library/BaseLib.h>
#include "PatchMemory.h"

#define FDT_MAGIC 0xd00dfeedu

#define REG_BYTES 336
#define MAX_REGIONS 21
#define MIN_TOTAL (8ULL * 1024 * 1024 * 1024)

/* The 16G unit's stock map, from android-file/fdt.dtb (21 regions, 84 cells). */
STATIC CONST UINT32 mStock16[84] = {
  0x00000000, 0x81960000, 0x00000000, 0x000a0000, 0x00000000, 0x81a60000,
  0x00000000, 0x001a0000, 0x00000000, 0x81cf4000, 0x00000000, 0x0000c000,
  0x00000000, 0x82478000, 0x00000000, 0x00028000, 0x00000000, 0xc4800000,
  0x00000000, 0x13000000, 0x00000000, 0xd79c0000, 0x00000000, 0x00040000,
  0x00000000, 0xd7c00000, 0x00000000, 0x00400000, 0x00000000, 0xd8800000,
  0x00000000, 0x00000000, 0x00000000, 0xe34a0000, 0x00000000, 0x1c360000,
  0x00000008, 0x80000000, 0x00000000, 0x2f6fd000, 0x00000008, 0xb0000000,
  0x00000000, 0x006500000, 0x0000000a, 0x00000000, 0x00000002, 0x00000000,
  0x00000008, 0xc0000000, 0x00000001, 0x40000000, 0x00000000, 0x82600000,
  0x00000000, 0x00100000, 0x00000000, 0x82800000, 0x00000000, 0x002200000,
  0x00000000, 0x8a980000, 0x00000000, 0x00080000, 0x00000000, 0x9959c000,
  0x00000000, 0x00064000, 0x00000000, 0x9ce80000, 0x00000000, 0x02e00000,
  0x00000000, 0x9ffe0000, 0x00000000, 0x00070000, 0x00000000, 0xa0910000,
  0x00000000, 0x00a70000, 0x00000000, 0xa6400000, 0x00000000, 0x1d9d0000,
};

/* The 12G unit's stock map (the historical golden, same shape). */
STATIC CONST UINT32 mStock12[84] = {
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

STATIC INTN gFailures = 0;

#define CHECK(cond, name) do {                                         \
    if (cond) {                                                        \
      printf ("PASS: %s\n", name);                                     \
    } else {                                                           \
      printf ("FAIL: %s\n", name);                                     \
      gFailures++;                                                     \
    }                                                                  \
  } while (0)

STATIC VOID
Put32 (UINT8 *P, UINT32 Value)
{
  P[0] = (UINT8)(Value >> 24);
  P[1] = (UINT8)(Value >> 16);
  P[2] = (UINT8)(Value >> 8);
  P[3] = (UINT8)Value;
}

/*
 * Build the smallest device tree that has the shape PatchMemory cares about:
 * a root node at depth 0, optionally a memory@a0000000 child carrying a reg
 * property of RegBytesLen bytes. Returns the total size and, when asked, the
 * byte offset of the reg value inside the buffer.
 */
STATIC UINTN
BuildDtb (UINT8 *B, UINTN Cap, BOOLEAN WithMemory, CONST UINT8 *RegBytes,
          UINTN RegBytesLen, UINTN *RegValueOff)
{
  UINT8  *Struct;
  UINT8  *P;

  memset (B, 0, Cap);
  Struct = B + 56;
  P      = Struct;

  Put32 (P, 1);                       /* FDT_BEGIN_NODE, root */
  P += 4;
  memcpy (P, "root\0", 5);
  P += 5;
  while (((UINTN)(P - B) % 4) != 0) {
    *P++ = 0;
  }

  if (WithMemory) {
    Put32 (P, 1);
    P += 4;
    memcpy (P, "memory@a0000000\0", 16);
    P += 16;

    Put32 (P, 3);                     /* FDT_PROP device_type */
    P += 4;
    Put32 (P, 7);
    P += 4;
    Put32 (P, 0);
    P += 4;
    memcpy (P, "memory\0", 7);
    P += 7;
    *P++ = 0;

    Put32 (P, 3);                     /* FDT_PROP reg */
    P += 4;
    Put32 (P, (UINT32)RegBytesLen);
    P += 4;
    Put32 (P, 12);                    /* strings: "reg" at 12 */
    P += 4;
    if (RegValueOff != NULL) {
      *RegValueOff = (UINTN)(P - B);
    }
    memcpy (P, RegBytes, RegBytesLen);
    P += RegBytesLen;
    while (((UINTN)(P - B) % 4) != 0) {
      *P++ = 0;
    }

    Put32 (P, 2);                     /* FDT_END_NODE memory */
    P += 4;
  }

  Put32 (P, 2);                       /* FDT_END_NODE root */
  P += 4;
  Put32 (P, 9);                       /* FDT_END */
  P += 4;

  memcpy (P, "device_type\0", 12);    /* strings block */
  memcpy (P + 12, "reg\0", 4);
  P += 16;

  Put32 (B + 0, FDT_MAGIC);
  Put32 (B + 4, (UINT32)(P - B));
  Put32 (B + 8, 56);                  /* off_dt_struct */
  Put32 (B + 16, 40);                 /* off_mem_rsvmap */
  Put32 (B + 20, 17);                 /* version */
  Put32 (B + 24, 16);                 /* last_comp_version */
  Put32 (B + 28, 0);                  /* boot_cpuid_phys */
  Put32 (B + 32, 16);                 /* size_dt_strings */
  Put32 (B + 36, (UINT32)(P - Struct - 16));  /* size_dt_struct */
  Put32 (B + 12, (UINT32)(P - 16 - B));       /* off_dt_strings */
  return (UINTN)(P - B);
}

/* Encode Cells[84] as big-endian bytes, the way the FDT stores them. */
STATIC VOID
CellsToBe (CONST UINT32 *Cells, UINTN Count, UINT8 *Out)
{
  UINTN Index;

  for (Index = 0; Index < Count; Index++) {
    UINT32 Be = SwapBytes32 (Cells[Index]);
    memcpy (Out + Index * 4, &Be, 4);
  }
}

/*
 * One record in a synthetic GetMemoryMap buffer.
 */
typedef struct {
  UINT32  Type;
  UINT64  Start;
  UINT64  Pages;
} DescSpec;

STATIC VOID
WriteDesc (UINT8 *Base, UINTN Index, UINTN DescSize, CONST DescSpec *S)
{
  UINT8                *Rec = Base + Index * DescSize;
  EFI_MEMORY_DESCRIPTOR *D   = (EFI_MEMORY_DESCRIPTOR *)Rec;

  /* Fill the whole record first so stride padding keeps detectable bytes. */
  memset (Rec, 0xAA, DescSize);
  D->Type          = S->Type;
  D->PhysicalStart = S->Start;
  D->VirtualStart  = 0;
  D->NumberOfPages = S->Pages;
  D->Attribute     = 0;
}

STATIC UINTN
WriteDescs (UINT8 *Base, UINTN DescSize, CONST DescSpec *Specs, UINTN Count)
{
  UINTN Index;

  for (Index = 0; Index < Count; Index++) {
    WriteDesc (Base, Index, DescSize, &Specs[Index]);
  }
  return Count * DescSize;
}

/* The synthetic layout both A-tests expect, junk excluded: 8.375 GB. */
STATIC CONST DescSpec mBaseSpecs[] = {
  { EfiConventionalMemory,    0x0000000080000000ULL, 0x10000000ULL / 4096 }, /* 256M low */
  { EfiBootServicesCode,      0x0000000090000000ULL, 0x04000000ULL / 4096 }, /* 64M */
  { EfiConventionalMemory,    0x0000000094000000ULL, 0x04000000ULL / 4096 }, /* 64M, abuts */
  { EfiRuntimeServicesData,   0x0000000800000000ULL, 0x100000000ULL / 4096 }, /* 4G high */
  { EfiConventionalMemory,    0x0000000900001000ULL, 0x100000000ULL / 4096 }, /* 4G high +4K */
};

/* Junk that synthesis must ignore: wrong types and out-of-window RAM. */
STATIC CONST DescSpec mJunkSpecs[] = {
  { EfiReservedMemoryType,    0x00000000a0000000ULL, 0x1000000ULL / 4096 },
  { EfiMemoryMappedIO,        0x00000000a0000000ULL, 0x1000000ULL / 4096 },
  { EfiUnusableMemory,        0x0000000085000000ULL, 0x1000000ULL / 4096 },
  { EfiConventionalMemory,    0x0000000040000000ULL, 0x20000000ULL / 4096 }, /* below low window */
  { EfiConventionalMemory,    0x0000000400000000ULL, 0x40000000ULL / 4096 }, /* between windows */
};

STATIC CONST UINT32 mExpectedBase[84] = {
  /* The three low ranges are contiguous and coalesce into one. */
  0x00000000, 0x80000000, 0x00000000, 0x18000000,
  0x00000008, 0x00000000, 0x00000001, 0x00000000,
  0x00000009, 0x00001000, 0x00000001, 0x00000000,
};

STATIC VOID
TestSynthWritesExpectedRegions (VOID)
{
  UINT8     B[512];
  UINT8     Before[512];
  UINT8     Reg[REG_BYTES];
  UINT8     Expect[REG_BYTES];
  UINT8     Map[5 * 48];
  UINTN     Size;
  UINTN     RegOff = 0;
  UINTN     MapSize;
  UINTN     Regions = 0;
  UINT64    Total = 0;
  EFI_STATUS Status;

  MapSize = WriteDescs (Map, 48, mBaseSpecs, 5);
  memset (Reg, 0, sizeof (Reg));
  Size = BuildDtb (B, sizeof (B), TRUE, Reg, sizeof (Reg), &RegOff);
  memcpy (Before, B, Size);

  Status = PatchMemory (B, Size, (CONST EFI_MEMORY_DESCRIPTOR *)Map, MapSize, 48, &Regions, &Total);
  CHECK (Status == EFI_SUCCESS, "synthetic RAM map patches successfully");
  CHECK (Regions == 3,
         "three regions: contiguous ranges coalesce, the 4K gap does not");
  CHECK (Total == 0x218000000ULL, "total is 8.375 GB");

  memset (Expect, 0, sizeof (Expect));
  CellsToBe (mExpectedBase, 12, Expect);
  CHECK (memcmp (B + RegOff, Expect, REG_BYTES) == 0,
         "reg holds the merged regions followed by zero cells");

  Status = PatchMemory (B, Size, (CONST EFI_MEMORY_DESCRIPTOR *)Map, MapSize, 48, NULL, NULL);
  CHECK (Status == EFI_SUCCESS, "second patch with the same map succeeds");
  CHECK (memcmp (B + RegOff, Expect, REG_BYTES) == 0,
         "second patch is idempotent");

  {
    UINTN  Index;
    BOOLEAN OnlyReg = TRUE;

    for (Index = 0; Index < Size; Index++) {
      if (Index >= RegOff && Index < RegOff + REG_BYTES) {
        continue;
      }
      if (B[Index] != Before[Index]) {
        OnlyReg = FALSE;
      }
    }
    CHECK (OnlyReg, "only the reg value bytes change");
  }
}

STATIC VOID
TestSynthIgnoresJunkAndHonorsStride (VOID)
{
  UINT8     B[512];
  UINT8     Reg[REG_BYTES];
  UINT8     Expect[REG_BYTES];
  UINT8     Map[(5 + 5) * 48];
  UINTN     Size;
  UINTN     RegOff = 0;
  UINTN     MapSize;
  UINTN     Regions = 0;
  UINT64    Total = 0;
  EFI_STATUS Status;

  MapSize  = WriteDescs (Map, 48, mBaseSpecs, 5);
  MapSize += WriteDescs (Map + MapSize, 48, mJunkSpecs, 5);

  memset (Reg, 0, sizeof (Reg));
  Size = BuildDtb (B, sizeof (B), TRUE, Reg, sizeof (Reg), &RegOff);

  Status = PatchMemory (B, Size, (CONST EFI_MEMORY_DESCRIPTOR *)Map, MapSize, 48, &Regions, &Total);
  CHECK (Status == EFI_SUCCESS, "map with junk descriptors still patches");
  CHECK (Regions == 3 && Total == 0x218000000ULL,
         "reserved/mmio/unusable and out-of-window RAM are ignored");

  memset (Expect, 0, sizeof (Expect));
  CellsToBe (mExpectedBase, 12, Expect);
  CHECK (memcmp (B + RegOff, Expect, REG_BYTES) == 0,
         "output identical to the junk-free run (48-byte stride)");
}

/*
 * Reference merge used by the round-trip tests: read the stock cells,
 * drop size-0 entries, sort, coalesce contiguous ranges.
 */
STATIC UINTN
RefMerge (CONST UINT32 *Stock, UINT32 *Out)
{
  UINT64  Start[24];
  UINT64  End[24];
  UINTN   Count = 0;
  UINTN   Index;
  UINTN   OutCount = 0;

  for (Index = 0; Index < 84; Index += 4) {
    UINT64 Addr = ((UINT64)Stock[Index] << 32) | Stock[Index + 1];
    UINT64 Size = ((UINT64)Stock[Index + 2] << 32) | Stock[Index + 3];

    if (Size == 0) {
      continue;
    }
    Start[Count] = Addr;
    End[Count]   = Addr + Size;
    Count++;
  }

  for (Index = 1; Index < Count; Index++) {          /* insertion sort */
    UINT64 S = Start[Index];
    UINT64 E = End[Index];
    UINTN  J = Index;

    while (J > 0 && Start[J - 1] > S) {
      Start[J] = Start[J - 1];
      End[J]   = End[J - 1];
      J--;
    }
    Start[J] = S;
    End[J]   = E;
  }

  for (Index = 0; Index < Count; Index++) {
    if (OutCount > 0 && Start[Index] <= End[OutCount - 1]) {
      if (End[Index] > End[OutCount - 1]) {
        End[OutCount - 1] = End[Index];
      }
      continue;
    }
    Start[OutCount] = Start[Index];
    End[OutCount]   = End[Index];
    OutCount++;
  }

  for (Index = 0; Index < OutCount; Index++) {
    UINT64 S = Start[Index];
    UINT64 L = End[Index] - Start[Index];

    Out[Index * 4]     = (UINT32)(S >> 32);
    Out[Index * 4 + 1] = (UINT32)(S & 0xffffffffULL);
    Out[Index * 4 + 2] = (UINT32)(L >> 32);
    Out[Index * 4 + 3] = (UINT32)(L & 0xffffffffULL);
  }
  return OutCount;
}

STATIC VOID
RoundTrip (CONST UINT32 *Stock, CONST CHAR8 *Name)
{
  UINT8     B[512];
  UINT8     Reg[REG_BYTES];
  UINT8     Expect[REG_BYTES];
  UINT8     Map[24 * 48];
  UINT32    Ref[84];
  UINTN     Size;
  UINTN     RegOff = 0;
  UINTN     MapSize = 0;
  UINTN     Regions = 0;
  UINTN     RefCount;
  UINTN     Index;
  UINT64    Total = 0;
  UINT64    StockTotal = 0;
  EFI_STATUS Status;
  CHAR8     NameBuf[96];

  RefCount = RefMerge (Stock, Ref);
  memset (Expect, 0, sizeof (Expect));
  CellsToBe (Ref, RefCount * 4, Expect);

  for (Index = 0; Index < 84; Index += 4) {
    UINT64 Size64 = ((UINT64)Stock[Index + 2] << 32) | Stock[Index + 3];

    StockTotal += Size64;
  }

  for (Index = 0; Index < 84; Index += 4) {
    UINT64 Addr = ((UINT64)Stock[Index] << 32) | Stock[Index + 1];
    UINT64 Sz   = ((UINT64)Stock[Index + 2] << 32) | Stock[Index + 3];

    if (Sz == 0) {
      continue;
    }
    WriteDesc (Map, MapSize / 48, 48,
               &(DescSpec){ EfiConventionalMemory, Addr, Sz / 4096 });
    MapSize += 48;
  }

  memset (Reg, 0, sizeof (Reg));
  Size = BuildDtb (B, sizeof (B), TRUE, Reg, sizeof (Reg), &RegOff);

  Status = PatchMemory (B, Size, (CONST EFI_MEMORY_DESCRIPTOR *)Map, MapSize, 48, &Regions, &Total);
  snprintf (NameBuf, sizeof (NameBuf), "%s: synthesis succeeds", Name);
  CHECK (Status == EFI_SUCCESS, NameBuf);
  snprintf (NameBuf, sizeof (NameBuf), "%s: total equals the stock total",
            Name);
  CHECK (Total == StockTotal, NameBuf);
  snprintf (NameBuf, sizeof (NameBuf), "%s: region count fits the slot",
            Name);
  CHECK (Regions == RefCount && Regions <= MAX_REGIONS, NameBuf);
  snprintf (NameBuf, sizeof (NameBuf), "%s: cells cover the stock layout",
            Name);
  CHECK (memcmp (B + RegOff, Expect, REG_BYTES) == 0, NameBuf);
}

STATIC VOID
TestRoundTripStock16 (VOID)
{
  RoundTrip (mStock16, "stock16");
}

STATIC VOID
TestRoundTripStock12 (VOID)
{
  RoundTrip (mStock12, "stock12");
}

STATIC VOID
TestNotReadyWhenNoRam (VOID)
{
  UINT8     B[512];
  UINT8     Before[512];
  UINT8     Reg[REG_BYTES];
  UINT8     Map[2 * 48];
  UINTN     Size;
  UINTN     RegOff = 0;
  UINTN     MapSize;
  UINTN     Regions = 0xdead;
  UINT64    Total = 0xdead;
  EFI_STATUS Status;
  DescSpec  Reserved = { EfiReservedMemoryType, 0x80000000ULL,
                         0x40000000ULL / 4096 };

  MapSize = WriteDescs (Map, 48, &Reserved, 1);
  MapSize += WriteDescs (Map + MapSize, 48, &Reserved, 1);

  memset (Reg, 0, sizeof (Reg));
  Size = BuildDtb (B, sizeof (B), TRUE, Reg, sizeof (Reg), &RegOff);
  memcpy (Before, B, Size);

  Status = PatchMemory (B, Size, (CONST EFI_MEMORY_DESCRIPTOR *)Map, MapSize, 48, &Regions, &Total);
  CHECK (Status == EFI_NOT_READY, "all-reserved map is not ready");
  CHECK (memcmp (B, Before, Size) == 0, "buffer untouched on not-ready");
  CHECK (Regions == 0xdead && Total == 0xdead,
         "outputs untouched on not-ready");
}

STATIC VOID
TestNotReadyWhenTooSmall (VOID)
{
  UINT8     B[512];
  UINT8     Before[512];
  UINT8     Reg[REG_BYTES];
  UINT8     Map[48];
  UINTN     Size;
  UINTN     RegOff = 0;
  EFI_STATUS Status;
  DescSpec  OneG = { EfiConventionalMemory, 0x80000000ULL,
                     0x40000000ULL / 4096 };

  WriteDesc (Map, 0, 48, &OneG);
  memset (Reg, 0, sizeof (Reg));
  Size = BuildDtb (B, sizeof (B), TRUE, Reg, sizeof (Reg), &RegOff);
  memcpy (Before, B, Size);

  Status = PatchMemory (B, Size, (CONST EFI_MEMORY_DESCRIPTOR *)Map, 48, 48, NULL, NULL);
  CHECK (Status == EFI_NOT_READY, "1 GB total is refused as not-ready");
  CHECK (memcmp (B, Before, Size) == 0, "buffer untouched when too small");
}

STATIC VOID
TestNotReadyWhenMoreThan21Regions (VOID)
{
  UINT8     B[512];
  UINT8     Before[512];
  UINT8     Reg[REG_BYTES];
  UINT8     Map[22 * 48];
  UINTN     Size;
  UINTN     RegOff = 0;
  UINTN     MapSize = 0;
  UINTN     Index;
  EFI_STATUS Status;

  for (Index = 0; Index < 22; Index++) {
    DescSpec D = { EfiConventionalMemory,
                   0x0000000800000000ULL + Index * (0x20000000ULL + 0x1000),
                   0x20000000ULL / 4096 };

    MapSize += WriteDescs (Map + MapSize, 48, &D, 1);
  }

  memset (Reg, 0, sizeof (Reg));
  Size = BuildDtb (B, sizeof (B), TRUE, Reg, sizeof (Reg), &RegOff);
  memcpy (Before, B, Size);

  Status = PatchMemory (B, Size, (CONST EFI_MEMORY_DESCRIPTOR *)Map, MapSize, 48, NULL, NULL);
  CHECK (Status == EFI_NOT_READY, "22 disjoint regions are not-ready");
  CHECK (memcmp (B, Before, Size) == 0, "buffer untouched when too many");
}

STATIC VOID
TestNotReadyOnEmptyMap (VOID)
{
  UINT8     B[512];
  UINT8     Before[512];
  UINT8     Reg[REG_BYTES];
  UINTN     Size;
  UINTN     RegOff = 0;
  EFI_STATUS Status;

  memset (Reg, 0, sizeof (Reg));
  Size = BuildDtb (B, sizeof (B), TRUE, Reg, sizeof (Reg), &RegOff);
  memcpy (Before, B, Size);

  Status = PatchMemory (B, Size, NULL, 0, 0, NULL, NULL);
  CHECK (Status == EFI_NOT_READY, "empty map is not-ready");
  Status = PatchMemory (B, Size, (CONST EFI_MEMORY_DESCRIPTOR *)B, 48, 0,
                        NULL, NULL);
  CHECK (Status == EFI_NOT_READY, "zero desc size is not-ready");
  CHECK (memcmp (B, Before, Size) == 0, "buffer untouched on empty map");
}

STATIC VOID
TestRefusesShortReg (VOID)
{
  UINT8     B[512];
  UINT8     Placeholder[16];
  UINT8     Before[512];
  UINT8     Map[48];
  UINTN     Size;
  UINTN     RegOff = 0;
  EFI_STATUS Status;
  DescSpec  OneG = { EfiConventionalMemory, 0x80000000ULL,
                     0x40000000ULL / 4096 };

  WriteDesc (Map, 0, 48, &OneG);
  memset (Placeholder, 0, sizeof (Placeholder));
  Put32 (Placeholder + 4, 0xa0000000);

  Size = BuildDtb (B, sizeof (B), TRUE, Placeholder, sizeof (Placeholder),
                   &RegOff);
  memcpy (Before, B, Size);

  Status = PatchMemory (B, Size, (CONST EFI_MEMORY_DESCRIPTOR *)Map, 48, 48, NULL, NULL);
  CHECK (Status == EFI_BUFFER_TOO_SMALL, "short reg is refused");
  CHECK (memcmp (B, Before, Size) == 0, "refusal leaves the buffer untouched");
}

STATIC VOID
TestRefusesMissingMemoryNode (VOID)
{
  UINT8     B[512];
  UINT8     Map[48];
  UINTN     Size;
  EFI_STATUS Status;
  DescSpec  OneG = { EfiConventionalMemory, 0x80000000ULL,
                     0x40000000ULL / 4096 };

  WriteDesc (Map, 0, 48, &OneG);
  Size = BuildDtb (B, sizeof (B), FALSE, NULL, 0, NULL);
  Status = PatchMemory (B, Size, (CONST EFI_MEMORY_DESCRIPTOR *)Map, 48, 48, NULL, NULL);
  CHECK (Status == EFI_NOT_FOUND, "missing /memory node is refused");
}

STATIC VOID
TestRefusesBadMagic (VOID)
{
  UINT8     B[512];
  UINT8     Reg[REG_BYTES];
  UINTN     Size;
  EFI_STATUS Status;

  memset (Reg, 0, sizeof (Reg));
  Size = BuildDtb (B, sizeof (B), TRUE, Reg, sizeof (Reg), NULL);
  B[0] ^= 0xff;

  Status = PatchMemory (B, Size, NULL, 0, 0, NULL, NULL);
  CHECK (Status == EFI_VOLUME_CORRUPTED, "bad magic is refused");
}

int
main (VOID)
{
  TestSynthWritesExpectedRegions ();
  TestSynthIgnoresJunkAndHonorsStride ();
  TestRoundTripStock16 ();
  TestRoundTripStock12 ();
  TestNotReadyWhenNoRam ();
  TestNotReadyWhenTooSmall ();
  TestNotReadyWhenMoreThan21Regions ();
  TestNotReadyOnEmptyMap ();
  TestRefusesShortReg ();
  TestRefusesMissingMemoryNode ();
  TestRefusesBadMagic ();

  if (gFailures != 0) {
    printf ("%ld test(s) failed\n", (long)gFailures);
    return 1;
  }
  printf ("all patch memory tests passed\n");
  return 0;
}
