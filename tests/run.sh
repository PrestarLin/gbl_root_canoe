#!/usr/bin/env bash
# Compile and run the host-side device-tree patch tests.
#
# PatchMemory.c is built unmodified; -I points at the shim headers so the
# same source that runs in the firmware also runs here as a host process.
set -eu
cd "$(dirname "$0")/.."

SRC=submodules/uefi/edk2/QcomModulePkg/Application/SfbKernelBoot/PatchMemory.c
HDR=submodules/uefi/edk2/QcomModulePkg/Application/SfbKernelBoot

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

cc -Wall -Wextra -Werror \
   -I tests/uefi_shims -I "$HDR" \
   -o "$OUT/test_patch_memory" \
   tests/test_patch_memory.c "$SRC" tests/uefi_shims/shim.c

"$OUT/test_patch_memory"
