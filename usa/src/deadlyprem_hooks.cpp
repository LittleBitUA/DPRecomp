// deadlyprem - mid-asm hooks (DP1, 2026-09-06).
//
// 60 FPS: port of ehw's Xenia patch for the USA XEX (494F07D4, DPRecomp
// issue #3) to the PAL XEX (4D5607D1). The patched instructions sit in the
// frame pacing / vblank gate routine (PAL sub_825224B0); the PAL copy of that
// routine was located by matching the instruction pattern (same +0x28 / +0x94
// offsets):
//   USA 0x8252A27C  fadds f13,f0,f29 -> fmr f13,f0    PAL 0x82522654
//   USA 0x8252A2A4  bge   -> b +0x10 (skip the gate)  PAL 0x8252267C -> 0x8252268C
//   USA 0x8252A310  subf r8,r11,r10 -> li r8,1        PAL 0x825226E8
// Instead of patching bytes, the codegen emits calls to the hooks below at
// those addresses (see [[midasm_hook]] in deadlyprem_config.toml), and a cvar
// keeps the original behaviour reachable without a rebuild.
//
// How the routine works (constants read from the XEX, 2026-09-06):
//   * frame limiter: elapsed = QPC delta in ms; if elapsed < 16.667 ms then
//     Sleep((16.667 - elapsed) + 11.667) and re-check -> ~28 ms = the 30 FPS
//     cap. Hook 1 drops the +11.667 so the limiter targets 16.667 ms.
//   * vblank gate: wait until (vblank_now - vblank_last) >= 2. Hook 2 skips it.
//   * logic tick: r8 = vblank_now - vblank_last, converted to float, clamped
//     to [1, 4] and multiplied into the game's delta-time global (units of
//     1/60 s; 2 at 30 FPS). ehw's original `li r8,1` pins it to exactly one
//     vblank, which decouples game time from real time: every frame longer
//     than 16.67 ms (Sleep overshoot, heavy cutscenes) slows the game while
//     the streamed audio keeps real time -> animation drifts behind the audio
//     (DPRecomp #12). Hook 3 (2026-09-06, v1.1) therefore derives the tick from
//     the host clock instead: tick = frame_time / 16.667 ms, so a 50 FPS frame
//     advances 1.2 ticks and a 60 FPS frame exactly 1.0.
//     PAL 0x82522704 (USA 0x8252A32C) = `stfs f0,-11344(r24)`, right after the
//     integer -> float conversion of the vblank delta; the hook overrides f0.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/stats.h>
#include <rex/input/state_filter.h>

#include "deadlyprem_pad_layout.h"
#include "deadlyprem_title_skip.h"

#include "deadlyprem_pch.h"  // PPCRegister / PPCContext (generated/default is on the include path)

