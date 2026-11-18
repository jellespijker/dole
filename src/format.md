# The .dole song format

A dole song is one JSON file. Every field but `"dole"` is optional.

```json
{
  "dole": 2,
  "bpm": 88,
  "swing": 58,
  "grit": "12-bit",
  "vinyl": true,
  "pads": {
    "13": { "volume": 0.7, "tune": -2 },
    "9":  { "sample": "samples/soul.wav", "start": 1.25, "end": 1.9, "tune": -3 }
  },
  "patterns": {
    "A": {
      "1":  "X......x..x.....",
      "3":  "....X.......X...",
      "7":  "x.o.x.o.x.o.x.oo",
      "13": "x..........x...."
    }
  },
  "song": ["A", "A", "A", "B"]
}
```

- `dole`: 2. Files from the first version, with tracks instead of pads,
  still open.
- `bpm`: tempo, 40 to 240. Default 90.
- `swing`: 50 (straight) to 75. Every second sixteenth is pushed late; 58 is
  a lazy boom-bap feel, 66 a triplet shuffle. Default 50.
- `grit`: the master's character: `"clean"`, `"12-bit"` (an SP-1200's 26 kHz
  and 12 bits), or `"crushed"` (16 kHz, 7 bits, driven harder).
- `vinyl`: true adds crackle and hiss while the song plays.
- `pads`: settings for any of the sixteen pads, by number `"1"` to `"16"` or
  by the name of its kit sound. Pad 1 is at the bottom left, as on an MPC.
  The kit, pad by pad: 1 `boom`, 2 `knock` (kicks), 3 `crack`, 4 `dusty`
  (snares), 5 `rim`, 6 `clap`, 7 `hat`, 8 `openhat`, 9 `shaker`, 10 `tom`,
  11 `crash`, 12 `clang` (a sword), 13 `stab` (a minor piano chord), 14
  `bell`, 15 `bass` (a sub note on C2), 16 `horn`.
  - `sample`: an audio file (WAV, FLAC, MP3, anything ffmpeg reads), relative
    to the song file or absolute, played instead of the kit sound. Up to 120
    seconds are loaded.
  - `start`, `end`: in seconds, the chop of the sample the pad plays. Leave
    them out to play the whole sample.
  - `tune`: semitones, -24 to 24. Pitch and speed change together, as on an
    MPC: -12 plays an octave down at half speed. Tune copies of the bass pad
    to play a bassline.
  - `volume`: 0 to 1. Default 0.8.
  - `group`: a mute group, 1 to 4; a pad cuts off the others in its group.
    The hat and open hat are in group 1. 0 is none.
  - `mute`, `solo`: true or false.
- `patterns`: patterns `A` to `H`, each one bar of sixteen sixteenth notes,
  a row per pad, keyed like `pads`. Leave out a pattern or a pad to leave it
  empty. A row is 16 characters, one a step: `.` is silent, `x` a hit, `X` an
  accent, `o` a soft hit, or `1` to `4` for the level. Spaces and `|` are
  ignored, so `"x...|x...|x...|x..."` works.
- `song`: the patterns to play in order, one bar each. Empty, the song is
  pattern A once.

`dole check song.dole` reports any problem; `dole render song.dole -o song.wav`
writes the song as a 48 kHz 16-bit stereo WAV, grit and vinyl included.
