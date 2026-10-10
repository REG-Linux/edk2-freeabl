/** @file BootModeMenu.c
    Boot mode persistence using a UEFI non-volatile runtime variable,
    and interactive on-screen boot mode selection menu.

    The variable L"BootModeLinux" stores a single UINT8:
      1 = Linux (ESP/GRUB), 0 = Android.
    Default when unset: TRUE (Linux mode).

    Copyright (c) 2026, REG-Linux contributors.
    SPDX-License-Identifier: GPL-3.0-or-later
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DrawUI.h>
#include <Library/MenuKeysDetection.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/KeyPad.h>
#include <Library/ShutdownServices.h>
#include <Library/Board.h>
#include <Protocol/EFIRamPartition.h>

#include <Library/BootModeMenu.h>
#include <Library/DistroPartition.h>

/* ------------------------------------------------------------------ */
/* Boot mode variable persistence                                     */
/* ------------------------------------------------------------------ */

/* Variable name and vendor GUID (declared in QcomModulePkg.dec) */
STATIC CHAR16  *mBootModeVarName = L"BootModeLinux";
extern EFI_GUID gBootModeVariableGuid;

/* NV + BS + RT so the variable survives reboot and is accessible
   at both boot-services and runtime. */
#define BOOT_MODE_ATTRIBUTES \
  (EFI_VARIABLE_NON_VOLATILE | \
   EFI_VARIABLE_BOOTSERVICE_ACCESS | \
   EFI_VARIABLE_RUNTIME_ACCESS)

/**
  Read the persisted boot mode.
  Returns TRUE (Linux) if the variable is missing or cannot be read.
**/
BOOLEAN
GetBootModeLinux (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Value;
  UINTN       Size;

  Size   = sizeof (Value);
  Value  = 1; /* default: Linux */

  Status = gRT->GetVariable (
                  mBootModeVarName,
                  &gBootModeVariableGuid,
                  NULL,
                  &Size,
                  &Value
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_INFO, "GetBootModeLinux: Variable not found, defaulting to Linux\n"));
    return TRUE;
  }

  DEBUG ((EFI_D_INFO, "GetBootModeLinux: %a\n", Value ? "Linux" : "Android"));
  return (Value != 0);
}

/**
  Persist the boot mode.

  @param[in] LinuxMode  TRUE = Linux, FALSE = Android.
**/
EFI_STATUS
SetBootModeLinux (
  IN BOOLEAN  LinuxMode
  )
{
  UINT8  Value;

  Value = LinuxMode ? 1 : 0;

  DEBUG ((EFI_D_INFO, "SetBootModeLinux: Setting to %a\n",
          LinuxMode ? "Linux" : "Android"));

  return gRT->SetVariable (
                mBootModeVarName,
                &gBootModeVariableGuid,
                BOOT_MODE_ATTRIBUTES,
                sizeof (Value),
                &Value
                );
}

/* ------------------------------------------------------------------ */
/* System Stats helpers                                               */
/* ------------------------------------------------------------------ */

/* Chip IDs as reported by the firmware: the numbering of Linux's
   dt-bindings/arm/qcom,ids.h, plus 601 (read on an AYANEO Pocket S). */
CONST CHAR8 *
GetSocName (
  IN UINT32  ChipId
  )
{
  STATIC CHAR8  Unknown[40];

  switch (ChipId) {
    case 321:  return "SDM845 (Snapdragon 845)";
    case 444:  return "SM6115 (Snapdragon 662)";
    case 356:  return "SM8250 (Snapdragon 865)";
    case 519:  return "SM8550 (Snapdragon 8 Gen 2)";
    case 601:  return "Snapdragon G3x Gen 2 (SM8550 family)";
    case 603:  return "QCS8550";
    case 604:  return "QCM8550";
    case 557:  return "SM8650 (Snapdragon 8 Gen 3)";
    case 618:  return "SM8750 (Snapdragon 8 Elite)";
    default:
      AsciiSPrint (Unknown, sizeof (Unknown), "Unrecognised chipset (id %u)", ChipId);
      return Unknown;
  }
}

