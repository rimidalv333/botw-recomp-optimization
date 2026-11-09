// The PS5's input devices behind the SDL calls runtime/hle/input.c, bindings.c and vibration.c make (D2, D8): the
// signed-in user's DualSense through scePad, and USB keyboards and mice through sceKeyboard / sceMouse. They fill the
// same host state the desktop's SDL devices fill, so the Switch-controller mapping, rebinding and settings stay the
// desktop's: the DualSense is an SDL PS5 gamepad, its buttons and axes numbered and scaled as SDL's own DualSense driver
// reports them; a keyboard's keys are SDL scancodes (both are USB HID usages); a mouse is SDL's relative mouse.
//
// Lifecycle (docs/PS5-PORT-PLAN.md D8, ../ps5-native-gamepad-input-research CONTROLLER-DATA.md): the user service gives
// the signed-in user, whose standard pad this file opens and alone reads (the window loop's thread, through
// SDL_PollEvent: ps5_input_poll). Each poll drains scePadRead oldest-first; the newest usable record is published as
// the current state, and its changes go out as SDL events; every record's accelerometer and gyroscope go out as SDL
// sensor events at the record's own time (runtime/hle/motion.c fuses them). A record that is disconnected or
// intercepted by the system, or from a new connection generation, removes the gamepad (input.c then reads nothing
// pressed, sticks centred; motion.c forgets its orientation, timing and calibration; the motors stop); the next usable
// record adds it again as a new gamepad, its motion sensors configured afresh. Rumble (vibration.c) is
// scePadSetVibration. A user logging out closes the pad; a user logging in while none is signed in opens theirs.
//
// Keyboards and mice (../ps5-native-gamepad-input-research KEYBOARD-MOUSE.md): their sysmodules are loaded once, then
// device indexes 0 and 1 of each are opened for the signed-in user (one receiver can put its keyboard on one index and
// its mouse on the other); an index that does not open is tried again each second. An open handle stays open across
// unplugging: its records say whether a device is connected, and a replugged one reports on the same handle. Every
// record is processed oldest-first, so a tap between two polls still goes out as a press and a release: a keyboard's
// held usages (its modifier bits as Left Control..Right GUI) become key down / up events, a mouse's buttons button
// events; a poll's motion and wheel, summed over every record of every mouse, go out as one motion and one wheel event.
// A disconnected or intercepted record holds nothing. A key or button is down while any device holds it.
//
// The klog records device lifecycle and read errors, not every controller state transition or motion sample.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../input_bind.h"
#include "../rt.h"

// ---- the console's libraries (no SDK headers: declarations as ../ps5-native-gamepad-input-research has them)
typedef struct { int32_t event_type; int32_t user_id; } UserServiceEvent;  // event_type: 0 login, 1 logout
int32_t sceUserServiceInitialize(const void *params);
int32_t sceUserServiceGetInitialUser(int32_t *user_id);
int32_t sceUserServiceGetEvent(UserServiceEvent *event);
int32_t scePadInit(void);
int32_t scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void *params);
int32_t scePadClose(int32_t handle);
int32_t scePadRead(int32_t handle, void *records, int32_t capacity);
int32_t scePadReadState(int32_t handle, void *record);  // (the motion line's once-a-second comparison only)
int32_t scePadSetMotionSensorState(int32_t handle, bool enabled);
int32_t scePadSetTiltCorrectionState(int32_t handle, bool enabled);
int32_t scePadSetAngularVelocityDeadbandState(int32_t handle, bool enabled);
int32_t scePadResetOrientation(int32_t handle);
typedef struct {
    float touch_pixel_density;
    uint16_t touch_width, touch_height;
    uint8_t dead_zone_left, dead_zone_right;
    uint8_t connection_type;   // 0: a controller on this console; others: remote (Remote Play)
    uint8_t connected_count;
    int32_t connected;
    int32_t device_class;      // 0: standard
    uint8_t reserved[8];
} PadInformation;
_Static_assert(sizeof(PadInformation) == 28, "scePadGetControllerInformation");
int32_t scePadGetControllerInformation(int32_t handle, PadInformation *info);
typedef struct { uint8_t large_motor, small_motor; } PadVibration;
int32_t scePadSetVibration(int32_t handle, const PadVibration *vibration);

// scePadRead's 120-byte record
typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;        // 0..255, 128 centred, y down
    uint8_t l2, r2, reserved0[2];  // trigger travel 0..255
    float orientation[4];          // x, y, z, w
    float accel[3];                // G
    float gyro[3];                 // rad/s
    uint8_t touch[24];
    int32_t connected;
    uint64_t timestamp_us;         // the sample's time (process time, microseconds)
    uint8_t extension[16];
    uint8_t connected_count;       // the connection's generation
    uint8_t reserved1[2];
    uint8_t unique_length;
    uint8_t unique[12];
} PadRecord;
_Static_assert(sizeof(PadRecord) == 120, "scePadRead record");
_Static_assert(offsetof(PadRecord, accel) == 0x1c && offsetof(PadRecord, gyro) == 0x28, "motion fields");
_Static_assert(offsetof(PadRecord, connected) == 0x4c && offsetof(PadRecord, timestamp_us) == 0x50, "state fields");
_Static_assert(offsetof(PadRecord, connected_count) == 0x68, "generation field");

