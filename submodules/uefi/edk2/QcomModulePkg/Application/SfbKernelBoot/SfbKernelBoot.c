/*
 * Start a Linux kernel the way ABL does.
 *
 * The BDS can load a kernel as an EFI application, but that asks the kernel's
 * EFI stub to perform the handoff, and the stub needs a device tree. ABL never
 * publishes one in the EFI configuration table, so there is none to find and the
 * kernel never comes up -- it fails without a console and the watchdog resets
 * the board.
 *
 * ABL's own path does not need one published. It loads the kernel image to a
 * fixed address, exits boot services, disables the MMU and caches, and branches
 * to the image with the device tree in x0, which is exactly what the arm64 boot
 * protocol asks for. This application does the same thing, with the mainline
 * device tree rather than the one ABL patches for Android.
 *
 * Reference: QcomModulePkg/Library/BootLib/BootLinux.c (the end of BootLinux)
 * and Library/BootLib/ShutdownServices.c (ShutdownUefiBootServices,
 * PreparePlatformHardware).
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <Uefi.h>

#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Guid/FileInfo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>

/*
 * The files this application reads, on the volume it was loaded from.
 *
 * These are real file-system paths, not boot-root-relative ones. The BDS scans
 * the ext4 persist partition with its efisp directory as the boot root and
 * prepends that component itself, so the same files appear in BOOTENTRIES
 * without it. This application talks to EFI_FILE_PROTOCOL directly and so has
 * to spell it out.
 */
STATIC CONST CHAR16  mKernelPath[]  = L"\\kb\\kernel";
STATIC CONST CHAR16  mDtbPath[]     = L"\\kb\\dtb";
STATIC CONST CHAR16  mRamdiskPath[] = L"\\kb\\ramdisk";

/*
 * Progress log, written to a plain file on the SFBOOT volume.
 *
 * The firmware's own log is not usable. logfs is only mounted -- the earlier
 * boot-chain BDS owns the flush, and it flushes when the volume is mounted,
 * which is long before this application runs -- so anything printed after that
 * is written nowhere. And the volume the kernel files live on is ext4, whose
 * driver in this build is read-only.
 *
 * The SFBOOT volume is the one writable FAT volume on the device. It is not in
 * the BDS's scan set (the BDS classifies only FAT32, and this is FAT16), so it
 * has no file system bound when this runs; the driver itself handles FAT12,
 * FAT16 and FAT32, so connecting it is enough.
 *
 * Every entry is flushed and closed immediately, so a reset cannot lose what
 * has already been written.
 */
STATIC CONST CHAR16  mLogPath[] = L"\\kb\\last.txt";

/*
 * Find the kernel volume, bind a file system to it, and confirm it can be
 * written by creating and removing a probe file.
 *
 * The label is what identifies it. Size would not: metadata, dsp_a and
 * oplusreserve* are all in the same range, and picking one of those by accident
 * would put a file on a partition that has nothing to do with this.
 */
STATIC
EFI_STATUS
OpenKernelVolume (
  OUT EFI_FILE_PROTOCOL  **Root
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles = NULL;
  UINTN       Count = 0;
  UINTN       Index;

  *Root = NULL;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid,
                                    NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || Handles == NULL) {
    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    EFI_PARTITION_ENTRY              *Part = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs = NULL;
    EFI_FILE_PROTOCOL                *Probe = NULL;

    /*
     * Ask for the GPT entry, exactly as the BDS does when it hunts for logfs.
     *
     * PartitionName is the *partition* name -- "recovery_b" -- not the file
     * system label, which is "KBREC". They are set by different tools and
     * matching the wrong one silently never hits. The partition name is also
     * the easier of the two to read: it needs no media access at all, which
     * matters here because this device's logical sector size is 4096 and the
     * media cannot be read in 512-byte pieces.
     */
    Status = gBS->HandleProtocol (Handles[Index], &gEfiPartitionRecordGuid,
                                  (VOID **)&Part);
    if (EFI_ERROR (Status) || Part == NULL) {
      continue;
    }

    Print (L"SfbKernelBoot: partition '%s'\n", Part->PartitionName);

    if (StrnCmp (Part->PartitionName, L"recovery_b", 10) != 0) {
      continue;
    }

    Print (L"SfbKernelBoot: kernel volume at handle %u\n", (UINT32)Index);

    /* The BDS connected everything once at start-up; nothing has bound this
     * volume since, so ask for it explicitly. */
    gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);

    Status = gBS->HandleProtocol (Handles[Index],
                                 &gEfiSimpleFileSystemProtocolGuid,
                                 (VOID **)&Fs);
    if (EFI_ERROR (Status) || Fs == NULL) {
      Print (L"SfbKernelBoot: no file system bound to it (%r)\n", Status);
      continue;
    }

    Status = Fs->OpenVolume (Fs, Root);
    if (EFI_ERROR (Status)) {
      Print (L"SfbKernelBoot: OpenVolume -> %r\n", Status);
      continue;
    }
    Print (L"SfbKernelBoot: kernel volume mounted\n");

    /* Prove it is writable before relying on it for the log. */
    Status = (*Root)->Open (*Root, &Probe, L"\\kbprobe.tmp",
                            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                            EFI_FILE_MODE_CREATE, 0);
    if (!EFI_ERROR (Status) && Probe != NULL) {
      Probe->Delete (Probe);
      Print (L"SfbKernelBoot: kernel volume is writable\n");
      FreePool (Handles);
      return EFI_SUCCESS;
    }

    Print (L"SfbKernelBoot: kernel volume is read-only (%r)\n", Status);
    (*Root)->Close (*Root);
    *Root = NULL;
  }

  FreePool (Handles);
  return EFI_NOT_FOUND;
}

STATIC
VOID
LogProgress (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR8        *Line
  );

/*
 * Read back the markers a previous kernel boot left behind.
 *
 * The kernel writes a stage number, a value and a magic word to a fixed
 * physical address as it passes each step of its early entry code (see
 * arch/arm64/include/asm/early_markers.h). Nothing clears that address, and
 * DRAM survives a warm reset, so whatever the last attempt recorded is still
 * there now. This is the only view into the kernel's first instructions: there
 * is no console that early, and ramoops registers far too late to catch a
 * failure at that point.
 *
 * The magic is written last, so a valid magic means the whole mark landed.
 * Reading a stale mark from an older boot is possible in principle; the stage
 * and value are what matter, and they are logged with everything else.
 */
#define EARLY_MARK_BASE   0xB7000000ULL
#define EARLY_MARK_MAGIC  0x4541524c594d4152ULL

/*
 * A second address, a page above, for this application's own mark. It has to
 * be separate: the reader clears the kernel's mark each boot and the kernel
 * then writes its own, so a self-mark left in the same slot would be gone by
 * the time anyone looked.
 */
