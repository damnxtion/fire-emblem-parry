// Real-time Dodge / Parry system for FE6 ("Reflex Defense")
// Every time an enemy strikes one of your units, a short timing challenge
// plays before the battle is resolved. Press A on the impact flash to PARRY,
// or B to DODGE. Unique bosses use varied attack patterns (rush, delayed,
// feints, combos). Timing windows follow Clair Obscur: Expedition 33 style
// difficulty tiers (Story / Expeditioner / Expert).

#include "prelude.h"
#include "proc.h"
#include "hardware.h"
#include "gbaio.h"
#include "random.h"
#include "sound.h"
#include "m4a.h"
#include "text.h"
#include "ui.h"
#include "unit.h"
#include "faction.h"
#include "battle.h"
#include "action.h"
#include "ai_decide.h"
#include "item.h"
#include "mu.h"
#include "manim.h"
#include "map.h"
#include "banim.h"
#include "banim_sprite.h"
#include "banim_ekrbattle.h"
#include "arena.h"
#include "armfunc.h"
#include "oam.h"
#include "bm.h"
#include "gba/gba.h"

#include "hack_parry_str.h"

// ---- external (non-header) game symbols ----
extern struct ProcScr CONST_DATA ProcScr_CombatAction[];

// ---- configuration (patchable in ROM) ----

struct ParryWindow
{
    u8 parry_early, parry_late; // frames before / after impact
    u8 dodge_early, dodge_late;
};

struct ParryConfig
{
    u8 magic[4];       // "RFX1"
    u8 difficulty;     // 0 = Story, 1 = Expeditioner, 2 = Expert
    u8 enabled;        // 0 disables the system
    u8 all_boss;       // 1: every enemy uses boss techniques
    u8 pad;
    struct ParryWindow windows[3];
};

// Expeditioner: parry 8f (~133 ms), dodge 14f (~233 ms).
// Story: both windows +40% (as in E33 patch 1.3.0). Expert: tighter.
struct ParryConfig const gParryConfig __attribute__((section(".hackcfg"))) =
{
    { 'R', 'F', 'X', '1' },
    1, 1, 0, 0,
    {
        { 7, 3, 13, 6 }, // Story        parry 11f / dodge 20f
        { 5, 2,  9, 4 }, // Expeditioner parry  8f / dodge 14f
        { 3, 1,  6, 2 }, // Expert       parry  5f / dodge  9f
        // window = early + impact frame + late
    },
};

// ---- attack patterns ----

enum
{
    EV_END,
    EV_TICK,   // rhythm cue
    EV_WIND,   // wind-up (final cue before an impact)
    EV_HIT,    // impact: must be defended
    EV_FAKE,   // feint wind-up that leads nowhere
    EV_SLOW,   // wind-up of a delayed strike (different sound)
};

struct PatternEv { u8 time; u8 kind; };

#define INTRO_FRAMES 40

static CONST_DATA struct PatternEv PatNormal[] =
{ {0,EV_TICK}, {24,EV_TICK}, {48,EV_WIND}, {72,EV_HIT}, {0,EV_END} };

static CONST_DATA struct PatternEv PatRush[] =
{ {0,EV_TICK}, {14,EV_TICK}, {28,EV_WIND}, {42,EV_HIT}, {0,EV_END} };

static CONST_DATA struct PatternEv PatDelayed[] =
{ {0,EV_TICK}, {24,EV_TICK}, {48,EV_SLOW}, {92,EV_HIT}, {0,EV_END} };

static CONST_DATA struct PatternEv PatCombo[] =
{ {0,EV_TICK}, {24,EV_WIND}, {48,EV_HIT}, {66,EV_HIT}, {84,EV_HIT}, {0,EV_END} };

static CONST_DATA struct PatternEv PatFeint[] =
{ {0,EV_TICK}, {24,EV_FAKE}, {60,EV_WIND}, {84,EV_HIT}, {0,EV_END} };

static CONST_DATA struct PatternEv PatFlurry[] =
{ {0,EV_WIND}, {24,EV_HIT}, {36,EV_HIT}, {48,EV_SLOW}, {88,EV_HIT}, {0,EV_END} };

static CONST_DATA struct PatternEv * CONST_DATA BossPatterns[] =
{ PatRush, PatDelayed, PatCombo, PatFeint, PatFlurry, PatNormal };

#define BOSS_PATTERN_COUNT 6
#define MAX_STRIKES 8
#define MAX_HITS 6

enum { RES_NONE, RES_FAIL, RES_DODGE, RES_PARRY };

enum
{
    ST_PREDICT,
    ST_PLAY,
    ST_RESULT,
    ST_FINISH,
};

struct ParryProc
{
    PROC_HEADER;

    /* 29 */ u8 state;
    /* 2A */ u8 generating;  // TRUE while our battle generation is running
    /* 2B */ u8 n_done;      // number of strikes already decided
    /* 2C */ u8 strike_idx;  // counter used during generation
    /* 2D */ u8 pending;     // a new (undecided) strike exists after n_done
    /* 2E */ u8 pending_boss;
    /* 2F */ u8 riposte;     // parry granted a riposte for next player strike
    /* 30 */ u8 results[MAX_STRIKES];
    /* 38 */ ProcPtr parent;
    /* 3C */ struct Unit * instigator;
    /* 40 */ struct Unit * target;
    /* 44 */ u8 ballista;
    /* 45 */ u8 hit_count;
    /* 46 */ u8 hit_res[MAX_HITS];
    /* 4C */ u16 clock;
    /* 4E */ u16 result_clock;
    /* 50 */ struct PatternEv const * pattern;
    /* 54 */ u16 rand_st[3];
    /* 5A */ u8 next_ev;
    /* 5B */ u8 flash;
    /* 5C */ u8 shown_res;
    /* 5D */ u8 mode;
    /* 5E */ u8 in_scene;    // QTE is being shown inside the battle scene
    /* 5F */ u8 shown;       // hits already played in the battle scene
    /* 60 */ u8 total;       // hits in the latest generation pass
    /* 61 */ u8 pending_hit; // hit index of the pending (undecided) enemy strike
    /* 62 */ u8 seg_ready;   // the current scene segment has been prepared
    /* 63 */ u8 final_done;  // the final (real) generation pass has run
    /* 64 */ u16 saved_bldcnt;
    /* 66 */ u8 saved_coef_a, saved_coef_b, saved_y;
    /* 69 */ u8 allow_tint;  // scene: colour pulses only for the verdict
    /* 6A */ u16 zoom_clock; // keeps running after the last press for the zoom-out
};

