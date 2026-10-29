; The Windows installer, made by Inno Setup from the portable folder that windows-dist.sh builds:
;   ISCC /DAppVersion=0.1.2 /DDistDir=<portable folder> /DOutputDir=<folder for the setup> vybrant.iss
; It installs for the current user without admin rights (or for everyone, picked on its first page), adds Vybrant to the
; Start menu, makes .mtp projects open in it, and registers an uninstaller.
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
; The AppId names this app to Windows: it must never change, or a new version installs next to the old one
AppId={{BF330B2F-0D37-4B7B-9BC3-3C245417FEEE}
AppName=Vybrant
AppVersion={#AppVersion}
AppPublisher=Vybrant
AppPublisherURL=https://vybrant.studio
DefaultDirName={autopf}\Vybrant
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
ChangesAssociations=yes
SetupIconFile=..\resources\vybrant.ico
UninstallDisplayIcon={app}\Vybrant.exe
WizardStyle=modern
OutputDir={#OutputDir}
OutputBaseFilename=Vybrant-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#DistDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Vybrant"; Filename: "{app}\Vybrant.exe"
Name: "{autodesktop}\Vybrant"; Filename: "{app}\Vybrant.exe"; Tasks: desktopicon

[Registry]
; .mtp projects open in Vybrant (HKA: the user's or the machine's classes, matching how it was installed)
Root: HKA; Subkey: "Software\Classes\.mtp"; ValueType: string; ValueName: ""; ValueData: "Vybrant.Project"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\Vybrant.Project"; ValueType: string; ValueName: ""; ValueData: "Vybrant project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Vybrant.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\Vybrant.exe,0"
Root: HKA; Subkey: "Software\Classes\Vybrant.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\Vybrant.exe"" ""%1"""

[Run]
Filename: "{app}\Vybrant.exe"; Description: "{cm:LaunchProgram,Vybrant}"; Flags: nowait postinstall skipifsilent
