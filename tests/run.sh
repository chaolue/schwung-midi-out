#!/usr/bin/env bash
# Native unit tests for the MIDI Out DSP. No device, no cross-compiler: the
# module talks to the host only through host_api_v1_t, which the test fakes.
set -euo pipefail

cd "$(dirname "$0")/.."

CC="${CC:-cc}"
bin="build/tests/test_midi_out_fx"
mkdir -p "$(dirname "$bin")"

"$CC" -std=c11 -Wall -Wextra -Werror \
  -Isrc/dsp \
  tests/test_midi_out_fx.c \
  src/dsp/midi_out.c \
  -o "$bin"

"$bin"
