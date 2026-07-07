#pragma once

#include "lvgl.h"

namespace CameraTest {

void BuildRow(lv_obj_t* list);
void OnLoad();
void OnUnload();
void Poll();

}  // namespace CameraTest