enum
{
    MODE_PENDING, // decisions pending, waiting to know how the battle is shown
    MODE_MAP,     // QTE runs on the map before map animations
    MODE_SCENE,   // QTE runs inside the battle scene before each enemy strike
    MODE_DONE,    // all decided; waiting for the combat action to finish
};

static void Parry_Loop(struct ParryProc * proc);
static void Parry_OnEnd(struct ParryProc * proc);

struct ProcScr CONST_DATA ProcScr_ParryQte[] =
{
    PROC_NAME("ReflexDefense"),
    PROC_ONEND(Parry_OnEnd),
    PROC_REPEAT(Parry_Loop),
    PROC_END,
};

// ---------------------------------------------------------------------------
// battle generation helpers

static void GenerateBattle(struct ParryProc * proc, bool real)
{
    if (real)
    {
        if (proc->ballista)
            BattleGenerateBallistaReal(proc->instigator, proc->target);
        else
            BattleGenerateReal(proc->instigator, proc->target);
        return;
    }

    // simulation pass identical to BattleGenerateRealInternal but without
    // any of its persistent side effects (exp, stats tracking)

    gBattleSt.flags = proc->ballista ? (BATTLE_FLAG_REAL | BATTLE_FLAG_BALLISTA) : BATTLE_FLAG_REAL;

    InitBattleUnit(&gBattleUnitA, proc->instigator);
    InitBattleUnit(&gBattleUnitB, proc->target);

    gBattleSt.range = RECT_DISTANCE(
        gBattleUnitA.unit.x, gBattleUnitA.unit.y,
        gBattleUnitB.unit.x, gBattleUnitB.unit.y);

    if (gBattleSt.flags & BATTLE_FLAG_BALLISTA)
        SetBattleUnitWeaponBallista(&gBattleUnitA);
    else
        SetBattleUnitWeapon(&gBattleUnitA, -1);

    SetBattleUnitWeapon(&gBattleUnitB, -1);

    BattleInitTargetCanCounter();
    BattleApplyWeaponTriangleEffect(&gBattleUnitA, &gBattleUnitB);

    SetBattleUnitTerrainBonusesAuto(&gBattleUnitA);
    SetBattleUnitTerrainBonusesAuto(&gBattleUnitB);

    BattleGenerate(proc->instigator, proc->target);

    BattleUnitTargetCheckCanCounter(&gBattleUnitB);
    BattleUnitTargetSetEquippedWeapon(&gBattleUnitB);
}

// Runs a full generation pass with the decisions known so far.
static void RunPass(struct ParryProc * proc, bool real)
{
    RandSetSt(proc->rand_st);

    proc->generating = TRUE;
    proc->strike_idx = 0;
    proc->pending = FALSE;
    proc->riposte = FALSE;

    GenerateBattle(proc, real);

    proc->generating = FALSE;
    proc->total = gBattleHitIt - gBattleHits;

    if (real)
        proc->final_done = TRUE;
}

// After a decision: re-run the prediction; if nothing is left to decide,
// run the final (real) pass that also computes exp / level ups.
static void Regenerate(struct ParryProc * proc)
{
    RunPass(proc, FALSE);

    if (!proc->pending || proc->n_done >= MAX_STRIKES)
    {
        proc->pending = FALSE;
        RunPass(proc, TRUE);
    }
}

static bool IsPlayerSide(struct Unit * unit)
{
    return unit != NULL && UNIT_FACTION(unit) == FACTION_BLUE;
}

static bool IsEnemySide(struct Unit * unit)
{
    return unit != NULL && UNIT_FACTION(unit) == FACTION_RED;
}

bool ParryShouldRun(struct Unit * a, struct Unit * b)
{
    if (!gParryConfig.enabled)
        return FALSE;

    if (a == NULL || b == NULL)
        return FALSE;

    if (IsPlayerSide(a) && IsEnemySide(b))
        return TRUE;

    if (IsEnemySide(a) && IsPlayerSide(b))
        return TRUE;

    return FALSE;
}

// ---------------------------------------------------------------------------
// hook: replaces BattleGenerateHit

