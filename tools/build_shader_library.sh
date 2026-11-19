#!/bin/bash
# Builds the native renderer's shader package (banjo_shaders.mesp + .idx) from dumped shader containers, with the
# masseffect-nx toolchain (shaders/: XenosRecomp with MASSEFFECT_RECOMP, masseffect_hlsl front end, me_pack_shaders).
#
#   tools/build_shader_library.sh <dump folder> [<dump folder> ...]
#
# Dump folders: the "shaders" folders the game writes next to the exe with dump_guest_shaders = true
# (constructor shaders vs_*/ps_*.bin) and banjo_volcar_shaders_en_sitio = true (in-place shaders v_*/p_*.bin).
# Output: out/shaders/banjo_shaders.mesp (+ .idx). Copy both next to renut.exe / the NRO.
# Windows version of shaders/tools/build_shader_spirv.sh: no symlinks, MSYS2 clang, DXC from the
# UnleashedRecomp-NX tree, one translator process per container so one crash does not stop the rest.
set -u
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
SHADERS="$RAIZ/shaders"; XENOS="$SHADERS/XenosRecomp"; SDK="$RAIZ/sdk"
OUT="$RAIZ/out/shaders"
CXX=${CXX:-/c/devkitPro/msys2/clang64/bin/clang++.exe}
DXC=${DXC:-/c/devkitPro/UnleashedRecomp-NX/tools/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe}
JOBS=${JOBS:-8}
[ $# -ge 1 ] || { echo "usage: $0 <dump folder> [...]"; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/in" "$OUT/hlsl" "$OUT/spirv" "$OUT/bin" "$OUT/tmp"

echo "== building tools"
"$CXX" -std=c++20 -O2 -static -w -fms-extensions -include "$XENOS/pch_min.h" -I"$SHADERS" -I"$XENOS" \
  -I"$SDK/thirdparty/fmt/include" -I"$SDK/thirdparty/xxHash" -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DMASSEFFECT_RECOMP \
  "$SHADERS/tools/masseffect_hlsl.cpp" "$XENOS/shader_recompiler.cpp" -o "$OUT/bin/xenos_hlsl.exe" \
  > "$OUT/build_tools.log" 2>&1 || { echo "translator build failed, see $OUT/build_tools.log"; exit 1; }
"$CXX" -std=c++20 -O2 -static -w -I"$SDK/thirdparty/xxHash" -I"$RAIZ/app/src/me/masseffect" \
  "$SHADERS/tools/me_pack_shaders.cpp" "$RAIZ/app/src/me/masseffect/masseffect_shader_library.cpp" \
  -o "$OUT/bin/me_pack_shaders.exe" >> "$OUT/build_tools.log" 2>&1 || { echo "packer build failed, see $OUT/build_tools.log"; exit 1; }

echo "== gathering containers (vs_/ps_ names)"
for d in "$@"; do
  for f in "$d"/*.bin; do
    [ -e "$f" ] || continue
    b=$(basename "$f" .bin)
    case "$b" in
      vs_*|ps_*) cp "$f" "$OUT/in/$b.bin" ;;
      v_*) cp "$f" "$OUT/in/vs_i${b#v_}.bin" ;;
      p_*) cp "$f" "$OUT/in/ps_i${b#p_}.bin" ;;
    esac
  done
done
echo "   $(ls "$OUT/in" | wc -l) containers"

echo "== XenosRecomp (one process per container)"
traducir() {
  local f=$1 b; b=$(basename "$f" .bin)
  local t="$OUT/tmp/$b"; mkdir -p "$t/i"; cp "$f" "$t/i/"
  if "$OUT/bin/xenos_hlsl.exe" "$t/i" "$t/o" "$XENOS/shader_common.h" > "$t/log.txt" 2>&1 && [ -f "$t/o/$b.hlsl" ]; then
    mv "$t/o/$b.hlsl" "$OUT/hlsl/"; rm -rf "$t"
  else
    echo "$b $(tail -1 "$t/log.txt")" >> "$OUT/translate_failed.txt"
  fi
}
export -f traducir; export OUT XENOS
: > "$OUT/translate_failed.txt"
ls "$OUT"/in/*.bin | xargs -P "$JOBS" -I{} bash -c 'traducir "$@"' _ {}
echo "   $(ls "$OUT/hlsl" | wc -l) translated, $(wc -l < "$OUT/translate_failed.txt") failed"

echo "== early alpha (tools/early_alpha.py: banjo_gpu_early_alpha)"
PYTHON=${PYTHON:-python}
"$PYTHON" "$RAIZ/tools/early_alpha.py" "$OUT/hlsl" || echo "   early_alpha.py failed: the shaders keep no early alpha"

echo "== DXC"
compilar() {
  local f=$1 b t x; b=$(basename "$f" .hlsl)
  case $b in ps_*) t=ps_6_6; x="";; *) t=vs_6_6; x=-fvk-invert-y;; esac
  "$DXC" -spirv -T $t -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout -Werror=parameter-usage $x \
    -Fo "$OUT/spirv/$b.spv" "$f" > /dev/null 2> "$OUT/spirv/$b.err" && rm -f "$OUT/spirv/$b.err" || rm -f "$OUT/spirv/$b.spv"
}
export -f compilar; export DXC
ls "$OUT"/hlsl/*.hlsl | xargs -P "$JOBS" -I{} bash -c 'compilar "$@"' _ {}
echo "   $(ls "$OUT"/spirv/*.spv 2>/dev/null | wc -l) compiled, $(ls "$OUT"/spirv/*.err 2>/dev/null | wc -l) failed"

echo "== pack (only containers that compiled)"
mkdir -p "$OUT/in_ok"
for s in "$OUT"/spirv/*.spv; do b=$(basename "$s" .spv); cp "$OUT/in/$b.bin" "$OUT/in_ok/"; done
"$OUT/bin/me_pack_shaders.exe" "$OUT/in_ok" "$OUT/spirv" "$OUT/banjo_shaders.mesp" 2>&1 | tee "$OUT/pack.log"
ls -la "$OUT"/banjo_shaders.mesp* 2>/dev/null