UINT32
GetDdrSizeGB (
  VOID
  )
{
  EFI_STATUS                 Status;
  EFI_RAMPARTITION_PROTOCOL  *RamPartProt;
  RamPartitionEntry          *RamPartEntry;
  UINT32                     NumPartitions;
  UINT64                     TotalRam;
  UINT32                     i;

  RamPartProt   = NULL;
  NumPartitions = 0;
  TotalRam      = 0;

  Status = gBS->LocateProtocol (
                  &gEfiRamPartitionProtocolGuid,
                  NULL,
                  (VOID **)&RamPartProt
                  );
  if (EFI_ERROR (Status) || RamPartProt == NULL) {
    return 0;
  }

  Status = RamPartProt->GetRamPartitions (RamPartProt, NULL, &NumPartitions);
  if (Status != EFI_BUFFER_TOO_SMALL || NumPartitions == 0) {
    return 0;
  }

  RamPartEntry = AllocateZeroPool (NumPartitions * sizeof (RamPartitionEntry));
  if (RamPartEntry == NULL) {
    return 0;
  }

  Status = RamPartProt->GetRamPartitions (
                           RamPartProt,
                           RamPartEntry,
                           &NumPartitions
                           );
  if (EFI_ERROR (Status)) {
    FreePool (RamPartEntry);
    return 0;
  }

  for (i = 0; i < NumPartitions; i++) {
    TotalRam += RamPartEntry[i].AvailableLength;
  }

  FreePool (RamPartEntry);
  return (UINT32)(TotalRam / (1024ULL * 1024 * 1024));
}

/* ------------------------------------------------------------------ */
/* Boot mode selection menu                                           */
/* ------------------------------------------------------------------ */

/* Boot mode menu is text-only (AsciiPrint), not the graphical DrawMenu —
   this preserves ConOut text mode for subsequent debug output. */

/* ------------------------------------------------------------------ */
/* Text-mode boot menu and system stats helpers                       */
/* ------------------------------------------------------------------ */

/* PopulateSystemStats removed — text menu handles stats inline */

/**
  Switch to the alternate A/B slot and reboot.
  Does not return on success.
**/
STATIC
VOID
SwitchToAlternateSlot (
  VOID
  )
{
  Slot        CurrentSlot;
  Slot        NewSlot;
  EFI_STATUS  Status;

  CurrentSlot = GetCurrentSlotSuffix ();
  if (CurrentSlot.Suffix[0] == 0) {
    DEBUG ((EFI_D_ERROR, "SwitchToAlternateSlot: No active slot\n"));
    return;
  }

  ZeroMem (&NewSlot, sizeof (Slot));
  if (StrnCmp (CurrentSlot.Suffix, L"_a", 2) == 0) {
    StrnCpyS (NewSlot.Suffix, MAX_SLOT_SUFFIX_SZ, L"_b", 2);
  } else {
    StrnCpyS (NewSlot.Suffix, MAX_SLOT_SUFFIX_SZ, L"_a", 2);
  }

  DEBUG ((EFI_D_INFO, "SwitchToAlternateSlot: Switching from %s to %s\n",
          CurrentSlot.Suffix, NewSlot.Suffix));

  Status = SetActiveSlot (&NewSlot, TRUE);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SwitchToAlternateSlot: SetActiveSlot failed: %r\n",
            Status));
    return;
  }

  RebootDevice (NORMAL_MODE);
  /* Should not return */
}

/**
  Display the boot mode selection menu.

  Shows "Android", "Linux (ESP)", and optionally "Alternate Slot" options,
  followed by System Stats (SoC, RAM, Storage).
  Pre-selects the current persisted mode. 3-second timeout auto-continues
  with the current mode. Vol Up/Down navigate, Power confirms.

  If the user changes the selection, the new mode is persisted.
  If "Alternate Slot" is selected, the active slot is switched and
  the device reboots (this function does not return in that case).

  @retval TRUE   Linux (ESP) mode selected.
  @retval FALSE  Android mode selected.
**/
/**
  Draw the boot mode menu using pure AsciiPrint (text mode), not the
  graphical DrawMenu — this keeps ConOut in text mode for subsequent
  debug output.
**/
STATIC
VOID
DrawTextMenu (
  IN UINT32        Selected,
  IN UINT32        NumOptions,
  IN BOOLEAN       IsMultiSlot,
  IN UINT32        ChipId,
  IN UINT32        RamGB,
  IN CHAR8         *StorageType,
  IN CONST CHAR16  *DistroName  OPTIONAL
  )
{
  gST->ConOut->ClearScreen (gST->ConOut);
  AsciiPrint ("\n");
  AsciiPrint ("   LINUX ABL\n");
  AsciiPrint ("   =========\n\n");
  AsciiPrint ("   BOOT MODE\n\n");
  AsciiPrint ("   Vol keys to select, Power to confirm (5s timeout)\n\n");

  AsciiPrint ("   %a [0] Android\n", Selected == 0 ? ">>" : "  ");
  AsciiPrint ("   %a [1] Linux (ESP)\n", Selected == 1 ? ">>" : "  ");
  if (IsMultiSlot) {
    AsciiPrint ("   %a [2] Alternate Slot\n", Selected == 2 ? ">>" : "  ");
  }
  AsciiPrint ("\n");
  AsciiPrint ("   CHIPSET - %a\n", GetSocName (ChipId));
  if (RamGB > 0) {
    AsciiPrint ("   MEMORY - %d GB\n", RamGB);
  }
  AsciiPrint ("   STORAGE TYPE - %a\n", StorageType);
  if (DistroName != NULL) {
    AsciiPrint ("   DISTRO - %s\n", DistroName);
  }
}

