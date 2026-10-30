; rng-lens Windows installer. Build (Linux or Windows):
;   makensis -DVERSION=0.2.0 -DSRC=<folder with rng-lens.exe, dlls, x86\> [-DADB_DIR=<platform-tools>] installer.nsi
Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef SRC
  !define SRC "stage"
!endif
!ifndef OUTFILE
  !define OUTFILE "rng-lens-setup-${VERSION}.exe"
!endif

!define APP "rng-lens" ; folder, uninstall key, file names (unchanged so updates replace the old install)
!define NAME "Lens"     ; what people see
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}"

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

Name "${NAME}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APP}"
RequestExecutionLevel admin
BrandingText "${NAME} ${VERSION} - free and open source"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${NAME}"
VIAddVersionKey "FileDescription" "${NAME} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "rengi.dev"
VIAddVersionKey "LegalCopyright" "MIT"

!define MUI_ICON "${SRC}\rng-lens.ico"
!define MUI_UNICON "${SRC}\rng-lens.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "Turns your phone into a webcam (and microphone) for any app on this PC: background blur, auto-frame, looks and night mode included.$\r$\n$\r$\nThis adds the camera $\"Lens$\" that Zoom, Discord, OBS, Teams and browsers can pick.$\r$\n$\r$\nOn the phone: install the Lens app (rng-lens.apk) from the same release page."
!define MUI_FINISHPAGE_RUN "$INSTDIR\rng-lens.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start Lens"
!define MUI_FINISHPAGE_LINK "Get the phone app and updates"
!define MUI_FINISHPAGE_LINK_LOCATION "https://github.com/MRRengi/rng-lens/releases"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRC}\LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

!macro StopApp
  nsExec::Exec 'taskkill /f /im rng-lens.exe'
  Pop $0
  Sleep 800 ; let it exit and release its files
!macroend

; A camera DLL stays loaded in every app that has looked at the camera (Teams, browsers, OBS, the Windows
; camera service) until that app closes, and Windows won't overwrite or delete a loaded DLL. It does allow
; renaming one, so: rename the old file out of the way (deleted at the next restart) and write the new one.
!macro MoveAsideIfLocked PATH
  ${If} ${FileExists} "${PATH}"
    ClearErrors
    Delete "${PATH}"
    ${If} ${FileExists} "${PATH}"
      GetTempFileName $R0 "$INSTDIR"
      Delete $R0
      Rename "${PATH}" "$R0"
      Delete /REBOOTOK "$R0"
    ${EndIf}
  ${EndIf}
!macroend

!macro MoveAsideCameras
  !insertmacro MoveAsideIfLocked "$INSTDIR\rnglens_vcam.dll"
  !insertmacro MoveAsideIfLocked "$INSTDIR\rnglens_vcam2.dll"
  !insertmacro MoveAsideIfLocked "$INSTDIR\x86\rnglens_vcam.dll"
  !insertmacro MoveAsideIfLocked "$INSTDIR\x86\rnglens_vcam2.dll"
  !insertmacro MoveAsideIfLocked "$INSTDIR\rng-lens.exe"
!macroend

!macro UnregisterOne DLL
  ${If} ${FileExists} "$INSTDIR\${DLL}"
    ${DisableX64FSRedirection}
    ExecWait '"$WINDIR\System32\regsvr32.exe" /u /s "$INSTDIR\${DLL}"'
    ${EnableX64FSRedirection}
  ${EndIf}
  ${If} ${FileExists} "$INSTDIR\x86\${DLL}"
    ExecWait '"$WINDIR\SysWOW64\regsvr32.exe" /u /s "$INSTDIR\x86\${DLL}"'
  ${EndIf}
!macroend

!macro Unregister
  !insertmacro UnregisterOne "rnglens_vcam.dll"
  !insertmacro UnregisterOne "rnglens_vcam2.dll"
!macroend

!macro RegisterOne DLL
  ${DisableX64FSRedirection}
  ExecWait '"$WINDIR\System32\regsvr32.exe" /s "$INSTDIR\${DLL}"' $0
  ${EnableX64FSRedirection}
  ExecWait '"$WINDIR\SysWOW64\regsvr32.exe" /s "$INSTDIR\x86\${DLL}"' $1
  ${If} $0 != 0
    DetailPrint "Warning: ${DLL} (64-bit) did not register (code $0)"
  ${EndIf}
