#include "weather_icon_map.h"

#include <cstring>

namespace {

struct WeatherIconEntry {
    const char* text;
    const char* code;
};

// 与和风 icon 码一致；资源文件为 ic_s_weather_{code}.png / .spng
constexpr WeatherIconEntry kWeatherIconMap[] = {
    {"晴", "100"},
    {"多云", "101"},
    {"阴", "104"},
    {"阵雨", "300"},
    {"雷阵雨", "302"},
    {"雷阵雨伴有冰雹", "304"},
    {"雨夹雪", "404"},
    {"小雨", "305"},
    {"中雨", "306"},
    {"大雨", "307"},
    {"暴雨", "310"},
    {"大暴雨", "311"},
    {"特大暴雨", "312"},
    {"阵雪", "407"},
    {"小雪", "400"},
    {"中雪", "401"},
    {"大雪", "402"},
    {"暴雪", "403"},
    {"雾", "501"},
    {"冻雨", "313"},
    {"沙尘暴", "507"},
    {"小到中雨", "314"},
    {"中到大雨", "315"},
    {"大到暴雨", "316"},
    {"暴雨到大暴雨", "317"},
    {"大暴雨到特大暴雨", "318"},
    {"小到中雪", "408"},
    {"中到大雪", "409"},
    {"大到暴雪", "410"},
    {"浮尘", "504"},
    {"扬沙", "503"},
    {"强沙尘暴", "508"},
    {"龙卷风", "508"},
    {"弱高吹雪", "499"},
    {"轻雾", "500"},
    {"强浓雾", "510"},
    {"霾", "502"},
    {"中度霾", "511"},
    {"重度霾", "512"},
    {"严重霾", "513"},
    {"大雾", "514"},
    {"特强浓雾", "515"},
    {"雨", "399"},
    {"雪", "499"},
};

constexpr const char* kDefaultIconCode = "104";  // 阴

}  // namespace

const char* WeatherIconCodeForText(const std::string& text) {
    if (text.empty()) {
        return nullptr;
    }
    for (const auto& entry : kWeatherIconMap) {
        if (text == entry.text) {
            return entry.code;
        }
    }
    return kDefaultIconCode;
}
