#include "music_screen.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "SimpleUart.hpp"

#include "home_screen/home_screen.h"
#include "screen_util.h"

#include "lv_eaf.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "MusicScreen";

// ---------------------------------------------------------------------------
// 720x720 layout：
//   y=0     ┌────────────────────────────────┐
//   y=36   │ [←]                             │  back btn 72x72
//   y=48   │           Song title           │  font 30, white
//          │       (gap)                    │
//   y=140  │     ┌──────────────────┐       │
//          │     │ album   240x240  │       │  240x240 透明遮罩裁圆
//   y=380  │     └──────────────────┘       │
//          │       (gap)                    │
//   y=410  │   ★ Lyric line 0 (newest)      │  100% opa
//   y=444  │     · Lyric line 1 (older)     │   60% opa
//   y=478  │     · Lyric line 2 (oldest)    │   30% opa
//          │       (gap)                    │
//   y=560  │ [vol-]  [<] [▶/❚❚] [>]  [vol+] │  control row
//          │       (gap)                    │
//   ~y=694 │  蓝牙音箱模式 · 手机连本机 …   │  使用提示，font 20, dim
//   y=720  └────────────────────────────────┘
// ---------------------------------------------------------------------------
constexpr int32_t kPanelSize = 720;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorBgGrad = 0x161A22;
constexpr uint32_t kColorTextPrimary = 0xFFFFFF;
constexpr uint32_t kColorAccent = 0xE0FB3C;
constexpr uint32_t kColorCtrlBtnBg = 0x232732;
constexpr uint32_t kColorCtrlBtnBgPressed = 0x303644;
constexpr uint32_t kColorPlayBtnBg = 0x3A4150;
constexpr uint32_t kColorPlayBtnBgPressed = 0x4A5260;

constexpr int32_t kTitleY = 48;
constexpr int32_t kAlbumSize = 240;
// 屏幕最底部的常驻使用说明，距底边的内边距。
constexpr int32_t kHintBottomMargin = 16;
// GIF 圆形主体在 240x240 内是带抗锯齿白边的，如果遮罩刚好等于 240，
// 那一圈半透明的白色像素就会漏出来形成"毛刺"。把遮罩往里缩几个像素
// （每边各 4px），让 clip_corner 多裁一圈把白边一起切掉。
constexpr int32_t kAlbumMaskShrink = 4;  // 每边裁掉的像素，加大可去更多毛刺
constexpr int32_t kAlbumMaskSize = kAlbumSize - kAlbumMaskShrink * 2;
constexpr int32_t kAlbumY = 140;
constexpr int32_t kCtrlRowY = 560;
constexpr int32_t kCtrlRowWidth = 700;
constexpr int32_t kCtrlRowHeight = 120;

constexpr int32_t kCtrlSideBtnSize = 80;
constexpr int32_t kCtrlPlayBtnSize = 112;

// 歌词三行布局：最上面是最新一句，越往下越旧、越透明。
constexpr int32_t kLyricLineCount = 3;
constexpr int32_t kLyricY = 410;
constexpr int32_t kLyricLineGap = 34;
constexpr int32_t kLyricLineWidth = kPanelSize - 80;
// 每行的目标 opacity（顶 -> 底）。255 / 153 / 76 大约对应 100% / 60% / 30%。
constexpr lv_opa_t kLyricTargetOpa[kLyricLineCount] = {255, 153, 76};
constexpr uint32_t kLyricFadeDurationMs = 380;

struct MusicUi {
    lv_obj_t* lbl_song = nullptr;
    lv_obj_t* lbl_lyric[kLyricLineCount] = {nullptr, nullptr, nullptr};
    lv_obj_t* img_play_icon = nullptr;
    lv_obj_t* album_eaf = nullptr;
    // 进入界面时蓝牙端通常还没在播放，默认按钮显示"▶ 播放"，
    // 用户点击后才切到"❚❚ 暂停"图标。
    bool playing = false;
};

