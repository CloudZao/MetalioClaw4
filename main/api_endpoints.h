#pragma once

#include <cstdio>
#include <cstring>
#include <string>

// 设为 1 时在 OpenClaw 等调用处打印 HTTP 请求 URL、请求体与响应体。
#ifndef API_HTTP_DEBUG
#define API_HTTP_DEBUG 0
#endif

#if API_HTTP_DEBUG
#include "esp_log.h"
#endif

namespace api {

#if API_HTTP_DEBUG

namespace detail {

inline const char* HttpBodyForLog(const std::string& body) {
    return body.empty() ? "(empty)" : body.c_str();
}

}  // namespace detail

inline void LogHttpRequest(const char* tag, const char* method,
                           const std::string& url,
                           const std::string& body = std::string(),
                           const char* extra = nullptr) {
    ESP_LOGI(tag, "[API_HTTP] >>> %s %s", method, url.c_str());
    if (extra != nullptr && extra[0] != '\0') {
        ESP_LOGI(tag, "[API_HTTP] >>> %s", extra);
    }
    ESP_LOGI(tag, "[API_HTTP] >>> body=%s", detail::HttpBodyForLog(body));
}

inline void LogHttpBinaryRequest(const char* tag, const char* method,
                                 const std::string& url, size_t body_bytes,
                                 const char* extra = nullptr) {
    ESP_LOGI(tag, "[API_HTTP] >>> %s %s", method, url.c_str());
    if (extra != nullptr && extra[0] != '\0') {
        ESP_LOGI(tag, "[API_HTTP] >>> %s", extra);
    }
    ESP_LOGI(tag, "[API_HTTP] >>> body=(binary %u bytes)",
              static_cast<unsigned>(body_bytes));
}

inline void LogHttpResponse(const char* tag, int status,
                            const std::string& body) {
    ESP_LOGI(tag, "[API_HTTP] <<< status=%d body=%s", status,
             detail::HttpBodyForLog(body));
}

#else

inline void LogHttpRequest(const char*, const char*, const std::string&,
                           const std::string& = std::string(),
                           const char* = nullptr) {}

inline void LogHttpBinaryRequest(const char*, const char*, const std::string&,
                                 size_t, const char* = nullptr) {}

inline void LogHttpResponse(const char*, int, const std::string&) {}

#endif

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
constexpr const char* kOpenClawConversationDeleteFmt =
    "/api/v1/conversation/delete/%s";

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

inline std::string OpenClawConversationDeleteUrl(
    const std::string& conversation_id) {
    char path[192];
    std::snprintf(path, sizeof(path), kOpenClawConversationDeleteFmt,
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
