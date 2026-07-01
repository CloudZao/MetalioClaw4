#pragma once

#include "lvgl.h"

class WeatherScreen {
public:
    // 720x720 天气页：小智 device/weather/district 接口，支持省市区联动选择。
    // 图标：接口返回中文天气名，经 weather_icon_map 转为和风码，
    // 加载 A:ic_s_weather_{code}.spng（如 多云 -> 101）。
    // 后台 FreeRTOS 任务拉取数据，UI 更新在 esp_lv_adapter_lock 内完成。
    static lv_obj_t* Create();
};
