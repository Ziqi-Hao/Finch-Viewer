#!/usr/bin/env bash
# macOS build entry point.
# Configures with Ninja against the Homebrew toolchain and builds Release.
#
#   ./tools/build_mac.sh                 # build everything found
#   ./tools/build_mac.sh local_editor_qt # build just the Qt editor
#
# Prerequisites (one-time):
#   brew install cmake ninja qt libomp  # Qt6 (qtbase + qtshadertools qsb) + OpenMP
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
BREW_PREFIX="$(brew --prefix)"

export PATH="${BREW_PREFIX}/bin:${PATH}"

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_PREFIX_PATH="${BREW_PREFIX}" \
  -DCMAKE_BUILD_TYPE=Release

if [[ $# -gt 0 ]]; then
  cmake --build "${BUILD_DIR}" --target "$@"
else
  cmake --build "${BUILD_DIR}"
fi

echo
echo "Built into ${BUILD_DIR}. Run the Qt editor with:"
echo "  ./tools/run_qt_editor.sh --trk in.trk"
