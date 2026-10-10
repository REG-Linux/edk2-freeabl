/** @file BootEsp.c
    ESP boot chain implementation.  Locates and launches
    \EFI\BOOT\BOOTAA64.EFI from any available EFI System Partition
    (SD card or internal storage).

    Copyright (c) 2026, REG-Linux contributors.
    SPDX-License-Identifier: GPL-3.0-or-later
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>
#include <Guid/FileInfo.h>
#include <Library/DrawUI.h>
#include <Library/MenuKeysDetection.h>

#include <Protocol/EFIDisplayPwr.h>
#include <Protocol/Security.h>
#include <Protocol/Security2.h>
#include <Guid/EventGroup.h>

#include <Library/BootEsp.h>
#include <Library/RawFat.h>
#include <Library/EspTrace.h>
#include <Library/EspLog.h>

/* ------------------------------------------------------------------ */
/* Security protocol hooks — allow loading unsigned EFI binaries      */
/* ------------------------------------------------------------------ */

/* These hooks bypass UEFI Secure Boot image verification for the ESP
   chainload path. Without them, gBS->LoadImage() rejects unsigned EFI
   binaries (like GRUB) with EFI_VOLUME_CORRUPTED or EFI_ACCESS_DENIED. */

STATIC EFI_SECURITY_FILE_AUTHENTICATION_STATE mOriginalSecurityHandler = NULL;
STATIC EFI_SECURITY2_FILE_AUTHENTICATION      mOriginalSecurity2Handler = NULL;

STATIC
EFI_STATUS
EFIAPI
SecurityStubHandler (
  IN CONST EFI_SECURITY_ARCH_PROTOCOL  *This,
  IN UINT32                            AuthenticationStatus,
  IN CONST EFI_DEVICE_PATH_PROTOCOL    *File
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Security2StubHandler (
  IN CONST EFI_SECURITY2_ARCH_PROTOCOL *This,
  IN CONST EFI_DEVICE_PATH_PROTOCOL    *DevicePath,
  IN VOID                              *FileBuffer,
  IN UINTN                             FileSize,
  IN BOOLEAN                           BootPolicy
  )
{
  return EFI_SUCCESS;
}

/**
  Install security protocol hooks that allow loading any EFI image.
  Must be called before LoadImage() is used to load GRUB or other
  unsigned EFI binaries.
**/
VOID
InstallSecurityHooks (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_SECURITY_ARCH_PROTOCOL    *Security;
  EFI_SECURITY2_ARCH_PROTOCOL   *Security2;

  /* Hook Security Arch Protocol */
  Status = gBS->LocateProtocol (
                  &gEfiSecurityArchProtocolGuid,
                  NULL,
                  (VOID **)&Security
                  );
  if (!EFI_ERROR (Status) && Security != NULL) {
    mOriginalSecurityHandler = Security->FileAuthenticationState;
    Security->FileAuthenticationState = SecurityStubHandler;
    EspPrint ("  Security hook installed\n");
  }

  /* Hook Security2 Arch Protocol */
  Status = gBS->LocateProtocol (
                  &gEfiSecurity2ArchProtocolGuid,
                  NULL,
                  (VOID **)&Security2
                  );
  if (!EFI_ERROR (Status) && Security2 != NULL) {
    mOriginalSecurity2Handler = Security2->FileAuthentication;
    Security2->FileAuthentication = Security2StubHandler;
    EspPrint ("  Security2 hook installed\n");
  }
}

/* ------------------------------------------------------------------ */
/* ExitBootServices display power-off callback                        */
/* ------------------------------------------------------------------ */

/**
  ExitBootServices notification: power the panel off.  Context is the
  display power protocol located when the notification was registered.
**/
STATIC
VOID
EFIAPI
PanelOffAtExit (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_DISPLAY_POWER_PROTOCOL  *Power;

  Power = Context;
  Power->SetDisplayPowerState (Power, EfiDisplayPowerStateOff);
}

/**
  Power the panel off when the started image calls ExitBootServices, so
  the kernel's display driver starts from a known state.  Does nothing
  when the firmware has no display power protocol.
**/
VOID
EspPanelOffAtExitBootServices (
  VOID
  )
{
  EFI_STATUS                  Status;
  EFI_DISPLAY_POWER_PROTOCOL  *Power;
  EFI_EVENT                   Event;

  Status = gBS->LocateProtocol (&gEfiDisplayPowerStateProtocolGuid, NULL,
                                (VOID **)&Power);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_WARN, "Panel stays on at exit: no display power protocol (%r)\n",
            Status));
    return;
  }

  Status = gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_CALLBACK,
                             PanelOffAtExit, Power, &Event);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_WARN, "Panel stays on at exit: %r\n", Status));
  }
}

/* ------------------------------------------------------------------ */