MusicUi s_ui;
bool s_screen_active = false;
std::string s_rx_buffer;

void sync_album_eaf(bool playing) {
    if (s_ui.album_eaf == nullptr) {
        return;
    }
    if (playing) {
        lv_eaf_resume(s_ui.album_eaf);
    } else {
        lv_eaf_pause(s_ui.album_eaf);
    }
}

// 把 (part | state) 显式转成 lv_style_selector_t，规避
// -Wdeprecated-enum-enum-conversion 告警。
inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

// ---------------------------------------------------------------------------
// 异步 UI 更新（UART task -> LVGL 主线程）
// ---------------------------------------------------------------------------
struct AsyncTextMsg {
    char text[192];
};

void async_set_song(void* user_data) {
    auto* msg = static_cast<AsyncTextMsg*>(user_data);
    if (s_screen_active && s_ui.lbl_song != nullptr) {
        lv_label_set_text(s_ui.lbl_song, msg->text);
    }
    delete msg;
}

// 单行 opacity 动画：把 label 当前 opa 平滑过渡到 to。
void anim_label_opa_cb(void* var, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(var),
                         static_cast<lv_opa_t>(v), LV_PART_MAIN);
}

void start_opa_anim(lv_obj_t* obj, int32_t from, int32_t to) {
    if (obj == nullptr) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, kLyricFadeDurationMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_label_opa_cb);
    lv_anim_start(&a);
}

// 推动一行新歌词进队列：
//   line[2] <- line[1]   (旧 -> 更旧)
//   line[1] <- line[0]   (上 -> 中)
//   line[0] <- new       (新词在顶部)
// 同时把每行的 opacity 从「上一格的目标值」缓慢渐入到「自己格子的目标值」，
// 视觉上像是文字向下沉、越来越淡。
void push_lyric_line(const char* new_text) {
    if (s_ui.lbl_lyric[0] == nullptr) {
        return;
    }
    const char* cur_top = lv_label_get_text(s_ui.lbl_lyric[0]);
    const char* cur_mid = lv_label_get_text(s_ui.lbl_lyric[1]);
    std::string s_top = cur_top != nullptr ? cur_top : "";
    std::string s_mid = cur_mid != nullptr ? cur_mid : "";

    lv_label_set_text(s_ui.lbl_lyric[2], s_mid.c_str());
    lv_label_set_text(s_ui.lbl_lyric[1], s_top.c_str());
    lv_label_set_text(s_ui.lbl_lyric[0], new_text);

    // 顶行：从 0 渐入到 100% （新词淡入）
    // 中行：从 100% 渐落到 60%
    // 底行：从 60%  渐落到 30%
    start_opa_anim(s_ui.lbl_lyric[0], 0, kLyricTargetOpa[0]);
    start_opa_anim(s_ui.lbl_lyric[1], kLyricTargetOpa[0], kLyricTargetOpa[1]);
    start_opa_anim(s_ui.lbl_lyric[2], kLyricTargetOpa[1], kLyricTargetOpa[2]);
}

void async_set_lyric(void* user_data) {
    auto* msg = static_cast<AsyncTextMsg*>(user_data);
    if (s_screen_active && s_ui.lbl_lyric[0] != nullptr) {
        push_lyric_line(msg->text);
    }
    delete msg;
}

// ---- 播放 / 暂停图标同步（UART task -> LVGL 主线程） -----------------------
// 按钮图标的语义：图标本身就是「点了之后会发生的动作」。
//   playing == true  -> 当前正在播 -> 图标显示 ❚❚（点了就暂停）
//   playing == false -> 当前已暂停 -> 图标显示 ▶ （点了就播放）
struct AsyncPlayStateMsg {
    bool playing;
};

