#pragma once

#include <string>

// 接口返回中文天气描述（如「多云」「小雨」），映射到资源 ic_s_weather_{code}.spng。
const char* WeatherIconCodeForText(const std::string& text);