!macroend

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "rng-lens needs 64-bit Windows 10 or newer."
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

Section "Lens" SecMain
  SectionIn RO
  SetShellVarContext all
  !insertmacro StopApp
  SetOutPath "$INSTDIR"
  Delete "$INSTDIR\*.tmp" ; old files set aside by an earlier update, if nothing holds them any more
  !insertmacro Unregister ; an older version's camera, so the DLL can be replaced
  CreateDirectory "$INSTDIR\x86"
  !insertmacro MoveAsideCameras

  File "${SRC}\rng-lens.exe"
  File "${SRC}\rnglens_vcam.dll"
  File "${SRC}\rnglens_vcam2.dll"
  File "${SRC}\onnxruntime.dll"
  File "${SRC}\avcodec-*.dll"
  File "${SRC}\avutil-*.dll"
  File "${SRC}\swscale-*.dll"
  File "${SRC}\swresample-*.dll"
  File "${SRC}\rng-lens.ico"
  File "${SRC}\README.txt"
  File "${SRC}\LICENSE*.txt"
  SetOutPath "$INSTDIR\x86"
  File "${SRC}\x86\rnglens_vcam.dll"
  File "${SRC}\x86\rnglens_vcam2.dll"
  SetOutPath "$INSTDIR\models"
  File "${SRC}\models\*.onnx"
  File "${SRC}\models\LICENSES.txt"
!ifdef ADB_DIR
  SetOutPath "$INSTDIR\adb"
  File "${ADB_DIR}\adb.exe"
  File "${ADB_DIR}\AdbWinApi.dll"
  File "${ADB_DIR}\AdbWinUsbApi.dll"
  File /nonfatal "${ADB_DIR}\NOTICE.txt"
!endif
  SetOutPath "$INSTDIR"

  ; The cameras ("Lens" and, for the optional second phone, "Lens 2"), for 64-bit and 32-bit apps.
  !insertmacro RegisterOne "rnglens_vcam.dll"
  !insertmacro RegisterOne "rnglens_vcam2.dll"

  ; Let phones find this PC on Wi-Fi without a firewall prompt (only for rng-lens.exe).
  nsExec::Exec 'netsh advfirewall firewall delete rule name="rng-lens"'
  Pop $0
  nsExec::Exec 'netsh advfirewall firewall add rule name="rng-lens" dir=in action=allow program="$INSTDIR\rng-lens.exe" enable=yes profile=any'
  Pop $0

  ; Shortcuts from versions before the rename.
  RMDir /r "$SMPROGRAMS\rng-lens"
  Delete "$DESKTOP\rng-lens.lnk"
  CreateShortcut "$SMPROGRAMS\Lens.lnk" "$INSTDIR\rng-lens.exe" "" "$INSTDIR\rng-lens.ico"
  CreateShortcut "$DESKTOP\Lens.lnk" "$INSTDIR\rng-lens.exe" "" "$INSTDIR\rng-lens.ico"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${NAME}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "rengi.dev"
  WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "https://github.com/MRRengi/rng-lens"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\rng-lens.ico"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" 34000
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  !insertmacro StopApp
  !insertmacro Unregister
  !insertmacro MoveAsideCameras
  nsExec::Exec 'netsh advfirewall firewall delete rule name="rng-lens"'
  Pop $0
  Delete "$DESKTOP\Lens.lnk"
  Delete "$SMPROGRAMS\Lens.lnk"
  Delete "$DESKTOP\rng-lens.lnk"
  RMDir /r "$SMPROGRAMS\rng-lens"
  RMDir /r "$INSTDIR\adb"
  RMDir /r /REBOOTOK "$INSTDIR\x86"
  RMDir /r "$INSTDIR\models"
  Delete /REBOOTOK "$INSTDIR\*.dll"
  Delete /REBOOTOK "$INSTDIR\*.tmp"
  Delete "$INSTDIR\*.txt"
  Delete "$INSTDIR\rng-lens.exe"
  Delete "$INSTDIR\rng-lens.ico"
  Delete "$INSTDIR\uninstall.exe"
  RMDir /REBOOTOK "$INSTDIR"
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "rng-lens"
SectionEnd
