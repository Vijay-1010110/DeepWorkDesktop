[Setup]
AppName=DeepWork Desktop
AppVersion=1.0.0
DefaultDirName={autopf}\DeepWork Desktop
DefaultGroupName=DeepWork Desktop
UninstallDisplayIcon={app}\DesktopWidget.exe
Compression=lzma2
SolidCompression=yes
OutputDir=.\Installer
OutputBaseFilename=DeepWorkDesktop_Setup_v1.0
PrivilegesRequired=highest
SetupIconFile=DesktopWidget\DesktopWidget.ico

[Files]
Source: "x64\Release\DesktopWidget.exe"; DestDir: "{app}"; DestName: "DeepWorkDesktop.exe"; Flags: ignoreversion
Source: "DesktopWidget\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\DeepWork Desktop"; Filename: "{app}\DeepWorkDesktop.exe"
Name: "{autodesktop}\DeepWork Desktop"; Filename: "{app}\DeepWorkDesktop.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a Desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Run]
Filename: "{app}\DeepWorkDesktop.exe"; Description: "Launch DeepWork Desktop now"; Flags: nowait postinstall skipifsilent

[Registry]
Root: HKCU; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "DeepWork Desktop"; ValueData: """{app}\DeepWorkDesktop.exe"""; Flags: uninsdeletevalue