void async_set_play_icon(void* user_data) {
    auto* msg = static_cast<AsyncPlayStateMsg*>(user_data);
    if (s_screen_active && s_ui.img_play_icon != nullptr) {
        s_ui.playing = msg->playing;
        lv_image_set_src(s_ui.img_play_icon,
                         msg->playing ? "A:ic_s_player_pause.spng"
                                      : "A:ic_s_player_play.spng");
        sync_album_eaf(msg->playing);
    }
    delete msg;
}

void post_play_state(bool playing) {
    if (!s_screen_active) {
        return;
    }
    auto* msg = new AsyncPlayStateMsg{playing};
    lv_async_call(async_set_play_icon, msg);
}

void post_song(const std::string& text) {
    if (!s_screen_active) {
        return;
    }
    auto* msg = new AsyncTextMsg{};
    snprintf(msg->text, sizeof(msg->text), "%s", text.c_str());
    lv_async_call(async_set_song, msg);
}

void post_lyric(const std::string& text) {
    if (!s_screen_active) {
        return;
    }
    auto* msg = new AsyncTextMsg{};
    snprintf(msg->text, sizeof(msg->text), "%s", text.c_str());
    lv_async_call(async_set_lyric, msg);
}

// ---------------------------------------------------------------------------
// JSON 解析（极简）—— 只支持下面这两种行：
//   {"type":"song",  "data":"..."}
//   {"type":"lyrics","data":"..."}
// 实际数据来自手机回传，data 字段是 UTF-8 字符串，不会包含转义符号或
// 嵌套结构。这里直接做字符串查找，避开引入完整 JSON 解析器的开销。
// ---------------------------------------------------------------------------
bool extract_quoted_value(const std::string& line, const std::string& key,
                          std::string& out) {
    const std::string pattern = "\"" + key + "\"";
    size_t p = line.find(pattern);
    if (p == std::string::npos) {
        return false;
    }
    size_t colon = line.find(':', p + pattern.size());
    if (colon == std::string::npos) {
        return false;
    }
    size_t quote_open = line.find('"', colon + 1);
    if (quote_open == std::string::npos) {
        return false;
    }
    size_t quote_close = line.find('"', quote_open + 1);
    if (quote_close == std::string::npos) {
        return false;
    }
    out = line.substr(quote_open + 1, quote_close - quote_open - 1);
    return true;
}

void handle_json_line(const std::string& line) {
    if (line.empty() || line.front() != '{') {
        return;
    }
    std::string type;
    std::string data;
    if (!extract_quoted_value(line, "type", type)) {
        return;
    }
    if (!extract_quoted_value(line, "data", data)) {
        return;
    }
    if (type == "song") {
        ESP_LOGI(TAG, "song: %s", data.c_str());
        post_song(data);
    } else if (type == "lyrics") {
        ESP_LOGI(TAG, "lyrics: %s", data.c_str());
        post_lyric(data);
    }
}

// BT 模块在切换播放状态时会主动回包，行里通常包含 "MPLAY" 或 "MPAUSE"
// 字样（不强制是独立行，可能是 +EVT:MPLAY / OK MPAUSE 之类的格式）。
// 用 substring 匹配兼容所有形式。MPAUSE 必须先匹配，因为 "MPLAY" 不是
// "MPAUSE" 的子串、而判断顺序对结果有意义。
void handle_play_state_line(const std::string& line) {
    if (line.find("MPAUSE") != std::string::npos) {
        ESP_LOGI(TAG, "BT report: paused");
        post_play_state(false);
        return;
    }
    if (line.find("MPLAY") != std::string::npos) {
        ESP_LOGI(TAG, "BT report: playing");
        post_play_state(true);
    }
}

void handle_line(const std::string& line) {
    if (line.empty()) {
        return;
    }
    if (line.front() == '{') {
        handle_json_line(line);
    } else {
        handle_play_state_line(line);
    }
}