enum {
    PAD_CREATE = 1u << 0, PAD_L3 = 1u << 1, PAD_R3 = 1u << 2, PAD_OPTIONS = 1u << 3, PAD_UP = 1u << 4,
    PAD_RIGHT = 1u << 5, PAD_DOWN = 1u << 6, PAD_LEFT = 1u << 7, PAD_L2 = 1u << 8, PAD_R2 = 1u << 9,
    PAD_L1 = 1u << 10, PAD_R1 = 1u << 11, PAD_TRIANGLE = 1u << 12, PAD_CIRCLE = 1u << 13, PAD_CROSS = 1u << 14,
    PAD_SQUARE = 1u << 15, PAD_TOUCHPAD = 1u << 20, PAD_INTERCEPTED = 1u << 31,
};
#define PAD_RECORDS 64
#define LOG_PER_SECOND 30
#define USER_SERVICE_NO_EVENT 0x80960007u  // sceUserServiceGetEvent: nothing new

// ---- keyboards and mice (declarations and records as ../ps5-native-gamepad-input-research include/ps5_keyboard.hpp
// and ps5_mouse.hpp have them)
int32_t sceSysmoduleLoadModule(uint16_t id);
#define SYSMODULE_KEYBOARD 0x0106
#define SYSMODULE_MOUSE 0x00a9
int32_t sceKeyboardInit(void);
int32_t sceKeyboardOpen(int32_t user_id, int32_t type, int32_t index, const void *params);  // (8 reserved bytes)
int32_t sceKeyboardClose(int32_t handle);
int32_t sceKeyboardRead(int32_t handle, void *records, int32_t capacity);  // 1..16 records, oldest first
int32_t sceMouseInit(void);
int32_t sceMouseOpen(int32_t user_id, int32_t type, int32_t index, const void *params);  // (8 bytes; 0: one index)
int32_t sceMouseClose(int32_t handle);
int32_t sceMouseRead(int32_t handle, void *records, int32_t capacity);  // 1..64 records, oldest first

// sceKeyboardRead's 96-byte record
typedef struct {
    uint64_t timestamp_us;
    uint8_t intercepted, reserved0[7];  // nonzero: the system has the keyboard
    uint8_t connected, reserved1[3];
    int32_t length;
    uint32_t leds;                      // Num, Caps, Scroll Lock
    uint32_t modifiers;                 // bits 0..7: Left Control, Shift, Alt, GUI, then the right ones
    uint16_t keys[16];                  // the USB HID usages held (0: none; 1..3: a rollover / error report)
    uint8_t reserved2[32];
} KeyboardRecord;
_Static_assert(sizeof(KeyboardRecord) == 96, "sceKeyboardRead record");
_Static_assert(offsetof(KeyboardRecord, connected) == 0x10 && offsetof(KeyboardRecord, modifiers) == 0x1c &&
               offsetof(KeyboardRecord, keys) == 0x20, "keyboard fields");
// sceMouseRead's 40-byte record
typedef struct {
    uint64_t timestamp_us;
    uint8_t connected, reserved0[3];
    uint32_t buttons;                   // bits 0..4: primary, secondary, middle, X1, X2; MOUSE_INTERCEPTED
    int32_t x, y, wheel, tilt;          // relative: right, down, wheel away from the player, wheel tilted right
    uint8_t reserved1[8];
} MouseRecord;
_Static_assert(sizeof(MouseRecord) == 40, "sceMouseRead record");
_Static_assert(offsetof(MouseRecord, buttons) == 0x0c && offsetof(MouseRecord, tilt) == 0x1c, "mouse fields");
#define MOUSE_INTERCEPTED 0x80000000u
#define KEYBOARD_RECORDS 16
#define MOUSE_RECORDS 64
#define HID_INDEXES 2  // device indexes per kind (0 and 1 open; higher ones are refused)

// The DualSense buttons SDL's driver reports, in SDL's button numbers (the PS button stays the system's; the L2 / R2
// bits are left out: SDL reports the triggers as axes, and input.c presses ZL / ZR past half their travel)
static const struct { uint32_t bit; uint8_t sdl; const char *name; } k_buttons[] = {
    {PAD_CROSS, SDL_GAMEPAD_BUTTON_SOUTH, "Cross"},     {PAD_CIRCLE, SDL_GAMEPAD_BUTTON_EAST, "Circle"},
    {PAD_SQUARE, SDL_GAMEPAD_BUTTON_WEST, "Square"},    {PAD_TRIANGLE, SDL_GAMEPAD_BUTTON_NORTH, "Triangle"},
    {PAD_CREATE, SDL_GAMEPAD_BUTTON_BACK, "Create"},    {PAD_OPTIONS, SDL_GAMEPAD_BUTTON_START, "Options"},
    {PAD_L3, SDL_GAMEPAD_BUTTON_LEFT_STICK, "L3"},      {PAD_R3, SDL_GAMEPAD_BUTTON_RIGHT_STICK, "R3"},
    {PAD_L1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "L1"},   {PAD_R1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "R1"},
    {PAD_UP, SDL_GAMEPAD_BUTTON_DPAD_UP, "Up"},         {PAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN, "Down"},
    {PAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT, "Left"},   {PAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "Right"},
    {PAD_TOUCHPAD, SDL_GAMEPAD_BUTTON_TOUCHPAD, "Touchpad"},
};
#define NBUTTONS (int)(sizeof k_buttons / sizeof k_buttons[0])

struct SDL_Gamepad { int unused; };  // the one DualSense (input.c holds this while it is open)
static SDL_Gamepad g_gamepad;

