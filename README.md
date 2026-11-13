# Fire Emblem: The Binding Blade — Reflex Defense

A real-time dodge/parry battle system for FE6 (Japanese ROM `AFEJ` with the English fan translation).

## How battles work

- **Hit chance is gone.** Every blow lands (the HUD shows HIT 100), unless you parry or dodge it.
- **Enemy attacks play live.** While the enemy swings, the battle scene gently zooms in. The zoom is
  driven by the attack animation itself: it reaches full zoom when the dodge window opens and is centred
  on the frame the weapon actually lands. If you haven't pressed yet, the swing pauses for a moment
  at the point of impact (a short hit-stop) so the late part of the window stays fair.
  - **B while fully zoomed — Dodge:** the blow misses and your unit steps back out of the way.
  - **A right as the weapon lands — Parry:** the blow misses with a spark and **PARRY!**, and your unit
    immediately hits back (**COUNTER!**) for +50% of its damage margin.
  - **Too early, too late or no input:** the blow lands.
- **Unique bosses** vary their swings: Rush (double-speed swing), Delayed and Long Delay (the swing
  freezes mid-way before striking) and Feint (the zoom swells early, then backs off, before the real blow).
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
Presses are judged against what was on screen when you pressed (two frames earlier than the game is
computing), so full zoom on screen is exactly the dodge window. The counterattack after a parry is only
added when the battle record (7 blows) is guaranteed to have room for it.

## Applying

Apply `patches/Reflex Defense (Expert).ips` to your own copy of the translated ROM. No ROM files are included.

Config at ROM offset `0xF00000` (`RFX1` magic): byte +4 = difficulty (0 Story, 1 Expeditioner, 2 Expert),
+5 = enabled, +6 = every enemy uses boss techniques.

## Building

1. Run `tools/mkgfx.py` and `tools/mkstr.py` to regenerate the headers if you change art or text.
2. Clone the [FE6 decompilation](https://github.com/StanHash/fe6), install agbcc, place your ROM as `fe6-base.gba`.
3. Copy `src/*` into the decomp's `src/` (and the `hack_parry_*.h` headers into `include/`), apply `tools/decomp-build.diff`.
4. `make`, then `python3 tools/patch.py fe6-base.gba fe6.gba fe6.elf out.gba [difficulty]`.