#define SELF_MARK_BASE    0xB700F000ULL
#define SELF_MARK_MAGIC   0x53454c464d41524bULL   /* "SELFMARK" */

STATIC
VOID
ReadEarlyMarkers (
  IN EFI_FILE_PROTOCOL  *Root
  )
{
  volatile UINT64  *Mark = (volatile UINT64 *)(UINTN)EARLY_MARK_BASE;
  CHAR8            Line[128];
  UINT64           Stage;
  UINT64           Value;
  UINT64           Magic;

  Stage = Mark[0];
  Value = Mark[1];
  Magic = Mark[2];

  if (Magic != EARLY_MARK_MAGIC) {
    AsciiSPrint (Line, sizeof (Line),
                 "SfbKernelBoot: no early marker (stage %lx val %lx magic %lx)",
                 Stage, Value, Magic);
    LogProgress (Root, Line);
  } else {
    AsciiSPrint (Line, sizeof (Line),
                 "SfbKernelBoot: last kernel stage %lx, value %lx",
                 Stage, Value);
    LogProgress (Root, Line);
    Print (L"SfbKernelBoot: last kernel stage %lx, value %lx\n", Stage, Value);

    /* Clear it so the next attempt cannot be confused by this one. */
    Mark[0] = 0;
    Mark[1] = 0;
    Mark[2] = 0;
  }

  /*
   * Then this application's own mark, which proves the channel itself works:
   * it was written by the previous run, survived that run's reset, and is
   * being read back now. Without it, "the kernel wrote nothing" and "nothing
   * written here can be read back" look identical.
   */
  {
    volatile UINT64  *Self = (volatile UINT64 *)(UINTN)SELF_MARK_BASE;

    if (Self[2] == SELF_MARK_MAGIC) {
      AsciiSPrint (Line, sizeof (Line),
                   "SfbKernelBoot: self-mark survived: stage %lx val %lx",
                   Self[0], Self[1]);
      LogProgress (Root, Line);
    } else {
      AsciiSPrint (Line, sizeof (Line),
                   "SfbKernelBoot: self-mark NOT readable (magic %lx)",
                   Self[2]);
      LogProgress (Root, Line);
    }
  }

  if (Magic != EARLY_MARK_MAGIC) {
    return;
  }

  AsciiSPrint (Line, sizeof (Line),
               "SfbKernelBoot: last kernel stage %lx, value %lx",
               Stage, Value);
  LogProgress (Root, Line);
  Print (L"SfbKernelBoot: last kernel stage %lx, value %lx\n", Stage, Value);

  /* Clear it so the next attempt cannot be confused by this one. */
  Mark[0] = 0;
  Mark[1] = 0;
  Mark[2] = 0;
}


STATIC
VOID
LogProgress (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR8        *Line
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File = NULL;
  UINTN              Len;

  if (Root == NULL) {
    return;
  }

  Status = Root->Open (Root, &File, (CHAR16 *)mLogPath,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                       EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR (Status) || File == NULL) {
    return;
  }

  /*
   * Append, do not truncate. The first version truncated on every entry, which
   * left exactly one line -- the last one -- and threw away the sequence that
   * says how it got there. A file that grows by a few hundred bytes per boot on
   * a 100 MB volume costs nothing, and it means a failed attempt can be read
   * back whole.
   */
  File->SetPosition (File, 0xFFFFFFFFFFFFFFFFULL);

  Len    = AsciiStrLen (Line);
  Status = File->Write (File, &Len, (VOID *)Line);
  if (!EFI_ERROR (Status)) {
    File->Flush (File);
  }
  File->Close (File);
}

/*
 * Command line for the first experiment. Kept short on purpose: the point is to
 * prove the handoff, and every extra parameter is another thing that can be
 * wrong. earlycon and a high loglevel are the two that make a failure visible.
 *
 * console=tty0 is what makes them visible here: there is no UART on this board,
 * so a serial console goes nowhere. The device tree's /chosen carries a
 * simple-framebuffer node and the kernel is built with CONFIG_FB_SIMPLE, so a
 * frame buffer console exists as soon as that driver binds -- late, but it
 * replays the log buffer, so the whole boot appears on the panel at once.
 *
 * The ramoops settings are the other channel, and the one that works when the
 * kernel dies before any console exists: the zones live in DRAM at an address
 * outside the memory the device tree describes, so nothing else uses it.
 * memmap reserves it so the kernel does not either. panic=5 makes a panic
 * reboot -- a warm reset, which is what leaves the zones readable -- and
 * max_reason=4 lets the dump happen for every reason pstore knows about.
 */
STATIC CONST CHAR8  mCmdline[] =
  "root=PARTUUID=4B3A4040-F3F4-411C-B61B-D9783E4A9E25 earlycon "
  "console=tty0 loglevel=8 log_buf_len=16M panic=5 clk_ignore_unused "
  "pd_ignore_unused memmap=4M$0xB8000000 "
  "ramoops.mem_address=0xB8000000 ramoops.mem_size=0x400000 "
  "ramoops.record_size=0x40000 ramoops.console_size=0x200000 "
  "ramoops.max_reason=4";

/*
 * The arm64 image header, per Documentation/arm64/booting.rst. The kernel here
 * begins with an MZ stub -- it carries a PE header at offset 0x40 for the EFI
 * path -- so the file starts with code0/code1 and the header fields follow
 * immediately:
 *
 *   0x00 code0   0x04 code1   0x08 text_offset   0x10 image_size
 *   0x18 flags   0x20..0x34 reserved            0x38 magic
 *
 * Getting these wrong is not subtle: reading the magic from 0x3c lands on the
 * PE header offset, and reading text_offset from 0x10 lands on image_size.
 */
#define ARM64_HDR_TEXT_OFFSET  0x08
#define ARM64_HDR_IMAGE_SIZE   0x10
#define ARM64_HDR_FLAGS        0x18
#define ARM64_HDR_MAGIC        0x38U
#define ARM64_IMAGE_MAGIC      0x644D5241U   /* "ARM\x64" */

/* Flattened device tree header. */
#define FDT_MAGIC          0xD00DFEEDU
#define FDT_BEGIN_NODE     0x00000001U
#define FDT_END_NODE       0x00000002U
#define FDT_PROP           0x00000003U
#define FDT_NOP            0x00000004U
#define FDT_END            0x00000009U

/* ABL's device-tree slot size, from BootLinux.c: DT_SIZE_2MB. */
#define DT_SLOT_SIZE       (2U * 1024U * 1024U)

/* Upper bound on any of the three files; the kernel alone is 24 MB. */
#define FILE_MAX           (64U * 1024U * 1024U)

/*
 * Build-time handoff canary.  Off for a real boot; see CanaryBlink below.
 *
 *   -DSFB_CANARY=1            enable the canary instead of jumping to a kernel
 *   -DSFB_CANARY_FB=0x...     override the frame buffer address
 */
#ifndef SFB_CANARY
#define SFB_CANARY         0
#endif

