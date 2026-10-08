# termtris

A small, faithful Tetris for the terminal: one C file, no libraries beyond
libc, a ~40 KB binary that uses about 2 MB of memory. Linux only: the music
and input handling use Linux-specific calls.

```sh
make            # build
make test       # rules and input-parser tests
make install    # copies to ~/.local/bin (PREFIX=... to change)
```

`termtris --version` prints the version.

## Rules

It plays like the 1989 NES game, drawn like the 1984 original:

- 10×20 board, 7 tetrominoes, next-piece preview
- Nintendo rotation: pieces turn about a fixed pivot, no wall kicks
- NES gravity table (48 frames per row at level 0 down to 1 at level 29+)
- A piece locks as soon as it can't fall: no lock delay
- NES randomizer (reroll once on a repeat)
- Lines score 40 / 100 / 300 / 1200 × (level + 1); soft drop scores 1 per row,
  hard drop 2 per row
- A new level every 10 lines; pick a start level from 0 to 9
- Holding down never carries into the next piece; press it again

The ghost piece is off by default, as in the originals.

## Look and sound

- The well is drawn exactly as the 1984 original drew it (`<! . . !>`,
  `<!====!>`, `\/\/\/`), with no other decoration. The grid dots are muted.
- Bricks take their colours from your terminal theme. Where the terminal reports
  its palette (kitty, Ghostty, foot, WezTerm, Alacritty), bricks get a faint
  bevel, lit from the top left, and L becomes an orange mixed from the theme's
  red and yellow. Elsewhere they are flat theme colours. Press `c` for the
  monochrome `[]` look of the original.
- Music: Korobeiniki, the folk song the Game Boy version made the Tetris theme.
  The folk song is only eight bars, so termtris plays it four times, arranged
  differently each pass, before looping (about 51 seconds): the plain tune, a
  second voice in thirds over a walking bass, an echo an eighth behind, and the
  melody up an octave over the harmony. The arrangement is termtris's own; the
  Game Boy's slow middle section is Nintendo's composition and isn't included.
  The notes are stored as MIDI note numbers and played by a tiny built-in
  pulse-and-triangle synth (Game Boy style) through `pw-play`, `paplay` or
  `aplay`, whichever is installed. It plays only during a game, restarts with
  each new game, and stops on pause. `m` toggles it; `--no-music` starts muted.

## Keys

| Key | Action |
| --- | --- |
| ← → or h l | move |
| ↑ x k | rotate clockwise |
| z | rotate counter-clockwise |
| ↓ j | soft drop |
| space | hard drop |
| p or Esc | pause (the board hides) |
| g / n / c | ghost / next preview / classic `[]` style |
| m | music on/off |
| r | back to the title after game over |
| q | quit |

## Why it feels smooth

- Logic runs on a fixed 60 Hz clock, the same frame timing as the NES.
- Each frame sends only the cells that changed, wrapped in synchronized-output
  markers so the terminal never shows half a frame.
- In terminals that support the kitty keyboard protocol (kitty, Ghostty, foot,
  WezTerm, recent Alacritty), termtris gets key releases and runs its own
  auto-shift: first repeat after 10 frames, then one move every 2 frames. Holding
  a direction no longer depends on your OS key-repeat settings, and a held
  direction stays charged through line clears. The title screen shows
  `precise keys` when this is active.
- Other terminals (and tmux) fall back to plain key presses (`basic keys`).
  Run with `--basic-keys` to force that.
- Losing window focus pauses the game.

The high score is kept in `$XDG_DATA_HOME/termtris/highscore`
(`~/.local/share/termtris/highscore` by default).

## License

MIT, see [LICENSE](LICENSE). Korobeiniki is a 19th-century folk song in the public domain; the
arrangement in `termtris.c` is part of this project and covered by the same licence.
