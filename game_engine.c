#include "game_engine.h"
#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include "clock_timer.h"

typedef _Atomic uint32_t AtomicUint32;

GameEngineSettings game_engine_settings_init() {
    GameEngineSettings settings;
    settings.fps = 60.0f;
    settings.show_fps = false;
    settings.callback = NULL;
    settings.context = NULL;
    return settings;
}

struct GameEngine {
    Gui* gui;
    FuriPubSub* input_pubsub;
    FuriThreadId thread_id;
    GameEngineSettings settings;
};

struct RunningGameEngine {
    GameEngine* engine;
    float fps;
};

typedef enum {
    GameThreadFlagUpdate = 1 << 0,
    GameThreadFlagStop = 1 << 1,
} GameThreadFlag;

#define GameThreadFlagMask (GameThreadFlagUpdate | GameThreadFlagStop)

GameEngine* game_engine_alloc(GameEngineSettings settings) {
    furi_check(settings.callback != NULL);

    GameEngine* engine = malloc(sizeof(GameEngine));
    engine->gui = furi_record_open(RECORD_GUI);
    engine->input_pubsub = furi_record_open(RECORD_INPUT_EVENTS);
    engine->thread_id = furi_thread_get_current_id();
    engine->settings = settings;

    return engine;
}

void game_engine_free(GameEngine* engine) {
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_INPUT_EVENTS);
    free(engine);
}

static void canvas_printf(Canvas* canvas, uint8_t x, uint8_t y, const char* format, ...) {
    FuriString* string = furi_string_alloc();
    va_list args;
    va_start(args, format);
    furi_string_vprintf(string, format, args);
    va_end(args);

    canvas_draw_str(canvas, x, y, furi_string_get_cstr(string));

    furi_string_free(string);
}

static void clock_timer_callback(void* context) {
    GameEngine* engine = context;
    furi_thread_flags_set(engine->thread_id, GameThreadFlagUpdate);
}

static const GameKey keys[] = {
    [InputKeyUp] = GameKeyUp,
    [InputKeyDown] = GameKeyDown,
    [InputKeyRight] = GameKeyRight,
    [InputKeyLeft] = GameKeyLeft,
    [InputKeyOk] = GameKeyOk,
    [InputKeyBack] = GameKeyBack,
};

static const size_t keys_count = sizeof(keys) / sizeof(keys[0]);

static void input_events_callback(const void* value, void* context) {
    AtomicUint32* input_state = context;
    const InputEvent* event = value;

    if(event->key < keys_count) {
        switch(event->type) {
        case InputTypePress:
            *input_state |= (keys[event->key]);
            break;
        case InputTypeRelease:
            *input_state &= ~(keys[event->key]);
            break;
        default:
            break;
        }
    }
}

void game_engine_run(GameEngine* engine) {
    // create running engine
    RunningGameEngine run = {
        .engine = engine,
    };

    // input state
    AtomicUint32 input_state = 0;
    uint32_t input_prev_state = 0;

    // acquire gui canvas
    Canvas* canvas = gui_direct_draw_acquire(engine->gui);

    // subscribe to input events
    FuriPubSubSubscription* input_subscription =
        furi_pubsub_subscribe(engine->input_pubsub, input_events_callback, &input_state);

    // start "game update" timer
    clock_timer_start(clock_timer_callback, engine, engine->settings.fps);

    // init fps counter
    uint32_t time_start = DWT->CYCCNT;

    while(true) {
        uint32_t flags =
            furi_thread_flags_wait(GameThreadFlagMask, FuriFlagWaitAny, FuriWaitForever);
        furi_check((flags & FuriFlagError) == 0);

        if(flags & GameThreadFlagUpdate) {
            // update fps counter
            uint32_t time_end = DWT->CYCCNT;
            uint32_t time_delta = time_end - time_start;
            time_start = time_end;

            // update input state
            uint32_t input_current_state = input_state;
            InputState input = {
                .held = input_current_state,
                .pressed = input_current_state & ~input_prev_state,
                .released = ~input_current_state & input_prev_state,
            };
            input_prev_state = input_current_state;

            // clear screen
            canvas_reset(canvas);

            // calculate actual fps
            run.fps = (float)SystemCoreClock / time_delta;

            // do the work
            engine->settings.callback(&run, canvas, input, engine->settings.context);

            // show fps if needed
            if(engine->settings.show_fps) {
                canvas_set_color(canvas, ColorXOR);
                canvas_printf(canvas, 0, 7, "%u", (uint32_t)roundf(run.fps));
            }

            // and output screen buffer
            canvas_commit(canvas);
        }

        if(flags & GameThreadFlagStop) {
            break;
        }
    }

    // stop timer
    clock_timer_stop();

    // release gui canvas and unsubscribe from input events
    gui_direct_draw_release(engine->gui);
    furi_pubsub_unsubscribe(engine->input_pubsub, input_subscription);
}

void running_game_engine_stop(RunningGameEngine* run) {
    furi_thread_flags_set(run->engine->thread_id, GameThreadFlagStop);
}

float running_game_engine_get_delta_time(RunningGameEngine* engine) {
    return 1.0f / engine->fps;
}

float running_game_engine_get_delta_frames(RunningGameEngine* engine) {
    return engine->fps / engine->engine->settings.fps;
}