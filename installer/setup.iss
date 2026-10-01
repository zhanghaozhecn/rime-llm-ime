; setup.iss — 小狼毫 LLM 版安装包（Inno Setup，2026-08-27 直接安装版）
; 编译（开发机，installer\ 目录）: ..\scripts\build_pkg.bat 或
;   ISCC.exe setup.iss
; 产物: dist\weasel-llm-setup-<版本>.exe（约 15MB，不含模型——模型在配置 GUI 里下：
; 托盘 "LLM 重排设置" → 总控 → 「下载模型」，curl 断点续传；安装器不再询问）。
;
; 安装策略:
;   全新机器  = 复制文件（app + data + WeaselSetup）→ WeaselSetup.exe /s
;               （官方静默安装路径：System32/SysWOW64 TSF 部署 + MSCTF 注册）
;   已有小狼毫 = 装入其目录原地升级：停服务 → 系统 TSF DLL 改名腾位替换
;               （.llm_old，加载中的镜像可改名——实测），不动注册表
;   参数零写入方案：装完任何方案由 librime 全局挂载 llm_filter（enabled
;               默认 false；托盘 "LLM 重排设置" 开启，保存即重新部署生效）。
;   模型下载（2026-10-01 用户定案：两版统一到配置 GUI）：安装器**不再有**
;               下载页；托盘「LLM 重排设置」→ 总控 → 「下载模型」（与插件版
;               同一套 curl 断点续传，落点 = 模型路径框，失败保留 .download
;               分片可续传）。是否启用重排由用户在 GUI 里选方案 → 保存并生效
;               （写组件行 + 方案 llm_rerank 节）决定。

#define MyAppName "小狼毫 LLM 版"
#define MyAppVer "2026.10.01-2"  ; 同日重打安装包在日期后加 -2/-3/-4/-5 序号（2026-09-11 用户定案，避免同号不同内容）
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
; source\，不再从 weasel\output 单独引用）+ 数据目录 + TSF 应急修复 +
; 方案接入助手（schema_add.ps1，幂等插入/剥离 llm_filter，跨版自动转换）
Source: "source\*"; DestDir: "{app}"; Flags: ignoreversion
Source: "repair_tsf.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "schema_add.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\bin\data\*"; DestDir: "{app}\data"; Flags: recursesubdirs ignoreversion
; 系统位 TSF DLL 部署（改名腾位在 PrepareToInstall 完成）。
; 2026-08-31 修：原仅 IsUpgrade 执行、全新路径交给 WeaselSetup /s——但
; 从官方版/插件版卸载切换的机器会踩坑（官方卸载器删 TSF 注册键 → 误判
; 全新；System32 残留官方 DLL，/s 只补缺失不替换）→ 注册到官方 DLL：
; 打字正常但无 LLM 菜单、无 TSF 采上文（真机实测）。现无条件部署：全新
; 机两步顺序不变（[Files] 先放好我们的 DLL，[Run] WeaselSetup /s 见系统
; 位已存在直接注册即收敛）。
; restartreplace 兜底：改名腾位失败且文件被 TSF 占用时排队重启替换，
; 避免安装中途报错中止——2026-08-27 实测直写成功属幸运路径，不可依赖）
Source: "source\weaselx64.dll"; DestDir: "{sys}"; DestName: "weasel.dll"; Flags: ignoreversion restartreplace
Source: "source\weasel32.dll"; DestDir: "{syswow64}"; DestName: "weasel.dll"; Flags: ignoreversion restartreplace

[Run]
; 全新机器：官方静默安装（TSF 注册 + 系统 DLL 部署；安装器已提权）
Filename: "{app}\WeaselSetup.exe"; Parameters: "/s"; Flags: runhidden; Check: IsFreshInstall
; 始终启动服务（无 skipifsilent——静默安装同样要恢复输入法服务；
; postinstall 勾选项只保留 GUI）。
; runasoriginaluser（2026-09-11 真机踩坑）：server 必须非提权运行——
; 提权进程 GetActiveObject 读非提权 WPS 的 ROT 被 DCOM 跨完整性级别
; 拒绝，COM 上文旁路静默失效（TSF/历史不受影响，难定位）。历史潜伏
; 根因：升级安装时总有非提权旧 server 存续，新起的提权实例因单例退
; 出被掩盖；全新/清场安装必现（插件版 install_plugin.ps1 的 explorer
; 代启是同坑同解法）。右键"以管理员身份运行"安装器时此标志无效。
Filename: "{app}\WeaselServer.exe"; Flags: nowait runhidden runasoriginaluser
Filename: "{app}\WeaselLLMSetup.exe"; Flags: nowait postinstall skipifsilent unchecked; Description: "打开 LLM 重排设置（选择/检查模型与参数）"

[UninstallRun]
; 官方卸载注册路径（停止 TSF 注册 + 移除系统文件）
Filename: "{app}\WeaselSetup.exe"; Parameters: "/u"; Flags: runhidden; RunOnceId: "UnregTSF"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
var
  FreshInstall: Boolean;

// 2026-10-01 用户定案：安装器不再有"模型下载"页（那一步挪进配置 GUI：托盘
// 「LLM 重排设置」→ 总控 → 下载模型，与插件版同一套 curl 断点续传）。

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
  // 系统位改名腾位无条件执行（2026-08-31 修，配套 [Files] 去 IsUpgrade）：
  // 从官方版卸载切换的机器 TSF 注册键缺失（误判全新）但 System32 残留
  // 官方 DLL——必须腾位才能放入我们的构建。RenameAside 对不存在文件是
  // no-op，真全新机不受影响。
  CleanOld(ExpandConstant('{sys}'));
  CleanOld(ExpandConstant('{syswow64}'));
  RenameAside(ExpandConstant('{sys}\weasel.dll'));
  RenameAside(ExpandConstant('{syswow64}\weasel.dll'));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    CleanOld(ExpandConstant('{app}'));
end;
