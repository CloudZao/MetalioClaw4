#pragma once

#include "lvgl.h"
#include "screen_util.h"

// ---------------------------------------------------------------------------
// SdCardScreen
//
// SD 卡管理 App。
//   - 顶部返回按钮 + 标题
//   - SD 卡检测状态（已插入 / 未检测到）
//   - 已插入时显示剩余容量 / 总容量
//   - 一级目录文件列表，每行右侧有删除按钮
//
// 设计：
//   SD 卡的 mount/unmount 由板级（XINGZHI_395::InitializeSdCard()）通过
//   SdCardManager 单例完成，开机就挂好。本页面只读 SdCardManager 的状态、
//   做 UI 渲染和文件删除，不再自己 mount，也不会在退出时 unmount。
// ---------------------------------------------------------------------------
class SdCardScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
