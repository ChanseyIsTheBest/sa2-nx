// sasrt-nx (2026-11-08, sasrt_gpu_depth_resolve_float_ps; the second GPU round): the single-sampled depth resolve's
// float alone, for the resolves whose words nothing reads (the depth-only pipeline, words target write-masked): the same
// load and the same float as depth_resolve_ps, without the stencil load, the word's encode and its output.
#define DEPTH_RESOLVE_MS 0
#define DEPTH_RESOLVE_FLOAT_ONLY 1
#include "depth_resolve.hlsli"
