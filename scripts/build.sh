#!/usr/bin/env bash
# Build the MIDI Out module for Ableton Move (aarch64).
#
# With no CROSS_PREFIX and outside a container, re-runs itself inside the
# scripts/Dockerfile toolchain. Inside (or with CROSS_PREFIX set) it compiles
# dsp.so, checks it, and packages dist/midi-out-module.tar.gz.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="midi-out"
IMAGE_NAME="schwung-midi-out-builder"

if [ -z "${CROSS_PREFIX:-}" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Building MIDI Out (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        bash scripts/build.sh
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"
rm -rf "dist/$MODULE_ID" "dist/$MODULE_ID-module.tar.gz"
mkdir -p build "dist/$MODULE_ID"

echo "=== Compiling dsp.so ==="
"${CROSS_PREFIX}gcc" -std=gnu11 -O2 -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -Wall -Wextra -Werror \
    -Isrc/dsp \
    src/dsp/midi_out.c \
    -o build/dsp.so

# What the chain host will check, checked here instead of on the device: the
# right architecture, and the init symbol it dlsym()s. A misspelt export loads
# fine and then fails as "missing init symbol" in a log nobody is reading.
if ! "${CROSS_PREFIX}readelf" -h build/dsp.so | grep -q 'Machine:.*AArch64'; then
    echo "ERROR: build/dsp.so is not AArch64" >&2
    "${CROSS_PREFIX}readelf" -h build/dsp.so >&2
    exit 1
fi
if ! "${CROSS_PREFIX}nm" -D --defined-only build/dsp.so | grep -qw 'move_midi_fx_init'; then
    echo "ERROR: build/dsp.so does not export move_midi_fx_init" >&2
    exit 1
fi
echo "glibc symbol versions required:"
"${CROSS_PREFIX}readelf" -V build/dsp.so | grep -o 'GLIBC_[0-9.]*' | sort -uV | sed 's/^/  /'

echo "=== Packaging ==="
# Every file below is part of the module, so a missing one fails the build
# (set -e) instead of shipping a tarball without it.
cp src/module.json "dist/$MODULE_ID/module.json"
cp build/dsp.so    "dist/$MODULE_ID/dsp.so"
cp src/help.json   "dist/$MODULE_ID/help.json"
cp LICENSE         "dist/$MODULE_ID/LICENSE"
chmod 755 "dist/$MODULE_ID/dsp.so"

(cd dist && tar -czf "$MODULE_ID-module.tar.gz" "$MODULE_ID/")

# The installer extracts into modules/midi_fx/, so the tarball must hold one
# directory named for the id and nothing beside it.
bad=$(tar -tzf "dist/$MODULE_ID-module.tar.gz" | grep -v "^$MODULE_ID/" || true)
if [ -n "$bad" ]; then
    echo "ERROR: tarball has entries outside $MODULE_ID/:" >&2
    echo "$bad" >&2
    exit 1
fi

echo ""
tar -tzvf "dist/$MODULE_ID-module.tar.gz"
echo ""
echo "OK: dist/$MODULE_ID-module.tar.gz"
echo "To install on a Move: ./scripts/install.sh"