/*
 * The live frame buffer is the splash region at 0xFC800000, 5088 stride x 2772
 * lines x 32bpp, so 0xD73000 bytes of visible pixels.  The region is larger
 * (0x2B00000); only the visible part is touched.
 */
#ifndef SFB_CANARY_FB
#define SFB_CANARY_FB           0xFC800000ULL
#endif
#define CANARY_FB_PIXELS        0x360000U
#define CANARY_PRELUDE_ON       0xFF00FFFFU   /* a8r8g8b8: cyan */
#define CANARY_PRELUDE_OFF      0xFF000000U   /* a8r8g8b8: opaque black */
#define CANARY_ON               0xFFFF00FFU   /* a8r8g8b8: magenta */
#define CANARY_OFF              0xFF000000U   /* a8r8g8b8: opaque black */
#define CANARY_PRELUDE_ROUNDS   4U
#define CANARY_SPIN_FAST        0x20000000U   /* caches on */
#define CANARY_SPIN_SLOW        0x02000000U   /* caches off, device memory */

typedef struct {
  UINT64  TotalSize;
  UINT64  ImageSize;
  UINT64  TextOffset;
  UINT32  Flags;
} KERNEL_IMAGE_INFO;

STATIC
VOID
Fail (
  IN CONST CHAR16  *What,
  IN EFI_STATUS     Status
  )
{
  Print (L"SfbKernelBoot: %s failed: %r\n", What, Status);
}

/* ---- file access -------------------------------------------------------- */

STATIC
EFI_STATUS
ReadWholeFile (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *Path,
  OUT VOID               **Buffer,
  OUT UINTN              *Size
  )
{
  EFI_STATUS        Status;
  EFI_FILE_PROTOCOL *File = NULL;
  EFI_FILE_INFO     *Info = NULL;
  UINTN             InfoSize = 0;
  UINTN             Want;
  VOID              *Data = NULL;

  *Buffer = NULL;
  *Size   = 0;

  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    Fail (Path, Status);
    return Status;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    Fail (L"GetInfo(size)", Status);
    goto Done;
  }
  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) {
    Fail (L"GetInfo", Status);
    goto Done;
  }

  if (Info->FileSize == 0 || Info->FileSize > FILE_MAX) {
    Print (L"SfbKernelBoot: %s has an implausible size %lu\n",
           Path, (UINT64)Info->FileSize);
    Status = EFI_LOAD_ERROR;
    goto Done;
  }

  Data = AllocatePool ((UINTN)Info->FileSize);
  if (Data == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  Want   = (UINTN)Info->FileSize;
  Status = File->Read (File, &Want, Data);
  if (EFI_ERROR (Status)) {
    Fail (L"Read", Status);
    goto Done;
  }
  if (Want != (UINTN)Info->FileSize) {
    Print (L"SfbKernelBoot: %s: short read, %lu of %lu\n",
           Path, (UINT64)Want, (UINT64)Info->FileSize);
    Status = EFI_LOAD_ERROR;
    goto Done;
  }

  *Buffer = Data;
  *Size   = Want;
  Data    = NULL;
  Status  = EFI_SUCCESS;

Done:
  if (Data != NULL) {
    FreePool (Data);
  }
  if (Info != NULL) {
    FreePool (Info);
  }
  if (File != NULL) {
    File->Close (File);
  }
  return Status;
}

/* ---- validation --------------------------------------------------------- */

STATIC
BOOLEAN
CheckKernelImage (
  IN  CONST UINT8     *Image,
  IN  UINTN            Size,
  OUT KERNEL_IMAGE_INFO *Info
  )
{
  UINT32  Magic;
  UINT32  Flags;

  ZeroMem (Info, sizeof (*Info));

  if (Size < ARM64_HDR_MAGIC + 4) {
    Print (L"SfbKernelBoot: kernel is too small to hold an arm64 header\n");
    return FALSE;
  }

  Magic = *(UINT32 *)(Image + ARM64_HDR_MAGIC);
  if (Magic != ARM64_IMAGE_MAGIC) {
    Print (L"SfbKernelBoot: kernel magic %08x, want %08x (Image.gz?)\n",
           Magic, ARM64_IMAGE_MAGIC);
    return FALSE;
  }

  CopyMem (&Info->TextOffset, Image + ARM64_HDR_TEXT_OFFSET, sizeof (UINT64));
  CopyMem (&Info->ImageSize,  Image + ARM64_HDR_IMAGE_SIZE,  sizeof (UINT64));
  CopyMem (&Flags,            Image + ARM64_HDR_FLAGS,       sizeof (UINT32));
  Info->Flags     = Flags;
  Info->TotalSize = Size;

  Print (L"SfbKernelBoot: kernel header: text_offset=%lx image_size=%lx "
         L"flags=%x\n",
         Info->TextOffset, Info->ImageSize, Flags);

  if (Info->ImageSize == 0) {
    Print (L"SfbKernelBoot: image_size is zero; refusing\n");
    return FALSE;
  }
  /*
   * image_size is the "effective image size" -- the span to reserve -- and the
   * boot protocol says it must be respected. It is normally a little larger
   * than the file, the difference being BSS. A value smaller than the file
   * would mean the header is not describing this image, and placing anything
   * after the kernel would then land inside it.
   */
  if (Info->ImageSize < Size) {
    Print (L"SfbKernelBoot: image_size %lx is smaller than the file (%lx)\n",
           Info->ImageSize, (UINT64)Size);
    return FALSE;
  }
  if (Info->ImageSize > Size + 16 * 1024 * 1024) {
    Print (L"SfbKernelBoot: image_size %lx is implausibly larger than the "
           L"file (%lx)\n", Info->ImageSize, (UINT64)Size);
    return FALSE;
  }
  if (Info->TextOffset != 0) {
    Print (L"SfbKernelBoot: text_offset is %lx, not 0; the image is not\n"
           L"               position independent and must go at that offset\n",
           Info->TextOffset);
    return FALSE;
  }
  /*
   * flags: bit 0 endianness, bits 1-2 page size (1 = 4K), bit 3 physical
   * placement, bits 4-63 reserved. Placement bit 3 of 1 means the image only
   * has to lie inside the 48-bit addressable range, which is the case here;
   * the code below aligns to 2 MB regardless, so either value is satisfied.
   */
  if ((Flags & 0x1) != 0) {
    Print (L"SfbKernelBoot: flags %x declare a big-endian kernel\n", Flags);
    return FALSE;
  }
  if (((Flags >> 1) & 0x3) != 1) {
    Print (L"SfbKernelBoot: flags %x do not declare 4 KB pages\n", Flags);
    return FALSE;
  }
  if ((Flags & ~0xF) != 0) {
    Print (L"SfbKernelBoot: flags %x set reserved bits 4-63\n", Flags);
    return FALSE;
  }

  return TRUE;
}

