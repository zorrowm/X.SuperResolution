Unicode true
ManifestDPIAware true
ManifestDPIAwareness PerMonitorV2
ManifestSupportedOS all
SetCompressor /SOLID lzma
SetCompressorDictSize 32
!cd "${__FILEDIR__}"

!ifndef PUBLISH_DIR
    !define PUBLISH_DIR "..\artifacts\publish\win-x64-full"
!endif
!ifndef CONFIGURATION
    !define CONFIGURATION "Release"
!endif

; Direct compilation also regenerates all metadata, file lists and the native skin.
!system 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "Prepare-Nsis.ps1" -PublishDir "${PUBLISH_DIR}" -Configuration "${CONFIGURATION}" -NsisDir "${NSISDIR}"' = 0
!include "generated\Generated.AppInfo.nsh"
!include "generated\Generated.Payload.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
!addplugindir /x86-unicode "generated"

Name "${APP_NAME}"
OutFile "${APP_INSTALLER_PATH}"
Icon "${APP_ICON}"
UninstallIcon "${APP_ICON}"
InstallDir "$LOCALAPPDATA\Programs\${APP_NAME}"
InstallDirRegKey HKCU "Software\${APP_NAME}" "InstallDir"
RequestExecutionLevel user
SilentInstall normal
SilentUnInstall normal
AutoCloseWindow true
AllowSkipFiles off
ShowInstDetails nevershow
ShowUninstDetails nevershow
LoadLanguageFile "${NSISDIR}\Contrib\Language files\SimpChinese.nlf"

VIProductVersion "${APP_VERSION4}"
VIAddVersionKey /LANG=2052 "ProductName" "${APP_PRODUCT}"
VIAddVersionKey /LANG=2052 "CompanyName" "${APP_COMPANY}"
VIAddVersionKey /LANG=2052 "LegalCopyright" "${APP_COPYRIGHT}"
VIAddVersionKey /LANG=2052 "FileDescription" "${APP_NAME} 安装程序"
VIAddVersionKey /LANG=2052 "FileVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=2052 "ProductVersion" "${APP_VERSION}"

!define APP_REGKEY "Software\${APP_NAME}"
!define UNINSTALL_REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"
Var Headless
Var DesktopShortcut
Var FailureText
Var LaunchApp

; NSIS owns extraction and registration. The plugin owns the entire visible window.
; Switching the engine to silent in .onInit prevents any stock wizard from flashing.
Page instfiles
UninstPage instfiles

!macro SkinProgress Percent Detail
    ${If} $Headless != 1
        InstallerSkin::Update /NOUNLOAD "${Percent}" "${Detail}"
    ${EndIf}
!macroend

Function .onInit
    StrCpy $Headless 0
    IfSilent 0 +2
        StrCpy $Headless 1
    SetSilent silent
    SetShellVarContext current
    SetRegView 64
    StrCpy $DesktopShortcut 1
    ${IfNot} ${AtLeastWin10}
    ${OrIfNot} ${RunningX64}
        ${If} $Headless != 1
            MessageBox MB_OK|MB_ICONSTOP "X.SuperResolution 需要 Windows 10 或更高版本的 64 位系统。"
        ${EndIf}
        SetErrorLevel 1633
        Quit
    ${EndIf}
    InitPluginsDir
    InstallerSkin::Configure /NOUNLOAD "${APP_REGKEY}" "X.Lucifer.SuperResolution"
    File "/oname=$PLUGINSDIR\logo.ico" "${APP_ICON}"
    File "/oname=$PLUGINSDIR\payload-manifest.txt" "generated\payload-manifest.txt"
    ${If} $Headless != 1
        InstallerSkin::Show /NOUNLOAD "${APP_VERSION}" "$INSTDIR" "$PLUGINSDIR\logo.ico" "install" "${APP_REQUIRED_MB}"
        Pop $0
        Pop $INSTDIR
        Pop $DesktopShortcut
        ${If} $0 != "install"
            InstallerSkin::Close
            SetErrorLevel 1602
            Quit
        ${EndIf}
    ${EndIf}
    InstallerSkin::Validate /NOUNLOAD "$INSTDIR" "install" "${APP_REQUIRED_MB}"
    Pop $FailureText
    ${If} $FailureText != ""
        Call ReportFailure
        Quit
    ${EndIf}
FunctionEnd

Function ReportFailure
    ${If} $Headless != 1
        InstallerSkin::Finish /NOUNLOAD "error" "$FailureText"
        Pop $0
    ${EndIf}
    InstallerSkin::Close
    SetErrorLevel 1603
FunctionEnd

