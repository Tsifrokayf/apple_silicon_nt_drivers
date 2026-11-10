#!/usr/bin/env bash
# Build and run the host-side unit tests for the OS-independent protocol code.
# Needs only a host C compiler (CC, default cc). Does not need the WDK.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
out="${OUT_DIR:-$here/out/host}"
cc="${CC:-cc}"

mkdir -p "$out"

"$cc" -std=c99 -O1 -g -Wall -Wextra -Werror -Wconversion -Wshadow \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o "$out/test_dchid_proto" \
    "$root/AppleDockChannelHid/test/test_dchid_proto.c" \
    "$root/AppleDockChannelHid/src/dchid_proto.c"

"$out/test_dchid_proto"