void on_uart_data(const std::vector<uint8_t>& data) {
    s_rx_buffer.append(data.begin(), data.end());

    size_t pos = 0;
    while (true) {
        size_t nl = s_rx_buffer.find('\n', pos);
        if (nl == std::string::npos) {
            break;
        }
        std::string line = s_rx_buffer.substr(pos, nl - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        handle_line(line);
        pos = nl + 1;
    }
    if (pos > 0) {
        s_rx_buffer.erase(0, pos);
    }
    if (s_rx_buffer.size() > 2048) {
        ESP_LOGW(TAG, "RX buffer overflow, clearing");
        s_rx_buffer.clear();
    }
}

// ---------------------------------------------------------------------------
// 蓝牙模式三切换 task：BT 模块需要 AT+RX=1 + 700ms 间隔 + AT+MODE=3。
// ---------------------------------------------------------------------------
void switch_to_mode3_task(void* /*arg*/) {
    SimpleUart& uart = SimpleUart::getInstance();
    if (!uart.isInitialized()) {
        ESP_LOGE(TAG, "UART not initialized, cannot switch BT to mode 3");
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "TX: AT+RX=1");
    uart.sendString("AT+RX=1\r\n");
    vTaskDelay(pdMS_TO_TICKS(700));
    ESP_LOGI(TAG, "TX: AT+MODE=3");
    uart.sendString("AT+MODE=3\r\n");
    vTaskDelete(nullptr);
}

// 退出音乐界面：BT 模块需要先 AT+RX=2，延时 700ms，再 AT+MODE=1，
// 把模块从音乐接收模式切回普通模式。
void switch_to_mode1_task(void* /*arg*/) {
    SimpleUart& uart = SimpleUart::getInstance();
    if (!uart.isInitialized()) {
        ESP_LOGE(TAG, "UART not initialized, cannot switch BT to mode 1");
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "TX: AT+RX=2");
    uart.sendString("AT+RX=2\r\n");
    vTaskDelay(pdMS_TO_TICKS(700));
    ESP_LOGI(TAG, "TX: AT+MODE=1");
    uart.sendString("AT+MODE=1\r\n");
    vTaskDelete(nullptr);
}

void send_at(const char* cmd) {
    SimpleUart& uart = SimpleUart::getInstance();
    if (!uart.isInitialized()) {
        ESP_LOGW(TAG, "UART not initialized, drop cmd: %s", cmd);
        return;
    }
    ESP_LOGI(TAG, "TX: %s", cmd);
    uart.sendString(cmd);
}

// ---------------------------------------------------------------------------
// 控件回调
// ---------------------------------------------------------------------------
void OnPrevClicked(lv_event_t* /*e*/) { send_at("AT+PREV\r\n"); }
void OnNextClicked(lv_event_t* /*e*/) { send_at("AT+NEXT\r\n"); }

void OnPlayClicked(lv_event_t* /*e*/) {
    // 按钮图标语义 = 「点了之后的动作」。
    //   - 正在播放（playing=true，图标=❚❚） -> 点 = 暂停 -> 发 MPAUSE
    //   - 已暂停  （playing=false，图标=▶ ） -> 点 = 播放 -> 发 MPLAY
    // 先乐观切图标，BT 模块随后会回包 MPLAY / MPAUSE 让 handle_play_state_line
    // 做最终对齐，万一命令丢了也能恢复。
    const bool want_playing = !s_ui.playing;
    send_at(want_playing ? "AT+MPLAY=1\r\n" : "AT+MPAUSE=1\r\n");
    s_ui.playing = want_playing;
    if (s_ui.img_play_icon != nullptr) {
        lv_image_set_src(s_ui.img_play_icon,
                         want_playing ? "A:ic_s_player_pause.spng"
                                      : "A:ic_s_player_play.spng");
    }
    sync_album_eaf(want_playing);
}

void OnVolDownClicked(lv_event_t* /*e*/) { send_at("AT+VOLDOWN\r\n"); }
void OnVolUpClicked(lv_event_t* /*e*/) { send_at("AT+VOLUP\r\n"); }

void OnSwipeBack() {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    s_screen_active = false;
    s_ui = MusicUi{};
}

// ---------------------------------------------------------------------------
// UI 构造
// ---------------------------------------------------------------------------
lv_obj_t* CreateRoundButton(lv_obj_t* parent, int32_t size, uint32_t bg_color,
                            uint32_t bg_pressed, const char* icon_path,
                            lv_event_cb_t cb) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_pressed),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(btn, 12);

    lv_obj_t* img = lv_image_create(btn);
    lv_image_set_src(img, icon_path);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(img);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return img;
}

