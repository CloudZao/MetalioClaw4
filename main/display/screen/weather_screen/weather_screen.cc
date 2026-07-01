#include "weather_screen.h"

#include "Weather.hpp"
#include "home_screen/home_screen.h"
#include "screen_util.h"
#include "settings.h"
#include "weather_city_list.h"
#include "weather_icon_map.h"

#include "esp_lv_adapter.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr int32_t kPanelW = 720;
constexpr int32_t kHeaderH  = 88;
constexpr int32_t kBackBtnSize = 72;
constexpr int32_t kHeaderSidePad = 16;
constexpr int32_t kRegionBtnW = 72;
constexpr int32_t kRefreshBtnW = 72;
constexpr int32_t kHeaderBtnH = 56;
constexpr int32_t kHeaderBtnGap = 8;
constexpr int32_t kModalCardW = 560;
constexpr int32_t kModalCardPad = 24;
constexpr int32_t kModalDdW = kModalCardW - kModalCardPad * 2;
constexpr int32_t kModalDdH = 44;
constexpr int32_t kTabBarH = 52;
constexpr int32_t kTabPad  = 14;
constexpr int32_t kInnerW = kPanelW - kTabPad * 2;

// 资源原生 128×128；6 日预报（跳过今天）3 列网格
constexpr int32_t kHeroIconSize      = 128;
constexpr int32_t kForecastIconSize  = 128;
constexpr int32_t kForecastCols        = 3;
constexpr int32_t kForecastDayCount    = 6;
constexpr int32_t kForecastGridPadH  = 4;
constexpr int32_t kForecastColGap    = 10;
constexpr int32_t kForecastRowGap    = 10;
constexpr int32_t kForecastCardPad   = 6;
constexpr int32_t kForecastUsableW =
    kInnerW - kForecastGridPadH * 2;
constexpr int32_t kForecastCardW =
    (kForecastUsableW - kForecastColGap * (kForecastCols - 1)) / kForecastCols;
constexpr int32_t kForecastCardH = 244;

constexpr uint32_t kColorBg         = 0x0E1116;
constexpr uint32_t kColorBgGrad     = 0x161A22;
constexpr uint32_t kColorCard       = 0x2A2F3A;
constexpr uint32_t kColorCardAccent = 0xE0FB3C;
constexpr uint32_t kColorText       = 0xFFFFFF;
constexpr uint32_t kColorSubtle     = 0x9AA3B2;
constexpr uint32_t kColorAccent     = 0x60A5FA;
constexpr uint32_t kColorTabActive  = 0x3B82F6;
constexpr uint32_t kColorHeaderBg   = 0x12151C;
constexpr uint32_t kColorDivider    = 0x2A2F3A;
constexpr uint32_t kColorHeaderBtn  = 0x2A2F3A;
constexpr uint32_t kColorHeaderBtnBorder = 0x3B4556;

lv_obj_t* s_screen       = nullptr;
lv_obj_t* s_tabview      = nullptr;
lv_obj_t* s_tab_overview  = nullptr;
lv_obj_t* s_tab_forecast  = nullptr;
lv_obj_t* s_tab_hours     = nullptr;
lv_obj_t* s_tab_index     = nullptr;
lv_obj_t* s_status_lbl   = nullptr;
lv_obj_t* s_prov_dd      = nullptr;
lv_obj_t* s_city_dd      = nullptr;
lv_obj_t* s_dist_dd      = nullptr;
lv_obj_t* s_city_dlg_mask = nullptr;
std::string s_prov_options;
std::string s_city_options;
std::string s_dist_options;
bool s_dd_syncing        = false;
std::atomic<uint32_t> s_session{0};

const lv_font_t* font_main() { return &font_puhui_30_4; }
const lv_font_t* font_sub()  { return &font_puhui_20_4; }

void TriggerFetch();