STATIC
BOOLEAN
CheckDtb (
  IN CONST UINT8  *Dtb,
  IN UINTN         Size
  )
{
  UINT32  Magic;

  if (Size < 40) {
    Print (L"SfbKernelBoot: device tree too small\n");
    return FALSE;
  }
  /*
   * Everything in a flattened device tree is big-endian, on a little-endian
   * machine included. Reading these straight gives byte-swapped nonsense: the
   * magic comes out as 0xEDFE0DD0 instead of 0xD00DFEED.
   */
  CopyMem (&Magic, Dtb, sizeof (Magic));
  Magic = SwapBytes32 (Magic);
  if (Magic != FDT_MAGIC) {
    Print (L"SfbKernelBoot: device tree magic %08x, want %08x\n",
           Magic, FDT_MAGIC);
    return FALSE;
  }
  return TRUE;
}

/* ---- device tree -------------------------------------------------------- */

/*
 * Rewrite /chosen's handoff properties in place.
 *
 * Three of them, and all three matter:
 *
 *   bootargs           the command line, a string
 *   linux,initrd-start  where the ramdisk is, a u64
 *   linux,initrd-end    where it ends, a u64
 *
 * The kernel reads the command line from bootargs and the ramdisk from the two
 * initrd properties (drivers/of/fdt.c, early_init_dt_check_for_initrd). Without
 * the initrd pair the kernel does not know the ramdisk exists and stops before
 * it can print anything -- which is indistinguishable, from the outside, from
 * never having started at all.
 *
 * All three are put there beforehand, on the host, with room to spare. That is
 * what makes this a fixed-size field update: the structure block does not grow,
 * the string table does not move, no header offset changes, and every later
 * walk of the tree still sees the same offsets. Replacing the values in place
 * and padding with zeros leaves the property length honest.
 */
STATIC
EFI_STATUS
PatchChosen (
  IN OUT UINT8   *Dtb,
  IN UINTN        Size,
  IN CONST CHAR8 *Cmdline,
  IN UINT64       RamdiskStart,
  IN UINT64       RamdiskEnd
  )
{
  UINT32        TotalSize;
  UINT32        OffStruct;
  UINT32        SizeStruct;
  UINT32        OffStrings;
  UINTN         Pos;
  UINTN         Depth;
  UINT32        Tag;
  UINT32        PropLen;
  UINT32        NameOff;
  BOOLEAN       InChosen = FALSE;
  UINTN         Patched  = 0;
  UINTN         CmdLen;
  UINT64        Be64;

  /* Big-endian, as everything in an FDT is. */
  CopyMem (&TotalSize,  Dtb + 0x04, 4);
  CopyMem (&OffStruct,  Dtb + 0x08, 4);
  CopyMem (&SizeStruct, Dtb + 0x24, 4);
  CopyMem (&OffStrings, Dtb + 0x0C, 4);
  TotalSize  = SwapBytes32 (TotalSize);
  OffStruct  = SwapBytes32 (OffStruct);
  SizeStruct = SwapBytes32 (SizeStruct);
  OffStrings = SwapBytes32 (OffStrings);

  if (TotalSize > Size || OffStruct + SizeStruct > Size) {
    Print (L"SfbKernelBoot: device tree header describes %x bytes of "
           L"structure at %x, beyond the file (%lx)\n",
           SizeStruct, OffStruct, (UINT64)Size);
    return EFI_VOLUME_CORRUPTED;
  }

  CmdLen = AsciiStrLen (Cmdline) + 1;

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
         * The chosen node sits at depth 1 under the root. Its name may carry a
         * unit address, so compare the stem only.
         */
        if (Depth == 1 && AsciiStrnCmp (Name, "chosen", 6) == 0 &&
            (Name[6] == '\0' || Name[6] == '@')) {
          InChosen = TRUE;
        }
        Depth++;
        Pos += ALIGN_VALUE (NameLen, 4);
      }
      break;

    case FDT_END_NODE:
      if (Depth == 2) {
        InChosen = FALSE;
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

      if (InChosen && OffStrings + NameOff < Size) {
        CONST CHAR8  *PropName = (CONST CHAR8 *)(Dtb + OffStrings + NameOff);

        if (AsciiStrCmp (PropName, "bootargs") == 0) {
          if (PropLen < CmdLen) {
            Print (L"SfbKernelBoot: bootargs holds %u bytes, the command line "
                   L"needs %lu; the host-side placeholder is too small\n",
                   PropLen, (UINT64)CmdLen);
            return EFI_BUFFER_TOO_SMALL;
          }
          CopyMem (Dtb + Pos, (VOID *)Cmdline, CmdLen);
          if (CmdLen < PropLen) {
            SetMem (Dtb + Pos + CmdLen, PropLen - CmdLen, 0);
          }
          Print (L"SfbKernelBoot: bootargs: %u bytes available, %lu used\n",
                 PropLen, (UINT64)CmdLen);
          Patched++;
        } else if (AsciiStrCmp (PropName, "linux,initrd-start") == 0) {
          if (PropLen != sizeof (UINT64)) {
            Print (L"SfbKernelBoot: linux,initrd-start is %u bytes, expected "
                   L"8\n", PropLen);
            return EFI_VOLUME_CORRUPTED;
          }
          Be64 = SwapBytes64 (RamdiskStart);
          CopyMem (Dtb + Pos, &Be64, sizeof (Be64));
          Print (L"SfbKernelBoot: initrd-start = %lx\n", RamdiskStart);
          Patched++;
        } else if (AsciiStrCmp (PropName, "linux,initrd-end") == 0) {
          if (PropLen != sizeof (UINT64)) {
            Print (L"SfbKernelBoot: linux,initrd-end is %u bytes, expected "
                   L"8\n", PropLen);
            return EFI_VOLUME_CORRUPTED;
          }
          Be64 = SwapBytes64 (RamdiskEnd);
          CopyMem (Dtb + Pos, &Be64, sizeof (Be64));
          Print (L"SfbKernelBoot: initrd-end   = %lx\n", RamdiskEnd);
          Patched++;
        }
      }
      Pos += ALIGN_VALUE (PropLen, 4);
      break;

    case FDT_NOP:
      break;

    case FDT_END:
      if (Patched != 3) {
        Print (L"SfbKernelBoot: reached the end of the device tree with only "
               L"%lu of the 3 handoff properties patched\n", (UINT64)Patched);
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

/* ---- memory ------------------------------------------------------------- */

/*
 * Reserve one contiguous, 2 MB aligned span holding the kernel, the ramdisk and
 * the device-tree slot, then place all three in it.
 *
 * The allocation is a real one. Writing into a span that the memory map calls
 * free without allocating it first is what the earlier MuChain experiment did
 * with 0xC6900000, and it took the device down: that span was
 * EfiBootServicesData and belonged to something else.
 */
STATIC
EFI_STATUS
PlaceAll (
  IN  UINTN   KernelSize,
  IN  UINTN   RamdiskSize,
  IN  UINTN   DtbSize,
  OUT UINT8   **Base,
  OUT UINTN   *SpanSize
  )
{
  EFI_STATUS            Status;
  EFI_MEMORY_DESCRIPTOR *Map = NULL;
  UINTN                 MapSize = 0;
  UINTN                 MapKey;
  UINTN                 DescSize;
  UINT32                DescVer;
  UINTN                 Count;
  UINTN                 Want;
  UINTN                 Pages;
  UINTN                 Index;
  EFI_PHYSICAL_ADDRESS  At;

  /* Worst case, before alignment: everything, plus the slot and a page each. */
  Want = KernelSize + RamdiskSize + DtbSize + DT_SLOT_SIZE + 4 * EFI_PAGE_SIZE;
  Want = ALIGN_VALUE (Want, SIZE_2MB);

  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVer);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    Fail (L"GetMemoryMap(size)", Status);
    return Status;
  }
  MapSize += 8 * DescSize;
  Map = AllocatePool (MapSize);
  if (Map == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVer);
  if (EFI_ERROR (Status)) {
    Fail (L"GetMemoryMap", Status);
    FreePool (Map);
    return Status;
  }

  Count = MapSize / DescSize;
  Print (L"SfbKernelBoot: looking for %lx bytes, 2 MB aligned, in %lu regions\n",
         (UINT64)Want, (UINT64)Count);

  for (Index = 0; Index < Count; Index++) {
    EFI_MEMORY_DESCRIPTOR  *D;
    UINT64                 Start;
    UINT64                 End;
    UINT64                 Aligned;

    D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index * DescSize);
    if (D->Type != EfiConventionalMemory) {
      continue;
    }

    Start   = D->PhysicalStart;
    End     = Start + EFI_PAGES_TO_SIZE (D->NumberOfPages);
    Aligned = ALIGN_VALUE (Start, (UINT64)SIZE_2MB);
    if (Aligned + Want > End) {
      continue;
    }

    Pages = EFI_SIZE_TO_PAGES (Want);
    At    = Aligned;
    Status = gBS->AllocatePages (AllocateAddress, EfiLoaderData, Pages, &At);
    if (EFI_ERROR (Status)) {
      Print (L"SfbKernelBoot:   %lx-%lx: AllocateAddress at %lx -> %r\n",
             Start, End - 1, Aligned, Status);
      continue;
    }

    Print (L"SfbKernelBoot: reserved %lx bytes at %lx\n",
           (UINT64)Want, (UINT64)At);
    FreePool (Map);

    *Base     = (UINT8 *)(UINTN)At;
    *SpanSize = Want;
    return EFI_SUCCESS;
  }

  Print (L"SfbKernelBoot: no conventional region can hold a %lx byte span\n",
         (UINT64)Want);
  FreePool (Map);
  return EFI_OUT_OF_RESOURCES;
}

