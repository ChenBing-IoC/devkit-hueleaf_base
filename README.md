# 彩叶（基础版） HueLeaf Base

OSPTEK 墨水屏开发套件 · ESP32-C5 主控 · 3.7″ 六色墨水屏 · 双频 Wi-Fi 6

<p align="center">
  <img src="docs/images/banner.png" alt="OSPTEK 彩叶 HueLeaf 墨水屏开发套件 基础版" width="100%">
  <img src="docs/images/overview.jpg" >
  <img src="docs/images/button.jpg" >
</p>

<p align="center">
  <img src="https://img.shields.io/badge/MCU-ESP32--C5-1E1E1E?style=for-the-badge" alt="MCU">
  <img src="https://img.shields.io/badge/PMU-AXP2101-D93A2B?style=for-the-badge" alt="PMU">
  <img src="https://img.shields.io/badge/E--Paper-3.7%22%206--Color-F0BE32?style=for-the-badge" alt="Display">
  <img src="https://img.shields.io/badge/Wi--Fi%206-2.4%20%2F%205%20GHz-2058C4?style=for-the-badge" alt="Wi-Fi">
  <img src="https://img.shields.io/badge/RTC-PCF8563-2E8B4A?style=for-the-badge" alt="RTC">
</p>

<p align="center">
  <b>简体中文</b> &nbsp;|&nbsp; <a href="README_EN.md">English</a>
</p>

<img src="docs/images/divider.svg" alt="" width="100%" height="6">

## 简介

彩叶 HueLeaf 是 OSPTEK 推出的墨水屏开发套件，本仓库对应其基础版。

基础版以 ESP32-C5 为主控，搭载一块 3.7 英寸六色墨水屏，可显示黑、白、红、黄、蓝、绿六种颜色。板上集成 AXP2101 电源管理芯片、PCF8563 RTC 时钟、MSA321 加速度计、4 MB Flash 和 TF 卡槽，ESP32-C5 提供 2.4 GHz / 5 GHz 双频 Wi-Fi 6 连接。基础版内置 500maH 3.7V标压锂电池，带NTC检测，深度睡眠预计可待机30天以上

整机采用挂牌式外壳，可以挂在胸前当电子工牌，也可以放在桌面上使用。墨水屏只在刷新画面时耗电，配合 AXP2101 的锂电池管理和 PCF8563 的定时唤醒，设备可以长时间休眠、按需刷新，适合用电池长期供电。

目前发布的外壳为3D打印版本，厚度约8.5mm。您可以在这个链接内找到我们公开发布的外壳：https://makerworld.com.cn/zh/models/3128516-yu-ying-guang-dian-esp32c5xin-pian-3-7ying-cun-mo#profileId-3685850

本仓库面向二次开发，提供基于 ESP-IDF 的示例代码、出厂固件和硬件资料。

### 典型用途

- 电子工牌、会议胸牌
- 桌面日历、天气时钟
- 信息看板、消息提醒
- 电子价签、门牌标识

## 硬件规格

| 项目 | 参数 |
| :--- | :--- |
| 主控 | ESP32-C5（ESP32-C5HF4） |
| 显示屏 | 3.7″ 六色墨水屏 EPD037A02，480 × 720（黑、白、红、黄、蓝、绿） |
| 无线 | 双频 Wi-Fi 6，2.4 / 5 GHz |
| 电源管理 | AXP2101 |
| RTC 时钟 | PCF8563 |
| 加速度计 | MSA321 |
| 存储 | 4 MB Flash，TF 卡 |
| USB | Type-C，ESP32-C5 原生 USB |
| 按键 | 复位键、电源键、BOOT 键 |
| 指示灯 | 充电指示（蓝）、开机指示（绿） |

完整原理图见 [hardware/HueLeaf_Base_Schematic_V1.0.pdf](hardware/HueLeaf_Base_Schematic_V1.0.pdf)，元件位置见位号图 [hardware/HueLeaf_Base_Placement.pdf](hardware/HueLeaf_Base_Placement.pdf)。

### 引脚分配

| 功能 | GPIO | 说明 |
| :--- | :--- | :--- |
| SPI MOSI | 0 | 墨水屏与 TF 卡共用 |
| SPI CLK | 1 | 墨水屏与 TF 卡共用 |
| SPI MISO | 9 | TF 卡 |
| 墨水屏 CS / DC / RST / BUSY | 5 / 6 / 7 / 8 | |
| TF 卡 CS | 10 | |
| I2C SDA / SCL | 2 / 3 | AXP2101、PCF8563、MSA321 共用 |
| 加速度计中断 | 4 | MSA321 |
| RTC 中断 | 23 | PCF8563 |
| PMIC 中断 | 25 | AXP2101 |
| BOOT 键 | 28 | 低电平有效 |

