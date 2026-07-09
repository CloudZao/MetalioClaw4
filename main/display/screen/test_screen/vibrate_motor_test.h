#pragma once

#include "lvgl.h"

namespace VibrateMotorTest {

void BuildRow(lv_obj_t* list);
void OnLoad();
void OnUnload();
void StartMotor();
void StopMotor();

}  // namespace VibrateMotorTest
