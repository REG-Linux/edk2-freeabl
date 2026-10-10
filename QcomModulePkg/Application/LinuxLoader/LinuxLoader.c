/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Copyright (c) 2009-2021, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of The Linux Foundation nor
 *       the names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *  Changes from Qualcomm Innovation Center are provided under the following license:
 *
 *  Copyright (c) 2022 - 2025 Qualcomm Innovation Center, Inc. All rights
 *  reserved.
 *  Modified for FreeABL, 2026 (see FREEABL.md).
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted (subject to the limitations in the
 *  disclaimer below) provided that the following conditions are met:
 *
 *      * Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      * Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials provided
 *        with the distribution.
 *
 *      * Neither the name of Qualcomm Innovation Center, Inc. nor the names of its
 *        contributors may be used to endorse or promote products derived
 *        from this software without specific prior written permission.
 *
 *  NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
 *  GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
 *  HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 *  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 *  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 *  IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "AutoGen.h"
#include "BootLinux.h"
#include "BootStats.h"
#include "KeyPad.h"
#include "LinuxLoaderLib.h"
#include <FastbootLib/FastbootMain.h>
#include <Library/DeviceInfo.h>
#include <Library/DrawUI.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/ShutdownServices.h>
#include <Library/StackCanary.h>
#include "Library/ThreadStack.h"
#include <Library/HypervisorMvCalls.h>
#include <Library/UpdateCmdLine.h>
#include <Protocol/EFICardInfo.h>
#include <Library/BootEsp.h>
#include <Library/BootModeMenu.h>
#include <Library/BootDispatch.h>
#include <Library/EspLog.h>

#define MAX_APP_STR_LEN 64
#define MAX_NUM_FS 10
#define DEFAULT_STACK_CHK_GUARD 0xc0c0c0c0

#if HIBERNATION_SUPPORT_NO_AES
VOID BootIntoHibernationImage (BootInfo *Info,
                               BOOLEAN *SetRotAndBootStateAndVBH);
#endif

BccParams_t BccParamsRecvdFromAVB = {{0}};
STATIC BOOLEAN BootReasonAlarm = FALSE;
STATIC BOOLEAN BootIntoFastboot = FALSE;
STATIC BOOLEAN BootIntoRecovery = FALSE;
UINT64 FlashlessBootImageAddr = 0;
STATIC DeviceInfo DevInfo;
STATIC UINT32 BootDeviceType = EFI_MAX_FLASH_TYPE;

// This function is used to Deactivate MDTP by entering recovery UI
STATIC EFI_STATUS MdtpDisable (VOID)
{
  BOOLEAN MdtpActive = FALSE;
  EFI_STATUS Status = EFI_SUCCESS;
  QCOM_MDTP_PROTOCOL *MdtpProtocol;

  if (FixedPcdGetBool (EnableMdtpSupport)) {
    Status = IsMdtpActive (&MdtpActive);

    if (EFI_ERROR (Status))
      return Status;

    if (MdtpActive) {
      Status = gBS->LocateProtocol (&gQcomMdtpProtocolGuid, NULL,
                                    (VOID **)&MdtpProtocol);
      if (EFI_ERROR (Status)) {
        DEBUG ((EFI_D_ERROR, "Failed to locate MDTP protocol, Status=%r\n",
                Status));
        return Status;
      }
      /* Perform Local Deactivation of MDTP */
      Status = MdtpProtocol->MdtpDeactivate (MdtpProtocol, FALSE);
    }
  }

  return Status;
}

STATIC UINT8
GetRebootReason (UINT32 *ResetReason)
{
  EFI_RESETREASON_PROTOCOL *RstReasonIf;
  EFI_STATUS Status;

  Status = gBS->LocateProtocol (&gEfiResetReasonProtocolGuid, NULL,
                                (VOID **)&RstReasonIf);
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Error locating the reset reason protocol\n"));
    return Status;
  }

  RstReasonIf->GetResetReason (RstReasonIf, ResetReason, NULL, NULL);
  if (RstReasonIf->Revision >= EFI_RESETREASON_PROTOCOL_REVISION)
    RstReasonIf->ClearResetReason (RstReasonIf);
  return Status;
}

