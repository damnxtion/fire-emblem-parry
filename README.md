# Fire Emblem: The Binding Blade — Reflex Defense

A real-time dodge/parry battle system for FE6 (Japanese ROM `AFEJ` with the English fan translation).

## How battles work

- **Hit chance is gone.** Every blow lands (the HUD shows HIT 100), unless you parry or dodge it.
- **When an enemy is about to strike one of your units,** the battle scene gently zooms in during the
  wind-up. It reaches full zoom exactly when the dodge window opens and holds there for the whole window.
  - **B while fully zoomed — Dodge:** the blow misses.
  - **A in the middle of the full zoom — Parry:** the blow misses and your next blow hits harder (+50% of the damage margin).
  - **Too early, too late or no input:** the blow lands.
- **Unique bosses** vary the wind-up: Rush (fast zoom), Delayed (slow zoom), Combo (three zoom pulses),
  Feint (the zoom starts building, then backs off) and Flurry (two quick pulses, then a slow one).
- With battle animations off, the challenge plays on the map with a text box instead.

## Timing windows (60 fps)

| Difficulty | Parry | Dodge = full-zoom hold |
|---|---|---|
| Story | 11 frames | 20 frames |
| Expeditioner | 8 frames | 14 frames |
| Expert | 5 frames (~83 ms) | 9 frames (150 ms) |

Modeled on Clair Obscur: Expedition 33, whose exact windows aren't published, so these are estimates.

## About the zoom

The GBA can only scale sprites, not the tile layers the battle background and platform are drawn on.
Fighters are scaled in both directions with affine sprites; the background and platform are stretched
vertically to match, line by line from the HBlank interrupt. The HUD stays fixed. Maximum zoom is +6.25%.
The zoom is computed two frames ahead so what is on screen lines up with when a press is read.

## Applying

Apply `patches/Reflex Defense (Expert).ips` to your own copy of the translated ROM. No ROM files are included.

Config at ROM offset `0xF00000` (`RFX1` magic): byte +4 = difficulty (0 Story, 1 Expeditioner, 2 Expert),
+5 = enabled, +6 = every enemy uses boss techniques.

## Building

1. Clone the [FE6 decompilation](https://github.com/StanHash/fe6), install agbcc, place your ROM as `fe6-base.gba`.
2. Copy `src/*` into the decomp's `src/` (and `hack_parry_str.h` into `include/`), apply `tools/decomp-build.diff`.
3. `make`, then `python3 tools/patch.py fe6-base.gba fe6.gba fe6.elf out.gba [difficulty]`.
