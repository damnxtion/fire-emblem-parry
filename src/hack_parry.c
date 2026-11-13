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
#include "hack_parry_gfx.h"

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
#define BATTLE_HIT_MAX_COUNT 7 // size of gBattleHits

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

static int HitsPerRound(struct BattleUnit * unit)
{
    if (!unit->weapon)
        return 0;

    return (unit->weapon_attributes & ITEM_ATTR_BRAVE) ? 2 : 1;
}

// Upper bound on how many hits the standard battle sequence can still add
// after the hit currently being generated (used to keep gBattleHits in range).
static int MaxRemainingHits(struct BattleUnit * attacker)
{
    struct BattleHit * hit = gBattleHitIt;
    int count = HitsPerRound(attacker);
    int in_phase = 0, followup;
    int a = HitsPerRound(&gBattleUnitA), b = HitsPerRound(&gBattleUnitB);

    followup = a > b ? a : b;

    // a brave weapon strikes twice in one phase
    if (count == 2)
    {
        bool second = hit > gBattleHits
            && ((hit - 1)->info & BATTLE_HIT_INFO_ACTORB) == (hit->info & BATTLE_HIT_INFO_ACTORB)
            && ((hit - 1)->attributes & (BATTLE_HIT_ATTR_RETALIATE | BATTLE_HIT_ATTR_FOLLOWUP))
                == (hit->attributes & (BATTLE_HIT_ATTR_RETALIATE | BATTLE_HIT_ATTR_FOLLOWUP));

        in_phase = second ? 0 : 1;
    }

    if (hit->attributes & BATTLE_HIT_ATTR_FOLLOWUP)
        return in_phase;

    if (hit->attributes & BATTLE_HIT_ATTR_RETALIATE)
        return in_phase + followup;

    return in_phase + b + followup;
}

