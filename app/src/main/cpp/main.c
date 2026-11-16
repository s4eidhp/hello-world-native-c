// Native C Android: experimental 2D phone motion trail.
// Uses linear acceleration + device orientation (game rotation vector).
// NOTE: Double-integration drift makes this an approximate visualization,
// NOT a reliable position tracker.

#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/sensor.h>
#include <android/looper.h>

#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_POINTS 2500
#define RGB(r,g,b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

#define BG       RGB(10, 19, 34)
#define PANEL    RGB(16, 30, 51)
#define GRID     RGB(37, 56, 79)
#define AXIS     RGB(67, 88, 115)
#define TEXT     RGB(239, 247, 255)
#define MUTED    RGB(148, 169, 194)
#define CYAN     RGB(69, 220, 243)
#define GREEN    RGB(42, 216, 149)

typedef struct {
    pthread_mutex_t mutex;
    pthread_t worker;
    ANativeWindow *window;
    int running;
    int stop;
    int resumed;
    int sensorsOK; // -1 checking, 0 unavailable, 1 ready
    int gotOrientation;
    int gotAcceleration;

    // Rotation from phone coordinates into a stable-ish world frame.
    float qx, qy, qz, qw;

    // Estimated position/velocity, with the app launch at the origin.
    float x, y, vx, vy;
    float accelerationX, accelerationY;
    int64_t lastAccelerationNs;

    float pathX[MAX_POINTS];
    float pathY[MAX_POINTS];
    int pointCount;
} App;

// Compact 5x7 uppercase pixel font.
static const uint8_t FONT[128][7] = {
    ['A']={14,17,17,31,17,17,17}, ['B']={30,17,17,30,17,17,30},
    ['C']={14,17,16,16,16,17,14}, ['D']={30,17,17,17,17,17,30},
    ['E']={31,16,16,30,16,16,31}, ['F']={31,16,16,30,16,16,16},
    ['G']={14,17,16,23,17,17,14}, ['H']={17,17,17,31,17,17,17},
    ['I']={31,4,4,4,4,4,31},     ['J']={7,2,2,2,18,18,12},
    ['K']={17,18,20,24,20,18,17},['L']={16,16,16,16,16,16,31},
    ['M']={17,27,21,21,17,17,17},['N']={17,25,21,19,17,17,17},
    ['O']={14,17,17,17,17,17,14},['P']={30,17,17,30,16,16,16},
    ['Q']={14,17,17,17,21,18,13},['R']={30,17,17,30,20,18,17},
    ['S']={15,16,16,14,1,1,30}, ['T']={31,4,4,4,4,4,4},
    ['U']={17,17,17,17,17,17,14},['V']={17,17,17,17,17,10,4},
    ['W']={17,17,17,21,21,21,10},['X']={17,17,10,4,10,17,17},
    ['Y']={17,17,10,4,4,4,4},   ['Z']={31,1,2,4,8,16,31},
    ['0']={14,17,19,21,25,17,14},['1']={4,12,4,4,4,4,14},
    ['2']={14,17,1,2,4,8,31},  ['3']={30,1,1,14,1,1,30},
    ['4']={2,6,10,18,31,2,2},  ['5']={31,16,30,1,1,17,14},
    ['6']={6,8,16,30,17,17,14},['7']={31,1,2,4,8,8,8},
    ['8']={14,17,17,14,17,17,14},['9']={14,17,17,15,1,2,12},
    [':']={0,4,4,0,4,4,0},     ['.']={0,0,0,0,0,12,12},
    ['+']={0,4,4,31,4,4,0},    ['-']={0,0,0,31,0,0,0},
    ['/']={1,1,2,4,8,16,16},   ['=']={0,31,0,31,0,0,0}
};

static int maximum(int a, int b) { return a > b ? a : b; }
static float absolute(float v) { return v < 0.0f ? -v : v; }

static void pixel(ANativeWindow_Buffer *b, int x, int y, uint32_t color) {
    if ((unsigned)x >= (unsigned)b->width || (unsigned)y >= (unsigned)b->height)
        return;
    ((uint32_t *)b->bits)[y * b->stride + x] = color;
}

static void rectangle(ANativeWindow_Buffer *b, int x0, int y0,
                      int x1, int y1, uint32_t color) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > b->width) x1 = b->width;
    if (y1 > b->height) y1 = b->height;
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t *p = (uint32_t *)b->bits;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            p[y * b->stride + x] = color;
}

