# Dole

![Dole playing a song](screenshot.png)

Dole is a loop machine for drums and a bassline. You get eight tracks with
sixteen steps each. One-bar patterns A to H chain into a song, and the result
exports to WAV. It's made for minimal, repetitive grooves: a hard kick and
snare, a hi-hat, and a bass riff going round, with room left for a voice on
top. Record the voice in your recorder of choice and mix it over the export.

Every sound is built in: kick, snare, hat, open hat, clap, rim, cowbell, and a
mono bass synth. Any track can load a sample instead (WAV, FLAC, MP3, anything
`ffmpeg` reads). A chopped loop on step 1 plays through the bar, and a sample on
the bass track is pitched by its notes. Swing, accents, and bass notes held
across steps are all there. Dole follows the Omarchy theme and language.

```
dole open [song.dole]
```

## Shortcuts

| Keys | Action |
| --- | --- |
| Space | Play or stop |
| P | Play the pattern being edited, or the song |
| ← → ↑ ↓, h j k l | Move between steps |
| Enter, x | Turn a step on or off |
| Shift+Enter | Hold the bass note through a step |
| Delete | Clear a step |
| Shift+↑ ↓ | Louder or softer; a bass note up or down |
| Ctrl+Shift+↑ ↓ | A bass note up or down an octave |
| 1 – 8 | Edit pattern A to H (it takes over at the next bar) |
| Ctrl+C, Ctrl+V | Copy and paste a pattern |
| Ctrl+Delete | Clear the pattern |
| [ ] and { } | Tempo down or up, by 1 or 10 |
| , . | Less or more swing |
| M, S | Mute or solo the track |
| Shift+← → | Track volume |
| O | Load a sample onto the track |
| Shift+O | Back to the built-in sound |
| Tab | Switch between the steps and the song |
| 1 – 8 (song) | Set the slot, or add one at the end |
| I, Delete (song) | Insert or remove a slot |
| Enter (song) | Edit the slot's pattern |
| Ctrl+Z, Ctrl+Shift+Z | Undo or redo |
| Ctrl+N, Ctrl+O | New or open a song |
| Ctrl+S, Ctrl+Shift+S | Save, or save as |
| Ctrl+E | Export the song as WAV |
| ? | Show or hide the shortcuts |
| Q | Quit |

You can also use the mouse. Click a step to turn it on, right-click to clear it
(on the bass, to hold), and scroll over a step to change its level or note.
Scroll over a track to change its volume, and click its sound to load a sample.

## Songs are text

A song is a small JSON file that you, a script, or an agent can write:

```json
{
  "dole": 1, "bpm": 92, "swing": 54,
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

`dole help format` describes the whole format. These commands need no display;
each takes `--json` and exits 0 or 1:

| Command | |
| --- | --- |
| `dole new song.dole` | Start a song from a short groove (`--empty` for none) |
| `dole check song.dole` | Report every problem, and the tempo, patterns, and length |
| `dole render song.dole -o song.wav` | Render the song (`--pattern B --bars 4` for one pattern) |
| `dole skill install` | Install the agent skill for Claude Code and Codex |

## Building

Dole uses Qt 6 (base, declarative, multimedia, svg, wayland) and `ffmpeg`, all
of which come with Omarchy.

```
bin/build                                # build into build/
bin/test                                 # build, then run the tests offscreen
cmake --install build --prefix ~/.local  # add it to the app launcher
```

## License

MIT. The theme, translator, file picker, and command line follow Omacut,
Hype, and the other Omarchy apps by David Heinemeier Hansson.