/* ---- the last boot's progress records ----------------------------------- */

/*
 * The kernel under test has no console at all in its first seconds, so it
 * leaves a record in its own .data as it passes each step. Those records are
 * still in the memory the last kernel was placed in -- this runs before this
 * boot's kernel is copied over them -- and each one is:
 *
 *   +0  value, whatever that step was carrying
 *   +8  magic | stage, written last so a torn record is not counted
 *
 * Scanning for them rather than reading a fixed address means the kernel does
 * not have to publish where its buffer ended up, and nothing has to arrange a
 * mapping for it on the kernel side either.
 */
#define EARLY_TRACE_MAGIC   0x4541524c59545200ULL   /* "EARLYTR\0" */
#define EARLY_LOG_MAGIC     0x4541524c594c4f47ULL   /* "EARLYLOG" */
#define EARLY_LOG_TEXT      (256 * 1024)
#define EARLY_LOG_HEADER    32                      /* magic, length, snapshots, reason */

STATIC
EFI_STATUS
KlogOpen (
  IN  EFI_FILE_PROTOCOL  *Root,
  OUT EFI_FILE_PROTOCOL  **File
  );

STATIC
VOID
KlogWrite (
  IN EFI_FILE_PROTOCOL  *File,
  IN CONST VOID         *Data,
  IN UINTN               Len
  );

STATIC
VOID
ScanLastImage (
  IN EFI_FILE_PROTOCOL  *Root,
  IN UINT8              *Span,
  IN UINTN               Size
  )
{
  volatile UINT64  *Words = (volatile UINT64 *)Span;
  CHAR8            Line[128];
  UINTN            Stage;
  UINTN            Index;
  UINTN            Seen = 0;

  for (Stage = 1; Stage < 64; Stage++) {
    for (Index = 0; Index + 2 <= Size / 8; Index += 2) {
      if (Words[Index + 1] != (EARLY_TRACE_MAGIC | Stage)) {
        continue;
      }

      AsciiSPrint (Line, sizeof (Line),
                   "SfbKernelBoot: TRACE stage %lu value %lx",
                   (UINT64)Stage, (UINT64)Words[Index]);
      LogProgress (Root, Line);
      Seen++;
      break;
    }
  }

  AsciiSPrint (Line, sizeof (Line),
               "SfbKernelBoot: %lu trace records found in the last kernel image",
               (UINT64)Seen);
  LogProgress (Root, Line);

  /*
   * And the kernel's own log, sitting in the same image: printk's ring buffer
   * copied there by early_log_snapshot(), header first.
   */
  for (Index = 0; Index + 4 <= Size / 8; Index++) {
    EFI_FILE_PROTOCOL  *File = NULL;
    UINT64             Length;
    UINT64             Snapshots;

    if (Words[Index] != EARLY_LOG_MAGIC) {
      continue;
    }

    Length    = Words[Index + 1];
    Snapshots = Words[Index + 2];
    if ((Length == 0) || (Length > EARLY_LOG_TEXT)) {
      continue;
    }

    AsciiSPrint (Line, sizeof (Line),
                 "SfbKernelBoot: kernel log %lu bytes from %lu snapshots",
                 Length, Snapshots);
    LogProgress (Root, Line);

    if (!EFI_ERROR (KlogOpen (Root, &File)) && (File != NULL)) {
      AsciiSPrint (Line, sizeof (Line),
                   "---- kernel log, %lu bytes, %lu snapshots ----\r\n",
                   Length, Snapshots);
      KlogWrite (File, Line, AsciiStrLen (Line));
      KlogWrite (File, (VOID *)(Span + Index * 8 + EARLY_LOG_HEADER),
                 (UINTN)Length);
      File->Flush (File);
      File->Close (File);
    }
    break;
  }
}

/* ---- the previous boot's kernel log ------------------------------------- */

