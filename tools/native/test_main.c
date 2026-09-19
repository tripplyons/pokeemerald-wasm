#include "native_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern int NativeTestSave(void);
extern int NativeTestLoad(void);
extern uint32_t NativePointerToWord(const void *pointer);
extern void *NativeDecodePointer(unsigned long word);

static int check_pointer_arithmetic(void)
{
    const size_t pageSize = 1u << 24;
    unsigned char *buffer = malloc(pageSize * 2);
    if (!buffer) return 1;
    uintptr_t boundary = ((uintptr_t)buffer + pageSize) & ~(uintptr_t)(pageSize - 1);
    uint32_t word = NativePointerToWord((void *)(boundary - 1));
    int failed = NativeDecodePointer(word + 2) != (void *)(boundary + 1);
    free(buffer);
    return failed;
}

int main(void)
{
    char path[] = "/tmp/pokeemerald-native-save-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);
    NativeEngine *engine = native_engine_create();
    if (!engine) return 1;
    native_engine_boot(engine);
    for (int frame = 0; frame < 300; frame++) native_engine_run_frame(engine);
    int result = check_pointer_arithmetic();
    if (!result) result = NativeTestSave();
    if (!result) {
        native_engine_save_flash_if_changed(engine, path, 0, true);
        native_engine_destroy(engine);
        engine = native_engine_create();
        if (!engine) return 1;
        native_engine_load_flash(engine, path);
        native_engine_boot(engine);
        for (int frame = 0; frame < 300; frame++) native_engine_run_frame(engine);
        result = NativeTestLoad();
    }
    native_engine_destroy(engine);
    unlink(path);
    if (result) fprintf(stderr, "native save test failed: %d\n", result);
    else puts("native save format and flash round-trip passed");
    return result;
}