bool ParryBattleGenerateHit(struct BattleUnit * attacker, struct BattleUnit * defender)
{
    struct ParryProc * proc = FindProc(ProcScr_ParryQte);

    if (attacker == &gBattleUnitB)
        gBattleHitIt->info |= BATTLE_HIT_INFO_ACTORB;

    BattleUpdateBattleStats(attacker, defender);

    if (proc != NULL && proc->generating)
    {
        bool enemyStrike = UNIT_FACTION(&attacker->unit) == FACTION_RED
            && UNIT_FACTION(&defender->unit) == FACTION_BLUE;

        if (enemyStrike)
        {
            int idx = proc->strike_idx++;

            if (idx < proc->n_done)
            {
                int res = proc->results[idx];

                if (res == RES_DODGE || res == RES_PARRY)
                    gBattleSt.hit = 0; // forced miss

                if (res == RES_PARRY)
                    proc->riposte = TRUE;
            }
            else if (idx == proc->n_done && !proc->pending)
            {
                proc->pending = TRUE;
                proc->pending_hit = gBattleHitIt - gBattleHits;
                proc->pending_boss = ((UNIT_ATTRIBUTES(&attacker->unit) & UNIT_ATTR_BOSS) || gParryConfig.all_boss) ? TRUE : FALSE;
            }
        }
        else if (UNIT_FACTION(&attacker->unit) == FACTION_BLUE && proc->riposte)
        {
            // riposte: guaranteed hit with +50% of the damage margin
            int margin = gBattleSt.attack - gBattleSt.defense;

            proc->riposte = FALSE;
            gBattleSt.hit = 100;

            if (margin > 0)
                gBattleSt.attack += (margin + 1) / 2;
        }
    }

    BattleGenerateHitTriangleAttack(attacker, defender);
    BattleGenerateHitAttributes(attacker);
    BattleGenerateHitEffects(attacker, defender);

    if (attacker->unit.hp == 0 || defender->unit.hp == 0)
    {
        attacker->wexp_gain++;

        gBattleHitIt->info |= BATTLE_HIT_INFO_FINISHES;

        if (gBattleUnitB.unit.hp == 0)
            gBattleHitIt->info |= BATTLE_HIT_INFO_KILLS_TARGET;

        gBattleHitIt++;
        return TRUE;
    }

    gBattleHitIt++;
    return FALSE;
}

// ---------------------------------------------------------------------------
// presentation

static bool sInScene(void);

static void DrawBanner(char const * line1, char const * line2)
{
    if (sInScene())
        return;

    TmFill(gBg0Tm, 0);
    TmFill(gBg1Tm, 0);

    UnpackUiWindowFrameGraphics();
    PutUiWindowFrame(3, 1, 24, 6, UI_WINDOW_REGULAR);

    ResetTextFont();

    if (line1 != NULL)
        PutDrawText(NULL, gBg0Tm + TM_OFFSET(5, 2), TEXT_COLOR_SYSTEM_WHITE, 0, 20, line1);

    if (line2 != NULL)
        PutDrawText(NULL, gBg0Tm + TM_OFFSET(5, 4), TEXT_COLOR_SYSTEM_GOLD, 0, 20, line2);

    gDispIo.disp_ct.bg0_enable = TRUE;
    gDispIo.disp_ct.bg1_enable = TRUE;

    EnableBgSync(BG0_SYNC_BIT | BG1_SYNC_BIT);
}

static void ClearBanner(void)
{
    if (sInScene())
        return;

    TmFill(gBg0Tm, 0);
    TmFill(gBg1Tm, 0);
    EnableBgSync(BG0_SYNC_BIT | BG1_SYNC_BIT);
}

static void SetMapTint(int effect, int amount)
{
    if (effect == BLEND_EFFECT_NONE)
    {
        if (sInScene())
        {
            struct ParryProc * proc = FindProc(ProcScr_ParryQte);

            *((u16 *) &gDispIo.blend_ct) = proc->saved_bldcnt;
            gDispIo.blend_coef_a = proc->saved_coef_a;
            gDispIo.blend_coef_b = proc->saved_coef_b;
            gDispIo.blend_y = proc->saved_y;
            return;
        }

        SetBlendNone();
        return;
    }

    if (sInScene())
    {
        struct ParryProc * proc = FindProc(ProcScr_ParryQte);

        if (!proc->allow_tint)
            return;
    }

    SetBlendConfig(effect, 0, 0, amount);

    if (sInScene())
    {
        // whole battle screen
        SetBlendTargetA(1, 1, 1, 1, 1);
        gDispIo.blend_ct.target1_enable_bd = TRUE;
    }
    else
    {
        SetBlendTargetA(0, 0, 1, 1, 1);
    }

    SetBlendTargetB(0, 0, 0, 0, 0);
}

static void PutHitPips(struct ParryProc * proc)
{
    // small row of markers: one per impact of the pattern
    int i;
    u16 * tm = gBg0Tm + TM_OFFSET(22, 4);

    if (proc->in_scene)
        return;

    for (i = 0; i < proc->hit_count && i < MAX_HITS; ++i)
    {
        int ch;

        switch (proc->hit_res[i])
        {
        case RES_PARRY: ch = TEXT_SPECIAL_A; break;
        case RES_DODGE: ch = TEXT_SPECIAL_B; break;
        case RES_FAIL:  ch = TEXT_SPECIAL_DASH; break;
        default:        ch = TEXT_SPECIAL_ARROW; break;
        }

        PutSpecialChar(tm + i, TEXT_COLOR_SYSTEM_GREEN, ch);
    }

    EnableBgSync(BG0_SYNC_BIT);
}

// ---------------------------------------------------------------------------
// QTE logic

static void StartStrike(struct ParryProc * proc)
{
    int i;

    if (proc->pending_boss)
        proc->pattern = BossPatterns[RandNextB() % BOSS_PATTERN_COUNT];
    else
        proc->pattern = PatNormal;

    proc->hit_count = 0;

    for (i = 0; proc->pattern[i].kind != EV_END; ++i)
    {
        if (proc->pattern[i].kind == EV_HIT && proc->hit_count < MAX_HITS)
            proc->hit_res[proc->hit_count++] = RES_NONE;
    }

    proc->clock = 0;
    proc->next_ev = 0;
    proc->flash = 0;

    if (proc->pending_boss)
        DrawBanner(STR_BOSS, STR_KEYS);
    else
        DrawBanner(STR_INCOMING, STR_KEYS);

    PutHitPips(proc);

    SetMapTint(BLEND_EFFECT_DARKEN, 4);
    proc->state = ST_PLAY;
}

static int HitTime(struct ParryProc * proc, int hit)
{
    int i, n = 0;

    for (i = 0; proc->pattern[i].kind != EV_END; ++i)
    {
        if (proc->pattern[i].kind == EV_HIT)
        {
            if (n == hit)
                return INTRO_FRAMES + proc->pattern[i].time;

            n++;
        }
    }

    return 0xFFFF;
}

