; MarvinCapture Windows installer (NSIS 3.x, Modern UI 2).
; Built by scripts\package-windows.ps1, which passes these defines:
;   VERSION       e.g. 0.1.0
;   VERSION_NUM   four-part, e.g. 0.1.0.0
;   DIST          build\windows-x86_64\dist (GUI, CLI, dlls, firmware\)
;   ROOT          repository root
;   WDI_SIMPLE    path of the libwdi wdi-simple.exe (x64) to install the WinUSB driver
;   WDI_DOCS      directory with the libwdi licence / source notes (installed next to it)
;   LICENSE_FILE  LICENSE with CRLF line endings
;   OUTFILE       output setup .exe
;
; NSIS makes 32-bit installers, so this runs under WOW64: the file-system redirection is
; switched off in .onInit and the registry view is set to 64-bit, so PowerShell, pnputil
; and the registry are the native 64-bit ones.

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma
SetCompressorDictSize 32

!ifndef VERSION
  !error "VERSION is not defined (run scripts\package-windows.ps1)"
!endif

!define APP_NAME      "MarvinCapture"
!define APP_PUBLISHER "Jonas Cz."
!define APP_URL       "https://github.com/JonasCz/MarvinCapture"
!define APP_EXE       "MarvinCaptureGUI.exe"
!define UNINST_KEY    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"
!define POWERSHELL    '"$WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File'

Name "${APP_NAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APP_NAME}"
RequestExecutionLevel admin
BrandingText "${APP_NAME} ${VERSION}"
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${VERSION_NUM}"
VIAddVersionKey "ProductName"     "${APP_NAME}"
VIAddVersionKey "ProductVersion"  "${VERSION}"
VIAddVersionKey "FileVersion"     "${VERSION}"
VIAddVersionKey "CompanyName"     "${APP_PUBLISHER}"
VIAddVersionKey "LegalCopyright"  "AGPL-3.0"
VIAddVersionKey "FileDescription" "${APP_NAME} setup"

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "WinVer.nsh"

!insertmacro GetSize
!insertmacro un.GetParameters
!insertmacro un.GetOptions

Var DriverFailures
Var DriverLog
Var Upgrading        ; uninstaller: 1 = /UPGRADE (called by a newer installer)
Var KeepDriver       ; uninstaller: 1 = /KEEPDRIVER

; --- Modern UI ----------------------------------------------------------------------
!define MUI_ICON   "${ROOT}\gui\windows\MarvinCaptureGUI\Assets\AppIcon.ico"
!define MUI_UNICON "${ROOT}\gui\windows\MarvinCaptureGUI\Assets\AppIcon.ico"
!define MUI_ABORTWARNING
!define MUI_UNABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE DirLeave
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP_NAME}"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchGUI
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_COMPONENTS
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; --- helpers ------------------------------------------------------------------------

; Start the GUI as the normal user, not elevated like this installer.
Function LaunchGUI
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\${APP_EXE}"'
FunctionEnd

; The uninstaller deletes $INSTDIR recursively, so the directory is always a
; "...\MarvinCapture" folder: append it if the user picked something else.
Function NormalizeInstDir
  StrCpy $0 $INSTDIR 13 -13
  ${If} $0 != "${APP_NAME}"
    StrCpy $INSTDIR "$INSTDIR\${APP_NAME}"
  ${EndIf}
FunctionEnd

Function DirLeave
  Call NormalizeInstDir
FunctionEnd

; Remove an earlier installation (files, shortcuts, uninstall entry) but keep its driver
; and PATH entry: this installer re-applies the options chosen now.
Function UninstallPrevious
  ReadRegStr $R0 HKLM "${UNINST_KEY}" "InstallLocation"
  ${If} $R0 == ""
    Return
  ${EndIf}
  ${IfNot} ${FileExists} "$R0\uninstall.exe"
    Return
  ${EndIf}
  DetailPrint "Removing the previous version in $R0 ..."
  CopyFiles /SILENT "$R0\uninstall.exe" "$TEMP\marvincapture-uninst.exe"
  ExecWait '"$TEMP\marvincapture-uninst.exe" /S /UPGRADE _?=$R0' $R1
  Delete "$TEMP\marvincapture-uninst.exe"
  Delete "$R0\uninstall.exe"
  RMDir "$R0"
FunctionEnd

; !insertmacro InstallDriver <pid> <name>
!macro InstallDriver PID NAME
  DetailPrint "Installing the WinUSB driver for ${NAME} (USB 2304:${PID}) ..."
  nsExec::ExecToLog '${POWERSHELL} "$INSTDIR\installer\driver-install.ps1" -WdiSimple "$INSTDIR\driver\wdi-simple.exe" -ProductId ${PID} -Name "${NAME}" -Log "$DriverLog"'
  Pop $0
  ${If} $0 S!= "0"
    IntOp $DriverFailures $DriverFailures + 1
    DetailPrint "  failed (exit code $0), see $DriverLog"
  ${Else}
    DetailPrint "  done"
  ${EndIf}
