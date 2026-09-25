/*
 * Console UI for the super-fastboot boot menu.
 *
 * Three keys drive everything: volume up and volume down move the cursor, and
 * power confirms.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbMenu.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/ShutdownServices.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
/* HiiFont.h also pulls in GraphicsOutput.h and HiiImage.h. */
#include <Protocol/HiiFont.h>
#include <Protocol/SimpleTextIn.h>

/* Keeps the translation unit legal when the feature is compiled out. */
CONST CHAR8 *gSfbMenuModuleTag = "SuperFbMenu";

SFB_KEY
SfbWaitForKey (IN UINT32 TimeoutMs)
{
  EFI_STATUS     Status;
  EFI_EVENT      TimerEvent = NULL;
  EFI_EVENT      WaitList[2];
  UINTN          WaitCount;
  UINTN          EventIndex;
  EFI_INPUT_KEY  Key;
  SFB_KEY        Result = SfbKeyTimeout;

  if (TimeoutMs != 0) {
    Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &TimerEvent);
    if (EFI_ERROR (Status)) {
      TimerEvent = NULL;
    } else {
      /* Boot services timers count in 100ns units. */
      Status = gBS->SetTimer (TimerEvent, TimerRelative,
                              (UINT64)TimeoutMs * 10000);
      if (EFI_ERROR (Status)) {
        gBS->CloseEvent (TimerEvent);
        TimerEvent = NULL;
      }
    }
  }

  WaitList[0] = gST->ConIn->WaitForKey;
  WaitCount = 1;
  if (TimerEvent != NULL) {
    WaitList[1] = TimerEvent;
    WaitCount = 2;
  }

  while (TRUE) {
    Status = gBS->WaitForEvent (WaitCount, WaitList, &EventIndex);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "SFB: WaitForEvent failed: %r\n", Status));
      break;
    }

    if (EventIndex == 1) {
      break;
    }

    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (EFI_ERROR (Status)) {
      continue;
    }

    /*
     * On the handset the Qualcomm keypad driver reports the volume keys as
     * SCAN_UP and SCAN_DOWN, and power arrives as a carriage return.
     *
     * Anything left over counts as confirm: on a three-key handset there is
     * nothing else it can be, so the menu stays usable even if a platform
     * reports power differently from what is expected here.
     */
    if (Key.ScanCode == SCAN_UP) {
      Result = SfbKeyUp;
    } else if (Key.ScanCode == SCAN_DOWN) {
      Result = SfbKeyDown;
    } else {
      DEBUG ((EFI_D_VERBOSE, "SFB: confirm key scan=0x%x char=0x%x\n",
              Key.ScanCode, Key.UnicodeChar));
      Result = SfbKeySelect;
    }
    break;
  }

  if (TimerEvent != NULL) {
    gBS->CloseEvent (TimerEvent);
  }

  return Result;
}

/* ---- drawing ------------------------------------------------------------ */

STATIC CONST CHAR16*
SfbGetFileName (IN CONST CHAR16 *Path)
{
  CONST CHAR16 *FileName = Path;
  while (*Path != L'\0') {
    if (*Path == L'\\') FileName = Path + 1;
    Path++;
  }
  return FileName;
}

STATIC BOOLEAN
SfbStrCaseEqual (IN CONST CHAR16 *Str1, IN CONST CHAR16 *Str2)
{
  while (*Str1 && *Str2) {
    CHAR16 c1 = (*Str1 >= L'a' && *Str1 <= L'z') ? *Str1 - 0x20 : *Str1;
    CHAR16 c2 = (*Str2 >= L'a' && *Str2 <= L'z') ? *Str2 - 0x20 : *Str2;
    if (c1 != c2) return FALSE;
    Str1++;
    Str2++;
  }
  return *Str1 == L'\0' && *Str2 == L'\0';
}

/* ---- scaled text renderer ------------------------------------------------ */