static int FirstUnresolvedHit(struct ParryProc * proc)
{
    int i;

    for (i = 0; i < proc->hit_count; ++i)
        if (proc->hit_res[i] == RES_NONE)
            return i;

    return -1;
}

static void ProcessPress(struct ParryProc * proc, bool parry)
{
    struct ParryWindow const * win = &gParryConfig.windows[gParryConfig.difficulty < 3 ? gParryConfig.difficulty : 1];

    int hit = FirstUnresolvedHit(proc);
    int d, early, late;

    if (hit < 0)
        return;

    d = (int) proc->clock - HitTime(proc, hit);

    early = parry ? win->parry_early : win->dodge_early;
    late = parry ? win->parry_late : win->dodge_late;

    if (d >= -early && d <= late)
    {
        proc->hit_res[hit] = parry ? RES_PARRY : RES_DODGE;
        PlaySe(parry ? 0x6A : 0x67);
        SetMapTint(BLEND_EFFECT_BRIGHTEN, parry ? 10 : 6);
        proc->flash = 6;
    }
    else
    {
        // mistimed (too early): the strike lands
        proc->hit_res[hit] = RES_FAIL;
        PlaySe(0x6C);
    }

    PutHitPips(proc);
}

static void FinishStrike(struct ParryProc * proc)
{
    int i, res = RES_PARRY;

    for (i = 0; i < proc->hit_count; ++i)
    {
        if (proc->hit_res[i] == RES_FAIL || proc->hit_res[i] == RES_NONE)
        {
            res = RES_FAIL;
            break;
        }

        if (proc->hit_res[i] == RES_DODGE)
            res = RES_DODGE;
    }

    proc->results[proc->n_done++] = res;
    proc->shown_res = res;

    switch (res)
    {
    case RES_PARRY:
        DrawBanner(STR_PARRY, STR_RIPOSTE);
        break;

    case RES_DODGE:
        DrawBanner(STR_DODGE, STR_DODGE2);
        break;

    default:
        DrawBanner(STR_FAIL, STR_FAIL2);
        break;
    }

    PutHitPips(proc);
    proc->result_clock = 36;
    proc->state = ST_RESULT;
}

static bool sInScene(void)
{
    struct ParryProc * proc = FindProc(ProcScr_ParryQte);
    return proc != NULL && proc->in_scene;
}


// ---------------------------------------------------------------------------
// battle-scene zoom
//
// While an enemy winds up, the battle scene gently scales up around its
// centre. It reaches full size exactly when the dodge window opens, holds
// there for the whole dodge window, then eases back. Fighters (sprites) are
// scaled in both directions with affine sprites; the background and platform
// (tile layers, which the GBA cannot scale) are stretched to match vertically,
// line by line from the HBlank interrupt. The HUD is left untouched.

#define ZOOM_MAX_EXTRA 16   // 16/256 = +6.25 % at full zoom
#define ZOOM_CX 120
#define ZOOM_CY 72
#define ZOOM_HUD_Y 104      // sprites centred below this line are HUD
#define ZOOM_DECAY 10
#define ZOOM_MAGIC 0x5A4D
#define ZOOM_LEAD 2         // frames between computing the zoom and the player's press being read
#define ZOOM_AFFINE_BASE 28 // affine matrices 28..31 (plain, hflip, vflip, both)

struct ZoomSt
{
    u16 magic;
    u16 scale;     // 8.8 fixed point, 0x100 = 1.0
    u16 inv;       // 8.8 fixed point, 0x10000 / scale
    u16 base2;     // BG2/BG3 vertical offsets latched for the current frame
    u16 base3;
    u16 frame_scale; // scale/inverse latched at VBlank: what this frame displays
    u16 frame_inv;
    u16 pad;
    struct ParryProc * proc;
    Func prev_hblank;
};

// tail of the debug-text buffer, which retail play never uses
#define sZoom ((struct ZoomSt volatile *) 0x02027B60)

extern Func gOnHBlankB;

static bool ZoomActive(void)
{
    struct ParryProc * proc;

    if (sZoom->magic != ZOOM_MAGIC)
        return FALSE;

    proc = sZoom->proc;

    if (proc == NULL || proc->proc_script != ProcScr_ParryQte || !proc->in_scene)
        return FALSE;

    return sZoom->frame_scale > 0x100;
}

static void ZoomHBlank(void)
{
    int n = REG_VCOUNT + 1;
    int src;

    if (n >= 228)
        n = 0;

    if (n >= DISPLAY_HEIGHT)
        return;

    src = ZOOM_CY + (((n - ZOOM_CY) * (int) sZoom->frame_inv) >> 8);

    REG_BG2VOFS = sZoom->base2 + src - n;
    REG_BG3VOFS = sZoom->base3 + src - n;
}

static void ZoomSetScale(int scale)
{
    sZoom->scale = scale;
    sZoom->inv = 0x10000 / scale;
}

static void ZoomBegin(struct ParryProc * proc)
{
    sZoom->proc = proc;
    sZoom->base2 = gDispIo.bg_off[2].y;
    sZoom->base3 = gDispIo.bg_off[3].y;
    ZoomSetScale(0x100);
    sZoom->frame_scale = 0x100;
    sZoom->frame_inv = 0x100;
    sZoom->magic = ZOOM_MAGIC;

    sZoom->prev_hblank = gOnHBlankB;
    SetOnHBlankB(ZoomHBlank);
}

static void ZoomEnd(void)
{
    if (sZoom->magic != ZOOM_MAGIC)
        return;

    ZoomSetScale(0x100);
    sZoom->frame_scale = 0x100;
    sZoom->frame_inv = 0x100;
    SetOnHBlankB(sZoom->prev_hblank);

    sZoom->magic = 0;
    sZoom->proc = NULL;
}

