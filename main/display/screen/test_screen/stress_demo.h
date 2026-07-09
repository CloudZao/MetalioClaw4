#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STRESS_DEMO_TIME_STEP 50

void stress_demo_start(void);
void stress_demo_stop(void);
bool stress_demo_finished(void);

#ifdef __cplusplus
}
#endif