STATIC VOID
SetDefaultAudioFw ()
{
  CHAR8 AudioFW[MAX_AUDIO_FW_LENGTH];
  STATIC CHAR8* Src;
  STATIC CHAR8* AUDIOFRAMEWORK;
  STATIC UINT32 Length;
  EFI_STATUS Status;

  AUDIOFRAMEWORK = GetAudioFw ();
  Status = ReadAudioFrameWork (&Src, &Length);
  if ((AsciiStrCmp (Src, "audioreach") == 0) ||
                              (AsciiStrCmp (Src, "elite") == 0) ||
                              (AsciiStrCmp (Src, "awe") == 0)) {
    if (Status == EFI_SUCCESS) {
      if (AsciiStrLen (Src) == 0) {
        if (AsciiStrLen (AUDIOFRAMEWORK) > 0) {
          AsciiStrnCpyS (AudioFW, MAX_AUDIO_FW_LENGTH, AUDIOFRAMEWORK,
          AsciiStrLen (AUDIOFRAMEWORK));
          StoreAudioFrameWork (AudioFW, AsciiStrLen (AUDIOFRAMEWORK));
        }
      }
    }
    else {
      DEBUG ((EFI_D_ERROR, "AUDIOFRAMEWORK is NOT updated length =%d, %a\n",
      Length, AUDIOFRAMEWORK));
    }
  }
  else {
    if (Src != NULL) {
      Status =
      ReadWriteDeviceInfo (READ_CONFIG, (VOID *)&DevInfo, sizeof (DevInfo));
      if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "Unable to Read Device Info: %r\n", Status));
       }
      gBS->SetMem (DevInfo.AudioFramework, sizeof (DevInfo.AudioFramework), 0);
      gBS->CopyMem (DevInfo.AudioFramework, AUDIOFRAMEWORK,
                                      AsciiStrLen (AUDIOFRAMEWORK));
      Status =
      ReadWriteDeviceInfo (WRITE_CONFIG, (VOID *)&DevInfo, sizeof (DevInfo));
      if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "Unable to store audio framework: %r\n", Status));
        return;
      }
    }
  }
}

BOOLEAN IsABRetryCountUpdateRequired (VOID)
{
 BOOLEAN BatteryStatus;

 /* Check power off charging */
 TargetPauseForBatteryCharge (&BatteryStatus);

 /* Do not decrement bootable retry count in below states:
 * fastboot, fastbootd, charger, recovery
 */
 if ((BatteryStatus &&
 IsChargingScreenEnable ()) ||
 BootIntoFastboot ||
 BootIntoRecovery) {
  return FALSE;
 }
  return TRUE;
}

/**
  This function is used to check for boot type:
    FlashlessBoot, NetworkBoot, Fastboot.
 **/

UINT32 GetBootDeviceType ()
{
  UINTN  DataSize = sizeof (BootDeviceType);
  EFI_STATUS Status = EFI_SUCCESS;

  if (BootDeviceType == EFI_MAX_FLASH_TYPE) {
    Status = gRT->GetVariable (L"SharedImemBootCfgVal",
               &gQcomTokenSpaceGuid, NULL, &DataSize, &BootDeviceType);
    if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "Failed to get boot device type, %r\n", Status));
    }
  }

  return BootDeviceType;
}

/**
  Linux Loader Application EntryPoint

  @param[in] ImageHandle    The firmware allocated handle for the EFI image.
  @param[in] SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS       The entry point is executed successfully.
  @retval other             Some error occurs when executing this entry point.

 **/

/*
  Read-only safety check before entering the Android boot path.

  The active slot's "boot" partition must start with an Android boot image
  header, and its "dtbo" partition, when the partition exists, must start with
  a DTBO table.  Otherwise the Android path can only fail, and failing there
  burns A/B retries and may mark the slot unbootable (GPT attribute writes).
  In that case we go to fastboot instead.  Nothing is written here.
*/
STATIC BOOLEAN
PartitionStartsWith (CHAR16 *Pname, CONST UINT8 *Magic, UINTN MagicLen,
                     BOOLEAN *Exists)
{
  VOID       *Buf;
  UINT32     Size = EFI_PAGE_SIZE;
  EFI_STATUS Status;
  BOOLEAN    Match = FALSE;

  *Exists = FALSE;
  Buf = AllocatePages (1);
  if (Buf == NULL) {
    return FALSE;
  }
  Status = LoadImageFromPartition (Buf, &Size, Pname);
  if (!EFI_ERROR (Status)) {
    *Exists = TRUE;
    Match = (CompareMem (Buf, Magic, MagicLen) == 0);
  } else if (Status != EFI_NO_MEDIA && Status != EFI_NOT_FOUND) {
    *Exists = TRUE;   /* present but unreadable: treat as invalid */
  }
  FreePages (Buf, 1);
  return Match;
}

