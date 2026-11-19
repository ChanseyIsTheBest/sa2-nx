// tw2de_nx (2026-11-01): the PowerPC instruction tests on the Switch.
//
// Xenia's instruction tests (src/xenia/cpu/ppc/testing: inputs, one instruction or a few, the results the Xbox 360
// gives), turned into code by this SDK's code generator (rexglue recompile-tests, with the project's code generator
// options) and built with devkitA64 and the Switch build's flags: what the game's code is, instruction by
// instruction, on the console. tools/ppc_tests/build_ppc_tests.py builds it.
//
// romfs:/tests.bin: per test the function's index, then its operations (set a register / memory, check one);
// romfs:/names.txt: the names; romfs:/known.txt: "name<TAB>why" for the cases whose expectation is not the
// hardware's (see build_ppc_tests.py). The results: on screen, and every failure in
// renut_ppc_tests.txt next to the NRO (renut-ultimate, port 147: the folder the homebrew menu started it from).
#include <switch.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

#include "ppc_config.h"
#include <rex/ppc.h>

PPCRegister* R(PPCContext& c, int n);
PPCRegister* F(PPCContext& c, int n);
PPCVRegister* V(PPCContext& c, int n);
uint32_t CR(PPCContext& c);
using Fn = void (*)(PPCContext&, uint8_t*);
extern Fn g_fns[];
extern int g_nfns;

namespace {
struct Op {
  uint8_t kind, reg, lane, pad;
  uint32_t pad2;
  uint64_t v;
};

// The guest memory the tests use: 0x10000000 .. 0x10000000 + kWindow (their MEMORY_IN / MEMORY_OUT addresses and
// the registers pointing there). base = window - 0x10000000, as the generated code adds the guest address to it.
constexpr uint32_t kWindowStart = 0x10000000u;
constexpr size_t kWindow = 0x100000;

bool ReadFile(const char* path, std::vector<uint8_t>& out) {
  FILE* f = fopen(path, "rb");
  if (!f)
    return false;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize(size_t(n));
  const bool ok = fread(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}

std::vector<std::string> ReadLines(const char* path) {
  std::vector<std::string> lines;
  FILE* f = fopen(path, "r");
  if (!f)
    return lines;
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    lines.push_back(line);
  }
  fclose(f);
  return lines;
}

void ResetHostFp() {
  uint64_t z = 0;
  __asm__ volatile("msr fpcr, %0" ::"r"(z));
  __asm__ volatile("msr fpsr, %0" ::"r"(z));
}

void WaitForPlus(PadState& pad) {
  printf("\nPress + to exit.\n");
  consoleUpdate(nullptr);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
      break;
    svcSleepThread(16'000'000);
  }
}
}  // namespace

