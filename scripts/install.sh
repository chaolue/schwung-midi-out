#!/usr/bin/env bash
# Install MIDI Out onto a Move over SSH, from dist/ (run ./scripts/build.sh
# first) or from a release tarball given as the first argument.
#
#   ./scripts/install.sh
#   ./scripts/install.sh path/to/midi-out-module.tar.gz
#   MOVE_HOST=192.168.1.20 ./scripts/install.sh
#
# The module directory is SWAPPED IN BY RENAME, never overwritten in place. A
# slot that has this MIDI FX loaded has dsp.so mapped; rewriting that file
# under it can crash Move. A rename leaves the running copy's inode intact
# until the slot reloads.
#
# Everything is staged under /data/UserData -- never /tmp, which on the Move
# sits on a root filesystem that is usually full.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="midi-out"
MOVE="ableton@${MOVE_HOST:-move.local}"
SCHWUNG_DIR="${SCHWUNG_DIR:-/data/UserData/schwung}"   # overridable for testing
DEST="$SCHWUNG_DIR/modules/midi_fx/$MODULE_ID"
STAGE="$SCHWUNG_DIR/.install-staging/$MODULE_ID"

TARBALL="${1:-$REPO_ROOT/dist/$MODULE_ID-module.tar.gz}"
if [ ! -f "$TARBALL" ]; then
    echo "Error: $TARBALL not found. Run ./scripts/build.sh first," >&2
    echo "or pass a release tarball: ./scripts/install.sh midi-out-module.tar.gz" >&2
    exit 1
fi

echo "=== Installing MIDI Out to $MOVE ==="

ssh "$MOVE" "rm -rf '$STAGE' && mkdir -p '$STAGE' '$(dirname "$DEST")'"
scp "$TARBALL" "$MOVE:$STAGE/module.tar.gz"
ssh "$MOVE" "set -e
cd '$STAGE'
tar -xzf module.tar.gz
test -f '$MODULE_ID/module.json' && test -f '$MODULE_ID/dsp.so'
chmod -R a+rw '$MODULE_ID'
rm -rf old
if [ -d '$DEST' ]; then mv '$DEST' old; fi
mv '$MODULE_ID' '$DEST'
cd / && rm -rf '$STAGE'"

echo ""
echo "Installed to $DEST"
echo "Add it to a slot from the MIDI FX picker. A slot that already had it"
echo "loaded keeps the old build until the slot is reloaded or Move restarts."
