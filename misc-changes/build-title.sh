#!/usr/bin/env bash
# Savage Wind PS5 - compiles, links, signs and stages a title folder.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Adapted from PS5_vkQuake's tools/build.sh (ps5-native-app-boilerplate, BlackBearReloaded, GPL-3.0-or-later):
# same loader constants, crt, link script, module writer and signed clean-room libc.prx (ps5/tooling).
#
#   ps5/tools/build-title.sh <name> <source.c|.cpp>...    -> ps5/dist/<name>/<TITLE_ID>/
#
# Extra compile flags: PS5_CFLAGS. Extra link inputs (archives, objects): PS5_LINK_INPUTS.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
[[ $# -ge 2 ]] || { echo "usage: ${0##*/} <name> <source>..." >&2; exit 2; }
name=$1; shift
[[ $name =~ ^[a-z0-9_-]+$ ]] || { echo "invalid name: $name" >&2; exit 2; }

sdk=$(bash "$root/ps5/tools/setup-deps.sh")
export PS5_PAYLOAD_SDK="$sdk"
tooling="$root/ps5/tooling"
native="$tooling/native"
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$tooling/prospero-clang18" "$@"; }

param="$root/ps5/sce_sys/param.json"
title_id=$(python3 - "$param" <<'PY'
import json, re, sys
value = json.load(open(sys.argv[1], encoding="utf-8"))
title_id = value["titleId"]
if not re.fullmatch(r"PPSA\d{5}", title_id) or title_id not in value["contentId"]:
    raise SystemExit("param.json: titleId/contentId invalid")
a3 = value["attribute3"]
# Retail VRR + high frame rate (0x40040); 0x80000 turns the system's VRR off (docs/PS5-PORT-PLAN.md D4)
if a3 & 0x40040 != 0x40040 or a3 & 0x80000:
    raise SystemExit(f"param.json: attribute3 {a3:#x} must set 0x40040 and clear 0x80000")
print(title_id)
PY
)

build="$root/build/ps5/$name"
app="$root/ps5/dist/$name/$title_id"
mkdir -p "$build/obj" "$build/host" "$build/stubs"

# Host tool: ELF -> SCE module writer and SELF signer
tool="$build/host/ps5-native-tool"
cxx=$(command -v clang++-18 || command -v clang++)
"$cxx" -std=c++20 -O2 -Wall -Wextra -Werror \
    "$native/native_app_builder.cpp" "$native/self_container.cpp" \
    "$native/elf_object.cpp" "$native/sce_module_writer.cpp" -lz -o "$tool"

# The signed clean-room libc.prx: rebuilt here and checked against the recorded digest
libc="$root/ps5/runtime/libc.prx"
if [[ ! -f $libc ]]; then
    "$cxx" -std=c++20 -O2 -Wall -Wextra -Werror "$native/libc_builder.cpp" -o "$build/host/libc-builder"
    "$build/host/libc-builder" "$native/runtime/api-surface.txt" "$native/runtime/imports.txt" "$build/libc.raw.elf"
    "$tool" self --sign --in "$build/libc.raw.elf" --out "$libc"
fi
(cd "$root/ps5/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)

objects=()
for source in "$@"; do
    object="$build/obj/${source//\//_}.o"  # (the path: runtime/ps5/input.c and runtime/hle/input.c are both linked)
    # (.mm: the shared UI sources, plain C++ apart from __APPLE__ blocks, as the Makefile builds them on Linux)
    case $source in *.c) std=(-std=gnu11) ;; *.S) std=() ;; *.mm) std=(-x c++ -std=c++20) ;; *) std=(-std=c++20) ;; esac
    extra=()
    case $source in
        third_party/FidelityFX/sdk/src/*.cpp|runtime/nvn/vulkan/fsr3.cpp|runtime/nvn/vulkan/ffx_shaders.cpp)
            extra=(-include "$root/third_party/FidelityFX/ffx_compat.h") ;;
        runtime/nvn/hd_ui_ncnn.cpp)
            extra=(-I"$PS5_NCNN_SRC/src" -I"$PS5_NCNN_SRC/../build/src") ;;
        runtime/gamecode_compile.cpp)
            extra=(-fno-rtti) ;;
    esac
    # shellcheck disable=SC2086
    cc "${std[@]}" "${extra[@]}" -O2 -g1 -Wall -Wextra -fno-omit-frame-pointer -ffunction-sections -fdata-sections \
        ${PS5_CFLAGS:-} -c "$root/$source" -o "$object"
    objects+=("$object")
done
printf '%s\n' "${objects[@]##*/}" > "$build/runtime-objects.list"
for crt in app_crt app_cpp_runtime; do
    exception_flag=-fno-exceptions
    [[ $crt != app_cpp_runtime ]] || exception_flag=-fexceptions
    cc -std=c++20 -O2 -Wall -Wextra "$exception_flag" -fno-rtti -ffunction-sections -fdata-sections \
        -c "$native/$crt.cpp" -o "$build/obj/$crt.o"
