#!/usr/bin/env bash
#
# Verify that every source the Android engine library compiles is valid under
# the Android configuration — WITHOUT needing the NDK or a cross sysroot.
#
# It reuses the desktop build's compile_commands.json, then re-checks each
# engine translation unit with clang -fsyntax-only after switching it to the
# Android preprocessor configuration: define __ANDROID__ and drop the
# desktop-only ND_HAVE_GDK_PIXBUF / ND_HAVE_LIBRSVG capability macros (Android
# links neither gdk-pixbuf nor librsvg; GdkTexture is replaced by nd_texture).
#
# This catches Android-source regressions on an ordinary Linux build. It does
# NOT exercise the NDK toolchain or the cross-compiled dependency sysroot —
# that is android/scripts/build-deps.sh and runs in CI / on an NDK box.
#
# Usage: android/scripts/check-android-sources.sh [builddir]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILDDIR="${1:-${REPO_ROOT}/builddir}"
CCJSON="${BUILDDIR}/compile_commands.json"

if [ ! -f "${CCJSON}" ]; then
    echo "no ${CCJSON}; run 'meson setup ${BUILDDIR}' first" >&2
    exit 2
fi

python3 - "$CCJSON" "$BUILDDIR" <<'PY'
import json, shlex, subprocess, sys

ccjson, builddir = sys.argv[1], sys.argv[2]
entries = json.load(open(ccjson))

# The embed library (libnordstjernen.so) compiles exactly the engine source
# set that the Android build uses, so check those translation units.
def is_engine(e):
    blob = e.get('output', '') + e.get('command', '')
    return 'libnordstjernen.so.p' in blob and e['file'].endswith('.c')

DROP_WITH_ARG = {'-o', '-MF', '-MQ', '-MT'}
DROP = {'-c', '-MD', '-MMD', '-MP'}

fails = []
checked = 0
for e in entries:
    if not is_engine(e):
        continue
    args = shlex.split(e['command']) if e.get('command') else list(e['arguments'])
    out, skip = [], False
    for a in args:
        if skip:
            skip = False; continue
        if a in DROP_WITH_ARG:
            skip = True; continue
        if a in DROP or a.endswith('.o') or a.endswith('.o.d'):
            continue
        if a.startswith('-DND_HAVE_GDK_PIXBUF') or a.startswith('-DND_HAVE_LIBRSVG'):
            continue
        out.append(a)
    out += ['-D__ANDROID__', '-fsyntax-only']
    r = subprocess.run(out, cwd=builddir, capture_output=True, text=True)
    checked += 1
    name = e['file'].split('/src/')[-1]
    if r.returncode != 0:
        fails.append(name)
        print(f"FAIL {name}")
        print('\n'.join(r.stderr.splitlines()[:25]))
    else:
        print(f"ok   {name}")

print(f"\nchecked {checked} engine sources under __ANDROID__; "
      f"{len(fails)} failed")
sys.exit(1 if fails else 0)
PY
