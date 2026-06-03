; Nordstjernen Windows installer (NSIS Modern UI).
; Per-user install — no elevation required — to %LOCALAPPDATA%\Programs\Nordstjernen.
; Invoked by scripts/pack-windows-installer.sh, which passes VERSION/SRCDIR/OUTFILE.

Unicode true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "FileFunc.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef SRCDIR
  !define SRCDIR "..\..\dist\nordstjernen-win64"
!endif
!ifndef OUTFILE
  !define OUTFILE "..\..\dist\nordstjernen-${VERSION}-win64-setup.exe"
!endif

!define APP_NAME "Nordstjernen"
!define APP_DISPLAY "Nordstjernen Web Navigator"
!define APP_PUBLISHER "Andreas Rosdal"
!define APP_URL "https://nordstjernen.org"
!define APP_REGKEY "Software\Nordstjernen"
!define APP_ARPKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Nordstjernen"
!define APP_SMIKEY "Software\Clients\StartMenuInternet\Nordstjernen"
!define APP_PROGID "NordstjernenHTML"
!define APP_CLASSKEY "Software\Classes\${APP_PROGID}"

Name "${APP_NAME} ${VERSION}"
OutFile "${OUTFILE}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\Nordstjernen"
InstallDirRegKey HKCU "${APP_REGKEY}" "InstallDir"
ShowInstDetails show
ShowUnInstDetails show
BrandingText "${APP_DISPLAY} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey "LegalCopyright" "(c) 2026 ${APP_PUBLISHER}"
VIAddVersionKey "FileDescription" "${APP_DISPLAY} installer"

!define MUI_ABORTWARNING
!define MUI_ICON   "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

!define MUI_FINISHPAGE_RUN "$INSTDIR\nordstjernen.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Run Nordstjernen now"
!define MUI_FINISHPAGE_SHOWREADME ""
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Set Nordstjernen as my default browser"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION OpenDefaultAppsSettings
!define MUI_FINISHPAGE_LINK "Visit nordstjernen.org"
!define MUI_FINISHPAGE_LINK_LOCATION "${APP_URL}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Nordstjernen (required)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${SRCDIR}\*"

  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" \
                 "$INSTDIR\nordstjernen.exe" "" \
                 "$INSTDIR\nordstjernen.exe" 0 \
                 SW_SHOWNORMAL "" "${APP_DISPLAY}"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" \
                 "$INSTDIR\uninstall.exe"

  WriteRegStr HKCU "${APP_REGKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${APP_REGKEY}" "Version"    "${VERSION}"

  WriteRegStr   HKCU "${APP_ARPKEY}" "DisplayName"          "${APP_DISPLAY}"
  WriteRegStr   HKCU "${APP_ARPKEY}" "DisplayVersion"       "${VERSION}"
  WriteRegStr   HKCU "${APP_ARPKEY}" "Publisher"            "${APP_PUBLISHER}"
  WriteRegStr   HKCU "${APP_ARPKEY}" "URLInfoAbout"         "${APP_URL}"
  WriteRegStr   HKCU "${APP_ARPKEY}" "DisplayIcon"          "$INSTDIR\nordstjernen.exe,0"
  WriteRegStr   HKCU "${APP_ARPKEY}" "InstallLocation"      "$INSTDIR"
  WriteRegStr   HKCU "${APP_ARPKEY}" "UninstallString"      '"$INSTDIR\uninstall.exe"'
  WriteRegStr   HKCU "${APP_ARPKEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${APP_ARPKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${APP_ARPKEY}" "NoRepair" 1

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKCU "${APP_ARPKEY}" "EstimatedSize" "$0"

  WriteRegStr HKCU "${APP_SMIKEY}"                       "" "${APP_DISPLAY}"
  WriteRegStr HKCU "${APP_SMIKEY}\DefaultIcon"           "" "$INSTDIR\nordstjernen.exe,0"
  WriteRegStr HKCU "${APP_SMIKEY}\shell\open\command"    "" '"$INSTDIR\nordstjernen.exe"'

  WriteRegStr HKCU "${APP_SMIKEY}\Capabilities" "ApplicationName"        "${APP_NAME}"
  WriteRegStr HKCU "${APP_SMIKEY}\Capabilities" "ApplicationDescription" "${APP_DISPLAY}"
  WriteRegStr HKCU "${APP_SMIKEY}\Capabilities" "ApplicationIcon"        "$INSTDIR\nordstjernen.exe,0"

  WriteRegStr HKCU "${APP_SMIKEY}\Capabilities\FileAssociations" ".htm"  "${APP_PROGID}"
  WriteRegStr HKCU "${APP_SMIKEY}\Capabilities\FileAssociations" ".html" "${APP_PROGID}"
  WriteRegStr HKCU "${APP_SMIKEY}\Capabilities\FileAssociations" ".xht"  "${APP_PROGID}"
  WriteRegStr HKCU "${APP_SMIKEY}\Capabilities\FileAssociations" ".xhtml" "${APP_PROGID}"
  WriteRegStr HKCU "${APP_SMIKEY}\Capabilities\URLAssociations"  "http"  "${APP_PROGID}"
  WriteRegStr HKCU "${APP_SMIKEY}\Capabilities\URLAssociations"  "https" "${APP_PROGID}"
  WriteRegStr HKCU "${APP_SMIKEY}\Capabilities\StartMenu" "StartMenuInternet" "${APP_NAME}"

  WriteRegStr HKCU "${APP_CLASSKEY}"                    "" "Nordstjernen HTML Document"
  WriteRegStr HKCU "${APP_CLASSKEY}\DefaultIcon"        "" "$INSTDIR\nordstjernen.exe,0"
  WriteRegStr HKCU "${APP_CLASSKEY}\shell\open\command" "" '"$INSTDIR\nordstjernen.exe" "%1"'

  WriteRegStr HKCU "Software\RegisteredApplications" "${APP_NAME}" "${APP_SMIKEY}\Capabilities"

  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Function OpenDefaultAppsSettings
  ExecShell "open" "ms-settings:defaultapps?registeredAppName=${APP_NAME}"
FunctionEnd

Section "Desktop shortcut" SecDesktop
  CreateShortCut "$DESKTOP\${APP_NAME}.lnk" \
                 "$INSTDIR\nordstjernen.exe" "" \
                 "$INSTDIR\nordstjernen.exe" 0 \
                 SW_SHOWNORMAL "" "${APP_DISPLAY}"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
  RMDir  "$SMPROGRAMS\${APP_NAME}"

  RMDir /r "$INSTDIR"

  DeleteRegKey   HKCU "${APP_REGKEY}"
  DeleteRegKey   HKCU "${APP_ARPKEY}"
  DeleteRegKey   HKCU "${APP_SMIKEY}"
  DeleteRegKey   HKCU "${APP_CLASSKEY}"
  DeleteRegValue HKCU "Software\RegisteredApplications" "${APP_NAME}"
SectionEnd
