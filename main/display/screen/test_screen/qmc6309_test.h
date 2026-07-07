#pragma once

#include "lvgl.h"

namespace Qmc6309Test {

void BuildRow(lv_obj_t* list);
void OnLoad();
void OnUnload();
void Poll();

}  // namespace Qmc6309Test