STATIC BOOLEAN
ActiveSlotLooksBootable (BOOLEAN MultiSlotBoot)
{
  STATIC CONST UINT8 BootMagic[] = { 'A', 'N', 'D', 'R', 'O', 'I', 'D', '!' };
  STATIC CONST UINT8 DtboMagic[] = { 0xD7, 0xB7, 0xAB, 0x1E };
  CHAR16  BootName[MAX_GPT_NAME_SIZE] = L"boot";
  CHAR16  DtboName[MAX_GPT_NAME_SIZE] = L"dtbo";
  BOOLEAN Exists;

  if (MultiSlotBoot) {
    Slot Cur = GetCurrentSlotSuffix ();
    StrCatS (BootName, ARRAY_SIZE (BootName), Cur.Suffix);
    StrCatS (DtboName, ARRAY_SIZE (DtboName), Cur.Suffix);
  }

  BOOLEAN BootOk;
  BOOLEAN DtboExists;
  BOOLEAN DtboOk;

  BootOk = PartitionStartsWith (BootName, BootMagic, sizeof (BootMagic), &Exists);
  DtboOk = PartitionStartsWith (DtboName, DtboMagic, sizeof (DtboMagic), &DtboExists);
  if (!BootOk) {
    EspPrint ("ABL: %s has no Android boot image\n", BootName);
  }
  if (DtboExists && !DtboOk) {
    EspPrint ("ABL: %s is not a valid DTBO image\n", DtboName);
  }
  return EspSlotBootable (BootOk, DtboExists, DtboOk);
}

/* Wait up to TimeoutMs for the power key; returns early on press. */
STATIC VOID
WaitPowerKeyOrTimeout (UINT32 TimeoutMs)
{
  UINT32 Key;
  UINT32 Waited;

  for (Waited = 0; Waited < TimeoutMs; Waited += 50) {
    Key = SCAN_NULL;
    if (GetKeyPress (&Key) == EFI_SUCCESS && Key == SCAN_SUSPEND) {
      break;
    }
    MicroSecondDelay (50000);
  }
}

/*
  One-shot "boot Android on the next boot" request.  Used when Linux mode
  finds nothing to boot: SD card init has already disconnected/reconnected
  the BlockIo handles, so instead of entering the Android path in that state
  we reboot and let the next boot go straight to Android, skipping the menu
  and the SD init.  The variable is cleared as soon as it is read, so it
  can never cause a reboot loop.
*/
extern EFI_GUID gBootModeVariableGuid;
STATIC CHAR16 *mOneShotAndroidVar = L"EspOneShotAndroid";

STATIC BOOLEAN
TakeOneShotAndroid (VOID)
{
  UINT8      Value = 0;
  UINTN      Size  = sizeof (Value);
  EFI_STATUS Status;

  Status = gRT->GetVariable (mOneShotAndroidVar, &gBootModeVariableGuid,
                             NULL, &Size, &Value);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }
  gRT->SetVariable (mOneShotAndroidVar, &gBootModeVariableGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                    EFI_VARIABLE_RUNTIME_ACCESS, 0, NULL);
  return Value == 1;
}



