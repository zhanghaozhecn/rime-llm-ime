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
;               默认 false；托盘 "LLM 重排设置" 开启，保存即重新部署生效）。
;   模型下载页（2026-09-04 引入；2026-09-05 改版）：Ready 页后询问是否
;               下载模型，下拉候选默认"暂不下载"；显示"当前模型位置"——
;               2026-09-30 起配置写在**方案 llm_rerank 节**（全局
;               llm_rerank.yaml 已取消）：扫描 %APPDATA%\Rime\*.schema.yaml
;               的 llm_rerank 节取 model_path，都没有则为默认位置
;               %APPDATA%\Rime（不做本机模型扫描）。
;               下载落点 = 当前位置；**不再写任何配置文件**——是否启用重排
;               由用户在托盘 "LLM 重排设置" 里选方案 → 接入 LLM（写组件行 +
;               配置节）决定。非 ASCII 的 model_path 经 ANSI 读入会错码——
;               按未配置处理（回落默认位置，用户可在 GUI 里选择真实路径）。

#define MyAppName "小狼毫 LLM 版"
#define MyAppVer "2026.09.30-4"  ; 同日重打安装包在日期后加 -2/-3/-4 序号（2026-09-11 用户定案，避免同号不同内容）
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
  ModelPage: TWizardPage;
  ModelCombo: TNewComboBox;
  CurPathLbl: TNewStaticText;
  DownloadPage: TDownloadWizardPage;

const
  // 与插件版 GUI 同源（unsloth 镜像，ModelScope 国内直连）
  ModelUrlStr = 'https://modelscope.cn/models/unsloth/Qwen3.5-0.8B-GGUF/resolve/master/Qwen3.5-0.8B-Q4_K_M.gguf';
  ModelFileName = 'Qwen3.5-0.8B-Q4_K_M.gguf';

// ---- 取"当前模型位置"：扫描 %APPDATA%\Rime\*.schema.yaml 的 llm_rerank 节
// 读 model_path（2026-09-30 起配置写在方案里，全局 llm_rerank.yaml 已取消）。
// 无任何方案配置节 → 默认位置。文件为无 BOM UTF-8，Inno 按 ANSI 读入——非
// ASCII 路径会错码，按未配置处理（回落默认位置，用户可在 GUI 里选真实路径）
function IsAscii(s: String): Boolean;
var
  i: Integer;
begin
  Result := True;
  for i := 1 to Length(s) do
    if Ord(s[i]) > 127 then begin
      Result := False;
      Exit;
    end;
end;

function SchemaModelPath(): String;
var
  fr: TFindRec;
  dir, path, s, val, key: String;
  lines: TArrayOfString;
  i, c: Integer;
  inCfg: Boolean;
