#!/usr/bin/env bash
# Package Finch-Viewer.app into a SELF-CONTAINED, distributable bundle + DMG.
#
# Dev iteration doesn't need this — run_qt_editor.sh runs straight from build/
# against Homebrew Qt. This is only for shipping to a Mac WITHOUT the Homebrew Qt
# the app was built against. It:
#   1. builds a pristine Release bundle,
#   2. copies it into a NON-iCloud temp dir and deploys there (see below),
#   3. bundles Qt frameworks + plugins via macdeployqt (rewriting /opt/homebrew
#      paths to @executable_path/../Frameworks), drops the unused SVG plugin, and
#      fixes any framework install-name ID macdeployqt missed,
#   4. ad-hoc codesigns and verifies nothing still points at /opt/homebrew,
#   5. copies the finished bundle back to build/ and writes the DMG there.
# libomp needs no handling — it's statically linked (see CMakeLists.txt).
#
# Why stage in a temp dir: when the repo lives under an iCloud-synced folder
# (~/Documents), the fileprovider daemon re-adds com.apple.FinderInfo to the
# bundle faster than we can strip it, and codesign rejects that "detritus".
# $TMPDIR (/var/folders/...) is not synced, so signing there is deterministic.
#
#   ./tools/package_mac.sh            # -> build/Finch-Viewer.app (self-contained)
#   ./tools/package_mac.sh --dmg      # also -> build/Finch-Viewer.dmg
#
# Gatekeeper note: an ad-hoc signature is enough to LAUNCH, but other users must
# right-click -> Open once (or `xattr -dr com.apple.quarantine` the .app) since it
# has no Apple Developer ID. Distributing a notarized build is out of scope here.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
APP="${BUILD_DIR}/Finch-Viewer.app"
BREW_PREFIX="$(brew --prefix)"
MACDEPLOYQT="$(command -v macdeployqt || echo "${BREW_PREFIX}/bin/macdeployqt")"

MAKE_DMG=0
for arg in "$@"; do
  case "$arg" in
    --dmg) MAKE_DMG=1 ;;
    *) echo "unknown arg: $arg (use --dmg)" >&2; exit 2 ;;
  esac
done
[ -x "$MACDEPLOYQT" ] || { echo "macdeployqt not found (brew install qt)" >&2; exit 1; }

# 1. Build a PRISTINE bundle. macdeployqt is NOT idempotent (it rewrites the
#    binary's Qt paths to @rpath in place, and an incremental rebuild won't
#    relink), so always start from a freshly linked bundle with Homebrew paths.
rm -rf "$APP"
"${REPO_ROOT}/tools/build_mac.sh" local_editor_qt
[ -d "$APP" ] || { echo "app bundle not found: $APP" >&2; exit 1; }

# 2. Deploy + sign in a non-iCloud staging dir (see header) and copy back.
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/finch-pkg.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
SAPP="${STAGE}/Finch-Viewer.app"
cp -R "$APP" "$SAPP"
xattr -cr "$SAPP"   # drop xattrs cp carried over from the synced source

# Bundle Qt frameworks + plugins. macdeployqt prints a non-fatal "cannot resolve
# QtSvg" for the SVG icon plugin we delete next — ignore it.
"$MACDEPLOYQT" "$SAPP" -verbose=1 -no-codesign

# Drop the SVG icon engine: the app uses no SVG (nothing else links QtSvg, which
# ships in a separate formula macdeployqt can't resolve) — removes dead weight and
# the only dangling dependency.
rm -rf "${SAPP}/Contents/PlugIns/iconengines"

# Fix framework self-IDs macdeployqt forgot to rewrite (observed: QtDBus, a
# transitive dep of QtGui — its ID still pointed at Homebrew while QtCore's was
# fixed). Any such ID would break on a machine without Homebrew.
while IFS= read -r -d '' fw; do
  case "$(otool -D "$fw" 2>/dev/null | tail -1)" in
    /opt/homebrew/*)
      install_name_tool -id "@executable_path/../${fw#${SAPP}/Contents/}" "$fw" ;;
  esac
done < <(find "${SAPP}/Contents/Frameworks" -type f -print0)

# Ad-hoc codesign (macdeployqt + install_name_tool invalidated the linker's
# signatures; Apple Silicon won't launch unsigned code). Deterministic here.
xattr -cr "$SAPP"
codesign --force --deep --sign - "$SAPP"

# 3. Verify self-containment: nothing under the bundle may link /opt/homebrew.
echo
echo "=== dependency check (want: nothing under /opt/homebrew) ==="
STRAY=""
while IFS= read -r -d '' f; do
  if otool -L "$f" 2>/dev/null | grep -q "/opt/homebrew"; then
    STRAY="${STRAY}${f#${STAGE}/}"$'\n'
  fi
done < <(find "$SAPP" -type f -print0 2>/dev/null)
if [ -n "$STRAY" ]; then
  echo "FAIL — these still link Homebrew paths:" >&2
  printf '%s' "$STRAY" >&2
  exit 1
fi
echo "OK: bundle is self-contained ($(du -sh "$SAPP" | cut -f1))."

# 4. DMG from the clean staged bundle (frozen before the copy-back can dirty it).
if [ "$MAKE_DMG" -eq 1 ]; then
  DMG="${BUILD_DIR}/Finch-Viewer.dmg"
  rm -f "$DMG"
  hdiutil create -volname "Finch-Viewer" -srcfolder "$SAPP" -ov -format UDZO "$DMG" >/dev/null
  echo "DMG: ${DMG} ($(du -sh "$DMG" | cut -f1))"
fi

# 5. Replace the build/ bundle with the deployed one (so run_qt_editor.sh and a
#    direct .app copy both work). This copy may pick up iCloud xattrs again, which
#    is harmless for local running — the pristine distributable is the DMG.
rm -rf "$APP"
cp -R "$SAPP" "$APP"
echo "App: ${APP}"
exit 0
