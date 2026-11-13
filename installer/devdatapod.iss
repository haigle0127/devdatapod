; devdatapod 安装包脚本
; 发布命名约定：Release tag = v<版本>，资产 = DevDataPod-v<版本>-Windows.exe
; 例：https://github.com/haigle0127/devdatapod/releases/download/v1.0.0/DevDataPod-v1.0.0-Windows.exe
; 设置页「检查更新」按这个约定从 releases/latest 里抠版本号。

#define MyAppName "devdatapod"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "devdatapod contributors"
#define MyAppExeName "devdatapod.exe"
#define RepoRoot "D:\code\devdatapod"

[Setup]
AppId={{B8A3F2E1-7C4D-4E5F-9A1B-2C3D4E5F6A7B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\devdatapod
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir={#RepoRoot}\dist
OutputBaseFilename=DevDataPod-v{#MyAppVersion}-Windows
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
SetupIconFile={#RepoRoot}\assets\logo.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
VersionInfoVersion={#MyAppVersion}
VersionInfoDescription=devdatapod - local dev services panel
VersionInfoProductName=devdatapod}
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; stage 目录由 windeployqt + VC 运行库组成（scripts 里的打包步骤生成）
Source: "{#RepoRoot}\dist\stage\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 devdatapod"; Flags: postinstall skipifsilent