/*
 * The kernel this application starts has no console: the only one it is given
 * is a UART that is not wired up, and a frame buffer console needs a display
 * driver that may never probe. So the log is taken out of the machine instead,
 * through ramoops.
 *
 * The pstore RAM backend is configured by the command line to keep its zones in
 * DRAM at PSTORE_REGION -- which is outside the memory the device tree
 * describes, so the kernel never allocates it, and on the same 4 GB page table
 * mapping as the marker area this application already reads. The kernel writes
 * every printk into the console zone as it happens and dumps the whole ring
 * buffer into the record zones on a panic, and neither is cleared by a reboot.
 * This reads both back and copies them to the volume, where they can be read
 * without a working kernel at all.
 *
 * Layout, from the sizes on the command line: interleaved first, the record
 * zones (PSTORE_MEM / PSTORE_RECORD of them), then the console zone. Each zone
 * is a ring buffer with a twelve byte header:
 *
 *   +0  signature, PERSISTENT_RAM_SIG xor the zone type
 *   +4  start, offset of the oldest byte
 *   +8  size, number of valid bytes
 *   +12 data, wrapping at the end of the zone
 */
#define PSTORE_SIG       0x43474244U   /* PERSISTENT_RAM_SIG, "DBGC" */
#define PSTORE_HDR       12
#define PSTORE_REGION    0xB8000000ULL
#define PSTORE_MEM       0x400000U
#define PSTORE_RECORD    0x40000U
#define PSTORE_CONSOLE   0x200000U
#define KLOG_PATH        L"\\kb\\klog.txt"

STATIC
EFI_STATUS
KlogOpen (
  IN  EFI_FILE_PROTOCOL  *Root,
  OUT EFI_FILE_PROTOCOL  **File
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Old = NULL;

  /* Start from nothing: opening an existing file does not truncate it. */
  Status = Root->Open (Root, &Old, (CHAR16 *)KLOG_PATH,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (!EFI_ERROR (Status) && (Old != NULL)) {
    Old->Delete (Old);
  }

  return Root->Open (Root, File, (CHAR16 *)KLOG_PATH,
                     EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                     EFI_FILE_MODE_CREATE, 0);
}

STATIC
VOID
KlogWrite (
  IN EFI_FILE_PROTOCOL  *File,
  IN CONST VOID         *Data,
  IN UINTN               Len
  )
{
  UINTN  Write = Len;

  if ((File != NULL) && (Len != 0)) {
    File->Write (File, &Write, (VOID *)Data);
  }
}

/* ---- handoff ------------------------------------------------------------ */

#if SFB_CANARY

/*
 * Answer one question: does anything execute after the machine is quiesced?
 *
 * There is no UART on this device and physical memory does not survive a
 * reset, so neither a log nor a marker can report on the far side of
 * ExitBootServices.  The panel can: the display controller keeps scanning out
 * of its reserved buffer, and that buffer is the splash region at 0xFC800000
 * (see the simple-framebuffer node and splash_region in the device tree), which
 * is outside the DRAM the MMU was mapping -- so a physical store reaches it
 * even with the MMU off.
 *
 * The canary runs twice, in two colours, so that one run separates the three
 * possible failures:
 *
 *   cyan, before ExitBootServices   the application reached the handoff at all
 *   magenta, after it              the whole sequence -- boot services gone,
 *                                  caches off, MMU off, TLB invalidated,
 *                                  branch taken -- really did execute
 *
 * Cyan and no magenta means the handoff itself is what fails.  Neither means
 * the application never got this far, which the log will confirm.
 *
 * The address is read from the graphics output protocol rather than assumed:
 * the firmware draws its own console through that protocol, so the buffer is
 * both the live scanout and already mapped by the page tables the application
 * is running on.  The device tree's splash region is only a fallback, and the
 * log records which one was used.
 */
typedef struct {
  UINTN    Base;
  UINTN    Pixels;
} CANARY_FRAMEBUFFER;

STATIC CANARY_FRAMEBUFFER  mCanaryFb = { SFB_CANARY_FB, CANARY_FB_PIXELS };

STATIC
VOID
CanaryPrepare (
  IN EFI_FILE_PROTOCOL  *Root
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop = NULL;
  EFI_STATUS                    Status;
  CHAR8                         Line[160];
  UINT64                        Pixels;

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL,
                                (VOID **)&Gop);
  if (EFI_ERROR (Status) || (Gop == NULL) || (Gop->Mode == NULL) ||
      (Gop->Mode->Info == NULL))
  {
    AsciiSPrint (Line, sizeof (Line),
                 "SfbKernelBoot: canary: no GOP, assuming fb %lx",
                 (UINT64)mCanaryFb.Base);
    LogProgress (Root, Line);
    return;
  }

  Pixels = (UINT64)Gop->Mode->Info->PixelsPerScanLine *
           Gop->Mode->Info->VerticalResolution;
  if ((Pixels * 4) > Gop->Mode->FrameBufferSize) {
    Pixels = Gop->Mode->FrameBufferSize / 4;
  }

  mCanaryFb.Base   = (UINTN)Gop->Mode->FrameBufferBase;
  mCanaryFb.Pixels = (UINTN)Pixels;

  AsciiSPrint (Line, sizeof (Line),
               "SfbKernelBoot: canary fb %lx+%lx %ux%u stride %u fmt %u",
               (UINT64)Gop->Mode->FrameBufferBase,
               (UINT64)Gop->Mode->FrameBufferSize,
               (UINT32)Gop->Mode->Info->HorizontalResolution,
               (UINT32)Gop->Mode->Info->VerticalResolution,
               (UINT32)Gop->Mode->Info->PixelsPerScanLine,
               (UINT32)Gop->Mode->Info->PixelFormat);
  LogProgress (Root, Line);
}

STATIC
VOID
CanaryFill (
  IN UINT32  Colour
  )
{
  volatile UINT32  *Fb = (volatile UINT32 *)(UINTN)mCanaryFb.Base;
  UINTN             Index;

  for (Index = 0; Index < mCanaryFb.Pixels; Index++) {
    Fb[Index] = Colour;
  }
}

STATIC
VOID
CanarySpin (
  IN UINTN  Iterations
  )
{
  volatile UINTN  Spin;

  for (Spin = 0; Spin < Iterations; Spin++) {
    __asm__ __volatile__ ("" ::: "memory");
  }
}

/* Before the firmware is gone: a few cyan flashes, then back to black. */
STATIC
VOID
CanaryPrelude (
  VOID
  )
{
  UINTN  Round;

  for (Round = 0; Round < CANARY_PRELUDE_ROUNDS; Round++) {
    CanaryFill (CANARY_PRELUDE_ON);
    CanarySpin (CANARY_SPIN_FAST);
    CanaryFill (CANARY_PRELUDE_OFF);
    CanarySpin (CANARY_SPIN_FAST);
  }
}

