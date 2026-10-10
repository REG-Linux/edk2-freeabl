/** @file EspCmdlineFile.c
    Optional on-disk kernel command-line override.

    On the boot / data volume the user may drop one of two text files:

      cmdline.txt          — replaces the kernel command line entirely
      cmdline-append.txt   — appended to the existing kernel command line

    If cmdline.txt is present it wins and cmdline-append.txt is ignored.
    If neither file exists this is a silent no-op and the caller's command
    line is left untouched.

    Written against the standard EDK2 SimpleFileSystem API.  Only the
    non-Android (Linux/ESP) boot path should call this.

    Copyright (c) 2026, REG-Linux contributors.
    SPDX-License-Identifier: GPL-3.0-or-later
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

#include <Library/EspCmdlineFile.h>

/**
  Read an entire file, by name, from the first SimpleFileSystem volume that
  contains it.  Every mounted volume is probed in turn (SD card, internal
  storage, ...), so the caller does not need to know which volume the file
  lives on.

  The returned buffer is NUL-terminated and sanitised for use as a kernel
  command line: every CR and LF byte is turned into a space, and trailing
  whitespace is trimmed.  The caller owns the buffer and must FreePool it.

  @param[in]  FileName  File to look for, e.g. L"cmdline.txt".
  @param[out] FileData  Receives an allocated, NUL-terminated ASCII buffer.
  @param[out] FileLen   Receives the length in bytes (excluding the NUL).

  @retval EFI_SUCCESS    File found and read.
  @retval EFI_NOT_FOUND  File not present on any volume.
  @retval other          Allocation or I/O error.
**/
STATIC
EFI_STATUS
EspReadTextFileFromAnyVolume (
  IN  CONST CHAR16  *FileName,
  OUT CHAR8         **FileData,
  OUT UINTN         *FileLen
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_FILE_INFO                    *Info;
  UINTN                            InfoSize;
  UINTN                            ReadSize;
  CHAR8                            *Buffer;
  UINTN                            i;

  *FileData = NULL;
  *FileLen  = 0;

  Handles     = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status) || HandleCount == 0) {
    return EFI_NOT_FOUND;
  }

  Status = EFI_NOT_FOUND;

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Fs
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Root->Open (Root, &File, (CHAR16 *)FileName, EFI_FILE_MODE_READ, 0);
    Root->Close (Root);
    if (EFI_ERROR (Status)) {
      continue;
    }

    /* Query the file size (GetInfo with a NULL buffer returns the size). */
    InfoSize = 0;
    Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      File->Close (File);
      continue;
    }

    Info = AllocatePool (InfoSize);
    if (Info == NULL) {
      File->Close (File);
      Status = EFI_OUT_OF_RESOURCES;
      break;
    }

    Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
    if (EFI_ERROR (Status)) {
      FreePool (Info);
      File->Close (File);
      continue;
    }

    ReadSize = (UINTN)Info->FileSize;
    FreePool (Info);

    Buffer = AllocatePool (ReadSize + 1);
    if (Buffer == NULL) {
      File->Close (File);
      Status = EFI_OUT_OF_RESOURCES;
      break;
    }

    Status = File->Read (File, &ReadSize, Buffer);
    File->Close (File);
    if (EFI_ERROR (Status)) {
      FreePool (Buffer);
      continue;
    }

    /* Sanitise: turn line breaks into spaces so the text reads as a single
       command line, then trim trailing whitespace and NUL-terminate. */
    for (i = 0; i < ReadSize; i++) {
      if (Buffer[i] == '\r' || Buffer[i] == '\n') {
        Buffer[i] = ' ';
      }
    }
    while (ReadSize > 0 && Buffer[ReadSize - 1] == ' ') {
      ReadSize--;
    }
    Buffer[ReadSize] = '\0';

    *FileData = Buffer;
    *FileLen  = ReadSize;
    Status    = EFI_SUCCESS;
    break;
  }

  FreePool (Handles);
  return Status;
}

/**
  Apply an optional on-disk command-line override.  See file header.

  On success *Cmdline is replaced with a newly-allocated string and the old
  string is freed.  On no-op (neither file present) *Cmdline is untouched and
  EFI_NOT_FOUND is returned; callers may treat that as success.

  @param[in,out] Cmdline  Pointer to the current (heap-allocated) command line.

  @retval EFI_SUCCESS    An override/append file was applied.
  @retval EFI_NOT_FOUND  Neither cmdline.txt nor cmdline-append.txt present.
  @retval other          Allocation error (original *Cmdline left intact).
**/
EFI_STATUS
EspApplyCmdlineFile (
  IN OUT CHAR8  **Cmdline
  )
{
  EFI_STATUS  Status;
  CHAR8       *FileData;
  UINTN       FileLen;
  CHAR8       *Base;
  UINTN       BaseLen;
  UINTN       NewLen;
  CHAR8       *NewCmdline;

  if (Cmdline == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  /* 1. cmdline.txt fully replaces the command line. */
  Status = EspReadTextFileFromAnyVolume (L"cmdline.txt", &FileData, &FileLen);
  if (!EFI_ERROR (Status)) {
    DEBUG ((EFI_D_INFO, "EspCmdline: cmdline.txt overrides kernel cmdline\n"));
    if (*Cmdline != NULL) {
      FreePool (*Cmdline);
    }
    *Cmdline = FileData;   /* transfer ownership */
    return EFI_SUCCESS;
  }

  /* 2. cmdline-append.txt is appended to the existing command line. */
  Status = EspReadTextFileFromAnyVolume (L"cmdline-append.txt", &FileData, &FileLen);
  if (EFI_ERROR (Status)) {
    /* Neither file present — silent no-op. */
    return EFI_NOT_FOUND;
  }

  Base    = (*Cmdline != NULL) ? *Cmdline : (CHAR8 *)"";
  BaseLen = AsciiStrLen (Base);

  /* base + optional separating space + append + NUL */
  NewLen     = BaseLen + 1 + FileLen + 1;
  NewCmdline = AllocatePool (NewLen);
  if (NewCmdline == NULL) {
    FreePool (FileData);
    return EFI_OUT_OF_RESOURCES;
  }

  AsciiStrCpyS (NewCmdline, NewLen, Base);
  /* Insert a single space only if the base is non-empty and does not already
     end with one, so we do not create "foo  bar" or a leading space. */
  if (BaseLen > 0 && NewCmdline[BaseLen - 1] != ' ') {
    AsciiStrCatS (NewCmdline, NewLen, " ");
  }
  AsciiStrCatS (NewCmdline, NewLen, FileData);

  FreePool (FileData);
  if (*Cmdline != NULL) {
    FreePool (*Cmdline);
  }
  *Cmdline = NewCmdline;

  DEBUG ((EFI_D_INFO, "EspCmdline: appended cmdline-append.txt\n"));
  return EFI_SUCCESS;
}