// Bresenham line drawing (with clipped pixels).
static void line(ANativeWindow_Buffer *b, int x0, int y0, int x1, int y1,
                 uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        pixel(b, x0, y0, color);
        pixel(b, x0 + 1, y0, color);
        pixel(b, x0, y0 + 1, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

static void circle(ANativeWindow_Buffer *b, int cx, int cy, int radius,
                   uint32_t color) {
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x)
            if (x * x + y * y <= radius * radius)
                pixel(b, cx + x, cy + y, color);
}

static void text(ANativeWindow_Buffer *b, const char *s, int x0, int y0,
                 int scale, uint32_t color) {
    for (int i = 0; s[i]; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 128) continue;
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (FONT[c][row] & (1u << (4 - col)))
                    rectangle(b, x0 + (i * 6 + col) * scale,
                              y0 + row * scale,
                              x0 + (i * 6 + col + 1) * scale,
                              y0 + (row + 1) * scale, color);
    }
}

static void textCentered(ANativeWindow_Buffer *b, const char *s,
                         int y, int scale, uint32_t color) {
    int width = ((int)strlen(s) * 6 - 1) * scale;
    text(b, s, (b->width - width) / 2, y, scale, color);
}

// Must be called with app->mutex held.
static void render(App *app) {
    if (!app->window) return;
    ANativeWindow_Buffer b;
    if (ANativeWindow_lock(app->window, &b, NULL) != 0) return;

    rectangle(&b, 0, 0, b.width, b.height, BG);
    int titleScale = maximum(2, b.width / 145);
    int smallScale = maximum(1, b.width / 230);

    textCentered(&b, "PHONE MOTION PATH", b.height * 5 / 100,
                 titleScale, TEXT);
    textCentered(&b, "RELATIVE 2D TRACKING", b.height * 10 / 100,
                 smallScale, MUTED);

    int x0 = b.width * 6 / 100;
    int x1 = b.width * 94 / 100;
    int y0 = b.height * 16 / 100;
    int y1 = b.height * 79 / 100;
    rectangle(&b, x0, y0, x1, y1, PANEL);

    int cx = (x0 + x1) / 2;
    int cy = (y0 + y1) / 2;

    for (int i = 1; i < 10; ++i) {
        int gx = x0 + (x1 - x0) * i / 10;
        int gy = y0 + (y1 - y0) * i / 10;
        line(&b, gx, y0, gx, y1, GRID);
        line(&b, x0, gy, x1, gy, GRID);
    }
    line(&b, cx, y0, cx, y1, AXIS);
    line(&b, x0, cy, x1, cy, AXIS);

    // Automatic map scaling, keeping the origin centered.
    float largest = 1.0f;
    for (int i = 0; i < app->pointCount; ++i) {
        if (absolute(app->pathX[i]) > largest)
            largest = absolute(app->pathX[i]);
        if (absolute(app->pathY[i]) > largest)
            largest = absolute(app->pathY[i]);
    }
    float halfWidth = (float)(x1 - x0) * 0.45f;
    float halfHeight = (float)(y1 - y0) * 0.45f;
    float scaleX = halfWidth / largest;
    float scaleY = halfHeight / largest;
    float mapScale = scaleX < scaleY ? scaleX : scaleY;

    for (int i = 1; i < app->pointCount; ++i) {
        int ax = cx + (int)(app->pathX[i-1] * mapScale);
        int ay = cy - (int)(app->pathY[i-1] * mapScale);
        int bx = cx + (int)(app->pathX[i] * mapScale);
        int by = cy - (int)(app->pathY[i] * mapScale);
        line(&b, ax, ay, bx, by, CYAN);
    }

    circle(&b, cx, cy, maximum(5, b.width / 110), GREEN);
    text(&b, "START", cx + 12, cy + 10, smallScale, GREEN);

    int phoneX = cx + (int)(app->x * mapScale);
    int phoneY = cy - (int)(app->y * mapScale);
    circle(&b, phoneX, phoneY, maximum(10, b.width / 48), GRID);
    circle(&b, phoneX, phoneY, maximum(5, b.width / 95), CYAN);

    char label[96];
    snprintf(label, sizeof(label), "X: %+.2f M", (double)app->x);
    text(&b, label, x0 + 10, b.height * 83 / 100,
         smallScale, TEXT);
    snprintf(label, sizeof(label), "Y: %+.2f M", (double)app->y);
    text(&b, label, x0 + 10, b.height * 88 / 100,
         smallScale, TEXT);

    const char *status = app->sensorsOK == 0 ? "REQUIRED SENSORS MISSING" :
                         app->sensorsOK < 0 ? "STARTING SENSORS" :
                         !app->gotAcceleration ? "WAITING FOR MOVEMENT DATA" :
                         "APPROXIMATE PATH - DRIFT EXPECTED";
    textCentered(&b, status, b.height * 95 / 100,
                 maximum(1, b.width / 280), MUTED);

    ANativeWindow_unlockAndPost(app->window);
}

