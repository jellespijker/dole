---
name: dole
description: |
  Make beats and basslines with dole, a loop sequencer whose songs are .dole
  JSON files of drum and bass patterns chained into a song. Use for ANY request
  to write, change, check, or render a beat, drum pattern, bassline, loop, or
  .dole file, or to turn one into a WAV.
---

# dole

A dole song is one JSON file: a tempo, eight tracks (kick, snare, hat, openhat,
clap, rim, cowbell, bass), patterns A to H of sixteen steps, and a song chain
that plays the patterns in order, a bar each. You write the file with your
ordinary editing tools; the `dole` command starts, checks, and renders it. No
command needs a display.

Run `dole help format` first. It prints the whole file format for this
version of dole.

## Workflow

1. Start: `dole new beat.dole` writes a short groove to build on;
   `dole new beat.dole --empty` writes nothing at all.
2. Edit the JSON. Drum rows are 16 characters (`x...x...x...x...`), the bass
   row 16 words (`C2 - - . D#2 . . .`). Samples go beside the song and are
   named relative to it.
3. Check: `dole check beat.dole --json` reports every problem, and the
   tempo, patterns, and length.
4. Listen: `dole render beat.dole -o beat.wav` renders the song;
   `--pattern B --bars 4` renders one pattern looping.
5. The person can open it to play and edit with `dole open beat.dole`.

Each command takes `--json`, exits 0 or 1, and writes errors to stderr.