REXCVAR_DEFINE_BOOL(dp_60fps, true, "DP1",
                    "60 FPS: bypass the vblank-count gate, keep the in-game frame limiter at "
                    "16.67 ms and derive the logic tick from real frame time (ehw's patch, "
                    "DPRecomp #3, tick fix #12). Needs a 60 Hz guest video mode "
                    "(video_mode_refresh_rate = 60).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(dp_60fps_tick_min, 0.5, "DP1",
                      "Lower clamp of the real-time logic tick in 1/60 s units. 1.0 = never "
                      "slower than one tick per frame (v1.1; frames shorter than 16.67 ms then "
                      "ran the game ahead of the audio, #12), 0.5 = follow real time (v1.2)")
    .range(0.1, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(dp_60fps_tick_max, 4.0, "DP1",
                      "Upper clamp of the real-time logic tick in 1/60 s units (the game itself "
                      "clamps to [1, 4]); frames longer than this are treated as a hitch")
    .range(1.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_60fps_stats, false, "DP1",
                    "Log frame time / game-time-vs-real-time statistics every 600 frames "
                    "(diagnostics for #12)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_60fps_integer_tick, true, "DP1",
                    "60 FPS: feed the game whole 1/60 s ticks (1, sometimes 2) with an error "
                    "accumulator, so game time tracks real time exactly while the tick stays "
                    "an integer like the console's vblank count (the game's scripted scenes "
                    "misbehave with fractional ticks, and this is also what the PC port does "
                    "in cutscenes). false = fractional real-time tick clamped to "
                    "[dp_60fps_tick_min, dp_60fps_tick_max] (1.1/1.2 behaviour)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// PAL 0x82522654, after `fadds f13,f0,f29`: equivalent of `fmr f13,f0`.
void DP60FpsTimerHook(PPCRegister& f13, PPCRegister& f0) {
  if (REXCVAR_GET(dp_60fps)) {
    f13.f64 = f0.f64;
  }
}

// PAL 0x8252267C, before `bge cr6,0x8252268C`: true = take the branch
// unconditionally (equivalent of replacing bge with b).
bool DP60FpsVblankGateHook() {
  return REXCVAR_GET(dp_60fps);
}

// PAL 0x8252271C (USA 0x8252A344), before `bge cr6,<skip fmr f0,f28>`: the game
// clamps the tick it just stored to >= 1.0 (f28 = DAT_82000B94 = 1.0f). With
// the real-time tick that turns the symmetric frame-time jitter (13..20 ms
// around a 16.7 ms mean) into a systematic speed-up: short frames are rounded
// up to 1.0, long frames count in full, measured +0.2..0.7 % here and more on
// jittery machines - the animation runs ahead of the streamed audio over a
// long cutscene (DPRecomp #12, 1.1 and 1.2). Take the branch unconditionally
// in 60 FPS mode; DP60FpsTickHook has already applied [tick_min, tick_max].
bool DP60FpsTickClampHook() {
  return REXCVAR_GET(dp_60fps);
}

// PAL 0x82522704, before `stfs f0,-11344(r24)`: f0 holds the vblank delta as
// a float. Replace it with the real frame time in 1/60 s units.
void DP60FpsTickHook(PPCRegister& f0) {
  using clock = std::chrono::steady_clock;
  static clock::time_point last_frame{};
  static bool have_last = false;
  static double accumulator = 0.0;  // real time not yet handed to the game, in ticks
  const clock::time_point now = clock::now();
  if (!REXCVAR_GET(dp_60fps)) {
    // 30 FPS mode: keep the game's own vblank-count tick, just track time.
    last_frame = now;
    have_last = true;
    accumulator = 0.0;
    return;
  }
  const double raw_frame_ms =
      have_last ? std::chrono::duration<double, std::milli>(now - last_frame).count() : 0.0;
  last_frame = now;
  have_last = true;
  // Loading screens / alt-tab / debugger stalls: a very long gap is a hitch,
  // not game time to catch up on (the game clamps to 4 ticks anyway).
  const bool frame_valid = raw_frame_ms > 0.0 && raw_frame_ms < 500.0;
  const double real_ticks = frame_valid ? raw_frame_ms / (1000.0 / 60.0) : 1.0;

  const double tick_max = REXCVAR_GET(dp_60fps_tick_max);
  double tick;
  if (REXCVAR_GET(dp_60fps_integer_tick)) {
    // Whole ticks, like the console's vblank delta: hand out round(accumulated
    // real time), never less than 1 (the game never ran slower than one tick
    // per frame), at most tick_max; the remainder carries over, so a 16.72 ms
    // average frame gives 1, 1, 1, ... and a 2 every few hundred frames
    // instead of a permanent 0.3 % speed-up.
    accumulator += real_ticks;
    tick = std::floor(accumulator + 0.5);
    tick = std::clamp(tick, 1.0, std::max(1.0, tick_max));
    accumulator -= tick;
    // Never bank more than a couple of ticks of debt or credit (hitches).
    accumulator = std::clamp(accumulator, -2.0, 2.0);
  } else {
    const double tick_min = std::min(REXCVAR_GET(dp_60fps_tick_min), tick_max);
    tick = std::clamp(real_ticks, tick_min, tick_max);
    accumulator = 0.0;
  }
  f0.f64 = tick;

  // Diagnostics (#12): game time vs real time.
  if (REXCVAR_GET(dp_60fps_stats) && frame_valid) {
    static uint32_t n = 0;
    static double sum_ms = 0.0, min_ms = 1e9, max_ms = 0.0, sum_tick = 0.0;
    static uint32_t sub_one = 0, multi = 0;
    ++n;
    sum_ms += raw_frame_ms;
    min_ms = std::min(min_ms, raw_frame_ms);
    max_ms = std::max(max_ms, raw_frame_ms);
    sum_tick += tick;
    if (tick < 1.0) ++sub_one;
    if (tick >= 2.0) ++multi;
    if (n >= 600) {
      REXLOG_INFO(
          "DP60 stats: {} frames avg {:.3f} ms (min {:.2f} max {:.2f}), tick<1 on {} frames, "
          "tick>=2 on {} frames, accumulator {:+.3f}; game/real {:.4f}",
          n, sum_ms / n, min_ms, max_ms, sub_one, multi, accumulator,
          sum_tick * (1000.0 / 60.0) / sum_ms);
      n = 0;
      sum_ms = 0.0;
      min_ms = 1e9;
      max_ms = 0.0;
      sum_tick = 0.0;
      sub_one = 0;
      multi = 0;
    }
  }
}

// ---------------------------------------------------------------------------
// DP1 diagnostics (2026-09-13): guest thread names. The game's thread wrapper
// (PAL sub_82530430 / USA sub_82538058) receives r4 = descriptor
// {entry, arg, name, stack_size, priority, flags} and calls CreateThread
// synchronously; the name ("GameThread", "LoadThread", "PhysicsThread", ...)
// is announced to the runtime so the new XThread carries it in every log line.
// ---------------------------------------------------------------------------
void DPThreadCreateNameHook(PPCRegister& r4) {
  const uint32_t desc = r4.u32;
  if (!desc) return;
  auto* memory = REX_KERNEL_MEMORY();
  const uint32_t name_ptr = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<const uint8_t*>(desc + 8));
  if (name_ptr < 0x82000000u || name_ptr >= 0x84400000u) return;
  const char* name = memory->TranslateVirtual<const char*>(name_ptr);
  size_t len = 0;
  while (len < 31 && name[len] >= 0x20 && name[len] < 0x7F) ++len;
  if (!len) return;
  rex::system::XThread::SetPendingGuestThreadName(std::string_view(name, len));
}

// ---------------------------------------------------------------------------
// DP1 diagnostics (2026-09-13): the game's own error channels into the log.
//
// PhysX 2.6 error stream. Every PhysX-side check funnels through
// PAL sub_825686A8(code, file, line, fmt, ...) -> sub_82569290 (USA
// sub_82570380 -> sub_82570F68), which vsnprintf-s the message into a 160-byte
// stack buffer (grown from the heap when it does not fit) and hands it to the
// game's NxUserOutputStream. The hook sits right after the vsnprintf loop,
// where r30 = message, r25 = NxErrorCode, r23 = file, r22 = line
// (PAL 0x825693A0, USA 0x82571078; both bodies are instruction-identical).
// Code 107 goes to reportAssertViolation, 208 to print(), the rest to
// reportError(code, message, file, line) - the same split as the game makes.
// ---------------------------------------------------------------------------

REXCVAR_DEFINE_BOOL(dp_physx_log, true, "DP1",
                    "Write PhysX error-stream messages (invalid parameters, skipped calls, "
                    "out-of-memory, asserts) into the log: the first 5 of each distinct text, "
                    "then every 100th, with a running count")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_audio_cue_log, false, "DP1",
                    "Log every sound-effect cue the game triggers through its SE wrapper "
                    "(cue id, engine, arguments); the frame number in the line pattern shows "
                    "which update fired it")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// Copies a NUL-terminated guest string, or returns a placeholder when the
