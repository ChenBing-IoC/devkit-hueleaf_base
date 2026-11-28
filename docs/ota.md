# OTA 空间评估（4 MB Flash / ESP32-C5）

本文只做空间与路线评估，不含 OTA 实现。

> **决定（2026-11-28）：本项目不做 OTA。** 升级方式定为 TF 卡 + `esptool.py` 手动烧录（路线 3）。本文件保留作为以后重新评估的依据；如果将来要发货并需要远程升级，按下文路线 1 或 2 重新算空间。

## 1. 现状

| 区域 | 偏移 | 大小（字节） | 大小（MiB） | 说明 |
| :--- | :--- | ---: | ---: | :--- |
| bootloader + 分区表 | 0x0 | 0x8000 | 32 KiB | bootloader 0x2000、分区表 0x8000 |
| nvs | 0x9000 | 0x6000 | 24 KiB | 命名空间 wifi_cfg / content / weather / pwrmgr / netcfg |
| phy_init | 0xF000 | 0x1000 | 4 KiB | PHY 校准数据 |
| factory（app） | 0x10000 | 0x2F0000 | 2.9375 MiB | 应用单分区 |
| image_cache | 0x300000 | 0x100000 | 1 MiB | 3 张图片 × 0x50000 + 4 KiB 元数据 |
| 合计 | — | 4,194,304 | 4 MiB | 4 MB Flash 全部占满 |

当前固件（含 mDNS / 安全配置 / 电量提示）约 **2.16 MiB（约 2.26 MB）**，factory 分区剩余约 0.78 MiB（27%）。应用还能继续增长，但没有第二个 app 分区。

## 2. 为什么双 bank 放不下

- 双 bank OTA 需要两个能容纳完整应用的分区：`2 × 2.16 MiB = 4.32 MiB`，已经超过 4 MiB 总量。
- 即使把应用瘦身到 1.6 MiB，`2 × 1.6 MiB + 1 MiB 图片缓存 + 64 KiB 启动区 = 4.26 MiB`，仍然超出 4 MiB。
- 所以“保留 3 槽缓存 + 双 bank”在 4 MB 上无解；必须放弃其中一项或改用 TF 卡。

## 3. 三条候选路线

### 路线 1：砍缓存到 2 槽（必要时压缩帧），换取 2 × 1.6 MiB OTA

思路：把 `image_cache` 从 1 MiB 缩到 704 KiB（2 个 0x50000 槽位 + 64 KiB 元数据），腾出的 320 KiB 与 factory 的 2.9375 MiB 合并，切成两个 1.625 MiB 的 OTA 分区。

建议分区表：

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xF000,   0x1000,
ota_0,    app,  ota_0,   0x10000,  0x1A0000,
ota_1,    app,  ota_1,   0x1B0000, 0x1A0000,
image_cache, data, 0x40, 0x350000, 0xB0000,
```

- 单分区 0x1A0000 = 1,703,936 字节（1.625 MiB），两个合计 3,407,872 字节；缓存 720,896 字节；加启动区正好 4 MiB。
- **代价一：应用必须先瘦身到 1.625 MiB 以内**（当前 2.16 MiB，需要减约 0.55 MiB）。可行的方向：压缩内嵌网页（gzip + 运行时解压）、裁剪 mbedTLS 证书包为固定 CA、关闭未使用的 Wi-Fi 协议、把图片/字体资源外置到 TF 卡。每一项都要单独评估启动时间与 RAM。
- **代价二：图片槽位从 3 降到 2**，并且分区表与 `screen_rotation.c` 的 `CACHE_SLOTS` 必须同时改；元数据偏移由 `CACHE_SLOTS * CACHE_SLOT_BYTES` 自动推导，改槽数后会落在 0xA0000，仍在新分区内。
- **代价三（可选“压缩帧”）**：当前槽位是 480×720、4 bit/像素的裸帧（172,800 字节）。若加 RLE/差分压缩，槽位可缩到约 0x28000，两个槽只占 320 KiB，多出的空间可把应用上限抬到约 1.8 MiB；但需要新的编解码路径和版本兼容，属于额外功能。
- 需要改动：`partitions.csv`、`sdkconfig`（`CONFIG_PARTITION_TABLE_TWO_OTA=y` 或继续自定义表）、`main/screen_driver/screen_rotation.c`（`CACHE_SLOTS`、槽位数相关 UI/接口上限）、新增 `esp_https_ota` 下载与回滚逻辑（建议 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`）。

