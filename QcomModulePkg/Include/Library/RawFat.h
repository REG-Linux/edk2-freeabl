/** @file RawFat.h
    Minimal raw FAT16/FAT32 reader used as a fallback when the Qualcomm
    SimpleFileSystem (FAT) driver refuses to bind a card or partition.

    Copyright (c) 2026, REG-Linux contributors.
    SPDX-License-Identifier: GPL-3.0-or-later
**/

#ifndef __RAW_FAT_H__
#define __RAW_FAT_H__

#include <Uefi.h>

/**
  Read a file by path directly over BlockIo, bypassing the SimpleFileSystem
  driver.  Tries the BlockIo behind FsHandleHint first (if given), then probes
  every BlockIo handle in the system.

  Supports FAT16 and FAT32 with 8.3 short names and sub-directory paths.  The
  path uses backslash separators, e.g. L"\\EFI\\BOOT\\BOOTAA64.EFI".

  The returned buffer is allocated with AllocatePool, holds *OutSize bytes and
  is NUL-terminated one byte past the end (convenient for text files).  The
  caller owns it and must FreePool it.

  @param[in]  FsHandleHint  Optional handle whose BlockIo to try first.
  @param[in]  Path          Backslash-separated file path.
  @param[out] OutData       Receives the allocated file buffer.
  @param[out] OutSize       Receives the file size in bytes.

  @retval EFI_SUCCESS            File read successfully.
  @retval EFI_NOT_FOUND          File not found on any FAT volume.
  @retval EFI_INVALID_PARAMETER  NULL Path/OutData/OutSize.
  @retval other                  Allocation or BlockIo error.
**/
EFI_STATUS
RawFatReadFile (
  IN  EFI_HANDLE    FsHandleHint  OPTIONAL,
  IN  CONST CHAR16  *Path,
  OUT VOID          **OutData,
  OUT UINTN         *OutSize
  );

/**
  Like RawFatReadFile(); also returns the BlockIo (partition) handle the
  file was found on, or leaves *FromHandle untouched on failure.
**/
EFI_STATUS
RawFatReadFileEx (
  IN  EFI_HANDLE    FsHandleHint  OPTIONAL,
  IN  CONST CHAR16  *Path,
  OUT VOID          **OutData,
  OUT UINTN         *OutSize,
  OUT EFI_HANDLE    *FromHandle   OPTIONAL
  );

#endif /* __RAW_FAT_H__ */