/*
 * The UEFI text console draws glyphs at a fixed 8x19 pixels and never scales
 * them, which is unreadably small on the handset panel. The menu therefore
 * paints its own text: the HII font protocol renders one line at native size
 * into an offscreen bitmap, the bitmap is enlarged by an integer scale factor
 * with nearest-neighbour sampling, and the result reaches the display in a
 * single GOP blit. Every line is centred horizontally and the selected row
 * sits on a highlight bar. When GOP or the HII font protocol is missing the
 * same drawing falls back to ConOut, centred by cursor position instead.
 */

/* Longest line the renderer keeps; the stack buffers are sized from this. */
#define SFB_MAX_RENDER_CHARS  128
/* Glyph scale bounds: 16x38 px at 2x, 24x57 px at 3x. */
#define SFB_MAX_SCALE         3
/* A 48-character description plus its "> * " prefix must stay on one row. */
#define SFB_MIN_COLUMNS       52
/* Lines (title, subtitle, blank, rows, footer) that must fit in most of the
 * screen height at the chosen scale. */
#define SFB_LAYOUT_LINES      16

/* Console attribute colours, ordered blue-green-red like GraphicsConsole. */
STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mSfbColors[16] = {
  {0x00, 0x00, 0x00, 0x00},  /*  0: black      */
  {0x98, 0x00, 0x00, 0x00},  /*  1: lightblue  */
  {0x00, 0x98, 0x00, 0x00},  /*  2: lightgreen */
  {0x98, 0x98, 0x00, 0x00},  /*  3: lightcyan  */
  {0x00, 0x00, 0x98, 0x00},  /*  4: lightred   */
  {0x98, 0x00, 0x98, 0x00},  /*  5: magenta    */
  {0x00, 0x98, 0x98, 0x00},  /*  6: brown      */
  {0x98, 0x98, 0x98, 0x00},  /*  7: lightgray  */
  {0x30, 0x30, 0x30, 0x00},  /*  8: darkgray   */
  {0xff, 0x00, 0x00, 0x00},  /*  9: blue       */
  {0x00, 0xff, 0x00, 0x00},  /* 10: lime       */
  {0xff, 0xff, 0x00, 0x00},  /* 11: cyan       */
  {0x00, 0x00, 0xff, 0x00},  /* 12: red        */
  {0xff, 0x00, 0xff, 0x00},  /* 13: fuchsia    */
  {0x00, 0xff, 0xff, 0x00},  /* 14: yellow     */
  {0xff, 0xff, 0xff, 0x00}   /* 15: white      */
};

STATIC BOOLEAN                        mSfbInited    = FALSE;
STATIC BOOLEAN                        mSfbGfx       = FALSE;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL   *mSfbGop      = NULL;
STATIC EFI_HII_FONT_PROTOCOL          *mSfbHiiFont  = NULL;
STATIC UINT32                         mSfbScreenW   = 0;
STATIC UINT32                         mSfbScreenH   = 0;
STATIC UINTN                          mSfbScale     = 1;
STATIC UINTN                          mSfbLineHeight = 0;
STATIC UINTN                          mSfbMaxChars  = 0;
STATIC UINTN                          mSfbPenY      = 0;
STATIC UINTN                          mSfbConCols   = 0;
STATIC UINTN                          mSfbConRows   = 0;
STATIC UINTN                          mSfbConRow    = 0;

/*
 * Locate GOP and the HII font protocol once, then fix the screen geometry:
 * the glyph scale, the line pitch, and the longest line that still fits.
 */
