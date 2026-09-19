#ifndef GUARD_NATIVE_SAVE_H
#define GUARD_NATIVE_SAVE_H

extern unsigned char gNativeSaveBlock1[0x3D88];
void NativePackSaveBlock1(void);
void NativeUnpackSaveBlock1(void);

#endif
