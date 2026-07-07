#pragma once

#include "lvgl.h"

namespace AudioTest {

void BuildRow(lv_obj_t* list);
void OnLoad();
void OnUnload();
void Poll();

}  // namespace AudioTest