### 路线 2：单 app 分区 + “下载到 TF 卡 → 重启进入最小刷写应用”

思路：保留 1 MiB `image_cache`，把 3.0 MiB 的 app 区间切成“大应用 + 小刷写应用”两块，并加 8 KiB `otadata` 让 bootloader 能在两者间切换（切换必须通过 `otadata`，`factory` 分区不能由 OTA 选择）：

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xF000,   0x1000,
otadata,  data, ota,     0x10000,  0x2000,
ota_0,    app,  ota_0,   0x20000,  0x2A0000,
ota_1,    app,  ota_1,   0x2C0000, 0x40000,
image_cache, data, 0x40, 0x300000, 0x100000,
```

`ota_0` 为 2.625 MiB（当前 2.16 MiB 仍有余量），`ota_1` 为 256 KiB 的刷写专用镜像。

流程：正常固件把新 `.bin` 下载到 TF 卡 → 校验长度/哈希 → `esp_ota_set_boot_partition(ota_1)` → 重启进入只做一件事的最小应用 → 用 `esp_partition_write` 把 TF 上的镜像写回 `ota_0` → 校验 FNV/SHA → 切回 `ota_0` 重启。

- **优点**：保留 3 张图片缓存；不需要 2×app；下载失败不影响运行中的固件。
- **代价**：需要维护第二个工程/目标（体积、构建产物、发布流程）；刷写期间断电会让 factory 处于半写状态，必须在 flasher 里保留“上次启动是否成功”的标志并支持重试，否则只能 USB 救砖；没有 A/B 回滚。
- 需要改动：`partitions.csv`、新增 `flasher` 目标与构建脚本、正常固件的下载/校验/切换逻辑、flasher 的 TF 读取与分区写入逻辑。

### 路线 3：不做 OTA，TF 卡 / USB 手动烧录（`esptool.py`）

保持现有分区与应用完全不变，升级时用电脑烧录 app 镜像。适合当前阶段：发布频率低、设备在手上、4 MB 空间紧张。

步骤：

1. 构建并确认产物：`examples/3.7einkDvb/build/3.7einkDvb.bin`（约 2.16 MiB）。
2. 只更新应用时，烧录地址是 `0x10000`；bootloader、分区表、NVS 都不动：

   ```bash
   esptool.py --chip esp32c5 -p COM5 -b 460800 write_flash 0x10000 3.7einkDvb.bin
   ```

3. 如果这次改动了分区表或 bootloader，则必须整片烧录（会擦除 NVS 与图片缓存）：

   ```bash
   esptool.py --chip esp32c5 -p COM5 -b 460800 erase_flash
   esptool.py --chip esp32c5 -p COM5 -b 460800 write_flash 0x0 3.7einkDvb_factory_0x0.bin
   ```

4. 没有电脑时，可把 `.bin` 放到 TF 卡带到现场，但最终仍需 USB 连接主机执行 `esptool.py`；ESP32-C5 进入下载模式的办法是按住 BOOT（GPIO28）再上电/复位。

- 需要改动：无（仅发布流程文档）。

## 4. 推荐

- **现在：路线 3。** 4 MB 里已有 1 MiB 图片缓存和 2.16 MiB 应用，双 bank 需要先做一轮有明显功能取舍的瘦身，收益与风险不匹配；手动烧录流程简单、可回退。
- **将来：路线 1。** 当应用能稳定压到 1.6 MiB 以内（网页资源压缩 + 证书裁剪优先），再切双 bank OTA，同时接受图片槽位 3→2。路线 2 只在“必须保留 3 槽且一定要远程升级”时考虑，复杂度与变砖风险最高。

## 5. 改动清单速查

| 路线 | 分区表 | 代码 | 风险 |
| :--- | :--- | :--- | :--- |
| 1 双 bank | `ota_0/ota_1` 各 0x1A0000，`image_cache` 0xB0000 | `screen_rotation.c` 槽数、`esp_https_ota`、回滚标志 | 应用必须 ≤1.625 MiB；升级中断可由另一 bank 恢复 |
| 2 TF 刷写 | `otadata` + `ota_0` 0x2A0000 + `ota_1` 0x40000 | 第二目标、下载校验、刷写与断电恢复 | 无 A/B；刷写中断电需 USB 救砖 |
| 3 手动 | 不变 | 无 | 需电脑与 USB；现场升级不便 |
