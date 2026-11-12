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

static void DrawBanner(char const * line1, char const * line2)
{
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
    TmFill(gBg0Tm, 0);
    TmFill(gBg1Tm, 0);
    EnableBgSync(BG0_SYNC_BIT | BG1_SYNC_BIT);
}

static void SetMapTint(int effect, int amount)
{
    if (effect == BLEND_EFFECT_NONE)
    {
        SetBlendNone();
        return;
    }

    SetBlendConfig(effect, 0, 0, amount);
    SetBlendTargetA(0, 0, 1, 1, 1);
    SetBlendTargetB(0, 0, 0, 0, 0);
}

static void PutHitPips(struct ParryProc * proc)
{
    // small row of markers: one per impact of the pattern
    int i;
    u16 * tm = gBg0Tm + TM_OFFSET(22, 4);

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

static void Parry_Loop(struct ParryProc * proc)
{
    switch (proc->state)
    {

    case ST_PREDICT:
        if (proc->n_done >= MAX_STRIKES)
        {
            proc->state = ST_FINISH;
            break;
        }

        RunPass(proc, FALSE);

        if (!proc->pending)
        {
            proc->state = ST_FINISH;
            break;
        }

        StartStrike(proc);
        break;

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

        break;
    }

    case ST_RESULT:
        if (proc->result_clock != 0)
        {
            proc->result_clock--;
            break;
        }

        SetMapTint(BLEND_EFFECT_NONE, 0);
        ClearBanner();
        proc->state = ST_PREDICT;
        break;

    case ST_FINISH:
        SetMapTint(BLEND_EFFECT_NONE, 0);
        ClearBanner();

        RunPass(proc, TRUE);

        SpawnProcLocking(ProcScr_CombatAction, proc->parent);
        Proc_End(proc);
        break;

    }
}

static void Parry_OnEnd(struct ParryProc * proc)
{
    proc->generating = FALSE;
}

static void StartParry(ProcPtr parent, struct Unit * instigator, struct Unit * target, bool ballista)
{
    struct ParryProc * proc = SpawnProcLocking(ProcScr_ParryQte, parent);
    int i;

    proc->state = ST_PREDICT;
    proc->generating = FALSE;
    proc->n_done = 0;
    proc->parent = parent;
    proc->instigator = instigator;
    proc->target = target;
    proc->ballista = ballista;

    for (i = 0; i < MAX_STRIKES; ++i)
        proc->results[i] = RES_NONE;

    RandGetSt(proc->rand_st);
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
