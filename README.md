# Fire Emblem: The Binding Blade — Reflex Defense

A real-time dodge/parry battle system for FE6 (Japanese ROM `AFEJ` with the English fan translation).

## How battles work

- **Hit chance is gone.** Every blow lands (the HUD shows HIT 100), unless you parry or dodge it.
- **Defend live, during the enemy's swing.** The windows sit on the frame the enemy's weapon actually
  lands, read from the attack animation itself. If you haven't pressed yet, the swing pauses for a moment
  at the point of impact (hit-stop) so the late part of the window stays fair.
  - **A as the weapon lands — Parry (9 frames, 0.15 s):** the blow misses with a spark and **PARRY!**,
    and your unit hits straight back (**COUNTER!**) for +50% of its damage margin.
  - **B around the impact — Dodge (wider window):** the blow misses and your unit steps back.
  - **Too early, too late or no input:** the blow lands.
- **Swings depend on the weapon, not the enemy.** Every weapon type has its own style, and it changes
  with the weapon's rank (E to S): 42 styles in all.
- With battle animations off, the challenge plays on the map with a text box instead.

## Timing overlay (dev tool)

A small overlay sits in the strip of sky between the two name tags at the top of the battle scene,
clear of the fighters. Press **SELECT** during a battle to hide or show it.

- **Timeline:** grey run-up, blue dodge zone, yellow parry zone and a white impact line, at 2 pixels per
  frame. A white marker slides along it during the enemy's swing and crosses the impact line on the
  frame the weapon lands.
- **Countdown:** frames until impact, e.g. `+12F`, with an orange `H` while the swing is held mid-way.
- **Last verdict:** `P` parry, `D` dodge, `X` failed, followed by how far from impact you pressed
  (`+2F` = 2 frames early, `-3F` = 3 frames late; no number means no press).

## Swing styles

| Weapon | Style | How rank changes it |
|---|---|---|
| Sword | Quick and precise | D+ swings at double speed; C+ adds short hitches; A/S feint twice |
| Lance | Steady, pauses before the thrust | Longer pauses with rank; B/S pause twice; S is fast |
| Axe | Heavy wind-up high in the swing | Longer wind-ups; B/S add a second hesitation |
| Bow | Holds the draw before release | Longer draws; B/S release faster; S double-draws |
| Anima | Fast casts | Higher ranks gather power before releasing |
| Light | Slow, long charge | Longer charges; B/S charge twice |
| Dark | Erratic, stuttering casts | A stutters three times; D/B/S cast at double speed |

Timings are in `sSwingProfiles` in `src/hack_parry.c`. Whatever the style, the final approach to
impact always runs frame by frame, so every frame of the windows exists.

## Timing windows (60 fps)

| Difficulty | Parry | Dodge |
|---|---|---|
| Story | 9 frames (0.15 s) | 21 frames |
| Expeditioner | 9 frames (0.15 s) | 17 frames |
| Expert | 9 frames (0.15 s) | 15 frames |

Buttons are sampled at every VBlank (60 times a second, even when the game drops a frame) and each
press is judged in real time against the frame contact actually happens, using what was on screen
when you pressed. So the windows are exactly 9 and 15 frames regardless of slowdown. The
counterattack after a parry is only added when the battle record (7 blows) has room for it.

## Applying

Apply `patches/Reflex Defense (Expert).ips` to your own copy of the translated ROM. No ROM files are included.

Config at ROM offset `0xF00000` (`RFX1` magic): byte +4 = difficulty (0 Story, 1 Expeditioner, 2 Expert),
+5 = enabled.

## Building

1. Run `tools/mkgfx.py` and `tools/mkstr.py` to regenerate the headers if you change art or text.
2. Clone the [FE6 decompilation](https://github.com/StanHash/fe6), install agbcc, place your ROM as `fe6-base.gba`.
3. Copy `src/*` into the decomp's `src/` (and the `hack_parry_*.h` headers into `include/`), apply `tools/decomp-build.diff`.
4. `make`, then `python3 tools/patch.py fe6-base.gba fe6.gba fe6.elf out.gba [difficulty]`.
