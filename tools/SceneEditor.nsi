; Windows installer for Scene Editor (NSIS). Build it with meson, which passes the paths:
;
;   meson setup build-release --buildtype=release -Db_vscrt=mt
;   meson compile -C build-release installer
;
; Installs the editor, Box2D and raylib's sources (see src/AppPaths.h) for the current user, so
; without admin rights, into %LOCALAPPDATA%\Programs\SceneEditor: no space in the name, since
; make (which Play runs) can't handle them. The uninstaller leaves editor settings in %APPDATA%.
;
; Defines: VERSION, EXE, ICON, LICENSE, BOX2D_DIR, RAYLIB_SRC, RAYLIB_LICENSE, OUTFILE

Unicode true
!include "MUI2.nsh"

; meson passes paths with forward slashes; File needs backslashes.
!searchreplace EXE_FILE "${EXE}" "/" "\"
!searchreplace ICON_FILE "${ICON}" "/" "\"
!searchreplace LICENSE_FILE "${LICENSE}" "/" "\"
!searchreplace BOX2D_FILES "${BOX2D_DIR}" "/" "\"
!searchreplace RAYLIB_FILES "${RAYLIB_SRC}" "/" "\"
!searchreplace RAYLIB_LICENSE_FILE "${RAYLIB_LICENSE}" "/" "\"
!searchreplace OUT_FILE "${OUTFILE}" "/" "\"

!define APP "Scene Editor"
!define PUBLISHER "Rishi Kaley"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\SceneEditor"
!define SETTINGS_KEY "Software\SceneEditor"

Name "${APP}"
OutFile "${OUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\SceneEditor"
InstallDirRegKey HKCU "${SETTINGS_KEY}" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma

VIProductVersion "${VERSION}.0.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APP} installer"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "LegalCopyright" "MIT License"

!define MUI_ICON "${ICON_FILE}"
!define MUI_UNICON "${ICON_FILE}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\SceneEditor.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "${APP}" SecEditor
    SectionIn RO

    SetOutPath "$INSTDIR"
    File "${EXE_FILE}"
    File /oname=LICENSE.txt "${LICENSE_FILE}"

    ; Copied into exports that use physics.
    SetOutPath "$INSTDIR\box2d"
    File /r "${BOX2D_FILES}\*.*"

    ; Built by Play along with the game, with the game's compiler.
    SetOutPath "$INSTDIR\raylib\src"
    File /r "${RAYLIB_FILES}\*.*"
    SetOutPath "$INSTDIR\raylib"
    File /oname=LICENSE.txt "${RAYLIB_LICENSE_FILE}"

    SetOutPath "$INSTDIR"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\SceneEditor.exe"

    WriteRegStr HKCU "${SETTINGS_KEY}" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "${PUBLISHER}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\SceneEditor.exe"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Section "Desktop shortcut" SecDesktop
    CreateShortcut "$DESKTOP\${APP}.lnk" "$INSTDIR\SceneEditor.exe"
SectionEnd

Section "Open .sceneproj files with ${APP}" SecAssociate
    WriteRegStr HKCU "Software\Classes\.sceneproj" "" "SceneEditor.Project"
    WriteRegStr HKCU "Software\Classes\SceneEditor.Project" "" "${APP} project"
    WriteRegStr HKCU "Software\Classes\SceneEditor.Project\DefaultIcon" "" "$INSTDIR\SceneEditor.exe,0"
    WriteRegStr HKCU "Software\Classes\SceneEditor.Project\shell\open\command" "" '"$INSTDIR\SceneEditor.exe" --open "%1"'
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)' ; file associations changed
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecEditor} "The editor, with Box2D and raylib's sources for exports and Play."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A shortcut on the desktop."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecAssociate} "Double-click a .sceneproj file to open the project."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
    ; Only what the installer put there, in case the folder holds anything else.
    RMDir /r "$INSTDIR\box2d"
    RMDir /r "$INSTDIR\raylib"
    Delete "$INSTDIR\SceneEditor.exe"
    Delete "$INSTDIR\LICENSE.txt"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"

    Delete "$SMPROGRAMS\${APP}.lnk"
    Delete "$DESKTOP\${APP}.lnk"

    ; The file association, if it's still ours.
    ReadRegStr $0 HKCU "Software\Classes\.sceneproj" ""
    StrCmp $0 "SceneEditor.Project" 0 +2
        DeleteRegKey HKCU "Software\Classes\.sceneproj"
    DeleteRegKey HKCU "Software\Classes\SceneEditor.Project"
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

    DeleteRegKey HKCU "${UNINSTALL_KEY}"
    DeleteRegKey HKCU "${SETTINGS_KEY}"
SectionEnd