EFI_STATUS EFIAPI  __attribute__ ( (no_sanitize ("safe-stack")))
LinuxLoaderEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  UINT32 BootReason = NORMAL_MODE;
  UINT32 KeyPressed = SCAN_NULL;
  /* SilentMode Boot */
  CHAR8 SilentBootMode = NON_SILENT_MODE;
  /* MultiSlot Boot */
  BOOLEAN MultiSlotBoot = FALSE;
  /* Flashless Boot */
  BOOLEAN FlashlessBoot = FALSE;
  EFI_MEM_CARDINFO_PROTOCOL *CardInfo = NULL;
  /* set ROT, BootState and VBH only once per boot*/
  BOOLEAN SetRotAndBootStateAndVBH = FALSE;
  BOOLEAN FDRDetected = FALSE;

  /* Record everything printed from here on (read: `fastboot oem esp-log`). */
  EspLogHookConsole ();

  DEBUG ((EFI_D_INFO, "Loader Build Info: %a %a\n", __DATE__, __TIME__));
  DEBUG ((EFI_D_VERBOSE, "LinuxLoader Load Address to debug ABL: 0x%llx\n",
         (UINTN)LinuxLoaderEntry & (~ (0xFFF))));
  DEBUG ((EFI_D_VERBOSE, "LinuxLoaderEntry Address: 0x%llx\n",
         (UINTN)LinuxLoaderEntry));

  BootStatsSetInitTimeStamp ();

  Status = InitThreadUnsafeStack ();

  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Unable to Allocate memory for Unsafe Stack: %r\n",
            Status));
    goto stack_guard_update_default;
  }

  StackGuardChkSetup ();

  BootStatsSetTimeStamp (BS_BL_START);

  /* Check if memory card is present; goto flashless if not */
  Status = gBS->LocateProtocol (&gEfiMemCardInfoProtocolGuid, NULL,
                                  (VOID **)&CardInfo);
  if (EFI_ERROR (Status)) {
    FlashlessBootImageAddr = BASE_ADDRESS;
    FlashlessBoot = TRUE;
    /* In flashless boot avoid all access to secondary storage during boot */
    goto flashless_boot;
  }

  // Initialize verified boot & Read Device Info
  Status = DeviceInfoInit ();
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Initialize the device info failed: %r\n", Status));
    goto stack_guard_update_default;
  }
  EspPrint ("ABL: DeviceInfo OK\n");

  Status = EnumeratePartitions ();

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "LinuxLoader: Could not enumerate partitions: %r\n",
            Status));
    goto stack_guard_update_default;
  }
  EspPrint ("ABL: Partitions OK\n");

  UpdatePartitionEntries ();
  /*Check for multislot boot support*/
  MultiSlotBoot = PartitionHasMultiSlot ((CONST CHAR16 *)L"boot");
  if (MultiSlotBoot) {
    DEBUG ((EFI_D_VERBOSE, "Multi Slot boot is supported\n"));
    FindPtnActiveSlot ();
  }

  Status = GetKeyPress (&KeyPressed);
  if (Status == EFI_SUCCESS) {
    if (KeyPressed == SCAN_DOWN)
      BootIntoFastboot = TRUE;
    if (KeyPressed == SCAN_UP)
      BootIntoRecovery = TRUE;
    if (KeyPressed == SCAN_ESC)
      RebootDevice (EMERGENCY_DLOAD);
  } else if (Status == EFI_DEVICE_ERROR) {
    DEBUG ((EFI_D_ERROR, "Error reading key status: %r\n", Status));
    goto stack_guard_update_default;
  }

  SetDefaultAudioFw ();

  // check for reboot mode
  Status = GetRebootReason (&BootReason);
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Failed to get Reboot reason: %r\n", Status));
    goto stack_guard_update_default;
  }

  switch (BootReason) {
  case FASTBOOT_MODE:
    BootIntoFastboot = TRUE;
    break;
  case RECOVERY_MODE:
    BootIntoRecovery = TRUE;
    break;
  case ALARM_BOOT:
    BootReasonAlarm = TRUE;
    break;
  case DM_VERITY_ENFORCING:
    // write to device info
    Status = EnableEnforcingMode (TRUE);
    if (Status != EFI_SUCCESS)
      goto stack_guard_update_default;
    break;
  case DM_VERITY_LOGGING:
    /* Disable MDTP if it's Enabled through Local Deactivation */
    Status = MdtpDisable ();
    if (EFI_ERROR (Status) && Status != EFI_NOT_FOUND) {
      DEBUG ((EFI_D_ERROR, "MdtpDisable Returned error: %r\n", Status));
      goto stack_guard_update_default;
    }
    // write to device info
    Status = EnableEnforcingMode (FALSE);
    if (Status != EFI_SUCCESS)
      goto stack_guard_update_default;

    break;
  case DM_VERITY_KEYSCLEAR:
    Status = ResetDeviceState ();
    if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_ERROR, "VB Reset Device State error: %r\n", Status));
      goto stack_guard_update_default;
    }
    break;
  case SILENT_MODE:
    SilentBootMode = SILENT_MODE;
    break;
  case NON_SILENT_MODE:
    SilentBootMode = NON_SILENT_MODE;
    break;
  case FORCED_SILENT:
    SilentBootMode = FORCED_SILENT;
    break;
  case FORCED_NON_SILENT:
    SilentBootMode = FORCED_NON_SILENT;
    break;
  default:
    if (BootReason != NORMAL_MODE) {
      DEBUG ((EFI_D_ERROR,
             "Boot reason: 0x%x not handled, defaulting to Normal Boot\n",
             BootReason));
    }
    break;
  }

  Status = RecoveryInit (&BootIntoRecovery);
  if (Status != EFI_SUCCESS)
    DEBUG ((EFI_D_VERBOSE, "RecoveryInit failed ignore: %r\n", Status));

   if (BootIntoRecovery) {
    Status = DetectFDR (&FDRDetected);
    if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_ERROR, "DetectFDR failed: %r\n", Status));
    }
    if (FDRDetected) {
      DEBUG ((EFI_D_INFO, "LinuxloaderEntry: FDRDetected\n"));
      Status = SetFDRFlag ();
      if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "SetFDRFlag failed: %r\n", Status));
      }
    }
  }

