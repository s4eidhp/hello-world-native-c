
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <stdint.h>
#include <stddef.h>

// Small 5x7 bitmap font for HELLO WORLD!
static const uint8_t *glyph(char c) {
    static const uint8_t H[7] = {17,17,17,31,17,17,17};
    static const uint8_t E[7] = {31,16,16,30,16,16,31};
    static const uint8_t L[7] = {16,16,16,16,16,16,31};
    static const uint8_t O[7] = {14,17,17,17,17,17,14};
    static const uint8_t W[7] = {17,17,17,21,21,21,10};
    static const uint8_t R[7] = {30,17,17,30,20,18,17};
    static const uint8_t D[7] = {30,17,17,17,17,17,30};
    static const uint8_t X[7] = {4,4,4,4,4,0,4};
    static const uint8_t S[7] = {0,0,0,0,0,0,0};

    switch (c) {
        case 'H': return H;
        case 'E': return E;
        case 'L': return L;
        case 'O': return O;
        case 'W': return W;
        case 'R': return R;
        case 'D': return D;
        case '!': return X;
        default:  return S;
    }
}

// Draw the Hello World screen
static void draw(ANativeActivity *activity,
                 ANativeWindow *window) {
    (void)activity;

    ANativeWindow_setBuffersGeometry(
        window, 0, 0, WINDOW_FORMAT_RGBA_8888
    );

    ANativeWindow_Buffer buffer;

    if (ANativeWindow_lock(window, &buffer, NULL) != 0)
        return;

    uint32_t *pixels = (uint32_t *)buffer.bits;
    int width = buffer.width;
    int height = buffer.height;
    int stride = buffer.stride;

    // Fill background with dark color
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            pixels[y * stride + x] = 0xFF202020;
        }
    }

    const char *message = "HELLO WORLD!";
    const int chars = 12;

    int scale = width / 90;
    if (height / 28 < scale)
        scale = height / 28;
    if (scale < 1)
        scale = 1;

    int textWidth = (chars * 6 - 1) * scale;
    int startX = (width - textWidth) / 2;
    int startY = (height - 7 * scale) / 2;

    for (int i = 0; i < chars; i++) {
        const uint8_t *letter = glyph(message[i]);

        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (!(letter[row] & (1 << (4 - col))))
                    continue;

                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        int px = startX + (i * 6 + col) * scale + dx;
                        int py = startY + row * scale + dy;

                        if (px >= 0 && px < width &&
                            py >= 0 && py < height) {
                            pixels[py * stride + px] = 0xFFFFFFFF;
                        }
                    }
                }
            }
        }
    }

    ANativeWindow_unlockAndPost(window);
}

// Native Android entry point
void ANativeActivity_onCreate(
    ANativeActivity *activity,
    void *savedState,
    size_t savedStateSize
) {
    (void)savedState;
    (void)savedStateSize;

    activity->callbacks->onNativeWindowCreated = draw;
    activity->callbacks->onNativeWindowRedrawNeeded = draw;
    activity->callbacks->onNativeWindowResized = draw;
}
