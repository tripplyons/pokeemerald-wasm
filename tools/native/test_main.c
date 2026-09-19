#include "native_engine.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int NativeTestSave(void);
extern int NativeTestLoad(void);
extern uint32_t NativePointerToWord(const void *pointer);
extern void *NativeDecodePointer(unsigned long word);
extern void CpuSet(uintptr_t src, uintptr_t dst, uint32_t mode);
extern void CpuFastSet(uintptr_t src, uintptr_t dst, uint32_t mode);
extern void WasmCopyOamMatrices(uintptr_t src, uintptr_t dst, uintptr_t dummy,
                                uint32_t count, uint32_t limit);

extern void ObjAffineSet(uintptr_t src, uintptr_t dst, uint32_t count, uint32_t offset);
extern void BgAffineSet(uintptr_t src, uintptr_t dst, uint32_t count);

static void reference_affine(int16_t xScale, int16_t yScale, uint16_t rotation, int16_t matrix[4])
{
    double angle = rotation * 3.14159265358979323846 * 2.0 / 0x10000;
    double sn = sin(angle) * 256.0;
    double cs = cos(angle) * 256.0;
    matrix[0] = (int16_t)(int32_t)(cs * xScale / 256.0);
    matrix[1] = (int16_t)(int32_t)(-sn * xScale / 256.0);
    matrix[2] = (int16_t)(int32_t)(sn * yScale / 256.0);
    matrix[3] = (int16_t)(int32_t)(cs * yScale / 256.0);
}

static int check_affine_math(void)
{
    const int16_t scales[] = {INT16_MIN, -32767, -1024, -257, -256, -1,
                             0, 1, 255, 256, 257, 1024, 32766, INT16_MAX};
    ObjAffineSet(0, 0, 0, 2);
    BgAffineSet(0, 0, 0);
    // Every angle is followed by repeated calls with different scales. This
    // checks both cache misses and hits, including nonzero low angle bits.
    for (uint32_t rotation = 0; rotation <= UINT16_MAX; rotation++) {
        for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
            int16_t xScale = scales[i], yScale = scales[sizeof(scales) / sizeof(scales[0]) - i - 1];
            int16_t matrix[4];
            reference_affine(xScale, yScale, (uint16_t)rotation, matrix);
            for (uint32_t offset = 2; offset <= 8; offset += 6) {
                unsigned char input[13], actual[65], expected[65];
                memset(actual, 0xA5, sizeof(actual));
                memset(expected, 0xA5, sizeof(expected));
                for (size_t record = 0; record < 2; record++) {
                    memcpy(input + 1 + record * 6, &xScale, 2);
                    memcpy(input + 3 + record * 6, &yScale, 2);
                    uint16_t angle = (uint16_t)rotation;
                    memcpy(input + 5 + record * 6, &angle, 2);
                    for (size_t j = 0; j < 4; j++)
                        memcpy(expected + 1 + (record * 4 + j) * offset, matrix + j, 2);
                }
                ObjAffineSet((uintptr_t)(input + 1), (uintptr_t)(actual + 1), 2, offset);
                if (memcmp(actual, expected, sizeof(actual))) return 9;
            }
            unsigned char input[21] = {0}, actual[18], expected[18];
            int32_t texX = 123456, texY = -654321;
            int16_t scrX = -7, scrY = 11;
            uint16_t angle = (uint16_t)rotation;
            memcpy(input + 1, &texX, 4);
            memcpy(input + 5, &texY, 4);
            memcpy(input + 9, &scrX, 2);
            memcpy(input + 11, &scrY, 2);
            memcpy(input + 13, &xScale, 2);
            memcpy(input + 15, &yScale, 2);
            memcpy(input + 17, &angle, 2);
            memset(actual, 0xA5, sizeof(actual));
            memset(expected, 0xA5, sizeof(expected));
            memcpy(expected + 1, matrix, sizeof(matrix));
            // Translation uses the untruncated 32-bit coefficients. At 180
            // degrees, negating INT16_MIN can produce +32768 before storage.
            double radians = rotation * 3.14159265358979323846 * 2.0 / 0x10000;
            double sn = sin(radians) * 256.0, cs = cos(radians) * 256.0;
            int32_t a = (int32_t)(cs * xScale / 256.0);
            int32_t b = (int32_t)(-sn * xScale / 256.0);
            int32_t c = (int32_t)(sn * yScale / 256.0);
            int32_t d = (int32_t)(cs * yScale / 256.0);
            int32_t dx = texX - scrX * a - scrY * b;
            int32_t dy = texY - scrX * c - scrY * d;
            memcpy(expected + 9, &dx, 4);
            memcpy(expected + 13, &dy, 4);
            BgAffineSet((uintptr_t)(input + 1), (uintptr_t)(actual + 1), 1);
            if (memcmp(actual, expected, sizeof(actual))) return 10;
        }
    }
    return 0;
}

static int check_oam_matrices(void)
{
    unsigned char matrices[257], dummy[9], actual[1026], expected[1026];
    for (unsigned int i = 0; i < sizeof(matrices); i++)
        matrices[i] = (unsigned char)(i * 37 + 11);
    for (unsigned int i = 0; i < sizeof(dummy); i++)
        dummy[i] = (unsigned char)(i * 19 + 7);

    // Odd addresses exercise the helper's unaligned copies too. Check every
    // count/limit pair, including records beyond the configured OAM limit.
    for (unsigned int count = 0; count <= 128; count++) {
        for (unsigned int limit = 0; limit <= 128; limit++) {
            for (unsigned int i = 0; i < sizeof(actual); i++)
                actual[i] = expected[i] = (unsigned char)(i * 13 + count + limit);
            for (unsigned int i = count; i < limit; i++)
                memcpy(expected + 1 + i * 8, dummy + 1, 8);
            for (unsigned int i = 0; i < 128; i++)
                memcpy(expected + 1 + i * 8 + 6, matrices + 1 + i * 2, 2);
            WasmCopyOamMatrices((uintptr_t)(matrices + 1), (uintptr_t)(actual + 1),
                               (uintptr_t)(dummy + 1), count, limit);
            if (memcmp(actual, expected, sizeof(actual))) return 8;
        }
    }
    return 0;
}

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
    if (!result) result = check_oam_matrices();
    if (!result) result = check_affine_math();
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
    else puts("native copies, OAM matrices, affine math, pointer arithmetic, save format, and flash round-trip passed");
    return result;
}