static CONST_DATA u8 sObjSizes[3][4][2] =
{
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } }, // square
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } }, // wide
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } }, // tall
};

static void ZoomOam(u16 * buf, int count)
{
    int i, scale = sZoom->frame_scale;

    for (i = 0; i < count; ++i, buf += 4)
    {
        int a0 = buf[0], a1 = buf[1];
        int shape, size, w, h, x, y, cx, cy, mtx;

        // skip hidden sprites and sprites that already use affine
        if (a0 & 0x0300)
            continue;

        shape = (a0 >> 14) & 3;

        if (shape == 3)
            continue;

        size = (a1 >> 14) & 3;
        w = sObjSizes[shape][size][0];
        h = sObjSizes[shape][size][1];

        y = a0 & 0xFF;
        if (y >= DISPLAY_HEIGHT)
            y -= 0x100;

        x = a1 & 0x1FF;
        if (x >= 0x100)
            x -= 0x200;

        cy = y + h / 2;

        if (cy >= ZOOM_HUD_Y)
            continue;

        cx = x + w / 2;

        cx = ZOOM_CX + (((cx - ZOOM_CX) * scale) >> 8);
        cy = ZOOM_CY + (((cy - ZOOM_CY) * scale) >> 8);

        // double-size affine box: top-left is centre minus full size
        x = cx - w;
        y = cy - h;

        mtx = ZOOM_AFFINE_BASE + ((a1 >> 12) & 1) + ((a1 >> 13) & 1) * 2;

        buf[0] = (a0 & ~0x00FF) | 0x0300 | (y & 0xFF);
        buf[1] = (a1 & 0xC000) | (mtx << 9) | (x & 0x1FF);
    }
}

static void ZoomPutMatrices(void)
{
    int inv = sZoom->frame_inv;

    SetObjAffine(ZOOM_AFFINE_BASE + 0,  inv, 0, 0,  inv);
    SetObjAffine(ZOOM_AFFINE_BASE + 1, -inv, 0, 0,  inv);
    SetObjAffine(ZOOM_AFFINE_BASE + 2,  inv, 0, 0, -inv);
    SetObjAffine(ZOOM_AFFINE_BASE + 3, -inv, 0, 0, -inv);
}

struct OamSectionMirror
{
    u16 * buf;
    void * oam;
    u16 offset;
    u16 count;
};

#define sOamHiSect ((struct OamSectionMirror *) 0x03000018)
#define sOamLoSect ((struct OamSectionMirror *) 0x03000028)

extern u16 * gOamHiPutIt;
extern u16 * gOamLoPutIt;

// hook: replaces SyncHiOam
void ParrySyncHiOam(void)
{
    if (ZoomActive())
    {
        ZoomPutMatrices();
        ZoomOam(sOamHiSect->buf, sOamHiSect->count);
    }

    CpuFastCopy(sOamHiSect->buf, sOamHiSect->oam, sOamHiSect->count * 8);
    ClearOam(sOamHiSect->buf, sOamHiSect->count);

    gOamHiPutIt = sOamHiSect->buf;

    gOamAffinePutIt = (struct OamView *) gOam;
    gOamAffinePutId = 0;
}

// hook: replaces SyncLoOam
void ParrySyncLoOam(void)
{
    // OnVBlank calls this first; latch the zoom for the frame about to be shown
    // so sprites (synced now) and tile layers (HBlank) always agree
    if (sZoom->magic == ZOOM_MAGIC && gBmSt.main_loop_ended)
    {
        sZoom->frame_scale = sZoom->scale;
        sZoom->frame_inv = sZoom->inv;
        sZoom->base2 = gDispIo.bg_off[2].y;
        sZoom->base3 = gDispIo.bg_off[3].y;
    }

    if (sOamLoSect->count == 0)
        return;

    if (ZoomActive())
    {
        ZoomPutMatrices();
        ZoomOam(sOamLoSect->buf, sOamLoSect->count);
    }

    CpuFastCopy(sOamLoSect->buf, sOamLoSect->oam, sOamLoSect->count * 8);
    ClearOam(sOamLoSect->buf, sOamLoSect->count);

    gOamLoPutIt = sOamLoSect->buf;
}

// smoothstep on 0..256
static int Smooth(int x)
{
    if (x <= 0)
        return 0;

    if (x >= 256)
        return 256;

    return (x * x * (3 * 256 - 2 * x)) >> 16;
}

// Zoom level (0..256) at pattern clock c. Full zoom == dodge window.
static int ZoomLevel(struct ParryProc * proc, int c)
{
    struct ParryWindow const * win = &gParryConfig.windows[gParryConfig.difficulty < 3 ? gParryConfig.difficulty : 1];
    struct PatternEv const * ev;
    int best = 0, prev_b = -1, wind = -1, i;

    for (i = 0; (ev = proc->pattern + i)->kind != EV_END; ++i)
    {
        int t = INTRO_FRAMES + ev->time;
        int z = 0;

        switch (ev->kind)
        {

        case EV_WIND:
        case EV_SLOW:
            wind = t;
            break;

        case EV_FAKE:
            // feint: the zoom starts building, then backs off
            if (c >= t && c < t + 16)
                z = (Smooth(((c - t) * 256) / 16) * 150) >> 8;
            else if (c >= t + 16 && c < t + 26)
                z = (150 * (256 - Smooth(((c - t - 16) * 256) / 10))) >> 8;
            break;

        case EV_HIT:
        {
            int a = t - win->dodge_early;
            int b = t + win->dodge_late;
            struct PatternEv const * next;
            bool decay;

            if (c >= a && c <= b)
            {
                z = 256;
            }
            else if (c < a)
            {
                if (wind >= 0 && wind >= prev_b)
                {
                    // fresh wind-up: ease in from nothing
                    if (c >= wind)
                        z = (a > wind) ? Smooth(((c - wind) * 256) / (a - wind)) : 256;
                }
                else if (prev_b >= 0)
                {
                    // combo follow-up: dip to 30 % between blows
                    if (c > prev_b)
                    {
                        int gap = a - prev_b;
                        int d = gap > 0 ? ((c - prev_b) * 256) / gap : 256;
                        int tri = 256 - (d * 2 - 256 < 0 ? 256 - d * 2 : d * 2 - 256);

                        z = 256 - ((tri * 179) >> 8);
                    }
                }
                else if (c >= t - 30)
                {
                    z = Smooth(((c - (t - 30)) * 256) / (a - (t - 30) > 0 ? a - (t - 30) : 1));
                }
            }
            else
            {
                // after the window: ease out, unless a combo blow follows
                next = ev + 1;
                decay = TRUE;

                if (next->kind == EV_HIT)
                    decay = FALSE;

                if (decay && c < b + ZOOM_DECAY)
                    z = 256 - Smooth(((c - b) * 256) / ZOOM_DECAY);
            }

            prev_b = b;
            break;
        }

        }

        if (z > best)
            best = z;
    }

    return best;
}