// Convert phone-frame linear acceleration to world-frame acceleration.
// Android rotation-vector quaternion maps device axes into world axes.
static void rotateToWorld(const App *app, float ax, float ay, float az,
                          float *wx, float *wy) {
    float qx = app->qx, qy = app->qy, qz = app->qz, qw = app->qw;
    float tx = 2.0f * (qy * az - qz * ay);
    float ty = 2.0f * (qz * ax - qx * az);
    float tz = 2.0f * (qx * ay - qy * ax);
    *wx = ax + qw * tx + (qy * tz - qz * ty);
    *wy = ay + qw * ty + (qz * tx - qx * tz);
}

// Integrate motion using sensor event timestamps. Deliberately adds damping
// to reduce unbounded drift while stationary; this sacrifices accuracy.
static void integrateAcceleration(App *app, const ASensorEvent *e) {
    app->gotAcceleration = 1;
    if (!app->gotOrientation) return;

    if (app->lastAccelerationNs == 0) {
        app->lastAccelerationNs = e->timestamp;
        return;
    }
    float dt = (float)((double)(e->timestamp - app->lastAccelerationNs) * 1e-9);
    app->lastAccelerationNs = e->timestamp;
    if (dt <= 0.0f || dt > 0.12f) return;

    float wx, wy;
    rotateToWorld(app, e->acceleration.x, e->acceleration.y,
                  e->acceleration.z, &wx, &wy);
    app->accelerationX = wx;
    app->accelerationY = wy;

    // Ignore small accelerations caused by sensor noise.
    if (wx * wx + wy * wy < 0.20f * 0.20f) {
        wx = 0.0f;
        wy = 0.0f;
    }

    // Velocity and displacement from acceleration (dead reckoning).
    app->vx += wx * dt;
    app->vy += wy * dt;
    float damping = 1.0f - 1.4f * dt;
    if (damping < 0.0f) damping = 0.0f;
    app->vx *= damping;
    app->vy *= damping;
    if (wx == 0.0f && wy == 0.0f &&
        app->vx * app->vx + app->vy * app->vy < 0.0025f) {
        app->vx = 0.0f;
        app->vy = 0.0f;
    }

    app->x += app->vx * dt;
    app->y += app->vy * dt;

    // Cap extreme drift and remember the polyline.
    if (app->x > 10000.0f) app->x = 10000.0f;
    if (app->x < -10000.0f) app->x = -10000.0f;
    if (app->y > 10000.0f) app->y = 10000.0f;
    if (app->y < -10000.0f) app->y = -10000.0f;

    if (app->pointCount == MAX_POINTS) {
        // Preserve the starting point and discard one old mid-path point.
        memmove(app->pathX + 1, app->pathX + 2,
                (MAX_POINTS - 2) * sizeof(float));
        memmove(app->pathY + 1, app->pathY + 2,
                (MAX_POINTS - 2) * sizeof(float));
        app->pointCount--;
    }
    app->pathX[app->pointCount] = app->x;
    app->pathY[app->pointCount] = app->y;
    app->pointCount++;
}

