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
| 显示 | 传图、刷图、清屏与恢复屏幕；可保存 3 张图片，出厂内置三张示例图；信息页优先（日期、农历/节气、天气与 4 天预报、倒计时、便签，醒着时状态行显示 IP）；BOOT 键切换图片，电源键单击全屏刷白 / 恢复 |
| 刷图模式 | 六色抖动、黑白抖动 |
| 网络 | 热点（AP，默认 WPA2，设备唯一密码）配网与连接路由器（STA），支持 2.4 GHz / 5 GHz 双频 Wi-Fi 6；STA 模式广播 mDNS `hueleaf-xxxx.local` 与 `_http._tcp`；可选 API token 保护所有 POST 接口 |
| 充电 | AXP2101 电量计及电池容量自学习；可设定电池标称容量，按 0.2C / 0.5C / 1C 调整充电电流，最大 1000 mA |
| 开关机 | 按住电源键 0.5 秒开机，按住 4 秒关机 |
| 低功耗 | 无网络活动 2 分钟后关闭热点、断开放射，电池供电时进入深睡 + 定时唤醒（默认 60 分钟；插 USB 时保持浅睡）；待机绘制信息页，定时唤醒只在信息页指纹变化时刷新；电源键唤醒重开热点；定时唤醒关闭后仍为 30 分钟自动关机，另加 5% 电量保护 |
| TF 卡 | FAT32 卡挂载到 `/sdcard`；网页「图片」页的「TF 卡内容库」可从 `/sdcard/eink` 导入 172800 字节裸六色帧到任一槽位，也可把槽位导出到卡；维护唤醒把 UTC 时间、电量、USB 状态和唤醒原因追加到 `/sdcard/log/power.csv`；进入低功耗前自动卸载以省电，网页再次访问 TF 接口时按需挂载 |
| 加速度计 | MSA321：唤醒时约 15 Hz 采样，翻面或双击切换下一张缓存图片；`/api/imu` 提供实时状态；拿起/翻动可唤醒深睡（GPIO4，阈值可调） |
| RTC | PCF8563 掉电保时；开机用它设置系统时钟，联网后 SNTP 校时并写回，深睡和换电池期间时间不丢 |

> [!NOTE]
> 48 / 64 / 256 / 384 / 512 色全局刷新（约 23 秒）、局部刷新（约 23 秒）、快速刷新（约 3 秒）、局部快速刷新（约 2 秒）等功能暂未开放，将在后续版本中更新。固件内置局部窗口刷新探针：网页「设置」页的「屏幕诊断」可一键运行，也可直接 `POST /api/panel/probe`，结果从 `GET /api/panel/probe` 读取，详见 docs/panel_partial_probe.md。RTC 的更多应用功能也将在后续版本中更新。

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

> [!NOTE]
> 配网热点默认启用 WPA2，烧录后首次配网请按屏幕显示的密码连接（见下文「网络与安全」）。本项目**不做 OTA**，升级方式是 TF 卡 + `esptool.py` 手动烧录；4 MB 的空间账见 [docs/ota.md](docs/ota.md)。

#### 配网

1. 首次上电，设备打开配网热点，屏幕显示 **ready to setup / WiFi AP WPA2**、热点名、密码和配网地址。
2. 手机连接设备热点 **`Eink E6 3.7`**，密码为屏幕上的 `PASS hueleaf-xxxx`（5 GHz，手机需支持 5 GHz）。
3. 在浏览器打开 `http://192.168.4.1`。
4. 在「网络」中点击「扫描 Wi-Fi」，选择网络，填写密码后连接。
5. 连接成功后热点自动关闭，屏幕刷新为 **Scan to upload** 二维码。

Wi-Fi 信息会保存在设备中，下次开机自动连接；12 秒内未连上则重新打开配网热点。

