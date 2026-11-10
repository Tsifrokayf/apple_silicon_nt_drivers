# Building on Linux

The drivers in this repository are cross-compiled on Linux with LLVM's
`clang-cl` and `lld-link` against the Windows Driver Kit (WDK) and SDK headers
and libraries from Microsoft's NuGet packages. No Windows machine or Visual
Studio is involved. The resulting binaries have only been checked statically
(PE headers, imports, signature verification). None has been loaded on
Windows yet.

## Requirements

- clang-cl and lld-link (tested with LLVM 18: `apt-get install clang-18 lld-18`;
  the script also looks in `/usr/lib/llvm-18/bin`)
- python3 (for `casefix.py`), unzip, curl
- a host C compiler (`cc`) for `test.sh`
- optional: `osslsigncode` (tested with 2.8) and openssl for test signing

## Getting the WDK and SDK packages

Two NuGet packages are needed. The versions below are the ones the build was
done with:

| Package | Version | Extract to |
|---|---|---|
| `Microsoft.Windows.WDK.ARM64` | 10.0.28000.2526 | `$WDK_ROOT/wdk` |
| `Microsoft.Windows.SDK.CPP` | 10.0.28000.2957 | `$WDK_ROOT/sdk` |

The SDK package supplies `sal.h`, `specstrings.h`, `driverspecs.h` and `vhf.h`.
The kernel and KMDF libraries come from the WDK package. A `.nupkg` is a zip
file:

```sh
export WDK_ROOT=$HOME/wdkx
mkdir -p "$WDK_ROOT/wdk" "$WDK_ROOT/sdk"
curl -L -o /tmp/wdk.nupkg https://www.nuget.org/api/v2/package/Microsoft.Windows.WDK.ARM64/10.0.28000.2526
curl -L -o /tmp/sdk.nupkg https://www.nuget.org/api/v2/package/Microsoft.Windows.SDK.CPP/10.0.28000.2957
unzip -q /tmp/wdk.nupkg -d "$WDK_ROOT/wdk"
unzip -q /tmp/sdk.nupkg -d "$WDK_ROOT/sdk"
```

The URLs follow NuGet's standard v2 download endpoint. They were not
downloaded again while this script was written. If a version has been
delisted, pick the matching pair from nuget.org and set `WDK_VER` to the kit
directory name found under `wdk/c/Include/`. The packages are covered by
Microsoft's licence terms. Do not commit or redistribute them.

`build.sh` runs `casefix.py` on the include directories every time. The
Windows headers include each other with a different letter case than the
file names in the packages. The script adds symlinks with the spelling each
`#include` uses. It is idempotent and only adds links inside `$WDK_ROOT`.

## Scripts

| Script | Purpose |
|---|---|
| `build.sh` | builds `AppleDockChannelHid.sys` into `build/out/arm64/` (`OUT_DIR` overrides); test-signs it when `SIGN_CERT` and `SIGN_KEY` are set |
| `test.sh` | builds and runs the host unit tests for the OS-independent protocol code |
| `casefix.py` | case-variant symlinks for the WDK/SDK headers |

Environment: `WDK_ROOT` (required for `build.sh`), `WDK_VER` (default
`10.0.28000.0`), `KMDF_VER` (default `1.33`), `LLVM_BIN`, `OUT_DIR`, `CC`
(for `test.sh`).

## Test-signing certificate

Generate a throwaway certificate locally and never commit it (`.gitignore`
excludes keys and certificates):

```sh
openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 3650 \
    -keyout test.key -out test.crt -subj "/CN=NTASP Test Signing" \
    -addext extendedKeyUsage=codeSigning
SIGN_CERT=test.crt SIGN_KEY=test.key WDK_ROOT=... build/build.sh
```

Windows accepts any certificate only with `TESTSIGNING` on and Secure Boot
off.
