#include "screen_util.h"

#include <cstdlib>

#include "esp_log.h"

#include "IOExpander.hpp"
#include "application.h"

namespace {

constexpr const char* kScreenUtilTag = "ScreenUtil";

// 720x720 panel -> use a slightly larger threshold than the 480p source.
constexpr int16_t kSwipeBackThreshold = 80;

struct SwipeState {
    int16_t start_x = 0;
    int16_t start_y = 0;
    bool tracking = false;
};

// One in-flight gesture at a time -- only one screen is loaded at any
// moment, so a single global is sufficient.
SwipeState s_swipe;

// Use an unused LVGL object flag bit to mark "this widget owns horizontal
// drag semantics; don't count its events as swipe-back candidates."  LVGL
// reserves USER_1..USER_4 for application use, which is exactly what we
// want here.
constexpr lv_obj_flag_t kSwipeBackIgnoreFlag = LV_OBJ_FLAG_USER_1;

// Walk from `from` up to `top` (exclusive) and return true if any ancestor
// (including `from`) is one of the widget types that intrinsically eat
// horizontal touch drags -- LVGL's built-in slider/arc/roller -- or has
// been explicitly marked by screen_swipe_back_ignore().  The screen-level
// swipe handler must treat these the same: the drag is the widget's, not
// ours.
bool event_originates_in_drag_owner(lv_obj_t* from, lv_obj_t* top) {
    for (lv_obj_t* obj = from; obj != nullptr && obj != top;
         obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, kSwipeBackIgnoreFlag)) {
            return true;
        }
        if (lv_obj_check_type(obj, &lv_slider_class) ||
            lv_obj_check_type(obj, &lv_arc_class) ||
            lv_obj_check_type(obj, &lv_roller_class)) {
            return true;
        }
    }
    return false;
}

void swipe_back_event_cb(lv_event_t* e) {
    auto on_back = reinterpret_cast<screen_swipe_back_cb_t>(
        lv_event_get_user_data(e));
    const lv_event_code_t code = lv_event_get_code(e);

    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }

    // Bail out as soon as the touch is owned by a widget that handles
    // horizontal drags itself (slider knob, arc, roller, anything tagged
    // via screen_swipe_back_ignore()).  Without this guard, dragging a
    // slider rightward looks identical to a swipe-back to us: the slider
    // bubbles its PRESSED / RELEASED (and sometimes GESTURE) up the tree,
    // we measure dx > 80 and fire on_back().  Doing the check up here also
    // resets `s_swipe.tracking` so a previous unrelated PRESSED can't leak
    // into the next gesture.
    lv_obj_t* scr_obj = lv_event_get_current_target_obj(e);
    lv_obj_t* press_target = lv_event_get_target_obj(e);
    if (event_originates_in_drag_owner(press_target, scr_obj)) {
        s_swipe.tracking = false;
        return;
    }

    // ----- LVGL gesture path ------------------------------------------------
    // When the user starts a press on a child widget (e.g. a digit button)
    // and then drags far enough horizontally, LVGL produces a single
    // LV_EVENT_GESTURE on the originally-pressed widget.  With
    // LV_OBJ_FLAG_GESTURE_BUBBLE on every descendant, that event also
    // bubbles up to the screen, so we can intercept it here regardless of
    // where the swipe started.  We then call lv_indev_wait_release() to
    // suppress the pending CLICKED on whichever button was pressed.
    if (code == LV_EVENT_GESTURE) {
        const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        if (dir == LV_DIR_RIGHT && on_back != nullptr) {
            lv_indev_wait_release(indev);
            s_swipe.tracking = false;
            on_back();
        }
        return;
    }

    // ----- Manual press/release tracker -------------------------------------
    // Catches swipes that start on the bare screen background, where LVGL's
    // built-in gesture detector wouldn't run (no widget was pressed).
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_PRESSED) {
        s_swipe.start_x = point.x;
        s_swipe.start_y = point.y;
        s_swipe.tracking = true;
        return;
    }

    if (code != LV_EVENT_RELEASED || !s_swipe.tracking) {
        return;
    }

    s_swipe.tracking = false;

    const int16_t dx = point.x - s_swipe.start_x;
    const int16_t dy = point.y - s_swipe.start_y;
    if (dx > kSwipeBackThreshold && std::abs(dy) < std::abs(dx)) {
        if (on_back != nullptr) {
            on_back();
        }
    }
}