static void ZoomUpdate(struct ParryProc * proc)
{
    int level;

    if (!proc->in_scene)
        return;

    level = ZoomLevel(proc, proc->zoom_clock + ZOOM_LEAD);
    ZoomSetScale(0x100 + ((level * ZOOM_MAX_EXTRA) >> 8));

    proc->zoom_clock++;
}

// Runs one frame of the timing challenge. Returns TRUE once the strike
// (and its result display) is complete and the decision has been recorded.
static bool QteStep(struct ParryProc * proc)
{
    switch (proc->state)
    {

    case ST_PLAY:
    {
        struct PatternEv const * ev;
        int hit;

        ZoomUpdate(proc);

        // fire pattern events
        while (TRUE)
        {
            ev = proc->pattern + proc->next_ev;

            if (ev->kind == EV_END || INTRO_FRAMES + ev->time != proc->clock)
                break;

            switch (ev->kind)
            {
            case EV_TICK:
                PlaySe(0x66);
                SetMapTint(BLEND_EFFECT_DARKEN, 6);
                break;

            case EV_WIND:
            case EV_FAKE:
                PlaySe(0x68);
                SetMapTint(BLEND_EFFECT_DARKEN, 10);
                break;

            case EV_SLOW:
                PlaySe(0x6F);
                SetMapTint(BLEND_EFFECT_DARKEN, 12);
                break;

            case EV_HIT:
                SetMapTint(BLEND_EFFECT_BRIGHTEN, 8);
                proc->flash = 4;
                break;
            }

            proc->next_ev++;
        }

        if (gKeySt->pressed & KEY_BUTTON_A)
            ProcessPress(proc, TRUE);
        else if (gKeySt->pressed & KEY_BUTTON_B)
            ProcessPress(proc, FALSE);

        // strikes whose window has passed without input land
        hit = FirstUnresolvedHit(proc);

        if (hit >= 0)
        {
            struct ParryWindow const * win = &gParryConfig.windows[gParryConfig.difficulty < 3 ? gParryConfig.difficulty : 1];
            int late = win->parry_late > win->dodge_late ? win->parry_late : win->dodge_late;

            if ((int) proc->clock > HitTime(proc, hit) + late)
            {
                proc->hit_res[hit] = RES_FAIL;
                PlaySe(0x6C);
                PutHitPips(proc);
            }
        }

        if (proc->flash != 0)
        {
            if (--proc->flash == 0)
                SetMapTint(BLEND_EFFECT_DARKEN, 4);
        }

        proc->clock++;

        if (FirstUnresolvedHit(proc) < 0 && proc->flash == 0)
            FinishStrike(proc);

        return FALSE;
    }

    case ST_RESULT:
        ZoomUpdate(proc);

        if (proc->result_clock != 0)
        {
            proc->result_clock--;

            // in battle: show the verdict as a short colour pulse
            if (proc->in_scene && proc->result_clock == 30)
            {
                proc->allow_tint = TRUE;

                if (proc->shown_res == RES_FAIL)
                    SetMapTint(BLEND_EFFECT_DARKEN, 8);
                else
                    SetMapTint(BLEND_EFFECT_BRIGHTEN, proc->shown_res == RES_PARRY ? 12 : 6);
            }

            if (proc->in_scene && proc->result_clock == 20)
                SetMapTint(BLEND_EFFECT_NONE, 0);

            return FALSE;
        }

        SetMapTint(BLEND_EFFECT_NONE, 0);
        ClearBanner();

        if (proc->in_scene)
            ZoomEnd();

        proc->allow_tint = FALSE;
        return TRUE;

    }

    return TRUE;
}

static void BeginQte(struct ParryProc * proc, bool in_scene)
{
    proc->in_scene = in_scene;
    proc->allow_tint = FALSE;
    proc->zoom_clock = 0;

    if (in_scene)
    {
        ZoomBegin(proc);

        proc->saved_bldcnt = *((u16 *) &gDispIo.blend_ct);
        proc->saved_coef_a = gDispIo.blend_coef_a;
        proc->saved_coef_b = gDispIo.blend_coef_b;
        proc->saved_y = gDispIo.blend_y;
    }

    StartStrike(proc);
}

// ---------------------------------------------------------------------------
// map-animation path: a small proc that blocks the combat action while the
// timing challenges run on the map

static void MapWaiter_Loop(ProcPtr waiter);

struct ProcScr CONST_DATA ProcScr_ParryMapWaiter[] =
{
    PROC_NAME("ReflexDefenseWait"),
    PROC_REPEAT(MapWaiter_Loop),
    PROC_END,
};

