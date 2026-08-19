#!/usr/bin/env bash
# Build the Halo firmware from any working directory. See SKILL.md alongside this script.
#
# Usage:
#   build.sh              incremental build (fast; reuses build/halo + build/mcuboot)
#   build.sh pristine     full rebuild: west build --sysbuild -b halo alif/applications/halo/ -p
#   RELEASE=1 build.sh …  release build (default is debug)
set -euo pipefail

SKILL_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# alif/.claude/skills/build → the West workspace root is four levels up
WS_ROOT=$(cd "$SKILL_DIR/../../../.." && pwd)

if [[ ! -x "$WS_ROOT/.venv/bin/west" ]]; then
    echo "error: no West venv at $WS_ROOT/.venv — see alif/applications/halo/SETUP.md" >&2
    exit 1
fi

# cmake lives in the venv; west fails with "CMake is not installed" without this PATH
export PATH="$WS_ROOT/.venv/bin:$HOME/.local/bin:$PATH"
cd "$WS_ROOT"

case "${1:-incremental}" in
    incremental) west build -d build ;;
    pristine)    west build --sysbuild -b halo alif/applications/halo/ -p ;;
    *)           echo "usage: build.sh [incremental|pristine]  (RELEASE=1 for release)" >&2; exit 2 ;;
esac

echo
echo "Signed OTA image: $WS_ROOT/build/halo/zephyr/zephyr.signed.bin"
