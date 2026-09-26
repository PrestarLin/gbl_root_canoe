#ifndef HOST_LIBRARY_BASELIB_H
#define HOST_LIBRARY_BASELIB_H

#include <Uefi.h>

UINT32 SwapBytes32 (UINT32 Data);
UINT64 SwapBytes64 (UINT64 Data);

UINTN   AsciiStrLen (const CHAR8 *String);
INTN    AsciiStrCmp (const CHAR8 *FirstString, const CHAR8 *SecondString);
INTN    AsciiStrnCmp (const CHAR8 *FirstString, const CHAR8 *SecondString, UINTN Length);

#endif
