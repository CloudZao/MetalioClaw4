#include "gzip_util.h"

#include <esp_log.h>
#include <zlib.h>

#include <vector>

namespace {

constexpr const char* TAG = "GzipUtil";

}  // namespace

bool IsGzipData(const void* data, size_t len) {
    if (data == nullptr || len < 2) {
        return false;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    return bytes[0] == 0x1f && bytes[1] == 0x8b;
}

bool GzipDecompress(const std::string& compressed, std::string& out) {
    out.clear();
    if (compressed.empty()) {
        return false;
    }

    z_stream strm = {};
    // 15 + 16：自动识别 gzip 头
    int ret = inflateInit2(&strm, MAX_WBITS + 16);
    if (ret != Z_OK) {
        ESP_LOGE(TAG, "inflateInit2 failed: %d", ret);
        return false;
    }

    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
    strm.avail_in = static_cast<uInt>(compressed.size());

    std::vector<char> buf(4096);
    do {
        strm.next_out = reinterpret_cast<Bytef*>(buf.data());
        strm.avail_out = static_cast<uInt>(buf.size());
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            ESP_LOGE(TAG, "inflate failed: %d", ret);
            inflateEnd(&strm);
            out.clear();
            return false;
        }
        const size_t produced = buf.size() - strm.avail_out;
        if (produced > 0) {
            out.append(buf.data(), produced);
        }
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);
    if (ret != Z_STREAM_END) {
        ESP_LOGE(TAG, "gzip stream not complete: %d", ret);
        out.clear();
        return false;
    }

    ESP_LOGD(TAG, "decompressed %u -> %u bytes",
             static_cast<unsigned>(compressed.size()), static_cast<unsigned>(out.size()));
    return true;
}

bool MaybeGunzip(const std::string& body, std::string& out) {
    if (IsGzipData(body)) {
        return GzipDecompress(body, out);
    }
    out = body;
    return true;
}
