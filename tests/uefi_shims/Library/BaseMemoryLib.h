#ifndef HOST_LIBRARY_BASEMEMORYLIB_H
#define HOST_LIBRARY_BASEMEMORYLIB_H

#include <Uefi.h>

VOID *CopyMem (OUT VOID *DestinationBuffer, IN CONST VOID *SourceBuffer, IN UINTN Length);
VOID *SetMem (OUT VOID *Buffer, IN UINTN Length, IN UINT8 Value);

#endif
