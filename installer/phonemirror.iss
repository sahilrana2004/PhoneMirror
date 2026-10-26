; Inno Setup 6 script for PhoneMirror. Build the Release exe first, then:
;   ISCC.exe /DAppVersion=1.0.2 installer\phonemirror.iss
; Output: build\installer\PhoneMirror-Setup-<version>.exe

#ifndef AppVersion
  #define AppVersion "1.0.2"
#endif

[Setup]
AppId={{6F3C2B9E-4D1A-4E4B-9C7E-2A51F0D8B7C3}
AppName=PhoneMirror
AppVersion={#AppVersion}
AppPublisher=Sahil Rana
DefaultDirName={autopf}\PhoneMirror
DefaultGroupName=PhoneMirror
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=..\build\installer
OutputBaseFilename=PhoneMirror-Setup-{#AppVersion}
SetupIconFile=..\res\phonemirror.ico
UninstallDisplayIcon={app}\PhoneMirror.exe
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Admin is needed to add the inbound firewall rule iPhones connect through.
PrivilegesRequired=admin
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked
Name: "startup"; Description: "Start PhoneMirror when I sign in (runs in the tray)"; Flags: unchecked

[Files]
Source: "..\build\Release\PhoneMirror.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"

[Icons]
Name: "{group}\PhoneMirror"; Filename: "{app}\PhoneMirror.exe"
Name: "{autodesktop}\PhoneMirror"; Filename: "{app}\PhoneMirror.exe"; Tasks: desktopicon
Name: "{userstartup}\PhoneMirror"; Filename: "{app}\PhoneMirror.exe"; Tasks: startup

[Run]
; Inbound rule for the AirPlay ports and mDNS; private/domain networks only.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""PhoneMirror"" dir=in action=allow program=""{app}\PhoneMirror.exe"" enable=yes profile=private,domain"; Flags: runhidden; StatusMsg: "Adding firewall rule..."
Filename: "{app}\PhoneMirror.exe"; Description: "Launch PhoneMirror"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/IM PhoneMirror.exe /F"; Flags: runhidden; RunOnceId: "StopApp"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""PhoneMirror"" program=""{app}\PhoneMirror.exe"""; Flags: runhidden; RunOnceId: "RemoveFirewallRule"
