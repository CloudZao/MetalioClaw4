#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// ThemeManager
//
// 设备 UI 主题切换的 NVS 管理。一共 4 套主题（theme1 ~ theme4），
// 由 spng 图标文件名前缀区分：
//   ic_app_home_theme1_chat.spng
//   ic_app_home_theme2_chat.spng
//   ic_app_home_theme3_chat.spng
//   ic_app_home_theme4_chat.spng
//
// HomeScreen 在构建时读取当前主题 id，并拼接成具体图标资源路径。
// 用户在「主题」App 中选择新主题后：
//   SetCurrentThemeId(N) -> 写 NVS -> HomeScreen::Create() 按新前缀加载图标。
// 主屏在运行时重建即可热切换，无需重启设备。
// ---------------------------------------------------------------------------
namespace ThemeManager {

constexpr int kMinThemeId     = 1;
constexpr int kMaxThemeId     = 4;
constexpr int kThemeCount     = kMaxThemeId - kMinThemeId + 1;
constexpr int kDefaultThemeId = 1;

// 读取当前主题 id (1 ~ 4)。未配置 / 越界都返回 kDefaultThemeId。
int GetCurrentThemeId();

// 持久化主题 id。id 不在 [1, 4] 范围内直接忽略，不抛错。
void SetCurrentThemeId(int id);

}  // namespace ThemeManager
