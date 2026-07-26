// raylib GUI frontend for the headless native engine core.
// All engine state lives in native_engine.c; this file only handles the
// window, touch/keyboard input, speed control, and texture presentation.
#include "raylib.h"
#include "native_engine.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SAVE_PATH "build/native/pokeemerald-native.sav"
#define SAVE_FLUSH_FRAMES 60
#define DISPLAY_FPS 60
#define MIN_DISPLAY_FPS 6
#define MAX_INTERNAL_FRAME_SECONDS (1.0 / MIN_DISPLAY_FPS)

typedef struct {
    const char *label;
    Rectangle rect;
    uint32_t mask;
} UiButton;

typedef enum {
    SPEED_HALVE,
    SPEED_RESET,
    SPEED_DOUBLE,
} SpeedAction;

typedef struct {
    const char *label;
    Rectangle rect;
    SpeedAction action;
} SpeedButton;

typedef struct {
    Rectangle screen;
    UiButton buttons[10];
    size_t buttonCount;
    SpeedButton speedButtons[4];
    size_t speedButtonCount;
} UiLayout;

static bool mouse_button_down(Rectangle rect)
{
    return IsMouseButtonDown(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), rect);
}

static uint32_t keyboard_buttons(void)
{
    uint32_t held = 0;
    if (IsKeyDown(KEY_Z)) held |= NATIVE_BUTTON_A;
    if (IsKeyDown(KEY_X)) held |= NATIVE_BUTTON_B;
    if (IsKeyDown(KEY_ENTER)) held |= NATIVE_BUTTON_START;
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) held |= NATIVE_BUTTON_SELECT;
    if (IsKeyDown(KEY_RIGHT)) held |= NATIVE_BUTTON_RIGHT;
    if (IsKeyDown(KEY_LEFT)) held |= NATIVE_BUTTON_LEFT;
    if (IsKeyDown(KEY_UP)) held |= NATIVE_BUTTON_UP;
    if (IsKeyDown(KEY_DOWN)) held |= NATIVE_BUTTON_DOWN;
    if (IsKeyDown(KEY_S)) held |= NATIVE_BUTTON_R;
    if (IsKeyDown(KEY_A)) held |= NATIVE_BUTTON_L;
    return held;
}

static void add_button(UiLayout *layout, const char *label, Rectangle rect, uint32_t mask)
{
    layout->buttons[layout->buttonCount++] = (UiButton){ label, rect, mask };
}

static void add_speed_button(UiLayout *layout, const char *label, Rectangle rect, SpeedAction action)
{
    layout->speedButtons[layout->speedButtonCount++] = (SpeedButton){ label, rect, action };
}

static UiLayout make_layout(void)
{
    UiLayout layout = {0};
    const int width = GetScreenWidth();
    const int height = GetScreenHeight();
    const float screenAreaHeight = height - 210.0f;
    float scale = fminf((width - 40.0f) / NATIVE_DISPLAY_WIDTH, screenAreaHeight / NATIVE_DISPLAY_HEIGHT);
    if (scale < 1.0f)
        scale = 1.0f;

    layout.screen = (Rectangle){
        floorf((width - NATIVE_DISPLAY_WIDTH * scale) * 0.5f),
        20.0f,
        NATIVE_DISPLAY_WIDTH * scale,
        NATIVE_DISPLAY_HEIGHT * scale,
    };

    const float y = layout.screen.y + layout.screen.height + 30.0f;
    const float unit = 50.0f;
    const float dpadX = 50.0f;
    add_button(&layout, "UP", (Rectangle){dpadX + unit, y, unit, unit}, NATIVE_BUTTON_UP);
    add_button(&layout, "LEFT", (Rectangle){dpadX, y + unit, unit, unit}, NATIVE_BUTTON_LEFT);
    add_button(&layout, "RIGHT", (Rectangle){dpadX + unit * 2, y + unit, unit, unit}, NATIVE_BUTTON_RIGHT);
    add_button(&layout, "DOWN", (Rectangle){dpadX + unit, y + unit * 2, unit, unit}, NATIVE_BUTTON_DOWN);

    const float faceX = width - 230.0f;
    add_button(&layout, "B", (Rectangle){faceX, y + unit, 70.0f, 70.0f}, NATIVE_BUTTON_B);
    add_button(&layout, "A", (Rectangle){faceX + 90.0f, y + 30.0f, 70.0f, 70.0f}, NATIVE_BUTTON_A);
    add_button(&layout, "L", (Rectangle){150.0f, y - 20.0f, 95.0f, 34.0f}, NATIVE_BUTTON_L);
    add_button(&layout, "R", (Rectangle){width - 245.0f, y - 20.0f, 95.0f, 34.0f}, NATIVE_BUTTON_R);
    add_button(&layout, "SELECT", (Rectangle){width * 0.5f - 110.0f, y + unit * 2.2f, 95.0f, 34.0f}, NATIVE_BUTTON_SELECT);
    add_button(&layout, "START", (Rectangle){width * 0.5f + 15.0f, y + unit * 2.2f, 95.0f, 34.0f}, NATIVE_BUTTON_START);

    const float speedY = y + unit * 0.35f;
    const float speedX = width * 0.5f - 160.0f;
    add_speed_button(&layout, "0.5x", (Rectangle){speedX, speedY, 96.0f, 34.0f}, SPEED_HALVE);
    add_speed_button(&layout, "Reset", (Rectangle){speedX + 112.0f, speedY, 84.0f, 34.0f}, SPEED_RESET);
    add_speed_button(&layout, "2x", (Rectangle){speedX + 212.0f, speedY, 96.0f, 34.0f}, SPEED_DOUBLE);
    return layout;
}