/* After it: magenta against black, forever. */
STATIC
VOID
CanaryBlink (
  VOID
  )
{
  UINTN  Phase;

  for (Phase = 0; ; Phase++) {
    CanaryFill (((Phase & 1) == 0) ? CANARY_ON : CANARY_OFF);

    /*
     * Uncached stores to device memory plus instruction fetches from DRAM with
     * the I-cache off make each of these iterations slower than the count
     * suggests; the point is only that the delay is plainly visible.
     */
    CanarySpin (CANARY_SPIN_SLOW);
  }
}

#endif

/*
 * Leave the firmware, quiesce the machine, and branch.
 *
 * ABL calls ArmLib for this (PreparePlatformHardware), and so does this. The
 * order is the part that matters: write back and invalidate each region while
 * the caches are still enabled, then clean and invalidate, then disable the
 * caches, then the MMU, then the TLB.
 *
 * Nothing here can be undone and nothing can be reported after it, which is why
 * every check this application makes happens before it is called.
 */
STATIC
VOID
ExitAndJump (
  IN VOID  *Kernel,
  IN VOID  *Dtb,
  IN UINTN  KernelSize,
  IN UINTN  RamdiskSize,
  IN VOID  *Ramdisk,
  IN UINTN  DtbSize
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map = NULL;
  UINTN                  MapSize = 0;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVer;
  UINTN                  Pages = 0;

#if SFB_CANARY
  /* Cyan, while the firmware is still alive. */
  CanaryPrelude ();
#endif

  /* --- boot services ---------------------------------------------------- */
  do {
    Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVer);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      if (Map != NULL) {
        FreePages (Map, Pages);
      }
      Pages = EFI_SIZE_TO_PAGES (MapSize) + 1;
      Map   = AllocatePages (Pages);
      if (Map == NULL) {
        Print (L"SfbKernelBoot: cannot allocate the memory map\n");
        CpuDeadLoop ();
      }
      MapSize = EFI_PAGES_TO_SIZE (Pages);
      continue;
    }
    if (EFI_ERROR (Status)) {
      Print (L"SfbKernelBoot: GetMemoryMap -> %r (retrying)\n", Status);
      MapSize = 0;
      continue;
    }

    /* Nothing may allocate between the map and the call that consumes it. */
    Status = gBS->ExitBootServices (gImageHandle, MapKey);
    if (EFI_ERROR (Status)) {
      Print (L"SfbKernelBoot: ExitBootServices -> %r (retrying)\n", Status);
      MapSize = 0;
    }
  } while (EFI_ERROR (Status));

  /* --- no console from here on ------------------------------------------ */

  ArmDisableBranchPrediction ();
  ArmDisableInterrupts ();
  ArmDisableAsynchronousAbort ();

  WriteBackInvalidateDataCacheRange (Kernel,  KernelSize);
  WriteBackInvalidateDataCacheRange (Ramdisk, RamdiskSize);
  WriteBackInvalidateDataCacheRange (Dtb,     DtbSize);

  ArmCleanDataCache ();
  ArmInvalidateInstructionCache ();
  ArmDisableDataCache ();
  ArmDisableInstructionCache ();
  ArmDisableMmu ();
  ArmInvalidateTlb ();

#if SFB_CANARY
  CanaryBlink ();
#endif

  /*
   * x0 = device tree, the rest zero. Masking daif again is belt and braces:
   * the target's exception vectors are not installed yet, so an interrupt
   * taken from here would use whatever vector base was left behind.
   */
  __asm__ __volatile__ (
    "msr   daifset, #0xf\n"
    "mov   x0, %0\n"
    "mov   x1, xzr\n"
    "mov   x2, xzr\n"
    "mov   x3, xzr\n"
    "mov   x16, %1\n"
    "br    x16\n"
    :
    : "r" (Dtb), "r" (Kernel)
    : "x0", "x1", "x2", "x3", "x16", "memory"
    );

  CpuDeadLoop ();
}

/* ---- entry -------------------------------------------------------------- */

