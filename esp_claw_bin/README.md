# ESPClaw 附属固件

本目录存放 **ESPClaw（edge_agent）** 所需的分区镜像。主固件（xingzhi）烧录到 `ota_0` 后，若要使用主屏「ESPClaw」入口切到 `ota_1`，还须把这里的固件一并烧进对应地址，否则会提示未找到 ESPClaw 或缺少表情/存储等资源。

分区方案见 `partitions/v1/32m_dual.csv`（32MB 双系统）。

---

## 命名规则

```
{固件名}_{烧录地址}.bin
```

| 部分 | 说明 | 示例 |
|:---|:---|:---|
| `固件名` | 分区/镜像用途 | `edge_agent`、`emote_assets` |
| `烧录地址` | Flash 偏移（十六进制，带 `0x` 前缀） | `0xb00000` |
| 后缀 | 固定为 `.bin` | |

完整文件名示例：`edge_agent_0xb00000.bin` → 将内容烧录到地址 `0xb00000`。

---

## 当前文件与烧录地址

| 文件 | 烧录地址 | 对应分区 / 用途 |
|:---|:---|:---|
| `edge_agent_0xb00000.bin` | `0xb00000` | `ota_1`，ESPClaw 应用本身 |
| `emote_assets_0x1396000.bin` | `0x1396000` | `emote`（SPIFFS），表情资源 |
| `system_0x1796000.bin` | `0x1796000` | `system`（FAT） |
| `storage_0x1896000.bin` | `0x1896000` | `storage`（FAT） |

地址随分区表调整时，请同步重命名文件中的 `_0x...` 段，避免烧错偏移。

---

## 推荐：完整一次烧录

**推荐把主固件、分区表、模型/资源与 ESPClaw 相关镜像放在同一目录，用一条命令全部烧录**，避免漏烧或地址不一致。地址一律以文件名中的 `_0x...` 为准。

需要同时烧录的固件（文件名高亮如下）：

- `bootloader_0x2000.bin`
- `partition-table_0x9000.bin`
- `ota_data_initial_0x10e000.bin`
- `srmodels_0x111000.bin`
- `xiaozhi_0x200000.bin`
- `edge_agent_0xb00000.bin`
- `resources_0xf00000.bin`
- `factory_test_0x1300000.bin`
- `emote_assets_0x1396000.bin`
- `system_0x1796000.bin`
- `storage_0x1896000.bin`

其中本目录提供 ESPClaw 侧：`edge_agent_0xb00000.bin`、`emote_assets_0x1396000.bin`、`system_0x1796000.bin`、`storage_0x1896000.bin`；其余为主工程 bootloader / 分区表 / ota_data / 模型 / 主应用 / resources / factory_test 等，需与 `32m_dual` 分区方案配套。

在包含上述全部 `.bin` 的目录中执行：

```bash
esptool.py -p /dev/ttyUSB0 write_flash \
  0x2000     bootloader_0x2000.bin \
  0x9000     partition-table_0x9000.bin \
  0x10e000   ota_data_initial_0x10e000.bin \
  0x111000   srmodels_0x111000.bin \
  0x200000   xiaozhi_0x200000.bin \
  0xb00000   edge_agent_0xb00000.bin \
  0xf00000   resources_0xf00000.bin \
  0x1300000  factory_test_0x1300000.bin \
  0x1396000  emote_assets_0x1396000.bin \
  0x1796000  system_0x1796000.bin \
  0x1896000  storage_0x1896000.bin
```

也可在 ESP-IDF / ESP LaunchPad / 图形烧录工具中按同样地址一次性勾选写入。

---

## 说明

- 不推荐只烧主固件再单独补烧 ESPClaw；漏烧 `edge_agent` / `emote` / `system` / `storage` 时无法正常进入 ESPClaw。
- `edge_agent` 必须落在 `ota_1`（`0xb00000`）；主屏切换逻辑依赖该分区存在且可启动。
- 更新任一镜像时，保持「固件名_烧录地址」命名，并尽量按上一节做完整一次烧录。
