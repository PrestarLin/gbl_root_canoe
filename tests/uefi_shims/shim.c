#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>

UINT32
SwapBytes32 (UINT32 Data)
{
  return __builtin_bswap32 (Data);
}

UINT64
SwapBytes64 (UINT64 Data)
{
  return __builtin_bswap64 (Data);
}

UINTN
AsciiStrLen (const CHAR8 *String)
{
  return strlen (String);
}

INTN
AsciiStrCmp (const CHAR8 *FirstString, const CHAR8 *SecondString)
{
  return strcmp (FirstString, SecondString);
}

INTN
AsciiStrnCmp (const CHAR8 *FirstString, const CHAR8 *SecondString, UINTN Length)
{
  return strncmp (FirstString, SecondString, Length);
}

VOID *
CopyMem (OUT VOID *DestinationBuffer, IN CONST VOID *SourceBuffer, IN UINTN Length)
{
  return memcpy (DestinationBuffer, SourceBuffer, Length);
}

VOID *
SetMem (OUT VOID *Buffer, IN UINTN Length, IN UINT8 Value)
{
  return memset (Buffer, Value, Length);
}

UINTN
Print (IN CONST CHAR16 *Format, ...)
{
  va_list Args;
  int     Written;

  va_start (Args, Format);
  Written = vfwprintf (stderr, (const wchar_t *)Format, Args);
  va_end (Args);
  fputc ('\n', stderr);
  return (Written < 0) ? 0 : (UINTN)Written;
}