static struct {
    bool started;             // the user service and pad library were initialized
    int32_t user;             // the signed-in user whose devices are open (-1: none)
    int32_t handle;           // their pad (-1: not open)
    uint64_t retry_ns;        // when to try opening it again
    bool present;             // an SDL gamepad: connected, not intercepted
    SDL_JoystickID id;        // its SDL id (a new one each time it is added)
    uint8_t generation;       // its connection generation (connected_count)
    uint32_t sdl_buttons;     // SDL buttons held (bit per SDL_GamepadButton)
    Sint16 axis[SDL_GAMEPAD_AXIS_COUNT];
    uint32_t read_errors;
    bool motion;              // its motion sensors are on (scePadSetMotionSensorState took it)
    bool sensor_on[3];        // input.c turned them on (by SDL_SensorType)
    uint32_t motion_samples;  // sensor records in the current rate-measurement window
    uint64_t motion_rate_start_us;
    float motion_hz;
    uint8_t rumble[2];        // the motors' speeds now (large, small)
    uint64_t rumble_until_ns; // SDL's duration: the motors stop then unless asked again
    char rumble_error[96];
} g_pad = {.user = -1, .handle = -1};

static uint64_t g_last_poll_ns;

// ---- events
static void push(SDL_Event *e) {
    e->gdevice.timestamp = plat_now_ns();
    SDL_PushEvent(e);
}
static void push_device(uint32_t type) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.gdevice.which = g_pad.id;
    push(&e);
}

// Rate-limited lines: at most LOG_PER_SECOND a second, then how many were left out
static bool log_allowed(void) {
    static uint64_t window_ns;
    static int lines, dropped;
    uint64_t now = plat_now_ns();
    if (now - window_ns >= 1000000000ull) {
        if (dropped) LOG("[input] (%d input lines left out in the last second)\n", dropped);
        window_ns = now, lines = dropped = 0;
    }
    if (lines < LOG_PER_SECOND) { lines++; return true; }
    dropped++;
    return false;
}

// SDL's DualSense scaling: a stick byte to -32768..32767 (128 -> 128), trigger travel to 0..32767
static Sint16 stick_axis(uint8_t v) { return (Sint16)((int)v * 257 - 32768); }
static Sint16 trigger_axis(uint8_t v) { return (Sint16)((int)v * 257 / 2); }

// ---- the gamepad's state
static void pad_clear_state(void) {
    g_pad.sdl_buttons = 0;
    memset(g_pad.axis, 0, sizeof g_pad.axis);
}

// ---- rumble: SDL's two motors are the DualSense's (low frequency: the large one), each speed's high byte as SDL's own
// DualSense driver sends it
static bool rumble_send(uint8_t large, uint8_t small, const char *why) {
    (void)why;
    PadVibration v = {large, small};
    int32_t r = scePadSetVibration(g_pad.handle, &v);
    if (r < 0) {
        snprintf(g_pad.rumble_error, sizeof g_pad.rumble_error, "scePadSetVibration failed: %#x", (unsigned)r);
        ps5_sdl_error(g_pad.rumble_error);
        return false;
    }
    g_pad.rumble[0] = large, g_pad.rumble[1] = small;
    return true;
}

// ---- motion: every record's accelerometer and gyroscope, at the record's own time, to runtime/hle/motion.c (which
// fuses them; the pad's own orientation is left unused, so nothing is fused twice). Its axes are taken as SDL's (X
// right, Y up out of the face, Z toward the player), as SDL's DualSense driver passes the controller's axes through;
// accelerations come in G and go out in m/s^2, angular velocities in rad/s. That convention is not yet validated on a
// held controller: the [motion] lines show the readings (flat on a table: accelerometer about 0, +1, 0 G).
#define STANDARD_GRAVITY 9.80665f
static void pad_configure_motion(void) {
    int32_t rm = scePadSetMotionSensorState(g_pad.handle, true);
    // (tilt correction only shapes the pad's orientation, which is unused; the deadband would swallow a slow aim,
    // and motion.c learns the gyroscope's bias at rest itself)
    (void)scePadSetTiltCorrectionState(g_pad.handle, false);
    (void)scePadSetAngularVelocityDeadbandState(g_pad.handle, false);
    (void)scePadResetOrientation(g_pad.handle);
    g_pad.motion = rm >= 0;
    memset(g_pad.sensor_on, 0, sizeof g_pad.sensor_on);
    g_pad.motion_samples = 0, g_pad.motion_hz = 0;
    g_pad.motion_rate_start_us = 0;
}

static void pad_motion(const PadRecord *r) {
    if (!g_pad.motion_rate_start_us) g_pad.motion_rate_start_us = r->timestamp_us;
    g_pad.motion_samples++;
    uint64_t span = r->timestamp_us - g_pad.motion_rate_start_us;
    if (span >= 1000000) {
        g_pad.motion_hz = (float)(g_pad.motion_samples * 1000000.0 / (double)span);
        g_pad.motion_samples = 0;
        g_pad.motion_rate_start_us = r->timestamp_us;
    }
    SDL_Event e;
    for (int s = SDL_SENSOR_ACCEL; s <= SDL_SENSOR_GYRO; s++) {
        if (!g_pad.motion || !g_pad.sensor_on[s]) continue;
        const float *d = s == SDL_SENSOR_ACCEL ? r->accel : r->gyro;
        float k = s == SDL_SENSOR_ACCEL ? STANDARD_GRAVITY : 1.0f;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_GAMEPAD_SENSOR_UPDATE;
        e.gsensor.which = g_pad.id, e.gsensor.sensor = s;
        e.gsensor.data[0] = d[0] * k, e.gsensor.data[1] = d[1] * k, e.gsensor.data[2] = d[2] * k;
        e.gsensor.sensor_timestamp = r->timestamp_us * 1000ull;
        push(&e);
    }
}