// pointer is outside every guest heap (a bad pointer must never take the
// log hook down with it).
std::string DPGuestString(uint32_t guest_ptr, size_t max_len) {
  if (!guest_ptr) return "(null)";
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory || !memory->LookupHeap(guest_ptr)) return "(bad ptr)";
  const char* p = memory->TranslateVirtual<const char*>(guest_ptr);
  size_t len = 0;
  while (len < max_len && p[len] != '\0') ++len;
  return std::string(p, len);
}

const char* DPPhysXCodeName(int code) {
  switch (code) {
    case 0: return "no-error";
    case 1: return "invalid-parameter";
    case 2: return "invalid-operation";
    case 4: return "out-of-memory";
    case 8: return "internal-error";
    case 100: return "assertion";
    case 107: return "assert-violation";
    case 108: return "db-warning";
    case 109: return "db-info";
    case 208: return "print";
    default: return "code";
  }
}

std::mutex g_dp_physx_mutex;
std::unordered_map<std::string, uint32_t> g_dp_physx_counts;
uint32_t g_dp_physx_total = 0;

}  // namespace

// PAL 0x825693A0 / USA 0x82571078 (see the block comment above).
void DPPhysXReportHook(PPCRegister& r30, PPCRegister& r25, PPCRegister& r23, PPCRegister& r22) {
  if (!REXCVAR_GET(dp_physx_log)) return;
  const int code = r25.s32;
  const std::string message = DPGuestString(r30.u32, 1024);
  uint32_t count = 0;
  uint32_t total = 0;
  {
    std::lock_guard<std::mutex> lock(g_dp_physx_mutex);
    count = ++g_dp_physx_counts[message];
    total = ++g_dp_physx_total;
  }
  rex::stats::Set("game/physx_reports", total);
  if (count > 5 && (count % 100) != 0) return;
  if (code == 208) {
    REXLOG_INFO("PhysX print: {} (x{})", message, count);
    return;
  }
  const std::string file = DPGuestString(r23.u32, 256);
  REXLOG_WARN("PhysX {} ({}): {} [{}:{}] (x{})", DPPhysXCodeName(code), code, message, file,
              r22.s32, count);
}

