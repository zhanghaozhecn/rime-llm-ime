; setup.iss — 小狼毫 LLM 版安装包（Inno Setup，2026-08-27 直接安装版）
; 编译（开发机，installer\ 目录）: ..\scripts\build_pkg.bat 或
;   ISCC.exe setup.iss
; 产物: dist\weasel-llm-setup-<版本>.exe（约 15MB，不含模型——装后托盘
; "LLM 重排设置" 首次提示下载，GUI 内断点续传）。
;
; 安装策略:
;   全新机器  = 复制文件（app + data + WeaselSetup）→ WeaselSetup.exe /s
;               （官方静默安装路径：System32/SysWOW64 TSF 部署 + MSCTF 注册）
;   已有小狼毫 = 装入其目录原地升级：停服务 → 系统 TSF DLL 改名腾位替换
;               （.llm_old，加载中的镜像可改名——实测），不动注册表
;   参数零写入方案：装完任何方案由 librime 全局挂载 llm_filter（enabled
;   默认 false；托盘 "LLM 重排设置" 开启，保存即热重载生效）。

#define MyAppName "小狼毫 LLM 版"
#define MyAppVer "2026.08.27"
#define MyAppId "{{3F8A2D5C-6B1E-4F9A-8D73-9C2E5B7A1F40}"

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVer}
AppPublisher=rime-llm-ime
DefaultDirName={code:GetInstallDir}
PrivilegesRequired=admin
OutputDir=dist
OutputBaseFilename=weasel-llm-setup-{#MyAppVer}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
MinVersion=10.0

[Languages]
; 官方中文翻译（issrc Files/Languages/，2026-08-27 取 main 分支 6.5.0+ 版，
; 入库本地引用——Inno 6.4.3 起不再捆绑；编译器 6.4.x 对 6.5 翻译缺省项回退英文）
Name: "chs"; MessagesFile: "Languages\ChineseSimplified.isl"

[Registry]
; weasel 软件键（WeaselRoot：TSF 托盘菜单定位安装目录用——RERUN_SERVICE/
; LLM 重排设置均按此解析；官方 WeaselSetup 写入，但经历过变砖修复的机器
; 可能缺失，此处幂等补写。两个视图都写：64 位 TSF 读 64 位视图，
; 32 位 TSF（is_wow64）显式读 WOW6432Node）
Root: HKLM; Subkey: "Software\Rime\Weasel"; ValueType: string; ValueName: "WeaselRoot"; ValueData: "{app}"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\WOW6432Node\Rime\Weasel"; ValueType: string; ValueName: "WeaselRoot"; ValueData: "{app}"; Flags: uninsdeletevalue

[Files]
; 应用目录：source\* = 10 个载荷（8 个 LLM 组件 + WinSparkle.dll 依赖 +
; WeaselSetup.exe 注册工具——2026-08-27 起统一由 make_installer.ps1 同步入
; source\，不再从 weasel\output 单独引用）+ 数据目录 + TSF 应急修复
Source: "source\*"; DestDir: "{app}"; Flags: ignoreversion
Source: "repair_tsf.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\bin\data\*"; DestDir: "{app}\data"; Flags: recursesubdirs ignoreversion
; 升级路径：系统位 TSF DLL 原位替换（改名腾位在 PrepareToInstall 完成；
; 全新路径由 WeaselSetup /s 自己部署系统文件，跳过两条。
; restartreplace 兜底：改名腾位失败且文件被 TSF 占用时排队重启替换，
; 避免安装中途报错中止——2026-08-27 实测直写成功属幸运路径，不可依赖）
Source: "source\weaselx64.dll"; DestDir: "{sys}"; DestName: "weasel.dll"; Flags: ignoreversion restartreplace; Check: IsUpgrade
Source: "source\weasel32.dll"; DestDir: "{syswow64}"; DestName: "weasel.dll"; Flags: ignoreversion restartreplace; Check: IsUpgrade

[Run]
; 全新机器：官方静默安装（TSF 注册 + 系统 DLL 部署；安装器已提权）
Filename: "{app}\WeaselSetup.exe"; Parameters: "/s"; Flags: runhidden; Check: IsFreshInstall
; 始终启动服务（无 skipifsilent——静默安装同样要恢复输入法服务；
; postinstall 勾选项只保留 GUI）
Filename: "{app}\WeaselServer.exe"; Flags: nowait runhidden
Filename: "{app}\WeaselLLMSetup.exe"; Flags: nowait postinstall skipifsilent unchecked; Description: "打开 LLM 重排设置（首次使用：下载模型并启用）"

[UninstallRun]
; 官方卸载注册路径（停止 TSF 注册 + 移除系统文件）
Filename: "{app}\WeaselSetup.exe"; Parameters: "/u"; Flags: runhidden; RunOnceId: "UnregTSF"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
var
  FreshInstall: Boolean;

// 已有小狼毫 → 其目录原地升级；否则默认独立目录
//（不用 FindFirst：探测用固定候选清单覆盖官方 0.17.x-0.19.x 与本包自身）
function GetInstallDir(Param: string): string;
var
  pf: string;
  vers: array of String;
  i: Integer;
  cand: string;
begin
  Result := ExpandConstant('{pf}\Rime\weasel-llm');
  pf := ExpandConstant('{pf}\Rime');
  vers := ['weasel-llm', 'weasel-0.19.9', 'weasel-0.19.8', 'weasel-0.19.7',
           'weasel-0.19.6', 'weasel-0.19.5', 'weasel-0.19.4', 'weasel-0.19.3',
           'weasel-0.19.2', 'weasel-0.19.1', 'weasel-0.19.0',
           'weasel-0.18.9', 'weasel-0.18.8', 'weasel-0.18.7', 'weasel-0.18.6',
           'weasel-0.18.5', 'weasel-0.18.4', 'weasel-0.18.3', 'weasel-0.18.2',
           'weasel-0.18.1', 'weasel-0.18.0',
           'weasel-0.17.9', 'weasel-0.17.8', 'weasel-0.17.7', 'weasel-0.17.6',
           'weasel-0.17.5', 'weasel-0.17.4', 'weasel-0.17.3', 'weasel-0.17.2',
           'weasel-0.17.1', 'weasel-0.17.0'];
  for i := 0 to GetArrayLength(vers) - 1 do begin
    cand := pf + '\' + vers[i];
    if FileExists(cand + '\rime.dll') then begin
      Result := cand;
      Exit;
    end;
  end;
end;

// 全新判定：weasel TSF CLSID 未注册（64 位视图；安装器为 64 位进程）
function InitializeSetup(): Boolean;
begin
  FreshInstall := not RegKeyExists(
      HKEY_LOCAL_MACHINE,
      'SOFTWARE\Classes\CLSID\{A3F4CDED-B1E9-41EE-9CA6-7B4D0DE6CB0A}\InprocServer32');
  Result := True;
end;

function IsFreshInstall(): Boolean;
begin
  Result := FreshInstall;
end;

function IsUpgrade(): Boolean;
begin
  Result := not FreshInstall;
end;

procedure KillServer();
var
  ec: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im WeaselServer.exe', '',
       SW_HIDE, ewWaitUntilTerminated, ec);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im WeaselDeployer.exe', '',
       SW_HIDE, ewWaitUntilTerminated, ec);