!macroend

; --- init ---------------------------------------------------------------------------

Function .onInit
  ${IfNot} ${IsNativeAMD64}
    MessageBox MB_OK|MB_ICONSTOP "${APP_NAME} needs 64-bit Windows on an Intel/AMD processor."
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_OK|MB_ICONSTOP "${APP_NAME} needs Windows 10 or later."
    Abort
  ${EndIf}
  ${DisableX64FSRedirection}
  SetRegView 64
  SetShellVarContext all
  StrCpy $DriverLog "$TEMP\MarvinCapture-driver-install.log"

  ; upgrade: default to the folder of the existing installation
  ${If} $INSTDIR == "$PROGRAMFILES64\${APP_NAME}"
    ReadRegStr $0 HKLM "${UNINST_KEY}" "InstallLocation"
    ${If} $0 != ""
      StrCpy $INSTDIR $0
    ${EndIf}
  ${EndIf}
FunctionEnd

; --- sections -----------------------------------------------------------------------

Section "${APP_NAME} (required)" SecMain
  SectionIn RO
  Call NormalizeInstDir
  Call UninstallPrevious

  SetOutPath "$INSTDIR"
  File /r /x "*.pdb" "${DIST}\*.*"

  SetOutPath "$INSTDIR\installer"
  File "${__FILEDIR__}\driver-install.ps1"
  File "${__FILEDIR__}\driver-uninstall.ps1"
  File "${__FILEDIR__}\path-edit.ps1"

  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Add/Remove Programs
  WriteRegStr   HKLM "${UNINST_KEY}" "DisplayName"          "${APP_NAME}"
  WriteRegStr   HKLM "${UNINST_KEY}" "DisplayVersion"       "${VERSION}"
  WriteRegStr   HKLM "${UNINST_KEY}" "Publisher"            "${APP_PUBLISHER}"
  WriteRegStr   HKLM "${UNINST_KEY}" "DisplayIcon"          "$INSTDIR\${APP_EXE}"
  WriteRegStr   HKLM "${UNINST_KEY}" "InstallLocation"      "$INSTDIR"
  WriteRegStr   HKLM "${UNINST_KEY}" "UninstallString"      '"$INSTDIR\uninstall.exe"'
  WriteRegStr   HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegStr   HKLM "${UNINST_KEY}" "URLInfoAbout"         "${APP_URL}"
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "USB driver (WinUSB) for all supported devices" SecDriver
  SetOutPath "$INSTDIR\driver"
  File "${WDI_SIMPLE}"
  File /nonfatal /r "${WDI_DOCS}\*.*"

  StrCpy $DriverFailures 0
  DetailPrint "Driver log: $DriverLog"
  !insertmacro InstallDriver "0213" "Pinnacle Studio 500-USB"
  !insertmacro InstallDriver "0223" "Pinnacle Studio 510-USB"
  !insertmacro InstallDriver "0212" "Pinnacle Studio 700-USB"
  !insertmacro InstallDriver "0224" "Pinnacle MovieBox Plus 710-USB"
  !insertmacro InstallDriver "0206" "Pinnacle MovieBox Deluxe"

  ${If} $DriverFailures > 0
    DetailPrint "WARNING: the driver could not be installed for $DriverFailures of 5 devices."
    ${IfNot} ${Silent}
      MessageBox MB_OK|MB_ICONEXCLAMATION "The WinUSB driver could not be installed for $DriverFailures of the 5 supported devices.$\r$\n$\r$\n${APP_NAME} itself is installed. If a device does not work, install the driver for it by hand with Zadig (WinUSB, see docs\windows-driver.md in the project), or run this setup again. If Pinnacle's own driver (MarvinAVS64) is installed it may still be bound to the device.$\r$\n$\r$\nLog file: $DriverLog"
    ${EndIf}
  ${Else}
    DetailPrint "WinUSB driver installed for all 5 devices (for devices not plugged in now, Windows uses it when you plug them in)."
  ${EndIf}
SectionEnd

Section "Start menu shortcuts" SecStartMenu
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" "$INSTDIR\uninstall.exe"
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
SectionEnd

