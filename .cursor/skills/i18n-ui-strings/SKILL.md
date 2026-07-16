---
name: i18n-ui-strings
description: >-
  Maintain runtime UI i18n for xingzhi-ai-395. Use whenever adding, changing, or
  reviewing user-visible LVGL/UI strings (labels, buttons, tabs, dialogs, status
  text, format strings shown on screen). Do NOT use for ESP_LOG* messages. Ensures
  every new display string is added to main/i18n/catalog.json with translations
  for all locales (zh-CN, en-US, and future ja-JP/ko-KR…), regenerated tables,
  and code uses I18n::T / I18n::Tr.
---

# UI 多语言（i18n）维护

本项目 UI 文案使用**运行时 i18n**（可切换语言），与编译期 `Lang::Strings` / `assets/locales/*/language.json`（系统状态/音效）并行存在。

> **硬性规则**：凡改动包含**新的界面显示字符串**（用户能在屏上看到的文字），必须同步写入语言配置并补齐**所有已支持语言**的翻译。Log 文案不用管。

## 架构速查

| 路径 | 作用 |
|------|------|
| `main/i18n/catalog.json` | **唯一手改源**：词条 + 各语言译文 |
| `scripts/gen_i18n.py` | 生成 `main/i18n/i18n_strings_gen.h` |
| `main/i18n/i18n.h` / `i18n.cc` | `I18n::T` / `Tr` / `SetLocale` / NVS |
| 设置 → **语言** Tab | 运行时切换；写 NVS `ui`/`locale` 后重建主页 |

支持语言列表在 `catalog.json` → `locales[]`。当前：`zh-CN`、`en-US`。扩展日语/韩语时在此追加，并给**每条** string 增加对应字段。

## 代码怎么写

**首选（gettext 风格，msgid = 中文源文案）：**

```cpp
#include "i18n.h"

lv_label_set_text(title, I18n::T("设置"));
std::snprintf(buf, sizeof(buf), I18n::T("%d 分钟"), minutes);
```

**类型安全（适合高频/关键键）：**

```cpp
lv_label_set_text(title, I18n::Tr(I18n::Str::SETTINGS));
```

**禁止：**

- 在 UI 路径直接写死仅中文、且不入库的新文案
- 把 `I18n::T(...)` 放进 `constexpr` 数组初始化（非常量表达式）
- 翻译**数据键**（如天气 API 中文条件 → icon 码映射表必须保持中文 msgid，显示时再 `I18n::T`）
- 给 `ESP_LOG*` 包 `I18n::T`
- 把 `I18n::T("...")` 与 `PRId32` / 相邻 `"..."` 字面量拼接（非法）。含 `%" PRId32 "` 的格式串应改成**单一**完整 format（优先 `%d`）再包一层 `I18n::T`
- 在**静态初始化**里调用 `I18n::T`（会在 `I18n::Init` 之前执行，语言被冻成默认中文）。表内只存 msgid，显示时再 `I18n::T`

**静态表模式（推荐）：** 表内存中文 msgid，显示时再译：

```cpp
constexpr const char* kName = "聊天";
lv_label_set_text(lbl, I18n::T(kName));
```

## 新增 / 修改显示字符串（必做清单）

1. **找出所有用户可见新文案**（含 `snprintf` 格式串、Tab 名、对话框、空态提示）
2. 打开 `main/i18n/catalog.json`，在 `strings` 中新增或更新条目：

```json
{
  "key": "MY_NEW_LABEL",
  "zh-CN": "我的新文案",
  "en-US": "My new label"
}
```

3. **每个** `locales[].id` 都必须有对应字段；缺省语言时生成器会回退到 `default_locale`，但提交前应补全，避免英文界面漏翻
4. `key`：`UPPER_SNAKE_CASE`，语义清晰，全局唯一
5. 重新生成：

```bash
python3 scripts/gen_i18n.py
```

（CMake 也会在构建时生成；本地改完请先跑一遍，避免 IDE 看到旧 enum）

6. 代码里用 `I18n::T("我的新文案")` 或 `I18n::Tr(I18n::Str::MY_NEW_LABEL)`
7. 若本次改动是功能开发，按项目惯例更新 `local-changes-summary.md`

## 新增一种语言（如 ja-JP）

1. 在 `catalog.json` 的 `locales` 追加：

```json
{ "id": "ja-JP", "name": "日本語", "english_name": "Japanese" }
```

2. 为 `strings` 里**每一条**增加 `"ja-JP": "..."`（可先用 en-US 占位，但应尽快补全）
3. 运行 `python3 scripts/gen_i18n.py`
4. 设置页语言 Tab 会通过 `I18n::GetLocaleCount()` **自动列出**新语言，无需改 Tab 布局代码
5. 确认字体能覆盖新文字（日/韩可能需加字库；缺字形时先评估再合入）

## 语言切换行为

- `I18n::Init()`：开机读 NVS（`main.cc` 已调用）
- `I18n::SetLocale` / `SetLocaleByCode`：写 NVS 并切换
- UI 刷新：与主题类似，**重建屏幕**（设置语言 Tab 会 `HomeScreen::Create()`）。若在别的屏改语言，至少重建当前屏 + 主页

## 自检

- [ ] 新 UI 字符串已进 `catalog.json`
- [ ] 所有已支持 locale 都有译文
- [ ] 已运行 `gen_i18n.py`
- [ ] 代码使用 `I18n::T` / `Tr`，且未破坏 `constexpr` / 数据键表
- [ ] 未翻译 log
- [ ] （可选）在设备上中/英切换抽查相关界面

## 与旧 `Lang::Strings` 的关系

| 体系 | 用途 | 能否运行时切换 |
|------|------|----------------|
| `I18n::*` + `catalog.json` | LVGL App / 设置等 UI | ✅ |
| `Lang::Strings` + `assets/locales/*/language.json` | 配网/OTA/旧 Display/音效，Kconfig 编译期语言 | ❌ |

新 UI 一律走 `I18n`。不要把新界面文案只写进 `language.json`。