BOOLEAN
DisplayBootModeMenu (
  VOID
  )
{
  BOOLEAN                  IsMultiSlot;
  BOOLEAN                  CurrentMode;
  UINT32                   Selected;
  UINT32                   NumOptions;
  UINT32                   ChipId;
  UINT32                   RamGB;
  CHAR8                    StorageType[16];
  UINT32                   TimeoutMs;
  UINT32                   KeyPressed;
  DISTRO_PARTITION_RESULT  DistroResult;
  CONST CHAR16             *DistroName;

  IsMultiSlot = PartitionHasMultiSlot ((CONST CHAR16 *)L"boot");
  CurrentMode = GetBootModeLinux ();
  /* "Alternate Slot" entry disabled: it writes GPT slot attributes.
     Switch slots with `fastboot --set-active` instead. */
  IsMultiSlot = FALSE;
  NumOptions  = 2;
  Selected    = CurrentMode ? 1 : 0;

  /* Gather system stats */
  ChipId = BoardPlatformRawChipId ();
  RamGB  = GetDdrSizeGB ();
  GetRootDeviceType (StorageType, sizeof (StorageType));

  /* Detect known CFW distro partition scheme once (read-only GPT scan, no
     destructive action) — purely informational, shown in System Stats.
     Not re-run on every redraw: it is a full disk scan, too slow to repeat
     on every key press. */
  DistroName = NULL;
  if (!EFI_ERROR (DetectDistroPartitionScheme (&DistroResult))) {
    DistroName = DistroResult.Scheme->DistroName;
  }

  /* Draw initial menu */
  DrawTextMenu (Selected, NumOptions, IsMultiSlot, ChipId, RamGB, StorageType, DistroName);

  /* Poll for key input with 3-second timeout */
  for (TimeoutMs = 0; TimeoutMs < 5000; TimeoutMs += 100) {
    KeyPressed = SCAN_NULL;
    if (GetKeyPress (&KeyPressed) == EFI_SUCCESS) {
      if (KeyPressed == SCAN_UP) {
        if (Selected > 0) Selected--;
        DrawTextMenu (Selected, NumOptions, IsMultiSlot, ChipId, RamGB, StorageType, DistroName);
        TimeoutMs = 0;  /* reset timeout on key press */
      } else if (KeyPressed == SCAN_DOWN) {
        if (Selected < NumOptions - 1) Selected++;
        DrawTextMenu (Selected, NumOptions, IsMultiSlot, ChipId, RamGB, StorageType, DistroName);
        TimeoutMs = 0;
      } else if (KeyPressed == SCAN_SUSPEND) {
        /* Power key = confirm */
        break;
      }
    }
    MicroSecondDelay (100000);  /* 100ms */
  }

  AsciiPrint ("\n");

  if (Selected == 2 && IsMultiSlot) {
    AsciiPrint ("ABL: Switching to alternate slot...\n");
    SwitchToAlternateSlot ();
    return FALSE;
  }

  BOOLEAN NewMode = (Selected == 1);

  if (NewMode != CurrentMode) {
    SetBootModeLinux (NewMode);
  }

  return NewMode;
}

/* ------------------------------------------------------------------ */
/* Big-font boot menu                                                 */
/* ------------------------------------------------------------------ */

#define BOOT_MENU_TIMEOUT_MS  4000

/* Index in mBootChoiceMsgs of each choice (same order as ESP_BOOT_CHOICE). */
STATIC CONST UINT32 mBootChoiceItem[] = { 3, 2, 4 };

STATIC MENU_MSG_INFO mBootChoiceMsgs[] = {
  {{"BOOT\n"},
    BIG_FACTOR, BGR_WHITE, BGR_BLACK, COMMON, 0, NOACTION},
  {{"Volume keys to select, Power to start\n\n"},
    COMMON_FACTOR, BGR_WHITE, BGR_BLACK, COMMON, 0, NOACTION},
  {{"Linux (SD card)"},
    BIG_FACTOR, BGR_GREEN, BGR_BLACK, COMMON, 0, NOACTION},
  {{"Android"},
    BIG_FACTOR, BGR_CYAN, BGR_BLACK, COMMON, 0, NOACTION},
  {{"Fastboot"},
    BIG_FACTOR, BGR_YELLOW, BGR_BLACK, COMMON, 0, NOACTION},
  {{"\n"},
    COMMON_FACTOR, BGR_WHITE, BGR_BLACK, COMMON, 0, NOACTION},
  {{"CHIPSET - "},
    COMMON_FACTOR, BGR_WHITE, BGR_BLACK, COMMON, 0, NOACTION},
  {{"MEMORY - "},
    COMMON_FACTOR, BGR_WHITE, BGR_BLACK, COMMON, 0, NOACTION},
  {{"STORAGE TYPE - "},
    COMMON_FACTOR, BGR_WHITE, BGR_BLACK, COMMON, 0, NOACTION},
};

