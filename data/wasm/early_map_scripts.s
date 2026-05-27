#include "config.h"
#include "constants/global.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h"
#include "constants/field_tasks.h"
#include "constants/flags.h"
#include "constants/heal_locations.h"
#include "constants/map_event_ids.h"
#include "constants/map_scripts.h"
#include "constants/maps.h"
#include "constants/metatile_labels.h"
#include "constants/script_menu.h"
#include "constants/vars.h"

    .section .rodata

    .include "data/maps/InsideOfTruck/scripts.inc"

LittlerootTown_MapScripts::
    map_script MAP_SCRIPT_ON_TRANSITION, LittlerootTown_WasmNoopScript
    map_script MAP_SCRIPT_ON_FRAME_TABLE, LittlerootTown_OnFrame
    map_script MAP_SCRIPT_ON_WARP_INTO_MAP_TABLE, LittlerootTown_WasmNoopTable
    .byte 0

LittlerootTown_WasmNoopScript:
    end

LittlerootTown_WasmNoopTable:
    .2byte 0

LittlerootTown_OnFrame:
    map_script_2 VAR_LITTLEROOT_INTRO_STATE, 1, LittlerootTown_EventScript_StepOffTruckMale
    map_script_2 VAR_LITTLEROOT_INTRO_STATE, 2, LittlerootTown_EventScript_StepOffTruckFemale
    .2byte 0

LittlerootTown_EventScript_StepOffTruckMale::
    lockall
    setvar VAR_0x8004, 5
    setvar VAR_0x8005, 8
    call LittlerootTown_EventScript_GoInsideWithMom
    setflag FLAG_HIDE_LITTLEROOT_TOWN_BRENDANS_HOUSE_TRUCK
    warpsilent MAP_LITTLEROOT_TOWN_BRENDANS_HOUSE_1F, 8, 8
    waitstate
    releaseall
    end

LittlerootTown_EventScript_StepOffTruckFemale::
    lockall
    setvar VAR_0x8004, 14
    setvar VAR_0x8005, 8
    call LittlerootTown_EventScript_GoInsideWithMom
    setflag FLAG_HIDE_LITTLEROOT_TOWN_MAYS_HOUSE_TRUCK
    warpsilent MAP_LITTLEROOT_TOWN_MAYS_HOUSE_1F, 2, 8
    waitstate
    releaseall
    end

LittlerootTown_EventScript_GoInsideWithMom:
    delay 15
    playse SE_LEDGE
    applymovement LOCALID_PLAYER, LittlerootTown_Movement_PlayerStepOffTruck
    waitmovement 0
    opendoor VAR_0x8004, VAR_0x8005
    waitdooranim
    addobject LOCALID_LITTLEROOT_MOM
    applymovement LOCALID_LITTLEROOT_MOM, LittlerootTown_Movement_MomExitHouse
    waitmovement 0
    closedoor VAR_0x8004, VAR_0x8005
    waitdooranim
    delay 10
    applymovement LOCALID_LITTLEROOT_MOM, LittlerootTown_Movement_MomApproachPlayerAtTruck
    waitmovement 0
    msgbox LittlerootTown_Text_OurNewHomeLetsGoInside, MSGBOX_DEFAULT
    closemessage
    applymovement LOCALID_LITTLEROOT_MOM, LittlerootTown_Movement_MomApproachDoor
    applymovement LOCALID_PLAYER, LittlerootTown_Movement_PlayerApproachDoor
    waitmovement 0
    opendoor VAR_0x8004, VAR_0x8005
    waitdooranim
    applymovement LOCALID_LITTLEROOT_MOM, LittlerootTown_Movement_MomEnterHouse
    applymovement LOCALID_PLAYER, LittlerootTown_Movement_PlayerEnterHouse
    waitmovement 0
    setflag FLAG_HIDE_LITTLEROOT_TOWN_MOM_OUTSIDE
    setvar VAR_LITTLEROOT_INTRO_STATE, 3
    hideplayer
    closedoor VAR_0x8004, VAR_0x8005
    waitdooranim
    clearflag FLAG_HIDE_LITTLEROOT_TOWN_FAT_MAN
    clearflag FLAG_HIDE_MAP_NAME_POPUP
    return

LittlerootTown_Movement_MomExitHouse:
    walk_down
    step_end

LittlerootTown_Movement_MomApproachPlayerAtTruck:
    walk_down
    walk_in_place_faster_left
    step_end

LittlerootTown_Movement_MomApproachDoor:
    delay_16
    delay_8
    walk_up
    step_end

LittlerootTown_Movement_MomEnterHouse:
    walk_up
    set_invisible
    step_end

LittlerootTown_Movement_PlayerApproachDoor:
    delay_16
    delay_8
    walk_right
    walk_in_place_faster_up
    step_end

LittlerootTown_Movement_PlayerEnterHouse:
    walk_up
    walk_up
    step_end

LittlerootTown_Movement_PlayerStepOffTruck:
    delay_16
    delay_16
    delay_16
    step_end

LittlerootTown_Text_OurNewHomeLetsGoInside:
    .string "MOM: {PLAYER}, we're here, honey!\p"
    .string "It must be tiring riding with our things\n"
    .string "in the moving truck.\p"
    .string "Well, this is LITTLEROOT TOWN.\p"
    .string "How do you like it?\n"
    .string "This is our new home!\p"
    .string "It has a quaint feel, but it seems to be\n"
    .string "an easy place to live, don't you think?\p"
    .string "And, you get your own room, {PLAYER}!\n"
    .string "Let's go inside.$"