void BuildBackButton(lv_obj_t* scr) {
    // 透明圆形按钮 + ← 图标，按下时白色半透明叠加，
    // 与 sd_card / network / vibrate / level 等页面保持同一视觉规范。
    lv_obj_t* back_btn = lv_button_create(scr);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, 72, 72);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20,
                            Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 32, 36);
    // 返回按钮自身的点击不应被全屏右滑手势拦截。
    screen_swipe_back_ignore(back_btn, true);

    lv_obj_t* back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_add_event_cb(
        back_btn, [](lv_event_t* /*e*/) { OnSwipeBack(); },
        LV_EVENT_CLICKED, nullptr);
}

void BuildUsageHint(lv_obj_t* scr) {
    // 屏幕最底部一行操作引导：「这是什么 · 怎么用」。
    // 暗灰色 + 20pt 字号，弱化但全程可见——即使蓝牙没连，
    // 用户也知道下一步该做什么。
    // 控件行容器虽然在 y=560..680，但实际按钮居中在内、可视底比容器
    // 早 ~20px 结束，因此提示距底边 16px 已能与按钮拉开 ~30px 的视觉间距。
    lv_obj_t* hint = lv_label_create(scr);
    lv_label_set_text(
        hint,
        "蓝牙音箱模式 · 手机蓝牙连接本设备后，用手机音乐 App 播放歌曲");
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8B92A3), LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(hint, kPanelSize - 60);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -kHintBottomMargin);
    screen_make_input_passive(hint);
}

void BuildSongTitle(lv_obj_t* scr) {
    s_ui.lbl_song = lv_label_create(scr);
    // 默认占位文本，等手机回传 song 字段时被覆盖。
    lv_label_set_text(s_ui.lbl_song, "蓝牙音乐");
    lv_obj_set_style_text_font(s_ui.lbl_song, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.lbl_song, lv_color_hex(kColorTextPrimary),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.lbl_song, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.lbl_song, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.lbl_song, kPanelSize - 80);
    lv_obj_align(s_ui.lbl_song, LV_ALIGN_TOP_MID, 0, kTitleY);
    screen_make_input_passive(s_ui.lbl_song);
}

void BuildAlbum(lv_obj_t* scr) {
    // 用一个跟 GIF 同样大小（240x240）的容器把 album 包起来：
    //   - radius = CIRCLE + clip_corner = true，所有超出 240 内切圆的像素
    //     都会被裁掉（也就是 GIF 四个白色方形角）；
    //   - 容器自身 opa = 0，被裁掉的角直接漏出底下的屏幕背景，
    //     看起来跟黑色背景融为一体；
    //   - 没有边框 / 描边 / 阴影，避免任何绿色或多余的圈线。
    lv_obj_t* mask = lv_obj_create(scr);
    lv_obj_set_size(mask, kAlbumMaskSize, kAlbumMaskSize);
    // 遮罩比 album 小，y 上加上 shrink 让 album 的中心保持原位。
    lv_obj_align(mask, LV_ALIGN_TOP_MID, 0, kAlbumY + kAlbumMaskShrink);
    screen_strip_obj_chrome(mask);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(mask, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(mask, true, LV_PART_MAIN);

    s_ui.album_eaf = lv_eaf_create(mask);
    lv_eaf_set_src(s_ui.album_eaf, "A:ic_s_music_album.eaf");
    lv_obj_set_size(s_ui.album_eaf, kAlbumSize, kAlbumSize);
    lv_image_set_inner_align(s_ui.album_eaf, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_center(s_ui.album_eaf);
    // 进入界面默认未播放，专辑动画保持静止。
    sync_album_eaf(s_ui.playing);

    screen_make_input_passive(mask);
}

void BuildLyric(lv_obj_t* scr) {
    for (int i = 0; i < kLyricLineCount; ++i) {
        lv_obj_t* lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "");
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorAccent),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, kLyricLineWidth);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0,
                     kLyricY + i * kLyricLineGap);
        // 三行各自的初始 opacity：顶 100%、中 60%、底 30%。
        lv_obj_set_style_opa(lbl, kLyricTargetOpa[i], LV_PART_MAIN);
        screen_make_input_passive(lbl);
        s_ui.lbl_lyric[i] = lbl;
    }
}

