#!/bin/bash
# Builds the native renderer's shader library (banjo_shaders.nfsp) from dumped shader containers.
#
#   tools/build_shader_library.sh <dump folder> [<dump folder> ...]
#
# Dump folders are the "shaders" folders the game writes next to the exe with
#   dump_guest_shaders = true             (constructor shaders: vs_*.bin / ps_*.bin, reNut's dumper)
#   banjo_volcar_shaders_en_sitio = true  (in-place shaders the engine builds from its bundles: v_*.bin / p_*.bin)
# Output: out/shaders/banjo_shaders.nfsp (+ hlsl/, spirv/ and logs). Copy it next to renut.exe / renut.nro.
#
# Steps (after nfsmw-nx's shaders/nfsmw_regenerar_biblioteca_pcf.sh, minus its NFSMW-only HLSL rewrites):
#   1. build the translator (XenosRecomp with NFSMW_RECOMP) and the packer with MSYS2 clang;
#   2. gather every .bin into one folder as p_<name>.bin / v_<name>.bin;
#   3. XenosRecomp: container -> HLSL; 4. DXC: HLSL -> SPIR-V (Vulkan 1.2); 5. pack and verify.
set -u
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
SHADERS="$RAIZ/shaders"
SDK="$RAIZ/sdk"
OUT="$RAIZ/out/shaders"
CXX=${CXX:-/c/devkitPro/msys2/clang64/bin/clang++.exe}
DXC=${DXC:-/c/devkitPro/UnleashedRecomp-NX/tools/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe}
[ $# -ge 1 ] || { echo "usage: $0 <dump folder> [...]"; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/in" "$OUT/hlsl" "$OUT/spirv" "$OUT/bin"

echo "== building tools"
COMMON=(-std=c++23 -O1 -static "-I$SHADERS" "-I$RAIZ/app/src/native" "-I$SHADERS/XenosRecomp" "-I$SDK/thirdparty/fmt/include"
        "-I$SDK/thirdparty/xxHash" -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DNDEBUG -w)
"$CXX" "${COMMON[@]}" -include "$SHADERS/pch_min.h" -DNFSMW_RECOMP \
  "$SHADERS/nfsmw_hlsl.cpp" "$SHADERS/XenosRecomp/shader_recompiler.cpp" -o "$OUT/bin/hlsl.exe" \
  > "$OUT/build_tools.log" 2>&1 || { echo "translator build failed, see $OUT/build_tools.log"; exit 1; }
"$CXX" "${COMMON[@]}" "-I$RAIZ/app/src/native" \
  "$SHADERS/nfsmw_empaquetar.cpp" "$RAIZ/app/src/native/nfsmw_shader_library.cpp" -o "$OUT/bin/pack.exe" \
  >> "$OUT/build_tools.log" 2>&1 || { echo "packer build failed, see $OUT/build_tools.log"; exit 1; }

echo "== gathering containers"
for d in "$@"; do
  for f in "$d"/*.bin; do
    [ -e "$f" ] || continue
    b=$(basename "$f" .bin)
    case "$b" in
      vs_*) cp "$f" "$OUT/in/v_c${b#vs_}.bin" ;;
      ps_*) cp "$f" "$OUT/in/p_c${b#ps_}.bin" ;;
      v_*|p_*) cp "$f" "$OUT/in/$b.bin" ;;
    esac
  done
done
echo "   $(ls "$OUT/in" | wc -l) containers"

echo "== XenosRecomp"
"$OUT/bin/hlsl.exe" "$OUT/in" "$OUT/hlsl/out" "$SHADERS/XenosRecomp/shader_common.h" > "$OUT/translate.log" 2>&1
echo "   exit $?; $(ls "$OUT/hlsl/out" 2>/dev/null | wc -l) HLSL files (see translate.log)"

echo "== DXC"
: > "$OUT/dxc.log"
ok=0; fail=0
mkdir -p "$OUT/in_ok"
for f in "$OUT"/hlsl/out/*.hlsl; do
  [ -e "$f" ] || continue
  base=$(basename "$f" .hlsl)
  if [[ "$base" == p_* ]]; then tipo=ps_6_6; extra=(); else tipo=vs_6_6; extra=(-fvk-invert-y); fi
  if "$DXC" -spirv -T "$tipo" -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout \
       "${extra[@]}" -Fo "$OUT/spirv/$base.spv" "$f" >> "$OUT/dxc.log" 2>&1; then
    ok=$((ok + 1)); cp "$OUT/in/$base.bin" "$OUT/in_ok/"
  else
    fail=$((fail + 1)); echo "DXC failed: $base" >> "$OUT/dxc_failed.txt"
  fi
done
echo "   $ok compiled, $fail failed"

echo "== pack"
"$OUT/bin/pack.exe" "$OUT/in_ok" "$OUT/spirv" "$OUT/banjo_shaders.nfsp" 2>&1 | tee "$OUT/pack.log"
ls -la "$OUT/banjo_shaders.nfsp" 2>/dev/null
