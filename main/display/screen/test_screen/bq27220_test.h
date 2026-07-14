#pragma once

#include "lvgl.h"

namespace Bq27220Test {

void BuildRow(lv_obj_t* list);
void OnLoad();
void OnUnload();
void Poll();

}  // namespace Bq27220Test
