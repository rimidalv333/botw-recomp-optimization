// Boundaries for subsystems not linked into the PS5 title yet: the overlay menu and the desktop's performance
// overlay. Each answers as the desktop runtime does with no such device or feature.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../rt.h"
#include "../perfmon.h"
#include "../ui/menu.h"
#include <stdint.h>

const char *rt_version(void) { return "0.3.1-alpha.3"; }

// ---- the input devices are runtime/ps5/input.c behind runtime/hle/input.c and bindings.c (as on desktop); the
// button prompts runtime/nvn/glyphs.c; Savage Wind's Options pages runtime/ui/game_options.mm
bool bind_capture_event(const SDL_Event *e);  // runtime/hle/bindings.c
bool game_options_input_hold(void);           // runtime/ui/game_options.mm

// ---- the overlay menu (runtime/ui/menu.mm, ImGui; opened with keys): none, and the display plane takes no requests.
// A binding capture (the game's Options screen) still takes the press first, as the menu's event handler does, and
// the game gets no input while it runs.
bool menu_is_open(void) { return false; }
bool menu_cursor_free(void) { return false; }  // (no Esc to free a cursor: the mouse always turns the camera)
void menu_apply_window_requests(void *window) { (void)window; }
void menu_test_open(void) {}
// Window size's list: the PS5 Video page has no Window size row (runtime/ps5/video_page.cpp)
void menu_size_list_open(void) { LOG("[options] no window size list on the PS5\n"); }

// ---- the performance overlay's numbers (runtime/perfmon.c; shown by the overlay menu)
void perfmon_frame(void) {}
void perfmon_generated_frame(void) {}
void perfmon_set_gpu_hint(const char *name) { (void)name; }
