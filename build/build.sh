#!/usr/bin/env bash
# Cross-compile AppleDockChannelHid.sys (Windows ARM64, KMDF) on Linux with
# clang-cl + lld-link against the WDK/SDK NuGet packages. See build/README.md.
#
# Required:
#   WDK_ROOT   directory holding the extracted packages:
#                $WDK_ROOT/wdk  <- Microsoft.Windows.WDK.ARM64
#                $WDK_ROOT/sdk  <- Microsoft.Windows.SDK.CPP
# Optional:
#   WDK_VER    kit version directory name (default 10.0.28000.0)
#   KMDF_VER   KMDF header/library version (default 1.33)
#   LLVM_BIN   directory with clang-cl and lld-link (default: PATH, then /usr/lib/llvm-18/bin)
#   OUT_DIR    output directory (default build/out/arm64)
#   SIGN_CERT, SIGN_KEY   PEM certificate and key; if both are set the driver
#              is also test-signed with osslsigncode (needs TESTSIGNING on).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
src="$root/AppleDockChannelHid/src"
out="${OUT_DIR:-$here/out/arm64}"

: "${WDK_ROOT:?set WDK_ROOT to the directory with the extracted wdk/ and sdk/ packages (see build/README.md)}"
wdk_ver="${WDK_VER:-10.0.28000.0}"
kmdf_ver="${KMDF_VER:-1.33}"
wdk="$WDK_ROOT/wdk/c"
sdk="$WDK_ROOT/sdk/c"

find_tool() {
    if [ -n "${LLVM_BIN:-}" ]; then echo "$LLVM_BIN/$1"; return; fi
    if command -v "$1" >/dev/null 2>&1; then command -v "$1"; return; fi
    if [ -x "/usr/lib/llvm-18/bin/$1" ]; then echo "/usr/lib/llvm-18/bin/$1"; return; fi
    echo "error: $1 not found (set LLVM_BIN)" >&2
    exit 1
}
clang_cl="$(find_tool clang-cl)"
lld_link="$(find_tool lld-link)"

inc_km="$wdk/Include/$wdk_ver/km"
inc_dirs=("$inc_km/crt" "$inc_km" "$wdk/Include/$wdk_ver/shared"
          "$sdk/Include/$wdk_ver/shared" "$wdk/Include/wdf/kmdf/$kmdf_ver")
lib_km="$wdk/Lib/$wdk_ver/km/ARM64"
lib_kmdf="$wdk/Lib/wdf/kmdf/ARM64/$kmdf_ver"

for d in "${inc_dirs[@]}" "$lib_km" "$lib_kmdf"; do
    [ -d "$d" ] || { echo "error: missing $d" >&2; exit 1; }
done
# specstrings.h/sal.h only exist in the SDK package; vhf.h is in sdk shared too.
[ -f "$sdk/Include/$wdk_ver/shared/vhf.h" ] || { echo "error: vhf.h not found in SDK" >&2; exit 1; }

python3 -I "$here/casefix.py" "${inc_dirs[@]}"

mkdir -p "$out"

defines=(-D_ARM64_ -D_ARM64 -DARM64 -D_WIN64 -D_KERNEL_MODE -DWINNT=1
         -DNTDDI_VERSION=0x0A00000C -D_WIN32_WINNT=0x0A00
         -DKMDF_VERSION_MAJOR="${kmdf_ver%%.*}" -DKMDF_VERSION_MINOR="${kmdf_ver##*.}"
         -DPOOL_NX_OPTIN=1)
imsvc=()
for d in "${inc_dirs[@]}"; do imsvc+=(-imsvc "$d"); done

# /W4 for our code. Headers come in through -imsvc (system headers), so their
# warnings are not reported. Warnings are errors.
cflags=(--target=aarch64-pc-windows-msvc /c /kernel /GS /Gy /Zi /W4 /WX /O2
        -Wno-unused-command-line-argument)

objs=()
for f in driver fifo dchid platform dchid_proto; do
    "$clang_cl" "${cflags[@]}" "${defines[@]}" "${imsvc[@]}" \
        "/Fo$out/$f.obj" "$src/$f.c"
    objs+=("$out/$f.obj")
done

# FxDriverEntry (wdfdriverentry.lib) calls __security_init_cookie, so /GS is
# safe with this entry point; bufferoverflowfastfailk.lib provides the cookie.
# vhfkm.lib (built with MSVC) calls _Interlocked* and __security_push/pop_cookie
# as out-of-line functions; libcntpr.lib, the kit's kernel CRT, defines them.
"$lld_link" /nologo /machine:arm64 /driver /kernel /subsystem:native,10.00 \
    /entry:FxDriverEntry /nodefaultlib /incremental:no /release /opt:ref /opt:icf \
    /merge:_TEXT=.text /merge:_PAGE=PAGE /section:INIT,d /tsaware:no \
    /debug "/pdb:$out/AppleDockChannelHid.pdb" \
    "/libpath:$lib_km" "/libpath:$lib_kmdf" \
    wdfdriverentry.lib wdfldr.lib vhfkm.lib ntoskrnl.lib hal.lib \
    bufferoverflowfastfailk.lib ntstrsafe.lib libcntpr.lib \
    "${objs[@]}" "/out:$out/AppleDockChannelHid.sys"

echo "built $out/AppleDockChannelHid.sys"

if [ -n "${SIGN_CERT:-}" ] && [ -n "${SIGN_KEY:-}" ]; then
    command -v osslsigncode >/dev/null || { echo "error: osslsigncode not installed" >&2; exit 1; }
    osslsigncode sign -certs "$SIGN_CERT" -key "$SIGN_KEY" -h sha256 -ph \
        -in "$out/AppleDockChannelHid.sys" -out "$out/AppleDockChannelHid.signed.sys"
    osslsigncode verify -in "$out/AppleDockChannelHid.signed.sys" -CAfile "$SIGN_CERT"
    mv -f "$out/AppleDockChannelHid.signed.sys" "$out/AppleDockChannelHid.sys"
    echo "test-signed $out/AppleDockChannelHid.sys"
fi
