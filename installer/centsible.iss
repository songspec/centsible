; Centsible installer (Inno Setup 6). Build: ISCC.exe installer\centsible.iss  ->  dist\<version>\Centsible-Setup-<version>.exe  (the files it packs are in dist\<version>\)
#define AppName "Centsible"
; the release workflow passes /DAppVersion=<version from CMakeLists.txt>; the
; fallback is only for building the installer by hand
#ifndef AppVersion
  #define AppVersion "1.3.0"
#endif

[Setup]
AppId={{6B0E2B1C-5C1B-4B52-9D3E-C3A7510C0A11}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=SongSpec
DefaultDirName={autopf}\Centsible
DefaultGroupName=Centsible
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist\{#AppVersion}
OutputBaseFilename=Centsible-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Centsible
CloseApplications=no

[Types]
Name: "full"; Description: "Plugin, standalone app and command line tool"
Name: "plugin"; Description: "VST3 plugin only"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugin (for your DAW)"; Types: full plugin custom; Flags: fixed
Name: "standalone"; Description: "Standalone app (drop audio files to analyse them)"; Types: full custom
Name: "cli"; Description: "Command line analyzer (batch CSV, match mode)"; Types: full custom

[Files]
Source: "..\dist\{#AppVersion}\Centsible.vst3\*"; DestDir: "{commoncf64}\VST3\Centsible.vst3"; Components: vst3; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "..\dist\{#AppVersion}\Centsible-Standalone.exe"; DestDir: "{app}"; DestName: "Centsible.exe"; Components: standalone; Flags: ignoreversion
Source: "..\dist\{#AppVersion}\centsible_cli.exe"; DestDir: "{app}"; Components: cli; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\BEATSWITCH.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\Centsible"; Filename: "{app}\Centsible.exe"; Components: standalone
Name: "{group}\Uninstall Centsible"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\Centsible.exe"; Description: "Start Centsible"; Components: standalone; Flags: postinstall nowait skipifsilent unchecked

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
  if MsgBox('Please close your DAW (FL Studio, Ableton, Reaper ...) before installing: Windows cannot replace a plugin that is loaded.' + #13#10 + #13#10 + 'Continue?', mbConfirmation, MB_YESNO) = IDNO then
    Result := False;
end;