| I2C 设备 | 地址 |
| :--- | :--- |
| AXP2101 | `0x34` |
| PCF8563 | `0x51` |
| MSA321 | `0x62` |

## 仓库内容

| 目录 | 内容 |
| :--- | :--- |
| [`examples/`](examples) | 示例代码与出厂固件 |
| [`hardware/`](hardware) | 原理图、位号图 |

## 开发环境

示例代码基于 [ESP-IDF](https://github.com/espressif/esp-idf) 开发。

## 示例

### 3.7einkDvb 网页传图

用手机或电脑打开网页，把图片或文字发送到墨水屏上显示。代码位于 [`examples/3.7einkDvb`](examples/3.7einkDvb)。

<p align="center">
  <img src="docs/images/device_photo.jpg" alt="实物显示效果" width="360">
</p>

#### 出厂固件功能

出厂固件由本示例编译而来，支持以下功能：

| 类别 | 功能 |
| :--- | :--- |
| 显示 | 传图、刷图、清屏与恢复屏幕；可保存 3 张图片循环显示，出厂内置三张示例图，页面底部白条显示设备 IP；BOOT 键切换下一张图片，电源键单击全屏刷白 / 恢复 |
| 刷图模式 | 六色抖动、黑白抖动 |
| 网络 | 热点（AP）配网与连接路由器（STA），支持 2.4 GHz / 5 GHz 双频 Wi-Fi 6 |
| 充电 | AXP2101 电量计及电池容量自学习；可设定电池标称容量，按 0.2C / 0.5C / 1C 调整充电电流，最大 1000 mA |
| 开关机 | 按住电源键 0.5 秒开机，按住 4 秒关机 |
| 低功耗 | 无网络活动 2 分钟后关闭热点、断开放射，电池供电时进入深睡 + 定时唤醒（默认 60 分钟；插 USB 时保持浅睡）；定时唤醒只校时、不刷屏；电源键唤醒重开热点；定时唤醒关闭后仍为 30 分钟自动关机，另加 5% 电量保护 |
| TF 卡 | 驱动已内置，支持初始化 |
| 加速度计 | MSA321 驱动已内置，支持初始化与读数 |
| RTC | PCF8563 驱动已内置，支持初始化与读写 |

> [!NOTE]
> 48 / 64 / 256 / 384 / 512 色全局刷新（约 23 秒）、局部刷新（约 23 秒）、快速刷新（约 3 秒）、局部快速刷新（约 2 秒）等功能暂未开放，将在后续版本中更新。TF 卡、加速度计、RTC 的应用功能也将在后续版本中更新。

#### 烧录

**方式一：直接烧录出厂固件**

固件为合并镜像，从 `0x0` 地址烧录：

```bash
esptool.py --chip esp32c5 write_flash 0x0 examples/3.7einkDvb/firmware/3.7einkDvb_factory_0x0.bin
```

**方式二：从源码编译烧录**

```bash
cd examples/3.7einkDvb
idf.py build flash monitor
```

#### 配网

1. 首次上电，设备打开配网热点，屏幕显示 **ready to setup / WiFi AP OPEN** 和配网地址。
2. 手机连接设备热点 **`Eink E6 3.7`**（无密码，5 GHz，手机需支持 5 GHz）。
3. 在浏览器打开 `http://192.168.4.1`。
4. 在「网络」中点击「扫描 Wi-Fi」，选择网络，填写密码后连接。
5. 连接成功后热点自动关闭，屏幕刷新为 **Scan to upload** 二维码。

Wi-Fi 信息会保存在设备中，下次开机自动连接；12 秒内未连上则重新打开配网热点。

热点不会一直开着：**没有客户端连接，或最后一次网页请求结束 2 分钟后，热点自动关闭、设备进入低功耗**（见下文「低功耗」）。电池供电时会进入深睡，由定时器每隔一段时间唤醒校时；按一下电源键即可唤醒并重新打开热点（若电源键不能作为深睡唤醒源，按键会被锁存，在下次定时唤醒时生效）。手机仍连接着热点时不会进入低功耗，断开手机连接后重新开始 2 分钟计时。

> [!NOTE]
> 示例中 Wi-Fi 国家码固定为 `CN`，配网热点使用 5 GHz 36 信道。在其他国家或地区使用时，请按当地无线电法规修改 `examples/3.7einkDvb/main/wifi_mgr/wifi_mgr.c` 中的国家码，以及 `wifi_mgr.h` 中的 `WIFI_AP_CHANNEL`。

#### 网页功能

手机切回刚才配置的 Wi-Fi，扫描屏幕上的 **Scan to upload** 二维码进入网页。网页分为四个标签页：

| 标签页 | 功能 |
| :--- | :--- |
| 网络 | 显示连接状态与设备地址，扫描并连接 Wi-Fi |
| 文本 | 输入文字，选择背景色、字色和字号，发送到屏幕 |
| 图片 | 选择图片（可拖入或粘贴），调整后以六色抖动发送到屏幕；可选择替换三个存储槽位中的哪一个（并预览当前槽位内容）；可清空屏幕 |
| 设置 | 填写电池额定容量（mAh），选择涓流 / 正常 / 快速充电档位 |

图片标签页中可调整：

- **取景与旋转**：自动裁剪为屏幕比例，可缩放、水平 / 垂直移动取景，或左右旋转 90°
- **六色混色与风格**：抖动、降噪，可勾选「查看抖动像素」预览效果
- **明暗与颜色**：曝光、亮度、对比度、高光、阴影、饱和度
- **区域替换与重复发送**：只替换画面中的一块区域

图片在浏览器中处理，发送时只上传六色点阵；屏幕整屏刷新。

| 网络 | 文本 |
| :---: | :---: |
| <img src="docs/images/web_network.png" alt="网络" width="400"> | <img src="docs/images/web_text.png" alt="文本" width="400"> |
| **图片** | **设置** |
| <img src="docs/images/web_image_loaded.png" alt="图片" width="400"> | <img src="docs/images/web_settings.png" alt="设置" width="400"> |

#### 按键

| 按键 | 功能 |
| :--- | :--- |
| 电源键（按住 0.5 秒） | 开机 |
| 电源键（按住 4 秒） | 关机（AXP2101 硬件关机，任意状态下均有效） |
| 电源键（单击） | 正常状态：屏幕在白屏和当前画面之间切换，画面内容保留；低功耗状态：唤醒并重新打开热点 |
| BOOT 键（左上角） | 正常状态：切换到下一张已保存的图片，画面底部显示当前 IP 小字，并重新开始 2 分钟 Wi-Fi 计时；低功耗/白屏状态下不改变画面 |

默认屏幕方向由 `examples/3.7einkDvb/main/pin_map.h` 中的 `EINK_ROTATION` 设置（可选 `0`、`90`、`180`、`270`，默认 `0`），运行中不再通过按键旋转。按键引脚宏为 `EINK_PICTURE_BUTTON`（GPIO28，旧名 `EINK_ROTATE_BUTTON` 仍可用）。

#### 低功耗

电池供电时，Wi-Fi 和屏幕刷新是主要耗电来源，因此固件按下面的规则自动省电（实现见 `examples/3.7einkDvb/main/power_mgr/`）：

| 状态 | 触发条件 | 设备行为 |
| :--- | :--- | :--- |
| 工作 | 有热点客户端连接，或 2 分钟内有网页请求、屏幕正在刷新 | 热点保持开启，正常提供网页服务 |
| 低功耗（深睡） | 无客户端连接，且连续 2 分钟没有网络活动，按当前策略选择深睡 | 关闭热点、关闭射频，屏幕去掉 IP 小字并切换到下一张已保存的图片（没有图片时显示 **LOW POWER / Press POWER key**），随后 MCU 进入深度睡眠，由 RTC 定时器按「唤醒间隔」唤醒（整机待机电流以实测为准，面板和 PMIC 仍在供电） |
| 低功耗（浅睡） | 同上，但插着 USB 且策略为「自动」，或策略为「仅浅睡」 | 与旧版一致：MCU 轻睡眠，每 500 ms 轮询电源键，网页和 USB 控制台保持可用 |
| 定时维护 | 深睡被定时器唤醒 | 不开热点，只用已保存的 STA 凭据联网（10 秒上限）校时并写回 PCF8563、记录电量，不刷新屏幕也不加 IP 小字；整个会话 45 秒硬上限，然后继续深睡 |
| 关机 | 定时唤醒关闭时，低功耗持续 30 分钟且未接 USB；或未接 USB 且电量有效低于 5% | 通过 AXP2101 软件关机（等效按住电源键 4 秒），按住电源键 0.5 秒可重新开机 |

- 任意网页请求（含 `index.html`、`/api/status` 轮询、图片上传）都会重置 2 分钟计时；手机连着热点时不会进入低功耗。
- 深睡中按一下电源键：ESP32-C5 只有 LP 域 GPIO 能作为深睡唤醒源，PMIC nIRQ（GPIO25）是否在 LP 域由启动时的 `esp_sleep_is_valid_wakeup_gpio()` 自检决定。可用时按键直接唤醒；不可用时按键事件由 AXP2101 锁存，在下一次定时唤醒时被发现，并把这次唤醒升级为完整的工作会话（日志与 `/api/power` 的 `deep_ok` 会说明当前是哪一种）。
- 唤醒后重新打开配网热点（已配网则先尝试连接路由器），并在 2 分钟后重新开始计时。
- **IP 小字就是工作状态指示**：工作状态下，图片底部有一条白底黑字的小字（如 `AP 192.168.4.1`）；进入低功耗时这条小字被去掉并刷新下一张图片，所以看到画面没有 IP 小字就表示设备已进入低功耗，而待机画面本身仍是完整的一张图。定时维护唤醒不碰屏幕，也不会加回小字。
- 图片保存在 `image_cache` 分区（3 个槽位，掉电不丢）。进入低功耗时按槽位顺序切换下一张；网页上传时可指定替换哪个槽位，页面上能看到三个槽位的缩略图。定时唤醒不参与轮播。
- 固件内置三张示例图（黄昏风景、六色海报、测试卡，源码见 `examples/3.7einkDvb/demo_images/`）。首次上电或缓存为空时，开机自动写入三个槽位（约 1 秒），所以开箱即是「图片 + IP 小字」，不再是配网文字页；上传后会逐一替换掉示例图。清空缓存后低功耗仍显示 **LOW POWER / Press POWER key**。
- 低功耗、深睡和自动关机期间墨水屏画面保持不变，内容不会丢失；网页需要重新连接热点后刷新。
- 时间保持：PCF8563 由独立电池供电，跨深睡和掉电继续走时。开机读取 RTC（年份 2024–2100 才采用）灌入系统时钟，SNTP 校时成功后写回 RTC；系统时间只允许向前跳。
- **自动关机规则已调整**：启用定时唤醒时不再执行「低功耗 30 分钟自动关机」，否则定时刷新会被关掉；改为电量保护（未接 USB、电量有效且低于 5% 时关机，充电中不触发）。把唤醒间隔设为「关闭」即恢复旧的 30 分钟关机规则。
- 充电（USB 供电）时「自动」策略只关闭热点进入浅睡，不深睡也不执行自动关机，方便网页和控制台调试；「强制深睡」会无视 USB 直接深睡。

低功耗策略可在网页「设置」标签页修改，也可直接调用接口：

- `GET /api/power` 返回 `mode`、`interval_min`、`deep_ok`（电源键所在 GPIO25 能否作为深睡唤醒源）、`wake_pins`（25/4/28/23 四个引脚的自检结果）、`last_wake`（timer / timer+key / gpio / poweron / reset）、`usb`、`batt`（SOC，无效为 -1）、`sleep_state`（当前状态 active / low / maint / off）。
- `POST /api/power` 提交 `{"mode":"auto|light|deep","interval_min":60}`（`0` 关闭定时唤醒，上限 10080 分钟），保存到 NVS 命名空间 `pwrmgr`，下一次进入低功耗时生效。

相关参数都在 `examples/3.7einkDvb/main/power_mgr/power_mgr.h` 中，可按需修改：

| 宏 | 默认值 | 说明 |
| :--- | :--- | :--- |
| `POWER_IDLE_TIMEOUT_MS` | `2 * 60 * 1000` | 无网络活动的超时时间 |
| `POWER_SLEEP_POLL_MS` | `500` | 浅睡轮询电源键的周期，越小唤醒越快、平均功耗略高 |
| `POWER_WAKE_INTERVAL_MIN` | `60` | 深睡定时唤醒间隔（分钟）的首次开机默认值，`0` 关闭；之后以 NVS 中的值为准 |
| `POWER_AUTO_OFF_MS` | `30 * 60 * 1000` | 仅当定时唤醒关闭时的自动关机时间，设为 `0` 关闭自动关机 |
| `POWER_BATT_CUTOFF_PCT` | `5` | 电量有效且低于该百分比时保护关机 |

## 许可证

- 代码（`examples/`）：[Apache-2.0](LICENSE)
- 硬件资料与文档（`hardware/`、`docs/`）：[CC BY-NC-SA 4.0](LICENSE-CC-BY-NC-SA-4.0)
- 第三方组件（如 Vue）沿用其原有许可证

<img src="docs/images/divider.svg" alt="" width="100%" height="6">

<p align="center">
  <b>O S P T E K</b> &nbsp;·&nbsp; <code>HUELEAF-BASE</code>
</p>