static uint32_t ui_buttons(const UiLayout *layout)
{
    uint32_t held = 0;
    for (size_t i = 0; i < layout->buttonCount; i++) {
        if (mouse_button_down(layout->buttons[i].rect))
            held |= layout->buttons[i].mask;
    }
    return held;
}

static void handle_speed_buttons(const UiLayout *layout, float *speed, float *frameAccumulator)
{
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        return;

    Vector2 mouse = GetMousePosition();
    for (size_t i = 0; i < layout->speedButtonCount; i++) {
        if (!CheckCollisionPointRec(mouse, layout->speedButtons[i].rect))
            continue;

        switch (layout->speedButtons[i].action) {
        case SPEED_HALVE:
            *speed *= 0.5f;
            break;
        case SPEED_RESET:
            *speed = 1.0f;
            break;
        case SPEED_DOUBLE:
            *speed *= 2.0f;
            break;
        }
        *frameAccumulator = 0.0f;
        return;
    }
}

static int frames_for_speed(float speed, double elapsedSeconds, float *frameAccumulator)
{
    *frameAccumulator += speed * (float)elapsedSeconds * DISPLAY_FPS;
    int frames = (int)floorf(*frameAccumulator);
    *frameAccumulator -= frames;
    return frames;
}

static void draw_button(const UiButton *button, uint32_t held)
{
    bool down = (held & button->mask) != 0;
    Color fill = down ? (Color){70, 190, 110, 255} : (Color){44, 49, 64, 255};
    DrawRectangleRounded(button->rect, 0.24f, 12, fill);
    DrawRectangleRoundedLines(button->rect, 0.24f, 12, down ? GREEN : GRAY);
    int textWidth = MeasureText(button->label, 20);
    DrawText(button->label,
             (int)(button->rect.x + (button->rect.width - textWidth) * 0.5f),
             (int)(button->rect.y + (button->rect.height - 20) * 0.5f),
             20,
             RAYWHITE);
}

static void draw_speed_button(const SpeedButton *button, float speed)
{
    bool active = button->action == SPEED_RESET && fabsf(speed - 1.0f) < 0.01f;
    Color fill = active ? (Color){70, 120, 205, 255} : (Color){44, 49, 64, 255};
    DrawRectangleRounded(button->rect, 0.22f, 12, fill);
    DrawRectangleRoundedLines(button->rect, 0.22f, 12, active ? SKYBLUE : GRAY);
    int textWidth = MeasureText(button->label, 18);
    DrawText(button->label,
             (int)(button->rect.x + (button->rect.width - textWidth) * 0.5f),
             (int)(button->rect.y + (button->rect.height - 18) * 0.5f),
             18,
             RAYWHITE);
}