flashless_boot:
  /* Populate board data required for fastboot, dtb selection and cmd line */
  Status = BoardInit ();
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Error finding board information: %r\n", Status));
    return Status;
  }
  EspPrint ("ABL: Board init OK\n");


#ifdef ESP_DEBUG_CONOUT
  /* Boot pause (debug builds): hold power to read the boot messages */
  EspPrint ("PRESS POWER KEY TO PAUSE BOOT\n\n\n\n");
  {
    UINT32 PauseKey = SCAN_NULL;
    UINT32 PauseMs;
    for (PauseMs = 0; PauseMs < 1000; PauseMs += 50) {
      MicroSecondDelay (50000);
      if (GetKeyPress (&PauseKey) == EFI_SUCCESS && PauseKey == SCAN_SUSPEND) {
        EspPrint ("PRESS POWER KEY TO CONTINUE\n\n\n\n");
        /* Wait for power key release then press */
        MicroSecondDelay (500000);
        {
          UINT32 WaitCount;
          for (WaitCount = 0; WaitCount < 1200; WaitCount++) {
            PauseKey = SCAN_NULL;
            if (GetKeyPress (&PauseKey) == EFI_SUCCESS && PauseKey == SCAN_SUSPEND) {
              break;
            }
            MicroSecondDelay (50000);
          }
        }
        break;
      }
    }
  }
