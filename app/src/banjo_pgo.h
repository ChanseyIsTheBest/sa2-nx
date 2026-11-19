// banjo-nx: profile collection of the PGO-instrumented Switch build (BANJO_PGO=generate), see banjo_pgo.cpp.
// From sa2-nx (app/src/sa2_pgo.h, GPL-3.0, same project family). In every other build these do nothing.

#pragma once

namespace banjo::pgo {

#if !defined(__SWITCH__)
// banjo-nx (2026-10-29): banjo_pgo.cpp is a Switch source (app/CMakeLists.txt); the PC build has nothing to collect.
inline void Start() {}
inline void DumpNow() {}
inline void Stop() {}
#else
// Picks this session's file (pgo/banjo_pgo_<N>.stream next to the executable) and starts the 2-minute dumps.
void Start();
// Writes every count so far to this session's file (the whole session each time, replacing the previous dump).
void DumpNow();
// The last dump, then the dump thread ends.
void Stop();
#endif

}  // namespace banjo::pgo
