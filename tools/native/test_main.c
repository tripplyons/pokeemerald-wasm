#include "native_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int NativeTestSave(void);
extern int NativeTestLoad(void);
extern uint32_t NativePointerToWord(const void *pointer);
extern void *NativeDecodePointer(unsigned long word);
extern void CpuSet(uintptr_t src, uintptr_t dst, uint32_t mode);
extern void CpuFastSet(uintptr_t src, uintptr_t dst, uint32_t mode);

static int check_cpu_copies(void)
{
    unsigned char actual[128], expected[128];
    CpuSet(0, 0, 0);
    CpuFastSet(0, 0, 0);
    for (unsigned int size = 2; size <= 4; size += 2) {
        for (unsigned int fill = 0; fill <= 1; fill++) {
            for (unsigned int src = 0; src < 32; src++) {
                for (unsigned int dst = 0; dst < 32; dst++) {
                    for (unsigned int count = 0; count <= 16; count++) {
                        for (unsigned int i = 0; i < sizeof(actual); i++)
                            actual[i] = expected[i] = (unsigned char)(i * 37 + 11);
                        for (unsigned int i = 0; i < count; i++)
                            memmove(expected + dst + i * size,
                                    expected + src + (fill ? 0 : i * size), size);
                        uint32_t mode = count | (fill << 24) | ((size == 4) << 26);
                        CpuSet((uintptr_t)(actual + src), (uintptr_t)(actual + dst), mode);
                        if (memcmp(actual, expected, sizeof(actual))) return 6;
                        if (size == 4) {
                            for (unsigned int i = 0; i < sizeof(actual); i++)
                                actual[i] = (unsigned char)(i * 37 + 11);
                            CpuFastSet((uintptr_t)(actual + src), (uintptr_t)(actual + dst), mode);
                            if (memcmp(actual, expected, sizeof(actual))) return 7;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

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
    int result = check_cpu_copies();
    if (!result) result = check_pointer_arithmetic();
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
    if (result) fprintf(stderr, "native engine test failed: %d\n", result);
    else puts("native copies, pointer arithmetic, save format, and flash round-trip passed");
    return result;
}