static void pad_remove(const char *why) {
    if (!g_pad.present) return;
    if (g_pad.rumble[0] || g_pad.rumble[1]) rumble_send(0, 0, " (removed)");
    g_pad.rumble_until_ns = 0;
    g_pad.present = false;
    pad_clear_state();
    push_device(SDL_EVENT_GAMEPAD_REMOVED);
    LOG("[input] DualSense removed (%s): nothing pressed, sticks centred\n", why);
}

static void pad_add(const PadRecord *r) {
    g_pad.present = true;
    g_pad.generation = r->connected_count;
    g_pad.id++;
    pad_clear_state();
    LOG("[input] DualSense added: user %d, pad handle %d, connection generation %u, SDL gamepad %u\n", g_pad.user,
        g_pad.handle, r->connected_count, g_pad.id);
    PadInformation info;
    memset(&info, 0, sizeof info);
    int32_t ri = scePadGetControllerInformation(g_pad.handle, &info);
    LOG("[input] DualSense information %#x: connection type %u (%s), device class %d, stick dead zones %u / %u, "
        "touch pad %ux%u\n", (unsigned)ri, info.connection_type,
        ri < 0 ? "unknown" : info.connection_type == 0 ? "a controller on this console" : "remote, e.g. Remote Play",
        info.device_class, info.dead_zone_left, info.dead_zone_right, info.touch_width, info.touch_height);
    pad_configure_motion();
    push_device(SDL_EVENT_GAMEPAD_ADDED);
}

// The newest usable record as the current state; each change an SDL event
static void pad_publish(const PadRecord *r) {
    uint32_t sdl = 0;
    for (int i = 0; i < NBUTTONS; i++)
        if (r->buttons & k_buttons[i].bit) sdl |= 1u << k_buttons[i].sdl;
    Sint16 axis[SDL_GAMEPAD_AXIS_COUNT] = {stick_axis(r->lx), stick_axis(r->ly), stick_axis(r->rx), stick_axis(r->ry),
                                           trigger_axis(r->l2), trigger_axis(r->r2)};
    uint32_t changed = sdl ^ g_pad.sdl_buttons;
    g_pad.sdl_buttons = sdl;
    SDL_Event e;
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) {
        if (axis[a] == g_pad.axis[a]) continue;
        g_pad.axis[a] = axis[a];
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
        e.gaxis.which = g_pad.id, e.gaxis.axis = (Uint8)a, e.gaxis.value = axis[a];
        push(&e);
    }
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
        if (!(changed >> b & 1)) continue;
        memset(&e, 0, sizeof e);
        e.type = sdl >> b & 1 ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = g_pad.id, e.gbutton.button = (Uint8)b, e.gbutton.down = sdl >> b & 1;
        push(&e);
    }
}

static void pad_read(void) {
    PadRecord rec[PAD_RECORDS];
    int32_t n = scePadRead(g_pad.handle, rec, PAD_RECORDS);
    if (n == 0) return;  // nothing new: the current state stands
    if (n < 0 || n > PAD_RECORDS) {
        if (g_pad.read_errors++ % 1000 == 0)
            LOG("[input] scePadRead(handle %d) failed: %#x (%u failures)\n", g_pad.handle, (unsigned)n, g_pad.read_errors);
        pad_remove("its records could not be read");
        return;
    }
    const PadRecord *newest = NULL;
    for (int32_t i = 0; i < n; i++) {
        const PadRecord *r = &rec[i];
        bool usable = r->connected && !(r->buttons & PAD_INTERCEPTED);
        if (g_pad.present && !usable)
            pad_remove(!r->connected ? "disconnected" : "the system has the controller");
        else if (g_pad.present && r->connected_count != g_pad.generation)
            pad_remove("a new connection");
        if (!g_pad.present && usable) pad_add(r);
        if (g_pad.present) newest = r, pad_motion(r);
    }
    if (newest) pad_publish(newest);
}

// ---- keyboards and mice
enum { HID_KEYBOARD, HID_MOUSE, HID_KINDS };
static const char *const k_hid_name[HID_KINDS] = {"USB keyboard", "USB mouse"};
typedef struct {
    int32_t handle;       // -1: not open
    uint64_t retry_ns;    // when to try opening it again
    int32_t open_error;   // the failure last logged (each new one is)
    uint32_t read_errors;
    bool connected;       // its records say a device is there
    uint8_t keys[32];     // keyboard: the HID usages it holds (bit per usage)
    uint32_t buttons;     // mouse: the buttons it holds (bits 0..4)
} HidDevice;
static struct {
    bool lib[HID_KINDS];             // the sysmodule loaded and the library initialized
    HidDevice dev[HID_KINDS][HID_INDEXES];
    uint8_t keys[32];                // the usages any keyboard holds (sent as key events)
    uint32_t buttons;                // the mouse buttons any mouse holds (bit per SDL_BUTTON_*)
    int64_t moved[4];                // motion x, y, wheel y, x since the last [input] mouse line
    uint64_t moved_log_ns;
} g_hid = {.dev = {{{.handle = -1}, {.handle = -1}}, {{.handle = -1}, {.handle = -1}}}};

