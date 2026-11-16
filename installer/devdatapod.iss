; devdatapod 安装包脚本
; 发布命名约定：Release tag = v<版本>，资产 = DevDataPod-v<版本>-Windows.exe
; 例：https://github.com/haigle0127/devdatapod/releases/download/v1.0.0/DevDataPod-v1.0.0-Windows.exe
; 设置页「检查更新」按这个约定从 releases/latest 里抠版本号。

#define MyAppName "devdatapod"
#define MyAppVersion "1.0.1"
#define MyAppPublisher "devdatapod contributors"
#define MyAppExeName "devdatapod.exe"
#define RepoRoot "D:\code\devdatapod"

[Setup]
AppId={{B8A3F2E1-7C4D-4E5F-9A1B-2C3D4E5F6A7B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={code:GetDefaultDir}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequiredOverridesAllowed=dialog
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

[Code]
// 默认安装目录：有 D 盘装 D:\Program Files\devdatapod，没有则退回 C:\Program Files。
// 注意 C:\Program Files 普通用户不可写，面板的数据目录（data/logs）就在安装根下，
// 那种机器上运行面板需要管理员权限，装完若写数据失败请右键管理员运行。
function GetDefaultDir(Param: String): String;
begin
  if DirExists('D:\') then
    Result := 'D:\Program Files\devdatapod'
  else
    Result := ExpandConstant('{autopf}\devdatapod');
end;

[Files]
; stage 目录由 windeployqt + VC 运行库组成（scripts 里的打包步骤生成）
Source: "{#RepoRoot}\dist\stage\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 devdatapod"; Flags: postinstall skipifsilent
