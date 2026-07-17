#!/usr/bin/env bash
# Build a Microsoft Store-ready MSIX package from the Windows bundle.
#
# Runs pack-windows.sh first, stages the bundle with an AppxManifest and
# Store tile assets, then packs it with makeappx.exe (Windows 10/11 SDK)
# or makemsix (msix-packaging CLI), whichever is found. The result is
# unsigned: Store submissions are re-signed by Microsoft after
# certification; for local sideload testing set NS_MSIX_CERT_PFX to a
# self-signed .pfx and the script signs with signtool. See
# docs/windows-store.md for the full submission guide.
#
# Identity values default to placeholders — override them with the values
# Partner Center shows under Product identity once the name is reserved:
#   NS_MSIX_IDENTITY_NAME       Package/Identity/Name
#   NS_MSIX_PUBLISHER           Package/Identity/Publisher (CN=GUID)
#   NS_MSIX_PUBLISHER_DISPLAY   PublisherDisplayName
#
# NS_MSIX_AI selects the local AI chat feature (llama.cpp): defaults to
# "disabled" so the Store package carries no model downloader or llama.cpp
# runtime; set NS_MSIX_AI=enabled to build a package that includes it.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
BUILDDIR=${BUILDDIR:-$ROOT/builddir-msix} \
NS_MESON_SETUP_ARGS="-Dai=${NS_MSIX_AI:-disabled} ${NS_MSIX_MESON_SETUP_ARGS:-}" \
    "$SCRIPT_DIR/pack-windows.sh"

VERSION=$(grep -E "^[[:space:]]*version:" "$ROOT/meson.build" \
          | head -1 | sed -E "s/.*'([^']+)'.*/\\1/")
# The Store reserves the fourth (revision) field — it must be 0 on submission
# (docs/windows-store.md), so map meson's Major.Minor.Build to X.Y.Z.0 rather
# than passing a fourth field through.
MSIX_VERSION=${NS_MSIX_VERSION:-$(awk -F. '{printf "%d.%d.%d.0", $1, $2, $3}' <<<"${VERSION%%-*}")}
IDENTITY_NAME=${NS_MSIX_IDENTITY_NAME:-29567TheFreecivProject.NordstjernenWebBrowser}
PUBLISHER=${NS_MSIX_PUBLISHER:-CN=631F98F7-2280-49EE-8EF8-534CC36D09CF}
PUBLISHER_DISPLAY=${NS_MSIX_PUBLISHER_DISPLAY:-Nordstjernen}
DISPLAY_NAME=${NS_MSIX_DISPLAY_NAME:-Nordstjernen Web Browser}
PHONE_PRODUCT_ID=${NS_MSIX_PHONE_PRODUCT_ID:-2c47a178-dfb0-4383-9dc0-aa7195bc8354}
PHONE_PUBLISHER_ID=${NS_MSIX_PHONE_PUBLISHER_ID:-eb62046e-1fa9-48a1-b651-cbf7237e9a03}

BUNDLE=$ROOT/dist/nordstjernen-win64
STAGE=$ROOT/dist/nordstjernen-msix
MSIX=$ROOT/dist/nordstjernen-${VERSION}-win64.msix
TEMPLATE=$ROOT/data/msix/AppxManifest.xml.in
SVG=$ROOT/data/icons/hicolor/scalable/apps/nordstjernen.svg

if ! command -v rsvg-convert >/dev/null 2>&1; then
    echo "pack-msix: rsvg-convert not found (pacman -S mingw-w64-x86_64-librsvg)" >&2
    exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/Assets"
cp -r "$BUNDLE"/. "$STAGE/"

render_asset() {
    rsvg-convert -w "$2" -h "$2" -f png -o "$STAGE/Assets/$1" "$SVG"
}
render_scaled() {
    render_asset "$1.png" "$2"
    local scale
    for scale in 100 125 150 200 400; do
        render_asset "$1.scale-$scale.png" $(( ($2 * scale + 50) / 100 ))
    done
}
render_wide_asset() {
    local b64 x
    b64=$(rsvg-convert -w "$3" -h "$3" -f png "$SVG" | base64 -w0)
    x=$(( ($2 - $3) / 2 ))
    printf '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="%s" height="%s"><image x="%s" y="0" width="%s" height="%s" xlink:href="data:image/png;base64,%s"/></svg>' \
        "$2" "$3" "$x" "$3" "$3" "$b64" \
        | rsvg-convert -f png -o "$STAGE/Assets/$1"
}
render_wide_scaled() {
    render_wide_asset "$1.png" "$2" "$3"
    local scale
    for scale in 100 125 150 200 400; do
        render_wide_asset "$1.scale-$scale.png" \
            $(( ($2 * scale + 50) / 100 )) $(( ($3 * scale + 50) / 100 ))
    done
}
render_pids=()
render_in_background() { "$@" & render_pids+=($!); }

