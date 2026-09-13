/**
 * Guest-state provider for the crash handler: guest thread name, PowerPC
 * register state and a back-chain walk of the guest stack, with every return
 * address mapped to the recompiled function that contains it.
 * DP1 logging plan (docs/logging_audit_2026-09-08.md), item 1.
 */

#include <rex/crash_handler.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

namespace rex::system {

namespace {

// Nearest registered function start at or below `address` (functions are
// registered by their entry address only). Bounded scan; guest code is small.
uint32_t FindFunctionStart(runtime::FunctionDispatcher* dispatcher, uint32_t address) {
  if (!dispatcher || !address) {
    return 0;
  }
  uint32_t a = address & ~3u;
  for (uint32_t n = 0; n < 0x20000 / 4; ++n, a -= 4) {
    if (dispatcher->GetFunction(a)) {
      return a;
    }
    if (a < 0x80000000u) {
      break;
    }
  }
  return 0;
}

uint32_t LoadGuestU32(memory::Memory* memory, uint32_t address) {
  const uint8_t* p = memory->TranslateVirtual<const uint8_t*>(address);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

bool PlausibleGuestAddress(uint32_t address) {
  return address >= 0x00010000u && address < 0xE0000000u && (address & 3u) == 0;
}

void Describe(std::string& out, runtime::FunctionDispatcher* dispatcher, uint32_t address,
              const char* label) {
  char line[128];
  uint32_t start = FindFunctionStart(dispatcher, address);
  if (start) {
    std::snprintf(line, sizeof(line), "  %-4s %08X  sub_%08X+0x%X\n", label, address, start,
                  address - start);
  } else {
    std::snprintf(line, sizeof(line), "  %-4s %08X  (not in a recompiled function)\n", label,
                  address);
  }
  out += line;
}

void GuestCrashInfo(std::string& out) {
  char line[256];
  XThread* thread = XThread::GetCurrentThread();
  if (thread) {
    std::snprintf(line, sizeof(line), "guest thread: %s (id %u)\n", thread->name().c_str(),
                  thread->thread_id());
    out += line;
  } else {
    out += "guest thread: none (host thread)\n";
  }

  runtime::ThreadState* ts = runtime::ThreadState::Get();
  if (!ts || !ts->context()) {
    return;
  }
  const ::PPCContext* c = ts->context();
  std::snprintf(line, sizeof(line),
                "ppc: r1=%08X lr=%08X ctr=%08X r3=%08X r4=%08X r5=%08X r6=%08X r11=%08X "
                "r30=%08X r31=%08X\n",
                c->r1.u32, static_cast<uint32_t>(c->lr), c->ctr.u32,
                c->r3.u32, c->r4.u32, c->r5.u32, c->r6.u32, c->r11.u32, c->r30.u32, c->r31.u32);
  out += line;

  KernelState* ks = kernel_state();
  if (!ks) {
    return;
  }
  runtime::FunctionDispatcher* dispatcher = ks->function_dispatcher();
  memory::Memory* memory = ks->memory();
  out += "guest stack (innermost first):\n";
  Describe(out, dispatcher, static_cast<uint32_t>(c->lr), "lr");

  // Every recompiled prologue is `mflr r12; stw r12,-8(r1); ...; stwu r1,-N(r1)`:
  // the caller's return address sits 8 bytes below the caller's frame, and each
  // frame's first word is the previous frame pointer (the back chain).
  uint32_t sp = c->r1.u32;
  for (int depth = 0; depth < 40; ++depth) {
    if (!PlausibleGuestAddress(sp)) {
      break;
    }
    uint32_t back = LoadGuestU32(memory, sp);
    if (!PlausibleGuestAddress(back) || back <= sp) {
      break;
    }
    uint32_t return_address = LoadGuestU32(memory, back - 8);
    if (!PlausibleGuestAddress(return_address) || return_address < 0x80000000u) {
      break;
    }
    Describe(out, dispatcher, return_address, "");
    sp = back;
  }
}

}  // namespace

void RegisterGuestCrashInfo() { rex::crash::SetGuestInfoProvider(&GuestCrashInfo); }

}  // namespace rex::system
