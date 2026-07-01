#pragma once

#include <cstdio>
#include <cstring>
#include <string>

namespace api {

constexpr const char* kHost = "http://xxxxx.com";

constexpr const char* kApiV1Prefix = "/api/v1";
constexpr const char* kXiaozhiDevicePrefix = "/xiaozhi/device";

// OpenClaw
constexpr const char* kOpenClawDeviceStatus =
    "/api/v1/devices/status";
constexpr const char* kOpenClawConversationList =
    "/api/v1/conversation?page=1&size=100";
constexpr const char* kOpenClawUpload = "/api/v1/upload";
constexpr const char* kOpenClawMessagesFmt =
    "/api/v1/conversation/%s/messages?page=1&size=100";
constexpr const char* kOpenClawRemoveAll =
    "/api/v1/conversation/removeAll";

// Weather
constexpr const char* kWeatherDistrictPath =
    "/api/v1/weather/district?dataType=all&districtId=";

// GPS
constexpr const char* kGpsLocationReport =
    "/xiaozhi/device/gps/location/report/cell";
constexpr const char* kGpsStaticMap =
    "/xiaozhi/device/gps/location/static-map";

inline std::string Url(const char* path) {
    return std::string(kHost) + path;
}

inline std::string WeatherDistrictUrl(const std::string& district_id) {
    return Url(kWeatherDistrictPath) + district_id;
}

inline std::string OpenClawMessagesUrl(const std::string& conversation_id) {
    char path[192];
    std::snprintf(path, sizeof(path), kOpenClawMessagesFmt,
                  conversation_id.c_str());
    return Url(path);
}

// 日志脱敏：响应体等可能含 staticMapUrl 等完整地址，禁止输出 claw 域名 URL。
inline std::string RedactClawUrlsForLog(const std::string& text) {
    std::string out = text;
    const size_t prefix_len = std::strlen(kHost);
    size_t pos = 0;
    while ((pos = out.find(kHost, pos)) != std::string::npos) {
        size_t end = out.find_first_of("\"' \t\r\n,}", pos + prefix_len);
        if (end == std::string::npos) {
            end = out.size();
        }
        out.replace(pos, end - pos, "[redacted]");
        pos += 10;
    }
    return out;
}

}  // namespace api
