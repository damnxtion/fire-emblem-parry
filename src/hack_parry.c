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
        { 7, 4, 13, 7 }, // Story        parry 11f / dodge 20f
        { 5, 3,  9, 5 }, // Expeditioner parry  8f / dodge 14f (window counts incl. impact frame)
        { 3, 2,  6, 3 }, // Expert       parry  5f / dodge  9f
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
        if (proc->result_clock != 0)
        {
            proc->result_clock--;

            // in battle: show the verdict as a short colour pulse
            if (proc->in_scene && proc->result_clock == 30)
            {
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
        return TRUE;

    }

    return TRUE;
}

static void BeginQte(struct ParryProc * proc, bool in_scene)
{
    proc->in_scene = in_scene;

    if (in_scene)
    {
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
