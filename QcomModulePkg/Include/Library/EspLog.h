/** @file EspLog.h

  In-RAM boot log for the ESP-enabled LinuxLoader.  Collects DEBUG()/ASSERT()
  output (EspDebugLib) and, once EspLogHookConsole() ran, everything printed
  to the console.  Read it from a PC with `fastboot oem esp-log`.

  Copyright (c) 2026, REG-Linux contributors.
  SPDX-License-Identifier: GPL-3.0-or-later
**/

#ifndef __ESP_LOG_H__
#define __ESP_LOG_H__

#include <Uefi.h>
#include <Protocol/SimpleTextOut.h>

VOID         EspLogAppendAscii   (IN CONST CHAR8  *Str);
VOID         EspLogAppendUnicode (IN CONST CHAR16 *Str);
CONST CHAR8 *EspLogData          (OUT UINTN *Len, OUT BOOLEAN *Wrapped);
VOID         EspLogHookConsole   (VOID);
VOID         EspLogUnhookConsole (VOID);
/* Write to the console without being recorded twice. */
EFI_STATUS   EspLogConsoleWrite  (IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
                                  IN CHAR16 *Str);

/* ESP progress messages: on screen only in debug builds (ESP_DEBUG_SCREEN /
   QEMU); release builds keep them in the RAM log (fastboot oem esp-log) and
   leave the screen to the boot logo, as the stock ABL does. */
#ifdef ESP_DEBUG_CONOUT
#define EspPrint(...)  AsciiPrint (__VA_ARGS__)
#else
#define EspPrint(...)  DEBUG ((EFI_D_ERROR, __VA_ARGS__))
#endif

#endif