static void draw_ui(Texture2D texture, const UiLayout *layout, uint32_t held, float speed, int internalFps, int displayFps)
{
    BeginDrawing();
    ClearBackground((Color){18, 20, 28, 255});
    DrawRectangleRounded((Rectangle){layout->screen.x - 8, layout->screen.y - 8, layout->screen.width + 16, layout->screen.height + 16}, 0.03f, 8, BLACK);
    DrawTexturePro(texture,
                   (Rectangle){0, 0, NATIVE_DISPLAY_WIDTH, NATIVE_DISPLAY_HEIGHT},
                   layout->screen,
                   (Vector2){0, 0},
                   0.0f,
                   WHITE);
    const char *fpsText = TextFormat("Internal FPS: %d   Display FPS: %d", internalFps, displayFps);
    DrawText(fpsText,
             (int)(layout->screen.x + (layout->screen.width - MeasureText(fpsText, 18)) * 0.5f),
             (int)(layout->screen.y + layout->screen.height + 8.0f),
             18,
             RAYWHITE);

    const char *speedText = TextFormat("Speed: %.3gx", speed);
    DrawText(speedText,
             (int)(GetScreenWidth() * 0.5f - MeasureText(speedText, 18) * 0.5f),
             (int)(layout->speedButtons[0].rect.y - 24),
             18,
             GRAY);
    for (size_t i = 0; i < layout->buttonCount; i++)
        draw_button(&layout->buttons[i], held);
    for (size_t i = 0; i < layout->speedButtonCount; i++)
        draw_speed_button(&layout->speedButtons[i], speed);
    EndDrawing();
}

int main(int argc, char **argv)
{
    const char *savePath = DEFAULT_SAVE_PATH;
    int frameLimit = 0;
    float initialSpeed = 1.0f;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            savePath = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frameLimit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            initialSpeed = strtof(argv[++i], NULL);
        }
    }

    NativeEngine *engine = native_engine_create();
    if (!engine)
        return 1;
    uint32_t lastSaveHash = native_engine_load_flash(engine, savePath);
    native_engine_boot(engine);

    InitWindow(960, 720, "pokeemerald wasm2c native");
    SetWindowState(FLAG_WINDOW_RESIZABLE);
    SetTargetFPS(DISPLAY_FPS);

    Image image = GenImageColor(NATIVE_DISPLAY_WIDTH, NATIVE_DISPLAY_HEIGHT, BLACK);
    Texture2D texture = LoadTextureFromImage(image);
    UnloadImage(image);
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);

    uint32_t frame = 0;
    float speed = initialSpeed > 0.0f ? initialSpeed : 1.0f;
    float frameAccumulator = 0.0f;
    double lastFrameTime = GetTime();
    double lastFpsTime = lastFrameTime;
    int internalFramesThisSecond = 0;
    int displayFramesThisSecond = 0;
    int internalFps = 0;
    int displayFps = 0;
    while (!WindowShouldClose()) {
        double frameNow = GetTime();
        double elapsedSeconds = frameNow - lastFrameTime;
        lastFrameTime = frameNow;

        UiLayout layout = make_layout();
        handle_speed_buttons(&layout, &speed, &frameAccumulator);
        uint32_t held = keyboard_buttons() | ui_buttons(&layout);
        int framesToRun = frames_for_speed(speed, elapsedSeconds, &frameAccumulator);
        if (frameLimit > 0 && frame + (uint32_t)framesToRun > (uint32_t)frameLimit)
            framesToRun = (int)((uint32_t)frameLimit - frame);

        int framesRun = 0;
        double internalStart = GetTime();
        if (framesToRun > 0)
            native_engine_set_keys(engine, held);
        while (framesRun < framesToRun) {
            native_engine_run_frame(engine);
            frame++;
            internalFramesThisSecond++;
            framesRun++;
            if (framesRun < framesToRun && (framesRun & 15) == 0 && GetTime() - internalStart >= MAX_INTERNAL_FRAME_SECONDS)
                break;
        }
        if (framesRun < framesToRun && frameLimit == 0)
            frameAccumulator += framesToRun - framesRun;
        native_engine_render(engine);

        displayFramesThisSecond++;
        double now = GetTime();
        double fpsElapsed = now - lastFpsTime;
        if (fpsElapsed >= 1.0) {
            internalFps = (int)round(internalFramesThisSecond / fpsElapsed);
            displayFps = (int)round(displayFramesThisSecond / fpsElapsed);
            internalFramesThisSecond = 0;
            displayFramesThisSecond = 0;
            lastFpsTime = now;
        }

        UpdateTexture(texture, native_engine_display_buffer(engine));
        draw_ui(texture, &layout, held, speed, internalFps, displayFps);

        if (framesToRun > 0 && frame % SAVE_FLUSH_FRAMES == 0)
            lastSaveHash = native_engine_save_flash_if_changed(engine, savePath, lastSaveHash, false);
        if (frameLimit > 0 && frame >= (uint32_t)frameLimit)
            break;
    }

    lastSaveHash = native_engine_save_flash_if_changed(engine, savePath, lastSaveHash, true);
    (void)lastSaveHash;
    UnloadTexture(texture);
    CloseWindow();
    native_engine_destroy(engine);
    return 0;
}