static void *sensorWorker(void *arg) {
    App *app = (App *)arg;
    ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    ASensorManager *manager = ASensorManager_getInstance();
    const ASensor *linear = manager ? ASensorManager_getDefaultSensor(
        manager, ASENSOR_TYPE_LINEAR_ACCELERATION) : NULL;
    const ASensor *rotation = manager ? ASensorManager_getDefaultSensor(
        manager, ASENSOR_TYPE_GAME_ROTATION_VECTOR) : NULL;
    if (!rotation && manager)
        rotation = ASensorManager_getDefaultSensor(
            manager, ASENSOR_TYPE_ROTATION_VECTOR);

    ASensorEventQueue *queue = manager && looper && linear && rotation ?
        ASensorManager_createEventQueue(manager, looper, 1, NULL, NULL) : NULL;
    int ok = 0;
    if (queue) {
        int l = ASensorEventQueue_enableSensor(queue, linear);
        int r = ASensorEventQueue_enableSensor(queue, rotation);
        ok = (l == 0 && r == 0);
        if (ok) {
            ASensorEventQueue_setEventRate(queue, linear, 20000);
            ASensorEventQueue_setEventRate(queue, rotation, 20000);
        }
    }

    pthread_mutex_lock(&app->mutex);
    app->sensorsOK = ok;
    render(app);
    pthread_mutex_unlock(&app->mutex);

    int64_t lastRender = 0;
    while (1) {
        pthread_mutex_lock(&app->mutex);
        int stop = app->stop;
        pthread_mutex_unlock(&app->mutex);
        if (stop) break;

        int ident = ALooper_pollOnce(50, NULL, NULL, NULL);
        if (ident != 1 || !ok) continue;

        ASensorEvent batch[32];
        ssize_t count;
        int updated = 0;
        int64_t newest = lastRender;
        while ((count = ASensorEventQueue_getEvents(queue, batch, 32)) > 0) {
            pthread_mutex_lock(&app->mutex);
            for (ssize_t i = 0; i < count; ++i) {
                const ASensorEvent *e = &batch[i];
                if (e->type == ASENSOR_TYPE_GAME_ROTATION_VECTOR ||
                    e->type == ASENSOR_TYPE_ROTATION_VECTOR) {
                    app->qx = e->data[0];
                    app->qy = e->data[1];
                    app->qz = e->data[2];
                    app->qw = e->data[3];
                    app->gotOrientation = 1;
                } else if (e->type == ASENSOR_TYPE_LINEAR_ACCELERATION) {
                    integrateAcceleration(app, e);
                    updated = 1;
                }
                if (e->timestamp > newest) newest = e->timestamp;
            }
            if (updated && (lastRender == 0 ||
                newest - lastRender >= 33000000LL)) {
                render(app);
                lastRender = newest;
            }
            pthread_mutex_unlock(&app->mutex);
        }
    }

    if (queue) {
        if (linear) ASensorEventQueue_disableSensor(queue, linear);
        if (rotation) ASensorEventQueue_disableSensor(queue, rotation);
        ASensorManager_destroyEventQueue(manager, queue);
    }
    return NULL;
}

static void startWorker(App *app) {
    if (!app->resumed || !app->window || app->running) return;
    app->stop = 0;
    app->gotOrientation = 0;
    app->lastAccelerationNs = 0;
    app->vx = app->vy = 0.0f;
    if (pthread_create(&app->worker, NULL, sensorWorker, app) == 0)
        app->running = 1;
}

static void stopWorker(App *app) {
    if (!app->running) return;
    pthread_mutex_lock(&app->mutex);
    app->stop = 1;
    pthread_mutex_unlock(&app->mutex);
    pthread_join(app->worker, NULL);
    app->running = 0;
}

static void onWindowCreated(ANativeActivity *activity, ANativeWindow *window) {
    App *app = (App *)activity->instance;
    pthread_mutex_lock(&app->mutex);
    app->window = window;
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
    render(app);
    pthread_mutex_unlock(&app->mutex);
    startWorker(app);
}

static void onWindowDestroyed(ANativeActivity *activity, ANativeWindow *window) {
    (void)window;
    App *app = (App *)activity->instance;
    stopWorker(app);
    pthread_mutex_lock(&app->mutex);
    app->window = NULL;
    pthread_mutex_unlock(&app->mutex);
}

static void onWindowRedraw(ANativeActivity *activity, ANativeWindow *window) {
    (void)window;
    App *app = (App *)activity->instance;
    pthread_mutex_lock(&app->mutex);
    render(app);
    pthread_mutex_unlock(&app->mutex);
}

static void onResume(ANativeActivity *activity) {
    App *app = (App *)activity->instance;
    app->resumed = 1;
    startWorker(app);
}

static void onPause(ANativeActivity *activity) {
    App *app = (App *)activity->instance;
    app->resumed = 0;
    stopWorker(app);
}

static void onDestroy(ANativeActivity *activity) {
    App *app = (App *)activity->instance;
    stopWorker(app);
    pthread_mutex_destroy(&app->mutex);
    free(app);
    activity->instance = NULL;
}

void ANativeActivity_onCreate(ANativeActivity *activity,
                             void *savedState, size_t savedStateSize) {
    (void)savedState;
    (void)savedStateSize;
    App *app = (App *)calloc(1, sizeof(App));
    if (!app) return;
    pthread_mutex_init(&app->mutex, NULL);
    app->sensorsOK = -1;
    app->pointCount = 1; // first path point = (0,0)
    activity->instance = app;
    activity->callbacks->onNativeWindowCreated = onWindowCreated;
    activity->callbacks->onNativeWindowDestroyed = onWindowDestroyed;
    activity->callbacks->onNativeWindowRedrawNeeded = onWindowRedraw;
    activity->callbacks->onNativeWindowResized = onWindowRedraw;
    activity->callbacks->onResume = onResume;
    activity->callbacks->onPause = onPause;
    activity->callbacks->onDestroy = onDestroy;
}