int main(int argc, char** argv) {
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  printf("renut-ultimate: PowerPC instruction tests (Xenia's, recorded on the Xbox 360)\n\n");
  consoleUpdate(nullptr);

  if (R_FAILED(romfsInit())) {
    printf("romfs: not found\n");
    WaitForPlus(pad);
    consoleExit(nullptr);
    return 1;
  }
  std::vector<uint8_t> data;
  const std::vector<std::string> names = ReadLines("romfs:/names.txt");
  std::unordered_map<std::string, std::string> known;
  for (const std::string& l : ReadLines("romfs:/known.txt")) {
    const size_t tab = l.find('\t');
    if (tab != std::string::npos)
      known[l.substr(0, tab)] = l.substr(tab + 1);
  }
  if (!ReadFile("romfs:/tests.bin", data) || names.empty()) {
    printf("romfs:/tests.bin or names.txt missing\n");
    WaitForPlus(pad);
    consoleExit(nullptr);
    return 1;
  }

  uint8_t* window = static_cast<uint8_t*>(aligned_alloc(0x10000, kWindow));
  if (!window) {
    printf("no memory for the guest window\n");
    WaitForPlus(pad);
    consoleExit(nullptr);
    return 1;
  }
  memset(window, 0, kWindow);
  uint8_t* base = window - kWindowStart;
  auto in_window = [](uint64_t a) { return a >= kWindowStart && a < kWindowStart + kWindow; };

  // renut-ultimate (port 147): next to the NRO (argv[0], the path the homebrew menu gives), else sdmc:/switch/.
  std::string log_path = "sdmc:/switch/renut_ppc_tests.txt";
  if (argc > 0 && argv[0] && std::strncmp(argv[0], "sdmc:/", 6) == 0) {
    std::string self = argv[0];
    const size_t slash = self.find_last_of('/');
    if (slash != std::string::npos)
      log_path = self.substr(0, slash + 1) + "renut_ppc_tests.txt";
  }
  FILE* log = fopen(log_path.c_str(), "w");
  if (log)
    fprintf(log, "renut-ultimate PowerPC instruction tests: failures (FAIL name: register = got, want expected)\n\n");

  size_t pos = 0;
  int idx = 0, run = 0, failed = 0, known_failed = 0, shown = 0;
  while (pos + 8 <= data.size()) {
    uint32_t fi, nops;
    memcpy(&fi, &data[pos], 4);
    memcpy(&nops, &data[pos + 4], 4);
    pos += 8;
    const Op* ops = reinterpret_cast<const Op*>(&data[pos]);
    pos += size_t(nops) * sizeof(Op);
    const std::string& name = idx < int(names.size()) ? names[idx] : std::string("?");
    ++idx;
    if (fi >= uint32_t(g_nfns))
      continue;

    ResetHostFp();
    ppc_global_lock_count_().store(0);
    PPCContext ctx{};
    ctx.fpscr.loadFromHost();
    bool skip = false;
    for (uint32_t i = 0; i < nops; i++) {
      const Op& o = ops[i];
      switch (o.kind) {
        case 1: R(ctx, o.reg)->u64 = o.v; break;
        case 2: case 3: F(ctx, o.reg)->u64 = o.v; break;
        case 4: V(ctx, o.reg)->u32[o.lane] = uint32_t(o.v); break;
        case 5:
          if (in_window(o.v >> 8)) base[o.v >> 8] = uint8_t(o.v);
          else skip = true;
          break;
        case 6: ctx.xer.so = (o.v >> 31) & 1; ctx.xer.ov = (o.v >> 30) & 1; ctx.xer.ca = (o.v >> 29) & 1; break;
        case 7:
          ctx.cr0.set_raw((o.v >> 28) & 0xF); ctx.cr1.set_raw((o.v >> 24) & 0xF);
          ctx.cr2.set_raw((o.v >> 20) & 0xF); ctx.cr3.set_raw((o.v >> 16) & 0xF);
          ctx.cr4.set_raw((o.v >> 12) & 0xF); ctx.cr5.set_raw((o.v >> 8) & 0xF);
          ctx.cr6.set_raw((o.v >> 4) & 0xF); ctx.cr7.set_raw(o.v & 0xF);
          break;
      }
    }
    ++run;
    bool bad = false;
    std::string why;
    if (skip) {
      bad = true;
      why = "memory outside the test window";
    } else {
      try {
        g_fns[fi](ctx, base);
      } catch (const std::exception& e) {
        bad = true;
        why = e.what();
      } catch (...) {
        bad = true;
        why = "exception";
      }
    }
    char line[256];
    if (!why.empty() && log)
      fprintf(log, "FAIL %s: %s\n", name.c_str(), why.c_str());
    for (uint32_t i = 0; i < nops && why.empty(); i++) {
      const Op& o = ops[i];
      uint64_t got = 0, want = o.v;
      char what[32] = {};
      switch (o.kind) {
        case 11: got = R(ctx, o.reg)->u64; snprintf(what, sizeof what, "r%d", o.reg); break;
        case 12: case 13: got = F(ctx, o.reg)->u64; snprintf(what, sizeof what, "f%d", o.reg); break;
        case 14: got = V(ctx, o.reg)->u32[o.lane]; snprintf(what, sizeof what, "v%d[%d]", o.reg, o.lane); break;
        case 15:
          got = in_window(o.v >> 8) ? base[o.v >> 8] : 0;
          want = o.v & 0xFF;
          snprintf(what, sizeof what, "mem[%llX]", (unsigned long long)(o.v >> 8));
          break;
        case 16: got = CR(ctx); snprintf(what, sizeof what, "cr"); break;
        case 17:
          got = (uint32_t(ctx.xer.so) << 31) | (uint32_t(ctx.xer.ov) << 30) | (uint32_t(ctx.xer.ca) << 29);
          snprintf(what, sizeof what, "xer");
          break;
        default: continue;
      }
      if (got != want) {
        bad = true;
        snprintf(line, sizeof line, "FAIL %s: %s = %llX, want %llX", name.c_str(), what, (unsigned long long)got,
                 (unsigned long long)want);
        if (log)
          fprintf(log, "%s\n", line);
      }
    }
    for (uint32_t i = 0; i < nops; i++)
      if ((ops[i].kind == 5 || ops[i].kind == 15) && in_window(ops[i].v >> 8))
        base[ops[i].v >> 8] = 0;
    if (bad) {
      const auto k = known.find(name);
      if (k != known.end()) {
        ++known_failed;
        if (log)
          fprintf(log, "  (known: %s)\n", k->second.c_str());
      } else {
        ++failed;
        if (shown++ < 12)
          printf("FAIL %s%s%s\n", name.c_str(), why.empty() ? "" : ": ", why.c_str());
      }
    }
    if ((run & 0x3FFF) == 0) {
      printf("  %d tests...\n", run);
      consoleUpdate(nullptr);
    }
  }
  ResetHostFp();
  printf("\n%d tests run: %d passed, %d failed, %d known (their expectation is not the hardware's)\n", run,
         run - failed - known_failed, failed, known_failed);
  if (log) {
    fprintf(log, "\n%d tests run: %d passed, %d failed, %d known\n", run, run - failed - known_failed, failed,
            known_failed);
    fclose(log);
    printf("Failures: %s\n", log_path.c_str());
  }
  free(window);
  romfsExit();
  WaitForPlus(pad);
  consoleExit(nullptr);
  return failed ? 1 : 0;
}