/* Boot target, looked up on every SimpleFileSystem volume: the UEFI
   removable-media path for arm64. */
STATIC CHAR16 *mBootAA64Paths[] = {
  L"\\EFI\\BOOT\\BOOTAA64.EFI",
};
#define BOOT_PATH_COUNT (sizeof (mBootAA64Paths) / sizeof (mBootAA64Paths[0]))

/* SD removable GUID - declared in LinuxLoader.inf (CLO stock) */
extern EFI_GUID gEfiSdRemovableGuid;

/* Qualcomm platform event the firmware's SD host driver waits for before
   it enumerates the card (see InitSdCard). */
STATIC EFI_GUID mQcomSdDetectEventGuid = {
  0xb7972c36, 0x8a4c, 0x4a56,
  { 0x8b, 0x02, 0x11, 0x59, 0xb5, 0x2d, 0x4b, 0xfb }
};

/* ------------------------------------------------------------------ */

/**
  Early SD/MMC initialization — signal the SD removable GUID events
  and reconnect all controllers so the SDHCI driver enumerates media.

  Must be called early in LinuxLoaderEntry, BEFORE boot mode selection,
  so the SD card has time to enumerate before the boot-target scan runs.
**/
/**
  Disconnect all existing BlockIO handles to ensure a clean slate
  for re-enumeration before scanning for boot media.
**/
STATIC
VOID
DisconnectBlockIoHandles (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *HandleBuffer;
  UINTN       HandleCount;
  UINTN       Index;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status) || HandleCount == 0) {
    EspPrint ("  DisconnectBlockIo: no handles\n");
    return;
  }

  EspPrint ("  DisconnectBlockIo: %u handles\n", (UINT32)HandleCount);

  for (Index = 0; Index < HandleCount; Index++) {
    gBS->DisconnectController (HandleBuffer[Index], NULL, NULL);
  }

  FreePool (HandleBuffer);
}

VOID
InitSdCard (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;

  /* Security hooks are installed earlier in LinuxLoaderEntry.  This only
     handles SD detection, and only on the Linux boot path. */

  /* Disconnect existing BlockIO handles for clean re-enumeration. */
  EspPrint ("  Disconnecting BlockIO...\n");
  DisconnectBlockIoHandles ();

  /* The SD host driver enumerates the card once these event groups are
     signalled: UEFI ReadyToBoot and EndOfDxe, the Qualcomm platform event
     and the CLO removable-media event. */
  EspPrint ("  Signalling SD detection events...\n");
  EfiSignalEventReadyToBoot ();
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);
  EfiEventGroupSignal (&mQcomSdDetectEventGuid);
  EfiEventGroupSignal (&gEfiSdRemovableGuid);

  /* Reconnect every controller so drivers bind to the new media. */
  EspPrint ("  Connecting controllers...\n");
  Status = gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < Count; Index++) {
      gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
    }
    FreePool (Handles);
  }

  EspPrint ("  InitSdCard done\n");
}

/**
  Scan for SD/MMC media by counting SimpleFileSystem handles.
  Should be called after InitSdCard().

  @retval TRUE   At least one filesystem handle exists.
  @retval FALSE  No filesystem handles found.
**/
BOOLEAN
ScanSD (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;

  Handles     = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (!EFI_ERROR (Status)) {
    FreePool (Handles);
  }

  EspPrint ("ABL: ScanSD: %u filesystem handle(s)\n", (UINT32)HandleCount);

  if (HandleCount > 0) {
    return TRUE;
  }

  EspPrint ("ABL: ScanSD: NO handles found!\n");
  return FALSE;
}

/* ------------------------------------------------------------------ */

