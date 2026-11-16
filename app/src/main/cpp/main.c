#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/sensor.h>
#include <android/looper.h>
#include <android/window.h>

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Simple 5x7 bitmap font
static const uint8_t FONT[128][7] = {
    ['A']={14,17,17,31,17,17,17},
    ['C']={14,17,16,16,16,17,14},
    ['E']={31,16,16,30,16,16,31},
    ['G']={14,17,16,23,17,17,14},
    ['I']={31,4,4,4,4,4,31},
    ['L']={16,16,16,16,16,16,31},
    ['M']={17,27,21,21,17,17,17},
    ['N']={17,25,21,19,17,17,17},
    ['O']={14,17,17,17,17,17,14},
    ['R']={30,17,17,30,20,18,17},
    ['S']={15,16,16,14,1,1,30},
    ['T']={31,4,4,4,4,4,4},
    ['W']={17,17,17,21,21,21,10},
    ['X']={17,17,10,4,10,17,17},
    ['Y']={17,17,10,4,4,4,4},
    ['Z']={31,1,2,4,8,16,31},

    ['0']={14,17,19,21,25,17,14},
    ['1']={4,12,4,4,4,4,14},
    ['2']={14,17,1,2,4,8,31},
    ['3']={30,1,1,14,1,1,30},
    ['4']={2,6,10,18,31,2,2},
    ['5']={31,16,30,1,1,17,14},
    ['6']={6,8,16,30,17,17,14},
    ['7']={31,1,2,4,8,8,8},
    ['8']={14,17,17,14,17,17,14},
    ['9']={14,17,17,15,1,2,12},

    [':']={0,4,4,0,4,4,0},
    ['+']={0,4,4,31,4,4,0},
    ['-']={0,0,0,31,0,0,0},
    ['.']={0,0,0,0,0,12,12},
    ['/']={1,1,2,4,8,16,16}
};

typedef struct {
    ANativeWindow *window;
    pthread_t thread;
    pthread_mutex_t mutex;

    int running;
    int stop;
    int resumed;
    int sensorAvailable;
    int hasData;

    float x, y, z;
} App;

// Draw text onto the native window buffer
static void drawText(
    ANativeWindow_Buffer *b,
    const char *str,
    int x0, int y0, int scale,
    uint32_t color
) {
    uint32_t *pixels = (uint32_t *)b->bits;

    for (int i = 0; str[i]; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c >= 128) continue;

        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (!(FONT[c][row] & (1 << (4-col))))
                    continue;

                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        int x = x0 + (i*6+col)*scale+dx;
                        int y = y0 + row*scale+dy;

                        if (x >= 0 && x < b->width &&
                            y >= 0 && y < b->height) {
                            pixels[y*b->stride+x] = color;
                        }
                    }
                }
            }
        }
    }
}

static void centerText(
    ANativeWindow_Buffer *b,
    const char *str,
    int y, int scale, uint32_t color
) {
    int w = ((int)strlen(str)*6-1)*scale;
    drawText(b, str, (b->width-w)/2,
             y, scale, color);
}

// Call only while holding the mutex
static void render(App *app) {
    if (!app->window) return;

    ANativeWindow_Buffer b;

    if (ANativeWindow_lock(app->window, &b, NULL) != 0)
        return;

    uint32_t *pixels = (uint32_t *)b.bits;

    // Dark background
    for (int y=0; y<b.height; y++)
        for (int x=0; x<b.width; x++)
            pixels[y*b.stride+x] = 0xFF171C29;

    int scale = b.width / 90;
    if (scale > b.height / 80)
        scale = b.height / 80;
    if (scale < 1) scale = 1;

    int top = b.height/2 - 31*scale;

    centerText(&b, "ACCELEROMETER",
               top, scale, 0xFFFFFFFF);

    if (app->sensorAvailable == 0) {
        centerText(&b, "NO SENSOR",
                   top+25*scale, scale,
                   0xFFFFFFFF);
    } else if (!app->hasData) {
        centerText(&b, "WAITING...",
                   top+25*scale, scale,
                   0xFFFFFFFF);
    } else {
        char line[64];

        snprintf(line, sizeof(line),
                 "X: %+07.2f", app->x);
        centerText(&b, line, top+18*scale,
                   scale, 0xFFFFFFFF);

        snprintf(line, sizeof(line),
                 "Y: %+07.2f", app->y);
        centerText(&b, line, top+31*scale,
                   scale, 0xFFFFFFFF);

        snprintf(line, sizeof(line),
                 "Z: %+07.2f", app->z);
        centerText(&b, line, top+44*scale,
                   scale, 0xFFFFFFFF);

        centerText(&b, "M/S2", top+60*scale,
                   scale, 0xFFFFFFFF);
    }

    ANativeWindow_unlockAndPost(app->window);
}