热点不会一直开着：**没有客户端连接，或最后一次网页请求结束 2 分钟后，热点自动关闭、设备进入低功耗**（见下文「低功耗」）。电池供电时会进入深睡，由定时器每隔一段时间唤醒校时；按一下电源键即可唤醒并重新打开热点（若电源键不能作为深睡唤醒源，按键会被锁存，在下次定时唤醒时生效）。手机仍连接着热点时不会进入低功耗，断开手机连接后重新开始 2 分钟计时。

> [!NOTE]
> 示例中 Wi-Fi 国家码固定为 `CN`，配网热点使用 5 GHz 36 信道。在其他国家或地区使用时，请按当地无线电法规修改 `examples/3.7einkDvb/main/wifi_mgr/wifi_mgr.c` 中的国家码，以及 `wifi_mgr.h` 中的 `WIFI_AP_CHANNEL`。

#### 网络与安全

**mDNS 名字。** 连上路由器（STA）后，设备以 `hueleaf-xxxx.local` 广播 `_http._tcp`（端口 80，TXT `path=/`），其中 `xxxx` 是 STA MAC 后两字节的小写十六进制；配网页「网络」标签会显示 `http://hueleaf-xxxx.local`，可直接代替 IP 访问。设备还会尝试注册别名 `hueleaf.local`，若局域网内已有其它设备占用则只保留带后缀的名字并在串口记日志。AP 模式下仍使用 `http://192.168.4.1`。mDNS 跟随 Wi-Fi 生命周期启停：开机联网后启动、进入低功耗关闭、恢复网络后重启；定时维护唤醒只连 STA，不启动 mDNS。

**热点密码。** 配网热点默认使用 WPA2，设备唯一密码为 `hueleaf-` + STA MAC 后 4 位十六进制（小写，共 12 字符），例如 `hueleaf-1a2b`。首次启动生成并保存在 NVS（命名空间 `netcfg`，键 `ap_pass`）。屏幕的配网页和网页「网络」标签都会显示当前 SSID 与密码；网页「设置」页可改密码（8–63 个可打印 ASCII，不含引号和反斜杠），保存后热点立即用新密码重启，手机需要重新连接，屏幕也会同步刷新显示新密码。

**忘记密码怎么办。** 上电前按住 BOOT 键（GPIO28）并保持 5 秒，设备会清除 AP 密码（恢复上面的默认值）、清除 API token、清除已保存的 STA 凭据，并在串口和屏幕上明确提示。正常运行中的 BOOT 短按仍是切换图片，不受影响。这是最后的安全阀。

**API token（可选）。** 默认没有 token，API 行为与旧固件完全一致。在「设置」页填入 8–64 个可打印 ASCII 并保存后，**所有 `POST /api/*` 都必须携带 token**，形式为请求头 `X-Auth-Token: <token>` 或查询参数 `?token=<token>`；缺失或错误返回 `401` 和 `{"ok":false,"error":"unauthorized"}`。`GET` 接口（页面加载、状态轮询、`GET /api/tf` 列表）不强制，避免把浏览器锁死。网页把 token 存在浏览器 localStorage，所有修改请求自动带上；收到 401 时页面顶部会出现输入框，填对即可继续。

token 以明文保存在 NVS，作用是**局域网内防误操作**（例如别人的手机乱点「清屏」「改配置」），不是强安全边界；忘记 token 同样用 BOOT 长按 5 秒清除。密码与 token 只允许可打印 ASCII，不会写入日志。

#### 网页功能

手机切回刚才配置的 Wi-Fi，扫描屏幕上的 **Scan to upload** 二维码进入网页。网页分为四个标签页：

| 标签页 | 功能 |
| :--- | :--- |
| 网络 | 显示连接状态与设备地址，扫描并连接 Wi-Fi |
| 文本 | 输入文字，选择背景色、字色和字号，发送到屏幕 |
| 图片 | 选择图片（可拖入或粘贴），调整后以六色抖动发送到屏幕；可选择替换三个存储槽位中的哪一个（并预览当前槽位内容）；可清空屏幕；「TF 卡内容库」可从卡内 `/eink` 导入裸帧到槽位，或把槽位导出到卡 |
| 设置 | 填写电池额定容量（mAh），选择充电档位、低功耗策略、信息页便签 / 倒计时与天气坐标（Open-Meteo），并可立即刷新信息页或立即获取天气 |

