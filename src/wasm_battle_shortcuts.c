#include "global.h"

#if WASM

#include "battle.h"
#include "battle_anim.h"
#include "battle_util.h"
#include "data.h"
#include "pokemon.h"
#include "wasm_battle_shortcuts.h"
#include "constants/characters.h"
#include "constants/moves.h"
#include "constants/party_menu.h"
#include "constants/species.h"

#define PARTY_SHORTCUT_COUNT PARTY_SIZE
#define MOVE_SHORTCUT_COUNT MAX_MON_MOVES
#define SHORTCUT_COUNT (MOVE_SHORTCUT_COUNT + PARTY_SHORTCUT_COUNT)

struct WasmBattleShortcut
{
    u8 type;
    u8 value;
    u8 label[WASM_BATTLE_SHORTCUT_MAX_LABEL + 1];
};

static struct WasmBattleShortcut sShortcuts[SHORTCUT_COUNT];
static u8 sShortcutCount;
static u8 sShortcutBattler;
static u8 sPendingBattler;
static u8 sPendingType;
static u8 sPendingValue;

static u8 *CopyShortcutLabel(u8 *dest, const u8 *src)
{
    u8 i;

    for (i = 0; i < WASM_BATTLE_SHORTCUT_MAX_LABEL && src[i] != EOS; i++)
    {
        u8 c = src[i];

        if (c >= CHAR_A && c <= CHAR_Z)
            *dest++ = 'A' + c - CHAR_A;
        else if (c >= CHAR_a && c <= CHAR_z)
            *dest++ = 'A' + c - CHAR_a;
        else if (c >= CHAR_0 && c <= CHAR_9)
            *dest++ = '0' + c - CHAR_0;
        else if (c == CHAR_SPACE)
            *dest++ = ' ';
        else if (c == CHAR_HYPHEN)
            *dest++ = '-';
        else if (c == CHAR_PERIOD)
            *dest++ = '.';
        else if (c == CHAR_MALE)
            *dest++ = 'M';
        else if (c == CHAR_FEMALE)
            *dest++ = 'F';
    }
    *dest = '\0';
    return dest;
}

static void AddShortcut(u8 type, u8 value, const u8 *label)
{
    struct WasmBattleShortcut *shortcut;

    if (sShortcutCount >= SHORTCUT_COUNT)
        return;

    shortcut = &sShortcuts[sShortcutCount++];
    shortcut->type = type;
    shortcut->value = value;
    CopyShortcutLabel(shortcut->label, label);
}

static u8 PartyIdAtBattleSlot(const u8 *partyOrder, u8 slot)
{
    if (slot & 1)
        return partyOrder[slot / 2] & 0xF;
    return partyOrder[slot / 2] >> 4;
}

static void AddPartyShortcuts(u8 partyAction, const u8 *partyOrder)
{
    u8 i;

    if (partyAction == PARTY_ACTION_CANT_SWITCH || partyAction == PARTY_ACTION_ABILITY_PREVENTS)
        return;

    for (i = 0; i < PARTY_SHORTCUT_COUNT; i++)
    {
        u8 partyId = PartyIdAtBattleSlot(partyOrder, i);
        struct Pokemon *mon = &gPlayerParty[partyId];
        u16 species = GetMonData(mon, MON_DATA_SPECIES);
        u8 activeBattler;
        bool8 canSwitch = TRUE;

        if (species == SPECIES_NONE || GetMonData(mon, MON_DATA_HP) == 0 || GetMonData(mon, MON_DATA_IS_EGG))
            continue;
        if ((gBattleTypeFlags & BATTLE_TYPE_MULTI) && (i == 1 || i == 4 || i == 5))
            continue;
        for (activeBattler = 0; activeBattler < gBattlersCount; activeBattler++)
        {
            if (GetBattlerSide(activeBattler) == B_SIDE_PLAYER && partyId == gBattlerPartyIndexes[activeBattler])
            {
                canSwitch = FALSE;
                break;
            }
        }
        if (!canSwitch || partyId == gBattleStruct->prevSelectedPartySlot)
            continue;

        AddShortcut(WASM_BATTLE_SHORTCUT_PARTY, i, gSpeciesNames[species]);
    }
}