static bool usage_held(const uint8_t *set, unsigned u) { return set[u >> 3] >> (u & 7) & 1; }

// The usages a record holds: its key array and its modifier bits (as Left Control..Right GUI, 0xe0..0xe7; SDL's
// scancodes are these usages). Nothing for a disconnected or intercepted record. False: a rollover / error report
// (usages 1..3), which says nothing about the keys
static bool keyboard_usages(const KeyboardRecord *r, uint8_t out[32]) {
    memset(out, 0, 32);
    if (!r->connected || r->intercepted) return true;
    for (int i = 0; i < 16; i++) {
        unsigned u = r->keys[i];
        if (u >= 1 && u <= 3) return false;
        if (u >= 4 && u < 256) out[u >> 3] |= (uint8_t)(1u << (u & 7));
    }
    for (unsigned b = 0; b < 8; b++)
        if (r->modifiers >> b & 1) out[(0xe0 + b) >> 3] |= (uint8_t)(1u << ((0xe0 + b) & 7));
    return true;
}

static SDL_Keymod keyboard_mod(void) {
    static const SDL_Keymod k_mod[8] = {SDL_KMOD_LCTRL, SDL_KMOD_LSHIFT, SDL_KMOD_LALT, SDL_KMOD_LGUI,
                                        SDL_KMOD_RCTRL, SDL_KMOD_RSHIFT, SDL_KMOD_RALT, SDL_KMOD_RGUI};
    SDL_Keymod m = SDL_KMOD_NONE;
    for (unsigned b = 0; b < 8; b++)
        if (usage_held(g_hid.keys, 0xe0 + b)) m |= k_mod[b];
    return m;
}

static const char *key_name(unsigned u) {
    static char buf[16];
    const char *n = SDL_GetScancodeName((SDL_Scancode)u);
    if (*n) return n;
    snprintf(buf, sizeof buf, "usage %#x", u);
    return buf;
}

static void key_event(unsigned u, bool down, int index) {
    if (down) g_hid.keys[u >> 3] |= (uint8_t)(1u << (u & 7));
    else g_hid.keys[u >> 3] &= (uint8_t)~(1u << (u & 7));
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.which = (uint32_t)index + 1;
    e.key.scancode = (SDL_Scancode)u;
    e.key.mod = keyboard_mod();
    e.key.key = SDL_GetKeyFromScancode(e.key.scancode, e.key.mod, true);
    e.key.down = down;
    push(&e);
    if (!log_allowed()) return;
    char held[256] = "";
    size_t n = 0;
    for (unsigned h = 4; h < 256 && n < sizeof held - 1; h++)
        if (usage_held(g_hid.keys, h)) n += (size_t)snprintf(held + n, sizeof held - n, " [%s]", key_name(h));
    LOG("[input] keyboard: %s %s | held:%s\n", key_name(u), down ? "down" : "up", n ? held : " (none)");
}

// The keys any keyboard holds, as events for each change: releases first (other keys, then modifiers), then presses
// (modifiers, then other keys), so a chord's modifier is down before its key and up after it
static void keyboard_publish(int index) {
    uint8_t now[32] = {0};
    for (int i = 0; i < HID_INDEXES; i++)
        for (int b = 0; b < 32; b++) now[b] |= g_hid.dev[HID_KEYBOARD][i].keys[b];
    for (unsigned u = 4; u < 0xe0; u++)
        if (usage_held(g_hid.keys, u) && !usage_held(now, u)) key_event(u, false, index);
    for (unsigned u = 0xe0; u < 256; u++)
        if (usage_held(g_hid.keys, u) && !usage_held(now, u)) key_event(u, false, index);
    for (unsigned u = 0xe0; u < 256; u++)
        if (!usage_held(g_hid.keys, u) && usage_held(now, u)) key_event(u, true, index);
    for (unsigned u = 4; u < 0xe0; u++)
        if (!usage_held(g_hid.keys, u) && usage_held(now, u)) key_event(u, true, index);
}

// A mouse's buttons (bits 0..4) as SDL's, and their names
static const uint8_t k_mouse_sdl[5] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE, SDL_BUTTON_X1, SDL_BUTTON_X2};
static const char *const k_mouse_name[5] = {"Left", "Right", "Middle", "X1", "X2"};

static void mouse_publish(void) {
    uint32_t now = 0;
    for (int i = 0; i < HID_INDEXES; i++)
        for (int b = 0; b < 5; b++)
            if (g_hid.dev[HID_MOUSE][i].buttons >> b & 1) now |= 1u << k_mouse_sdl[b];
    for (int b = 0; b < 5; b++) {
        uint32_t bit = 1u << k_mouse_sdl[b];
        if (!((now ^ g_hid.buttons) & bit)) continue;
        bool down = now & bit;
        g_hid.buttons ^= bit;
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.button = k_mouse_sdl[b];
        e.button.down = down;
        push(&e);
        if (!log_allowed()) continue;
        char held[64] = "";
        size_t n = 0;
        for (int h = 0; h < 5; h++)
            if (g_hid.buttons >> k_mouse_sdl[h] & 1) n += (size_t)snprintf(held + n, sizeof held - n, " %s", k_mouse_name[h]);
        LOG("[input] mouse: %s %s | held:%s\n", k_mouse_name[b], down ? "down" : "up", n ? held : " (none)");
    }
}

