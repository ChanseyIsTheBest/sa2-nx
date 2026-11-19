#!/bin/bash
# banjo-nx: offline size of translated shaders (docs/shader.md, "Measuring"). The current translator, then
# tools/early_alpha.py and DXC as tools/build_shader_library.sh runs them, then the SPIR-V specialised the way the
# driver does it (specialization constants frozen, folded, optimised by spirv-opt inside DXC) and counted: body
# instructions, conditional branches, samples, loads, phis, selects. A proxy for NAK's count, which only the console
# gives (banjo_diag_shader_stats); package 19 had ~0.8 NAK instructions per counted SPIR-V instruction in the heavy
# pixel shaders and ~0.57 in their vertex shaders.
#
#   tools/shader_size.sh <tag> <constant 0> <constant 61> <container> [...]
#     containers by name without .bin, from out/shaders_prev/in (the library build's input)
#     constant 0: the pipeline's specialization bits (the console's heavy draws: 0x0028A320)
#     constant 61: fetch constants that may have texture signs (0 = none, 0xFFFFFFFF = every one)
#     environment PSIN: constant 60, the pixel shader's input locations for a vertex shader (default every one)
#     environment EXTRA_SPEC: more "id:value" pairs, e.g. bool constants "1243:0 1244:1" (banjo_gpu_bool_spec)
#   e.g. tools/shader_size.sh now 0x0028A320 0 ps_d93d4dbdf1bb78aa9 vs_d31c562b4e5fd5f9c
# Work files go to out/shader_size/<tag> (a few MB). Never run it while tools/build_shader_library.sh runs: both read
# out/shaders_prev/in, and the library build rebuilds out/shaders.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CXX=${CXX:-/c/devkitPro/msys2/clang64/bin/clang++.exe}
DXC=${DXC:-/c/devkitPro/UnleashedRecomp-NX/tools/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe}
PYTHON=${PYTHON:-python}
[ $# -ge 4 ] || { sed -n 2,17p "$0"; exit 1; }
TAG=$1; SPEC0=$2; SPEC61=$3; shift 3
BASE="$ROOT/out/shader_size"; W="$BASE/$TAG"
rm -rf "$W"; mkdir -p "$W/i" "$W/o"
cd "$ROOT" || exit 1
TRANSLATOR="$BASE/xenos_hlsl.exe"
if [ ! -f "$TRANSLATOR" ] || [ shaders/XenosRecomp/shader_recompiler.cpp -nt "$TRANSLATOR" ] ||
   [ shaders/tools/masseffect_hlsl.cpp -nt "$TRANSLATOR" ]; then
  "$CXX" -std=c++20 -O2 -static -w -fms-extensions -include shaders/XenosRecomp/pch_min.h -Ishaders -Ishaders/XenosRecomp \
    -Isdk/thirdparty/fmt/include -Isdk/thirdparty/xxHash -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DMASSEFFECT_RECOMP \
    shaders/tools/masseffect_hlsl.cpp shaders/XenosRecomp/shader_recompiler.cpp -o "$TRANSLATOR" || exit 1
fi
for n in "$@"; do  # one translator process per container (its output folder must be empty), as the library build
  mkdir -p "$W/i/$n"; cp "out/shaders_prev/in/$n.bin" "$W/i/$n/" || continue
  "$TRANSLATOR" "$W/i/$n" "$W/t/$n" shaders/XenosRecomp/shader_common.h > "$W/$n.translate.txt" 2>&1
  mv "$W/t/$n/$n.hlsl" "$W/o/" 2>/dev/null || { echo "$n: translation failed"; tail -2 "$W/$n.translate.txt"; }
done
"$PYTHON" tools/early_alpha.py "$W/o" | tail -1
for n in "$@"; do
  [ -f "$W/o/$n.hlsl" ] || continue
  case $n in ps_*) t=ps_6_6; x="";; *) t=vs_6_6; x=-fvk-invert-y;; esac
  "$DXC" -spirv -T $t -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout -Werror=parameter-usage $x \
    "-Oconfig=--set-spec-const-default-value=0:$(($SPEC0)) 61:$(($SPEC61)) 60:$((${PSIN:-0xFFFFFFFF})) ${EXTRA_SPEC:-},--freeze-spec-const,--fold-spec-const-op-composite,-O,--fold-spec-const-op-composite,-O" \
    -Fc "$W/$n.txt" -Fo "$W/$n.spv" "$W/o/$n.hlsl" > "$W/$n.err" 2>&1 || { echo "$n: DXC failed"; head -5 "$W/$n.err"; continue; }
  "$PYTHON" - "$W/$n.txt" "$n" <<'EOF'
import re, sys, collections
L = open(sys.argv[1]).read().split('\n')
ops = collections.Counter(m.group(1) for l in L for m in [re.match(r'\s*(?:%\S+ = )?(Op\w+)', l)] if m)
body = sum(v for k, v in ops.items() if k not in ('OpName', 'OpDecorate', 'OpMemberDecorate', 'OpMemberName', 'OpCapability',
           'OpExtension', 'OpExtInstImport', 'OpMemoryModel', 'OpEntryPoint', 'OpExecutionMode', 'OpSource', 'OpString')
           and not k.startswith('OpType') and not k.startswith('OpConstant') and k != 'OpVariable')
print(f"{sys.argv[2]}: {body} body instructions, {ops['OpBranchConditional']} conditional branches, "
      f"{ops['OpImageSampleImplicitLod'] + ops['OpImageSampleExplicitLod']} samples, {ops['OpLoad']} loads, "
      f"{ops['OpPhi']} phis, {ops['OpSelect']} selects")
EOF
done
