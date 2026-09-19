// Keep the flash format unchanged when native pointers are eight bytes.
#include "global.h"
#include "native_save.h"

u8 gNativeSaveBlock1[0x3D88];

_Static_assert(offsetof(struct SaveBlock1, objectEventTemplates) == 0xC70, "save prefix layout");
_Static_assert(sizeof(struct ObjectEventTemplate) == 20 + sizeof(void *), "saved object layout");
_Static_assert(offsetof(struct SaveBlock1, enigmaBerry) - offsetof(struct SaveBlock1, flags) == 0x31F8 - 0x1270, "save middle layout");
_Static_assert(sizeof(struct EnigmaBerry) == 44 + 2 * sizeof(void *), "saved berry layout");
_Static_assert(sizeof(struct SaveBlock1) - offsetof(struct SaveBlock1, mysteryGift) == 0x3D88 - 0x322C, "save suffix layout");
_Static_assert(sizeof(struct SaveBlock2) == 0xF2C, "save block 2 layout");

// Saved script addresses are replaced by LoadSaveblockObjEventScripts. Keep
// their four-byte representation here; they are not native process pointers.
void NativePackSaveBlock1(void)
{
    u32 i;
    u8 *berry = (u8 *)&gSaveBlock1Ptr->enigmaBerry;
    memcpy(gNativeSaveBlock1, gSaveBlock1Ptr, 0xC70);
    for (i = 0; i < OBJECT_EVENT_TEMPLATES_COUNT; i++)
    {
        const struct ObjectEventTemplate *object = &gSaveBlock1Ptr->objectEventTemplates[i];
        u8 *out = gNativeSaveBlock1 + 0xC70 + i * 24;
        memcpy(out, object, 16);
        memcpy(out + 16, &object->script, 4);
        memcpy(out + 20, &object->flagId, 4);
    }
    memcpy(gNativeSaveBlock1 + 0x1270, gSaveBlock1Ptr->flags, 0x31F8 - 0x1270);
    memcpy(gNativeSaveBlock1 + 0x31F8, berry, 12);
    memcpy(gNativeSaveBlock1 + 0x3204, &gSaveBlock1Ptr->enigmaBerry.berry.description1, 4);
    memcpy(gNativeSaveBlock1 + 0x3208, &gSaveBlock1Ptr->enigmaBerry.berry.description2, 4);
    memcpy(gNativeSaveBlock1 + 0x320C, berry + 12 + 2 * sizeof(void *), 32);
    memcpy(gNativeSaveBlock1 + 0x322C, &gSaveBlock1Ptr->mysteryGift, 0x3D88 - 0x322C);
}

void NativeUnpackSaveBlock1(void)
{
    u32 i;
    u8 *berry = (u8 *)&gSaveBlock1Ptr->enigmaBerry;
    memcpy(gSaveBlock1Ptr, gNativeSaveBlock1, 0xC70);
    for (i = 0; i < OBJECT_EVENT_TEMPLATES_COUNT; i++)
    {
        struct ObjectEventTemplate *object = &gSaveBlock1Ptr->objectEventTemplates[i];
        const u8 *in = gNativeSaveBlock1 + 0xC70 + i * 24;
        memcpy(object, in, 16);
        object->script = NULL;
        memcpy(&object->script, in + 16, 4);
        memcpy(&object->flagId, in + 20, 4);
    }
    memcpy(gSaveBlock1Ptr->flags, gNativeSaveBlock1 + 0x1270, 0x31F8 - 0x1270);
    memcpy(berry, gNativeSaveBlock1 + 0x31F8, 12);
    gSaveBlock1Ptr->enigmaBerry.berry.description1 = NULL;
    gSaveBlock1Ptr->enigmaBerry.berry.description2 = NULL;
    memcpy(&gSaveBlock1Ptr->enigmaBerry.berry.description1, gNativeSaveBlock1 + 0x3204, 4);
    memcpy(&gSaveBlock1Ptr->enigmaBerry.berry.description2, gNativeSaveBlock1 + 0x3208, 4);
    memcpy(berry + 12 + 2 * sizeof(void *), gNativeSaveBlock1 + 0x320C, 32);
    memcpy(&gSaveBlock1Ptr->mysteryGift, gNativeSaveBlock1 + 0x322C, 0x3D88 - 0x322C);
}