STATIC VOID
SfbTextInit (VOID)
{
  EFI_STATUS  Status;
  UINTN       Scale;

  if (mSfbInited) {
    return;
  }
  mSfbInited = TRUE;

  mSfbGop = NULL;
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL,
                                (VOID **)&mSfbGop);
  if (EFI_ERROR (Status)) {
    mSfbGop = NULL;
  }
  mSfbHiiFont = NULL;
  Status = gBS->LocateProtocol (&gEfiHiiFontProtocolGuid, NULL,
                                (VOID **)&mSfbHiiFont);
  if (EFI_ERROR (Status)) {
    mSfbHiiFont = NULL;
  }

  mSfbGfx = FALSE;
  if (mSfbGop != NULL && mSfbHiiFont != NULL &&
      mSfbGop->Mode != NULL && mSfbGop->Mode->Info != NULL) {
    mSfbScreenW = mSfbGop->Mode->Info->HorizontalResolution;
    mSfbScreenH = mSfbGop->Mode->Info->VerticalResolution;
    if (mSfbScreenW >= 640 && mSfbScreenH >= 480) {
      mSfbGfx = TRUE;
    }
  }

  if (mSfbGfx) {
    /*
     * Start at the largest scale and step down while the screen is too narrow
     * to keep whole descriptions on one row, or too short to hold the layout.
     */
    Scale = SFB_MAX_SCALE;
    while (Scale > 1 &&
           ((mSfbScreenW - mSfbScreenW / 10) / (EFI_GLYPH_WIDTH * Scale) <
              SFB_MIN_COLUMNS ||
            SFB_LAYOUT_LINES * (EFI_GLYPH_HEIGHT + 4) * Scale >
              mSfbScreenH * 3 / 4)) {
      Scale--;
    }

    mSfbScale      = Scale;
    mSfbLineHeight = (EFI_GLYPH_HEIGHT + 4) * Scale;
    mSfbMaxChars   = (mSfbScreenW - mSfbScreenW / 10) /
                     (EFI_GLYPH_WIDTH * Scale);
    if (mSfbMaxChars > SFB_MAX_RENDER_CHARS) {
      mSfbMaxChars = SFB_MAX_RENDER_CHARS;
    }
    if (mSfbMaxChars < 16) {
      mSfbMaxChars = 16;
    }
    return;
  }

  /* ConOut fallback: centre inside the text-mode grid instead. */
  if (gST->ConOut->QueryMode (gST->ConOut, gST->ConOut->Mode->Mode,
                              &mSfbConCols, &mSfbConRows) != EFI_SUCCESS ||
      mSfbConCols == 0 || mSfbConRows == 0) {
    mSfbConCols = 80;
    mSfbConRows = 25;
  }
  if (mSfbConCols > SFB_MAX_RENDER_CHARS) {
    mSfbConCols = SFB_MAX_RENDER_CHARS;
  }
  mSfbMaxChars = mSfbConCols;
}

/* Copy Text to Out, cut to MaxChars with an ellipsis, return its length. */
STATIC UINTN
SfbTruncate (IN CONST CHAR16 *Text, IN UINTN MaxChars, OUT CHAR16 *Out)
{
  UINTN  Len;

  Len = StrLen (Text);
  if (Len <= MaxChars) {
    CopyMem (Out, Text, (Len + 1) * sizeof (CHAR16));
    return Len;
  }

  if (MaxChars > 3) {
    CopyMem (Out, Text, (MaxChars - 3) * sizeof (CHAR16));
    Out[MaxChars - 3] = L'.';
    Out[MaxChars - 2] = L'.';
    Out[MaxChars - 1] = L'.';
    Out[MaxChars] = L'\0';
    return MaxChars;
  }

  CopyMem (Out, Text, MaxChars * sizeof (CHAR16));
  Out[MaxChars] = L'\0';
  return MaxChars;
}

/* Blank the screen and park the pen on the first line. */
STATIC VOID
SfbClearScreen (VOID)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black = {0x00, 0x00, 0x00, 0x00};

  SfbTextInit ();
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  if (mSfbGfx) {
    mSfbGop->Blt (mSfbGop, &Black, EfiBltVideoFill, 0, 0, 0, 0,
                  mSfbScreenW, mSfbScreenH, 0);
    mSfbPenY = mSfbScreenH / 8;
  } else {
    gST->ConOut->ClearScreen (gST->ConOut);
    mSfbConRow = 0;
  }
}

/* Centre a Lines-tall block vertically on the current screen. */
STATIC VOID
SfbCenterPen (IN UINTN Lines)
{
  UINTN  Block;

  SfbTextInit ();
  if (mSfbGfx) {
    Block = Lines * mSfbLineHeight;
    mSfbPenY = (mSfbScreenH > Block) ? (mSfbScreenH - Block) / 2 : 0;
  } else {
    mSfbConRow = (mSfbConRows > Lines) ? (mSfbConRows - Lines) / 2 : 0;
  }
}

