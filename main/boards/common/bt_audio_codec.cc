#include "bt_audio_codec.h"

#include <esp_log.h>
#include <cmath>
#include <cstring>

#define TAG "BTAudioCodec"

BTAudioCodec::~BTAudioCodec()
{
    if (rx_handle_ != nullptr)
    {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    if (tx_handle_ != nullptr)
    {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
}


BTAudioCodecDuplex::BTAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din)
{
    duplex_ = true;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    input_reference_ = true;                    // 是否使用参考输入，实现回声消除
    input_channels_ = input_reference_ ? 2 : 1; // 输入通道数

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_SLAVE,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif

        },
        .slot_cfg = {.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO, .slot_mode = I2S_SLOT_MODE_STEREO, .slot_mask = I2S_STD_SLOT_BOTH, .ws_width = I2S_DATA_BIT_WIDTH_32BIT, .ws_pol = false, .bit_shift = true,
#ifdef I2S_HW_VERSION_2
                     .left_align = true,
                     .big_endian = false,
                     .bit_order_lsb = false
#endif

        },
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = bclk, .ws = ws, .dout = dout, .din = din, .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}}};
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Duplex channels created");
}

int BTAudioCodec::Write(const int16_t *data, int samples)
{
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    // 双声道：每个采样点需要输出左右两个声道的数据
    std::vector<int32_t> buffer(samples * 2);

    // output_volume_: 0-100
    // volume_factor_: 0-65536
    int32_t volume_factor = pow(double(output_volume_) / 100.0, 2) * 65536;

    for (int i = 0; i < samples; i++)
    {
        int64_t temp = int64_t(data[i]) * volume_factor; // 使用 int64_t 进行乘法运算
        int32_t processed_sample;

        if (temp > INT32_MAX)
        {
            processed_sample = INT32_MAX;
        }
        else if (temp < INT32_MIN)
        {
            processed_sample = INT32_MIN;
        }
        else
        {
            processed_sample = static_cast<int32_t>(temp);
        }

        // 将处理后的采样点复制到左右两个声道（交错排列）
        buffer[i * 2] = processed_sample;     // 左声道
        buffer[i * 2 + 1] = processed_sample; // 右声道（复制相同的数据）
    }

    size_t bytes_written;
    ESP_ERROR_CHECK(i2s_channel_write(tx_handle_, buffer.data(), samples * 2 * sizeof(int32_t), &bytes_written, portMAX_DELAY));
    // 返回写入的采样点数量（以单声道计算）
    return bytes_written / (2 * sizeof(int32_t));
}

int BTAudioCodec::Read(int16_t *dest, int samples)
{
    size_t bytes_read;

    std::vector<int32_t> bit32_buffer(samples);
    if (i2s_channel_read(rx_handle_, bit32_buffer.data(), samples * sizeof(int32_t), &bytes_read, portMAX_DELAY) != ESP_OK)
    {
        ESP_LOGE(TAG, "Read Failed!");
        return 0;
    }

    samples = bytes_read / sizeof(int32_t);
    for (int i = 0; i < samples; i++)
    {
        int32_t value = bit32_buffer[i] >> 12;
        dest[i] = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? -INT16_MAX
                                                                         : (int16_t)value;
    }
    return samples;
}
