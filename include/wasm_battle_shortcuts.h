#ifndef GUARD_WASM_BATTLE_SHORTCUTS_H
#define GUARD_WASM_BATTLE_SHORTCUTS_H

#include "global.h"

#define WASM_BATTLE_SHORTCUT_NONE 0
#define WASM_BATTLE_SHORTCUT_MOVE 1
#define WASM_BATTLE_SHORTCUT_PARTY 2
#define WASM_BATTLE_SHORTCUT_MAX_LABEL 12

#if WASM
u32 WasmBattleShortcutCount(void);
u32 WasmBattleShortcutType(u32 index);
u32 WasmBattleShortcutLabel(u32 index);
void WasmBattleShortcutSelect(u32 index);
void WasmBattleShortcutSetBattler(u8 battler);
void WasmBattleShortcutQueue(u8 type, u8 value);
void WasmBattleShortcutHide(void);
bool8 WasmBattleShortcutTakeMove(u8 *moveSlot);
bool8 WasmBattleShortcutTakeParty(u8 *partySlot);
void WasmBattleShortcutSetAction(u8 battler, u8 count, const u16 *moves, const u8 *partyOrder);
void WasmBattleShortcutSetMoves(u8 count, const u16 *moves);
void WasmBattleShortcutClearMoves(void);
void WasmBattleShortcutSetParty(u8 battler, u8 partyAction, const u8 *partyOrder);
void WasmBattleShortcutClearParty(void);
#endif

#endif // GUARD_WASM_BATTLE_SHORTCUTS_H
