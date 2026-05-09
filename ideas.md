# Project ideas

Open ideas and drafts that aren't ready for a plan yet. Move things to
`docs/superpowers/specs/` once they're scoped to implement.

---

## Add 15 new ASCII pets (draft, 2026-05-09)

**Status:** sketches drafted, awaiting decision on which to spec out fully.

**Goal:** grow the firmware's ASCII pet roster from 20 → 35.

**Selected list (Kayla's pick + 2 bonus):**

| # | Name | Theme |
|---|---|---|
| 1 | `claude` | self-portrait, wispy thought-cloud |
| 2 | `sloth` | comedy of slow |
| 3 | `terminal` | CRT/cursor, fits CC theme |
| 4 | `duck` (real) | alert, paddling |
| 5 | `rubberduck` | debug-buddy |
| 6 | `trex` | tiny arms, big drama |
| 7 | `alien` | UFO antenna twitch |
| 8 | `cthulhu` | tentacle face |
| 9 | `slime` | RPG jiggle |
| 10 | `whale` | chonk + spout |
| 11 | `jellyfish` | drift + pulse |
| 12 | `crab` | sideways scuttle |
| 13 | `frog` | wide eyes, tongue |
| 14 | `bmo` | game console (Adventure Time) |
| 15 | `meeseeks` | eager blue helper (Rick and Morty) |

### Frame format

12 chars wide × 5 rows tall (matches `src/buddies/cat.cpp`). Each pet
needs all seven states: `sleep`, `idle`, `busy`, `attention`, `celebrate`,
`dizzy`, `heart`. Below are draft idle + busy silhouettes only — the
remaining five states still need to be designed when speccing each pet.

### Drafts (idle + busy, 12×5)

**1. claude** — self-portrait. Soft cloud-mind, wispy edges, warm curious
eyes, trailing thought-wisp. Eyes go from `o` to `O` when working.
Particles around the head when busy = sentences-in-progress.

```
Idle:                      Busy:
                              . , .
   ,~~~~,                     ,~~~~,
  ( o  o )                   ( O  O )
   '~~~~'~,                   '~~~~'~,
        ` `                    .   ` `
```

**2. sloth** — hangs from a limb. Comedy is that "busy" looks almost
identical to "idle" — just one eyebrow raised, a leaf falling.

```
Idle:                      Busy:
   ===|==                     ===|==
   /-..-\                     /^..-\
  ( o    o )                 ( O    o )
   \  ww  /                   \  ww  /
    \____/                     \____/.
```

**3. terminal** — CRT bezel, blinking `_` cursor. Idle shows a `claude>`
prompt with a soft cursor blink; busy streams `hello! / world.` lines.

```
Idle:                      Busy:
   ________                   ________
  | claude> |                | hello! |
  |    _    |                | world. |
  |________|                 | _      |
   `------`                   `------`
```

**4. duck** (real) — alert, paddling.

```
Idle:                      Busy:

    ___                        ___
   ( o \_=                    ( o \=
   /----\                     />v--\<
   ~~~~~~~                    ~v~~v~~
```

**5. rubberduck** — debug-buddy, squeaks when finding bugs.

```
Idle:                      Busy:
                              *!*
    ___                        ___
   ( o \_=                    ( O \=
    `---`                      '---'
   ~~~~~~~                    ~~v~~v~
```

**6. trex** — tiny arms, big drama. Heart state is going to be
incredible (tiny arms can't reach to make a heart).

```
Idle:                      Busy:
   ___                        ___
  /  o\__                    /  O\__
   |  ===                     |  ===
   |    )                     |->  )
  /\\__/\                    ^\\__/\^
```

**7. alien** — UFO with antennae, beam down for celebrate.

```
Idle:                      Busy:
   ,    ,                     ,    ,
   |    |                     |    |
  .------.                   .------.
 ( O    O )                 ( O -- O )
  /========\                |||/====\|||
```

**8. cthulhu** — tentacle face. Brooding idle, writhing busy.

```
Idle:                      Busy:
   _,_____,_                  _,_____,_
  / @     @ \                / @     @ \
  \    -    /                \    O    /
   '|v|v|v|'                  '~vVvVv~'
   ' V V V '                  /v V v v\
```

**9. slime** — RPG-style. Jiggle is the entire personality.

```
Idle:                      Busy:


   .------.                   .------.
  ( o    o )                 ( ^    ^ )
   `~----~`                   `~--__~`o
```

**10. whale** — chonky friendly. Idle = drift; busy = big spout +
motion lines.

```
Idle:                      Busy:
   .  .  .                   .|.|.|.|.
   .--------.                 .--------.
  /  o     /|                /  O     /|
  \_______V |                \___v___V |
   `~~~~~~`                  ~`~~~~~~`~
```

**11. jellyfish** — pulsing tendrils.

```
Idle:                      Busy:

   .-----.                    .-----.
  ( o   o )                  / o   o \
   `-----'                  (    -    )
   ~ | ~ |                    `-----'
                              ~ | | ~
```

**12. crab** — sideways. Claws snap on busy.

```
Idle:                      Busy:
   ,-, ,-,                    ,-, ,-,
   o     o                    O     O
 X(=======)X                ><(=======)><
   /v   v\                    /v_  _v\
  /       \                  /  v  v  \
```

**13. frog** — smug, tongue-snap on busy.

```
Idle:                      Busy:

   .--..--.                   .--..--.
  ( O    O )                 ( O \\ O )
   \  ww  /                   \  WW  /
    `----`                     `----`*
```

**14. bmo** — game console. `==--==` is the button panel; legs are
the stick `/|` and `|\`.

```
Idle:                      Busy:
   .------.                   .------.
  | o    o |                 | <    > |
  |   __   |                 |  o>=<  |
  |==--====|                 |==--====|
   /|    |\                   /|    |\
```

**15. meeseeks** — eager blue helper. Idle hands clasped; busy hands
thrown wide ("CAN DO!"). The `-O-` middle row is the open mouth.

```
Idle:                      Busy:
                              \ . . /
   .-----.                    .-----.
   |o   O|                    |O   O|
   | -O- |                    | -O- |
   '-----'                    /|/-\|\
   /| | |\
```

### What still needs designing per pet (for full implementation)

- Five remaining states each: `sleep`, `attention`, `celebrate`, `dizzy`,
  `heart`. The existing `src/buddies/cat.cpp` is the reference for how
  the seven states layer together with their sequence arrays and color
  hex codes.
- Color choice (`uint16_t` 565 colors, used in `buddyPrintSprite(..., COLOR)`).
- Per-state animation timing (`(t/N) % len(SEQ)` cadence).

### Wiring tasks (not per-pet)

- `src/buddy.cpp` — add 15 new species to the dispatch table (lines that
  fan out `doSleep` / `doIdle` / etc. by species index).
- `src/buddy.h` — add the 15 new species enum entries.
- Settings menu — bump the species counter ceiling so the cycler reaches
  the new entries; existing NVS-backed selection in `stats.h` should
  carry over.
- `platformio.ini` — confirm the firmware still fits the 3 MB app
  partition after adding 15 × ~100 lines of pet code.

### Implementation strategy options

**A. One-at-a-time, review each silhouette**
Pick a single pet, draft the full `src/buddies/<name>.cpp` matching
`cat.cpp` structure, review the silhouette, commit, repeat. ~15 commits.
Slower but every pet gets eyeballed before landing.

**B. Batch via parallel subagents**
Dispatch subagents (one per pet, or grouped) to draft the full files
concurrently. Faster wall clock; needs a careful review pass at the end
because frames will land in bulk.

**C. Outsource the tedious frames**
Spec only the silhouette + state intent for each pet, then have an agent
fill in the five remaining states using the existing pets as style
reference. Lowest effort per pet; quality varies.

Recommendation: **A** for `claude` (self-portrait — should feel intentional)
and **B** for the rest, with a final pass to ensure the existing pets'
visual coherence is maintained.

### Open questions

- Are any of these culturally / IP-sensitive enough to skip?
  - **bmo** (Cartoon Network / Adventure Time)
  - **meeseeks** (Adult Swim / Rick and Morty)
  - **cthulhu** (public domain)
  - **trex** (generic)

  The upstream firmware ships pets like `doge` and `llama` (cherry-picked
  from a community fork), so there's precedent for character-inspired
  pets, but BMO and Meeseeks specifically are trademarked. Worth deciding
  whether to keep them generic ("game console", "blue helper") or lean in.

- Should `claude` be the default species on first boot? Currently `cat`
  is upstream's default. Changing the default has personality implications.

- Storage: 15 × ~100 lines × ~30 bytes/line ≈ 45 KB of new code. Plus the
  generated string literals. Should fit comfortably in the 3 MB app
  partition but worth confirming with `pio run` after the first 2-3 land.
