#pragma once

#include "lvgl.h"

// 测试页通用 UI 常量与行布局工具，供各测试项复用。

constexpr int kTestPanelW     = 720;
constexpr int kTestPanelH     = 720;
constexpr int kTestHeaderH    = 96;
constexpr int kTestRowH       = 84;
constexpr int kTestRowGap     = 10;
constexpr int kTestSideMargin = 20;
constexpr int kTestBodyY      = kTestHeaderH;
constexpr int kTestBodyH      = kTestPanelH - kTestHeaderH;

constexpr uint32_t kTestColorBg       = 0x0E1116;
constexpr uint32_t kTestColorCardBg   = 0x1B2030;
constexpr uint32_t kTestColorTextDim  = 0x9AA3B2;
constexpr uint32_t kTestColorHigh     = 0x10B981;
constexpr uint32_t kTestColorMuted    = 0x2A2F3A;
constexpr uint32_t kTestColorError    = 0xEF4444;

constexpr const char* kTestIconPass = "A:ic_app_test_pass.spng";
constexpr const char* kTestIconFail = "A:ic_app_test_fail.spng";

// 创建一行卡片：左侧标题 + 可选状态图标 + 右侧控件容器（flex 右对齐）。
// out_status_icon 传 nullptr 则不创建 pass/fail 图标（如震动马达手动测试项）。
// out_right_ctrl 返回右侧 flex 容器。
lv_obj_t* TestUiCreateRowShell(lv_obj_t* parent, const char* title,
                               lv_obj_t** out_status_icon,
                               lv_obj_t** out_right_ctrl);

void TestUiUpdateStatus(lv_obj_t* status_icon, bool pass);

// 测试页根 screen，Create 时设置，用于弹出确认框。
void TestUiSetScreen(lv_obj_t* scr);
lv_obj_t* TestUiGetScreen();

typedef void (*TestUiConfirmResultCb)(bool pass, void* user_data);

// 显示是/否确认框；pass=true 表示用户选「是」。
void TestUiShowConfirmDialog(const char* message, TestUiConfirmResultCb cb,
                             void* user_data);
void TestUiDismissConfirmDialog();

lv_obj_t* TestUiCreateValueLabel(lv_obj_t* right_ctrl);
lv_obj_t* TestUiCreateSwitch(lv_obj_t* right_ctrl, lv_event_cb_t cb,
                             void* user_data);
