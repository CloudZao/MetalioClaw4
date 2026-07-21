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
//   - 文件列表可浏览子目录：点文件夹进入，返回/右滑在子目录走上一级
//   - 点 jpg/png/sjpg 全屏预览图片；点 txt 预览文本（可滚动）；预览页左上角返回
//   - 文件行右侧有删除按钮（目录不可删）
//
// 设计：
//   SD 卡的 mount/unmount 由板级（METALIO_CLAW_4::InitializeSdCard()）通过
//   SdCardManager 单例完成，开机就挂好。本页面只读 SdCardManager 的状态、
//   做 UI 渲染和文件删除，不再自己 mount，也不会在退出时 unmount。
// ---------------------------------------------------------------------------
class SdCardScreen {
public:
    static lv_obj_t* Create();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
