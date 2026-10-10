/** @file EspCmdlineFile.h
    Optional on-disk kernel command-line override (cmdline.txt /
    cmdline-append.txt) read from the boot/data volume.

    Copyright (c) 2026, REG-Linux contributors.
    SPDX-License-Identifier: GPL-3.0-or-later
**/

#ifndef __ESP_CMDLINE_FILE_H__
#define __ESP_CMDLINE_FILE_H__

#include <Uefi.h>

/**
  Apply an optional on-disk command-line override.

  Looks for cmdline.txt (full replacement) then cmdline-append.txt (append)
  on every mounted SimpleFileSystem volume.  On success *Cmdline is replaced
  with a newly-allocated string and the previous buffer is freed.  If neither
  file exists, *Cmdline is left untouched and EFI_NOT_FOUND is returned, which
  callers may treat as a benign no-op.

  Intended for the non-Android (Linux/ESP) boot path only.

  @param[in,out] Cmdline  Pointer to the current heap-allocated command line.

  @retval EFI_SUCCESS           An override/append file was applied.
  @retval EFI_NOT_FOUND         Neither file present (no-op).
  @retval EFI_INVALID_PARAMETER Cmdline was NULL.
  @retval other                 Allocation error (original left intact).
**/
EFI_STATUS
EspApplyCmdlineFile (
  IN OUT CHAR8  **Cmdline
  );

#endif /* __ESP_CMDLINE_FILE_H__ */
