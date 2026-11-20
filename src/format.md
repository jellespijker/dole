# The .dole song format

A dole song is one JSON file. Every field but `"dole"` is optional.

```json
{
  "dole": 3,
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
      "7":  { "steps": "x.o.x.o.x.o.x.oo", "roll": { "16": 3 }, "chance": { "14": 50 } },
      "13": { "steps": "x..........x....", "lanes": { "filter": { "1": -0.8, "9": -0.2 } } }
    },
    "B": { "length": 32, "1": "X......x..x.....X......x..x...x." }
  },
  "song": ["A", "A", "A", "B"]
}
```

- `dole`: 3. Files from earlier versions still open.
- `bpm`: tempo, 40 to 240. Default 90.
- `swing`: 50 (straight) to 75. Every second sixteenth is pushed late; 58 is
  a lazy boom-bap feel, 66 a triplet shuffle. Default 50.
- `grit`: the master's character: `"clean"`, `"12-bit"` (an SP-1200's 26 kHz
  and 12 bits), or `"crushed"` (16 kHz, 7 bits, driven harder).
- `vinyl`: true adds crackle and hiss while the song plays.
- `pads`: settings for any of the 64 pads, in four banks of sixteen: by
  number, `"1"` to `"64"`, by bank and number, `"A1"` to `"D16"` (`"B1"` is
  pad 17), or, in bank A, by the name of its kit sound. Pad 1 is at the
  bottom left, as on an MPC. Banks B to D start empty, for samples and
  synths. Bank A's kit, pad by pad: 1 `boom`, 2 `knock` (kicks), 3 `crack`, 4 `dusty`
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
  - `pan`: -1 (left) to 1 (right). Default 0.
  - `filter`: -1 to 1. Below 0 a low-pass closing from 20 kHz to 60 Hz at
    -1; above 0 a high-pass opening from 20 Hz to 8 kHz at 1. Default 0, open.
  - `resonance`: 0 to 1, the filter's peak.
  - `attack`: 0 to 1, how slowly each hit fades in, up to half a second.
  - `decay`: 0 to 1, how soon each hit dies away: 0 is a click, 0.5 about a
    second, 1 (the default) lets it ring.
  - `reverse`: true plays the sample backwards.
  - `gate`: true stops a sample after `length` steps (or, played live, when
    the pad is let go) instead of letting it play out.
  - `group`: a choke group, 1 to 4; a pad cuts off the others in its group.
    The hat and open hat are in group 1. 0 is none.
  - `mute`, `solo`: true or false.
  - `synth`: a plugin that plays the pad instead of its sound (see Plugins).
    The pad's steps play notes on it, middle C moved by `tune`.
  - `length`: how many steps a synth pad's note, or a gated sample, is held:
    1, 2, 4, 8, or 16.
  - `swing`: the pad's own swing, 50 to 75, instead of the song's.
  - `loop`: the pad's own loop, 1 to 64 steps: it plays the first that many
    steps of its row over and over, counted from when the pattern came in,
    so a `loop` of 3 against a 16-step bar makes a polyrhythm. Left out, the
    pad loops with the pattern.
  - `effects`: up to four plugins the pad's sound runs through, in order.
- `master`: up to four plugins the whole mix runs through, before the grit.
- `patterns`: patterns `A` to `H`, a row per pad, keyed like `pads`. Leave
  out a pattern or a pad to leave it empty.
  - `length`: 16, 32, 48, or 64 steps: one to four bars. Default 16.
  - A row is a string of one character a step, as many as the pattern's
    length: `.` is silent, `x` a hit, `X` an accent, `o` a soft hit, or `1`
    to `4` for the level. Spaces and `|` are ignored, so
    `"x...|x...|x...|x..."` works.
  - A row with more to it is an object: `steps`, the string, and any of
    these, each keyed by step number from 1:
    - `roll`: 2 to 4 hits within the step.
    - `chance`: 0 to 100, the percent of times round the step plays. The
      same times every time, so a render matches what was heard.
    - `nudge`: -50 to 50, the percent of a step it plays early or late.
    - `lanes`: settings that move as the pattern plays. A value set on a
      step holds until the next one set; before the first, the pad's own
      setting plays. The lanes: `pitch` (semitones, -24 to 24, on top of
      `tune`; a sample still sounding bends with it), `volume`, `pan`,
      `filter`, `resonance`, `decay`, a synth's parameter as
      `synth.<parameter id>`, and an effect's as `fx<n>.<parameter id>`, `n`
      counting the pad's effects from 1: `"fx1.drive": {"1": 0.2, "9": 0.9}`.
      A plugin's lane takes the plugin's own range; when playing stops, the
      plugin goes back to its setting in the song.
- `song`: the patterns to play in order, each for its length. Empty, the
  song is pattern A once.

## Plugins

A plugin is `{"plugin": "<id>", "params": {"<id>": value}, "bypass": false}`,
with `"state"` (base64) for plugins that keep their own. `dole plugins`
lists every id on the machine. dole's own effects, which need nothing
installed, and their parameters:

- `overdrive`: `drive` 0–1, `fuzz` 0–1 (a hard, lopsided clip), `tone`
  300–16000 Hz, `mix` 0–1.
- `bitcrush`: `bits` 1–16, `rate` 300–48000 Hz, `mix` 0–1.
- `ringmod`: `freq` 20–4000 Hz, `shape` 0 (sine) to 1 (square), `mix` 0–1.
- `comb`: a metallic resonator; `freq` 30–3000 Hz, `feedback` 0–1, `damp`
  0–1, `mix` 0–1.
- `delay`: `time` 0.01–1.5 s, `feedback` 0–1.2 (past 1 it runs away into
  noise, held in by saturation), `drive` 0–1, `tone` 300–16000 Hz, `mix` 0–1.
- `stutter`: gates in time with the song; `rate` 0 (1/4) to 3 (1/32),
  `duty` 0–1, `depth` 0–1.

CLAP plugins are `clap:<plugin id>` (`clap:org.surge-synth-team.surge-xt`),
their parameters numbered as the plugin numbers them; LV2 plugins are
`lv2:<uri>`, their parameters named by port symbol.

`dole check song.dole` reports any problem, including a plugin that isn't
installed; `dole render song.dole -o song.wav`
writes the song as a 48 kHz 16-bit stereo WAV, grit and vinyl included.
