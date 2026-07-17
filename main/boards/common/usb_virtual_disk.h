#pragma once

#include <functional>

// ESP32-P4：GPIO24/25 在 USB Serial/JTAG 与 TinyUSB MSC（虚拟 U 盘）间切换。
// 非 P4 / 未启用 MSC 时提供空实现，SD 页按钮会隐藏或提示不可用。
class UsbVirtualDisk {
public:
    enum class UiHint {
        Idle,
        Switching,
        Enabling,
        Disabling,
        EnabledHost,
        EnabledLocal,
        Disabled,
        EnableFailed,
        DisableFailed,
        NoSdCard,
        FormatRequired,
        HostBusy,
    };

    using UiNotifyFn = std::function<void()>;

    static UsbVirtualDisk& GetInstance();

    UsbVirtualDisk(const UsbVirtualDisk&) = delete;
    UsbVirtualDisk& operator=(const UsbVirtualDisk&) = delete;

    // 创建 worker；启动时保持 USJ。可重复调用。
    void Init();

    bool IsSupported() const;
    bool IsGadgetActive() const;
    bool IsBusy() const;
    bool IsSdExportedToHost() const;
    UiHint GetUiHint() const;

    // 按钮切换：启用 / 停用虚拟 U 盘（异步，经 worker）。
    void Toggle();

    // 若当前已启用（或正在切换），异步停用；离开 SD 页时调用。
    void DisableIfActive();

    // SD 页注册；worker 完成后在 LVGL 线程外调用，UI 侧用 lv_async_call 刷新。
    void SetUiNotify(UiNotifyFn fn);

    // 中文 msgid，配合 I18n::T 显示。
    static const char* HintMsgid(UiHint hint);

private:
    UsbVirtualDisk() = default;
};