render_in_background render_scaled StoreLogo 50
render_in_background render_scaled Square44x44Logo 44
render_in_background render_scaled Square71x71Logo 71
render_in_background render_scaled Square150x150Logo 150
render_in_background render_scaled Square310x310Logo 310
render_in_background render_wide_scaled Wide310x150Logo 310 150
for ts in 16 24 32 48 256; do
    render_in_background render_asset "Square44x44Logo.targetsize-$ts.png" "$ts"
    render_in_background render_asset "Square44x44Logo.targetsize-${ts}_altform-unplated.png" "$ts"
done

for pid in "${render_pids[@]}"; do
    if ! wait "$pid"; then
        echo "pack-msix: asset rendering failed" >&2
        exit 1
    fi
done

sed -e "s|@MSIX_VERSION@|$MSIX_VERSION|g" \
    -e "s|@IDENTITY_NAME@|$IDENTITY_NAME|g" \
    -e "s|@PUBLISHER@|$PUBLISHER|g" \
    -e "s|@PUBLISHER_DISPLAY_NAME@|$PUBLISHER_DISPLAY|g" \
    -e "s|@DISPLAY_NAME@|$DISPLAY_NAME|g" \
    -e "s|@PHONE_PRODUCT_ID@|$PHONE_PRODUCT_ID|g" \
    -e "s|@PHONE_PUBLISHER_ID@|$PHONE_PUBLISHER_ID|g" \
    "$TEMPLATE" > "$STAGE/AppxManifest.xml"

winpath() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$1"
    else
        printf '%s' "$1"
    fi
}

resolve_kit_tool() {
    if command -v "$1" >/dev/null 2>&1; then
        command -v "$1"; return
    fi
    local root kit
    for root in "/c/Program Files (x86)/Windows Kits/10/bin" \
                "/c/Program Files/Windows Kits/10/bin"; do
        [ -d "$root" ] || continue
        kit=$(find "$root" -maxdepth 3 -name "$1" -path '*/x64/*' \
              2>/dev/null | sort -V | tail -1)
        [ -n "$kit" ] && { echo "$kit"; return; }
    done
    return 1
}

if MAKEPRI=$(resolve_kit_tool makepri.exe); then
    (cd "$STAGE" &&
     MSYS2_ARG_CONV_EXCL='*' "$MAKEPRI" createconfig /cf priconfig.xml /dq en-US /o >/dev/null &&
     MSYS2_ARG_CONV_EXCL='*' "$MAKEPRI" new /pr . /cf priconfig.xml /of resources.pri /mn AppxManifest.xml /o >/dev/null &&
     rm -f priconfig.xml)
else
    echo "pack-msix: makepri.exe not found; packing without resources.pri" >&2
    echo "pack-msix: (scale/targetsize asset variants will go unused)" >&2
fi

rm -f "$MSIX"
if [ -n "${MAKEAPPX:-}" ] && [ -f "$MAKEAPPX" ] || MAKEAPPX=$(resolve_kit_tool makeappx.exe); then
    MSYS2_ARG_CONV_EXCL='*' "$MAKEAPPX" pack /o /d "$(winpath "$STAGE")" /p "$(winpath "$MSIX")"
elif command -v makemsix >/dev/null 2>&1; then
    makemsix pack -d "$STAGE" -p "$MSIX"
else
    echo "pack-msix: neither makeappx.exe (Windows SDK) nor makemsix found." >&2
    echo "Staged layout left in $STAGE for manual packing." >&2
    exit 1
fi

if [ -n "${NS_MSIX_CERT_PFX:-}" ]; then
    if ! SIGNTOOL=$(resolve_kit_tool signtool.exe); then
        echo "pack-msix: NS_MSIX_CERT_PFX set but signtool.exe not found" >&2
        exit 1
    fi
    MSYS2_ARG_CONV_EXCL='*' "$SIGNTOOL" sign /fd SHA256 /a /f \
        "$(winpath "$NS_MSIX_CERT_PFX")" \
        ${NS_MSIX_CERT_PASS:+/p "$NS_MSIX_CERT_PASS"} "$(winpath "$MSIX")"
fi

size=$(du -h "$MSIX" | awk '{print $1}')
printf 'pack-msix: built %s (%s, identity %s, version %s)\n' \
    "$MSIX" "$size" "$IDENTITY_NAME" "$MSIX_VERSION"
