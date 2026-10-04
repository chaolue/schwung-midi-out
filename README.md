# MIDI Out for Schwung

A chain MIDI FX for [Schwung](https://github.com/charlesvestal/schwung) on
Ableton Move that sends a slot's MIDI out of Move's **USB-A** port, to an
external synth or any class-compliant MIDI device.

Put it after other MIDI FX in a slot and what they produce — played notes,
chords, arpeggiator steps, CCs, pitch bend, aftertouch, program changes — goes
out on the one channel you pick. Everything also passes down the chain
unchanged, so a synth after it keeps playing. Leave the slot's synth empty to
play only the external one.

## Prerequisites

- [Schwung](https://github.com/charlesvestal/schwung) **1.0.0 or later** on your
  Ableton Move. Earlier hosts give chain MIDI FX no way to reach USB-A; there
  the module passes MIDI through and sends nothing.

## Controls

| Knob | Parameter | Range |
|------|-----------|-------|
| 1 | MIDI Channel | 1–16 (default 1) |

Every message goes out on this channel, whatever channel it arrived on.

## Behaviour worth knowing

- **Nothing gets stuck on the external synth when Move is busy.** Schwung's
  outgoing USB-A queue is shared with Move's own output and refuses packets
  when full. A refused message is held and retried on the next audio block, in
  order, and a note-off is never the one thrown away.
- **Changing channel mid-phrase is safe.** Note-offs and sustain-pedal releases
  go to the channel their note started on, not the new one.
- **Removing the module releases what it left sounding.**
- **MIDI clock is not sent.** Turn on Move's own MIDI Clock Out for tempo sync;
  a second copy would double the external device's tempo.
- **Bypass with keys held strands them.** A bypassed MIDI FX never sees the
  note-offs. Release the keys before bypassing, or send an all-notes-off from
  the external synth.
- **Leave the slot's MIDI FX setting on `Schw`.** `Schw+Move` also sends notes
  into Move, which echoes them to USB-A on the slot's receive channel, so the
  external synth hears them twice.
- **Move's tracks can send to USB-A too.** Pick a channel they do not use, or
  the external synth hears both.
- **If the external synth echoes MIDI back in, turn its MIDI thru off.**

## Installing

From **Schwung Manager** (`http://move.local:7700`) once the module is listed
in the Schwung catalog. Until then, or for a local build, over SSH:

```bash
./scripts/install.sh                                # from dist/ after a build
./scripts/install.sh ~/Downloads/midi-out-module.tar.gz   # a release tarball
MOVE_HOST=192.168.1.20 ./scripts/install.sh         # a Move not at move.local
```

It installs to `/data/UserData/schwung/modules/midi_fx/midi-out/`. A slot
that already had the module loaded keeps the old build until the slot is
reloaded or Move restarts.

## Building

```bash
./scripts/build.sh       # cross-compiles in Docker (scripts/Dockerfile)
bash tests/run.sh        # native unit tests, no device or cross-compiler needed
```

With an aarch64 toolchain installed, `CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh`
skips Docker. The build checks that `dsp.so` is AArch64 and exports
`move_midi_fx_init`, and produces `dist/midi-out-module.tar.gz`.

## Releasing

1. Bump `version` in `src/module.json` and commit to `main`.
2. Tag and push: `git tag v0.2.0 && git push origin v0.2.0`.

`.github/workflows/release.yml` refuses a tag that does not match
`module.json`, runs the tests, cross-compiles, attaches the tarball to a GitHub
release, and updates `release.json` on `main` — the file Schwung Manager reads
to offer the update. `ci.yml` runs the tests and the build on every push and
pull request, and keeps the tarball as a workflow artifact.

### Catalog entry

To list it in Schwung Manager, add this to `module-catalog.json` in the Schwung
repo:

```json
{
  "id": "midi-out",
  "name": "MIDI Out",
  "description": "Sends the slot's MIDI, after any MIDI FX before it, out of the USB-A port on one channel",
  "author": "chaolue",
  "component_type": "midi_fx",
  "github_repo": "chaolue/schwung-midi-out",
  "default_branch": "main",
  "asset_name": "midi-out-module.tar.gz",
  "min_host_version": "1.0.0"
}
```

## Vendored headers

`src/dsp/plugin_api_v1.h` and `src/dsp/midi_fx_api_v1.h` are verbatim copies
of Schwung's `src/host/` headers as of host 1.5.0. **Never add fields to them**:
a module's copy of `host_api_v1_t` that declares a field the host does not
have reads someone else's memory (Schwung's CLAUDE.md records a boot loop
caused by exactly that). The one offset this module reads, `midi_send_external`
at +48, is pinned with a `_Static_assert` in `midi_out.c`, so a header that
drifts there fails the build rather than the device.

## License

MIT — see [LICENSE](LICENSE). The vendored headers are Schwung's, also MIT.
