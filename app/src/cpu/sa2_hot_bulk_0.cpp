// Sonic Adventure 2 - the bulk localized copies, shard 0 of 4 (sa2-nx 2026-10-28; bejeweled3-nx's hot shards).
//
// app/generated/hot/localized_bulk_hooks_0.inc is written by tools/localize_native.py --bulk from
// app/src/cpu/hot/localized_bulk.txt: the game thread's functions still in generated code, ranked by what they executed
// in the console session of the release's PGO profile. Each is a localized copy (the recompiled function's own
// operations, exact by construction) with its write-backs limited to the registers that may differ
// (tools/localize_writebacks.py), unguarded, and its own key, sa2_cpu_native_sub_<ADDR> (SA2_COPIES_DEFAULT).
#include "sa2_hot_prelude.h"

#if __has_include("../../generated/hot/localized_bulk_hooks_0.inc")
#include "../../generated/hot/localized_bulk_hooks_0.inc"
#endif
