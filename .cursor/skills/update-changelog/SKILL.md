---
name: update-changelog
description: >-
  After completing code changes in xingzhi-ai-395, append an entry to
  local-changes-summary.md at the project root. Use when implementing features,
  fixes, or refactors in this repo; when the user asks to record changes; or at
  the end of any non-trivial coding task before finishing the response.
---

# 本地修改记录维护

在本项目中，**每次完成实质性代码修改后**，必须更新根目录 [local-changes-summary.md](../../../local-changes-summary.md)。

> 该文件仅用于本地回溯，已加入 `.gitignore`，**禁止提交**。

## 何时更新

- 新增/修改 LVGL 页面、App 入口、主题资源
- Bug 修复、重构、CMake/分区/sdkconfig 配置变更
- OpenClaw / 蓝牙 / 音频 / 厂测等业务逻辑变更
- 用户明确要求「记录修改」「改动说明」

以下情况**可不写**：

- 仅回答问题、未改代码
- 纯格式化、无行为变化（除非用户要求）

## 更新步骤

1. 打开 `local-changes-summary.md`
2. 在 **`---` 分隔线之后、最新一条记录之上** 插入新条目（保持倒序：新的在上）
3. 使用当天日期（`YYYY-MM-DD`）和简短标题（模块或功能名）
4. 用 bullet 列出：**做了什么、为什么相关**（1–5 条即可）
5. 若有明显文件变更，可选增加「涉及文件」小节
6. **不要**删除历史记录；**不要**改写已有条目，除非用户要求整理
7. **不要** stage 或 commit 该文件

## 条目格式

```markdown
## YYYY-MM-DD — 简短标题（模块或功能名）

### 变更摘要
- 具体改动 1
- 具体改动 2

### 涉及文件（可选）
- `main/...`
```

同一次对话中若连续完成多个**独立主题**的改动，可写多条记录；若属于同一功能闭环，合并为一条。

## 写作要求

- 中文，简洁，面向后续开发者
- 写「能力/行为变化」，少堆文件名；文件多时用分组或表格
- UI 变更注明屏幕/Tab；NVS 变更注明键名；分区/资源变更注明分区名
- HTTP/API 变更注明路径与方法；硬件相关注明板型或 GPIO
- 与本次对话无关的旧功能不要重复抄写进新条目

## 完成检查

- [ ] `local-changes-summary.md` 已追加本次条目
- [ ] 日期与标题正确
- [ ] 条目内容与本次 diff 一致
- [ ] 未 stage / commit 该文件

## 示例

```markdown
## 2026-07-10 — 主屏空闲关机 NVS 配置

### 变更摘要
- 主屏空闲计时改为读取 NVS 动态配置（`display/idle_off_min`），默认 5 分钟
- 设置 Tab「待机」：滑块配置 0~60 分钟，0 表示永不关机
- 修复 NVS 键名超过 15 字符导致拖动崩溃的问题

### 涉及文件
- `main/display/settings_screen.cc`
- `main/display/home_screen.cc`
```