图片标签页中可调整：

- **取景与旋转**：自动裁剪为屏幕比例，可缩放、水平 / 垂直移动取景，或左右旋转 90°
- **六色混色与风格**：抖动、降噪，可勾选「查看抖动像素」预览效果
- **明暗与颜色**：曝光、亮度、对比度、高光、阴影、饱和度
- **区域替换与重复发送**：只替换画面中的一块区域

图片在浏览器中处理，发送时只上传六色点阵；屏幕整屏刷新。

##### TF 卡内容库接口

设备把 `image_cache` 槽位格式当作内容交换格式：480×720、每字节 2 个纵向像素、按 gate 排列（gate = 479 − x），共 172800 字节，高半字节为偶数 y，墨色 id 为 0/1/2/3/5/6（黑/白/黄/红/蓝/绿）。**只接受预抖动好的裸六色帧**；普通照片仍由网页在浏览器里抖动、转换成该格式后再上传。卡内文件放在 `/sdcard/eink/`，按文件名排序，最多列出 64 个。

- `GET /api/tf` 返回 `{"ok":true,"present":true,"mounted":true,"files":[{"name":"a.bin","size":172800,"valid":true}]}`。`present` / `mounted` 表示卡是否存在并已挂载；大小不是 172800 的文件 `valid:false`，页面标记为不能导入。卡不在时返回 `present:false`、`mounted:false` 和空列表。
- `POST /api/tf/import` 提交 `{"file":"a.bin","slot":0}`：从卡读取该帧写入槽位并整屏刷新。请求立即返回 `202 Accepted` 和 `job`（`kind:"tf-import"`），前端轮询 `/api/job` 等待完成；文件不存在或大小不对返回 400，屏幕正忙返回 409，卡不在返回 `{"ok":false,"error":"no card"}`。
- `POST /api/tf/export` 提交 `{"slot":0,"name":"backup1.bin"}`：把槽位裸帧写到 `/sdcard/eink/<name>`，同样返回 202 和 `job`（`kind:"tf-export"`）。文件名只允许字母、数字、下划线、点和短横线，`/`、`\`、`..` 会被拒绝；槽位为空返回 400。
- 导入与导出都在既有作业任务里持 rotation 锁执行，HTTP 处理函数不做 23 秒刷新或大文件循环。

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
| BOOT 键（左上角） | 正常/浅睡状态：切换到下一张已保存的图片（正常状态画面底部显示当前 IP 小字）并重新开始 2 分钟 Wi-Fi 计时；深睡与白屏状态下不改变画面 |
| BOOT 键（上电时按住 5 秒） | 恢复出厂网络设置：AP 密码回到默认值、清除 API token、清除 STA 凭据，串口和屏幕给出提示 |

默认屏幕方向由 `examples/3.7einkDvb/main/pin_map.h` 中的 `EINK_ROTATION` 设置（可选 `0`、`90`、`180`、`270`，默认 `0`），运行中不再通过按键旋转。按键引脚宏为 `EINK_PICTURE_BUTTON`（GPIO28，旧名 `EINK_ROTATE_BUTTON` 仍可用）。

#### 低功耗

电池供电时，Wi-Fi 和屏幕刷新是主要耗电来源，因此固件按下面的规则自动省电（实现见 `examples/3.7einkDvb/main/power_mgr/`）：

| 状态 | 触发条件 | 设备行为 |
| :--- | :--- | :--- |
| 工作 | 有热点客户端连接，或 2 分钟内有网页请求、屏幕正在刷新 | 热点保持开启，正常提供网页服务 |
| 低功耗（深睡） | 无客户端连接，且连续 2 分钟没有网络活动，按当前策略选择深睡 | 关闭热点、关闭射频，屏幕去掉 IP 小字并绘制信息页（三张图片保持不变；未启用信息页时退回下一张图片，没有图片则显示 **LOW POWER / Press POWER key**），随后 MCU 进入深度睡眠，由 RTC 定时器按「唤醒间隔」唤醒（整机待机电流以实测为准，面板和 PMIC 仍在供电） |
| 低功耗（浅睡） | 同上，但插着 USB 且策略为「自动」，或策略为「仅浅睡」 | 与旧版一致：MCU 轻睡眠，每 500 ms 轮询电源键，网页和 USB 控制台保持可用 |
| 定时维护 | 深睡被定时器唤醒 | 不开热点，只用已保存的 STA 凭据联网（10 秒上限）校时并写回 PCF8563、记录电量、抓取 Open-Meteo 天气（8 秒超时，失败保留旧缓存）；信息页指纹变化时整屏刷新一次（约 23 秒），相同则完全不碰屏；不加 IP 小字；并尝试向 TF 卡 `/sdcard/log/power.csv` 追加一行（挂载+写入+卸载 3 秒预算，卡不在则跳过）；整个会话 120 秒硬上限，然后继续深睡 |
| 关机 | 定时唤醒关闭时，低功耗持续 30 分钟且未接 USB；或未接 USB 且电量有效低于 5% | 通过 AXP2101 软件关机（等效按住电源键 4 秒），按住电源键 0.5 秒可重新开机 |

- 任意网页请求（含 `index.html`、`/api/status` 轮询、图片上传）都会重置 2 分钟计时；手机连着热点时不会进入低功耗。
- 深睡中按一下电源键：ESP32-C5 只有 LP 域 GPIO 能作为深睡唤醒源，PMIC nIRQ（GPIO25）是否在 LP 域由启动时的 `esp_sleep_is_valid_wakeup_gpio()` 自检决定。可用时按键直接唤醒；不可用时按键事件由 AXP2101 锁存，在下一次定时唤醒时被发现，并把这次唤醒升级为完整的工作会话（日志与 `/api/power` 的 `deep_ok` 会说明当前是哪一种）。
- 唤醒后重新打开配网热点（已配网则先尝试连接路由器），并在 2 分钟后重新开始计时。
- 运动唤醒：加速度计 INT1 接 GPIO4（C5 的 LP 域，合法深睡唤醒源），配置为 any-motion（active）中断——三轴使能（0x16 bit2:0）、映射到 INT1（0x19 bit2）、推挽高有效（0x20 = 0x01，对应 `ESP_GPIO_WAKEUP_GPIO_HIGH`）、锁存（0x21 的 LATCH_INT = 0111）。`power_mgr_deep_sleep()` 在进入深睡前先调用 `imu_app_prepare_wake()` 配置中断，再写 `RESET_INT`（0x21 = 0x87）清掉已锁存的事件，最后 arm 高电平唤醒；顺序不能颠倒，否则刚睡下就会被旧事件唤醒。`ESP_SLEEP_WAKEUP_GPIO` 且唤醒掩码含 GPIO4 时日志记为 `motion`，按交互会话处理（重开热点、加 IP 小字、重新 2 分钟计时）；唤醒后由开机流程的 `imu_app_clear_wake()` 再清一次锁存。阈值和时长是 `msa321.h` 顶部的两个宏：`MSA321_ACTIVE_TH`（默认 `0x14` = 156 mg，4 g 量程下 1 LSB = 7.81 mg）和 `MSA321_ACTIVE_DUR`（默认 `1` = 2 ms，实际时长 = 值 + 1）；桌面震动误唤醒就把 TH 调到 `0x20`~`0x30`，太钝叫不醒就降到 `0x0C`。IMU 缺失、任一寄存器读回不一致或清除失败时自动回落定时器/电源键，不会因 IMU 错误睡死。
- 运动唤醒是即时唤醒路径，这一点和电源键不同：GPIO25（PMIC nIRQ）不在 LP 域，深睡中按电源键仍靠 AXP2101 锁存事件、在下一次唤醒时才生效；拿起设备的运动中断则会立刻把 MCU 唤醒。
- **IP 显示就是工作状态指示**：醒着时，图片底部会叠加白底黑字的小字（如 `AP 192.168.4.1`），信息页则在底部状态行里显示 IP；进入低功耗后两者都没有 IP，所以看不到 IP 就表示设备已进入低功耗。定时维护唤醒不加 IP。
- 图片保存在 `image_cache` 分区（3 个槽位，掉电不丢）。BOOT 键和翻面/双击手势按槽位顺序切换下一张，待机不再自动轮播；网页上传时可指定替换哪个槽位，页面上能看到三个槽位的缩略图。定时唤醒不换图。
- 固件内置三张示例图（黄昏风景、六色海报、测试卡，源码见 `examples/3.7einkDvb/demo_images/`）。首次上电或缓存为空时，开机自动写入三个槽位（约 1 秒），所以开箱即可看到信息页（未启用信息页时是「图片 + IP 小字」），不再是配网文字页；上传后会逐一替换掉示例图。清空缓存后，未启用信息页时低功耗仍显示 **LOW POWER / Press POWER key**。
- 进入低功耗（深睡或浅睡）前会卸载 TF 卡并释放片选 / 上拉，避免睡眠期间 SD 卡和 SPI 上拉持续耗电；网页再次访问 `/api/tf`、`/api/tf/import`、`/api/tf/export` 时自动重新挂载。
- 低功耗、深睡和自动关机期间墨水屏画面保持不变，内容不会丢失；网页需要重新连接热点后刷新。
- 时间保持：PCF8563 由独立电池供电，跨深睡和掉电继续走时。开机读取 RTC（年份 2024–2100 才采用）灌入系统时钟，SNTP 校时成功后写回 RTC；系统时间只允许向前跳。
- **自动关机规则已调整**：启用定时唤醒时不再执行「低功耗 30 分钟自动关机」，否则定时刷新会被关掉；改为电量保护（未接 USB、电量有效且低于 5% 时关机，充电中不触发）。把唤醒间隔设为「关闭」即恢复旧的 30 分钟关机规则。
- 充电（USB 供电）时「自动」策略只关闭热点进入浅睡，不深睡也不执行自动关机，方便网页和控制台调试；「强制深睡」会无视 USB 直接深睡。

低功耗策略可在网页「设置」标签页修改，也可直接调用接口：

- `GET /api/power` 返回 `mode`、`interval_min`、`deep_ok`（电源键所在 GPIO25 能否作为深睡唤醒源）、`wake_pins`（25/4/28/23 四个引脚的自检结果）、`last_wake`（timer / timer+key / gpio / motion / poweron / reset）、`usb`、`batt`（SOC，无效为 -1）、`sleep_state`（当前状态 active / low / maint / off）。
- `GET /api/imu` 返回加速度计运行态：`ok`、`present`、`x/y/z`（g）、`orientation`（portrait / landscape / faceup / facedown）、`events.flip` / `events.tap`、`int_ready`、`wake_armed`。
- `GET /api/content` 返回信息页配置与最近绘制状态；`POST /api/content` 修改便签 / 倒计时 / 启用开关；`POST /api/content/refresh` 立即重画（作业系统，约 23 秒）。字段见下文「信息页」。
- `GET /api/weather` 返回天气配置与缓存；`POST /api/weather` 修改经纬度 / 城市标签 / 启用开关；`POST /api/weather/refresh` 立即抓取并重画（作业系统，约 30 秒）。字段与失败策略见下文「天气」。
- `POST /api/power` 提交 `{"mode":"auto|light|deep","interval_min":60}`（`0` 关闭定时唤醒，上限 10080 分钟），保存到 NVS 命名空间 `pwrmgr`，下一次进入低功耗时生效。

相关参数都在 `examples/3.7einkDvb/main/power_mgr/power_mgr.h` 中，可按需修改：

| 宏 | 默认值 | 说明 |
| :--- | :--- | :--- |
| `POWER_IDLE_TIMEOUT_MS` | `2 * 60 * 1000` | 无网络活动的超时时间 |
| `POWER_SLEEP_POLL_MS` | `500` | 浅睡轮询电源键的周期，越小唤醒越快、平均功耗略高 |
| `POWER_WAKE_INTERVAL_MIN` | `60` | 深睡定时唤醒间隔（分钟）的首次开机默认值，`0` 关闭；之后以 NVS 中的值为准 |
| `POWER_AUTO_OFF_MS` | `30 * 60 * 1000` | 仅当定时唤醒关闭时的自动关机时间，设为 `0` 关闭自动关机 |
| `POWER_BATT_CUTOFF_PCT` | `5` | 电量有效且低于该百分比时保护关机 |
| `POWER_MAINT_TOTAL_MS` | `120000` | 定时维护会话硬上限；包含 8 秒天气抓取与一次约 23 秒的信息页整屏刷新 |

#### 信息页（待机页）

信息页是默认主画面：醒着时显示带 IP 的状态行，无网络活动进入低功耗后去掉 IP 再绘制一版，不再轮播用户图片。三张图片仍完整保存在 `image_cache`，BOOT 键或翻面手势随时切回；下一次进入低功耗又回到信息页。内容为：

- 顶部：本地日期，例如 `FRI 27 NOV 2026`（`CST-8`，无夏令时）
- 时间行：`UPDATED 18:56`，即最近一次实际刷新时的本地时间（23 秒整屏刷新无法做走秒时钟，指纹也不会只为分钟变化而重画）
- 农历 `LUNAR 10-19`（闰月显示为 `LUNAR L6-12`）与当前节气英文名，例如 `MINOR SNOW`
- 天气矩形（红框）：主行 `18C CLEAR`，明细行 `H 22 L 11 OBS 14:32`，预报行 `TOD 22/11 WED 20/11 THU 19/8 FRI 21/10`；没有缓存时退回单行 `WEATHER --` 占位。数据来自 Open-Meteo（免 API key，需要联网），配置与缓存保存在 NVS 命名空间 `weather`，掉电和深睡后仍显示上次结果
- 倒计时：`DAYS LEFT 42` 与最多 16 字符的标签；日期已过则显示 `DAYS AGO`
- 两行便签，每行最多 32 个可打印 ASCII 字符
- 底部状态：电量、USB、醒着时的 IP、下次唤醒间隔

农历由内置的 1900–2100 每年 32 位压缩表（低 4 位闰月、bit16 闰月大小、其余 12 位大小月）计算，基准是 1900-01-31 = 农历 1900-01-01；越界年份显示 `LUNAR --`，不会崩溃。节气按 Meeus 太阳视黄经公式求当前所处的 15° 区间（UTC+8 当地日），输出英文名。`main/content/lunar.c` 的 host 自检见 `tools/lunar_selftest.c`。

刷新策略：日期、农历、节气、倒计时天数、便签和天气三行文本一起算 FNV-1a 指纹，成功刷新后写入 NVS 命名空间 `content`。定时维护唤醒只在指纹变化时刷新，相同则完全不碰屏；进入低功耗、手动刷新、以及信息页被电源键刷白后恢复都会强制重画一次。一次整屏六色刷新约 23 秒，所以维护会话预算从 45 秒提高到 120 秒。

接口：

- `GET /api/content` 返回 `{"ok":true,"enabled":true,"note":["...",""],"countdown_date":"2026-12-31","countdown_label":"TRIP","last_draw":1764...,"last_fingerprint":"1a2b3c4d","refreshed":false}`。`last_draw` 是 Unix 秒，`refreshed` 表示最近一次显示调用是否真的刷了屏。
- `POST /api/content` 提交 `enabled`、`note`（最多 2 个、每个 32 个 ASCII）、`countdown_date`（`YYYY-MM-DD` 或空）、`countdown_label`（最多 16 个 ASCII）；字段可省略，省略即保留原值。
- `POST /api/content/refresh` 空请求体，立即重画，走作业系统返回 `202` 和 `job`（`kind:"content"`），前端轮询 `/api/job` 等待约 23 秒的刷新完成。
- `GET /api/weather` 返回天气配置与缓存；`POST /api/weather` 保存经纬度 / 标签 / 启用开关；`POST /api/weather/refresh` 抓取并重画（作业系统，`kind:"weather"`，约 30 秒）。字段与抓取策略见下文「天气」。

#### 天气（Open-Meteo）

- 数据源固定为 Open-Meteo `https://api.open-meteo.com/v1/forecast`，无需 API key。请求只包含配置的经纬度、固定的字段列表和 `Asia/Shanghai` 时区参数；**不发送设备标识、SSID、账号或任何其他信息**。
- 抓取时机：定时维护唤醒在 SNTP 校时之后、绘制信息页之前抓取一次（8 秒超时，取剩余预算的较小值）；交互会话开机不抓取，只在网页点「立即获取天气」时通过作业系统抓取并重画。`enabled` 只控制抓取：关闭后维护唤醒完全跳过网络请求，屏上继续显示上一次成功缓存。
- 失败策略：断网、DNS、TLS、HTTP 非 200、响应过大或 JSON 解析失败都只记录 `last_error`，保留上一次成功缓存和屏上内容，不做重试循环，也不影响按时深睡。
- 屏上三行（框线仍为红色）：主行 `18C CLEAR`；明细行 `H 22 L 11 OBS 14:32`，`OBS` 是最近一次成功抓取的本地时间，时钟未知时显示 `OBS --`；预报行从 `TOD`（今天）开始最多 4 天。预报行在框内可用宽度（480 宽屏时约 380 像素）内绘制，放不下 4 天时按整词从末尾删减（通常显示 3 天），仍超宽才继续截断，不会溢出边框。城市标签只用于设置页和接口标识，不占屏上宽度。
- WMO 天气码映射为不超过 10 个字符的英文：`CLEAR / PARTLY / CLOUDY / OVERCAST / FOG / DRIZZLE / RAIN / SHOWERS / SNOW / SLEET / STORM`，未知码为 `N/A`。
- 缓存：当前温度（十分之一度）与天气码、今日高低温、未来 4 天的星期 / 高低温 / 天气码、观测时间（Unix 秒；系统时间早于 2020 年时记 0 表示未知）、最近错误与时间。配置与缓存都在 NVS 命名空间 `weather`。
- `GET /api/weather` 返回 `{"ok":true,"enabled":true,"lat":39.9042,"lon":116.4074,"label":"BEIJING","has_data":true,"code":1,"text":"PARTLY","today_max":22,"today_min":11,"temp_c":18,"temp_x10":186,"forecast":[{"day":"TOD","weekday":"THU","code":1,"text":"PARTLY","max":22,"min":11}],"observed_at":1764,"age_s":300,"last_error":"","last_error_at":0}`；`age_s` 为数据年龄秒数（时间未知为 `-1`），`last_error` 为最近失败的简短错误码（如 `http 404`、`net error`、`parse`、`body too big`）。
- `POST /api/weather` 提交 `{"lat":39.9,"lon":116.4,"label":"BEIJING","enabled":true}`；纬度 -90..90、经度 -180..180、标签最多 16 个 ASCII，字段可省略（省略即保留原值）。坐标以 ×10000 的整数存入 NVS。

## 许可证

- 代码（`examples/`）：[Apache-2.0](LICENSE)
- 硬件资料与文档（`hardware/`、`docs/`）：[CC BY-NC-SA 4.0](LICENSE-CC-BY-NC-SA-4.0)
- 第三方组件（如 Vue）沿用其原有许可证

<img src="docs/images/divider.svg" alt="" width="100%" height="6">

<p align="center">
  <b>O S P T E K</b> &nbsp;·&nbsp; <code>HUELEAF-BASE</code>
</p>