// Recursively flag every descendant so LV_EVENT_GESTURE bubbles up to the
// screen, regardless of which widget was the press target.  We also enable
// LV_OBJ_FLAG_EVENT_BUBBLE so PRESSED / RELEASED that originate on a child
// widget reach the manual tracker -- this lets a right-swipe that started
// on a button or icon still navigate back, even when LVGL's built-in
// gesture detector decides the movement wasn't large enough to fire
// LV_EVENT_GESTURE.  Called once from screen_attach_swipe_back() after the
// screen tree has been built.
void enable_gesture_bubble(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        enable_gesture_bubble(lv_obj_get_child(obj, i));
    }
}

void swipe_back_screen_loaded_cb(lv_event_t* e) {
    // The screen is built first, then loaded.  Walk the (now-populated)
    // tree and enable gesture bubbling on every child.  Doing it here
    // (rather than at attach time) lets callers add children after
    // screen_attach_swipe_back() and still get the right behaviour.
    lv_obj_t* scr = lv_event_get_current_target_obj(e);
    enable_gesture_bubble(scr);
}

}  // namespace

void screen_make_input_passive(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_ADV_HITTEST);

    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        screen_make_input_passive(lv_obj_get_child(obj, i));
    }
}

void screen_strip_obj_chrome(lv_obj_t* obj) {
    if (obj == nullptr) return;
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_margin_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void screen_swipe_back_ignore(lv_obj_t* obj, bool recursive) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_add_flag(obj, kSwipeBackIgnoreFlag);
    if (!recursive) {
        return;
    }
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        screen_swipe_back_ignore(lv_obj_get_child(obj, i), true);
    }
}

void screen_attach_swipe_back(lv_obj_t* scr, screen_swipe_back_cb_t on_back) {
    if (scr == nullptr) return;
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    void* user_data = reinterpret_cast<void*>(on_back);
    lv_obj_add_event_cb(scr, swipe_back_event_cb, LV_EVENT_PRESSED,
                        user_data);
    lv_obj_add_event_cb(scr, swipe_back_event_cb, LV_EVENT_RELEASED,
                        user_data);
    // Catch swipes that start on a child widget (button, image, etc.).  The
    // gesture event is fired on the press target and -- once we mark each
    // descendant with LV_OBJ_FLAG_GESTURE_BUBBLE -- bubbles up to the
    // screen.  We wait for SCREEN_LOADED so the tree is fully built before
    // we walk it.
    lv_obj_add_event_cb(scr, swipe_back_event_cb, LV_EVENT_GESTURE,
                        user_data);
    lv_obj_add_event_cb(scr, swipe_back_screen_loaded_cb,
                        LV_EVENT_SCREEN_LOADED, nullptr);
}

// ---------------------------------------------------------------------------
// Screen lifecycle
//
// We piggy-back on LVGL's existing LV_EVENT_SCREEN_LOADED / UNLOADED so the
// user callback runs at the exact same moments per-screen unload handlers
// already do.  Note: we deliberately do NOT delete the screen object here
// (the home screen's launch helpers already async-delete the old screen
// after switching); the lifecycle hook is purely an observer.
// ---------------------------------------------------------------------------

namespace {

void lifecycle_loaded_cb(lv_event_t* e) {
    auto cb = reinterpret_cast<screen_lifecycle_cb_t>(lv_event_get_user_data(e));
    if (cb != nullptr) {
        cb(SCREEN_LIFECYCLE_LOAD);
    }
}

void lifecycle_unloaded_cb(lv_event_t* e) {
    auto cb = reinterpret_cast<screen_lifecycle_cb_t>(lv_event_get_user_data(e));
    if (cb != nullptr) {
        cb(SCREEN_LIFECYCLE_UNLOAD);
    }
}

}  // namespace

void screen_attach_lifecycle(lv_obj_t* scr, screen_lifecycle_cb_t cb) {
    if (scr == nullptr || cb == nullptr) {
        return;
    }
    void* user_data = reinterpret_cast<void*>(cb);
    lv_obj_add_event_cb(scr, lifecycle_loaded_cb, LV_EVENT_SCREEN_LOADED,
                        user_data);
    lv_obj_add_event_cb(scr, lifecycle_unloaded_cb, LV_EVENT_SCREEN_UNLOADED,
                        user_data);
}

void screen_register_pwr_key_toggle_chat() {
    const esp_err_t err = IOExpander::getInstance().onClick(
        IOExpander::Pin::PWR_KEY, []() {
            Application::GetInstance().ToggleChatState();
        });
    if (err != ESP_OK) {
        ESP_LOGW(kScreenUtilTag, "PWR_KEY toggle register failed: 0x%x",
                 static_cast<unsigned>(err));
    }
}

void screen_unregister_pwr_key_toggle_chat() {
    IOExpander::getInstance().offClick(IOExpander::Pin::PWR_KEY);
}