static void MapWaiter_Loop(ProcPtr waiter)
{
    struct ParryProc * proc = FindProc(ProcScr_ParryQte);

    if (proc != NULL && proc->mode == MODE_MAP)
        return;

    // decisions are final: start the map battle animation as usual
    EndAllMus();
    RenderMap();

    StartBattleManim();

    gBattleSt.flags |= BATTLE_FLAG_MAPANIMS;

    Proc_End(waiter);
}

static void Parry_Loop(struct ParryProc * proc)
{
    switch (proc->mode)
    {

    case MODE_MAP:
        if (proc->state == ST_PREDICT)
        {
            if (!proc->pending)
            {
                if (!proc->final_done)
                    RunPass(proc, TRUE);

                proc->mode = MODE_DONE;
                break;
            }

            BeginQte(proc, FALSE);
            break;
        }

        if (QteStep(proc))
        {
            Regenerate(proc);
            proc->state = ST_PREDICT;
        }

        break;

    default:
        // stay alive while the combat action runs (holds scene state)
        if (FindProc(ProcScr_CombatAction) == NULL)
            Proc_End(proc);

        break;

    }
}

static void Parry_OnEnd(struct ParryProc * proc)
{
    if (sZoom->magic == ZOOM_MAGIC && sZoom->proc == proc)
        ZoomEnd();

    proc->generating = FALSE;
    proc->in_scene = FALSE;
}

static void StartParry(ProcPtr parent, struct Unit * instigator, struct Unit * target, bool ballista)
{
    struct ParryProc * proc = SpawnProcLocking(ProcScr_ParryQte, parent);
    int i;

    proc->state = ST_PREDICT;
    proc->mode = MODE_PENDING;
    proc->generating = FALSE;
    proc->in_scene = FALSE;
    proc->n_done = 0;
    proc->shown = 0;
    proc->seg_ready = FALSE;
    proc->final_done = FALSE;
    proc->parent = parent;
    proc->instigator = instigator;
    proc->target = target;
    proc->ballista = ballista;

    for (i = 0; i < MAX_STRIKES; ++i)
        proc->results[i] = RES_NONE;

    RandGetSt(proc->rand_st);

    // first prediction; if the enemy never strikes, resolve immediately
    RunPass(proc, FALSE);

    if (!proc->pending)
    {
        RunPass(proc, TRUE);
        proc->mode = MODE_DONE;
    }

    SpawnProcLocking(ProcScr_CombatAction, parent);
}

// ---------------------------------------------------------------------------
// hook: replaces BeginBattleAnimations

void ParryBeginBattleAnimations(void)
{
    struct ParryProc * proc = FindProc(ProcScr_ParryQte);

    TmFill(gBg2Tm, 0);
    EnableBgSync(BG2_SYNC_BIT);
    SetBackdropColor(0);

    RenderMap();

    if (PrepareBattleGraphicsMaybe_())
    {
        if (proc != NULL && proc->mode == MODE_PENDING)
            proc->mode = MODE_SCENE;

        SetBanimLinkArenaFlag(0);
        BeginAnimsOnBattleAnimations();
        return;
    }

    if (proc != NULL && proc->mode == MODE_PENDING)
    {
        struct ProcScr const * combat = ProcScr_CombatAction;
        ProcPtr action = FindProc(combat);

        proc->mode = MODE_MAP;
        proc->state = ST_PREDICT;

        if (action != NULL)
        {
            SpawnProcLocking(ProcScr_ParryMapWaiter, action);
            return;
        }
    }

    EndAllMus();
    RenderMap();

    StartBattleManim();

    gBattleSt.flags |= BATTLE_FLAG_MAPANIMS;
}

// ---------------------------------------------------------------------------
// battle scene: segment playback

// Keep only hits [shown, end) of the latest generation in gBattleHits and
// rebuild the battle animation script from them (same as the arena does).
static void PrepareSegment(struct ParryProc * proc)
{
    int end = proc->pending ? proc->pending_hit : proc->total;
    int i, n = end - proc->shown;

    for (i = 0; i < n; ++i)
        gBattleHits[i] = gBattleHits[proc->shown + i];

    gBattleHits[n].attributes = 0;
    gBattleHits[n].info = BATTLE_HIT_INFO_END;
    gBattleHits[n].damage = 0;

    proc->shown = end;
    proc->seg_ready = TRUE;

    ParseBattleHitToBanimCmd();
    BasInit();
    UpdateBanimFrame();
    InitMainAnims();
}

static void ParryEkrQte(struct ProcEkrBattle * ekr);

static void TriggerRoundStartBody(struct ProcEkrBattle * proc)
{
    struct BaSprite * anim;

    if (++proc->timer <= 0x1E)
        return;

    if (gEkrPairSideVaild[POS_L] == TRUE)
    {
        anim = MAIN_ANIM_FRONT(POS_L);
        anim->flags3 = ANIM_BIT3_NEW_ROUND_START;
        anim->flags2 |= ANIM_BIT2_STOP;

        anim = MAIN_ANIM_BACK(POS_L);
        anim->flags3 = ANIM_BIT3_NEW_ROUND_START;
        anim->flags2 |= ANIM_BIT2_STOP;
    }

    if (gEkrPairSideVaild[POS_R] == TRUE)
    {
        anim = MAIN_ANIM_FRONT(POS_R);
        anim->flags3 = ANIM_BIT3_NEW_ROUND_START;
        anim->flags2 |= ANIM_BIT2_STOP;

        anim = MAIN_ANIM_BACK(POS_R);
        anim->flags3 = ANIM_BIT3_NEW_ROUND_START;
        anim->flags2 |= ANIM_BIT2_STOP;
    }

    gBanimDoneFlag[POS_L] = FALSE;
    gBanimDoneFlag[POS_R] = FALSE;
    proc->proc_repeat_func = (ProcFunc)EkrBattleReleaseStandingAnim;
}