// Sensor worker thread
static void *sensorThread(void *arg) {
    App *app = (App *)arg;

    ALooper *looper = ALooper_prepare(
        ALOOPER_PREPARE_ALLOW_NON_CALLBACKS
    );

    ASensorManager *manager =
        ASensorManager_getInstance();

    const ASensor *sensor = manager ?
        ASensorManager_getDefaultSensor(
            manager, ASENSOR_TYPE_ACCELEROMETER
        ) : NULL;

    ASensorEventQueue *queue =
        (sensor && looper) ?
        ASensorManager_createEventQueue(
            manager, looper, 1, NULL, NULL
        ) : NULL;

    int enabled = queue &&
        ASensorEventQueue_enableSensor(
            queue, sensor
        ) == 0;

    if (enabled) {
        // Request approximately 20 samples/second
        int rate = 50000;
        int minRate = ASensor_getMinDelay(sensor);
        if (minRate > rate) rate = minRate;

        ASensorEventQueue_setEventRate(
            queue, sensor, rate
        );
    }

    pthread_mutex_lock(&app->mutex);
    app->sensorAvailable = enabled ? 1 : 0;
    render(app);
    pthread_mutex_unlock(&app->mutex);

    while (1) {
        pthread_mutex_lock(&app->mutex);
        int stop = app->stop;
        pthread_mutex_unlock(&app->mutex);

        if (stop) break;

        int ident = ALooper_pollOnce(
            50, NULL, NULL, NULL
        );

        if (ident == 1 && enabled) {
            ASensorEvent events[16];

            ssize_t count =
                ASensorEventQueue_getEvents(
                    queue, events, 16
                );

            for (ssize_t i=0; i<count; i++) {
                if (events[i].type !=
                    ASENSOR_TYPE_ACCELEROMETER)
                    continue;

                pthread_mutex_lock(&app->mutex);

                app->x = events[i].acceleration.x;
                app->y = events[i].acceleration.y;
                app->z = events[i].acceleration.z;
                app->hasData = 1;

                render(app);

                pthread_mutex_unlock(&app->mutex);
            }
        }
    }

    if (queue) {
        if (enabled)
            ASensorEventQueue_disableSensor(
                queue, sensor
            );

        ASensorManager_destroyEventQueue(
            manager, queue
        );
    }

    return NULL;
}

static void startWorker(App *app) {
    if (!app->resumed || !app->window ||
        app->running) return;

    app->stop = 0;

    if (pthread_create(&app->thread, NULL,
                       sensorThread, app) == 0)
        app->running = 1;
}

static void stopWorker(App *app) {
    if (!app->running) return;

    pthread_mutex_lock(&app->mutex);
    app->stop = 1;
    pthread_mutex_unlock(&app->mutex);

    pthread_join(app->thread, NULL);
    app->running = 0;
}

static void onWindowCreated(
    ANativeActivity *activity,
    ANativeWindow *window
) {
    App *app = (App *)activity->instance;

    pthread_mutex_lock(&app->mutex);

    app->window = window;
    app->sensorAvailable = -1;

    ANativeWindow_setBuffersGeometry(
        window, 0, 0, WINDOW_FORMAT_RGBA_8888
    );

    render(app);
    pthread_mutex_unlock(&app->mutex);

    startWorker(app);
}

static void onWindowDestroyed(
    ANativeActivity *activity,
    ANativeWindow *window
) {
    (void)window;
    App *app = (App *)activity->instance;

    stopWorker(app);

    pthread_mutex_lock(&app->mutex);
    app->window = NULL;
    pthread_mutex_unlock(&app->mutex);
}

static void onRedraw(
    ANativeActivity *activity,
    ANativeWindow *window
) {
    (void)window;
    App *app = (App *)activity->instance;

    pthread_mutex_lock(&app->mutex);
    render(app);
    pthread_mutex_unlock(&app->mutex);
}

static void onResume(
    ANativeActivity *activity
) {
    App *app = (App *)activity->instance;
    app->resumed = 1;
    startWorker(app);
}

static void onPause(
    ANativeActivity *activity
) {
    App *app = (App *)activity->instance;
    app->resumed = 0;
    stopWorker(app);
}

static void onDestroy(
    ANativeActivity *activity
) {
    App *app = (App *)activity->instance;

    stopWorker(app);
    pthread_mutex_destroy(&app->mutex);
    free(app);
    activity->instance = NULL;
}

// Native Android entry point
void ANativeActivity_onCreate(
    ANativeActivity *activity,
    void *savedState,
    size_t savedStateSize
) {
    (void)savedState;
    (void)savedStateSize;

    App *app = (App *)calloc(1, sizeof(App));
    if (!app) return;

    pthread_mutex_init(&app->mutex, NULL);
    app->sensorAvailable = -1;

    activity->instance = app;

    activity->callbacks->onNativeWindowCreated =
        onWindowCreated;

    activity->callbacks->onNativeWindowDestroyed =
        onWindowDestroyed;

    activity->callbacks->onNativeWindowRedrawNeeded =
        onRedraw;

    activity->callbacks->onNativeWindowResized =
        onRedraw;

    activity->callbacks->onResume = onResume;
    activity->callbacks->onPause = onPause;
    activity->callbacks->onDestroy = onDestroy;
}