// A record's connection: one line when a device comes or goes (what it held is released then)
static void hid_connection(int kind, int index, bool connected) {
    HidDevice *d = &g_hid.dev[kind][index];
    if (connected == d->connected) return;
    d->connected = connected;
    if (connected)
        LOG("[input] %s connected: user %d, index %d, handle %d\n", k_hid_name[kind], g_pad.user, index, d->handle);
    else
        LOG("[input] %s disconnected: index %d, handle %d (%s released)\n", k_hid_name[kind], index, d->handle,
            kind == HID_KEYBOARD ? "its keys" : "its buttons");
}

static void keyboard_read(int index) {
    HidDevice *d = &g_hid.dev[HID_KEYBOARD][index];
    KeyboardRecord rec[KEYBOARD_RECORDS];
    int32_t n = sceKeyboardRead(d->handle, rec, KEYBOARD_RECORDS);
    if (n < 0 || n > KEYBOARD_RECORDS) {
        if (d->read_errors++ % 1000 == 0)
            LOG("[input] sceKeyboardRead(handle %d) failed: %#x (%u failures)\n", d->handle, (unsigned)n, d->read_errors);
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        uint8_t keys[32];
        hid_connection(HID_KEYBOARD, index, rec[i].connected != 0);
        if (!keyboard_usages(&rec[i], keys)) continue;
        memcpy(d->keys, keys, sizeof keys);
        keyboard_publish(index);
    }
}

static void mouse_read(int index, int64_t sum[4]) {
    HidDevice *d = &g_hid.dev[HID_MOUSE][index];
    MouseRecord rec[MOUSE_RECORDS];
    int32_t n = sceMouseRead(d->handle, rec, MOUSE_RECORDS);
    if (n < 0 || n > MOUSE_RECORDS) {
        if (d->read_errors++ % 1000 == 0)
            LOG("[input] sceMouseRead(handle %d) failed: %#x (%u failures)\n", d->handle, (unsigned)n, d->read_errors);
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        const MouseRecord *r = &rec[i];
        hid_connection(HID_MOUSE, index, r->connected != 0);
        bool usable = r->connected && !(r->buttons & MOUSE_INTERCEPTED);
        d->buttons = usable ? r->buttons & 0x1fu : 0;
        mouse_publish();
        if (!usable) continue;
        sum[0] += r->x, sum[1] += r->y, sum[2] += r->wheel, sum[3] += r->tilt;
    }
}

// Every open keyboard's and mouse's new records; the mice's motion and wheel summed into one event each
static void hid_poll(void) {
    for (int i = 0; i < HID_INDEXES; i++)
        if (g_hid.dev[HID_KEYBOARD][i].handle >= 0) keyboard_read(i);
    int64_t sum[4] = {0, 0, 0, 0};
    for (int i = 0; i < HID_INDEXES; i++)
        if (g_hid.dev[HID_MOUSE][i].handle >= 0) mouse_read(i, sum);
    SDL_Event e;
    if (sum[0] || sum[1]) {
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.xrel = (float)sum[0], e.motion.yrel = (float)sum[1];
        push(&e);
    }
    if (sum[2] || sum[3]) {
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.x = (float)sum[3], e.wheel.y = (float)sum[2];
        push(&e);
    }
    for (int k = 0; k < 4; k++) g_hid.moved[k] += sum[k];
    uint64_t now = plat_now_ns();
    if (now < g_hid.moved_log_ns) return;
    if (g_hid.moved[0] || g_hid.moved[1] || g_hid.moved[2] || g_hid.moved[3])
        LOG("[input] mouse: moved %+lld,%+lld, wheel %+lld,%+lld in the last second\n", (long long)g_hid.moved[0],
            (long long)g_hid.moved[1], (long long)g_hid.moved[3], (long long)g_hid.moved[2]);
    memset(g_hid.moved, 0, sizeof g_hid.moved);
    g_hid.moved_log_ns = now + 1000000000ull;
}

static void hid_open(int kind, int index) {
    HidDevice *d = &g_hid.dev[kind][index];
    uint64_t now = plat_now_ns();
    if (!g_hid.lib[kind] || d->handle >= 0 || now < d->retry_ns) return;
    const uint8_t params[8] = {0};
    int32_t h = kind == HID_KEYBOARD ? sceKeyboardOpen(g_pad.user, 0, index, params)
                                     : sceMouseOpen(g_pad.user, 0, index, params);
    if (h < 0) {
        if (d->open_error != h)
            LOG("[input] %s(user %d, index %d) failed: %#x; trying again each second\n",
                kind == HID_KEYBOARD ? "sceKeyboardOpen" : "sceMouseOpen", g_pad.user, index, (unsigned)h);
        d->open_error = h;
        d->retry_ns = now + 1000000000ull;
        return;
    }
    d->handle = h;
    d->open_error = 0;
    d->read_errors = 0;
    d->connected = false;
    LOG("[input] user %d: %s index %d opened (handle %d)\n", g_pad.user, k_hid_name[kind], index, h);
}

