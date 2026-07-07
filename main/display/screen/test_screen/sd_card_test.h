#pragma once

#include "lvgl.h"

namespace SdCardTest {

void BuildRow(lv_obj_t* list);
void OnLoad();
void OnUnload();
void Poll();

}  // namespace SdCardTest
