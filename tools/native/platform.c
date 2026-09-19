#include "global.h"
#include "gba/m4a_internal.h"

// Native frames dispatch interrupts in C. The GBA startup still copies this
// buffer, but never executes it. Cry playback is disabled by the shared port.
const u32 IntrMain[0x200] = {0};
const struct ToneData voicegroup_dummy = {0};
struct ToneData gCryTable[384];
struct ToneData gCryTable_Reverse[384];
const u8 RomHeaderSoftwareVersion = 0;
const u8 RomHeaderGameCode[4] = {'B', 'P', 'E', 'E'};

// These layouts are shared with generate_data.py's native pointer relocations.
_Static_assert(sizeof(struct MapHeader) == 28 + 4 * (sizeof(void *) - 4), "map header layout");
_Static_assert(sizeof(struct MapLayout) == 24 + 4 * (sizeof(void *) - 4), "map layout");
_Static_assert(sizeof(struct ObjectEventTemplate) == 24 + sizeof(void *) - 4, "object event layout");
_Static_assert(sizeof(struct CoordEvent) == 16 + sizeof(void *) - 4, "coordinate event layout");
_Static_assert(sizeof(struct BgEvent) == 12 + sizeof(void *) - 4, "background event layout");
_Static_assert(sizeof(struct MapEvents) == 20 + 4 * (sizeof(void *) - 4), "map events layout");
_Static_assert(sizeof(struct MapConnections) == 8 + sizeof(void *) - 4, "map connections layout");
