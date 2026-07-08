[Setup]
AppName=Archive Previewer & Thumbnail Provider
AppVersion=1.0
DefaultDirName={autopf}\ArchivePreviewer
DefaultGroupName=Archive Previewer
UninstallDisplayIcon={app}\ArchivePreviewer.exe
Compression=lzma2
SolidCompression=yes
OutputDir=Output
OutputBaseFilename=ArchivePreviewerSetup
PrivilegesRequired=admin
SetupIconFile=src\app_icon.ico
ChangesAssociations=yes
ArchitecturesInstallIn64BitMode=x64

[Files]
Source: "ArchiveThumbnailProvider.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "ArchivePreviewer.exe"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
; Register CLSID for Thumbnail Provider (System-wide HKLM)
Root: HKLM; Subkey: "Software\Classes\CLSID\{{C3861219-5E79-4B52-8706-03761D98357F}"; ValueType: string; ValueName: ""; ValueData: "Archive Thumbnail Provider"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\CLSID\{{C3861219-5E79-4B52-8706-03761D98357F}"; ValueType: dword; ValueName: "DisableProcessIsolation"; ValueData: 1; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\CLSID\{{C3861219-5E79-4B52-8706-03761D98357F}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\ArchiveThumbnailProvider.dll"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\CLSID\{{C3861219-5E79-4B52-8706-03761D98357F}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"; Flags: uninsdeletekey

; Register Approved Shell Extension
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved"; ValueType: string; ValueName: "{{C3861219-5E79-4B52-8706-03761D98357F}"; ValueData: "Archive Thumbnail Provider"; Flags: uninsdeletevalue

; Register our ProgID: ArchivePreviewer.AssocFile
Root: HKLM; Subkey: "Software\Classes\ArchivePreviewer.AssocFile"; ValueType: string; ValueName: ""; ValueData: "Archive Previewer File"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\ArchivePreviewer.AssocFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\ArchivePreviewer.exe,0"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\ArchivePreviewer.AssocFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\ArchivePreviewer.exe"" ""%1"""; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\ArchivePreviewer.AssocFile\ShellEx\{{E357F5F9-A113-4973-B299-0F16C7D041EC}"; ValueType: string; ValueName: ""; ValueData: "{{C3861219-5E79-4B52-8706-03761D98357F}"; Flags: uninsdeletekey

; Register under extensions
Root: HKLM; Subkey: "Software\Classes\.zip\ShellEx\{{E357F5F9-A113-4973-B299-0F16C7D041EC}"; ValueType: string; ValueName: ""; ValueData: "{{C3861219-5E79-4B52-8706-03761D98357F}"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.zip\OpenWithProgids"; ValueType: string; ValueName: "ArchivePreviewer.AssocFile"; ValueData: ""; Flags: uninsdeletevalue

Root: HKLM; Subkey: "Software\Classes\.rar\ShellEx\{{E357F5F9-A113-4973-B299-0F16C7D041EC}"; ValueType: string; ValueName: ""; ValueData: "{{C3861219-5E79-4B52-8706-03761D98357F}"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.rar\OpenWithProgids"; ValueType: string; ValueName: "ArchivePreviewer.AssocFile"; ValueData: ""; Flags: uninsdeletevalue

Root: HKLM; Subkey: "Software\Classes\.7z\ShellEx\{{E357F5F9-A113-4973-B299-0F16C7D041EC}"; ValueType: string; ValueName: ""; ValueData: "{{C3861219-5E79-4B52-8706-03761D98357F}"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.7z\OpenWithProgids"; ValueType: string; ValueName: "ArchivePreviewer.AssocFile"; ValueData: ""; Flags: uninsdeletevalue

; Also write current user (HKCU) values to ensure override priority
Root: HKCU; Subkey: "Software\Classes\CLSID\{{C3861219-5E79-4B52-8706-03761D98357F}"; ValueType: string; ValueName: ""; ValueData: "Archive Thumbnail Provider"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\CLSID\{{C3861219-5E79-4B52-8706-03761D98357F}"; ValueType: dword; ValueName: "DisableProcessIsolation"; ValueData: 1; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\CLSID\{{C3861219-5E79-4B52-8706-03761D98357F}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\ArchiveThumbnailProvider.dll"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\CLSID\{{C3861219-5E79-4B52-8706-03761D98357F}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"; Flags: uninsdeletekey

Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved"; ValueType: string; ValueName: "{{C3861219-5E79-4B52-8706-03761D98357F}"; ValueData: "Archive Thumbnail Provider"; Flags: uninsdeletevalue

Root: HKCU; Subkey: "Software\Classes\ArchivePreviewer.AssocFile"; ValueType: string; ValueName: ""; ValueData: "Archive Previewer File"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\ArchivePreviewer.AssocFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\ArchivePreviewer.exe,0"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\ArchivePreviewer.AssocFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\ArchivePreviewer.exe"" ""%1"""; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\ArchivePreviewer.AssocFile\ShellEx\{{E357F5F9-A113-4973-B299-0F16C7D041EC}"; ValueType: string; ValueName: ""; ValueData: "{{C3861219-5E79-4B52-8706-03761D98357F}"; Flags: uninsdeletekey

Root: HKCU; Subkey: "Software\Classes\.zip\ShellEx\{{E357F5F9-A113-4973-B299-0F16C7D041EC}"; ValueType: string; ValueName: ""; ValueData: "{{C3861219-5E79-4B52-8706-03761D98357F}"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\.zip\OpenWithProgids"; ValueType: string; ValueName: "ArchivePreviewer.AssocFile"; ValueData: ""; Flags: uninsdeletevalue

Root: HKCU; Subkey: "Software\Classes\.rar\ShellEx\{{E357F5F9-A113-4973-B299-0F16C7D041EC}"; ValueType: string; ValueName: ""; ValueData: "{{C3861219-5E79-4B52-8706-03761D98357F}"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\.rar\OpenWithProgids"; ValueType: string; ValueName: "ArchivePreviewer.AssocFile"; ValueData: ""; Flags: uninsdeletevalue

Root: HKCU; Subkey: "Software\Classes\.7z\ShellEx\{{E357F5F9-A113-4973-B299-0F16C7D041EC}"; ValueType: string; ValueName: ""; ValueData: "{{C3861219-5E79-4B52-8706-03761D98357F}"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\.7z\OpenWithProgids"; ValueType: string; ValueName: "ArchivePreviewer.AssocFile"; ValueData: ""; Flags: uninsdeletevalue

[Icons]
Name: "{group}\Archive Previewer"; Filename: "{app}\ArchivePreviewer.exe"
Name: "{autodesktop}\Archive Previewer"; Filename: "{app}\ArchivePreviewer.exe"
