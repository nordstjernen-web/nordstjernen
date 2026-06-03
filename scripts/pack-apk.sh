#!/usr/bin/env bash
# Build a portable Nordstjernen Alpine package (.apk) by repackaging the
# bundle that pack-linux.sh produces. The binary is statically linked
# against the in-tree engine (lexbor, quickjs); abuild's tracedeps picks
# up the dynamic GTK / curl / rsvg system deps from the binary's SONAMEs.
set -euo pipefail

log() { printf '[pack-apk] %s\n' "$*" >&2; }
trap 'rc=$?; printf "[pack-apk] FAILED (exit %s) at line %s: %s\n" \
    "$rc" "$LINENO" "$BASH_COMMAND" >&2; exit $rc' ERR
[ -n "${ND_DEBUG:-}" ] && set -x

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=${VERSION:-$(awk -F"'" \
    '/^[[:space:]]*version[[:space:]]*:/ { print $2; exit }' "$ROOT/meson.build")}
ARCH=$(uname -m)
FSVERSION=${VERSION//\~/-}
FSVERSION=${FSVERSION//\//-}
SLUG="nordstjernen-${FSVERSION}-linux-${ARCH}"
STAGE="$ROOT/dist/${SLUG}"

if ! command -v abuild >/dev/null 2>&1; then
    echo "abuild not found. Install it first:" >&2
    echo "    sudo apk add alpine-sdk       # Alpine" >&2
    exit 1
fi

if [ ! -x "$STAGE/nordstjernen" ]; then
    log "bundle not staged yet — running pack-linux.sh first"
    "$ROOT/scripts/pack-linux.sh"
fi

APKVER=$(printf '%s' "$VERSION" | tr 'A-Z' 'a-z')
APKBASE=$(printf '%s' "$APKVER" | grep -oE '^[0-9]+(\.[0-9]+)*' || true)
APKBASE=${APKBASE:-0}
APKREST=${APKVER#"$APKBASE"}
APKDIGITS=$(printf '%s' "$APKREST" | tr -cd '0-9')
if [ -n "$APKDIGITS" ]; then
    APKVER="${APKBASE}_git${APKDIGITS:0:12}"
else
    APKVER="$APKBASE"
fi
log "version=$VERSION apkver=$APKVER arch=$ARCH"

export PACKAGER="${PACKAGER:-Andreas Røsdal <andreas.rosdal@gmail.com>}"
ABUILD_HOME="$ROOT/dist/abuild-home"
rm -rf "$ABUILD_HOME"
mkdir -p "$ABUILD_HOME"
export HOME="$ABUILD_HOME"
abuild-keygen -a -i -n >/dev/null 2>&1 || abuild-keygen -a -n >/dev/null 2>&1

BUILDTOP="$ROOT/dist/apkbuild"
rm -rf "$BUILDTOP"
mkdir -p "$BUILDTOP"
export REPODEST="$ROOT/dist/apkrepo"
rm -rf "$REPODEST"

cat > "$BUILDTOP/APKBUILD" <<APKBUILD_EOF
# Maintainer: Andreas Røsdal <andreas.rosdal@gmail.com>
pkgname=nordstjernen
pkgver=${APKVER}
pkgrel=0
pkgdesc="Nordstjernen Web Navigator — a small, hand-written web browser"
url="https://nordstjernen.org"
arch="${ARCH}"
license="custom"
depends="gtk4.0 libcurl uchardet librsvg sqlite-libs ca-certificates fontconfig font-dejavu"
options="!check !tracedeps !strip"
source=""

build() {
	return 0
}

package() {
	install -Dm755 "${STAGE}/nordstjernen" "\$pkgdir/usr/bin/nordstjernen"
	for icon in "${STAGE}"/data/icons/hicolor/scalable/apps/nordstjernen*.svg \\
	            "${STAGE}"/data/icons/hicolor/scalable/apps/nordstjernen.gif; do
		[ -e "\$icon" ] && install -Dm644 "\$icon" \\
			"\$pkgdir/usr/share/icons/hicolor/scalable/apps/\$(basename "\$icon")"
	done
	install -Dm644 "${STAGE}/data/nordstjernen.desktop" \\
		"\$pkgdir/usr/share/applications/com.nordstjernen.Browser.desktop"
	install -Dm644 "${STAGE}/License.md" \\
		"\$pkgdir/usr/share/nordstjernen/License.md"
	install -Dm644 "${STAGE}/README.md" \\
		"\$pkgdir/usr/share/doc/nordstjernen/README.md"
	install -Dm644 "${STAGE}/THIRD-PARTY-LICENSES.md" \\
		"\$pkgdir/usr/share/doc/nordstjernen/THIRD-PARTY-LICENSES.md"
}
APKBUILD_EOF

( cd "$BUILDTOP" && abuild -F -d rootpkg )

APKFILE=$(find "$REPODEST" -name "nordstjernen-${APKVER}-r*.apk" | head -n1 || true)
if [ -z "$APKFILE" ]; then
    echo "abuild produced no .apk — see $BUILDTOP for details." >&2
    exit 1
fi

DEST="$ROOT/dist/nordstjernen-${FSVERSION}-${ARCH}.apk"
cp "$APKFILE" "$DEST"
[ -s "$DEST" ] || { log "ERROR: $DEST missing or empty"; exit 1; }

apk_size=$(du -h "$DEST" 2>/dev/null | cut -f1 || echo '?')
echo
echo "Built: $DEST ($apk_size)"
echo
echo "Inspect: tar -tzf $DEST"
echo "Install: sudo apk add --allow-untrusted $DEST"
