#!/usr/bin/env bash
# Nordstjernen nightly build orchestrator. Builds, from a single Linux host,
# a source tarball, per-distro Linux packages (debian/ubuntu/opensuse via
# containers), and Windows + macOS builds (by driving the GitHub Actions
# runners), then collects everything into a dated directory under
# $NIGHTLY_ROOT with checksums, a manifest, a 'latest' symlink, and pruning
# of old builds. Intended to run unattended from cron. See --help.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)

NIGHTLY_ROOT=${NIGHTLY_ROOT:-/var/www/html/nightly}
NIGHTLY_REF=${NIGHTLY_REF:-origin/main}
NIGHTLY_GHA_BRANCH=${NIGHTLY_GHA_BRANCH:-main}
NIGHTLY_GHA_TIMEOUT=${NIGHTLY_GHA_TIMEOUT:-4200}
NIGHTLY_GHA_DISPATCH=${NIGHTLY_GHA_DISPATCH:-1}
NIGHTLY_PULL=${NIGHTLY_PULL:-1}
NIGHTLY_PULL_BRANCH=${NIGHTLY_PULL_BRANCH:-main}
DOCKER=${ND_DOCKER:-docker}

NIGHTLY_DEBIAN_IMAGE=${NIGHTLY_DEBIAN_IMAGE:-debian:trixie}
NIGHTLY_UBUNTU_IMAGE=${NIGHTLY_UBUNTU_IMAGE:-ubuntu:24.04}
NIGHTLY_OPENSUSE_IMAGE=${NIGHTLY_OPENSUSE_IMAGE:-opensuse/tumbleweed}
NIGHTLY_ALPINE_IMAGE=${NIGHTLY_ALPINE_IMAGE:-alpine:edge}

DO_TARBALL=1
DO_DOCKER=1
DO_GHA=1
DO_JAVA=1
DATE=""

usage() {
    cat <<EOF
Usage: ./scripts/nightly.sh [options]

Builds nightly artifacts for source, Linux (debian/ubuntu/opensuse via
containers), Windows and macOS (via GitHub Actions) into a dated dir.

Options:
  --date YYYY-MM-DD   Output dir date stamp (default: today, UTC).
  --root DIR          Output root (default: \$NIGHTLY_ROOT or /var/www/html/nightly).
  --ref REF           Git ref to archive/build (default: origin/main).
  --no-tarball        Skip the source tarball stage.
  --no-docker         Skip the Linux container builds.
  --no-gha            Skip driving Windows/macOS via GitHub Actions.
  --no-java           Skip the Java API jar/javadoc stage.
  --no-pull           Don't fast-forward the working tree to origin/main first.
  -h, --help          Show this help.

Before building, the script fast-forwards its own checkout to
origin/\$NIGHTLY_PULL_BRANCH (default main) and re-runs itself if that
moved nightly.sh, so a plain cron invocation always builds the latest
orchestrator. A dirty or diverged working tree is left untouched.

Environment overrides: NIGHTLY_ROOT, NIGHTLY_REF, NIGHTLY_PULL,
NIGHTLY_PULL_BRANCH, NIGHTLY_GHA_TIMEOUT, NIGHTLY_GHA_BRANCH, ND_DOCKER,
and the NIGHTLY_{DEBIAN,UBUNTU,OPENSUSE}_IMAGE image tags.
EOF
}

ORIG_ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        --date)      DATE="$2"; shift 2 ;;
        --root)      NIGHTLY_ROOT="$2"; shift 2 ;;
        --ref)       NIGHTLY_REF="$2"; shift 2 ;;
        --no-tarball) DO_TARBALL=0; shift ;;
        --no-docker)  DO_DOCKER=0; shift ;;
        --no-gha)     DO_GHA=0; shift ;;
        --no-java)    DO_JAVA=0; shift ;;
        --no-pull)    NIGHTLY_PULL=0; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

cd "$ROOT"

