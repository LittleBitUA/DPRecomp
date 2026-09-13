/**
 * Process-wide crash handler (DP1 logging plan, 2026-09).
 *
 * Installs an unhandled-exception filter that, on a fatal exception, writes
 * the exception code, fault address, host module + offset and any guest-side
 * description provided by the runtime layer into the log, flushes the log and
 * writes a minidump next to the log files. The runtime layer (which knows about
 * guest threads and the recompiled function table) registers a provider that
 * appends the guest thread name, the PowerPC register state and a guest
 * back-chain walk.
 */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace rex::crash {

// Appends guest-side diagnostics (one or more lines) to `out`. Must not throw
// and should touch as little state as possible: it runs inside the crashing
// thread with no locks held guarantees.
using GuestInfoProvider = void (*)(std::string& out);

// Installs the handler. `dump_dir` receives `<app_name>_<pid>_<time>.dmp` files.
void Install(const std::string& dump_dir, const std::string& app_name);

void SetGuestInfoProvider(GuestInfoProvider provider);

// Records a short "where we are" breadcrumb (e.g. the guest function being
// hooked) that is printed with the crash report. Cheap: a pointer store.
void SetBreadcrumb(const char* static_text);

// Re-installs the unhandled-exception filter if another component (overlay,
// driver, SDK) replaced it after Install(). Cheap; call once per frame.
void Reassert();

}  // namespace rex::crash

namespace rex::system {
// Registers the guest-state provider (thread name, PPC registers, back-chain).
void RegisterGuestCrashInfo();
}  // namespace rex::system