#endif

  DEBUG ((EFI_D_INFO, "KeyPress:%u, BootReason:%u\n", KeyPressed, BootReason));
  DEBUG ((EFI_D_INFO, "Fastboot=%d, Recovery:%d\n",
                                          BootIntoFastboot, BootIntoRecovery));
  DEBUG ((EFI_D_INFO, "SilentBoot Mode:%u\n", SilentBootMode));
  if (!GetVmData ()) {
    DEBUG ((EFI_D_ERROR, "VM Hyp calls not present\n"));
  }

  if (BootIntoFastboot) {
      goto fastboot;
  }
  else {
    /* ESP boot: routing decisions live in BootDispatch.c (host-tested). */
    BOOLEAN OneShot = TakeOneShotAndroid ();
    BOOLEAN MenuLinux = FALSE;
    BOOLEAN SdInitDone = FALSE;
    BOOLEAN LinuxNotFound = FALSE;

    for (;;) {
      if (OneShot) {
        EspPrint ("ABL: One-shot Android boot (no SD init)\n");
      } else {
        /* Big-font boot menu: Linux / Android / Fastboot. Needs no UEFI
           variable (the Odin firmware does not keep ours). After "Linux not
           found" it comes back with Android pre-selected. */
        ESP_BOOT_CHOICE Choice = DisplayBootChoiceMenu (LinuxNotFound);
        if (Choice == ESP_BOOT_FASTBOOT) {
          EspPrint ("ABL: Boot menu: Fastboot\n");
          goto fastboot;
        }
        MenuLinux = (Choice == ESP_BOOT_LINUX);
        EspPrint ("ABL: Boot mode: %a\n", MenuLinux ? "Linux" : "Android");
      }

      if (EspFirstAction (FALSE, OneShot, MenuLinux) != ESP_ACT_LINUX) {
        break;   /* Android */
      }

      if (!SdInitDone) {
        /* SD card init: disconnect BlockIO, signal GUIDs, reconnect.
           Linux path only; partitions are rescanned below before going on
           to Android or fastboot. */
        EspPrint ("ABL: Selected Linux - initializing SD...\n");
        /* Only needed for gBS->LoadImage of the EFI app. */
        EspPrint ("ABL: Installing security hooks...\n");
        InstallSecurityHooks ();
        InitSdCard ();
        SdInitDone = TRUE;
      }
      ScanSD ();
      EspPrint ("ABL: Loading BOOTAA64.EFI...\n");
      Status = StartEspBootLoader ();

      /* SD init disconnected/reconnected every block device: rescan the
         partition table so Android / fastboot do not use stale handles. */
      EnumeratePartitions ();
      UpdatePartitionEntries ();

      if (!EFI_ERROR (Status)) {
        /* The EFI app ran and came back: never fall into Android. */
        EspPrint ("ABL: Linux boot returned - entering fastboot\n");
        goto fastboot;
      }
      EspPrint ("ABL: EFI not found: %r\n", Status);
      LinuxNotFoundScreen ();
      LinuxNotFound = TRUE;
      OneShot = FALSE;
    }

    /* Android boot path */
    if (EspAndroidAction (ActiveSlotLooksBootable (MultiSlotBoot)) !=
        ESP_ACT_ANDROID) {
      EspPrint ("ABL: Android not bootable on this slot - entering fastboot\n");
      goto fastboot;
    }
    BootInfo Info = {0};
    Info.MultiSlotBoot = MultiSlotBoot;
    Info.BootIntoRecovery = BootIntoRecovery;
    Info.BootReasonAlarm = BootReasonAlarm;
    Info.FlashlessBoot = FlashlessBoot;
    Info.SilentBootMode = SilentBootMode;
  #if HIBERNATION_SUPPORT_NO_AES
    BootIntoHibernationImage (&Info, &SetRotAndBootStateAndVBH);
  #endif
    EspPrint ("ABL: LoadImageAndAuth...\n");
    Status = LoadImageAndAuth (&Info, FALSE, SetRotAndBootStateAndVBH
  #ifndef USE_DUMMY_BCC
                               , &BccParamsRecvdFromAVB
  #endif
                              );
    if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_ERROR, "LoadImageAndAuth failed: %r\n", Status));
      EspPrint ("ABL: LoadImageAndAuth FAILED: %r\n", Status);
      EspPrint ("ABL: entering fastboot in 10s (power key: now)\n");
      WaitPowerKeyOrTimeout (10000);
      goto fastboot;
    }

    EspPrint ("ABL: Booting kernel (partition: %s)...\n", Info.Pname);
    BootLinux (&Info);
  }

fastboot:
#ifdef AUTO_VIRT_ABL
  DEBUG ((EFI_D_INFO, "Rebooting the device.\n"));
  RebootDevice (NORMAL_MODE);
#endif
  if (FlashlessBoot) {
    DEBUG ((EFI_D_ERROR, "No fastboot support for flashless chipsets,"
                               " Infinte loop\n"));
    while (1);
  }
  DEBUG ((EFI_D_INFO, "Launching fastboot\n"));
  EspPrint ("ABL: Entering fastboot mode\n");
  Status = FastbootInitialize ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Launch Fastboot App: %d\n", Status));
    goto stack_guard_update_default;
  }

stack_guard_update_default:
  /*Update stack check guard with defualt value then return*/
  __stack_chk_guard = DEFAULT_STACK_CHK_GUARD;

  DeInitThreadUnsafeStack ();

  return Status;
}
