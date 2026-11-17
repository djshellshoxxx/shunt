; Shunt Windows installer (NSIS 3). Built by scripts/build-windows.sh and release.yml.
; makensis -DVERSION=x.y.z -DSRCDIR=<folder with Shunt.exe etc.> -DOUTFILE=<setup.exe> shunt.nsi
Unicode true
!include "MUI2.nsh"
!include "x64.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef SRCDIR
  !define SRCDIR "..\..\dist\Shunt"
!endif
!ifndef OUTFILE
  !define OUTFILE "Shunt-${VERSION}-windows-x64-setup.exe"
!endif

Name "Shunt ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\Shunt"
InstallDirRegKey HKLM "Software\Shunt" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "Shunt"
VIAddVersionKey "FileDescription" "Shunt installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Shunt"

!define MUI_ICON "${SRCDIR}\shunt.ico"
!define MUI_UNICON "${SRCDIR}\shunt.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\Shunt.exe"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRCDIR}\LEGAL.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Shunt (required)" SecMain
  SectionIn RO
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Shunt needs 64-bit Windows 10 or later."
    Abort
  ${EndIf}
  SetOutPath "$INSTDIR"
  File "${SRCDIR}\Shunt.exe"
  File "${SRCDIR}\shunt_headless.exe"
  File "${SRCDIR}\shunt_cli.exe"
  File "${SRCDIR}\shuntsim.exe"
  File "${SRCDIR}\shunt.ico"
  File "${SRCDIR}\LEGAL.txt"
  File "${SRCDIR}\README.txt"
  WriteRegStr HKLM "Software\Shunt" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shunt" "DisplayName" "Shunt"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shunt" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shunt" "Publisher" "Shunt"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shunt" "DisplayIcon" "$INSTDIR\shunt.ico"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shunt" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shunt" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shunt" "NoRepair" 1
SectionEnd

; RS-03: the installer adds the Windows Firewall rules (player network UDP 50000-50002,
; OSC input, Link discovery, local web server) so the first run needs no prompt.
Section "Windows Firewall rules" SecFirewall
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="Shunt"'
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="Shunt" dir=in action=allow program="$INSTDIR\Shunt.exe" enable=yes profile=any'
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="Shunt" dir=in action=allow program="$INSTDIR\shunt_headless.exe" enable=yes profile=any'
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="Shunt" dir=in action=allow protocol=UDP localport=50000-50002 enable=yes profile=any'
SectionEnd

Section "Start Menu shortcuts" SecShortcuts
  CreateDirectory "$SMPROGRAMS\Shunt"
  CreateShortcut "$SMPROGRAMS\Shunt\Shunt.lnk" "$INSTDIR\Shunt.exe" "" "$INSTDIR\shunt.ico"
  CreateShortcut "$SMPROGRAMS\Shunt\Uninstall Shunt.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section /o "Start Shunt when I sign in" SecAutostart
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "Shunt" '"$INSTDIR\Shunt.exe" --minimised'
SectionEnd

Section "Uninstall"
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="Shunt"'
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "Shunt"
  Delete "$INSTDIR\Shunt.exe"
  Delete "$INSTDIR\shunt_headless.exe"
  Delete "$INSTDIR\shunt_cli.exe"
  Delete "$INSTDIR\shuntsim.exe"
  Delete "$INSTDIR\shunt.ico"
  Delete "$INSTDIR\LEGAL.txt"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\Shunt\Shunt.lnk"
  Delete "$SMPROGRAMS\Shunt\Uninstall Shunt.lnk"
  RMDir "$SMPROGRAMS\Shunt"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Shunt"
  DeleteRegKey HKLM "Software\Shunt"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "Shunt desktop app, headless service, console and simulator."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecFirewall} "Allow the player network (UDP 50000-50002), Link, OSC and the overlay server through Windows Firewall."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecShortcuts} "Start Menu entries."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAutostart} "Start Shunt minimised to the tray when you sign in."
!insertmacro MUI_FUNCTION_DESCRIPTION_END