Section /o "Add the command-line tool to the system PATH" SecPath
  DetailPrint "Adding $INSTDIR to the system PATH ..."
  nsExec::ExecToLog '${POWERSHELL} "$INSTDIR\installer\path-edit.ps1" -Action Add -Dir "$INSTDIR"'
  Pop $0
  ${If} $0 S!= "0"
    DetailPrint "WARNING: could not update PATH (exit code $0)"
    ${IfNot} ${Silent}
      MessageBox MB_OK|MB_ICONEXCLAMATION "The system PATH could not be updated. You can still run MarvinCaptureCLI.exe from $INSTDIR."
    ${EndIf}
  ${EndIf}
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain}      "The ${APP_NAME} program (GUI and command-line tool), its libraries and the FPGA firmware files."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDriver}    "Installs a WinUSB driver (generated with libwdi, self-signed) for the Studio 500/510/700-USB and MovieBox Plus 710 / Deluxe. It replaces the original Pinnacle driver for these devices (that driver stays installed and can be switched back). Plug the device in first if you can; otherwise Windows uses the driver when you plug it in."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "Start menu entries for ${APP_NAME} and its uninstaller."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop}   "A shortcut to ${APP_NAME} on the desktop."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPath}      "Adds the install folder to the system PATH so MarvinCaptureCLI can be run from any terminal."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; --- uninstaller --------------------------------------------------------------------

Function un.onInit
  ${DisableX64FSRedirection}
  SetRegView 64
  SetShellVarContext all
  StrCpy $Upgrading 0
  StrCpy $KeepDriver 0
  ${un.GetParameters} $R0
  ClearErrors
  ${un.GetOptions} $R0 "/UPGRADE" $R1
  ${IfNot} ${Errors}
    StrCpy $Upgrading 1
  ${EndIf}
  ClearErrors
  ${un.GetOptions} $R0 "/KEEPDRIVER" $R1
  ${IfNot} ${Errors}
    StrCpy $KeepDriver 1
  ${EndIf}
  ClearErrors
FunctionEnd

; Must come before the file removal: the script lives in $INSTDIR\installer.
Section "un.Remove the USB driver and its certificates" UnSecDriver
  ${If} $Upgrading == 1
  ${OrIf} $KeepDriver == 1
    DetailPrint "Keeping the USB driver."
    Return
  ${EndIf}
  ${IfNot} ${FileExists} "$INSTDIR\installer\driver-uninstall.ps1"
    DetailPrint "driver-uninstall.ps1 not found, skipping the driver removal."
    Return
  ${EndIf}
  DetailPrint "Removing the WinUSB driver packages and certificates created by the installer ..."
  nsExec::ExecToLog '${POWERSHELL} "$INSTDIR\installer\driver-uninstall.ps1" -Log "$TEMP\MarvinCapture-driver-uninstall.log"'
  Pop $0
  ${If} $0 S!= "0"
    DetailPrint "WARNING: the driver removal reported problems (exit code $0), see $TEMP\MarvinCapture-driver-uninstall.log"
    ${IfNot} ${Silent}
      MessageBox MB_OK|MB_ICONEXCLAMATION "The driver could not be removed completely.$\r$\n$\r$\nLog file: $TEMP\MarvinCapture-driver-uninstall.log"
    ${EndIf}
  ${Else}
    DetailPrint "Driver removed. Windows will use the original Pinnacle driver for the devices again, if it is installed."
  ${EndIf}
SectionEnd

Section "un.${APP_NAME} program files and shortcuts" UnSecMain
  SectionIn RO

  ${If} $Upgrading != 1
    DetailPrint "Removing $INSTDIR from the system PATH ..."
    ${If} ${FileExists} "$INSTDIR\installer\path-edit.ps1"
      nsExec::ExecToLog '${POWERSHELL} "$INSTDIR\installer\path-edit.ps1" -Action Remove -Dir "$INSTDIR"'
      Pop $0
    ${EndIf}
  ${EndIf}

  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
  RMDir  "$SMPROGRAMS\${APP_NAME}"
  Delete "$DESKTOP\${APP_NAME}.lnk"

  DeleteRegKey HKLM "${UNINST_KEY}"

  ; only ever delete recursively inside a folder named MarvinCapture
  StrCpy $0 $INSTDIR 13 -13
  ${If} $0 == "${APP_NAME}"
    SetOutPath "$TEMP"
    RMDir /r "$INSTDIR"
  ${Else}
    DetailPrint "$INSTDIR is not a ${APP_NAME} folder, leaving its contents alone."
    Delete "$INSTDIR\uninstall.exe"
  ${EndIf}
SectionEnd

!insertmacro MUI_UNFUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${UnSecDriver} "Removes the WinUSB driver packages and the self-signed certificates that the installer created for the Marvin devices. Pinnacle's own driver is not touched; Windows binds it again if it is installed. Untick to keep the WinUSB driver."
  !insertmacro MUI_DESCRIPTION_TEXT ${UnSecMain}   "Removes the program files, shortcuts, the PATH entry and the Add/Remove Programs entry."
!insertmacro MUI_UNFUNCTION_DESCRIPTION_END
