// banjo-nx: on/off switches for every performance enhancement.
//
// Each enhancement has its own cvar (toml key) and belongs to one of two groups; it is active only when its own key
// AND its group's master switch are on:
//   banjo_cpu_enhancements     host CPU work: guest-code replacements, waits, thread usage
//   banjo_noncpu_enhancements  GPU / driver / storage side: shader compile cost, caches, prewarming
// The startup log has one line listing every enhancement and whether it is active (LogEnhancements), so a console log
// says exactly what ran. Build-time changes (the shader library, compiler flags) are listed in docs/enhancements.md.

#pragma once

namespace banjo {

enum class EnhancementGroup { kCpu, kNonCpu };

// Own key AND the group's master switch.
bool EnhancementActive(EnhancementGroup group, bool own_key);

// With a master switch off, resets the keys of the enhancements whose code reads its key directly (the native
// renderer's ring fast paths). Before the graphics system starts (RenutApp::OnPreSetup).
void ApplyMasterSwitches();

// One "[enhancements]" log line (once, after the config is loaded).
void LogEnhancements();

// The PC side of the generated code's self-checks, the indirect call census and the runtime census
// (banjo_codegen_checks.cpp; nothing on the Switch). Init: once, from ApplyMasterSwitches. Report: with every 10 s
// report of the renderer.
void CodegenChecksInit();
void CodegenChecksReport(int seconds);

// Runs every helper that has a form for one processor only (the conversions to integer, the divisions, the float
// copies, the small VMX helpers) on fixed edge inputs against plain C++ references and logs the result
// (banjo_forms_selfcheck.cpp). Once, from LogEnhancements: on the console it is the first time the AArch64 forms run.
void FormsSelfCheck();
// blur-nx (2026-11-11): timings of a few processor forms at boot when blur_microbench is on (blur_microbench.cpp).
void BlurMicrobench();

}  // namespace banjo