Section "Install"
    !insertmacro SkinProgress 2 "正在准备安装目录…"
    ClearErrors
    CreateDirectory "$INSTDIR"
    ${If} ${Errors}
        StrCpy $FailureText "无法创建安装目录，请检查目录权限并重新安装。"
        Goto install_failed
    ${EndIf}
    InstallerSkin::Prune /NOUNLOAD "$INSTDIR" "$PLUGINSDIR\payload-manifest.txt"
    Pop $FailureText
    ${If} $FailureText != ""
        Goto install_failed
    ${EndIf}
    ClearErrors
    CopyFiles /SILENT "$PLUGINSDIR\payload-manifest.txt" "$INSTDIR\install-manifest.txt"
    IfErrors install_failed
    SetOverwrite on
    !insertmacro InstallPayload
    !insertmacro SkinProgress 95 "正在创建快捷方式…"
    SetOutPath "$INSTDIR"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    IfErrors install_failed
    Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
    ClearErrors
    CreateDirectory "$SMPROGRAMS\${APP_NAME}"
    CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
    CreateShortcut "$SMPROGRAMS\${APP_NAME}\卸载 ${APP_NAME}.lnk" "$INSTDIR\Uninstall.exe"
    ${If} $DesktopShortcut == 1
        CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
    ${Else}
        IfFileExists "$DESKTOP\${APP_NAME}.lnk" 0 +2
            Delete "$DESKTOP\${APP_NAME}.lnk"
    ${EndIf}
    IfErrors install_failed
    !insertmacro SkinProgress 98 "正在完成安装…"
    WriteRegStr HKCU "${APP_REGKEY}" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_REGKEY}" "DisplayName" "${APP_NAME}"
    WriteRegStr HKCU "${UNINSTALL_REGKEY}" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKCU "${UNINSTALL_REGKEY}" "Publisher" "${APP_COMPANY}"
    WriteRegStr HKCU "${UNINSTALL_REGKEY}" "URLInfoAbout" "${APP_REPOSITORY_URL}"
    WriteRegStr HKCU "${UNINSTALL_REGKEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_REGKEY}" "DisplayIcon" "$INSTDIR\${APP_EXE},0"
    WriteRegStr HKCU "${UNINSTALL_REGKEY}" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
    WriteRegStr HKCU "${UNINSTALL_REGKEY}" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
    WriteRegDWORD HKCU "${UNINSTALL_REGKEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTALL_REGKEY}" "NoRepair" 1
    WriteRegDWORD HKCU "${UNINSTALL_REGKEY}" "EstimatedSize" ${APP_SIZE_KB}
    IfErrors install_failed
    StrCpy $LaunchApp 0
    ${If} $Headless != 1
        InstallerSkin::Finish /NOUNLOAD "success" ""
        Pop $LaunchApp
    ${EndIf}
    ; Release the application's single-instance mutex before launching it.
    InstallerSkin::Close
    ${If} $LaunchApp == 1
        Exec '"$INSTDIR\${APP_EXE}"'
    ${EndIf}
    SetErrorLevel 0
    Goto install_done
install_failed:
    ${If} $FailureText == ""
        StrCpy $FailureText "无法写入应用文件。请确认磁盘空间充足、目录可写且 X.SuperResolution 已退出，然后重新安装。"
    ${EndIf}
    Call ReportFailure
    Quit
install_done:
SectionEnd

Function un.onInit
    StrCpy $Headless 0
    IfSilent 0 +2
        StrCpy $Headless 1
    SetSilent silent
    SetShellVarContext current
    SetRegView 64
    InitPluginsDir
    InstallerSkin::Configure /NOUNLOAD "${APP_REGKEY}" "X.Lucifer.SuperResolution"
    File "/oname=$PLUGINSDIR\logo.ico" "${APP_ICON}"
    ${If} $Headless != 1
        InstallerSkin::Show /NOUNLOAD "${APP_VERSION}" "$INSTDIR" "$PLUGINSDIR\logo.ico" "uninstall" "0"
        Pop $0
        Pop $INSTDIR
        Pop $DesktopShortcut
        ${If} $0 != "install"
            InstallerSkin::Close
            SetErrorLevel 1602
            Quit
        ${EndIf}
    ${EndIf}
    InstallerSkin::Validate /NOUNLOAD "$INSTDIR" "uninstall" "0"
    Pop $FailureText
    ${If} $FailureText != ""
        Call un.ReportFailure
        Quit
    ${EndIf}
FunctionEnd

Function un.ReportFailure
    ${If} $Headless != 1
        InstallerSkin::Finish /NOUNLOAD "error" "$FailureText"
        Pop $0
    ${EndIf}
    InstallerSkin::Close
    SetErrorLevel 1603
FunctionEnd

Section "Uninstall"
    !insertmacro SkinProgress 10 "正在移除应用文件…"
    ClearErrors
    !insertmacro UninstallPayload
    ${If} ${Errors}
        StrCpy $FailureText "部分应用文件无法移除。请退出 X.SuperResolution 并检查目录权限后重试。"
        Call un.ReportFailure
        Quit
    ${EndIf}
    !insertmacro RemoveEmptyPayloadDirectories
    !insertmacro SkinProgress 85 "正在清理快捷方式…"
    Delete "$DESKTOP\${APP_NAME}.lnk"
    Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
    Delete "$SMPROGRAMS\${APP_NAME}\卸载 ${APP_NAME}.lnk"
    RMDir "$SMPROGRAMS\${APP_NAME}"
    DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "${APP_NAME}"
    DeleteRegKey HKCU "${UNINSTALL_REGKEY}"
    DeleteRegKey HKCU "${APP_REGKEY}"
    Delete "$INSTDIR\install-manifest.txt"
    Delete "$INSTDIR\Uninstall.exe"
    SetOutPath "$TEMP"
    ; Never recursively delete INSTDIR or APPDATA: downloaded cores and personal files survive.
    RMDir "$INSTDIR"
    ${If} $Headless != 1
        InstallerSkin::Finish /NOUNLOAD "success" ""
        Pop $0
    ${EndIf}
    InstallerSkin::Close
    SetErrorLevel 0
SectionEnd