void StyleDropdown(lv_obj_t* dd) {
    lv_obj_set_size(dd, kModalDdW, kModalDdH);
    lv_obj_set_style_radius(dd, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dd, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dd, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(dd, lv_color_hex(0x4B5563), LV_PART_MAIN);
    lv_obj_set_style_text_color(dd, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(dd, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_pad_left(dd, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_right(dd, 28, LV_PART_MAIN);
    lv_dropdown_set_symbol(dd, LV_SYMBOL_DOWN);
    screen_swipe_back_ignore(dd, true);
}

void StyleHeaderBtn(lv_obj_t* btn) {
    lv_obj_set_style_radius(btn, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorHeaderBtn), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(kColorHeaderBtnBorder),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3B4556),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    screen_swipe_back_ignore(btn, true);
}

void OnDropdownListOpened(lv_event_t* e) {
    lv_obj_t* dd = lv_event_get_target_obj(e);
    if (dd == nullptr) {
        return;
    }
    lv_obj_t* list = lv_dropdown_get_list(dd);
    if (list == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(list, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_text_color(list, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(list, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_radius(list, 10, LV_PART_MAIN);
    lv_obj_set_style_max_height(list, 320, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(kColorTabActive),
                              static_cast<lv_part_t>(LV_PART_SELECTED) |
                                  static_cast<lv_state_t>(LV_STATE_CHECKED));
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER,
                            static_cast<lv_part_t>(LV_PART_SELECTED) |
                                static_cast<lv_state_t>(LV_STATE_CHECKED));
    screen_swipe_back_ignore(list, true);
}

void AppendDropdownOption(std::string& buf, const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    if (!buf.empty()) {
        buf += '\n';
    }
    buf += name;
}

void FillProvinceOptions() {
    if (s_prov_dd == nullptr) {
        return;
    }
    s_prov_options.clear();
    for (size_t i = 0; i < weather_cities::kProvinceCount; ++i) {
        AppendDropdownOption(s_prov_options, weather_cities::kProvinces[i].name);
    }
    lv_dropdown_set_options(s_prov_dd, s_prov_options.c_str());
}

void FillCityOptions(size_t province_idx) {
    if (s_city_dd == nullptr || province_idx >= weather_cities::kProvinceCount) {
        return;
    }
    const weather_cities::ProvinceEntry& pe =
        weather_cities::kProvinces[province_idx];
    s_city_options.clear();
    for (uint16_t i = 0; i < pe.city_count; ++i) {
        AppendDropdownOption(s_city_options,
                             weather_cities::kCities[pe.city_start + i].name);
    }
    lv_dropdown_set_options(s_city_dd, s_city_options.c_str());
}

void FillDistrictOptions(size_t province_idx, size_t city_idx) {
    if (s_dist_dd == nullptr || province_idx >= weather_cities::kProvinceCount) {
        return;
    }
    const weather_cities::ProvinceEntry& pe =
        weather_cities::kProvinces[province_idx];
    if (city_idx >= pe.city_count) {
        return;
    }
    const weather_cities::CityEntry& ce =
        weather_cities::kCities[pe.city_start + city_idx];
    s_dist_options.clear();
    for (uint16_t i = 0; i < ce.district_count; ++i) {
        AppendDropdownOption(s_dist_options,
                             weather_cities::kDistricts[ce.district_start + i].name);
    }
    lv_dropdown_set_options(s_dist_dd, s_dist_options.c_str());
}

std::string LoadSavedDistrictId() {
    Settings settings("weather", false);
    std::string id =
        settings.GetString("district_id", WeatherService::kDefaultDistrictId);
    if (weather_cities::FindDistrictById(id.c_str()).province_idx >=
        weather_cities::kProvinceCount) {
        id = WeatherService::kDefaultDistrictId;
    }
    return id;
}

void SaveDistrictId(const char* district_id) {
    if (district_id == nullptr || district_id[0] == '\0') {
        return;
    }
    Settings settings("weather", true);
    settings.SetString("district_id", district_id);
}

const char* SelectedDistrictId() {
    if (s_prov_dd == nullptr || s_city_dd == nullptr || s_dist_dd == nullptr) {
        return WeatherService::kDefaultDistrictId;
    }
    const size_t prov = lv_dropdown_get_selected(s_prov_dd);
    const size_t city = lv_dropdown_get_selected(s_city_dd);
    const size_t dist = lv_dropdown_get_selected(s_dist_dd);
    const weather_cities::DistrictEntry* entry =
        weather_cities::GetDistrict(prov, city, dist);
    if (entry == nullptr) {
        return WeatherService::kDefaultDistrictId;
    }
    return entry->id;
}

void CloseCityPickerDialog() {
    if (s_city_dlg_mask != nullptr) {
        lv_obj_delete(s_city_dlg_mask);
        s_city_dlg_mask = nullptr;
    }
    s_prov_dd = nullptr;
    s_city_dd = nullptr;
    s_dist_dd = nullptr;
}

void SyncDropdownsFromDistrictId(const char* district_id) {
    if (s_prov_dd == nullptr || s_city_dd == nullptr || s_dist_dd == nullptr) {
        return;
    }
    const weather_cities::LookupResult found =
        weather_cities::FindDistrictById(district_id);
    size_t prov = found.province_idx;
    size_t city = found.city_idx;
    size_t dist = found.district_idx;
    if (prov >= weather_cities::kProvinceCount) {
        const weather_cities::LookupResult fallback =
            weather_cities::FindDistrictById(WeatherService::kDefaultDistrictId);
        prov = fallback.province_idx;
        city = fallback.city_idx;
        dist = fallback.district_idx;
    }

    s_dd_syncing = true;
    FillProvinceOptions();
    lv_dropdown_set_selected(s_prov_dd, static_cast<uint32_t>(prov));
    FillCityOptions(prov);
    lv_dropdown_set_selected(s_city_dd, static_cast<uint32_t>(city));
    FillDistrictOptions(prov, city);
    lv_dropdown_set_selected(s_dist_dd, static_cast<uint32_t>(dist));
    s_dd_syncing = false;
}

void OnProvinceChanged(lv_event_t* /*e*/) {
    if (s_dd_syncing || s_prov_dd == nullptr) {
        return;
    }
    const size_t prov = lv_dropdown_get_selected(s_prov_dd);
    s_dd_syncing = true;
    FillCityOptions(prov);
    lv_dropdown_set_selected(s_city_dd, 0);
    FillDistrictOptions(prov, 0);
    lv_dropdown_set_selected(s_dist_dd, 0);
    s_dd_syncing = false;
}

void OnCityChanged(lv_event_t* /*e*/) {
    if (s_dd_syncing || s_prov_dd == nullptr || s_city_dd == nullptr) {
        return;
    }
    const size_t prov = lv_dropdown_get_selected(s_prov_dd);
    const size_t city = lv_dropdown_get_selected(s_city_dd);
    s_dd_syncing = true;
    FillDistrictOptions(prov, city);
    lv_dropdown_set_selected(s_dist_dd, 0);
    s_dd_syncing = false;
}

void OnDistrictChanged(lv_event_t* /*e*/) {}

void OnCityPickerCancelClicked(lv_event_t* /*e*/) { CloseCityPickerDialog(); }

void OnCityPickerMaskClicked(lv_event_t* e) {
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    CloseCityPickerDialog();
}

void OnCityPickerSaveClicked(lv_event_t* /*e*/) {
    const char* district_id = SelectedDistrictId();
    SaveDistrictId(district_id);
    WeatherService::Instance().SetDistrictId(district_id);
    CloseCityPickerDialog();
    TriggerFetch();
}

void OpenCityPickerDialog() {
    if (s_screen == nullptr || s_city_dlg_mask != nullptr) {
        return;
    }

    constexpr int32_t kCardH = 480;
    constexpr int32_t kBtnW = 200;
    constexpr int32_t kBtnH = 56;
    constexpr int32_t kBtnRowH = 72;

    lv_obj_t* mask = lv_obj_create(s_screen);
    s_city_dlg_mask = mask;
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelW, kPanelW);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    lv_obj_add_event_cb(mask, OnCityPickerMaskClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kModalCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, kModalCardPad, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);

    lv_obj_t* dlg_title = lv_label_create(card);
    lv_label_set_text(dlg_title, "选择地区");
    lv_obj_set_style_text_color(dlg_title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(dlg_title, font_main(), LV_PART_MAIN);
    lv_obj_set_style_text_align(dlg_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(dlg_title, LV_PCT(100));
    lv_obj_set_style_pad_bottom(dlg_title, 8, LV_PART_MAIN);
    lv_obj_remove_flag(dlg_title, LV_OBJ_FLAG_CLICKABLE);

    auto make_picker_row = [&](const char* label, lv_obj_t** out_dd) {
        lv_obj_t* row = lv_obj_create(card);
        screen_strip_obj_chrome(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(row, 4, LV_PART_MAIN);

        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, font_sub(), LV_PART_MAIN);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        *out_dd = lv_dropdown_create(row);
        StyleDropdown(*out_dd);
        lv_obj_set_width(*out_dd, LV_PCT(100));
        lv_obj_add_event_cb(*out_dd, OnDropdownListOpened, LV_EVENT_READY, nullptr);
    };

    make_picker_row("省份", &s_prov_dd);
    lv_obj_add_event_cb(s_prov_dd, OnProvinceChanged, LV_EVENT_VALUE_CHANGED,
                        nullptr);

    make_picker_row("城市", &s_city_dd);
    lv_obj_add_event_cb(s_city_dd, OnCityChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    make_picker_row("区县", &s_dist_dd);
    lv_obj_add_event_cb(s_dist_dd, OnDistrictChanged, LV_EVENT_VALUE_CHANGED,
                        nullptr);

    SyncDropdownsFromDistrictId(WeatherService::Instance().DistrictId().c_str());

    lv_obj_t* btn_row = lv_obj_create(card);
    screen_strip_obj_chrome(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, kBtnRowH);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(btn_row, 12, LV_PART_MAIN);

    lv_obj_t* cancel = lv_button_create(btn_row);
    lv_obj_set_size(cancel, kBtnW, kBtnH);
    StyleHeaderBtn(cancel);
    lv_obj_add_event_cb(cancel, OnCityPickerCancelClicked, LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_t* cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "取消");
    lv_obj_set_style_text_font(cancel_lbl, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_center(cancel_lbl);
    lv_obj_remove_flag(cancel_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* save = lv_button_create(btn_row);
    lv_obj_set_size(save, kBtnW, kBtnH);
    lv_obj_set_style_radius(save, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(save, lv_color_hex(kColorTabActive), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(save, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(save, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(save, true);
    lv_obj_add_event_cb(save, OnCityPickerSaveClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* save_lbl = lv_label_create(save);
    lv_label_set_text(save_lbl, "保存");
    lv_obj_set_style_text_font(save_lbl, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_center(save_lbl);
    lv_obj_remove_flag(save_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void OnCityPickerOpenClicked(lv_event_t* /*e*/) { OpenCityPickerDialog(); }

void SetWeatherIcon(lv_obj_t* icon, const std::string& text, int32_t display_size) {
    if (icon == nullptr) {
        return;
    }
    const char* code = WeatherIconCodeForText(text);
    if (code == nullptr) {
        lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char path[48];
    std::snprintf(path, sizeof(path), "A:ic_s_weather_%s.spng", code);
    lv_obj_set_size(icon, display_size, display_size);
    lv_image_set_src(icon, path);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font,
                    uint32_t color, lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, align, LV_PART_MAIN);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    screen_make_input_passive(lbl);
    return lbl;
}

lv_obj_t* MakeSectionTitle(lv_obj_t* parent, const char* title) {
    lv_obj_t* lbl = MakeLabel(parent, title, font_main(), kColorText);
    lv_obj_set_width(lbl, kInnerW);
    lv_obj_set_style_pad_top(lbl, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(lbl, 8, LV_PART_MAIN);
    return lbl;
}

void AppendDetailRow(lv_obj_t* grid, const char* key, const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return;
    }
    char line[96];
    std::snprintf(line, sizeof(line), "%s  %s", key, value);
    lv_obj_t* lbl = MakeLabel(grid, line, font_sub(), kColorSubtle);
    lv_obj_set_width(lbl, (kInnerW - 12) / 2);
}

void FormatUptime(const std::string& uptime, char* buf, size_t len) {
    if (uptime.size() >= 12) {
        std::snprintf(buf, len, "%s-%s %s:%s", uptime.substr(4, 2).c_str(),
                      uptime.substr(6, 2).c_str(), uptime.substr(8, 2).c_str(),
                      uptime.substr(10, 2).c_str());
        return;
    }
    std::snprintf(buf, len, "%s", uptime.c_str());
}

void FormatVis(int32_t vis_m, char* buf, size_t len) {
    if (vis_m >= 1000) {
        std::snprintf(buf, len, "%.1f km", vis_m / 1000.f);
    } else {
        std::snprintf(buf, len, "%" PRId32 " m", vis_m);
    }
}

void ClearContainer(lv_obj_t* container) {
    if (container == nullptr) {
        return;
    }
    while (lv_obj_get_child_count(container) > 0) {
        lv_obj_delete(lv_obj_get_child(container, 0));
    }
}

void StyleScrollTab(lv_obj_t* tab) {
    lv_obj_set_style_pad_all(tab, kTabPad, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tab, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(tab, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
}

lv_obj_t* CreateForecastCard(lv_obj_t* parent, const WeatherForecastDay& day,
                             int index) {
    const bool accent = (index % 2 == 1);
    const uint32_t bg = accent ? kColorCardAccent : kColorCard;
    const uint32_t fg = accent ? 0x1A1A1A : kColorText;
    const uint32_t fg2 = accent ? 0x404040 : kColorSubtle;

    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, kForecastCardW, kForecastCardH);
    screen_strip_obj_chrome(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, kForecastCardPad, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 2, LV_PART_MAIN);

    const int32_t text_w = kForecastCardW - kForecastCardPad * 2;

    lv_obj_t* week_lbl = MakeLabel(card, day.week.empty() ? "--" : day.week.c_str(),
                                   font_sub(), fg, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(week_lbl, text_w);

    char date_buf[16];
    if (day.date.size() >= 10) {
        const int month = std::atoi(day.date.substr(5, 2).c_str());
        const int day_num = std::atoi(day.date.substr(8, 2).c_str());
        std::snprintf(date_buf, sizeof(date_buf), "%02d/%02d", month, day_num);
    } else {
        std::snprintf(date_buf, sizeof(date_buf), "--");
    }
    lv_obj_t* date_lbl = MakeLabel(card, date_buf, font_sub(), fg2, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(date_lbl, text_w);

    lv_obj_t* icon = lv_image_create(card);
    SetWeatherIcon(icon, day.text_day, kForecastIconSize);
    screen_make_input_passive(icon);

    char temp_buf[32];
    std::snprintf(temp_buf, sizeof(temp_buf), "%" PRId32 "~%" PRId32 "°", day.low,
                  day.high);
    lv_obj_t* temp_lbl = MakeLabel(card, temp_buf, font_sub(), fg, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(temp_lbl, text_w);

    lv_obj_t* day_lbl = MakeLabel(card, day.text_day.c_str(), font_sub(), fg2,
                                  LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(day_lbl, text_w);
    lv_label_set_long_mode(day_lbl, LV_LABEL_LONG_WRAP);

    lv_obj_t* night_lbl = MakeLabel(card, day.text_night.c_str(), font_sub(), fg2,
                                    LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(night_lbl, text_w);
    lv_label_set_long_mode(night_lbl, LV_LABEL_LONG_WRAP);

    screen_make_input_passive(card);
    return card;
}

lv_obj_t* CreateHourRow(lv_obj_t* parent, const WeatherForecastHour& hour) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, LV_PART_MAIN);

    lv_obj_t* row1 = lv_obj_create(card);
    screen_strip_obj_chrome(row1);
    lv_obj_set_width(row1, LV_PCT(100));
    lv_obj_set_height(row1, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row1, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(row1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row1, 12, LV_PART_MAIN);

    std::string time_str = hour.data_time;
    if (time_str.size() >= 16) {
        time_str = time_str.substr(11, 5);
    }
    lv_obj_t* time_lbl = MakeLabel(row1, time_str.c_str(), font_sub(), kColorText);
    lv_obj_set_width(time_lbl, 56);

    char temp_buf[16];
    std::snprintf(temp_buf, sizeof(temp_buf), "%" PRId32 "°", hour.temp_fc);
    MakeLabel(row1, temp_buf, font_sub(), kColorAccent);

    MakeLabel(row1, hour.text.c_str(), font_sub(), kColorText);

    char detail[240];
    std::snprintf(detail, sizeof(detail),
                  "%s %s  湿度%" PRId32 "%%  降水%.1f  云量%" PRId32 "%%  "
                  "降水概率%" PRId32 "%%  UV%" PRId32 "  %" PRId32 "hPa 露点%" PRId32 "°  "
                  "风向角%" PRId32 "°",
                  hour.wind_dir.c_str(), hour.wind_class.c_str(), hour.rh, hour.prec1h,
                  hour.clouds, hour.pop, hour.uvi, hour.pressure, hour.dpt,
                  hour.wind_angle);
    lv_obj_t* det = MakeLabel(card, detail, font_sub(), kColorSubtle);
    lv_obj_set_width(det, LV_PCT(100));

    screen_make_input_passive(card);
    return card;
}

lv_obj_t* CreateIndexCard(lv_obj_t* parent, const WeatherLifeIndex& idx) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 4, LV_PART_MAIN);

    char title[64];
    std::snprintf(title, sizeof(title), "%s · %s", idx.name.c_str(), idx.brief.c_str());
    MakeLabel(card, title, font_sub(), kColorText);

    if (!idx.detail.empty()) {
        lv_obj_t* det = MakeLabel(card, idx.detail.c_str(), font_sub(), kColorSubtle);
        lv_obj_set_width(det, LV_PCT(100));
    }
    screen_make_input_passive(card);
    return card;
}

void ShowStatus(const char* text) {
    if (s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, text);
    }
}

void BuildOverviewTab(const WeatherDistrictData& data) {
    if (s_tab_overview == nullptr) {
        return;
    }
    ClearContainer(s_tab_overview);

    lv_obj_t* hero = lv_obj_create(s_tab_overview);
    lv_obj_set_width(hero, LV_PCT(100));
    lv_obj_set_height(hero, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(hero);
    lv_obj_remove_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(hero, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 8, LV_PART_MAIN);

    lv_obj_t* icon = lv_image_create(hero);
    SetWeatherIcon(icon, data.text, kHeroIconSize);
    screen_make_input_passive(icon);

    lv_obj_t* hero_text = lv_obj_create(hero);
    screen_strip_obj_chrome(hero_text);
    lv_obj_set_size(hero_text, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hero_text, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(hero_text, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hero_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hero_text, 4, LV_PART_MAIN);

    char loc[128];
    std::snprintf(loc, sizeof(loc), "%s %s %s", data.province.c_str(),
                  data.city.c_str(), data.district.c_str());
    MakeLabel(hero_text, loc, font_main(), kColorText);

    char temp_line[48];
    std::snprintf(temp_line, sizeof(temp_line), "%" PRId32 "°  %s", data.temp,
                  data.text.c_str());
    MakeLabel(hero_text, temp_line, font_main(), kColorAccent);

    char feel_line[48];
    std::snprintf(feel_line, sizeof(feel_line), "体感 %" PRId32 "°  湿度 %" PRId32 "%%",
                  data.feels_like, data.rh);
    MakeLabel(hero_text, feel_line, font_sub(), kColorSubtle);

    char wind_line[64];
    std::snprintf(wind_line, sizeof(wind_line), "%s %s  风向角 %" PRId32 "°",
                  data.wind_dir.c_str(), data.wind_class.c_str(), data.wind_angle);
    MakeLabel(hero_text, wind_line, font_sub(), kColorSubtle);

    screen_make_input_passive(hero);

    MakeSectionTitle(s_tab_overview, "实况详情");
    lv_obj_t* detail_grid = lv_obj_create(s_tab_overview);
    lv_obj_set_width(detail_grid, LV_PCT(100));
    lv_obj_set_height(detail_grid, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(detail_grid);
    lv_obj_remove_flag(detail_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(detail_grid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(detail_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(detail_grid, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(detail_grid, 6, LV_PART_MAIN);

    char buf[64];
    FormatVis(data.vis, buf, sizeof(buf));
    AppendDetailRow(detail_grid, "能见度", buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 "%%", data.clouds);
    AppendDetailRow(detail_grid, "云量", buf);
    std::snprintf(buf, sizeof(buf), "%.1f mm/h", data.prec1h);
    AppendDetailRow(detail_grid, "1h降水", buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 " hPa", data.pressure);
    AppendDetailRow(detail_grid, "气压", buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 "°", data.dpt);
    AppendDetailRow(detail_grid, "露点", buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32, data.uvi);
    AppendDetailRow(detail_grid, "紫外线", buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32, data.aqi);
    AppendDetailRow(detail_grid, "AQI", buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 " µg/m³", data.pm25);
    AppendDetailRow(detail_grid, "PM2.5", buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 " µg/m³", data.pm10);
    AppendDetailRow(detail_grid, "PM10", buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 " µg/m³", data.no2);
    AppendDetailRow(detail_grid, "NO₂", buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 " µg/m³", data.so2);
    AppendDetailRow(detail_grid, "SO₂", buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 " µg/m³", data.o3);
    AppendDetailRow(detail_grid, "O₃", buf);
    std::snprintf(buf, sizeof(buf), "%.1f mg/m³", data.co);
    AppendDetailRow(detail_grid, "CO", buf);
    FormatUptime(data.uptime, buf, sizeof(buf));
    AppendDetailRow(detail_grid, "更新", buf);
    std::snprintf(buf, sizeof(buf), "%s", data.district_id.c_str());
    AppendDetailRow(detail_grid, "区划ID", buf);
    std::snprintf(buf, sizeof(buf), "%s", data.country.c_str());
    AppendDetailRow(detail_grid, "国家", buf);
    screen_make_input_passive(detail_grid);

    if (!data.alerts.empty()) {
        MakeSectionTitle(s_tab_overview, "预警");
        for (const auto& alert : data.alerts) {
            lv_obj_t* card = lv_obj_create(s_tab_overview);
            lv_obj_set_width(card, LV_PCT(100));
            lv_obj_set_height(card, LV_SIZE_CONTENT);
            screen_strip_obj_chrome(card);
            lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_color(card, lv_color_hex(0x7F1D1D), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
            lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);
            lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
            MakeLabel(card, alert.title.c_str(), font_sub(), kColorText);
            if (!alert.content.empty()) {
                lv_obj_t* c = MakeLabel(card, alert.content.c_str(), font_sub(),
                                        kColorSubtle);
                lv_obj_set_width(c, LV_PCT(100));
            }
            screen_make_input_passive(card);
        }
    }

    lv_obj_set_style_pad_bottom(s_tab_overview, 24, LV_PART_MAIN);
}

void BuildForecastTab(const WeatherDistrictData& data) {
    if (s_tab_forecast == nullptr) {
        return;
    }
    ClearContainer(s_tab_forecast);

    if (data.forecasts.size() <= 1) {
        MakeLabel(s_tab_forecast, "暂无6日预报", font_main(), kColorSubtle,
                  LV_TEXT_ALIGN_CENTER);
        return;
    }

    const size_t start = 1;
    const size_t end = std::min(
        data.forecasts.size(),
        start + static_cast<size_t>(kForecastDayCount));

    lv_obj_t* fc_grid = lv_obj_create(s_tab_forecast);
    lv_obj_set_width(fc_grid, LV_PCT(100));
    lv_obj_set_height(fc_grid, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(fc_grid);
    lv_obj_remove_flag(fc_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(fc_grid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(fc_grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(fc_grid, kForecastRowGap, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(fc_grid, kForecastGridPadH, LV_PART_MAIN);

    for (int row = 0; row < 2; ++row) {
        lv_obj_t* fc_row = lv_obj_create(fc_grid);
        lv_obj_set_width(fc_row, LV_PCT(100));
        lv_obj_set_height(fc_row, kForecastCardH);
        screen_strip_obj_chrome(fc_row);
        lv_obj_set_style_bg_opa(fc_row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(fc_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(fc_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(fc_row, kForecastColGap, LV_PART_MAIN);
        lv_obj_set_flex_align(fc_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);

        for (int col = 0; col < kForecastCols; ++col) {
            const size_t idx = start + static_cast<size_t>(row * kForecastCols + col);
            if (idx >= end) {
                break;
            }
            CreateForecastCard(fc_row, data.forecasts[idx],
                               static_cast<int>(idx - start));
        }
    }
    screen_make_input_passive(fc_grid);
    lv_obj_set_style_pad_bottom(s_tab_forecast, 24, LV_PART_MAIN);
}

void BuildIndexTab(const WeatherDistrictData& data) {
    if (s_tab_index == nullptr) {
        return;
    }
    ClearContainer(s_tab_index);

    if (data.indexes.empty()) {
        MakeLabel(s_tab_index, "暂无生活指数", font_main(), kColorSubtle,
                  LV_TEXT_ALIGN_CENTER);
        return;
    }

    lv_obj_t* idx_box = lv_obj_create(s_tab_index);
    lv_obj_set_width(idx_box, LV_PCT(100));
    lv_obj_set_height(idx_box, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(idx_box);
    lv_obj_remove_flag(idx_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(idx_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(idx_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(idx_box, 8, LV_PART_MAIN);
    for (const auto& idx : data.indexes) {
        CreateIndexCard(idx_box, idx);
    }
    screen_make_input_passive(idx_box);
    lv_obj_set_style_pad_bottom(s_tab_index, 24, LV_PART_MAIN);
}

void BuildHoursTab(const WeatherDistrictData& data) {
    if (s_tab_hours == nullptr) {
        return;
    }
    ClearContainer(s_tab_hours);

    if (data.forecast_hours.empty()) {
        MakeLabel(s_tab_hours, "暂无24小时预报", font_main(), kColorSubtle,
                  LV_TEXT_ALIGN_CENTER);
        return;
    }

    MakeSectionTitle(s_tab_hours, "24小时预报");
    lv_obj_t* hour_box = lv_obj_create(s_tab_hours);
    lv_obj_set_width(hour_box, LV_PCT(100));
    lv_obj_set_height(hour_box, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(hour_box);
    lv_obj_remove_flag(hour_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(hour_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(hour_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hour_box, 8, LV_PART_MAIN);
    for (const auto& hour : data.forecast_hours) {
        CreateHourRow(hour_box, hour);
    }
    screen_make_input_passive(hour_box);
    lv_obj_set_style_pad_bottom(s_tab_hours, 24, LV_PART_MAIN);
}

void BuildWeatherUi(const WeatherDistrictData& data) {
    BuildOverviewTab(data);
    BuildForecastTab(data);
    BuildHoursTab(data);
    BuildIndexTab(data);
}

void ClearTabMessage(lv_obj_t* tab, const char* msg) {
    if (tab == nullptr) {
        return;
    }
    ClearContainer(tab);
    if (msg != nullptr && msg[0] != '\0') {
        MakeLabel(tab, msg, font_main(), kColorSubtle, LV_TEXT_ALIGN_CENTER);
    }
}

void ApplyWeatherData(const WeatherDistrictData& data) {
    if (data.valid) {
        BuildWeatherUi(data);
        ShowStatus("");
    } else {
        ClearTabMessage(s_tab_overview, "暂无天气数据");
        ClearTabMessage(s_tab_forecast, nullptr);
        ClearTabMessage(s_tab_hours, nullptr);
        ClearTabMessage(s_tab_index, nullptr);
        ShowStatus("加载失败");
    }
}

void ShowLoadingPlaceholder() {
    ClearTabMessage(s_tab_overview, "加载中...");
    ClearTabMessage(s_tab_forecast, nullptr);
    ClearTabMessage(s_tab_hours, nullptr);
    ClearTabMessage(s_tab_index, nullptr);
}

void FetchTask(void* arg) {
    const uint32_t my_session = reinterpret_cast<uintptr_t>(arg);
    WeatherDistrictData data;
    const esp_err_t err = WeatherService::Instance().Fetch(data);

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (s_session.load() == my_session && s_screen != nullptr) {
            if (err == ESP_OK) {
                ApplyWeatherData(data);
            } else {
                ApplyWeatherData(WeatherDistrictData{});
                ShowStatus("加载失败");
            }
        }
        esp_lv_adapter_unlock();
    }
    vTaskDelete(nullptr);
}

void TriggerFetch() {
    const uint32_t session =
        s_session.fetch_add(1, std::memory_order_relaxed) + 1;
    ShowStatus("加载中...");
    ShowLoadingPlaceholder();

    BaseType_t ok = xTaskCreate(
        FetchTask, "weather_fetch", 12 * 1024,
        reinterpret_cast<void*>(static_cast<uintptr_t>(session)), 4, nullptr);
    if (ok != pdPASS) {
        ShowStatus("任务创建失败");
    }
}

void OnSwipeBack();

void OnRefreshClicked(lv_event_t* /*e*/) { TriggerFetch(); }

void OnBackClicked(lv_event_t* /*e*/) { OnSwipeBack(); }

void BuildTabView(lv_obj_t* scr) {
    const int32_t body_h = kPanelW - kHeaderH;

    lv_obj_t* tv = lv_tabview_create(scr);
    s_tabview = tv;
    lv_obj_set_size(tv, kPanelW, body_h);
    lv_obj_set_pos(tv, 0, kHeaderH);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv, kTabBarH);
    lv_obj_set_style_bg_color(tv, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tv, 0, LV_PART_MAIN);

    lv_obj_t* bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(bar, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorTabActive),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_t* content = lv_tabview_get_content(tv);
    screen_swipe_back_ignore(content, true);

    s_tab_overview = lv_tabview_add_tab(tv, "今日天气");
    StyleScrollTab(s_tab_overview);

    s_tab_forecast = lv_tabview_add_tab(tv, "6日天气");
    StyleScrollTab(s_tab_forecast);

    s_tab_hours = lv_tabview_add_tab(tv, "24时天气");
    StyleScrollTab(s_tab_hours);

    s_tab_index = lv_tabview_add_tab(tv, "生活指数");
    StyleScrollTab(s_tab_index);
}

void OnSwipeBack() {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    s_session.fetch_add(1, std::memory_order_relaxed);
    CloseCityPickerDialog();
    s_screen = nullptr;
    s_tabview = nullptr;
    s_tab_overview = nullptr;
    s_tab_forecast = nullptr;
    s_tab_hours = nullptr;
    s_tab_index = nullptr;
    s_status_lbl = nullptr;
}

}  // namespace

lv_obj_t* WeatherScreen::Create() {
    s_session.fetch_add(1, std::memory_order_relaxed);

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelW);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(kColorBgGrad), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    BuildTabView(scr);

    lv_obj_t* top = lv_obj_create(scr);
    lv_obj_set_size(top, kPanelW, kHeaderH);
    lv_obj_set_pos(top, 0, 0);
    screen_strip_obj_chrome(top);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(top, lv_color_hex(kColorHeaderBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* divider = lv_obj_create(top);
    screen_strip_obj_chrome(divider);
    lv_obj_set_size(divider, kPanelW, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorDivider), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    screen_make_input_passive(divider);

    lv_obj_t* back = lv_button_create(top);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, kHeaderSidePad, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = MakeLabel(top, "天气", font_main(), kColorText);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, kHeaderSidePad + kBackBtnSize + 12, 0);

    s_status_lbl = MakeLabel(top, "", font_sub(), kColorSubtle);
    lv_obj_align_to(s_status_lbl, title, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

    lv_obj_t* refresh = lv_button_create(top);
    lv_obj_set_size(refresh, kRefreshBtnW, kHeaderBtnH);
    lv_obj_align(refresh, LV_ALIGN_RIGHT_MID, -kHeaderSidePad, 0);
    StyleHeaderBtn(refresh);
    lv_obj_add_event_cb(refresh, OnRefreshClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* refresh_lbl = lv_label_create(refresh);
    lv_label_set_text(refresh_lbl, "刷新");
    lv_obj_set_style_text_font(refresh_lbl, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_text_color(refresh_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_center(refresh_lbl);
    lv_obj_remove_flag(refresh_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* region = lv_button_create(top);
    lv_obj_set_size(region, kRegionBtnW, kHeaderBtnH);
    lv_obj_align(region, LV_ALIGN_RIGHT_MID,
                 -(kHeaderSidePad + kRefreshBtnW + kHeaderBtnGap), 0);
    StyleHeaderBtn(region);
    lv_obj_add_event_cb(region, OnCityPickerOpenClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* region_lbl = lv_label_create(region);
    lv_label_set_text(region_lbl, "地区");
    lv_obj_set_style_text_font(region_lbl, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_text_color(region_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_center(region_lbl);
    lv_obj_remove_flag(region_lbl, LV_OBJ_FLAG_CLICKABLE);

    const std::string saved_id = LoadSavedDistrictId();
    WeatherService::Instance().SetDistrictId(saved_id);

    ShowLoadingPlaceholder();
    TriggerFetch();

    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    screen_attach_swipe_back(scr, OnSwipeBack);
    return scr;
}
