; MyCam installer (Inno Setup 6). Build with installer\build.ps1, which compiles the PC side, fetches
; the UsbDk driver package and runs ISCC. Replaces pc\install.ps1 for end users.
;
; Code signing (optional): pass a sign tool to ISCC, e.g.
;   ISCC /DSIGN "/Smycam=signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /a $f" mycam.iss

#ifndef AppVersion
  #define AppVersion "1.3.3"
#endif
#ifndef BuildDir
  #define BuildDir "..\pc\build-release\Release"
#endif
#define UsbDkMsi "UsbDk_1.0.22_x64.msi"

[Setup]
AppId={{5E0C8B4A-7F21-4D6B-9C3E-1A2B3C4D5E6F}
AppName=MyCam
AppVersion={#AppVersion}
AppVerName=MyCam {#AppVersion}
AppPublisher=MyCam
DefaultDirName={autopf}\MyCam
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 11 is required for the virtual camera API.
MinVersion=10.0.22000
OutputDir=output
OutputBaseFilename=MyCam-Setup-{#AppVersion}
SetupIconFile=..\pc\companion\res\app.ico
UninstallDisplayIcon={app}\MyCamCompanion.exe
UninstallDisplayName=MyCam
WizardStyle=modern
WizardImageFile=art\wizard-100.bmp,art\wizard-150.bmp,art\wizard-200.bmp
WizardSmallImageFile=art\small-100.bmp,art\small-150.bmp,art\small-200.bmp
Compression=lzma2
SolidCompression=yes
CloseApplications=no
; The Run entry for "Start with Windows" is per user (the companion's own toggle writes the same value).
UsedUserAreasWarning=no
#ifdef SIGN
SignTool=mycam
SignedUninstaller=yes
#endif

[Messages]
WelcomeLabel2=This will install MyCam {#AppVersion}, which turns your Android phone into a USB webcam.%n%nYour phone shows up as a camera called "MyCam" in Zoom, Teams, OBS, browsers and the Camera app.%n%nClose any app that is using MyCam before continuing.

[Tasks]
Name: "autostart"; Description: "Start MyCam when I sign in to Windows (recommended)"

[Files]
Source: "{#BuildDir}\MyCamCompanion.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\MyCamVCam.dll"; DestDir: "{app}"; Flags: ignoreversion
; Kept after install so the uninstaller can remove UsbDk if the user asks.
Source: "redist\{#UsbDkMsi}"; DestDir: "{app}\redist"; Flags: ignoreversion
Source: "redist\UsbDk-LICENSE.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "..\pc\third_party\libusb\COPYING"; DestDir: "{app}\licenses"; DestName: "libusb-COPYING.txt"; Flags: ignoreversion

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "MyCam"; \
    ValueData: """{app}\MyCamCompanion.exe"""; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{sys}\msiexec.exe"; Parameters: "/i ""{app}\redist\{#UsbDkMsi}"" /qn /norestart"; \
    StatusMsg: "Installing the UsbDk USB driver..."; Check: not UsbDkInstalled; Flags: runhidden waituntilterminated
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\MyCamVCam.dll"""; \
    StatusMsg: "Registering the MyCam camera..."; Flags: runhidden waituntilterminated
; Lets new phones get Windows' WinUSB driver later without a UAC prompt (a SYSTEM task users may only start).
Filename: "{app}\MyCamCompanion.exe"; Parameters: "--register-task"; \
    StatusMsg: "Setting up phone drivers..."; Flags: runhidden waituntilterminated
Filename: "{app}\MyCamCompanion.exe"; Parameters: "--settings"; Description: "Open MyCam now"; \
    Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\MyCamCompanion.exe"; Parameters: "--quit"; RunOnceId: "QuitMyCam"; Flags: runhidden waituntilterminated
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM MyCamCompanion.exe"; RunOnceId: "KillMyCam"; Flags: runhidden waituntilterminated
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u ""{app}\MyCamVCam.dll"""; RunOnceId: "UnregisterMyCam"; Flags: runhidden waituntilterminated
Filename: "{sys}\schtasks.exe"; Parameters: "/Delete /TN ""MyCam phone driver"" /F"; RunOnceId: "DeleteDriverTask"; Flags: runhidden waituntilterminated
; Windows' camera service may still have the DLL loaded; restart it so the file can be deleted.
Filename: "{sys}\net.exe"; Parameters: "stop FrameServer /y"; RunOnceId: "StopFrameServer"; Flags: runhidden waituntilterminated

[UninstallDelete]
Type: dirifempty; Name: "{app}"

[Code]
function UsbDkInstalled: Boolean;
begin
  Result := FileExists(ExpandConstant('{sys}\UsbDkHelper.dll'));
end;

procedure RunHidden(const FileName, Params: String);
var
  Code: Integer;
begin
  Exec(FileName, Params, '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

// Upgrades: close the running companion cleanly, then release the camera DLL from Windows' camera
// service so it can be replaced.
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  // Use this setup's own companion for --quit: an older installed version may not know the flag and
  // would just start up (and setup would wait on it forever).
  ExtractTemporaryFile('MyCamCompanion.exe');
  RunHidden(ExpandConstant('{tmp}\MyCamCompanion.exe'), '--quit');
  RunHidden(ExpandConstant('{sys}\taskkill.exe'), '/F /IM MyCamCompanion.exe');
  RunHidden(ExpandConstant('{sys}\net.exe'), 'stop FrameServer /y');
  Result := '';
end;

// UsbDk can be shared with other software (e.g. virtual machine USB redirection), so only remove it
// when the user says so.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if (CurUninstallStep = usUninstall) and UsbDkInstalled and not UninstallSilent then
    if MsgBox('Also remove the UsbDk USB driver?' + #13#10#13#10 +
              'Choose No if other software on this PC uses UsbDk.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      RunHidden(ExpandConstant('{sys}\msiexec.exe'), '/x "' + ExpandConstant('{app}\redist\{#UsbDkMsi}') + '" /qn /norestart');
end;
