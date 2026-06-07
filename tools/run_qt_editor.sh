#!/usr/bin/env bash
# Run the Qt/OpenGL tractography editor on macOS.
#
#   ./tools/run_qt_editor.sh                       # open empty; load via File menu
#   ./tools/run_qt_editor.sh --trk in.trk          # open with a tractogram
#   ./tools/run_qt_editor.sh --trk in.trk --display-n 16000 --disp-step 3
#
# Controls: left-drag rotate · right/middle-drag pan · wheel zoom · R reset.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="${REPO_ROOT}/build/local_editor_qt"

if [[ ! -x "${EXE}" ]]; then
  echo "Not built yet. Run: ./tools/build_mac.sh local_editor_qt" >&2
  exit 1
fi

exec "${EXE}" "$@"