u32 WasmBattleShortcutCount(void)
{
    return sShortcutCount;
}

u32 WasmBattleShortcutType(u32 index)
{
    if (index >= sShortcutCount)
        return WASM_BATTLE_SHORTCUT_NONE;
    return sShortcuts[index].type;
}

u32 WasmBattleShortcutLabel(u32 index)
{
    if (index >= sShortcutCount)
        return 0;
    return (u32)sShortcuts[index].label;
}

void WasmBattleShortcutSelect(u32 index)
{
    if (index < sShortcutCount)
    {
        sPendingBattler = sShortcutBattler;
        sPendingType = sShortcuts[index].type;
        sPendingValue = sShortcuts[index].value;
    }
}

void WasmBattleShortcutSetBattler(u8 battler)
{
    sShortcutBattler = battler;
    if (sPendingType != WASM_BATTLE_SHORTCUT_NONE && sPendingBattler != battler)
        sPendingType = WASM_BATTLE_SHORTCUT_NONE;
}

void WasmBattleShortcutQueue(u8 type, u8 value)
{
    sPendingBattler = sShortcutBattler;
    sPendingType = type;
    sPendingValue = value;
}

void WasmBattleShortcutHide(void)
{
    sShortcutCount = 0;
}

bool8 WasmBattleShortcutTakeMove(u8 *moveSlot)
{
    if (sPendingType != WASM_BATTLE_SHORTCUT_MOVE)
        return FALSE;

    *moveSlot = sPendingValue;
    sPendingType = WASM_BATTLE_SHORTCUT_NONE;
    return TRUE;
}

bool8 WasmBattleShortcutTakeParty(u8 *partySlot)
{
    if (sPendingType != WASM_BATTLE_SHORTCUT_PARTY)
        return FALSE;

    *partySlot = sPendingValue;
    sPendingType = WASM_BATTLE_SHORTCUT_NONE;
    return TRUE;
}

void WasmBattleShortcutSetAction(u8 battler, u8 count, const u16 *moves, const u8 *partyOrder)
{
    u8 i;

    (void)battler;
    sShortcutCount = 0;
    sPendingType = WASM_BATTLE_SHORTCUT_NONE;
    for (i = 0; i < count && i < MOVE_SHORTCUT_COUNT; i++)
    {
        if (moves[i] != MOVE_NONE)
            AddShortcut(WASM_BATTLE_SHORTCUT_MOVE, i, gMoveNames[moves[i]]);
    }
    if (partyOrder != NULL)
        AddPartyShortcuts(PARTY_ACTION_CHOOSE_MON, partyOrder);
}

void WasmBattleShortcutSetMoves(u8 count, const u16 *moves)
{
    u8 pendingType = sPendingType;
    u8 pendingValue = sPendingValue;

    WasmBattleShortcutSetAction(0, count, moves, NULL);
    sPendingType = pendingType;
    sPendingValue = pendingValue;
}

void WasmBattleShortcutClearMoves(void)
{
    sShortcutCount = 0;
    sPendingType = WASM_BATTLE_SHORTCUT_NONE;
}

void WasmBattleShortcutSetParty(u8 battler, u8 partyAction, const u8 *partyOrder)
{
    u8 pendingType = sPendingType;
    u8 pendingValue = sPendingValue;

    (void)battler;
    sShortcutCount = 0;
    AddPartyShortcuts(partyAction, partyOrder);
    sPendingType = pendingType;
    sPendingValue = pendingValue;
}

void WasmBattleShortcutClearParty(void)
{
    sShortcutCount = 0;
    sPendingType = WASM_BATTLE_SHORTCUT_NONE;
}

#endif // WASM
