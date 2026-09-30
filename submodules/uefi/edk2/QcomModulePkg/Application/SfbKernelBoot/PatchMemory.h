/*
 * Synthesize the platform DRAM map from the firmware's GetMemoryMap and
 * write it into a device tree's /memory node, the loader-side half of what
 * ABL does when it loads a kernel.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
#ifndef PATCH_MEMORY_H
#define PATCH_MEMORY_H

#include <Uefi.h>

/*
 * Map/MapSize/DescSize describe a GetMemoryMap buffer (DescSize is the
 * firmware's record stride, possibly larger than sizeof descriptor).
 * RegionCount/TotalBytes, when non-NULL, receive the synthesis result.
 *
 * EFI_SUCCESS        reg rewritten with the synthesized map
 * EFI_NOT_READY      synthesis/validation failed; caller keeps the tree's own
 * EFI_BUFFER_TOO_SMALL  reg is not the fixed 336-byte slot; cannot repair
 * EFI_NOT_FOUND      no /memory node
 * EFI_VOLUME_CORRUPTED  not a device tree / malformed structure
 */
EFI_STATUS
PatchMemory (
  IN OUT UINT8                        *Dtb,
  IN     UINTN                         Size,
  IN     CONST EFI_MEMORY_DESCRIPTOR   *Map,
  IN     UINTN                         MapSize,
  IN     UINTN                         DescSize,
  OUT    UINTN                        *RegionCount OPTIONAL,
  OUT    UINT64                       *TotalBytes OPTIONAL
  );

#endif /* PATCH_MEMORY_H */
