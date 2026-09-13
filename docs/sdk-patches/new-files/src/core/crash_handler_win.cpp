/**
 * Unhandled-exception filter with a minidump and a guest-state report.
 * DP1 logging plan (docs/logging_audit_2026-09-08.md), item 1.
 */

#include <rex/crash_handler.h>

#include <rex/cvar.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32

#include "platform_win.h"

#include <dbghelp.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <filesystem>

#include <rex/logging.h>

#pragma comment(lib, "dbghelp.lib")

REXCVAR_DEFINE_BOOL(crash_handler_test, false, "Log",
                    "Raise an access violation right after the crash handler is installed "
                    "(verifies the log report and the minidump path)");

namespace rex::crash {

namespace {

std::string g_dump_dir;
std::string g_app_name;
GuestInfoProvider g_guest_provider = nullptr;
std::atomic<const char*> g_breadcrumb{nullptr};
std::atomic<int> g_reported{0};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

const char* ExceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW: return "INT_OVERFLOW";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case 0xE06D7363: return "CPP_EXCEPTION";
    case 0xC0000409: return "FAIL_FAST (stack buffer overrun / abort)";
    default: return "UNKNOWN";
  }
}

// "module.dll+0x1234" for a host address.
std::string DescribeHostAddress(const void* address) {
  HMODULE module = nullptr;
  if (address &&
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(address), &module) &&
      module) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p) {
      if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    char name[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, base, -1, name, MAX_PATH, nullptr, nullptr);
    char buf[MAX_PATH + 32];
    std::snprintf(buf, sizeof(buf), "%s+0x%llX", name,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                  reinterpret_cast<uintptr_t>(module)));
    return buf;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%llX",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)));
  return buf;
}

std::string WriteMinidump(EXCEPTION_POINTERS* ex_info) {
  if (g_dump_dir.empty()) {
    return {};
  }
  std::error_code ec;
  std::filesystem::create_directories(g_dump_dir, ec);
  std::time_t now = std::time(nullptr);
  std::tm tm_now{};
  localtime_s(&tm_now, &now);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_now);
  std::string path = g_dump_dir + "\\" + (g_app_name.empty() ? "crash" : g_app_name) + "_" +
                     stamp + "_" + std::to_string(GetCurrentProcessId()) + ".dmp";

  HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return {};
  }
  MINIDUMP_EXCEPTION_INFORMATION mei{};
  mei.ThreadId = GetCurrentThreadId();
  mei.ExceptionPointers = ex_info;
  mei.ClientPointers = FALSE;
  // Threads + stacks + the memory referenced by registers/stack, plus data
  // segments of loaded modules: enough to see guest context and host frames
  // without dumping the whole guest arena.
  const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
      MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs | MiniDumpWithHandleData |
      MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
  BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                              ex_info ? &mei : nullptr, nullptr, nullptr);
  CloseHandle(file);
  if (!ok) {
    DeleteFileA(path.c_str());
    return {};
  }
  return path;
}