/* ConOut fallback: one centred line on the next console row. */
STATIC VOID
SfbConCenteredLine (IN CONST CHAR16 *Text, IN UINTN Attribute)
{
  CHAR16  Line[SFB_MAX_RENDER_CHARS + 1];
  UINTN   Len;
  UINTN   Column;

  if (mSfbConRow >= mSfbConRows) {
    return;
  }

  Len = SfbTruncate (Text, mSfbMaxChars, Line);
  Column = (mSfbConCols > Len) ? (mSfbConCols - Len) / 2 : 0;

  gST->ConOut->SetAttribute (gST->ConOut, Attribute);
  if (!EFI_ERROR (gST->ConOut->SetCursorPosition (gST->ConOut, Column,
                                                  mSfbConRow))) {
    gST->ConOut->OutputString (gST->ConOut, Line);
  }
  mSfbConRow++;
}

/*
 * One centred line through the scaled renderer: HII paints the glyphs into an
 * offscreen bitmap, the bitmap is enlarged, and the result is blitted once.
 */
STATIC VOID
SfbGfxCenteredLine (IN CONST CHAR16 *Text, IN UINTN Attribute)
{
  EFI_STATUS                     Status;
  CHAR16                         Line[SFB_MAX_RENDER_CHARS + 1];
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Bitmap = NULL;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Scaled = NULL;
  EFI_FONT_DISPLAY_INFO          *FontInfo = NULL;
  EFI_HII_ROW_INFO               *RowInfo = NULL;
  UINTN                          RowCount = 0;
  EFI_IMAGE_OUTPUT               Image;
  EFI_IMAGE_OUTPUT               *ImagePtr;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Foreground;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Background;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *SourceRow;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *TargetRow;
  UINTN                          Len;
  UINTN                          SourceWidth;
  UINTN                          RowWidth;
  UINTN                          RowHeight;
  UINTN                          ScaledWidth;
  UINTN                          ScaledHeight;
  UINTN                          LineX;
  UINTN                          BarWidth;
  UINTN                          X;
  UINTN                          Y;

  if (mSfbPenY >= mSfbScreenH) {
    return;
  }

  Len = SfbTruncate (Text, mSfbMaxChars, Line);
  if (Len == 0) {
    /* Blank separator line: just move the pen down. */
    mSfbPenY += mSfbLineHeight;
    return;
  }

  Foreground = mSfbColors[Attribute & 0x0f];
  Background = mSfbColors[(Attribute >> 4) & 0x0f];

  SourceWidth = (Len + 2) * EFI_GLYPH_WIDTH;
  Bitmap = AllocatePool (SourceWidth * EFI_GLYPH_HEIGHT *
                         sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  if (Bitmap == NULL) {
    mSfbPenY += mSfbLineHeight;
    return;
  }

  /* Pre-fill with the background so no stale pixels survive the blit. */
  for (X = 0; X < SourceWidth * EFI_GLYPH_HEIGHT; X++) {
    Bitmap[X] = Background;
  }

  FontInfo = AllocateZeroPool (sizeof (*FontInfo));
  if (FontInfo == NULL) {
    goto Advance;
  }
  /*
   * A zero mask with an empty name never matches a registered font, so HII
   * falls back to the 19-row system font while keeping the colours set here,
   * exactly like GraphicsConsole does for the console itself.
   */
  FontInfo->ForegroundColor = Foreground;
  FontInfo->BackgroundColor = Background;

  ZeroMem (&Image, sizeof (Image));
  Image.Width         = (UINT16)SourceWidth;
  Image.Height        = EFI_GLYPH_HEIGHT;
  Image.Image.Bitmap  = Bitmap;
  ImagePtr            = &Image;

  Status = mSfbHiiFont->StringToImage (
                          mSfbHiiFont,
                          EFI_HII_IGNORE_IF_NO_GLYPH | EFI_HII_IGNORE_LINE_BREAK,
                          Line, FontInfo, &ImagePtr, 0, 0,
                          &RowInfo, &RowCount, NULL);
  if (EFI_ERROR (Status) || RowInfo == NULL || RowCount == 0 ||
      RowInfo[0].LineWidth == 0) {
    goto Advance;
  }

  RowWidth  = RowInfo[0].LineWidth;
  RowHeight = (RowInfo[0].LineHeight != 0) ? RowInfo[0].LineHeight
                                           : EFI_GLYPH_HEIGHT;
  ScaledWidth  = RowWidth * mSfbScale;
  ScaledHeight = RowHeight * mSfbScale;
  if (ScaledWidth > mSfbScreenW || mSfbPenY + ScaledHeight > mSfbScreenH) {
    goto Advance;
  }

  Scaled = AllocatePool (ScaledWidth * ScaledHeight *
                         sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  if (Scaled == NULL) {
    goto Advance;
  }

  /* Nearest-neighbour enlargement: every scaled pixel copies its source. */
  for (Y = 0; Y < ScaledHeight; Y++) {
    SourceRow = Bitmap + (Y / mSfbScale) * SourceWidth;
    TargetRow = Scaled + Y * ScaledWidth;
    for (X = 0; X < ScaledWidth; X++) {
      TargetRow[X] = SourceRow[X / mSfbScale];
    }
  }

  LineX = (mSfbScreenW - ScaledWidth) / 2;

  /* Highlight bar behind a selected row: full text width, never the whole
   * screen narrower than six tenths of it, clipped to the panel. */
  if ((Attribute >> 4) != EFI_BLACK) {
    BarWidth = ScaledWidth + 16 * mSfbScale;
    if (BarWidth < mSfbScreenW * 6 / 10) {
      BarWidth = mSfbScreenW * 6 / 10;
    }
    if (BarWidth > mSfbScreenW) {
      BarWidth = mSfbScreenW;
    }
    mSfbGop->Blt (mSfbGop, &Background, EfiBltVideoFill, 0, 0,
                  (mSfbScreenW - BarWidth) / 2, mSfbPenY,
                  BarWidth, ScaledHeight, 0);
  }

  mSfbGop->Blt (mSfbGop, Scaled, EfiBltBufferToVideo, 0, 0, LineX, mSfbPenY,
                ScaledWidth, ScaledHeight,
                ScaledWidth * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));

Advance:
  mSfbPenY += mSfbLineHeight;
  if (RowInfo != NULL) {
    FreePool (RowInfo);
  }
  if (Scaled != NULL) {
    FreePool (Scaled);
  }
  if (FontInfo != NULL) {
    FreePool (FontInfo);
  }
  if (Bitmap != NULL) {
    FreePool (Bitmap);
  }
}

VOID
SfbPrintCentered (IN CONST CHAR16 *Text, IN UINTN Attribute)
{
  if (Text == NULL) {
    return;
  }

  SfbTextInit ();
  if (mSfbGfx) {
    SfbGfxCenteredLine (Text, Attribute);
  } else {
    SfbConCenteredLine (Text, Attribute);
  }
}

VOID
SfbBeginScreen (IN CONST CHAR16 *Title, IN CONST CHAR16 *Subtitle)
{
  SfbClearScreen ();
  SfbPrintCentered (Title, SFB_ATTR_TITLE);
  if (Subtitle != NULL) {
    SfbPrintCentered (Subtitle, SFB_ATTR_NORMAL);
  }
  SfbPrintCentered (L"", SFB_ATTR_NORMAL);
}

VOID
SfbEndScreen (IN CONST CHAR16 *Footer)
{
  UINTN  FooterY;

  SfbTextInit ();
  if (mSfbGfx) {
    /* Pin the footer near the bottom so short menus still fill the screen. */
    FooterY = mSfbScreenH - mSfbScreenH / 8 - mSfbLineHeight;
    if (mSfbPenY < FooterY) {
      mSfbPenY = FooterY;
    }
  } else {
    /* Blank line, the way the old Print (L"\r\n%s\r\n") did. */
    mSfbConRow++;
  }

  SfbPrintCentered (Footer, SFB_ATTR_NORMAL);
}

VOID
SfbDrawRow (IN BOOLEAN Selected, IN CONST CHAR16 *Marker, IN CONST CHAR16 *Text)
{
  CHAR16  Line[SFB_DESC_CHARS + SFB_PATH_CHARS + 8];

  UnicodeSPrint (Line, sizeof (Line), L"%s %s %s",
                 Selected ? L">" : L" ", Marker, Text);
  SfbPrintCentered (Line, Selected ? SFB_ATTR_SELECTED : SFB_ATTR_NORMAL);
}

/*
 * First row of the visible window, keeping the cursor inside it. Lists longer
 * than the window scroll rather than overflow the console.
 */
UINTN
SfbWindowStart (IN UINTN Cursor, IN UINTN Count, IN UINTN Rows)
{
  if (Count <= Rows) {
    return 0;
  }
  if (Cursor < Rows / 2) {
    return 0;
  }
  if (Cursor > Count - 1 - (Rows - Rows / 2 - 1)) {
    return Count - Rows;
  }

  return Cursor - Rows / 2;
}

VOID
SfbMoveCursor (IN OUT UINTN *Cursor, IN UINTN Count, IN SFB_KEY Key)
{
  if (Count == 0) {
    *Cursor = 0;
    return;
  }

  if (Key == SfbKeyUp) {
    *Cursor = (*Cursor == 0) ? Count - 1 : *Cursor - 1;
  } else if (Key == SfbKeyDown) {
    *Cursor = (*Cursor + 1 >= Count) ? 0 : *Cursor + 1;
  }
}

/* Format a status line the way Print would, show it, and wait for a key. */
VOID
SfbReportStatusFormat (IN CONST CHAR16 *What, IN EFI_STATUS Status)
{
  CHAR16  Line[SFB_DESC_CHARS + 64];

  UnicodeSPrint (Line, sizeof (Line), L"%s: %r", What, Status);

  SfbClearScreen ();
  SfbCenterPen (2);
  SfbPrintCentered (Line, SFB_ATTR_NORMAL);
  SfbPrintCentered (L"Press power to continue.", SFB_ATTR_NORMAL);
  SfbWaitForKey (0);
}

/* Report a failure and hold the screen until the user acknowledges it. */
VOID
SfbReportStatus (IN CONST CHAR16 *What, IN EFI_STATUS Status)
{
  SfbReportStatusFormat (What, Status);
}

/*
 * Hand the screen over to fastboot. The menu is the last thing that draws
 * before control leaves for the fastboot loop, which prints nothing of its own
 * until a host connects, so without this the user would be staring at a boot
 * menu that no longer responds to anything.
 */
VOID
SfbShowFastbootMode (VOID)
{
  SfbClearScreen ();
  SfbCenterPen (1);
  SfbPrintCentered (L"FASTBOOT MODE", SFB_ATTR_TITLE);
}

/*
 * Clear the menu away and announce the launch. The loaded image prints nothing
 * of its own until it takes over, so without this the boot menu would linger on
 * screen through the load.
 */
VOID
SfbShowBootingScreen (IN CONST CHAR16 *Name,
                      IN CONST CHAR16 *FilePath,
                      IN BOOLEAN       ClearScreen)
{
  CONST CHAR16  *FileName;
  CHAR16        Line[96];

  /*
   * An unattended default boot must not blank whatever is already on screen
   * (typically the boot splash): only clear when the launch came from the menu,
   * where the menu itself is what needs clearing away.
   */
  if (ClearScreen) {
    SfbClearScreen ();
  } else {
    gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  }

  if (FilePath != NULL) {
    FileName = SfbGetFileName (FilePath);
    if (SfbStrCaseEqual (FileName, L"boot.efi")) {
      return;
    }
  }

  UnicodeSPrint (Line, sizeof (Line), L"Booting %s",
                 (Name != NULL && Name[0] != L'\0') ? Name : L"...");
  SfbCenterPen (1);
  SfbPrintCentered (Line, SFB_ATTR_TITLE);
}

/*
 * Announce a power action (Power Off / Restart) and leave the message on
 * screen while the reset takes effect. Neither action returns, so the screen is
 * the last thing the user sees.
 */
VOID
SfbShowActionScreen (IN CONST CHAR16 *Text)
{
  SfbClearScreen ();
  SfbCenterPen (1);
  SfbPrintCentered (Text, SFB_ATTR_TITLE);
}

/*
 * Seconds to hold on the "Entering Boot Menu" screen before the menu starts
 * taking input. Long enough that a volume key held from power-on has been
 * released, so it does not immediately move the menu cursor.
 */
#define SFB_ENTER_MENU_DELAY_S  3

VOID
SfbShowEnteringMenu (VOID)
{
  SfbClearScreen ();
  SfbCenterPen (1);
  SfbPrintCentered (L"Entering Boot Menu", SFB_ATTR_TITLE);

  /* Wait for the key to be released... */
  gBS->Stall (SFB_ENTER_MENU_DELAY_S * 1000 * 1000);

  /* ...then drop anything typed or held during the wait so it does not leak
   * into the menu as a spurious keypress. */
  gST->ConIn->Reset (gST->ConIn, FALSE);
}

/* ---- boot menu ---------------------------------------------------------- */

STATIC
VOID
SfbDrawMenu (IN CONST SFB_MENU_STATE *Menu,
             IN UINTN                Cursor,
             IN CONST CHAR16         *Title)
{
  UINTN  Start;
  UINTN  Index;
  UINTN  Last;

  SfbBeginScreen (Title, NULL);

  if (Menu->Count == 0) {
    SfbPrintCentered (L"No boot entries found.", SFB_ATTR_NORMAL);
  }

  Start = SfbWindowStart (Cursor, Menu->Count, SFB_VISIBLE_ROWS);
  Last = Start + SFB_VISIBLE_ROWS;
  if (Last > Menu->Count) {
    Last = Menu->Count;
  }

  for (Index = Start; Index < Last; Index++) {
    CONST SFB_BOOT_ENTRY  *Entry = &Menu->Entry[Index];
    CONST CHAR16          *Marker = (Index == Menu->DefaultIndex) ? L"*" : L" ";

    /* Submenu rows get a trailing '>' so it is obvious they open another list
     * rather than launch an image. */
    if (Entry->Kind == SfbEntrySubmenu) {
      CHAR16  Text[SFB_DESC_CHARS + 4];

      UnicodeSPrint (Text, sizeof (Text), L"%s >", Entry->Desc);
      SfbDrawRow ((BOOLEAN)(Index == Cursor), Marker, Text);
    } else {
      SfbDrawRow ((BOOLEAN)(Index == Cursor), Marker, Entry->Desc);
    }
  }

  if (Last < Menu->Count) {
    CHAR16  More[64];

    UnicodeSPrint (More, sizeof (More), L"... %u more",
                   (UINT32)(Menu->Count - Last));
    SfbPrintCentered (More, SFB_ATTR_NORMAL);
  }

  SfbEndScreen (L"Vol Up/Down: move   Power: select");
}

/*
 * Run a submenu defined by the ENTRIES file at EntriesPath on Volume. The file
 * is parsed exactly like the root BOOTENTRIES, and may itself contain further
 * '%' submenu rows; Depth bounds the nesting so a chain of files that points at
 * one another cannot recurse without limit. The submenu state is heap-allocated
 * (a single SFB_MENU_STATE is ~17 KB) so deep nesting stays off the call stack.
 *
 * Returns when the user picks the trailing "Back" row, or when the file could
 * not be built at all; the caller then redraws its own menu.
 */
STATIC
VOID
SfbRunSubMenu (IN EFI_HANDLE   Volume,
               IN CONST CHAR16 *EntriesPath,
               IN CONST CHAR16 *Title,
               IN UINTN        Depth)
{
  SFB_MENU_STATE  *Menu = NULL;
  UINTN           Cursor = 0;
  BOOLEAN         Rebuild = TRUE;
  SFB_KEY         Key;
  EFI_STATUS      Status;

  Menu = AllocateZeroPool (sizeof (*Menu));
  if (Menu == NULL) {
    return;
  }
  Menu->DefaultIndex = SFB_NO_INDEX;

  while (TRUE) {
    UINTN  Chosen;

    if (Rebuild) {
      SfbFreeMenu (Menu);
      Status = SfbBuildSubMenu (Menu, Volume, EntriesPath);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (Title, Status);
        break;
      }
      Cursor = 0;
      Rebuild = FALSE;
    }

    SfbDrawMenu (Menu, Cursor, Title);

    /* Same input model as the root menu: volume keys move, power confirms. */
    Key = SfbWaitForKey (0);

    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, Menu->Count, Key);
      continue;
    }

    if (Menu->Count == 0) {
      continue;
    }

    Chosen = Cursor;
    switch (Menu->Entry[Chosen].Kind) {
    case SfbEntryBack:
      goto done;

    case SfbEntrySubmenu:
      if (Depth >= SFB_MAX_SUBMENU_DEPTH) {
        SfbReportStatus (L"Submenu too deep", EFI_BUFFER_TOO_SMALL);
      } else {
        SfbRunSubMenu (Menu->Entry[Chosen].Volume,
                       Menu->Entry[Chosen].Path,
                       Menu->Entry[Chosen].Desc,
                       Depth + 1);
      }
      /* Media may have changed while the child menu was open. */
      Rebuild = TRUE;
      break;

    case SfbEntryEfiFile:
    default:
      Status = SfbLaunchEntry (&Menu->Entry[Chosen], TRUE, TRUE);//Entries in submenu never defaults
      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Boot failed", Status);
      }
      Rebuild = TRUE;
      break;
    }
  }