// PAL 0x8225D0B8 / USA 0x8225CFE0: SE wrapper entry, sub(engine, cue 0..1030,
// r5, r6) -> table at engine+21692, 12 bytes per cue -> sub_8225B740.
void DPAudioCueHook(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
  if (!REXCVAR_GET(dp_audio_cue_log)) return;
  REXLOG_INFO("SE cue {} (engine {:08X}, r5 {:08X}, r6 {:08X})", r4.s32, r3.u32, r5.u32, r6.u32);
}

// DP1 #19 (2026-09-14): title mode state logger + intro skip. PAL sub_8241C840
// (USA sub_8241BFA0) is the title mode's phase dispatcher: r3 = this, r4 =
// phase (0 init, 1 update, 6/18 draw passes), r5 = params, *(r5) = requested
// start state (80 on boot, 0 when the game returns to the title). The state
// machine lives in a global block (PAL 0x83D7F688 / USA 0x83D7F680): +4
// current state, +8 next state, +48 press-start countdown (frames), +60 load
// step (18 = layouts loaded), +6964 per-state timer (frames). The decision
// logic and the measured state sequence are in src/deadlyprem_title_skip.h
// (tests/title_skip_test.cpp); this hook only reads and writes guest memory.
REXCVAR_DEFINE_STRING(dp_skip_intro, "off", "DP1",
                      "Skip the intro: off = the four publisher logos play; logos = jump "
                      "straight to the title screen; menu = also press Start for you and land "
                      "in the main menu. Exact names; anything else = off (DPRecomp #19)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_title_state_log, false, "DP1",
                    "Log the title mode's state transitions (logos, press start, attract demo, "
                    "main menu) with a timestamp; diagnostics for the intro skip (#19)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#if defined(DP_REGION_USA)
constexpr uint32_t kDPTitleBlockDefault = 0x83D7F680;
#else
constexpr uint32_t kDPTitleBlockDefault = 0x83D7F688;
#endif
// Read once per title entry (phase 0); a value changed at runtime applies to
// the next entry, and the state read from the block must match the start
// state the game passed in before anything is written.
REXCVAR_DEFINE_INT32(dp_title_block, static_cast<int32_t>(kDPTitleBlockDefault), "DP1",
                     "Guest address of the title mode's state block (PAL 0x83D7F688, USA "
                     "0x83D7F680, stored as a signed 32-bit number); 0 disables the intro skip");

namespace {
constexpr uint32_t kTitleTimerOffset = 6964;

uint8_t* DPTitlePtr(uint32_t guest_ptr) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory || !memory->LookupHeap(guest_ptr)) return nullptr;
  return memory->TranslateVirtual<uint8_t*>(guest_ptr);
}
uint32_t DPLoadU32(uint32_t guest_ptr) {
  uint8_t* p = DPTitlePtr(guest_ptr);
  return p ? rex::memory::load_and_swap<uint32_t>(p) : 0xFFFFFFFFu;
}
float DPLoadF32(uint32_t guest_ptr) {
  uint8_t* p = DPTitlePtr(guest_ptr);
  return p ? rex::memory::load_and_swap<float>(p) : 0.0f;
}
void DPStoreU32(uint32_t guest_ptr, uint32_t v) {
  if (uint8_t* p = DPTitlePtr(guest_ptr)) rex::memory::store_and_swap<uint32_t>(p, v);
}
void DPStoreF32(uint32_t guest_ptr, float v) {
  if (uint8_t* p = DPTitlePtr(guest_ptr)) rex::memory::store_and_swap<float>(p, v);
}
double DPSecondsSinceStart() {
  static const auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
dp::SkipIntro DPSkipIntroLevel() {
  const std::string v = REXCVAR_GET(dp_skip_intro);  // copy: hot-reload writes the cvar
  bool ok = false;
  const dp::SkipIntro level = dp::ParseSkipIntro(v.c_str(), &ok);
  if (!ok) {
    static std::string warned_for;
    if (warned_for != v) {
      warned_for = v;
      REXLOG_WARN("dp_skip_intro = \"{}\" is not one of off / logos / menu; treating it as off", v);
    }
  }
  return level;
}

// Title mode bookkeeping. The hook runs on the game thread only.
dp::TitleSkipState g_title;
uint32_t g_title_block = 0;
}  // namespace

