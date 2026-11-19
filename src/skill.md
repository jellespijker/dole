---
name: dole
description: |
  Make beats with dole, an MPC-style sampler and sequencer whose songs are
  .dole JSON files: sixteen pads (a boom-bap kit, samples, chops, or CLAP and
  LV2 synths), effect chains, one-bar patterns, and a song chain. Use for ANY request to write,
  change, check, or render a beat, drum pattern, loop, chop, or .dole file, or
  to turn one into a WAV.
---

# dole

A dole song is one JSON file: a tempo and swing, the master's grit, sixteen
pads, patterns A to H of sixteen steps (a row per pad), and a song chain that
plays the patterns in order, a bar each. You write the file with your ordinary
editing tools; the `dole` command starts, checks, and renders it. No command
needs a display.

Run `dole help format` first. It prints the whole file format, and the kit
on each pad, for this version of dole.

## Workflow

1. Start: `dole new beat.dole` writes a dark boom-bap loop to build on;
   `dole new beat.dole --empty` writes nothing at all.
2. Edit the JSON. Rows are 16 characters (`X.....x...x.....`). Samples go
   beside the song and are named relative to it; a pad plays a chop of one
   with `start` and `end` in seconds, and `tune` pitches it.
   Pads and the master take plugins: `synth` on a pad, `effects` on a pad,
   `master` for the mix. `dole plugins` lists what's installed; dole's own
   (overdrive, bitcrush, ringmod, comb, delay, stutter) are always there.
3. Check: `dole check beat.dole --json` reports every problem, and the
   tempo, patterns, and length.
4. Listen: `dole render beat.dole -o beat.wav` renders the song;
   `--pattern B --bars 4` renders one pattern looping.
5. The person can open it to play the pads and edit with `dole open beat.dole`.

Each command takes `--json`, exits 0 or 1, and writes errors to stderr.
