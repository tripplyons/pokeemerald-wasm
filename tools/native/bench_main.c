// Headless simulation-engine benchmark for the pokeemerald native port.
//
// The raylib frontend's "Internal FPS" counts WasmRunFrame calls and
// presents a rendered frame only after a batch. This benchmark measures
// the same engine work without raylib or a window: each scenario replays
// into a known state, then times exactly MEASURE_FRAMES WasmRunFrame
// calls with no rendering inside the timed region.
//
// Rendering happens before and after the fixed frame count. FNV-1a
// hashes of those screens must match committed goldens and agree across
// passes. This keeps the performance loop observable and catches state
// or rendering regressions without charging GUI/software-renderer work
// to the engine FPS score.
#include "native_engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_EVENTS 8192
#define MAX_NAME 64
#define MAX_PASSES 8
#define DEFAULT_PASSES 2
#define WARMUP_FRAMES 10007u
#define MEASURE_FRAMES 1000003u
#define MIN_MEASURE_SECONDS 0.01

typedef struct {
    uint32_t frame;
    uint32_t mask;
    bool press;
} ButtonEvent;

typedef struct {
    const char *name;
    uint32_t replayFrame;
} Scenario;

static const Scenario kScenarios[] = {
    { "overworld", 23126 }, // route-blocker-cleared
    { "menu", 26108 },      // bag-open
    { "battle", 27228 },    // mudkip-battle
};
static const size_t kScenarioCount = sizeof(kScenarios) / sizeof(kScenarios[0]);

typedef struct {
    double fps[sizeof(kScenarios) / sizeof(kScenarios[0])];
    double seconds[sizeof(kScenarios) / sizeof(kScenarios[0])];
    uint64_t startHash[sizeof(kScenarios) / sizeof(kScenarios[0])];
    uint64_t endHash[sizeof(kScenarios) / sizeof(kScenarios[0])];
    bool valid;
} PassResult;

static double now_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void fail_json(const char *error)
{
    printf("{\"score\":0,\"info\":{\"error\":\"%s\"}}\n", error);
    exit(0);
}

static uint32_t button_mask(const char *name)
{
    if (!strcmp(name, "a")) return NATIVE_BUTTON_A;
    if (!strcmp(name, "b")) return NATIVE_BUTTON_B;
    if (!strcmp(name, "select")) return NATIVE_BUTTON_SELECT;
    if (!strcmp(name, "start")) return NATIVE_BUTTON_START;
    if (!strcmp(name, "right")) return NATIVE_BUTTON_RIGHT;
    if (!strcmp(name, "left")) return NATIVE_BUTTON_LEFT;
    if (!strcmp(name, "up")) return NATIVE_BUTTON_UP;
    if (!strcmp(name, "down")) return NATIVE_BUTTON_DOWN;
    if (!strcmp(name, "r")) return NATIVE_BUTTON_R;
    if (!strcmp(name, "l")) return NATIVE_BUTTON_L;
    return 0;
}

static int compare_events(const void *va, const void *vb)
{
    const ButtonEvent *a = va;
    const ButtonEvent *b = vb;
    return (a->frame > b->frame) - (a->frame < b->frame);
}

static uint32_t load_script(const char *path, ButtonEvent *events)
{
    FILE *file = fopen(path, "r");
    if (!file)
        fail_json("cannot open replay script");

    uint32_t eventCount = 0;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char *comment = strchr(line, '#');
        if (comment)
            *comment = '\0';

        uint32_t frame;
        char kind[32], name[32], state[8];
        if (sscanf(line, "%u %31s %31s %7s", &frame, kind, name, state) != 4)
            continue;
        if (strcmp(kind, "button"))
            continue;

        uint32_t mask = button_mask(name);
        if (!mask)
            continue;
        if (eventCount >= MAX_EVENTS)
            fail_json("too many replay button events");
        events[eventCount++] = (ButtonEvent){
            .frame = frame,
            .mask = mask,
            .press = !strcmp(state, "on"),
        };
    }
    fclose(file);

    qsort(events, eventCount, sizeof(*events), compare_events);
    return eventCount;
}

static NativeEngine *replay_to(const ButtonEvent *events, uint32_t eventCount, uint32_t targetFrame)
{
    NativeEngine *engine = native_engine_create();
    if (!engine)
        fail_json("engine create failed");
    native_engine_boot(engine);

    uint32_t eventIndex = 0;
    uint32_t held = 0;
    for (uint32_t frame = 0; frame <= targetFrame; frame++) {
        while (eventIndex < eventCount && events[eventIndex].frame == frame) {
            if (events[eventIndex].press)
                held |= events[eventIndex].mask;
            else
                held &= ~events[eventIndex].mask;
            eventIndex++;
            native_engine_set_keys(engine, held);
        }
        native_engine_run_frame(engine);
    }

    // Scenario measurement is intentionally idle and independent of a
    // replay button that happened to remain held at the checkpoint.
    native_engine_set_keys(engine, 0);
    return engine;
}

