---
name: dole
description: |
  Make beats with dole, an MPC-style sampler and sequencer whose songs are
  .dole JSON files: 64 pads in four banks (a boom-bap kit, samples, chops, or
  CLAP and LV2 synths), each with its filter, envelope, pan, and effect
  chain, patterns of one to four bars with rolls, chance, nudges, and
  automation lanes, and a song chain. Use for ANY request to write,
  change, check, or render a beat, drum pattern, loop, chop, or .dole file, to
  turn one into a WAV, or to play along live in the open dole window.
---

# dole

A dole song is one JSON file: a tempo and swing, the master's grit, 64 pads
in banks A to D, patterns A to H of 16 to 64 steps (a row per pad), and a
song chain that plays the patterns in order. You write the file with your ordinary
editing tools; the `dole` command starts, checks, and renders it. No command
needs a display.

Run `dole help format` first. It prints the whole file format, and the kit
on each pad, for this version of dole.

## Workflow

1. Start: `dole new beat.dole` writes a dark boom-bap loop to build on;
   `dole new beat.dole --empty` writes nothing at all.
2. Edit the JSON. Rows have a character a step (`X.....x...x.....`); a row
   can be an object with `roll`, `chance`, `nudge`, and `lanes` that move
   its pitch, filter, volume, pan, decay, or a plugin's parameter step by
   step. Pads take `pan`, `filter`, `resonance`, `attack`, `decay`,
   `reverse`, `gate`, and their own `swing` and `loop`. Samples go
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

## Playing the open window live

When the person has dole open and wants you to play along, change the
window's song instead of the file. With the dole MCP server (`dole mcp`),
use its tools; otherwise `dole remote`, which prints JSON and exits 0 or 1:

1. `dole remote song` prints the song as it is now, edits by hand included.
2. `dole remote edit '<json>'` merges a patch in, heard from the next step:
   `{"patch": {"patterns": {"A": {"hat": "x.x.x.x.x.x.x.x."}}}}`. Objects
   merge, anything else replaces, `null` removes; a row given as an object
   keeps its steps (`{"hat": {"roll": {"16": 3}}}`). `{"song": {...}}`
   replaces the whole song. Each edit is one undo for the person.
3. `dole remote play`, `stop`, `select B` (show pattern B), `hit hat`.
4. `dole remote wait bar` (or `loop`, `step`, `stop`, `edit`) returns when
   the music gets there, to land a change on the beat or to notice the
   person editing.

Replies take you seconds, so play by writing steps, not by hitting pads in
time. The song isn't saved until `dole remote save`; ask before saving
over the person's file.
