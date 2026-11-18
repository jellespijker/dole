# The .dole song format

A dole song is one JSON file. Every field but `"dole"` is optional.

```json
{
  "dole": 1,
  "bpm": 92,
  "swing": 54,
  "tracks": {
    "kick": { "volume": 0.8 },
    "cowbell": { "sample": "samples/shout.wav", "volume": 0.6, "mute": false, "solo": false }
  },
  "patterns": {
    "A": {
      "kick":  "X.....x...X.....",
      "snare": "....X.......X...",
      "hat":   "x.o.x.o.x.o.x.o.",
      "bass":  "C2 - - - . . C2 . D#2 - . F2 . . G1 -"
    }
  },
  "song": ["A", "A", "B", "A"]
}
```

- `dole`: always 1.
- `bpm`: tempo, 40 to 240. Default 96.
- `swing`: 50 (straight) to 75. Every second sixteenth is pushed late; 66 is a
  triplet shuffle. Default 50.
- `tracks`: settings for any of the eight tracks, by id: `kick`, `snare`, `hat`,
  `openhat`, `clap`, `rim`, `cowbell`, `bass`.
  - `sample`: an audio file (WAV, FLAC, MP3, OGG, anything ffmpeg reads),
    relative to the song file or absolute, played instead of the built-in
    sound. A drum track plays it from the start on each step and cuts it off
    at its next step, so a chopped loop on step 1 plays through the bar. On
    the bass track it is pitched from C2. Up to 120 seconds are loaded.
  - `volume`: 0 to 1. Default 0.8.
  - `mute`, `solo`: true or false.
- `patterns`: patterns `A` to `H`, each one bar of sixteen sixteenth notes.
  Leave out a pattern or a track to leave it empty.
  - A drum row is 16 characters, one a step: `.` is silent, `x` a hit, `X` an
    accent, `o` a soft hit, or `1` to `4` for the level (soft to accent).
    Spaces and `|` are ignored, so `"x...|x...|x...|x..."` works.
  - The bass row is 16 words: a note from `C1` to `C4` (`C2`, `D#1`, `Eb2`),
    `-` to hold the note before it through the step, or `.` for silence.
  - The closed `hat` cuts off the `openhat`.
- `song`: the patterns to play in order, one bar each. Empty, the song is
  pattern A once.

`dole check song.dole` reports any problem; `dole render song.dole -o song.wav`
writes the song as a 48 kHz 16-bit stereo WAV.
