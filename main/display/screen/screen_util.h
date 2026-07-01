#pragma once

#include "lvgl.h"

// Make `obj` and all of its descendants ignore touch input, so that
// PRESSED / RELEASED events fall through to the screen. This is required
// for screen-level swipe gestures to receive coordinates regardless of
// where on the page the user touches.
void screen_make_input_passive(lv_obj_t* obj);

// Strip default LVGL chrome (padding / margin / border / radius / scrollbar)
// from a generic container that we are using purely for layout.
void screen_strip_obj_chrome(lv_obj_t* obj);

// Attach a right-swipe-to-back gesture to `scr`. When the user presses on
// the screen and releases at least `kSwipeBackThreshold` px to the right
// (with smaller vertical movement than horizontal), `on_back` is invoked.
// `on_back` is responsible for whatever navigation should happen.
typedef void (*screen_swipe_back_cb_t)();
void screen_attach_swipe_back(lv_obj_t* scr, screen_swipe_back_cb_t on_back);

// Mark `obj` (and by default all of its descendants) so that any touch event
// that originates inside it is *not* counted as a candidate for the
// right-swipe-back gesture. Use this on widgets that own horizontal drag
// semantics themselves (slider, arc, custom carousels, scroll views, ...)
// to keep their internal drag from being misinterpreted as a back gesture.
//
// `recursive=true` -- also tag every descendant. Defaults to true because
// the typical caller is "the whole slider widget including its knob".
void screen_swipe_back_ignore(lv_obj_t* obj, bool recursive = true);

// ---------------------------------------------------------------------------
// Screen lifecycle hooks
//
// A single callback receives both LOAD and UNLOAD notifications.  Use this
// to do per-screen logging, telemetry, or to start / stop background work
// that should run only while the screen is on stage.
//
//   void weather_lifecycle_cb(screen_lifecycle_event_t event) {
//       if (event == SCREEN_LIFECYCLE_LOAD) { ... }
//       else { ... }
//   }
//
// The hook is wired with `screen_attach_lifecycle(scr, cb)` -- typically
// called by the home / menu screen right after building each child screen,
// so the wiring lives in one central place rather than scattered through
// every screen implementation.
//
// LOAD fires when the screen becomes active (LV_EVENT_SCREEN_LOADED).
// UNLOAD fires when LVGL has finished switching away from the screen
// (LV_EVENT_SCREEN_UNLOADED).  The screen object remains valid throughout
// the UNLOAD callback; callers are expected to free their own state there.
// ---------------------------------------------------------------------------
typedef enum {
    SCREEN_LIFECYCLE_LOAD,    // matches LV_EVENT_SCREEN_LOADED
    SCREEN_LIFECYCLE_UNLOAD,  // matches LV_EVENT_SCREEN_UNLOADED
} screen_lifecycle_event_t;

typedef void (*screen_lifecycle_cb_t)(screen_lifecycle_event_t event);

void screen_attach_lifecycle(lv_obj_t* scr, screen_lifecycle_cb_t cb);

// PWR_KEY 单击切换对话状态：在 chat / digital_people 等屏幕 LOAD 时注册，
// UNLOAD 时注销，避免离开页面后仍响应电源键。
void screen_register_pwr_key_toggle_chat();
void screen_unregister_pwr_key_toggle_chat();