EFI_STATUS
EFIAPI
SfbKernelBootEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                       Status;
  EFI_FILE_PROTOCOL                *Root = NULL;
  EFI_FILE_PROTOCOL                *LogRoot = NULL;
  VOID                             *Kernel = NULL;
  UINTN                            KernelSize = 0;
  VOID                             *Dtb = NULL;
  UINTN                            DtbSize = 0;
  VOID                             *Ramdisk = NULL;
  UINTN                            RamdiskSize = 0;
  KERNEL_IMAGE_INFO                Info;
  UINT8                            *Span = NULL;
  UINTN                            SpanSize = 0;
  UINT8                            *DtAt;
  UINT8                            *RdAt;
  UINT8                            *KAt;
  UINTN                            CmdLen;
  UINTN                            Reserve;
  CHAR8                            Line[256];

  Print (L"SfbKernelBoot: starting\n");

  /*
   * --- 1. find the kernel volume ----------------------------------------
   *
   * One volume serves both purposes: it carries the kernel, device tree and
   * ramdisk, and it takes the progress log. It is a FAT volume, so it is
   * writable -- the volume this application is loaded from is ext4, whose
   * driver here is read-only, so a log could not go there.
   *
   * Failure to find it is fatal, unlike a log failure would be: without it
   * there is nothing to boot.
   */
  Status = OpenKernelVolume (&Root);
  if (EFI_ERROR (Status)) {
    Print (L"SfbKernelBoot: no kernel volume (%r)\n", Status);
    return Status;
  }
  LogRoot = Root;
  LogProgress (LogRoot, "---- SfbKernelBoot: starting a new attempt ----");

  /*
   * Read what the last kernel attempt left behind, before anything else runs
   * and before the markers are cleared.
   */
  ReadEarlyMarkers (LogRoot);


  LogProgress (LogRoot, "SfbKernelBoot: kernel volume opened");

  /* --- 2. read and check the three files -------------------------------- */
  Status = ReadWholeFile (Root, mKernelPath, &Kernel, &KernelSize);
  if (EFI_ERROR (Status)) {
    goto Out;
  }
  Status = ReadWholeFile (Root, mDtbPath, &Dtb, &DtbSize);
  if (EFI_ERROR (Status)) {
    goto Out;
  }
  Status = ReadWholeFile (Root, mRamdiskPath, &Ramdisk, &RamdiskSize);
  if (EFI_ERROR (Status)) {
    goto Out;
  }

  Print (L"SfbKernelBoot: kernel %lu, dtb %lu, ramdisk %lu bytes\n",
         (UINT64)KernelSize, (UINT64)DtbSize, (UINT64)RamdiskSize);
  AsciiSPrint (Line, sizeof (Line),
               "SfbKernelBoot: files read: kernel %lx dtb %lx ramdisk %lx",
               (UINT64)KernelSize, (UINT64)DtbSize, (UINT64)RamdiskSize);
  LogProgress (LogRoot, Line);

  if (!CheckKernelImage (Kernel, KernelSize, &Info)) {
    LogProgress (LogRoot, "SfbKernelBoot: FAILED kernel header check");
    Status = EFI_LOAD_ERROR;
    goto Out;
  }
  if (!CheckDtb (Dtb, DtbSize)) {
    LogProgress (LogRoot, "SfbKernelBoot: FAILED device tree check");
    Status = EFI_LOAD_ERROR;
    goto Out;
  }
  AsciiSPrint (Line, sizeof (Line),
               "SfbKernelBoot: checks passed: image_size %lx text_offset %lx "
               "flags %lx", Info.ImageSize, Info.TextOffset,
               (UINT64)Info.Flags);
  LogProgress (LogRoot, Line);

  CmdLen = AsciiStrLen (mCmdline) + 1;
  if (CmdLen > 1024) {
    Print (L"SfbKernelBoot: command line is %lu bytes, over the 1024 byte "
           L"placeholder\n", (UINT64)CmdLen);
    Status = EFI_LOAD_ERROR;
    goto Out;
  }

  /*
   * Everything downstream works in image_size, not the file size. The two
   * differ: image_size is the "effective image size" and includes BSS, so
   * placing the ramdisk at the end of the file would put it inside memory the
   * kernel is about to use.
   */
  Reserve = (UINTN)Info.ImageSize;
  Print (L"SfbKernelBoot: kernel file %lu, reserving %lu bytes\n",
         (UINT64)KernelSize, (UINT64)Reserve);

  /* --- 3. reserve and lay out ------------------------------------------- */
  Status = PlaceAll (Reserve, RamdiskSize, DtbSize, &Span, &SpanSize);
  if (EFI_ERROR (Status)) {
    LogProgress (LogRoot, "SfbKernelBoot: FAILED to reserve a span");
    goto Out;
  }

  /*
   * Kernel at the base of the span, then the ramdisk below the kernel's
   * reserved end, then the device-tree slot below that -- ABL's order, from
   * UpdateBootParams.
   */
  /*
   * The span still holds the previous kernel, records and all; read them
   * before anything writes over them.
   */
  ScanLastImage (LogRoot, Span, Reserve);

  KAt  = Span;
  RdAt = KAt + ALIGN_VALUE (Reserve, EFI_PAGE_SIZE);
  DtAt = RdAt + ALIGN_VALUE (RamdiskSize, EFI_PAGE_SIZE);

  if (DtAt + ALIGN_VALUE (DtbSize, EFI_PAGE_SIZE) > Span + SpanSize) {
    Print (L"SfbKernelBoot: layout does not fit the reserved span\n");
    Status = EFI_BUFFER_TOO_SMALL;
    goto Out;
  }

  CopyMem (KAt,  Kernel,  KernelSize);
  CopyMem (RdAt, Ramdisk, RamdiskSize);
  CopyMem (DtAt, Dtb,     DtbSize);

  Print (L"SfbKernelBoot: kernel %lx, ramdisk %lx, dtb %lx (span %lx-%lx)\n",
         (UINT64)(UINTN)KAt, (UINT64)(UINTN)RdAt, (UINT64)(UINTN)DtAt,
         (UINT64)(UINTN)Span, (UINT64)(UINTN)(Span + SpanSize - 1));
  AsciiSPrint (Line, sizeof (Line),
               "SfbKernelBoot: placed: kernel %lx ramdisk %lx dtb %lx "
               "span %lx+%lx", (UINT64)(UINTN)KAt, (UINT64)(UINTN)RdAt,
               (UINT64)(UINTN)DtAt, (UINT64)(UINTN)Span, (UINT64)SpanSize);
  LogProgress (LogRoot, Line);

  /*
   * Patch the three handoff properties into the placed copy: the command line,
   * and where the ramdisk starts and ends. The kernel finds the ramdisk only
   * through the last two, so leaving them out means it never looks at the
   * ramdisk at all.
   */
  Status = PatchChosen (DtAt, DtbSize, mCmdline,
                        (UINT64)(UINTN)RdAt,
                        (UINT64)(UINTN)RdAt + RamdiskSize);
  if (EFI_ERROR (Status)) {
    LogProgress (LogRoot, "SfbKernelBoot: FAILED to patch /chosen");
    goto Out;
  }
  AsciiSPrint (Line, sizeof (Line),
               "SfbKernelBoot: /chosen patched: bootargs %lx bytes, "
               "initrd %lx..%lx", (UINT64)CmdLen - 1,
               (UINT64)(UINTN)RdAt, (UINT64)(UINTN)RdAt + RamdiskSize);
  LogProgress (LogRoot, Line);

  Print (L"SfbKernelBoot: cmdline (%lu bytes): %a\n",
         (UINT64)CmdLen - 1, mCmdline);
  Print (L"SfbKernelBoot: leaving boot services and branching to %lx\n",
         (UINT64)(UINTN)KAt);

  /*
   * Prove the marker channel works before blaming the kernel for not using it.
   *
   * The kernel is supposed to write its own mark to this address on entry. If
   * it never does, either the kernel never ran or the address cannot be written
   * or read across a reset -- and those need telling apart. So write a mark
   * from here, using the same layout, and see whether the next boot reads it
   * back. If it does, the channel is good and the kernel really did not run.
   */
  {
    volatile UINT64  *Self = (volatile UINT64 *)(UINTN)SELF_MARK_BASE;

    Self[0] = 0x5E1F;                       /* a stage the kernel never uses */
    Self[1] = 0x5E1F5E1F5E1F5E1FULL;        /* a value nothing else would write */
    Self[2] = SELF_MARK_MAGIC;              /* committed last, as the kernel does */
    LogProgress (LogRoot,
                 "SfbKernelBoot: self-mark written before the jump");
  }

  /*
   * The last thing written before the point of no return. Whatever is in the
   * log after a failed attempt, this line being present means everything this
   * application controls was in place: the files were read, checked, placed,
   * and the device tree carried the command line and the ramdisk addresses.
   */
#if SFB_CANARY
  CanaryPrepare (LogRoot);
#endif

  AsciiSPrint (Line, sizeof (Line),
               "SfbKernelBoot: about to exit boot services; branching to %lx "
               "with x0=%lx", (UINT64)(UINTN)KAt, (UINT64)(UINTN)DtAt);
  LogProgress (LogRoot, Line);

  ExitAndJump (KAt, DtAt, Reserve, RamdiskSize, RdAt, DtbSize);

  /* Not reached: ExitAndJump disables the MMU and never returns. */
  return EFI_SUCCESS;

Out:
  if (Root != NULL) {
    Root->Close (Root);
  }
  if (LogRoot != NULL && LogRoot != Root) {
    LogRoot->Close (LogRoot);
  }
  if (Kernel != NULL) {
    FreePool (Kernel);
  }
  if (Dtb != NULL) {
    FreePool (Dtb);
  }
  if (Ramdisk != NULL) {
    FreePool (Ramdisk);
  }
  return Status;
}