done

stub() {  # Host-link stubs record imports from PS5 system modules.
    local name=$1
    local source=$2
    local soname=${3:-"$1.prx"}
    cc -std=c11 -O2 -fPIC -c "$tooling/ps5-stubs/$source" -o "$build/obj/$name.o"
    "$sdk/bin/prospero-lld" --shared -soname "$soname" -o "$build/stubs/$name.so" "$build/obj/$name.o"
    printf '%s\n' "$build/stubs/$name.so"
}
agc=$(stub libSceAgc agc_link_stub.c)
agc_driver=$(stub libSceAgcDriver agc_driver_link_stub.c)
common_dialog=$(stub libSceCommonDialog common_dialog_link_stub.c libSceCommonDialog.sprx)

# shellcheck source=ps5/tools/link-recipe.sh
source "$root/ps5/tools/link-recipe.sh"
link_recipe "$sdk" "$build" "${PS5_RADV_ARCHIVE:-}" || exit 2
if [[ ${PS5_GAMECODE_EXPORTS:-0} == 1 ]]; then
    export_args=()
    if [[ -n ${PS5_GAMECODE_EXPORT_MANIFEST:-} ]]; then
        export_args+=(--manifest "$PS5_GAMECODE_EXPORT_MANIFEST")
    else
        for archive in "$root"/build/ps5/gamecode*/libgamecode.a; do
            [[ -f $archive ]] && export_args+=(--archive "$archive")
        done
    fi
    for object in "${objects[@]}"; do
        export_args+=(--object "$object")
    done
    for library in "${link_inputs[@]}" "${link_stubs[@]}"; do
        [[ -f $library ]] && export_args+=(--library "$library")
    done
    python3 "$root/ps5/tools/gen-gamecode-exports.py" --nm "$sdk/bin/llvm-nm" \
        --output "$root/build/ps5/gamecode_exports.c" \
        --abi "ps5-x86_64-v3-clang18-emulated-tls:$PS5_CFLAGS" "${export_args[@]}"
    export_object="$build/obj/gamecode_exports.o"
    cc -std=gnu11 -O2 -Iruntime -c "$root/build/ps5/gamecode_exports.c" -o "$export_object"
    objects+=("$export_object")
    link_flags+=(--undefined=game_exports --undefined=game_export_count --undefined=game_export_abi_digest)
fi
# shellcheck disable=SC2086
"$sdk/bin/prospero-lld" -T "$native/ps5-pie-unwind.ld" -L "$native" --eh-frame-hdr --error-limit=0 \
    --Map="$build/title.map" --version-script "$native/app-symbols.map" --exclude-libs=ALL \
    "${link_flags[@]}" -e _start -o "$build/llvm-pie.elf" \
    "$build/obj/app_crt.o" "$build/obj/app_cpp_runtime.o" "${objects[@]}" ${PS5_LINK_INPUTS:-} \
    "${link_inputs[@]}" "$agc" "$agc_driver" "$common_dialog" --as-needed "${link_stubs[@]}"
python3 "$root/ps5/tools/check-title-imports.py" "$build/llvm-pie.elf" \
    "$native/runtime/api-surface.txt" "$agc" "$agc_driver" "$common_dialog" "${link_stubs[@]}"
"$tool" link --in "$build/llvm-pie.elf" --out "$build/eboot.elf" \
    --stub-dir "$sdk/target/lib" --stub "$agc" --stub "$agc_driver" --stub "$common_dialog" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf

rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$build/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$param" "$root/ps5/sce_sys/icon0.png" "$app/sce_sys/"
for asset in pic0.dds pic1.dds background-source.png; do
    if [[ -f "$root/ps5/sce_sys/$asset" ]]; then
        cp "$root/ps5/sce_sys/$asset" "$app/sce_sys/"
    fi
done
cp "$libc" "$app/sce_module/libc.prx"
"$tool" self --inspect --file "$app/eboot.bin" >/dev/null
printf '%s\n' "$app"