static void hid_close(int kind, int index, const char *why) {
    HidDevice *d = &g_hid.dev[kind][index];
    if (d->handle < 0) return;
    hid_connection(kind, index, false);
    memset(d->keys, 0, sizeof d->keys);
    d->buttons = 0;
    if (kind == HID_KEYBOARD) keyboard_publish(index);
    else mouse_publish();
    int32_t r = kind == HID_KEYBOARD ? sceKeyboardClose(d->handle) : sceMouseClose(d->handle);
    LOG("[input] user %d: %s index %d handle %d closed (%s): %#x\n", g_pad.user, k_hid_name[kind], index, d->handle,
        why, (unsigned)r);
    d->handle = -1;
    d->retry_ns = 0;
}

// The libraries, once: the sysmodule, then its init (a kind whose library fails stays closed)
static void hid_start(void) {
    int32_t km = sceSysmoduleLoadModule(SYSMODULE_KEYBOARD);
    int32_t ki = km >= 0 ? sceKeyboardInit() : km;
    int32_t mm = sceSysmoduleLoadModule(SYSMODULE_MOUSE);
    int32_t mi = mm >= 0 ? sceMouseInit() : mm;
    g_hid.lib[HID_KEYBOARD] = ki >= 0, g_hid.lib[HID_MOUSE] = mi >= 0;
    LOG("[input] keyboard sysmodule %#x, sceKeyboardInit %#x; mouse sysmodule %#x, sceMouseInit %#x\n", (unsigned)km,
        (unsigned)ki, (unsigned)mm, (unsigned)mi);
}

// ---- the signed-in user's devices
static void pad_open(void) {
    if (g_pad.handle >= 0 || plat_now_ns() < g_pad.retry_ns) return;
    int32_t h = scePadOpen(g_pad.user, 0, 0, NULL);
    if (h < 0) {
        static int32_t said;
        if (said != h) LOG("[input] scePadOpen(user %d) failed: %#x; trying again each second\n", g_pad.user, (unsigned)h);
        said = h;
        g_pad.retry_ns = plat_now_ns() + 1000000000ull;
        return;
    }
    g_pad.handle = h;
    g_pad.read_errors = 0;
    LOG("[input] user %d: standard pad opened (handle %d)\n", g_pad.user, h);
}

static void open_devices(void) {
    if (g_pad.user < 0) return;
    pad_open();
    for (int kind = 0; kind < HID_KINDS; kind++)
        for (int i = 0; i < HID_INDEXES; i++) hid_open(kind, i);
}

static void close_devices(const char *why) {
    pad_remove(why);
    if (g_pad.handle >= 0) {
        int32_t r = scePadClose(g_pad.handle);
        LOG("[input] user %d: pad handle %d closed (%s): %#x\n", g_pad.user, g_pad.handle, why, (unsigned)r);
        g_pad.handle = -1;
    }
    for (int kind = 0; kind < HID_KINDS; kind++)
        for (int i = 0; i < HID_INDEXES; i++) hid_close(kind, i, why);
}

static void user_events(void) {
    UserServiceEvent ev;
    int32_t r;
    for (int i = 0; i < 16 && (r = sceUserServiceGetEvent(&ev)) == 0; i++) {
        LOG("[input] user service: user %d logged %s\n", ev.user_id, ev.event_type == 0 ? "in" : "out");
        if (ev.event_type == 1 && ev.user_id == g_pad.user) {
            close_devices("the user logged out");
            g_pad.user = -1;
        } else if (ev.event_type == 0 && g_pad.user < 0) {
            g_pad.user = ev.user_id;
            g_pad.retry_ns = 0;
            for (int kind = 0; kind < HID_KINDS; kind++)
                for (int k = 0; k < HID_INDEXES; k++) g_hid.dev[kind][k].retry_ns = 0;
        }
    }
}

static void start(void) {
    g_pad.started = true;
    int32_t r = sceUserServiceInitialize(NULL);  // (already initialized: an error, and the service still works)
    int32_t user = -1;
    int32_t ru = sceUserServiceGetInitialUser(&user);
    int32_t rp = scePadInit();
    LOG("[input] user service %#x, initial user %d (%#x), scePadInit %#x\n", (unsigned)r, user, (unsigned)ru, (unsigned)rp);
    if (ru >= 0 && user >= 0) g_pad.user = user;
    hid_start();
}

void ps5_input_poll(void) {
    uint64_t now = plat_now_ns();
    if (g_pad.started && now - g_last_poll_ns < 2000000ull) return;  // (the window loop asks once per event)
    g_last_poll_ns = now;
    if (!g_pad.started) start();
    user_events();
    open_devices();
    if (g_pad.handle >= 0) pad_read();
    hid_poll();
    if ((g_pad.rumble[0] || g_pad.rumble[1]) && g_pad.rumble_until_ns && now >= g_pad.rumble_until_ns)
        rumble_send(0, 0, " (its duration ended)");
}

