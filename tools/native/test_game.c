#include "global.h"
#include "native_save.h"
#include "new_game.h"
#include "save.h"

int NativeTestSave(void)
{
    struct SaveBlock1 original;
    unsigned int i;

    memset(gSaveBlock1Ptr, 0x5A, sizeof(*gSaveBlock1Ptr));
    for (i = 0; i < OBJECT_EVENT_TEMPLATES_COUNT; i++)
        gSaveBlock1Ptr->objectEventTemplates[i].script = NULL;
    gSaveBlock1Ptr->enigmaBerry.berry.description1 = NULL;
    gSaveBlock1Ptr->enigmaBerry.berry.description2 = NULL;
    original = *gSaveBlock1Ptr;
    NativePackSaveBlock1();
    if (gNativeSaveBlock1[0x1270] != 0x5A || gNativeSaveBlock1[0x322C] != 0x5A)
        return 1;
    memset(gSaveBlock1Ptr, 0, sizeof(*gSaveBlock1Ptr));
    NativeUnpackSaveBlock1();
    if (memcmp(&original, gSaveBlock1Ptr, sizeof(original)))
        return 2;

    NewGameInitData();
    gSaveBlock1Ptr->flags[100] = 0x6D;
    gSaveBlock1Ptr->money = 12345;
    if (TrySavingData(SAVE_NORMAL) != SAVE_STATUS_OK)
        return 3;
    memset(gSaveBlock1Ptr, 0, sizeof(*gSaveBlock1Ptr));
    return 0;
}

int NativeTestLoad(void)
{
    if (LoadGameSave(SAVE_NORMAL) != SAVE_STATUS_OK)
        return 4;
    if (gSaveBlock1Ptr->flags[100] != 0x6D || gSaveBlock1Ptr->money != 12345)
        return 5;
    return 0;
}
