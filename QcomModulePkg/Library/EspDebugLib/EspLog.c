/** @file EspLog.c

  In-RAM boot log (see EspLog.h).  Fixed-size buffer; when full, the oldest
  half is dropped so the most recent messages are always kept.

  Copyright (c) 2026, REG-Linux contributors.
  SPDX-License-Identifier: GPL-3.0-or-later
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>

#include <Library/EspLog.h>

#define ESP_LOG_SIZE  (128 * 1024)

extern EFI_SYSTEM_TABLE  *mDebugST;   /* saved by DebugLibConstructor.c */

STATIC CHAR8            mEspLog[ESP_LOG_SIZE];
STATIC UINTN            mEspLogLen;
STATIC BOOLEAN          mEspLogWrapped;
STATIC EFI_TEXT_STRING  mOrigOutputString;
STATIC BOOLEAN          mInHook;

STATIC
VOID
EspLogPutc (
  IN CHAR8  C
  )
{
  if (C == '\r' || C == '\0') {
    return;
  }
  if (mEspLogLen >= ESP_LOG_SIZE) {
    CopyMem (mEspLog, mEspLog + ESP_LOG_SIZE / 2, ESP_LOG_SIZE / 2);
    mEspLogLen = ESP_LOG_SIZE / 2;
    mEspLogWrapped = TRUE;
  }
  mEspLog[mEspLogLen++] = C;
}

VOID
EspLogAppendAscii (
  IN CONST CHAR8  *Str
  )
{
  if (Str == NULL) {
    return;
  }
  while (*Str != '\0') {
    EspLogPutc (*Str++);
  }
}

VOID
EspLogAppendUnicode (
  IN CONST CHAR16  *Str
  )
{
  if (Str == NULL) {
    return;
  }
  for (; *Str != L'\0'; Str++) {
    EspLogPutc ((*Str < 0x80) ? (CHAR8)*Str : '?');
  }
}

CONST CHAR8 *
EspLogData (
  OUT UINTN    *Len,
  OUT BOOLEAN  *Wrapped
  )
{
  *Len = mEspLogLen;
  *Wrapped = mEspLogWrapped;
  return mEspLog;
}

STATIC
EFI_STATUS
EFIAPI
EspLogOutputString (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN CHAR16                           *String
  )
{
  if (!mInHook) {
    EspLogAppendUnicode (String);
  }
  return mOrigOutputString (This, String);
}

EFI_STATUS
EspLogConsoleWrite (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *ConOut,
  IN CHAR16                           *Str
  )
{
  EFI_STATUS Status;

  mInHook = TRUE;
  Status = ConOut->OutputString (ConOut, Str);
  mInHook = FALSE;
  return Status;
}

VOID
EspLogHookConsole (
  VOID
  )
{
  if (mDebugST == NULL || mDebugST->ConOut == NULL || mOrigOutputString != NULL) {
    return;
  }
  mOrigOutputString = mDebugST->ConOut->OutputString;
  mDebugST->ConOut->OutputString = EspLogOutputString;
  EspLogAppendAscii ("[esp-log] console capture on\n");
}

/* Put the firmware console back.  Must run before this image is unloaded,
   otherwise ConOut->OutputString would point into freed memory. */
VOID
EspLogUnhookConsole (
  VOID
  )
{
  if (mOrigOutputString != NULL && mDebugST != NULL && mDebugST->ConOut != NULL &&
      mDebugST->ConOut->OutputString == EspLogOutputString) {
    mDebugST->ConOut->OutputString = mOrigOutputString;
  }
  mOrigOutputString = NULL;
}

/* Library destructor: runs when the image exits (EDK2 calls library
   destructors after the entry point returns). */
EFI_STATUS
EFIAPI
EspDebugLibDestructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EspLogUnhookConsole ();
  return EFI_SUCCESS;
}