bool ParryBattleGenerateHit(struct BattleUnit * attacker, struct BattleUnit * defender)
{
    struct ParryProc * proc = FindProc(ProcScr_ParryQte);
    bool riposte_now = FALSE;

    // never write past the end of gBattleHits (last slot holds the end marker)
    if (gBattleHitIt >= gBattleHits + (BATTLE_HIT_MAX_COUNT - 1))
        return TRUE;

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
                {
                    proc->riposte = TRUE;

                    // counter right away if the defender can strike back and
                    // the hit list is guaranteed to have room for it
                    if (defender->weapon != 0
                        && (gBattleHitIt - gBattleHits) + 2 + MaxRemainingHits(attacker) <= BATTLE_HIT_MAX_COUNT - 1)
                    {
                        riposte_now = TRUE;
                    }
                }
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
            // riposte: +50% of the damage margin
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

    if (riposte_now)
    {
        // the parrying unit hits back immediately
        gBattleHitIt->attributes = (defender == &gBattleUnitB) ? BATTLE_HIT_ATTR_RETALIATE : BATTLE_HIT_ATTR_FOLLOWUP;

        if (ParryBattleGenerateHit(defender, attacker))
            return TRUE;

        // the next hit continues the interrupted phase
        gBattleHitIt->attributes = 0;
    }

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

        // effect sprites (palette 15, effect tiles) stay crisp
        if ((buf[2] >> 12) == 15 && ((buf[2] & 0x3FF) >= 30 * 32 + 17) && ((buf[2] & 0x3FF) <= 31 * 32 + 26))
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

#define FX_PAL_ID 15
static bool FxActive(void);
static void FxPutOam(u16 * buf, int count);

// hook: replaces SyncHiOam
void ParrySyncHiOam(void)
{
    if (FxActive())
    {
        int i;

        // the scene restores palettes from its own copy (e.g. after the miss
        // flash): keep our effect palette in place while effects are shown
        for (i = 0; i < 16; ++i)
            ((u16 *) (PLTT + 0x200))[FX_PAL_ID * 16 + i] = gParryGfxPal[i];

        // sprite-sheet uploads (ApplyDataMoves, just before this) can cover
        // the effect tiles: put them back every frame
        for (i = 0; i < 10 * 16; ++i)
        {
            ((u16 *) (0x06010000 + (30 * 32 + 17) * 32))[i] = ((u16 const *) gParryGfxRow30)[i];
            ((u16 *) (0x06010000 + (31 * 32 + 17) * 32))[i] = ((u16 const *) gParryGfxRow31)[i];
        }

        FxPutOam(sOamHiSect->buf, sOamHiSect->count);
    }

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

static void LiveReset(void);

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
        {
            proc->mode = MODE_SCENE;
            proc->in_scene = TRUE;
            LiveReset();
        }

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

// ---------------------------------------------------------------------------
// live defence inside the battle scene
//
// The pending enemy strike is the last hit of the current segment and is
// prepared as a hit. While it plays, we count the frames left until the
// attacker's animation reaches its contact command (read straight from the
// animation script), zoom the scene so full zoom == the dodge window around
// contact, and judge presses against what the player saw. A success turns
// the blow into a miss before the contact command runs.

#define LIVE_SEEN_LAG 2   // frames between a computed frame and a press made on seeing it
#define LIVE_RAMP 20      // zoom-in length before the dodge window
#define LIVE_DECAY 10

enum { BOSS_NONE, BOSS_RUSH, BOSS_DELAY, BOSS_FEINT, BOSS_LONG, BOSS_KIND_COUNT };

enum { FX_NONE, FX_PARRY, FX_COUNTER };

struct LiveSt
{
    u8 armed;        // the segment ends with an undecided enemy strike
    u8 decided;
    u8 round;        // round index of that strike in the current segment
    u8 side;         // attacker side (0 left, 1 right)
    u8 boss;
    u8 hold_trigger;
    u8 hold_len;
    u8 hold_left;
    u8 hold_done;
    u8 hold_active;
    u8 stall;        // frames frozen at the contact point waiting for input
    u8 hist_n;
    i8 hist[4];      // recent "frames until contact" values
    u8 zoom_on;
    u8 decay;        // zoom-out countdown
    u16 decay_from;
    u8 counter_next; // show COUNTER! when the next segment starts
    u8 gfx_loaded;
    u8 spark_t;      // spark effect frame counter (0 = off)
    u8 text_t;       // text effect frames left
    u8 text_kind;
    u8 dodge_t;      // dodge step frames left
    i8 dodge_dx;     // current dodge offset applied to the defender sprites
    i16 spark_x, spark_y;
    i16 text_x, text_y;
};

#define sLive ((struct LiveSt *) 0x02027600)
#define sGfxSave ((u8 *) 0x02027700)   // 640 bytes of OBJ tiles
#define sPalSave ((u16 *) 0x02027980)  // 16 colours

#define FX_PAL 15
#define FX_TILE_ROW30 (30 * 32 + 17)
#define FX_TILE_ROW31 (31 * 32 + 17)
#define FX_AFFINE 27

static void LiveReset(void)
{
    u8 * p = (u8 *) sLive;
    unsigned i;

    for (i = 0; i < sizeof(struct LiveSt); ++i)
        p[i] = 0;
}

static void Copy16(void const * src, void * dst, int bytes)
{
    u16 const * s = src;
    u16 * d = dst;
    int i;

    for (i = 0; i < bytes / 2; ++i)
        d[i] = s[i];
}

static u16 * ObjTile(int tile)
{
    return (u16 *) (0x06010000 + tile * 32);
}

static void FxLoadGfx(void)
{
    int i;

    if (sLive->gfx_loaded)
        return;

    // keep what was there so it can be put back afterwards
    Copy16(ObjTile(FX_TILE_ROW30), sGfxSave, 10 * 32);
    Copy16(ObjTile(FX_TILE_ROW31), sGfxSave + 10 * 32, 10 * 32);

    for (i = 0; i < 16; ++i)
        sPalSave[i] = ((u16 *) (PLTT + 0x200))[FX_PAL * 16 + i];

    Copy16(gParryGfxRow30, ObjTile(FX_TILE_ROW30), 10 * 32);
    Copy16(gParryGfxRow31, ObjTile(FX_TILE_ROW31), 10 * 32);

    // write only our palette, straight to palette RAM (and its shadow, in
    // case the scene syncs it); never resync the whole palette
    for (i = 0; i < 16; ++i)
    {
        ((u16 *) (PLTT + 0x200))[FX_PAL * 16 + i] = gParryGfxPal[i];
        gPal[0x100 + FX_PAL * 16 + i] = gParryGfxPal[i];
    }

    sLive->gfx_loaded = TRUE;
}

static void FxRestoreGfx(void)
{
    int i;

    if (!sLive->gfx_loaded)
        return;

    Copy16(sGfxSave, ObjTile(FX_TILE_ROW30), 10 * 32);
    Copy16(sGfxSave + 10 * 32, ObjTile(FX_TILE_ROW31), 10 * 32);

    for (i = 0; i < 16; ++i)
    {
        ((u16 *) (PLTT + 0x200))[FX_PAL * 16 + i] = sPalSave[i];
        gPal[0x100 + FX_PAL * 16 + i] = sPalSave[i];
    }

    sLive->gfx_loaded = FALSE;
}

static struct BaSprite * AtkAnim(int i) { return gAnims[sLive->side * 2 + i]; }
static struct BaSprite * DefAnim(int i) { return gAnims[(sLive->side ^ 1) * 2 + i]; }

static void SetAtkFrozen(bool frozen)
{
    int i;

    for (i = 0; i < 2; ++i)
    {
        struct BaSprite * bas = AtkAnim(i);

        if (bas == NULL)
            continue;

        if (frozen)
            bas->flags |= 0x0008; // BAS_BIT_FROZEN
        else
            bas->flags &= ~0x0008;
    }
}

// Frames (sprite updates) until the attacker's script reads its contact
// command, or -1 if it cannot be known yet (waiting for its turn, jumps...).
static int FramesUntilContact(struct BaSprite * bas, bool rush)
{
    u32 const * it = bas->scrCur;
    int frames = bas->timer;
    int fallback = -1;
    int i;

    if (rush && frames > 1)
        frames = (frames + 1) / 2;

    for (i = 0; i < 96; ++i)
    {
        u32 ins = *it++;
        int d;

        if (ins & 0x80000000)
        {
            if (ins & 0x40000000)
                return fallback;

            switch (0x3F & (ins >> 24))
            {

            case 5: // command
            {
                int cmd = ins & 0xFF;

                // 0x1A: normal hit lands; 0x08-0x0C: critical lands; 0x05: spell
                if (cmd == 0x1A || cmd == 0x05 || (cmd >= 0x08 && cmd <= 0x0C))
                    return frames;

                // 0x04 sets up a dodge; it is the contact point only if no
                // hit command follows before the attack ends
                if (cmd == 0x04 && fallback < 0)
                    fallback = frames;

                if (cmd == 0x01 || cmd == 0x02 || cmd == 0x03 || cmd == 0x13 || cmd == 0x18 || cmd == 0x39)
                    return (i == 0) ? -1 : fallback;

                frames += 1;
                break;
            }

            case 6: // frame
                d = ins & 0xFFFF;
                frames += rush ? (d + 1) / 2 : d;
                it += 2;
                break;

            case 4: // wait
                d = ins & 0xFFFF;
                frames += rush ? (d + 1) / 2 : d;
                break;

            case 3: // move
                d = (ins >> 16) & 0xFF;
                frames += rush ? (d + 1) / 2 : d;
                break;

            default:
                return fallback;

            }
        }
        else
        {
            d = ((ins >> 26) & 0x1C) + (ins & 3);
            frames += rush ? (d + 1) / 2 : d;
        }
    }

    return fallback;
}

static struct ParryWindow const * CurWindow(void)
{
    return &gParryConfig.windows[gParryConfig.difficulty < 3 ? gParryConfig.difficulty : 1];
}

static void FlipToMiss(void)
{
    u16 * rounds = (u16 *) 0x0203CD1E; // parsed per-round animation types
    int idx = sLive->round * 2 + (sLive->side ^ 1);
    int miss = (gEkrDistanceType == EKR_DISTANCE_CLOSE) ? 4 : 5;

    rounds[idx] = (rounds[idx] & 0xF000) | miss;
}

static void StartEffect(int kind)
{
    struct BaSprite * def = DefAnim(0);
    struct BaSprite * atk = AtkAnim(0);
    int dx;

    FxLoadGfx();

    if (kind == FX_PARRY)
    {
        dx = atk->xPosition - def->xPosition;
        sLive->spark_x = def->xPosition + dx / 3;
        sLive->spark_y = def->yPosition - 26;
        sLive->spark_t = 1;

        sLive->text_kind = FX_PARRY;
        sLive->text_t = 48;
        sLive->text_x = 120 - 24;
        sLive->text_y = 18;
    }
    else
    {
        sLive->text_kind = FX_COUNTER;
        sLive->text_t = 48;
        sLive->text_x = 120 - 32;
        sLive->text_y = 18;
    }
}

static void LiveDecide(struct ParryProc * proc, int res)
{
    struct ParryWindow const * win = CurWindow();

    sLive->decided = TRUE;
    sLive->armed = FALSE;

    if (res == RES_PARRY || res == RES_DODGE)
        FlipToMiss();

    proc->results[proc->n_done++] = res;

    if (res == RES_PARRY)
    {
        PlaySe(0x6A);
        StartEffect(FX_PARRY);
        sLive->counter_next = TRUE;
    }
    else if (res == RES_DODGE)
    {
        PlaySe(0x67);
        sLive->dodge_t = 18;
    }

    // let the swing continue
    sLive->hold_active = FALSE;
    sLive->stall = 0;
    SetAtkFrozen(FALSE);

    // ease the zoom out from where it is
    sLive->decay = LIVE_DECAY;
    sLive->decay_from = sZoom->scale;

    (void) win;
}

static int LiveZoomLevel(int e)
{
    struct ParryWindow const * win = CurWindow();
    int level = 0;

    if (e <= win->dodge_early && e >= -win->dodge_late)
        level = 256;
    else if (e > win->dodge_early && e <= win->dodge_early + LIVE_RAMP)
        level = Smooth(((win->dodge_early + LIVE_RAMP - e) * 256) / LIVE_RAMP);
    else if (e < -win->dodge_late)
    {
        // window over: start easing out immediately
        level = 256 - (-win->dodge_late - e) * 64;

        if (level < 0)
            level = 0;
    }

    // feint: the zoom swells early, then backs off, while the boss holds
    if (sLive->boss == BOSS_FEINT && sLive->hold_active)
    {
        int t = sLive->hold_len - sLive->hold_left;
        int f = 0;

        if (t < 12)
            f = (Smooth((t * 256) / 12) * 170) >> 8;
        else if (t < 22)
            f = (170 * (256 - Smooth(((t - 12) * 256) / 10))) >> 8;

        if (f > level)
            level = f;
    }

    return level;
}

static void LiveStep(struct ParryProc * proc)
{
    struct ParryWindow const * win = CurWindow();
    struct BaSprite * atk;
    int f, e, seen;

    if (!sLive->armed || sLive->decided)
        return;

    atk = AtkAnim(0);

    if (atk == NULL || atk->nextRoundId == 0 || atk->nextRoundId - 1 != sLive->round)
        return;

    // boss hold in progress
    if (sLive->hold_active)
    {
        if (--sLive->hold_left == 0)
        {
            sLive->hold_active = FALSE;
            sLive->hold_done = TRUE;
            SetAtkFrozen(FALSE);
        }
    }

    if (sLive->stall)
    {
        e = -(int) sLive->stall;
        sLive->stall++;
    }
    else
    {
        f = FramesUntilContact(atk, sLive->boss == BOSS_RUSH);

        if (f < 0)
            return; // not swinging yet

        // rush: the swing plays at double speed (apply first, then measure)
        if (sLive->boss == BOSS_RUSH && f > 1)
        {
            int i;

            for (i = 0; i < 2; ++i)
            {
                struct BaSprite * bas = AtkAnim(i);

                if (bas != NULL && bas->timer > 1)
                    bas->timer--;
            }

            f = FramesUntilContact(atk, TRUE);
        }

        // delayed / feint: freeze mid-swing for a while
        if (sLive->boss >= BOSS_DELAY && !sLive->hold_done && !sLive->hold_active && f - 1 <= sLive->hold_trigger)
        {
            sLive->hold_active = TRUE;
            sLive->hold_left = sLive->hold_len;
            SetAtkFrozen(TRUE);
        }

        e = f - 1 + (sLive->hold_active ? sLive->hold_left : (!sLive->hold_done && sLive->boss >= BOSS_DELAY ? sLive->hold_len : 0));

        if (f <= 1)
        {
            // the next sprite update would play the contact: hold it there
            // until the player has had the whole late part of the window
            sLive->stall = 1;
            SetAtkFrozen(TRUE);
            e = 0;
        }
    }

    if (!sLive->zoom_on)
    {
        ZoomBegin(proc);
        sLive->zoom_on = TRUE;
    }

    // history of e, to judge presses against what was on screen
    sLive->hist[3] = sLive->hist[2];
    sLive->hist[2] = sLive->hist[1];
    sLive->hist[1] = sLive->hist[0];
    sLive->hist[0] = e;

    if (sLive->hist_n < 4)
        sLive->hist_n++;

    seen = sLive->hist_n > LIVE_SEEN_LAG ? sLive->hist[LIVE_SEEN_LAG] : e + LIVE_SEEN_LAG;

    ZoomSetScale(0x100 + ((LiveZoomLevel(e) * ZOOM_MAX_EXTRA) >> 8));

    if (gKeySt->pressed & (KEY_BUTTON_A | KEY_BUTTON_B))
    {
        bool parry = (gKeySt->pressed & KEY_BUTTON_A) != 0;
        int early = parry ? win->parry_early : win->dodge_early;
        int late = parry ? win->parry_late : win->dodge_late;

        if (seen <= early && seen >= -late)
            LiveDecide(proc, parry ? RES_PARRY : RES_DODGE);
        else
            LiveDecide(proc, RES_FAIL);

        return;
    }

    if (seen < -(int) (win->dodge_late > win->parry_late ? win->dodge_late : win->parry_late))
        LiveDecide(proc, RES_FAIL);
}

// effects and zoom-out, every frame of the scene
static void LiveEffects(void)
{
    if (sLive->decay != 0)
    {
        int level = ((sLive->decay_from - 0x100) * Smooth((sLive->decay * 256) / LIVE_DECAY)) >> 8;

        sLive->decay--;
        ZoomSetScale(0x100 + level);

        if (sLive->decay == 0)
        {
            ZoomEnd();
            sLive->zoom_on = FALSE;
        }
    }

    if (sLive->spark_t != 0)
    {
        if (++sLive->spark_t > 22)
            sLive->spark_t = 0;
    }

    if (sLive->text_t != 0)
        sLive->text_t--;

    if (sLive->dodge_t != 0)
    {
        // quick step away from the blow, then back
        int t = 18 - sLive->dodge_t;
        int target, dir, i;

        dir = (sLive->side == 0) ? 1 : -1; // defender moves away from the attacker
        target = (t < 4) ? (t + 1) * 3 : (t < 10 ? 12 : 12 - (t - 9) * 3 / 2);

        if (target < 0)
            target = 0;

        target *= dir;

        for (i = 0; i < 2; ++i)
        {
            struct BaSprite * bas = DefAnim(i);

            if (bas != NULL)
                bas->xPosition += target - sLive->dodge_dx;
        }

        sLive->dodge_dx = target;

        if (--sLive->dodge_t == 0)
        {
            for (i = 0; i < 2; ++i)
            {
                struct BaSprite * bas = DefAnim(i);

                if (bas != NULL)
                    bas->xPosition -= sLive->dodge_dx;
            }

            sLive->dodge_dx = 0;
        }
    }

    if (sLive->gfx_loaded && sLive->spark_t == 0 && sLive->text_t == 0)
        FxRestoreGfx();
}

// Called from the VBlank OAM sync: put effect sprites in front of everything.
static void FxPutOam(u16 * buf, int count)
{
    u16 ents[4][3];
    int n = 0, i;

    if (sLive->spark_t != 0)
    {
        // 16x16 spark, affine double-size, growing then shrinking
        int t = sLive->spark_t;
        int scale = (t < 6) ? 192 + t * 54 : 516 - (t - 6) * 28;
        int inv;

        if (scale < 64)
            scale = 64;

        inv = 0x10000 / scale;

        SetObjAffine(FX_AFFINE, inv, 0, 0, inv);

        ents[n][0] = 0x0300 | ((sLive->spark_y - 16) & 0xFF);
        ents[n][1] = (1 << 14) | (FX_AFFINE << 9) | ((sLive->spark_x - 16) & 0x1FF);
        ents[n][2] = (FX_PAL << 12) | FX_TILE_ROW30;
        n++;
    }

    if (sLive->text_t != 0)
    {
        int tile = (sLive->text_kind == FX_PARRY) ? FX_TILE_ROW30 + 2 : FX_TILE_ROW31 + 2;
        int bob = sLive->text_t > 40 ? (sLive->text_t - 40) : 0;

        for (i = 0; i < 2; ++i)
        {
            ents[n][0] = (1 << 14) | ((sLive->text_y + bob) & 0xFF);       // wide
            ents[n][1] = (1 << 14) | ((sLive->text_x + i * 32) & 0x1FF);  // 32x8
            ents[n][2] = (FX_PAL << 12) | (tile + i * 4);
            n++;
        }
    }

    if (n == 0 || count <= n)
        return;

    // make room at the front (front entries are drawn on top)
    for (i = count - 1; i >= n; --i)
    {
        buf[i * 4 + 0] = buf[(i - n) * 4 + 0];
        buf[i * 4 + 1] = buf[(i - n) * 4 + 1];
        buf[i * 4 + 2] = buf[(i - n) * 4 + 2];
    }

    for (i = 0; i < n; ++i)
    {
        buf[i * 4 + 0] = ents[i][0];
        buf[i * 4 + 1] = ents[i][1];
        buf[i * 4 + 2] = ents[i][2];
    }
}

static bool FxActive(void)
{
    return sLive->gfx_loaded && (sLive->spark_t != 0 || sLive->text_t != 0);
}

// Keep only hits [shown, end) of the latest generation in gBattleHits and
// rebuild the battle animation script from them (same as the arena does).
static void PrepareSegment(struct ParryProc * proc)
{
    int start = proc->shown;
    int end = proc->pending ? proc->pending_hit + 1 : proc->total;
    int i, n = end - start;

    for (i = 0; i < n; ++i)
        gBattleHits[i] = gBattleHits[start + i];

    gBattleHits[n].attributes = 0;
    gBattleHits[n].info = BATTLE_HIT_INFO_END;
    gBattleHits[n].damage = 0;

    proc->shown = end;
    proc->seg_ready = TRUE;

    // live defence for the strike that closes this segment
    sLive->armed = proc->pending;
    sLive->decided = FALSE;
    sLive->round = n - 1;
    sLive->stall = 0;
    sLive->hist_n = 0;
    sLive->hold_active = FALSE;
    sLive->hold_done = FALSE;
    sLive->boss = BOSS_NONE;

    if (proc->pending)
    {
        struct BattleHit * hit = gBattleHits + (n - 1);
        bool actor_b = (hit->info & BATTLE_HIT_INFO_ACTORB) != 0;
        struct BattleUnit * enemy = actor_b ? &gBattleUnitB : &gBattleUnitA;

        sLive->side = (gpEkrBattleUnitLeft == enemy) ? 0 : 1;

        if (proc->pending_boss)
        {
            static CONST_DATA u8 kinds[] = { BOSS_RUSH, BOSS_DELAY, BOSS_FEINT, BOSS_LONG, BOSS_NONE };

            sLive->boss = kinds[RandNextB() % 5];

            switch (sLive->boss)
            {
            case BOSS_DELAY: sLive->hold_trigger = 8;  sLive->hold_len = 16; break;
            case BOSS_FEINT: sLive->hold_trigger = 10; sLive->hold_len = 28; break;
            case BOSS_LONG:  sLive->hold_trigger = 5;  sLive->hold_len = 26; break;
            }
        }
    }

    ParseBattleHitToBanimCmd();
    BasInit();
    UpdateBanimFrame();
    InitMainAnims();

    if (sLive->counter_next && start > 0)
    {
        sLive->counter_next = FALSE;
        StartEffect(FX_COUNTER);
    }

    sLive->counter_next = FALSE;
}

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

    if (proc != NULL && proc->mode == MODE_SCENE)
    {
        if (!proc->seg_ready)
        {
            bool continuation = proc->shown > 0;

            PrepareSegment(proc);

            // later segments follow straight on, without the opening pause
            if (continuation && ekr->timer < 0x1E)
                ekr->timer = 0x1E;
        }

        LiveEffects();
    }

    TriggerRoundStartBody(ekr);
}

// hook: replaces EkrBattleInRound
void ParryEkrBattleInRound(struct ProcEkrBattle * proc)
{
    struct ParryProc * pp = FindProc(ProcScr_ParryQte);
    int ret = 0;

    if (pp != NULL && pp->mode == MODE_SCENE)
    {
        LiveStep(pp);
        LiveEffects();
    }

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

    if (ret == 1 && pp != NULL && pp->mode == MODE_SCENE)
    {
        // safety: never leave a strike undecided or the scene zoomed/frozen
        if (sLive->armed && !sLive->decided)
            LiveDecide(pp, RES_FAIL);

        if (sLive->zoom_on && sLive->decay == 0)
        {
            ZoomEnd();
            sLive->zoom_on = FALSE;
        }

        // the decided strike changes what follows: regenerate the battle
        if (sLive->decided)
        {
            sLive->decided = FALSE;
            Regenerate(pp);
        }

        if (pp->shown < pp->total && gEkrDistanceType != EKR_DISTANCE_PROMOTION)
        {
            // more of this battle remains: next segment
            pp->seg_ready = FALSE;
            proc->timer = 0;
            proc->proc_repeat_func = (ProcFunc) ParryEkrTriggerNewRoundStart;
            return;
        }

        if (sLive->decay != 0)
        {
            sLive->decay = 0;
            ZoomEnd();
            sLive->zoom_on = FALSE;
        }

        sLive->text_t = 0;
        sLive->spark_t = 0;
        FxRestoreGfx();
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