void DPTitleModeHook(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  const uint32_t phase = r4.u32 & 0xFF;
  const bool log = REXCVAR_GET(dp_title_state_log);
  static uint32_t last_state = 0xFFFFFFFFu, last_next = 0xFFFFFFFFu, last_sub = 0xFFFFFFFFu;
  if (phase == dp::kTitlePhaseInit) {
    dp::TitleSkipOnInit(g_title, DPLoadU32(r5.u32));
    g_title_block = static_cast<uint32_t>(REXCVAR_GET(dp_title_block));
    last_state = last_next = last_sub = 0xFFFFFFFFu;
    if (log) {
      REXLOG_INFO("title init: this {:08X}, params {:08X}, start state {} (t={:.2f}s)", r3.u32, r5.u32,
                  static_cast<int32_t>(g_title.start_state), DPSecondsSinceStart());
    }
    return;
  }
  if (phase != dp::kTitlePhaseUpdate || g_title_block == 0) return;
  const uint32_t block = g_title_block;
  const uint32_t state = DPLoadU32(block + 4);
  const uint32_t next = DPLoadU32(block + 8);
  const uint32_t sub = DPLoadU32(block + 60);
  if (log && (state != last_state || next != last_next || sub != last_sub)) {
    REXLOG_INFO("title state {} (next {}, load step {}, countdown {:.2f}) t={:.2f}s",
                static_cast<int32_t>(state), static_cast<int32_t>(next), static_cast<int32_t>(sub),
                DPLoadF32(block + 48), DPSecondsSinceStart());
    last_state = state; last_next = next; last_sub = sub;
  }
  const dp::TitleSkipAction a = dp::TitleSkipOnUpdate(g_title, DPSkipIntroLevel(), state, next, sub);
  if (a.log_block_mismatch) {
    REXLOG_WARN("title block 0x{:08X}: state {} does not match start state {}; intro skip disabled "
                "for this title entry (dp_title_block)",
                block, static_cast<int32_t>(state), static_cast<int32_t>(g_title.start_state));
  }
  if (a.jump_to_title) {
    DPStoreU32(block + 4, dp::kTitleStateTitleFadeIn);
    DPStoreU32(block + 8, dp::kTitleStateTitleFadeIn);
    DPStoreF32(block + kTitleTimerOffset, 0.0f);
    REXLOG_INFO("Intro skipped: title state 80 -> 97 (dp_skip_intro = {}, t={:.2f}s)",
                REXCVAR_GET(dp_skip_intro), DPSecondsSinceStart());
  }
  if (a.log_started_pulsing) {
    REXLOG_INFO("Intro skipped: pressing Start on the title screen (dp_skip_intro = menu, t={:.2f}s)",
                DPSecondsSinceStart());
  }
  if (a.inject_start) {
    rex::input::InjectButtons(0, static_cast<uint16_t>(rex::input::X_INPUT_GAMEPAD_START),
                              dp::kTitleStartPulsePolls);
  }
  if (a.cancel_injection) rex::input::InjectButtons(0, 0, 0);
  if (a.log_start_left) {
    if (a.left_state == dp::kTitleStateStartAccepted) {
      REXLOG_INFO("Intro skipped: Start accepted after {} frames", a.pulse_frames);
    } else {
      REXLOG_INFO("Intro skipped: title screen left for state {} after {} frames without our Start",
                  static_cast<int32_t>(a.left_state), a.pulse_frames);
    }
  }
  if (a.log_gave_up) {
    REXLOG_WARN("Intro skipped: the title screen ignored Start for {} frames; giving up",
                dp::kTitleStartPulseMaxFrames);
  }
}

