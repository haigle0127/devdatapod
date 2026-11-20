; devdatapod 2.0.0 安装包脚本（Inno Setup 6）
; 发布命名约定：Release tag = v<版本>，安装包资产 = DevDataPod-v<版本>-Windows.exe
; 便携版资产 = DevDataPod-v<版本>-Windows-portable.exe
; 设置页「检查更新」按这个约定从 releases/latest 里抠版本号。
; 与 1.0.x 共用同一个 AppId，装 2.0.0 会原地升级已安装的 1.0.x（保留其安装目录）。

#define MyAppName "devdatapod"
#define MyAppVersion "2.0.0"
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
SetupIconFile={#RepoRoot}\crates\ddp-app\assets\logo.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
VersionInfoVersion={#MyAppVersion}
VersionInfoDescription=devdatapod - local dev services panel
VersionInfoProductName=devdatapod
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Default.isl"

[Messages]
; 本机 Inno 未附带简体中文语言包，直接用 [Messages] 覆盖全部向导文案。
SetupAppTitle=安装程序
SetupWindowTitle=%1 安装程序
ButtonBack=< 上一步(&B)
ButtonNext=下一步(&N) >
ButtonInstall=安装(&I)
ButtonCancel=取消
ButtonFinish=完成(&F)
BrowseDialogTitle=浏览文件夹
BrowseDialogLabel=请在下面的列表中选择一个文件夹，然后点击“确定”。
WelcomeLabel1=欢迎使用 [name] 安装向导
WelcomeLabel2=本向导将在你的电脑上安装 [name/ver]。%n%n建议在继续之前关闭所有其他应用程序。
SelectDirDesc=应将 [name] 安装到哪个目录？
SelectDirLabel3=安装向导将把 [name] 安装到下面的文件夹中。
SelectDirBrowseLabel=点击“下一步”继续。若想选择其他文件夹，请点击“浏览”。
DirExistsTitle=文件夹已存在
DirExists=文件夹：%n%n%1%n%n已存在。是否仍要安装到该文件夹？
SelectTasksDesc=要执行哪些附加任务？
SelectTasksLabel2=请选择安装 [name] 时需要执行的附加任务，然后点击“下一步”。
SelectStartMenuFolderDesc=安装向导应把程序的快捷方式放到哪个开始菜单文件夹？
SelectStartMenuFolderLabel3=安装向导将创建程序快捷方式到下面的开始菜单文件夹中。
ReadyLabel1=安装向导已准备好将 [name] 安装到你的电脑上。
ReadyLabel2a=点击“安装”继续；若想复查或修改设置，请点击“上一步”。
ReadyLabel2b=点击“安装”继续。
ReadyMemoDir=目标位置：
ReadyMemoGroup=开始菜单文件夹：
ReadyMemoTasks=附加任务：
InstallingLabel=请稍候，安装向导正在将 [name] 安装到你的电脑上。
FinishedLabel=安装向导已完成 [name] 的安装。可以勾选下方选项立即启动程序。
FinishedLabelNoIcons=安装向导已完成 [name] 的安装。
ClickFinish=点击“完成”退出安装向导。
PrivilegesRequiredOverrideTitle=选择安装模式
PrivilegesRequiredOverrideInstruction=选择安装模式
PrivilegesRequiredOverrideText1=%1 可以为所有用户安装（需要管理员权限），或仅为当前用户安装。
PrivilegesRequiredOverrideText2=%1 可以仅为当前用户安装，或为所有用户安装（需要管理员权限）。
PrivilegesRequiredOverrideAllUsers=为所有用户安装(&A)
PrivilegesRequiredOverrideAllUsersRecommended=为所有用户安装（推荐）(&A)
PrivilegesRequiredOverrideCurrentUser=仅为我安装(&M)
PrivilegesRequiredOverrideCurrentUserRecommended=仅为我安装（推荐）(&M)

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
; stage 目录由 scripts/build.ps1 -Installer 生成（单个 devdatapod.exe）
Source: "{#RepoRoot}\dist\stage\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加快捷方式:"; Flags: checkedonce

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 devdatapod"; Flags: postinstall skipifsilent