/**
  Probe all SimpleFileSystem volumes for BOOTAA64.EFI.
  Looks for \EFI\BOOT\BOOTAA64.EFI.

  @param[out] FoundVolume  Receives the handle of the volume where
                           BOOTAA64.EFI was found.  May be NULL if
                           the caller does not need the handle.

  @retval TRUE   File exists on at least one volume.
  @retval FALSE  File not found anywhere.
**/
BOOLEAN
FindEspLoader (
  OUT EFI_HANDLE  *FoundVolume
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  UINTN                            PathIdx;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;

  if (FoundVolume != NULL) {
    *FoundVolume = NULL;
  }

  EspPrint ("ABL: FindEspLoader: scanning volumes...\n");

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
    EspPrint ("ABL: FindEspLoader: no FS handles (%r)\n", Status);
    return FALSE;
  }

  EspPrint ("ABL: FindEspLoader: %u volume(s) found\n", (UINT32)HandleCount);

  for (PathIdx = 0; PathIdx < BOOT_PATH_COUNT; PathIdx++) {
    for (Index = 0; Index < HandleCount; Index++) {
      Status = gBS->HandleProtocol (
                      Handles[Index],
                      &gEfiSimpleFileSystemProtocolGuid,
                      (VOID **)&Fs
                      );
      if (EFI_ERROR (Status)) {
        EspPrint ("  Vol %u: HandleProtocol failed: %r\n", (UINT32)Index, Status);
        continue;
      }

      Status = Fs->OpenVolume (Fs, &Root);
      if (EFI_ERROR (Status)) {
        EspPrint ("  Vol %u: OpenVolume failed: %r\n", (UINT32)Index, Status);
        continue;
      }

      EspPrint ("  Vol %u: trying %s\n", (UINT32)Index, mBootAA64Paths[PathIdx]);
      Status = Root->Open (
                       Root,
                       &File,
                       mBootAA64Paths[PathIdx],
                       EFI_FILE_MODE_READ,
                       0
                       );
      Root->Close (Root);

      if (!EFI_ERROR (Status)) {
        File->Close (File);
        EspPrint ("  Vol %u: FOUND %s\n", (UINT32)Index, mBootAA64Paths[PathIdx]);
        if (FoundVolume != NULL) {
          *FoundVolume = Handles[Index];
        }
        FreePool (Handles);
        return TRUE;
      } else {
        EspPrint ("  Vol %u: %s -> %r (0x%x)\n", (UINT32)Index, mBootAA64Paths[PathIdx], Status, (UINT32)Status);
      }
    }
  }

  EspPrint ("ABL: FindEspLoader: NOT FOUND on any volume\n");
  FreePool (Handles);
  return FALSE;
}

/* ------------------------------------------------------------------ */

/**
  Build a device path for \EFI\BOOT\BOOTAA64.EFI on the volume that has
  it, then LoadImage + StartImage.

  @retval EFI_SUCCESS    Image started successfully.
  @retval EFI_NOT_FOUND  BOOTAA64.EFI not on any volume.
  @retval other          Load or start error.
**/
EFI_STATUS
StartEspBootLoader (
  VOID
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                Volume = NULL;
  EFI_DEVICE_PATH_PROTOCOL  *FilePath;
  EFI_HANDLE                ImageHandle;
  UINTN                     PathIdx;

  EspPrint ("ABL: StartEspBootLoader\n");

  Status = EFI_NOT_FOUND;
  if (!FindEspLoader (&Volume)) {
    /* No mounted filesystem has it.  The firmware FAT driver may simply not
       have bound the card (partition type, hybrid MBR, FAT layout it
       rejects): still try the raw FAT reader below. */
    EspPrint ("ABL: BOOTAA64.EFI not on any mounted volume\n");
  }

  /* Filesystem-based LoadImage with a valid DevicePath: the DevicePath tells
     GRUB which volume it was loaded from so it can find grub.cfg at
     $prefix/grub.cfg on that same volume. */
  for (PathIdx = 0; Volume != NULL && PathIdx < BOOT_PATH_COUNT; PathIdx++) {
    FilePath = FileDevicePath (Volume, mBootAA64Paths[PathIdx]);
    if (FilePath == NULL) {
      continue;
    }

    EspPrint ("ABL: LoadImage %s\n", mBootAA64Paths[PathIdx]);
    ImageHandle = NULL;
    Status = gBS->LoadImage (
                    FALSE,               /* BootPolicy = FALSE */
                    gImageHandle,        /* ParentImageHandle */
                    FilePath,            /* DevicePath — tells GRUB its root volume */
                    NULL,                /* SourceBuffer = NULL (load from path) */
                    0,                   /* SourceSize = 0 */
                    &ImageHandle
                    );
    FreePool (FilePath);

    if (!EFI_ERROR (Status)) {
      EspPrint ("ABL: Loaded OK\n");
      break;
    }
    EspPrint ("ABL: LoadImage: %r (0x%x)\n", Status, (UINT32)Status);
  }

  if (EFI_ERROR (Status)) {
    /* Last-resort fallback: the Qualcomm SimpleFileSystem (FAT) driver is
       known to sometimes refuse to mount a card it should accept (the
       volume is reported as corrupted), which makes it invisible
       to FindEspLoader above even though the file is really there. Read it
       directly over BlockIo instead, bypassing SimpleFileSystem entirely.

       The image is loaded from memory but still gets the partition's device
       path, so GRUB can find its root and $prefix/grub.cfg by itself. */
    EspPrint ("ABL: trying raw FAT fallback...\n");

    for (PathIdx = 0; PathIdx < BOOT_PATH_COUNT; PathIdx++) {
      VOID        *RawData;
      UINTN       RawSize;
      EFI_HANDLE  Part = NULL;

      Status = RawFatReadFileEx (NULL, mBootAA64Paths[PathIdx], &RawData, &RawSize, &Part);
      if (EFI_ERROR (Status)) {
        continue;
      }

      EspPrint ("ABL: RawFat read %s (%u bytes), loading it from memory\n",
                  mBootAA64Paths[PathIdx], (UINT32)RawSize);

      /* Give the image the partition's device path + file path, so GRUB
         knows its root partition (it reads it with its own FAT driver over
         BlockIo) and finds $prefix/grub.cfg there. */
      FilePath = (Part != NULL) ? FileDevicePath (Part, mBootAA64Paths[PathIdx]) : NULL;

      ImageHandle = NULL;
      Status = gBS->LoadImage (
                      FALSE,          /* BootPolicy = FALSE */
                      gImageHandle,   /* ParentImageHandle */
                      FilePath,       /* partition + file path (may be NULL) */
                      RawData,        /* SourceBuffer */
                      RawSize,        /* SourceSize */
                      &ImageHandle
                      );
      FreePool (RawData);   /* LoadImage does not take ownership of SourceBuffer */
      if (FilePath != NULL) {
        FreePool (FilePath);
      }

      if (!EFI_ERROR (Status)) {
        EspPrint ("ABL: RawFat image loaded\n");
        break;
      }
      EspPrint ("ABL: RawFat image load failed: %r (0x%x)\n", Status, (UINT32)Status);
    }
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  EspPrint ("ABL: Image loaded, starting...\n");

  /* Start the image */
  /* Display off at ExitBootServices, for mainline DRM. Only once an image
     really starts: Android (stock kernel, continuous splash) needs the panel
     left running, also when chosen after a failed Linux attempt. */
#if defined (ESP_DISPLAY_OFF_AT_EBS) && !defined (ESP_DEBUG_CONOUT)
  /* Per SoC (build.sh): the AYN Odin's mainline DRM wants the panel off. The
     Mangmi Pocket Max's display never comes back if it is powered off here,
     so SM8250 / SM6115 leave it on. Debug builds always leave it on, so a
     kernel with efifb / earlycon=efifb can show its boot log on screen. */
  EspPanelOffAtExitBootServices ();
#endif
  EspTraceStart (ImageHandle);
  Status = gBS->StartImage (ImageHandle, NULL, NULL);
  EspTraceStop ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "StartEspBootLoader: StartImage failed: %r\n", Status));
  }

  return Status;
}

