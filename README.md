# Fire Emblem: The Binding Blade — Reflex Defense

A real-time dodge/parry system for FE6 (Japanese ROM `AFEJ` with the English fan translation).

When an enemy is about to hit one of your units, a short timing challenge plays on the map:
cue beats, a wind-up sound, then a white flash on impact.

- **A on the flash — Parry:** the hit misses, and your next strike in that fight can't miss and deals about 50% more damage.
- **B on the flash — Dodge:** the hit misses (wider window than parry).
- **Too early or no input:** the enemy attacks with its normal hit chance.

Unique bosses pick from varied attacks: Rush, Delayed, Combo (3 hits), Feint, and Flurry.

## Timing windows (60 fps)

| Difficulty | Parry | Dodge |
|---|---|---|
| Story | 11 frames | 20 frames |
| Expeditioner (default) | 8 frames (~133 ms) | 14 frames (~233 ms) |
| Expert | 5 frames | 9 frames |

Modeled on Clair Obscur: Expedition 33. Its exact windows aren't published, so these are estimates;
Story is +40% as in that game's 1.3.0 update.

## Applying

Apply `patches/Reflex Defense (Expeditioner).ips` to your own copy of the translated ROM.
No ROM files are included in this repository.

Config lives at ROM offset `0xF00000` (`RFX1` magic): byte +4 = difficulty (0 Story, 1 Expeditioner, 2 Expert),
+5 = enabled, +6 = every enemy uses boss techniques.

## Building

1. Clone the [FE6 decompilation](https://github.com/StanHash/fe6), install agbcc, place your ROM as `fe6-base.gba`.
2. Copy `src/*` into the decomp's `src/`, apply `tools/decomp-build.diff`.
3. `make`, then `python3 tools/patch.py fe6-base.gba fe6.gba fe6.elf out.gba [difficulty]`.

`tools/mkstr.py` regenerates the banner strings in the translation patch's glyph encoding.