// ---- SDL's gamepad calls
SDL_JoystickID *SDL_GetGamepads(int *count) {
    SDL_JoystickID *ids = calloc(2, sizeof *ids);
    int n = 0;
    if (ids && g_pad.present) ids[n++] = g_pad.id;
    if (count) *count = n;
    return ids;
}
void SDL_free(void *mem) { free(mem); }
// input.c holds the gamepad it opened until it handles the removal; SDL_GetGamepadID keeps naming it until then, and
// its state reads as nothing pressed once it is gone (or another connection replaced it)
static SDL_JoystickID g_open_id;
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) {
    if (!g_pad.present || id != g_pad.id) {
        ps5_sdl_error("no such gamepad");
        return NULL;
    }
    g_open_id = id;
    return &g_gamepad;
}
void SDL_CloseGamepad(SDL_Gamepad *gamepad) {  // (the pad handle stays this file's)
    if (gamepad == &g_gamepad) g_open_id = 0;
}
static bool ours(SDL_Gamepad *gamepad) { return gamepad == &g_gamepad && g_pad.present && g_open_id == g_pad.id; }
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *gamepad) { return gamepad == &g_gamepad ? g_open_id : 0; }
const char *SDL_GetGamepadName(SDL_Gamepad *gamepad) { return gamepad ? "DualSense Wireless Controller" : NULL; }
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad) {
    return gamepad ? SDL_GAMEPAD_TYPE_PS5 : SDL_GAMEPAD_TYPE_UNKNOWN;
}
SDL_GamepadType SDL_GetGamepadTypeForID(SDL_JoystickID id) {
    return g_pad.present && id == g_pad.id ? SDL_GAMEPAD_TYPE_PS5 : SDL_GAMEPAD_TYPE_UNKNOWN;
}
bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button) {
    return ours(gamepad) && (unsigned)button < SDL_GAMEPAD_BUTTON_COUNT && (g_pad.sdl_buttons >> button & 1);
}
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis) {
    return ours(gamepad) && (unsigned)axis < SDL_GAMEPAD_AXIS_COUNT ? g_pad.axis[axis] : 0;
}

// Motion sensors: the accelerometer and gyroscope, when scePadSetMotionSensorState took them
bool SDL_GamepadHasSensor(SDL_Gamepad *gamepad, SDL_SensorType type) {
    return ours(gamepad) && g_pad.motion && (type == SDL_SENSOR_ACCEL || type == SDL_SENSOR_GYRO);
}
bool SDL_SetGamepadSensorEnabled(SDL_Gamepad *gamepad, SDL_SensorType type, bool enabled) {
    if (!SDL_GamepadHasSensor(gamepad, type)) {
        ps5_sdl_error("the DualSense's motion sensors are off");
        return false;
    }
    g_pad.sensor_on[type] = enabled;
    return true;
}
float SDL_GetGamepadSensorDataRate(SDL_Gamepad *gamepad, SDL_SensorType type) {
    return SDL_GamepadHasSensor(gamepad, type) ? g_pad.motion_hz : 0;  // (measured: 0 in a connection's first second)
}

// Rumble (runtime/hle/vibration.c): the speeds hold for `milliseconds` (it asks again every 50 ms while they last)
bool SDL_RumbleGamepad(SDL_Gamepad *gamepad, uint16_t low, uint16_t high, uint32_t milliseconds) {
    if (gamepad != &g_gamepad) {
        ps5_sdl_error("no such gamepad");
        return false;
    }
    if (!ours(gamepad)) {  // (removed: its motors were stopped then)
        if (!low && !high) return true;
        ps5_sdl_error("the DualSense is not connected");
        return false;
    }
    uint8_t large = (uint8_t)(low >> 8), small = (uint8_t)(high >> 8);
    g_pad.rumble_until_ns = milliseconds ? plat_now_ns() + milliseconds * 1000000ull : 0;
    if (large == g_pad.rumble[0] && small == g_pad.rumble[1]) return true;
    return rumble_send(large, small, "");
}

// ---- keyboard names (bindings.c: savagewind.ini's bind_key_* lines and the Controls labels). SDL 3.4's names and
// US-layout key codes (unshifted, shifted) per scancode, so the desktop's settings files read the same.
static const struct { const char *name; SDL_Keycode key, shifted; } k_scancodes[SDL_SCANCODE_COUNT] = {
#include "scancodes.inc"
};
const char *SDL_GetScancodeName(SDL_Scancode sc) {
    return (unsigned)sc < SDL_SCANCODE_COUNT && k_scancodes[sc].name ? k_scancodes[sc].name : "";
}
SDL_Scancode SDL_GetScancodeFromName(const char *name) {
    if (!name || !*name) return SDL_SCANCODE_UNKNOWN;
    for (int i = 0; i < SDL_SCANCODE_COUNT; i++)
        if (k_scancodes[i].name && !strcasecmp(k_scancodes[i].name, name)) return (SDL_Scancode)i;
    return SDL_SCANCODE_UNKNOWN;
}
SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode sc, SDL_Keymod mod, bool key_event) {
    (void)key_event;
    if ((unsigned)sc >= SDL_SCANCODE_COUNT) return 0;
    return mod & (SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT) ? k_scancodes[sc].shifted : k_scancodes[sc].key;
}
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key, SDL_Keymod *mod) {
    for (int i = 0; i < SDL_SCANCODE_COUNT; i++)
        if (k_scancodes[i].name && k_scancodes[i].key == key) {
            if (mod) *mod = SDL_KMOD_NONE;
            return (SDL_Scancode)i;
        }
    for (int i = 0; i < SDL_SCANCODE_COUNT; i++)
        if (k_scancodes[i].name && k_scancodes[i].shifted == key) {
            if (mod) *mod = SDL_KMOD_LSHIFT;
            return (SDL_Scancode)i;
        }
    if (mod) *mod = SDL_KMOD_NONE;
    return SDL_SCANCODE_UNKNOWN;
}

// ---- the mouse: no cursor on the display plane, so "relative mode" is always what a USB mouse gives
bool SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled) { (void)window; (void)enabled; return true; }