void BuildControls(lv_obj_t* scr) {
    lv_obj_t* row = lv_obj_create(scr);
    lv_obj_set_size(row, kCtrlRowWidth, kCtrlRowHeight);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, kCtrlRowY);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // 顺序：音量减 / 上一曲 / 播放暂停 / 下一曲 / 音量加
    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg,
                      kColorCtrlBtnBgPressed,
                      "A:ic_s_music_volume_down.spng", OnVolDownClicked);
    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg,
                      kColorCtrlBtnBgPressed,
                      "A:ic_s_player_previous.spng", OnPrevClicked);
    s_ui.img_play_icon = CreateRoundButton(row, kCtrlPlayBtnSize,
                                           kColorPlayBtnBg,
                                           kColorPlayBtnBgPressed,
                                           "A:ic_s_player_play.spng",
                                           OnPlayClicked);
    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg,
                      kColorCtrlBtnBgPressed,
                      "A:ic_s_player_next.spng", OnNextClicked);
    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg,
                      kColorCtrlBtnBgPressed,
                      "A:ic_s_music_volume_up.spng", OnVolUpClicked);
}

}  // namespace

lv_obj_t* MusicScreen::Create() {
    s_ui = MusicUi{};
    // 进入界面默认按钮是"▶ 播放"，等用户点一次才进入播放状态。
    s_ui.playing = false;
    s_rx_buffer.clear();

    lv_obj_t* scr = lv_obj_create(nullptr);
    screen_strip_obj_chrome(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(kColorBgGrad), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    BuildSongTitle(scr);
    BuildUsageHint(scr);
    BuildAlbum(scr);
    BuildLyric(scr);
    BuildControls(scr);
    // BackButton 最后建，保证它在 z-order 顶层、可被点中。
    BuildBackButton(scr);

    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    screen_attach_swipe_back(scr, OnSwipeBack);

    s_screen_active = true;
    return scr;
}

void MusicScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: music_screen -> switching BT to mode 3");
        // 让 BT 模块切到「音乐接收」模式三；命令需要 700ms 间隔，放后台 task。
        xTaskCreate(switch_to_mode3_task, "mus_mode3", 4096, nullptr, 5, nullptr);
        // 注册 UART RX 回调，开始监听手机回传的 JSON。
        s_rx_buffer.clear();
        SimpleUart::getInstance().registerCallback(on_uart_data);
    } else {
        ESP_LOGI(TAG, "unload: music_screen -> switching BT back to mode 1");
        // 摘掉回调，避免 UART task 仍向已经销毁的 UI 投递更新。
        SimpleUart::getInstance().registerCallback(
            std::function<void(const std::vector<uint8_t>&)>());
        s_screen_active = false;
        s_rx_buffer.clear();
        // 切回模式 1 同样需要 700ms 间隔，放后台 task 异步执行。
        xTaskCreate(switch_to_mode1_task, "mus_mode1", 4096, nullptr, 5, nullptr);
    }
}
