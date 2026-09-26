/*
 * Host tests for PatchMemory, the loader-side /memory fill that imitates what
 * ABL does when it loads a kernel. Written before the implementation exists:
 * this file is the specification.
 *
 * Ground truth is the DRAM map the vendor bootloader writes on this platform,
 * captured from a known-good device tree (21 regions, 84 cells). The
 * production code must write exactly those bytes, refuse device trees it
 * cannot fix in place, and leave everything else in the buffer untouched.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Uefi.h>
#include <Library/BaseLib.h>
#include "PatchMemory.h"

#define FDT_MAGIC 0xd00dfeedu

/* The known-good kaanapali DRAM map, as read from the good device tree. */
STATIC CONST UINT32 mGoldenMap[84] = {
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

STATIC VOID
GoldenBeBytes (UINT8 *Out)
{
  UINTN Index;

  for (Index = 0; Index < 84; Index++) {
    UINT32 Be = SwapBytes32 (mGoldenMap[Index]);
    memcpy (Out + Index * 4, &Be, 4);
  }
}

/*
 * Build the smallest device tree that has the shape PatchMemory cares about:
 * a root node at depth 0, optionally a memory@a0000000 child carrying a reg
 * property of RegBytes bytes. Returns the total size and, when asked, the
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

STATIC VOID
TestFillsWrongContentReg (VOID)
{
  UINT8    B[512];
  UINT8    Before[512];
  UINT8    Golden[336];
  UINT8    Empty[336];
  UINTN    Size;
  UINTN    RegOff = 0;
  EFI_STATUS Status;

  memset (Empty, 0, sizeof (Empty));
  GoldenBeBytes (Golden);
  Size = BuildDtb (B, sizeof (B), TRUE, Empty, sizeof (Empty), &RegOff);
  memcpy (Before, B, Size);

  Status = PatchMemory (B, Size);
  CHECK (Status == EFI_SUCCESS, "zeroed 336-byte reg is filled and succeeds");
  CHECK (memcmp (B + RegOff, Golden, 336) == 0,
         "reg becomes the known-good 21-region map");
}

STATIC VOID
TestLeavesEverythingElseAlone (VOID)
{
  UINT8    B[512];
  UINT8    Before[512];
  UINT8    Empty[336];
  UINTN    Size;
  UINTN    RegOff = 0;
  UINTN    Index;
  BOOLEAN  OnlyRegChanged = TRUE;
  EFI_STATUS Status;

  memset (Empty, 0, sizeof (Empty));
  Size = BuildDtb (B, sizeof (B), TRUE, Empty, sizeof (Empty), &RegOff);
  memcpy (Before, B, Size);

  Status = PatchMemory (B, Size);
  CHECK (Status == EFI_SUCCESS, "second behavior: patch succeeds");
  for (Index = 0; Index < Size; Index++) {
    if (Index >= RegOff && Index < RegOff + 336) {
      continue;
    }
    if (B[Index] != Before[Index]) {
      OnlyRegChanged = FALSE;
    }
  }
  CHECK (OnlyRegChanged, "only the reg value bytes change");
}

STATIC VOID
TestIdempotentOnGoodMap (VOID)
{
  UINT8    B[512];
  UINT8    Golden[336];
  UINTN    Size;
  UINTN    RegOff = 0;
  EFI_STATUS Status;

  GoldenBeBytes (Golden);
  Size = BuildDtb (B, sizeof (B), TRUE, Golden, sizeof (Golden), &RegOff);

  Status = PatchMemory (B, Size);
  CHECK (Status == EFI_SUCCESS, "already-good map still succeeds");
  CHECK (memcmp (B + RegOff, Golden, 336) == 0, "already-good map unchanged");
}

STATIC VOID
TestRefusesShortReg (VOID)
{
  UINT8    B[512];
  UINT8    Placeholder[16];
  UINT8    Before[512];
  UINTN    Size;
  UINTN    RegOff = 0;
  EFI_STATUS Status;

  /* The original placeholder: a two-cell address with size zero. */
  memset (Placeholder, 0, sizeof (Placeholder));
  Put32 (Placeholder + 4, 0xa0000000);

  Size = BuildDtb (B, sizeof (B), TRUE, Placeholder, sizeof (Placeholder), &RegOff);
  memcpy (Before, B, Size);

  Status = PatchMemory (B, Size);
  CHECK (Status == EFI_BUFFER_TOO_SMALL, "short reg is refused");
  CHECK (memcmp (B, Before, Size) == 0, "refusal leaves the buffer untouched");
}

STATIC VOID
TestRefusesMissingMemoryNode (VOID)
{
  UINT8    B[512];
  UINTN    Size;
  EFI_STATUS Status;

  Size = BuildDtb (B, sizeof (B), FALSE, NULL, 0, NULL);
  Status = PatchMemory (B, Size);
  CHECK (Status == EFI_NOT_FOUND, "missing /memory node is refused");
}

STATIC VOID
TestRefusesBadMagic (VOID)
{
  UINT8    B[512];
  UINT8    Empty[336];
  UINTN    Size;
  EFI_STATUS Status;

  memset (Empty, 0, sizeof (Empty));
  Size = BuildDtb (B, sizeof (B), TRUE, Empty, sizeof (Empty), NULL);
  B[0] ^= 0xff;

  Status = PatchMemory (B, Size);
  CHECK (Status == EFI_VOLUME_CORRUPTED, "bad magic is refused");
}

int
main (VOID)
{  TestFillsWrongContentReg ();
  TestLeavesEverythingElseAlone ();
  TestIdempotentOnGoodMap ();
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