// SEH needs a frame without objects that require unwinding; `out` is a reference.
bool CallProviderGuarded(GuestInfoProvider provider, std::string& out) {
  __try {
    provider(out);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ex_info);

// std::terminate (uncaught C++ exception, noexcept violation) and abort() take
// the fail-fast path on the UCRT, which bypasses the unhandled-exception
// filter: log what we can, flush, then continue into abort().
void TerminateHandler() {
  std::string what = "(no current exception)";
  if (std::exception_ptr ep = std::current_exception()) {
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      what = std::string("std::exception: ") + e.what();
    } catch (...) {
      what = "(non-std exception)";
    }
  }
  if (g_reported.exchange(1) == 0) {
    std::string report = "std::terminate called: " + what + "\n";
    if (const char* crumb = g_breadcrumb.load()) {
      report += "breadcrumb: ";
      report += crumb;
      report += "\n";
    }
    if (g_guest_provider) {
      if (!CallProviderGuarded(g_guest_provider, report)) {
        report += "(guest info provider faulted)\n";
      }
    }
    REXLOG_CRITICAL("*** TERMINATE ***\n{}", report);
    std::string dump = WriteMinidump(nullptr);
    if (!dump.empty()) {
      REXLOG_CRITICAL("Minidump written: {}", dump);
    }
    rex::FlushLogging();
  }
  std::abort();
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ex_info) {
  // Report once; a second fault inside the handler must not recurse.
  if (g_reported.exchange(1) != 0) {
    return EXCEPTION_EXECUTE_HANDLER;
  }
  const EXCEPTION_RECORD* rec = ex_info ? ex_info->ExceptionRecord : nullptr;
  const CONTEXT* ctx = ex_info ? ex_info->ContextRecord : nullptr;

  std::string report;
  report.reserve(4096);
  char line[512];
  if (rec) {
    std::snprintf(line, sizeof(line), "code 0x%08lX (%s) at %s", rec->ExceptionCode,
                  ExceptionName(rec->ExceptionCode),
                  DescribeHostAddress(rec->ExceptionAddress).c_str());
    report += line;
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
      const char* op = rec->ExceptionInformation[0] == 0   ? "read"
                       : rec->ExceptionInformation[0] == 1 ? "write"
                                                           : "execute";
      std::snprintf(line, sizeof(line), ", %s of address 0x%llX", op,
                    static_cast<unsigned long long>(rec->ExceptionInformation[1]));
      report += line;
    }
    report += "\n";
  }
  if (ctx) {
    std::snprintf(line, sizeof(line),
                  "host: rip=%s rsp=0x%llX rbp=0x%llX rax=0x%llX rcx=0x%llX rdx=0x%llX "
                  "r8=0x%llX r9=0x%llX host thread %lu\n",
                  DescribeHostAddress(reinterpret_cast<const void*>(ctx->Rip)).c_str(),
                  static_cast<unsigned long long>(ctx->Rsp),
                  static_cast<unsigned long long>(ctx->Rbp),
                  static_cast<unsigned long long>(ctx->Rax),
                  static_cast<unsigned long long>(ctx->Rcx),
                  static_cast<unsigned long long>(ctx->Rdx),
                  static_cast<unsigned long long>(ctx->R8),
                  static_cast<unsigned long long>(ctx->R9), GetCurrentThreadId());
    report += line;
  }
  if (const char* crumb = g_breadcrumb.load()) {
    report += "breadcrumb: ";
    report += crumb;
    report += "\n";
  }
  if (g_guest_provider) {
    if (!CallProviderGuarded(g_guest_provider, report)) {
      report += "(guest info provider faulted)\n";
    }
  }

  REXLOG_CRITICAL("*** CRASH ***\n{}", report);
  std::string dump = WriteMinidump(ex_info);
  if (!dump.empty()) {
    REXLOG_CRITICAL("Minidump written: {}", dump);
  } else {
    REXLOG_CRITICAL("Minidump not written");
  }
  rex::FlushLogging();

  if (g_previous_filter) {
    return g_previous_filter(ex_info);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void Install(const std::string& dump_dir, const std::string& app_name) {
  g_dump_dir = dump_dir;
  g_app_name = app_name;
  g_previous_filter = SetUnhandledExceptionFilter(UnhandledFilter);
  // Keep Windows from showing the "stopped working" dialog before our filter
  // ran; the filter itself decides what happens next.
  SetErrorMode(SetErrorMode(0) | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  // abort() must not fail-fast straight past us: no WER report, no message box.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  std::set_terminate(TerminateHandler);
  REXLOG_INFO("Crash handler installed (minidumps in {})", dump_dir);
  if (REXCVAR_GET(crash_handler_test)) {
    REXLOG_WARN("crash_handler_test: raising an access violation on purpose");
    volatile int* null_pointer = nullptr;
    *null_pointer = 1;
  }
}

void SetGuestInfoProvider(GuestInfoProvider provider) { g_guest_provider = provider; }

void Reassert() {
  static std::atomic<int> warned{0};
  LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(UnhandledFilter);
  if (current != UnhandledFilter && current != nullptr && warned.exchange(1) == 0) {
    REXLOG_WARN("Crash handler: the unhandled-exception filter had been replaced by {}; "
                "re-installed ours",
                DescribeHostAddress(reinterpret_cast<const void*>(current)));
  }
}

void SetBreadcrumb(const char* static_text) { g_breadcrumb.store(static_text); }

}  // namespace rex::crash

#else

namespace rex::crash {
void Install(const std::string&, const std::string&) {}
void SetGuestInfoProvider(GuestInfoProvider) {}
void SetBreadcrumb(const char*) {}
void Reassert() {}
}  // namespace rex::crash

#endif  // REX_PLATFORM_WIN32