// DP1 #19 (2026-09-14): controller layout (src/deadlyprem_pad_layout.h,
// tests/pad_layout_test.cpp). Installed by the app as the input system's
// state filter; runs on every physical pad's state before the devices of a
// user are merged (the MnK synthetic device is left alone: its bindings
// already target guest buttons). The vehicle check reads the camera anchor's
// +0x3C flags (bit 1 set while York is in a car; observed in logs 119/120 on
// 2026-09-13, not proven from the disassembly - if it ever reads wrong, the
// symptom is the car ignoring the triggers, and the launcher switch is the
// way out).
REXCVAR_DEFINE_STRING(dp_pad_layout, "original", "DP1",
                      "Controller layout: original = Xbox 360 (RT aims, A fires, LT holds "
                      "breath), dc = Director's Cut (LT aims, RT fires, A while aiming holds "
                      "breath; RT outside aiming = the old LT). Driving is never remapped. "
                      "Exact names; anything else = original (DPRecomp #19)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

bool DPInVehicle();  // deadlyprem_camera_hook.cpp

namespace {
dp::PadLayout DPPadLayoutValue() {
  const std::string v = REXCVAR_GET(dp_pad_layout);  // copy: hot-reload writes the cvar
  bool ok = false;
  const dp::PadLayout layout = dp::ParsePadLayout(v.c_str(), &ok);
  if (!ok) {
    static std::string warned_for;
    if (warned_for != v) {
      warned_for = v;
      REXLOG_WARN("dp_pad_layout = \"{}\" is not one of original / dc; treating it as original", v);
    }
  }
  return layout;
}
// One latch per guest user; the filter is called under the input system's
// per-call sequence for a user, and users never share a latch.
std::mutex g_pad_layout_mutex;
dp::PadLayoutState g_pad_layout_state[4];
}  // namespace

void DPInstallPadLayoutFilter() {
  rex::input::SetStateFilter([](uint32_t user_index, bool synthetic, rex::input::X_INPUT_GAMEPAD& pad) {
    // Keyboard/mouse emulation already binds keys to the game's own buttons
    // (Space = RT etc.); only physical pads get the alternative layout.
    if (synthetic || user_index >= 4) return;
    const dp::PadLayout layout = DPPadLayoutValue();
    uint16_t buttons = static_cast<uint16_t>(pad.buttons);
    uint8_t lt = pad.left_trigger;
    uint8_t rt = pad.right_trigger;
    {
      std::lock_guard<std::mutex> lock(g_pad_layout_mutex);
      dp::ApplyPadLayout(layout, buttons, lt, rt, DPInVehicle(), g_pad_layout_state[user_index]);
    }
    pad.buttons = buttons;
    pad.left_trigger = lt;
    pad.right_trigger = rt;
  });
  REXLOG_INFO("Controller layout filter installed (dp_pad_layout = {})", REXCVAR_GET(dp_pad_layout));
}