static void run_scenario(const ButtonEvent *events, uint32_t eventCount,
                         const Scenario *scenario, double *secondsOut,
                         double *fpsOut, uint64_t *startHashOut,
                         uint64_t *endHashOut)
{
    NativeEngine *engine = replay_to(events, eventCount, scenario->replayFrame);

    native_engine_render(engine);
    *startHashOut = native_engine_hash_display(engine);

    for (uint32_t frame = 0; frame < WARMUP_FRAMES; frame++)
        native_engine_run_frame(engine);

    double start = now_seconds();
    for (uint32_t frame = 0; frame < MEASURE_FRAMES; frame++)
        native_engine_run_frame(engine);
    double end = now_seconds();

    // Makes the fixed frame loop observable and validates its final
    // emulated screen state, while remaining outside the timed region.
    native_engine_render(engine);
    *endHashOut = native_engine_hash_display(engine);

    *secondsOut = end - start;
    *fpsOut = *secondsOut > 0.0 ? (double)MEASURE_FRAMES / *secondsOut : 0.0;
    native_engine_destroy(engine);
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    char *text = malloc((size_t)size + 1);
    if (!text) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(text, 1, (size_t)size, file);
    text[got] = '\0';
    fclose(file);
    return text;
}

static bool golden_lookup(const char *text, const char *name, uint64_t *value)
{
    char key[MAX_NAME + 4];
    snprintf(key, sizeof(key), "\"%s\"", name);
    const char *at = strstr(text, key);
    if (!at)
        return false;
    at = strchr(at + strlen(key), '"');
    if (!at)
        return false;
    at++;
    char *end = NULL;
    unsigned long long parsed = strtoull(at, &end, 16);
    if (end == at)
        return false;
    *value = (uint64_t)parsed;
    return true;
}

static void golden_key(char *out, size_t size, const char *scenario, const char *position)
{
    snprintf(out, size, "%s-%s", scenario, position);
}

static void write_golden(const char *path, const PassResult *result)
{
    FILE *file = fopen(path, "w");
    if (!file)
        fail_json("cannot write golden hashes");

    fprintf(file, "{\n  \"checkpoints\": {\n");
    for (size_t s = 0; s < kScenarioCount; s++) {
        fprintf(file, "    \"%s-start\": \"%016llx\",\n", kScenarios[s].name,
                (unsigned long long)result->startHash[s]);
        fprintf(file, "    \"%s-end\": \"%016llx\"%s\n", kScenarios[s].name,
                (unsigned long long)result->endHash[s],
                s + 1 < kScenarioCount ? "," : "");
    }
    fprintf(file, "  },\n  \"warmup_frames\": %u,\n  \"measure_frames\": %u\n}\n",
            WARMUP_FRAMES, MEASURE_FRAMES);
    fclose(file);
}

