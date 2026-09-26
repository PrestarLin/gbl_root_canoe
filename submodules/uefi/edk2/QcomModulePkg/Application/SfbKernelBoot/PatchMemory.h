/*
 * Write the platform DRAM map into a device tree's /memory node.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
#ifndef PATCH_MEMORY_H
#define PATCH_MEMORY_H

#include <Uefi.h>

EFI_STATUS
PatchMemory (
  IN OUT UINT8  *Dtb,
  IN     UINTN   Size
  );

#endif /* PATCH_MEMORY_H */