end;

// 系统 TSF DLL 改名腾位：加载中的镜像可改名（实测），旧进程继续用旧镜像；
// .llm_old 由下次安装清理（此时已无人占用）
procedure RenameAside(path: string);
begin
  if FileExists(path) then begin
    // 旧 .llm_old 若仍被进程占用（删不掉），rename 也会失败——此时靠
    // [Files] restartreplace 兜底，不中止安装
    if not DeleteFile(path + '.llm_old') then
      Log(Format('llm_old busy (kept): %s', [path + '.llm_old']));
    if not RenameFile(path, path + '.llm_old') then
      Log(Format('rename aside failed (restartreplace fallback): %s', [path]));
  end;
end;

procedure CleanOld(dir: string);
var
  names: array of String;
  i: Integer;
begin
  names := ['rime.dll', 'WeaselServer.exe', 'WeaselDeployer.exe', 'opencc.dll',
            'vcomp140.dll', 'weaselx64.dll', 'weasel32.dll', 'WeaselLLMSetup.exe'];
  for i := 0 to GetArrayLength(names) - 1 do
    if FileExists(dir + '\' + names[i] + '.llm_old') then
      if not DeleteFile(dir + '\' + names[i] + '.llm_old') then
        Log(Format('llm_old busy, kept: %s', [names[i]]));
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  KillServer();
  Sleep(1500);
  CleanOld(ExpandConstant('{app}'));
  if not FreshInstall then begin
    CleanOld(ExpandConstant('{sys}'));
    CleanOld(ExpandConstant('{syswow64}'));
    RenameAside(ExpandConstant('{sys}\weasel.dll'));
    RenameAside(ExpandConstant('{syswow64}\weasel.dll'));
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    CleanOld(ExpandConstant('{app}'));
end;