int main(int argc, char **argv)
{
    const char *scriptPath = NULL;
    const char *goldenPath = NULL;
    const char *writeGoldenPath = NULL;
    int passes = DEFAULT_PASSES;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--script") && i + 1 < argc)
            scriptPath = argv[++i];
        else if (!strcmp(argv[i], "--golden") && i + 1 < argc)
            goldenPath = argv[++i];
        else if (!strcmp(argv[i], "--write-golden") && i + 1 < argc)
            writeGoldenPath = argv[++i];
        else if (!strcmp(argv[i], "--passes") && i + 1 < argc)
            passes = atoi(argv[++i]);
    }

    if (!scriptPath)
        fail_json("missing --script");
    if (passes < 1)
        passes = 1;
    if (passes > MAX_PASSES)
        passes = MAX_PASSES;

    static ButtonEvent events[MAX_EVENTS];
    uint32_t eventCount = load_script(scriptPath, events);
    if (eventCount == 0)
        fail_json("replay script has no button events");

    PassResult results[MAX_PASSES] = {0};
    for (int pass = 0; pass < passes; pass++) {
        results[pass].valid = true;
        for (size_t s = 0; s < kScenarioCount; s++) {
            run_scenario(events, eventCount, &kScenarios[s],
                         &results[pass].seconds[s], &results[pass].fps[s],
                         &results[pass].startHash[s], &results[pass].endHash[s]);
            if (!isfinite(results[pass].fps[s])
                || results[pass].seconds[s] < MIN_MEASURE_SECONDS)
                results[pass].valid = false;
        }
    }

    bool deterministic = true;
    for (int pass = 1; pass < passes; pass++) {
        for (size_t s = 0; s < kScenarioCount; s++) {
            if (results[pass].startHash[s] != results[0].startHash[s]
                || results[pass].endHash[s] != results[0].endHash[s])
                deterministic = false;
        }
    }

    bool progressing = false;
    for (size_t s = 0; s < kScenarioCount; s++) {
        if (results[0].startHash[s] != results[0].endHash[s])
            progressing = true;
    }

    char *goldenText = goldenPath ? read_file(goldenPath) : NULL;
    bool haveGolden = goldenText != NULL;
    bool hashesOk = true;
    char mismatches[kScenarioCount * 2][MAX_NAME];
    size_t mismatchCount = 0;
    if (haveGolden) {
        for (size_t s = 0; s < kScenarioCount; s++) {
            const uint64_t actual[] = { results[0].startHash[s], results[0].endHash[s] };
            const char *positions[] = { "start", "end" };
            for (size_t p = 0; p < 2; p++) {
                char key[MAX_NAME];
                golden_key(key, sizeof(key), kScenarios[s].name, positions[p]);
                uint64_t expected = 0;
                if (!golden_lookup(goldenText, key, &expected) || expected != actual[p]) {
                    hashesOk = false;
                    snprintf(mismatches[mismatchCount++], MAX_NAME, "%s", key);
                }
            }
        }
    }

    if (writeGoldenPath)
        write_golden(writeGoldenPath, &results[0]);

    double aggregateFps[kScenarioCount];
    double score = 0.0;
    for (size_t s = 0; s < kScenarioCount; s++) {
        double totalSeconds = 0.0;
        for (int pass = 0; pass < passes; pass++)
            totalSeconds += results[pass].seconds[s];
        aggregateFps[s] = (double)MEASURE_FRAMES * passes / totalSeconds;
        score += aggregateFps[s];
    }
    score /= (double)kScenarioCount;

    const char *gateError = NULL;
    for (int pass = 0; pass < passes; pass++) {
        if (!results[pass].valid)
            gateError = "invalid timer sample";
    }
    if (!haveGolden && !writeGoldenPath)
        gateError = "no golden hashes; correctness cannot be verified";
    else if (haveGolden && !hashesOk)
        gateError = "framebuffer hash mismatch";
    else if (!deterministic)
        gateError = "nondeterministic screen hashes";
    else if (!progressing)
        gateError = "engine not progressing (all screens unchanged)";
    if (gateError)
        score = 0.0;

    printf("{\n  \"score\": %.2f,\n  \"info\": {\n", score);
    printf("    \"warmup_frames\": %u,\n", WARMUP_FRAMES);
    printf("    \"measure_frames\": %u,\n", MEASURE_FRAMES);
    if (gateError)
        printf("    \"error\": \"%s\",\n", gateError);
    printf("    \"scenario_fps\": {");
    for (size_t s = 0; s < kScenarioCount; s++)
        printf("%s\"%s\": %.2f", s ? ", " : "", kScenarios[s].name, aggregateFps[s]);
    printf("},\n    \"passes\": [\n");
    for (int pass = 0; pass < passes; pass++) {
        printf("      {\"valid\": %s, \"scenarios\": {", results[pass].valid ? "true" : "false");
        for (size_t s = 0; s < kScenarioCount; s++) {
            printf("%s\"%s\": {\"fps\": %.2f, \"seconds\": %.4f}",
                   s ? ", " : "", kScenarios[s].name,
                   results[pass].fps[s], results[pass].seconds[s]);
        }
        printf("}}%s\n", pass + 1 < passes ? "," : "");
    }
    printf("    ],\n");
    printf("    \"deterministic\": %s,\n", deterministic ? "true" : "false");
    printf("    \"progressing\": %s,\n", progressing ? "true" : "false");
    printf("    \"golden_present\": %s,\n", haveGolden ? "true" : "false");
    printf("    \"hashes_ok\": %s,\n", haveGolden && hashesOk ? "true" : "false");
    printf("    \"mismatches\": [");
    for (size_t i = 0; i < mismatchCount; i++)
        printf("%s\"%s\"", i ? ", " : "", mismatches[i]);
    printf("],\n    \"checkpoints\": {");
    for (size_t s = 0; s < kScenarioCount; s++) {
        printf("%s\"%s-start\": \"%016llx\", \"%s-end\": \"%016llx\"",
               s ? ", " : "", kScenarios[s].name,
               (unsigned long long)results[0].startHash[s], kScenarios[s].name,
               (unsigned long long)results[0].endHash[s]);
    }
    printf("}\n  }\n}\n");

    free(goldenText);
    return 0;
}
