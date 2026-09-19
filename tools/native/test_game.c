#include "global.h"
#include "main.h"
#include "native_save.h"
#include "new_game.h"
#include "save.h"
#include "sprite.h"

STATIC_ASSERT(__alignof__(struct Main) >= 16, NativeMainAlignment)
STATIC_ASSERT(__builtin_offsetof(struct Main, oamBuffer) % 16 == 0, NativeOamAlignment)

int NativeTestOamTail(void)
{
    bool8 loadDisabled = gMain.oamLoadDisabled;
    u8 *oam = (u8 *)OAM;
    unsigned int i;

    gMain.oamLoadDisabled = FALSE;

    // LoadOam leaves the records past the active sprite count in place, so a
    // fill over OAM has to make the next transfer reload all of them.
    LoadOam();
    CpuFastFill(0xFFFFFFFF, (void *)OAM, OAM_SIZE);
    LoadOam();
    if (memcmp(oam, gMain.oamBuffer, sizeof(gMain.oamBuffer)))
        return 11;

    // Code that reaches OAM or the OAM buffer some other way reports it
    // itself, which has to force a full transfer the same way.
    LoadOam();
    for (i = 0; i < OAM_SIZE; i++)
        oam[i] = 0xA5;
    WasmOamBufferModified();
    LoadOam();
    if (memcmp(oam, gMain.oamBuffer, sizeof(gMain.oamBuffer)))
        return 12;

    gMain.oamLoadDisabled = loadDisabled;
    return 0;
}

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