#define BOOT_CHOICE_MSG_COUNT \
  (sizeof (mBootChoiceMsgs) / sizeof (mBootChoiceMsgs[0]))

ESP_BOOT_CHOICE
DisplayBootChoiceMenu (
  IN BOOLEAN  PreferAndroid
  )
{
  UINT32           Location = 0;
  UINT32           Height   = 0;
  UINT32           i;
  UINT32           RamGB;
  CHAR8            StorageType[16];
  ESP_BOOT_CHOICE  Selected;
  ESP_BOOT_CHOICE  Previous;
  UINT32           TimeoutMs;
  UINT32           KeyPressed;

  Selected = (!PreferAndroid && GetBootModeLinux ()) ? ESP_BOOT_LINUX : ESP_BOOT_ANDROID;

  AsciiSPrint (mBootChoiceMsgs[6].Msg, sizeof (mBootChoiceMsgs[6].Msg),
               "CHIPSET - %a\n", GetSocName (BoardPlatformRawChipId ()));
  RamGB = GetDdrSizeGB ();
  if (RamGB > 0) {
    AsciiSPrint (mBootChoiceMsgs[7].Msg, sizeof (mBootChoiceMsgs[7].Msg),
                 "MEMORY - %d GB\n", RamGB);
  } else {
    AsciiSPrint (mBootChoiceMsgs[7].Msg, sizeof (mBootChoiceMsgs[7].Msg),
                 "MEMORY - N/A\n");
  }
  GetRootDeviceType (StorageType, sizeof (StorageType));
  AsciiSPrint (mBootChoiceMsgs[8].Msg, sizeof (mBootChoiceMsgs[8].Msg),
               "STORAGE TYPE - %a\n", StorageType);

  /* Backs up the boot logo, clears the screen. */
  DrawMenuInit ();
  for (i = 0; i < BOOT_CHOICE_MSG_COUNT; i++) {
    mBootChoiceMsgs[i].Location = Location;
    DrawMenu (&mBootChoiceMsgs[i], &Height);
    Location += Height;
  }
  UpdateMsgBackground (&mBootChoiceMsgs[mBootChoiceItem[Selected]], BGR_BLUE);

  for (TimeoutMs = 0; TimeoutMs < BOOT_MENU_TIMEOUT_MS; TimeoutMs += 100) {
    KeyPressed = SCAN_NULL;
    if (GetKeyPress (&KeyPressed) == EFI_SUCCESS) {
      Previous = Selected;
      if (KeyPressed == SCAN_UP && Selected != ESP_BOOT_LINUX) {
        /* Screen order: Linux, Android, Fastboot */
        Selected = (Selected == ESP_BOOT_FASTBOOT) ? ESP_BOOT_ANDROID : ESP_BOOT_LINUX;
      } else if (KeyPressed == SCAN_DOWN && Selected != ESP_BOOT_FASTBOOT) {
        Selected = (Selected == ESP_BOOT_LINUX) ? ESP_BOOT_ANDROID : ESP_BOOT_FASTBOOT;
      } else if (KeyPressed == SCAN_SUSPEND) {
        break;
      }
      if (Selected != Previous) {
        UpdateMsgBackground (&mBootChoiceMsgs[mBootChoiceItem[Previous]], BGR_BLACK);
        UpdateMsgBackground (&mBootChoiceMsgs[mBootChoiceItem[Selected]], BGR_BLUE);
      }
      TimeoutMs = 0;   /* any key restarts the countdown */
    }
    MicroSecondDelay (100000);
  }

  /* Remember Linux / Android where the firmware keeps variables (harmless
     where it does not). Fastboot is never the default. */
  if (Selected != ESP_BOOT_FASTBOOT &&
      (Selected == ESP_BOOT_LINUX) != GetBootModeLinux ()) {
    SetBootModeLinux (Selected == ESP_BOOT_LINUX);
  }

  /* Leave the boot logo on screen, as the stock ABL does. */
  if (Selected != ESP_BOOT_FASTBOOT) {
    RestoreBootLogoBitBuffer ();
  }
  DEBUG ((EFI_D_INFO, "Boot menu: %a\n",
          Selected == ESP_BOOT_LINUX ? "Linux" :
          Selected == ESP_BOOT_ANDROID ? "Android" : "Fastboot"));
  return Selected;
}
