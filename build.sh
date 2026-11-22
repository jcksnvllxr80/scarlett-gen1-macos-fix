#!/bin/bash
#
# build.sh - builds the shim and creates a patched copy of Scarlett MixControl.
#
# Usage:
#   ./build.sh                     build into ./build/
#   ./build.sh --install           build, then copy the patched app into /Applications
#   ./build.sh --app "/path/to/Scarlett MixControl.app" [--install]
#
# Your original "Scarlett MixControl.app" is never modified.

set -euo pipefail

SRC_APP="/Applications/Scarlett MixControl.app"
INSTALL=0
EXPECTED_VERSION="1.10.6"
PATCHED_NAME="Scarlett MixControl (Patched)"
SHIM_NAME="libScarlettMixControlShim.dylib"
EXEC_NAME="Scarlett MixControl"

while [ $# -gt 0 ]; do
    case "$1" in
        --install) INSTALL=1 ;;
        --app) shift; SRC_APP="${1:?--app needs a path}" ;;
        -h|--help) sed -n '3,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1 (try --help)" >&2; exit 1 ;;
    esac
    shift
done

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
OUT_APP="$BUILD/$PATCHED_NAME.app"

step() { printf '\n==> %s\n' "$*"; }
fail() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

# --- Preflight -------------------------------------------------------------

step "Checking requirements"

[ "$(uname -s)" = "Darwin" ] || fail "This only works on macOS."

command -v clang >/dev/null 2>&1 && xcode-select -p >/dev/null 2>&1 \
    || fail "Apple's command line tools are missing. Run:  xcode-select --install   then run this script again."

if [ "$(uname -m)" = "arm64" ] && ! /usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null; then
    fail "Rosetta 2 is required (MixControl is an Intel app). Run:  softwareupdate --install-rosetta --agree-to-license"
fi

[ -d "$SRC_APP" ] || fail "Can't find \"$SRC_APP\".
       Install Scarlett MixControl $EXPECTED_VERSION from Focusrite first (see README), or pass --app /path/to/it."

SRC_EXEC="$SRC_APP/Contents/MacOS/$EXEC_NAME"
[ -f "$SRC_EXEC" ] || fail "\"$SRC_APP\" doesn't look like Scarlett MixControl (no \"$EXEC_NAME\" executable)."

VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$SRC_APP/Contents/Info.plist" 2>/dev/null || echo unknown)"
echo "Found Scarlett MixControl version $VERSION at: $SRC_APP"
if [ "$VERSION" != "$EXPECTED_VERSION" ]; then
    echo "WARNING: this was built and tested against version $EXPECTED_VERSION. Continuing anyway."
fi

if ! codesign -dv "$SRC_APP" 2>&1 | grep -q "TeamIdentifier=7VYBQV3T2Q"; then
    echo "WARNING: this copy isn't signed by Focusrite (it may already be patched or modified). Continuing anyway."
fi

# --- Build the shim --------------------------------------------------------

step "Compiling the shim"
mkdir -p "$BUILD"
# MixControl is an Intel (x86_64) app, so the shim must be x86_64 too.
clang -arch x86_64 -dynamiclib -O2 -Wall -Wextra \
    -mmacosx-version-min=10.13 \
    -framework IOKit -framework CoreFoundation -framework CoreAudio \
    -o "$BUILD/$SHIM_NAME" "$ROOT/shim.c"

# --- Make the patched copy -------------------------------------------------

step "Creating patched copy: $OUT_APP"
rm -rf "$OUT_APP"
ditto "$SRC_APP" "$OUT_APP"
xattr -cr "$OUT_APP"   # drop download quarantine flags copied from the original

MACOS_DIR="$OUT_APP/Contents/MacOS"
mkdir -p "$OUT_APP/Contents/Frameworks"
cp "$BUILD/$SHIM_NAME" "$OUT_APP/Contents/Frameworks/$SHIM_NAME"

# The real program moves aside; a tiny launcher takes its place and starts it
# with the shim loaded. (Don't launch via /usr/bin/arch or another system tool:
# macOS strips DYLD_ variables when running protected system binaries.)
mv "$MACOS_DIR/$EXEC_NAME" "$MACOS_DIR/$EXEC_NAME.bin"
cat > "$MACOS_DIR/$EXEC_NAME" <<'LAUNCHER'
#!/bin/sh
# Launcher added by scarlett-mix-control: starts MixControl with the shim loaded.
DIR="$(cd "$(dirname "$0")" && pwd)"
export DYLD_INSERT_LIBRARIES="$DIR/../Frameworks/libScarlettMixControlShim.dylib"
exec "$DIR/Scarlett MixControl.bin" "$@"
LAUNCHER
chmod 755 "$MACOS_DIR/$EXEC_NAME"

PLIST="$OUT_APP/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleName $PATCHED_NAME" "$PLIST" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Add :CFBundleName string $PATCHED_NAME" "$PLIST"
/usr/libexec/PlistBuddy -c "Set :CFBundleDisplayName $PATCHED_NAME" "$PLIST" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Add :CFBundleDisplayName string $PATCHED_NAME" "$PLIST"

# --- Re-sign ---------------------------------------------------------------
# Focusrite's signature uses the "hardened runtime", which blocks loading the
# shim. We replace it with a local (ad-hoc) signature without that restriction.

step "Signing the patched copy (ad-hoc, local only)"
codesign --force --sign - "$OUT_APP/Contents/Frameworks/$SHIM_NAME"
codesign --force --sign - "$MACOS_DIR/$EXEC_NAME.bin"
codesign --force --sign - "$OUT_APP"
codesign --verify --strict "$OUT_APP" || fail "Signature check failed."

# --- Install ---------------------------------------------------------------

FINAL_APP="$OUT_APP"
if [ "$INSTALL" = 1 ]; then
    step "Installing to /Applications"
    DEST="/Applications/$PATCHED_NAME.app"
    rm -rf "$DEST"
    ditto "$OUT_APP" "$DEST"
    FINAL_APP="$DEST"
fi

step "Done"
cat <<EOF
Patched app: $FINAL_APP

Open it like any other app (double-click it in Finder), or run:
  open "$FINAL_APP"

Log file (useful if something goes wrong):
  ~/Library/Logs/ScarlettMixControlShim.log

Do NOT use MixControl's firmware update feature with the patched app.
EOF