begin
  Result := '';
  dir := ExpandConstant('{userappdata}\Rime');
  if not FindFirst(dir + '\*.schema.yaml', fr) then
    Exit;
  try
    repeat
      path := dir + '\' + fr.Name;
      if LoadStringsFromFile(path, lines) then begin
        inCfg := False;
        for i := 0 to GetArrayLength(lines) - 1 do begin
          s := lines[i];
          if (not inCfg) then begin
            if Pos('llm_rerank:', s) = 1 then
              inCfg := True;
            Continue;
          end;
          // 节内：缩进行继续，遇到顶层键即出节
          if (Length(s) > 0) and (s[1] <> ' ') and (s[1] <> #9) then
            inCfg := False;
          if not inCfg then
            Continue;
          c := Pos(':', s);
          if c < 1 then
            Continue;
          key := Trim(Copy(s, 1, c - 1));
          val := Trim(Copy(s, c + 1, Length(s) - c));
          if (Length(val) > 0) and (val[1] = '#') then
            Continue;
          if SameText(key, 'model_path') then begin
            if (Length(val) >= 2) and ((val[1] = '"') or (val[1] = '''')) and
               (val[Length(val)] = val[1]) then
              val := Copy(val, 2, Length(val) - 2);
            Result := val;
          end;
        end;
      end;
    until (Result <> '') or (not FindNext(fr));
  finally
    FindClose(fr);
  end;
  if (Result <> '') and (not IsAscii(Result)) then
    Result := '';
end;

// 当前生效的模型位置：方案 llm_rerank 节里配了 model_path 就用它，否则默认
// %APPDATA%\Rime（与 llm_filter 的默认一致）
function CurModelPath(): String;
begin
  Result := SchemaModelPath();
  if Result = '' then
    Result := ExpandConstant('{userappdata}\Rime\') + ModelFileName;
end;

procedure InitializeWizard();
var
  lbl: TNewStaticText;
  p: String;
begin
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing),
                                     SetupMessage(msgPreparingDesc), nil);
  ModelPage := CreateCustomPage(wpReady, '模型下载',
      '是否现在获取 LLM 重排模型？（不下载也可完成安装）');
  lbl := TNewStaticText.Create(ModelPage.Surface);
  lbl.Parent := ModelPage.Surface;
  lbl.Caption := 'LLM 重排需要 GGUF 模型文件（Qwen3.5-0.8B-Q4_K_M，约 508 MB），' +
      '放在下方"当前模型位置"。默认暂不下载；装完在托盘「LLM 重排设置」里' +
      '选方案 → 接入 LLM 即启用（配置写在该方案的 llm_rerank 节）。';
  lbl.WordWrap := True;
  lbl.SetBounds(ScaleX(0), ScaleY(0), ScaleX(430), ScaleY(44));
  ModelCombo := TNewComboBox.Create(ModelPage.Surface);
  ModelCombo.Parent := ModelPage.Surface;
  ModelCombo.Style := csDropDownList;
  ModelCombo.SetBounds(ScaleX(0), ScaleY(52), ScaleX(430), ScaleY(80));
  ModelCombo.Items.Add('暂不下载（默认）— 之后可重跑安装包，或在设置中自行放置模型');
  ModelCombo.Items.Add('下载 Qwen3.5-0.8B-Q4_K_M（约 508 MB，ModelScope）');
  ModelCombo.ItemIndex := 0;
  // 当前模型位置：读 yaml（配置过即显示），空置时为默认位置
  CurPathLbl := TNewStaticText.Create(ModelPage.Surface);
  CurPathLbl.Parent := ModelPage.Surface;
  p := CurModelPath();
  if FileExists(p) then
    CurPathLbl.Caption := '当前模型位置：' + p + '（文件已存在）'
  else
    CurPathLbl.Caption := '当前模型位置：' + p + '（文件不存在）';
  CurPathLbl.WordWrap := True;
  CurPathLbl.SetBounds(ScaleX(0), ScaleY(88), ScaleX(430), ScaleY(40));
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  idx: Integer;
  dest, err: String;
  ok, giveUp: Boolean;
begin
  Result := True;
  if CurPageID = ModelPage.ID then begin
    idx := ModelCombo.ItemIndex;
    if idx = 1 then begin
      // 下载分支：落点 = 当前显示位置（方案 llm_rerank 节配了就用它，否则默认）
      dest := CurModelPath();
      ForceDirectories(ExtractFileDir(dest));
      if FileExists(dest) then
        if MsgBox('已存在模型文件：' + dest + #13#10#13#10 +
                  '是否重新下载覆盖？（选"否"则沿用现有文件）',
                  mbConfirmation, MB_YESNO) = IDNO then
          Exit;   // 不写任何配置：启用与否由用户在「LLM 重排设置」里决定
      ok := False;
      giveUp := False;
      repeat
        DownloadPage.Clear;
        DownloadPage.Add(ModelUrlStr, ModelFileName, dest);
        DownloadPage.Show;
        try
          DownloadPage.Download;
          ok := True;
        except
          err := GetExceptionMessage;
        end;
        DownloadPage.Hide;
        if (not ok) and (MsgBox(
            '模型下载失败：' + err + #13#10 + #13#10 +
            '选"重试"再试；选"取消"跳过——之后重跑安装包下载，或手动下载：'#13#10 +
            ModelUrlStr + #13#10 + '放到：' + dest,
            mbError, MB_RETRYCANCEL) <> IDRETRY) then
          giveUp := True;
      until ok or giveUp;
      // 下载成功不写配置（2026-09-30：配置在方案 llm_rerank 节里，
      // 由「LLM 重排设置」→『接入 LLM』写入）
    end;
  end;
end;

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
