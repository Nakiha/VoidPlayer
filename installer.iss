; VoidPlayer Inno Setup 安装脚本模板
; 此文件由 build.py 自动处理，{{VERSION}} 等占位符会被替换

[Setup]
AppId={{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
AppName=VoidPlayer
AppVersion={{VERSION}}
AppPublisher=VoidPlayer
AppPublisherURL=https://github.com/yorune/VoidPlayer
AppSupportURL=https://github.com/yorune/VoidPlayer/issues
DefaultDirName={autopf}\VoidPlayer
DefaultGroupName=VoidPlayer
AllowNoIcons=yes
; 输出配置
OutputDir={{OUTPUT_DIR}}
OutputBaseFilename=VoidPlayer-{{VERSION}}-Setup
SetupIconFile={{PROJECT_ROOT}}\resources\icons\icon.ico
; 压缩配置
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
; 权限
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
; UI
WizardStyle=modern
UninstallDisplayIcon={app}\VoidPlayer.exe
UninstallDisplayName=VoidPlayer

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce

[Files]
; 复制所有 Nuitka 输出文件
Source: "{{PROJECT_ROOT}}\build\dist\run_player.dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\VoidPlayer"; Filename: "{app}\VoidPlayer.exe"
Name: "{group}\{cm:UninstallProgram,VoidPlayer}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\VoidPlayer"; Filename: "{app}\VoidPlayer.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\VoidPlayer.exe"; Description: "{cm:LaunchProgram,VoidPlayer}"; Flags: postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