// hook: replaces EkrBattleTriggerNewRoundStart
void ParryEkrTriggerNewRoundStart(struct ProcEkrBattle * ekr)
{
    struct ParryProc * proc = FindProc(ProcScr_ParryQte);

    if (proc != NULL && proc->mode == MODE_SCENE && !proc->seg_ready)
    {
        if (proc->pending && proc->pending_hit == proc->shown)
        {
            // the next thing to happen is an undecided enemy strike:
            // play the timing challenge right here in the battle
            BeginQte(proc, TRUE);
            ekr->proc_repeat_func = (ProcFunc) ParryEkrQte;
            return;
        }

        PrepareSegment(proc);
    }

    TriggerRoundStartBody(ekr);
}

static void ParryEkrQte(struct ProcEkrBattle * ekr)
{
    struct ParryProc * proc = FindProc(ProcScr_ParryQte);

    if (proc == NULL)
    {
        ekr->proc_repeat_func = (ProcFunc) ParryEkrTriggerNewRoundStart;
        return;
    }

    if (!QteStep(proc))
        return;

    proc->in_scene = FALSE;
    Regenerate(proc);

    ekr->proc_repeat_func = (ProcFunc) ParryEkrTriggerNewRoundStart;
}

// hook: replaces EkrBattleInRound
void ParryEkrBattleInRound(struct ProcEkrBattle * proc)
{
    struct ParryProc * pp = FindProc(ProcScr_ParryQte);
    int ret = 0;

    if (gKeySt->held & KEY_BUTTON_B)
        proc->speedup = TRUE;

    switch (gEkrDistanceType) {
    case EKR_DISTANCE_CLOSE:
    case EKR_DISTANCE_FAR:
    case EKR_DISTANCE_FARFAR:
        if ((gBanimDoneFlag[0] + gBanimDoneFlag[1]) == 2)
        {
            if (GetBattleAnimArenaFlag() == FALSE)
            {
                ret = 1;
            }
            else
            {
                gEkrPairExpGain[0] = gpEkrBattleUnitLeft->exp_gain;
                gEkrPairExpGain[1] = gpEkrBattleUnitRight->exp_gain;

                if (gEkrGaugeHp[POS_L] == 0)
                {
                    ArenaSetResult(1);
                    ret = 1;
                }
                else if (gEkrGaugeHp[POS_R] == 0)
                {
                    ArenaSetResult(2);
                    gEkrPairExpGain[POS_R] = 0;
                    ret = 1;
                }
                else if (proc->speedup == TRUE)
                {
                    func_fe6_0804C56C();
                    ArenaSetResult(4);
                    gEkrPairExpGain[POS_R] = 0;
                    ret = 1;
                }
                else
                {
                    ArenaContinueBattle();
                    ParseBattleHitToBanimCmd();
                    BasInit();
                    UpdateBanimFrame();
                    InitMainAnims();

                    proc->timer = 0;
                    proc->proc_repeat_func = (ProcFunc)EkrBattleTriggerNewRoundStart;
                }
            }
        }
        break;

    case EKR_DISTANCE_MONOCOMBAT:
        if ((gBanimDoneFlag[0] + gBanimDoneFlag[1]) == 1)
            ret = 1;
        break;

    case EKR_DISTANCE_PROMOTION:
        ret = 1;
        break;
    }

    if (ret == 1 && pp != NULL && pp->mode == MODE_SCENE && pp->shown < pp->total
        && gEkrDistanceType != EKR_DISTANCE_PROMOTION)
    {
        // more of this battle remains: next segment
        pp->seg_ready = FALSE;
        proc->timer = 0;
        proc->proc_repeat_func = (ProcFunc) ParryEkrTriggerNewRoundStart;
        return;
    }

    if (ret == 1)
        proc->proc_repeat_func = (ProcFunc)EkrBattleTriggerEnding;
}

// ---------------------------------------------------------------------------
// hook: replaces DoCombatAction (player phase)

bool ParryDoCombatAction(ProcPtr proc)
{
    struct Unit * instigator = GetUnit(gAction.instigator);
    struct Unit * target = GetUnit(gAction.target);
    bool ballista = gAction.item_slot == ITEMSLOT_BALLISTA;

    if (target == NULL)
        InitObstacleBattleUnit();

    if (ParryShouldRun(instigator, target))
    {
        StartParry(proc, instigator, target, ballista);
        return FALSE;
    }


    if (ballista)
        BattleGenerateBallistaReal(instigator, target);
    else
        BattleGenerateReal(instigator, target);

    SpawnProcLocking(ProcScr_CombatAction, proc);

    return FALSE;
}

// ---------------------------------------------------------------------------
// hook: replaces AiStartCombatAction (enemy phase)

void ParryAiStartCombatAction(ProcPtr proc)
{
    struct Unit * target = GetUnit(gAiDecision.target_id);
    bool ballista = (i8) gAiDecision.item_slot == -1;

    gAction.id = ACTION_COMBAT;
    gAction.target = gAiDecision.target_id;

    gActiveUnit->x = gAiDecision.x_move;
    gActiveUnit->y = gAiDecision.y_move;

    if (!ballista)
        UnitEquipItemSlot(gActiveUnit, gAiDecision.item_slot);

    if (gKeySt->held & KEY_BUTTON_R)
        EndAllMus();

    if (ParryShouldRun(gActiveUnit, target))
    {
        StartParry(proc, gActiveUnit, target, ballista);
        return;
    }

    if (ballista)
        BattleGenerateBallistaReal(gActiveUnit, target);
    else
        BattleGenerateReal(gActiveUnit, target);

    SpawnProcLocking(ProcScr_CombatAction, proc);
}

// ---------------------------------------------------------------------------
// hook: replaces ComputeBattleUnitEffectiveHitRate
// Hit chance is gone: every blow lands unless it is parried or dodged.

void ParryComputeBattleUnitEffectiveHitRate(struct BattleUnit * attacker, struct BattleUnit * defender)
{
    attacker->battle_effective_hit = 100;
}
