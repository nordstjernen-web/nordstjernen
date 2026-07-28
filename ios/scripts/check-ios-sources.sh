#!/usr/bin/env bash
#
# Verify that every source the iOS engine library would compile is valid
# against the iOS SDK — the engine's own C only, WITHOUT a cross-compiled
# dependency sysroot or a UIKit host app (neither exists yet).
#
# iOS, like Android, runs the GTK-free embeddable engine (libnordstjernen):
# no GTK 4, no gdk-pixbuf, and the WebGL GL-context backends
# (CGL/EGL/WGL) are dropped. This reuses the desktop build's
# compile_commands.json for the engine's include/define flags, then re-checks
# each engine translation unit with `clang -fsyntax-only` retargeted at the
# iOS SDK (device or simulator) through xcrun.
#
# It catches iOS-source-portability regressions on a macOS runner with Xcode.
# It does NOT link, cross-compile the dependency stack, or build an .ipa —
# that needs the iOS dependency sysroot and the (future) UIKit shell.
#
# Usage: ios/scripts/check-ios-sources.sh [device|simulator] [builddir]

set -euo pipefail

SDK_KIND="${1:-device}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILDDIR="${2:-${REPO_ROOT}/builddir}"
CCJSON="${BUILDDIR}/compile_commands.json"

case "$SDK_KIND" in
  device)    IOS_SDK=iphoneos;        IOS_TARGET=arm64-apple-ios15.0 ;;
  simulator) IOS_SDK=iphonesimulator; IOS_TARGET=arm64-apple-ios15.0-simulator ;;
  *) echo "usage: $0 [device|simulator] [builddir]" >&2; exit 2 ;;
esac

if [ ! -f "$CCJSON" ]; then
  echo "no $CCJSON; run 'meson setup $BUILDDIR' first" >&2
  exit 2
fi

if ! command -v xcrun >/dev/null 2>&1; then
  echo "xcrun not found — this check needs the Xcode/iOS SDK (macOS runner)" >&2
  exit 2
fi

IOS_SYSROOT="$(xcrun --sdk "$IOS_SDK" --show-sdk-path)"
CLANG="$(xcrun --sdk "$IOS_SDK" --find clang)"
echo "iOS SDK ($SDK_KIND): $IOS_SYSROOT"
echo "target:              $IOS_TARGET"
echo "clang:               $CLANG"
echo

python3 - "$CCJSON" "$BUILDDIR" "$CLANG" "$IOS_SYSROOT" "$IOS_TARGET" <<'PY'
import json, os, shlex, subprocess, sys

ccjson, builddir, clang, sysroot, target = sys.argv[1:6]
entries = json.load(open(ccjson))

# The embeddable engine library (libnordstjernen) compiles exactly the engine
# source set an iOS build would use, so check those translation units.
def is_engine(e):
    blob = e.get('output', '') + e.get('command', '')
    return 'libnordstjernen' in blob and e['file'].endswith('.c')

DROP_WITH_ARG = {'-o', '-MF', '-MQ', '-MT', '-isysroot', '-arch', '-install_name'}
DROP = {'-c', '-MD', '-MMD', '-MP'}
# Desktop-only capability macros dropped for the GTK-free iOS engine config:
# WebGL and its CGL/EGL GL-context backends, and the gdk-pixbuf image
# fallbacks (iOS, like Android, decodes with Wuffs and has no GTK toolkit).
DROP_DEFINE_PREFIX = ('-DNS_ENABLE_WEBGL', '-DNS_HAVE_CGL', '-DNS_HAVE_EGL',
                      '-DNS_HAVE_GDK_PIXBUF', '-DNS_HAVE_LIBRSVG')
COMPILERS = {'ccache', 'clang', 'clang++', 'cc', 'gcc', 'c++', 'g++'}

def strip_leading_compiler(args):
    i = 0
    while i < len(args):
        a = args[i]
        if a.startswith('-'):
            break
        if a in COMPILERS or os.path.basename(a) in COMPILERS:
            i += 1
            continue
        break
    return args[i:]

fails, checked, seen = [], 0, set()
for e in entries:
    if not is_engine(e) or e['file'] in seen:
        continue
    seen.add(e['file'])
    raw = shlex.split(e['command']) if e.get('command') else list(e['arguments'])
    raw = strip_leading_compiler(raw)
    out, skip = [], False
    for a in raw:
        if skip:
            skip = False; continue
        if a in DROP_WITH_ARG:
            skip = True; continue
        if a in DROP or a.endswith('.o') or a.endswith('.o.d'):
            continue
        if a.startswith(DROP_DEFINE_PREFIX):
            continue
        out.append(a)
    cmd = [clang, '-isysroot', sysroot, '-target', target,
           '-DNS_IOS=1', '-fsyntax-only'] + out
    r = subprocess.run(cmd, cwd=builddir, capture_output=True, text=True)
    checked += 1
    name = e['file'].split('/src/')[-1]
    if r.returncode != 0:
        fails.append(name)
        print(f"FAIL {name}")
        print('\n'.join(r.stderr.splitlines()[:30]))
    else:
        print(f"ok   {name}")

print(f"\nchecked {checked} engine sources against the iOS SDK ({target}); "
      f"{len(fails)} failed")
sys.exit(1 if fails else 0)
PY
