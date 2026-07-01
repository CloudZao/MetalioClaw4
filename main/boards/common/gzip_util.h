#pragma once

#include <cstddef>
#include <string>

/** 是否为 gzip 格式（魔数 0x1F 0x8B） */
bool IsGzipData(const void* data, size_t len);

inline bool IsGzipData(const std::string& data) {
    return IsGzipData(data.data(), data.size());
}

/** gzip 解压到 out；失败时 out 被清空 */
bool GzipDecompress(const std::string& compressed, std::string& out);

/** gzip 则解压，否则原样拷贝 */
bool MaybeGunzip(const std::string& body, std::string& out);