/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */

STATIC OPTION_MENU_INFO mNotFoundMenuInfo;

STATIC MENU_MSG_INFO mLinuxNotFoundMsgs[] = {
  {{"No Linux on SD card\n"},
    BIG_FACTOR, BGR_RED, BGR_BLACK, COMMON, 0, NOACTION},
  {{"\nNo bootable EFI image found on any volume.\n"},
    COMMON_FACTOR, BGR_WHITE, BGR_BLACK, COMMON, 0, NOACTION},
  {{"Insert an SD card with Linux, or choose Android.\n"},
    COMMON_FACTOR, BGR_WHITE, BGR_BLACK, COMMON, 0, NOACTION},
  {{"\nPress Power to return to the boot menu.\n"},
    COMMON_FACTOR, BGR_GREEN, BGR_BLACK, OPTION_ITEM, 0, CONTINUE},
};

#define NOTFOUND_MSG_COUNT \
  (sizeof (mLinuxNotFoundMsgs) / sizeof (mLinuxNotFoundMsgs[0]))

/**
  Display the "No Linux on SD card" error screen.

  Shows an on-screen warning when no bootable EFI image is found
  on any volume. Power key or 5-second timeout returns to caller
  so Android boot can proceed.
**/
VOID
LinuxNotFoundScreen (
  VOID
  )
{
  OPTION_MENU_INFO  *MenuInfo;
  UINT32            Location;
  UINT32            Height;
  UINT32            i;
  UINT32            j;

  MenuInfo = &mNotFoundMenuInfo;
  Location = 0;
  Height   = 0;

  DrawMenuInit ();

  ZeroMem (&MenuInfo->Info, sizeof (MENU_OPTION_ITEM_INFO));
  MenuInfo->Info.MsgInfo = mLinuxNotFoundMsgs;

  j = 0;
  for (i = 0; i < NOTFOUND_MSG_COUNT; i++) {
    if (mLinuxNotFoundMsgs[i].Attribute == OPTION_ITEM) {
      MenuInfo->Info.OptionItems[j] = i;
      j++;
    }
    mLinuxNotFoundMsgs[i].Location = Location;
    DrawMenu (&mLinuxNotFoundMsgs[i], &Height);
    Location += Height;
  }

  MenuInfo->Info.OptionNum   = j;
  MenuInfo->Info.OptionIndex = 0;
  MenuInfo->Info.MenuType    = DISPLAY_MENU_MORE_OPTION;
  MenuInfo->Info.TimeoutTime = 5;

  MenuKeysDetectionInit (MenuInfo);
  WaitForExitKeysDetection ();
}
