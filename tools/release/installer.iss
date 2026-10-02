; Academy Launcher installer (Inno Setup 6). Built by tools\release\package.ps1:
;   ISCC /DAppVersion=0.1.0 /DStageDir=...\work\release\AcademyLauncher /DOutputDir=...\work\release installer.iss
; Installs for the current user only (no administrator prompt). The launcher updates itself by running a newer setup
; with /SILENT /UPDATE=1, which closes the launcher, replaces its files and starts it again.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\..\work\release\AcademyLauncher"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\work\release"
#endif

[Setup]
AppId={{E17F57BB-A3C1-4C77-BEC5-1D801B5F15F6}
AppName=Academy Launcher
AppVersion={#AppVersion}
AppVerName=Academy Launcher {#AppVersion}
AppPublisher=Academy Launcher contributors
DefaultDirName={localappdata}\Programs\Academy Launcher
DefaultGroupName=Academy Launcher
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
OutputDir={#OutputDir}
OutputBaseFilename=AcademyLauncherSetup-{#AppVersion}
SetupIconFile=..\..\launcher\academy.ico
UninstallDisplayIcon={app}\AcademyLauncher.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=force
RestartApplications=no
MinVersion=10.0

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[InstallDelete]
; Mods are replaced as a whole, so files a newer version dropped do not linger.
Type: filesandordirs; Name: "{app}\mods"
Type: filesandordirs; Name: "{app}\runtime"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Academy Launcher"; Filename: "{app}\AcademyLauncher.exe"
Name: "{group}\Uninstall Academy Launcher"; Filename: "{uninstallexe}"
Name: "{userdesktop}\Academy Launcher"; Filename: "{app}\AcademyLauncher.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\AcademyLauncher.exe"; Description: "Start Academy Launcher"; Flags: nowait postinstall skipifsilent
Filename: "{app}\AcademyLauncher.exe"; Flags: nowait; Check: IsUpdate

[Code]
function IsUpdate: Boolean;
begin
  Result := ExpandConstant('{param:UPDATE|0}') = '1';
end;

// The launcher keeps the extracted game, built mods, saves and settings in %LOCALAPPDATA%\AcademyLauncher.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Data: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    Data := ExpandConstant('{localappdata}\AcademyLauncher');
    if DirExists(Data) and not UninstallSilent then
      if MsgBox('Also delete the game files the launcher extracted, your saves and your settings?' + #13#10 + #13#10 + Data,
        mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(Data, True, True, True);
  end;
end;
