#pragma once

#include "screen_util.h"

// ---------------------------------------------------------------------------
// PWR_KEY 统一管理
//
// 开机调用 PwrKey_Init() 一次，注册短按 + 长按，之后不再销毁。
// 各页面在 LOAD / UNLOAD 时通过 PwrKey_OnScreenLifecycle() 维护前台栈；
// 按键回调只读栈顶做分发与日志。
//
// 短按进待机仅当栈顶为 "home"。UNLOAD 绝不会回落成 "home"，避免
// 一级页跳二级页时父页销毁把前台误标成首页。
// ---------------------------------------------------------------------------

// 注册短按 / 长按。IO 扩展器就绪后调用；内部有 once 守卫。
void PwrKey_Init();

// 页面生命周期联动：LOAD 压栈，UNLOAD 移除最靠上的同名页。
void PwrKey_OnScreenLifecycle(const char* name, screen_lifecycle_event_t event);

// 当前前台页面名（永不为空；栈空时为 "none"）。
const char* PwrKey_ActiveScreen();