if [ "$NIGHTLY_PULL" = 1 ] && [ "${NIGHTLY_SELF_UPDATED:-0}" != 1 ]; then
    before=$(git rev-parse HEAD 2>/dev/null || echo none)
    if git fetch --quiet origin "$NIGHTLY_PULL_BRANCH"; then
        if [ -n "$(git status --porcelain 2>/dev/null)" ]; then
            echo "warn: working tree not clean; skipping pull, using it as-is"
        elif ! git merge --ff-only --quiet "origin/$NIGHTLY_PULL_BRANCH"; then
            echo "warn: cannot fast-forward to origin/$NIGHTLY_PULL_BRANCH (diverged?); using working tree as-is"
        fi
    else
        echo "warn: git fetch failed; using local refs"
    fi
    after=$(git rev-parse HEAD 2>/dev/null || echo none)
    if [ "$before" != "$after" ]; then
        echo "orchestrator updated ${before:0:7}..${after:0:7}; re-running latest nightly.sh"
        export NIGHTLY_SELF_UPDATED=1
        exec bash "$0" ${ORIG_ARGS[@]+"${ORIG_ARGS[@]}"}
    fi
else
    git fetch --quiet origin || echo "warn: git fetch failed; using local refs"
fi

[ -n "$DATE" ] || DATE=$(date -u +%Y-%m-%d)
DATESTAMP=${DATE//-/}
COMMIT=$(git rev-parse --short "$NIGHTLY_REF" 2>/dev/null || git rev-parse --short HEAD)
MESON_VERSION=$(grep -E "^[[:space:]]*version" "$ROOT/meson.build" | head -1 \
                | sed -E "s/.*version: '([^']+)'.*/\1/")
NVERSION="${MESON_VERSION}+nightly.${DATESTAMP}.g${COMMIT}"

OUTDIR="$NIGHTLY_ROOT"
WORK=$(mktemp -d)
mkdir -p "$OUTDIR"
rm -rf "$OUTDIR/source" "$OUTDIR/linux" "$OUTDIR/windows" "$OUTDIR/macos" "$OUTDIR/java"
rm -f "$OUTDIR"/SHA256SUMS "$OUTDIR"/MANIFEST.txt "$OUTDIR"/nightly.log \
      "$OUTDIR"/nordstjernen-*
LOG="$OUTDIR/nightly.log"
trap 'rm -rf "$WORK"' EXIT

exec > >(tee -a "$LOG") 2>&1

declare -A STATUS

log()  { printf '\n=== %s ===\n' "$*"; }
ok()   { STATUS[$1]=ok;      printf '[ ok ]   %s\n' "$1"; }
fail() { STATUS[$1]=FAILED;  printf '[FAIL]   %s — %s\n' "$1" "${2:-}"; }
skip() { STATUS[$1]=skipped; printf '[skip]   %s\n' "$1"; }
dump_tail() {
    local f="$1" n="${2:-80}"
    if [ ! -s "$f" ]; then
        printf -- '----- %s is empty or missing -----\n' "$f"
        return 0
    fi
    printf -- '----- %s (last %s lines) -----\n' "$f" "$n"
    tail -n "$n" "$f"
    printf -- '----- end %s -----\n' "$f"
}

log "Nordstjernen nightly $DATE"
printf 'ref=%s commit=%s version=%s\nroot=%s\n' \
    "$NIGHTLY_REF" "$COMMIT" "$NVERSION" "$OUTDIR"

archive_to() {
    git archive --format=tar --prefix="nordstjernen-${NVERSION}/" "$NIGHTLY_REF" \
        | tar -x -C "$1"
}

stage_tarball() {
    log "Stage: source tarball"
    local dst="$OUTDIR/source"
    mkdir -p "$dst"
    local base="nordstjernen-${NVERSION}"
    if git archive --format=tar --prefix="${base}/" "$NIGHTLY_REF" \
           | gzip -9 > "$dst/${base}.tar.gz" \
       && git archive --format=tar --prefix="${base}/" "$NIGHTLY_REF" \
           | xz -9 > "$dst/${base}.tar.xz"; then
        ok "source-tarball"
    else
        fail "source-tarball" "git archive failed"
    fi
}

stage_distro() {
    local distro="$1" image="$2" version="$3"
    local key="linux-$distro"
    log "Stage: $distro container build ($image)"
    if ! $DOCKER image inspect "$image" >/dev/null 2>&1; then
        $DOCKER pull "$image" || { fail "$key" "pull $image failed"; return; }
    fi
    local src="$WORK/$distro"
    mkdir -p "$src"
    archive_to "$src"
    local tree="$src/nordstjernen-${NVERSION}"
    local dst="$OUTDIR/linux/$distro"
    mkdir -p "$dst"
    if $DOCKER run --rm \
        -v "$tree:/build:z" -w /build \
        -e "VERSION=$version" \
        -e "ND_BUILD_DATE=$DATE" \
        "$image" \
        sh -c 'command -v bash >/dev/null 2>&1 || apk add --no-cache bash >/dev/null 2>&1 || true; exec bash scripts/nightly-distro-build.sh "$1"' sh "$distro" 2>&1 | tee "$dst/build.log"; then
        local n=0
        shopt -s nullglob
        for f in "$tree"/dist/*.zip "$tree"/dist/*.deb "$tree"/dist/*.rpm "$tree"/dist/*.apk; do
            cp "$f" "$dst/" && n=$((n+1))
        done
        shopt -u nullglob
        if [ "$n" -gt 0 ]; then ok "$key"; else fail "$key" "no artifacts produced (see $dst/build.log)"; fi
    else
        fail "$key" "container build failed (see $dst/build.log)"
    fi
    rm -rf "$src"
}

gha_run() {
    local wf="$1" plat="$2"
    local key="gha-$plat"
    log "Stage: GitHub Actions $plat ($wf)"
    if ! gh auth status >/dev/null 2>&1; then
        fail "$key" "gh not authenticated"
        return
    fi
    local sha
    sha=$(git rev-parse "$NIGHTLY_REF" 2>/dev/null || git rev-parse HEAD)
    local rid rstatus rconc usable=0
    read -r rid rstatus rconc < <(gh api \
        "repos/{owner}/{repo}/actions/workflows/$wf/runs?head_sha=$sha&per_page=1" \
        --jq '.workflow_runs[0] | "\(.id // 0) \(.status // "none") \(.conclusion // "none")"' \
        2>/dev/null || echo "0 none none")
    if [ "${rid:-0}" != 0 ] && { [ "$rstatus" != completed ] || [ "$rconc" = success ]; }; then
        usable=1
        printf 'reusing %s run %s for %s (status=%s)\n' "$wf" "$rid" "${sha:0:7}" "$rstatus"
    fi
    if [ "$usable" != 1 ]; then
        if [ "$NIGHTLY_GHA_DISPATCH" != 1 ]; then
            fail "$key" "no usable $wf run for ${sha:0:7} (dispatch disabled)"
            return
        fi
        printf 'no usable %s run for %s; dispatching on %s\n' \
            "$wf" "${sha:0:7}" "$NIGHTLY_GHA_BRANCH"
        if ! gh workflow run "$wf" --ref "$NIGHTLY_GHA_BRANCH"; then
            fail "$key" "workflow_dispatch failed"
            return
        fi
        rid=0
        local i
        for i in $(seq 1 40); do
            sleep 6
            rid=$(gh api \
                "repos/{owner}/{repo}/actions/workflows/$wf/runs?branch=$NIGHTLY_GHA_BRANCH&event=workflow_dispatch&per_page=1" \
                --jq '.workflow_runs[0].id // 0' 2>/dev/null || echo 0)
            [ "$rid" != 0 ] && break
        done
        if [ "$rid" = 0 ]; then
            fail "$key" "dispatched run did not appear (Actions disabled for this repo?)"
            return
        fi
    fi
    printf 'watching run %s (timeout %ss)\n' "$rid" "$NIGHTLY_GHA_TIMEOUT"
    local deadline=$(( $(date +%s) + NIGHTLY_GHA_TIMEOUT ))
    local status conclusion
    while :; do
        status=$(gh run view "$rid" --json status --jq '.status' 2>/dev/null || echo unknown)
        [ "$status" = completed ] && break
        if [ "$(date +%s)" -ge "$deadline" ]; then
            fail "$key" "timed out waiting for run $rid"
            return
        fi
        sleep 20
    done
    conclusion=$(gh run view "$rid" --json conclusion --jq '.conclusion' 2>/dev/null || echo unknown)
    local dst="$OUTDIR/$plat"
    mkdir -p "$dst"
    gh run download "$rid" -D "$dst" 2>/dev/null || true
    if [ "$conclusion" = success ] && [ -n "$(find "$dst" -type f 2>/dev/null)" ]; then
        ok "$key"
    else
        fail "$key" "conclusion=$conclusion (artifacts may be partial)"
    fi
}

stage_java() {
    log "Stage: Java API (jar + sources + javadoc)"
    local key="java"
    local jhome="${JAVA_HOME:-}"
    if [ -z "$jhome" ] && command -v javac >/dev/null 2>&1; then
        jhome=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")
    fi
    if [ -z "$jhome" ] || [ ! -x "$jhome/bin/javac" ]; then
        fail "$key" "JDK not found (JAVA_HOME='${JAVA_HOME:-}', no usable javac on PATH); install openjdk-21-jdk or set JAVA_HOME"
        return
    fi
    local dst="$OUTDIR/java"
    mkdir -p "$dst"
    local blog="$dst/build.log"
    local work="$WORK/javabuild"
    mkdir -p "$work/classes" "$work/stage" "$work/doc"

    {
        printf 'Nordstjernen Java API build — %s\n' "$NVERSION"
        printf 'JAVA_HOME=%s\nCC=%s\nengine build dir=%s\n' \
            "$jhome" "${CC:-cc}" "$WORK/java-engine"
        "$jhome/bin/javac" -version 2>&1 || true
        printf -- '----------------------------------------\n'
    } | tee "$blog"

    log "Java: build native libraries (engine + JNI bridge)"
    mkdir -p "$work/stage/native"
    local nativeok=0
    if command -v "$DOCKER" >/dev/null 2>&1; then
        local jsrc="$WORK/javanative"
        mkdir -p "$jsrc"
        archive_to "$jsrc"
        local jtree="$jsrc/nordstjernen-${NVERSION}"
        if ! $DOCKER image inspect "$NIGHTLY_DEBIAN_IMAGE" >/dev/null 2>&1; then
            $DOCKER pull "$NIGHTLY_DEBIAN_IMAGE" >> "$blog" 2>&1 || true
        fi
        if $DOCKER run --rm -v "$jtree:/build:z" -w /build \
                -e "CC=${CC:-cc}" "$NIGHTLY_DEBIAN_IMAGE" \
                bash scripts/nightly-java-native.sh >> "$blog" 2>&1 \
           && [ -d "$jtree/java/src/main/resources/native" ]; then
            cp -r "$jtree/java/src/main/resources/native/." "$work/stage/native/"
            nativeok=1
        fi
        rm -rf "$jsrc"
    fi
    if [ "$nativeok" != 1 ]; then
        log "Java: container native build unavailable; falling back to host toolchain"
        if JAVA_HOME="$jhome" BUILDDIR="$WORK/java-engine" CC="${CC:-cc}" \
                bash "$ROOT/java/scripts/build-native.sh" >> "$blog" 2>&1 \
           && [ -d "$ROOT/java/src/main/resources/native" ]; then
            cp -r "$ROOT/java/src/main/resources/native/." "$work/stage/native/"
            nativeok=1
        fi
    fi
    if [ "$nativeok" != 1 ]; then
        dump_tail "$blog"
        fail "$key" "native build failed (engine + JNI bridge) — see $blog"
        return
    fi
    log "Java: javac"
    if ! "$jhome/bin/javac" -d "$work/classes" \
            "$ROOT"/java/src/main/java/org/nordstjernen/*.java >> "$blog" 2>&1; then
        dump_tail "$blog"
        fail "$key" "javac failed — see $blog"
        return
    fi

    cp -r "$work/classes/." "$work/stage/"
    printf 'Automatic-Module-Name: org.nordstjernen\nEnable-Native-Access: ALL-UNNAMED\n' \
        > "$work/mf.txt"

    local base="nordstjernen-java-${NVERSION}"
    log "Java: jar"
    if ! "$jhome/bin/jar" --create --file "$dst/${base}.jar" \
             --manifest "$work/mf.txt" -C "$work/stage" . >> "$blog" 2>&1 \
       || ! "$jhome/bin/jar" --create --file "$dst/${base}-sources.jar" \
             -C "$ROOT/java/src/main/java" . >> "$blog" 2>&1; then
        dump_tail "$blog"
        fail "$key" "jar failed — see $blog"
        return
    fi

    log "Java: javadoc"
    if "$jhome/bin/javadoc" -quiet -Xdoclint:none -d "$work/doc" \
            -sourcepath "$ROOT/java/src/main/java" org.nordstjernen >> "$blog" 2>&1; then
        "$jhome/bin/jar" --create --file "$dst/${base}-javadoc.jar" -C "$work/doc" . >> "$blog" 2>&1 || true
        rm -rf "$dst/apidocs"
        cp -r "$work/doc" "$dst/apidocs"
    else
        dump_tail "$blog"
        fail "$key" "javadoc failed — see $blog"
        return
    fi

    ln -sfn "java/${base}.jar"         "$OUTDIR/nordstjernen-java.jar"
    ln -sfn "java/${base}-sources.jar" "$OUTDIR/nordstjernen-java-sources.jar"
    ln -sfn "java/${base}-javadoc.jar" "$OUTDIR/nordstjernen-java-javadoc.jar"
    ok "$key"
}

link_stable() {
    local name="$1" pattern="$2" matches
    shopt -s nullglob
    matches=( "$OUTDIR"/$pattern )
    shopt -u nullglob
    if [ "${#matches[@]}" -gt 0 ]; then
        ln -sfn "${matches[0]#"$OUTDIR"/}" "$OUTDIR/$name"
        printf '  %-32s -> %s\n' "$name" "${matches[0]#"$OUTDIR"/}"
    fi
}

stage_stable_links() {
    log "Stable download links (/nightly/)"
    link_stable nordstjernen-windows-x86_64.zip  'windows/*/*-windows-x86_64.zip'
    link_stable nordstjernen-windows-x86_64.exe  'windows/*/nordstjernen.exe'
    link_stable nordstjernen-macos.dmg           'macos/*/*.dmg'
    link_stable nordstjernen-macos-x86_64        'macos/*/nordstjernen'
    link_stable nordstjernen-debian-amd64.deb    'linux/debian/*.deb'
    link_stable nordstjernen-ubuntu-amd64.deb    'linux/ubuntu/*.deb'
    link_stable nordstjernen-opensuse-x86_64.rpm 'linux/opensuse/*.rpm'
    link_stable nordstjernen-linux-x86_64.zip    'linux/ubuntu/*-linux-x86_64.zip'
    link_stable nordstjernen-alpine-x86_64.zip   'linux/alpine/*-linux-x86_64.zip'
    link_stable nordstjernen-alpine-x86_64.apk   'linux/alpine/*.apk'
    link_stable nordstjernen-src.tar.xz          'source/*.tar.xz'
    link_stable nordstjernen-src.tar.gz          'source/*.tar.gz'
}

[ "$DO_TARBALL" = 1 ] && stage_tarball || skip "source-tarball"

if [ "$DO_DOCKER" = 1 ]; then
    if command -v "$DOCKER" >/dev/null 2>&1; then
        stage_distro debian   "$NIGHTLY_DEBIAN_IMAGE"   "$NVERSION"
        stage_distro ubuntu   "$NIGHTLY_UBUNTU_IMAGE"   "$NVERSION"
        stage_distro opensuse "$NIGHTLY_OPENSUSE_IMAGE" "$NVERSION"
        stage_distro alpine   "$NIGHTLY_ALPINE_IMAGE"   "$NVERSION"
    else
        fail "linux-debian" "$DOCKER not found"
        fail "linux-ubuntu" "$DOCKER not found"
        fail "linux-opensuse" "$DOCKER not found"
        fail "linux-alpine" "$DOCKER not found"
    fi
else
    skip "linux-debian"; skip "linux-ubuntu"; skip "linux-opensuse"
    skip "linux-alpine"
fi

if [ "$DO_GHA" = 1 ]; then
    gha_run windows.yml windows
    gha_run macos.yml   macos
else
    skip "gha-windows"; skip "gha-macos"
fi

[ "$DO_JAVA" = 1 ] && stage_java || skip "java"

stage_stable_links

log "Checksums"
( cd "$OUTDIR" && find . -type f ! -name SHA256SUMS ! -name nightly.log \
    -exec sha256sum {} + > SHA256SUMS ) && ok "checksums" || fail "checksums"

log "Manifest"
{
    echo "Nordstjernen nightly build"
    echo "date:    $DATE"
    echo "ref:     $NIGHTLY_REF"
    echo "commit:  $COMMIT"
    echo "version: $NVERSION"
    echo "built:   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo
    echo "Stage status:"
    for k in $(printf '%s\n' "${!STATUS[@]}" | sort); do
        printf '  %-18s %s\n' "$k" "${STATUS[$k]}"
    done
    echo
    echo "Artifacts:"
    ( cd "$OUTDIR" && find . -type f ! -name nightly.log | sort \
        | while read -r f; do printf '  %8s  %s\n' "$(du -h "$f" | cut -f1)" "$f"; done )
} > "$OUTDIR/MANIFEST.txt"
cat "$OUTDIR/MANIFEST.txt"

log "Summary"
rc=0
for k in $(printf '%s\n' "${!STATUS[@]}" | sort); do
    printf '  %-18s %s\n' "$k" "${STATUS[$k]}"
    [ "${STATUS[$k]}" = FAILED ] && rc=1
done
printf '\nOutput: %s\n' "$OUTDIR"
exit $rc
