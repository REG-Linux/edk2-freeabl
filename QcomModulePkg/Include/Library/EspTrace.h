/** @file EspTrace.h

  DEBUG ONLY: trace what a chainloaded EFI image (GRUB) asks the firmware.
  Wraps Boot/Runtime Services, ConOut/ConIn and every BlockIo ReadBlocks,
  keeps the last calls made from the image (or from memory it allocated)
  in a ring, and draws them on the screen through GOP:
    - on a synchronous exception / SError (with ESR/ELR/FAR), then spins;
    - from a 1 s timer, 20 s after start and every 20 s after that.
  The ring is also copied into the EspLog when the image returns.

  Copyright (c) 2026, REG-Linux contributors.
  SPDX-License-Identifier: GPL-3.0-or-later
**/

#ifndef __ESP_TRACE_H__
#define __ESP_TRACE_H__

#include <Uefi.h>

VOID EspTraceStart (IN EFI_HANDLE ImageHandle);
VOID EspTraceStop  (VOID);

#endif
