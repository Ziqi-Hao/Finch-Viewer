#!/usr/bin/env bash
# Run the Qt RHI (Metal) tractography editor on macOS.
#
#   ./tools/run_qt_editor.sh                       # open empty; load via File menu
#   ./tools/run_qt_editor.sh --trk in.trk          # open with a tractogram
#   ./tools/run_qt_editor.sh --trk in.trk --display-n 16000 --disp-step 3
#
# Controls: left-drag rotate · right/middle-drag pan · wheel zoom · R reset.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The target now builds a Finch-Viewer.app bundle (run the inner binary directly so
# CLI flags / headless --window-shot still work).
EXE="${REPO_ROOT}/build/Finch-Viewer.app/Contents/MacOS/Finch-Viewer"

if [[ ! -x "${EXE}" ]]; then
  echo "Not built yet. Run: ./tools/build_mac.sh local_editor_qt" >&2
  exit 1
fi

exec "${EXE}" "$@"