done:
  SfbFreeMenu (Menu);
  FreePool (Menu);
}

BOOLEAN
SfbRunBootMenu (VOID)
{
  SFB_MENU_STATE  Menu;
  UINTN           Cursor = 0;
  BOOLEAN         Rebuild = TRUE;
  SFB_KEY         Key;
  EFI_STATUS      Status;

  ZeroMem (&Menu, sizeof (Menu));
  Menu.DefaultIndex = SFB_NO_INDEX;

  while (TRUE) {
    UINTN  Chosen;

    if (Rebuild) {
      SfbFreeMenu (&Menu);
      SfbBuildMenu (&Menu);
      Cursor = (Menu.DefaultIndex == SFB_NO_INDEX) ? 0 : Menu.DefaultIndex;
      Rebuild = FALSE;
    }

    SfbDrawMenu (&Menu, Cursor, L"Boot Menu");

    /* The menu is purely interactive: it waits for a key indefinitely and
     * never launches anything unattended. */
    Key = SfbWaitForKey (0);

    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, Menu.Count, Key);
      continue;
    }

    Chosen = Cursor;

    if (Menu.Count == 0) {
      continue;
    }

    switch (Menu.Entry[Chosen].Kind) {
    case SfbEntryFastboot:
      SfbFreeMenu (&Menu);
      return TRUE;

    case SfbEntrySelector:
      SfbRunFileBrowser ();
      /* The browser may have added a custom entry. */
      Rebuild = TRUE;
      break;

    case SfbEntrySubmenu:
      SfbRunSubMenu (Menu.Entry[Chosen].Volume,
                     Menu.Entry[Chosen].Path,
                     Menu.Entry[Chosen].Desc,
                     1);
      /* Media may have changed while the submenu was open. */
      Rebuild = TRUE;
      break;

    case SfbEntryBack:
      /* Only submenus carry a Back row; the root menu never adds one. */
      Rebuild = TRUE;
      break;

    case SfbEntryPowerOff:
      SfbShowActionScreen (L"Powering off...");
      ShutdownDevice ();
      break;

    case SfbEntryRestart:
      SfbShowActionScreen (L"Restarting...");
      RebootDevice (NORMAL_MODE);
      break;

    case SfbEntryEfiFile:
    default:
      Status = SfbLaunchEntry (&Menu.Entry[Chosen], FALSE, TRUE);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Boot failed", Status);
      }
      /* Media or variables may have changed while the image ran. */
      Rebuild = TRUE;
      break;
    }
  }
}
