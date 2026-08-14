#include "stdafx.h"

#include <PipeChannel.h>

using namespace weasel;
using namespace std;
using namespace boost;

#define _ThrowLastError throw ::GetLastError()
#define _ThrowCode(__c) throw __c
#define _ThrowIfNot(__c)                 \
  {                                      \
    DWORD err;                           \
    if ((err = ::GetLastError()) != __c) \
      throw err;                         \
  }

// 诊断日志 (与 WeaselTSF 的 TSFDbgLog 写同一文件, 独立实现避免跨项目依赖)
static void PipeDbgLog(const wchar_t* fmt, ...) {
  wchar_t buf[1024];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
  va_end(ap);
  wchar_t path[MAX_PATH];
  ExpandEnvironmentStringsW(L"%TEMP%\\weasel_tsf_dbg.log", path,
                            _countof(path));
  FILE* f = _wfopen(path, L"a");
  if (f) {
    fwprintf(f, L"[%llu] [%lu.%lu] %s\n",
             (unsigned long long)GetTickCount64(), GetCurrentProcessId(),
             GetCurrentThreadId(), buf);
    fclose(f);
  }
}

PipeChannelBase::PipeChannelBase(std::wstring&& pn_cmd,
                                 size_t bs = 4 * 1024,
                                 SECURITY_ATTRIBUTES* s = NULL)
    : pname(pn_cmd), buff_size(bs), sa(s) {};

PipeChannelBase::~PipeChannelBase() {
  // Thread-specific pointers are cleaned up automatically
}

bool PipeChannelBase::_Ensure() {
  try {
    HANDLE* phandle = _GetPipeHandle();
    if (_Invalid(*phandle)) {
      PipeDbgLog(L"Pipe Ensure: connect begin");
      *phandle = _Connect(pname.c_str());
      PipeDbgLog(L"Pipe Ensure: connect done ok=%d",
                 (int)!_Invalid(*phandle));
      return !_Invalid(*phandle);
    }
  } catch (...) {
    PipeDbgLog(L"Pipe Ensure: exception");
    return false;
  }

  return true;
}

HANDLE PipeChannelBase::_Connect(const wchar_t* name) {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  int iter = 0;
  while (_Invalid(pipe = _TryConnect())) {
    if (iter++ % 5 == 0)
      PipeDbgLog(L"Pipe Connect: retry iter=%d err=%d", iter,
                 (int)GetLastError());
    ::WaitNamedPipe(name, 500);
  }
  PipeDbgLog(L"Pipe Connect: connected");
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe, &mode, NULL, NULL)) {
    _ThrowLastError;
  }
  return pipe;
}

void PipeChannelBase::_Reconnect() {
  HANDLE* phandle = _GetPipeHandle();
  _FinalizePipe(*phandle);
  _Ensure();
}

HANDLE PipeChannelBase::_TryConnect() {
  auto pipe = ::CreateFile(pname.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
  if (!_Invalid(pipe)) {
    // connected to the pipe
    return pipe;
  }
  // being busy is not really an error since we just need to wait.
  _ThrowIfNot(ERROR_PIPE_BUSY);
  // All pipe instances are busy
  return INVALID_HANDLE_VALUE;
}

size_t PipeChannelBase::_WritePipe(HANDLE pipe, size_t s, char* b,
                                   bool flush) {
  DWORD lwritten;
  if (!::WriteFile(pipe, b, s, &lwritten, NULL) || lwritten <= 0) {
    _ThrowLastError;
  }
  // flush = 等对端读取已写数据。客户端写请求需要 (Server 必须读到);
  // 但 Server 端写响应绝不能 flush —— 客户端此刻可能在等 Server 读
  // 下一条消息而不读响应 → 双方 FlushFileBuffers 互锁死锁 (32 位旧版
  // weasel 客户端 TSF 激活时实测卡死, QQ 音乐打不开)。
  if (flush)
    ::FlushFileBuffers(pipe);
  return lwritten;
}

void PipeChannelBase::_FinalizePipe(HANDLE& p) {
  if (!_Invalid(p)) {
    DisconnectNamedPipe(p);
    CloseHandle(p);
  }
  p = INVALID_HANDLE_VALUE;
}

void PipeChannelBase::_Receive(HANDLE pipe, LPVOID msg, size_t rec_len) {
  DWORD lread = 0;
  ::SetLastError(0);
  BOOL success = ::ReadFile(pipe, msg, rec_len, &lread, NULL);
  DWORD read_err = GetLastError();
  if (!success) {
    // 部分读取 (消息 > rec_len) 时系统返回 ERROR_MORE_DATA(234) 或
    // 本机观察到的 ERROR_ALREADY_EXISTS(183); 两种都走 fallback 读完
    if (read_err != ERROR_MORE_DATA && read_err != ERROR_ALREADY_EXISTS)
      throw read_err;

    auto ctx = _GetContext();
    memset(ctx->buffer.get(), 0, buff_size);
    ::SetLastError(0);
    success = ::ReadFile(pipe, ctx->buffer.get(), buff_size, &lread, NULL);
    if (!success) {
      _ThrowLastError;
    }
  }
  _GetContext()->has_body = false;
}

HANDLE PipeChannelBase::_ConnectServerPipe(std::wstring& pn) {
  HANDLE pipe =
      CreateNamedPipe(pn.c_str(), PIPE_ACCESS_DUPLEX,
                      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                      PIPE_UNLIMITED_INSTANCES, buff_size, buff_size, 0, sa);
  if (pipe == INVALID_HANDLE_VALUE || !::ConnectNamedPipe(pipe, NULL)) {
    _ThrowLastError;
  }
  return pipe;
}
