# HueLeaf ESP32-C5 3.7 寸 E6 墨水屏开发者说明

本文为 HueLeaf 开发者提供上手和二次开发指引，涵盖板载接口、GPIO 分配、屏幕数据格式、构建方式及主要固件模块。引脚号均指 ESP32-C5 GPIO 编号。随附工程可作为功能开发起点，开发者可在此基础上扩展界面、图像处理、联网服务和外设应用。

## 1. 硬件概览

| 项目 | 参数 |
| --- | --- |
| 主控 | ESP32-C5，模组/芯片符号 ESP32-C5HF4 |
| 显示屏 | EPD037A02，3.7 英寸 E6 多色电子纸，480 × 720 |
| 屏幕接口 | 4 线 SPI，含 CS、D/C、复位和 BUSY |
| 屏幕控制器 | 软件板级配置沿用 JD7601 历史命名；面板参数按 EPD037A02 配置 |
| 电源管理 | AXP2101，含电池充电与多路电源输出 |
| TF 卡 | SPI，共享 SPI2 总线 |
| IMU | MSA321，I²C，加速度计 |
| RTC | PCF8563，I²C，32.768 kHz 晶振 |
| USB | USB Type-C，USB D+/D- 经 ESD 保护连接主控 |
| Flash | ESP32-C5HF4 内置 4 MB Flash |
| 电池 | 出货版本内置 500 mAh、3.7 V 标称电压锂电池 |


完整原理图见 https://github.com/osptek/devkit-hueleaf_base/tree/main

## 2. GPIO 与总线连接

### SPI2：屏幕与 TF 卡

| 信号 | GPIO | 连接/说明 |
| --- | --- | --- |
| SDO / MOSI | GPIO0 | 屏幕 SDA，同时作为 SPI2 MOSI |
| SCK | GPIO1 | 屏幕 CLK，同时作为 SPI2 SCLK |
| MISO | GPIO9 | SPI2 MISO；供 TF 卡读取 |
| EPD CS | GPIO5 | 屏幕片选 |
| EPD D/C | GPIO6 | 屏幕命令/数据选择 |
| EPD RESET | GPIO7 | 屏幕复位 |
| EPD BUSY | GPIO8 | 屏幕忙状态输入 |
| TF CS | GPIO10 | TF 卡片选 |

SPI 主机为 SPI2。板级代码将共享总线访问串行化；自行添加 SPI 设备时需沿用同一主机和总线锁，并在非选中设备片选保持高电平。屏幕 SPI 时钟配置为 2 MHz，TF 卡配置为 10 MHz。屏幕驱动使用 4 KiB 最大 SPI 传输长度。

### I²C0：板载控制与传感器

| 信号 | GPIO | 参数 |
| --- | --- | --- |
| SDA | GPIO2 | I²C0，共用总线 |
| SCL | GPIO3 | I²C0，共用总线 |

| 设备 | 7-bit 地址 | 源码通信速率 | 连接/用途 |
| --- | --- | --- | --- |
| AXP2101 | `0x34` | 100 kHz | PMIC、电池/充电状态、电源键事件搭配IRQ一起使用 |
| MSA321 | `0x62` | 400 kHz | 三轴加速度 |
| PCF8563 | `0x51` | 400 kHz | 实时时钟 |

上述为 7-bit 地址，直接用于 ESP-IDF I²C 设备配置，无需左移。设备共享 SDA/SCL，必须复用 `board_i2c_bus()` 创建的总线，并通过 `board_i2c_lock()` / `board_i2c_unlock()` 保护共享访问。总线内部上拉已在当前固件初始化中启用；板上也有 I²C 上拉电阻。

### 中断、使能与按键

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| IMU_IRQ | GPIO4 | MSA321 中断网络；当前固件将其配置为输入 |
| PMIC_IRQ | GPIO25 | AXP2101 nIRQ，低有效，下降沿通知任务 |
| RTC_IRQ | GPIO23 | PCF8563 中断网络；当前固件将其配置为输入 |
| 画面按键 | GPIO28 | 低电平按下，内部上拉；唤醒状态下切换到下一张缓存图片（带 IP 小字），并重新开始 2 分钟 Wi-Fi 计时 |
| 电源按键 | AXP2101 PWRON | 由 PMIC 中断/状态寄存器读取（nIRQ 接 GPIO25），不占用独立 MCU GPIO；浅睡时短按唤醒，深睡时只在 GPIO25 属于 LP 域时可直接唤醒，否则事件被 PMIC 锁存在下次定时唤醒时生效；唤醒状态短按切换白屏/恢复画面 |

屏幕逻辑方向默认值在 `main/pin_map.h` 的 `EINK_ROTATION` 中设置，运行中不再由按键旋转。GPIO28 还与原理图标注的启动绑带网络相关；更改上拉、电路或启动配置前应检查 ESP32-C5 启动要求。

## 3. 屏幕与图像格式

| 参数 | 值 |
| --- | --- |
| 面板 | EPD037A02，480 × 720 |
| 接口 | SPI，独立 CS / D-C / RESET / BUSY |
| SPI 主机与时钟 | SPI2，2 MHz（当前驱动配置） |
| 颜色 | 黑、白、黄、红、蓝、绿，共六种逻辑色 |
| 帧缓冲格式 | 每像素 4 bit；每字节 2 个纵向像素，高半字节对应偶数 y |
| 帧大小 | 172,800 字节（480 × 720 ÷ 2） |
| 方向 | 0° / 180°；默认值由 `EINK_ROTATION` 控制，详见工程仓库内参考代码 |

帧数据为列优先打包，发送前必须遵守驱动声明的排列与半字节顺序。不能将常见的行优先 4-bit 位图直接当作屏幕帧上传。面板初始化与刷新波形按当前板载屏幕 profile 使用 OTP 流程；不要将其他控制器或面板的初始化序列、LUT 和电气参数直接套用。

图像渲染全部在浏览器端完成：`eink_pipeline.js` 将裁剪图转换到 OKLab，以感知色差选择墨色，再做蛇形 Floyd–Steinberg 误差扩散。`photo` 风格会在扩散前做阴影提亮、高光压缩和轻微饱和度提升，`illustration` 更接近原色，`flat` 则直接取最近墨色（用于文字和图标）。选墨与误差扩散都在 OKLab 中计算：在 sRGB 或线性 RGB 距离下，中性灰会被判成蓝色或绿色，这是旧版渲染发灰、发绿并出现彩色噪点的主要原因；低彩度像素现在只允许黑、白两色参与扩散。固件仍然只接收最终打包好的 4-bit 色号帧，不参与任何图像运算。

## 4. 电源与启动

原理图的 PMIC 设计参数标注如下，表示原理图的计划/初始配置；实际输出还受 PMIC 寄存器和固件初始化影响：

| AXP2101 输出 | 原理图标注 |
| --- | --- |
| DCDC1 | 3.3 V，开启 |
| DCDC2 | 关闭 |
| DCDC3 | 关闭 |
| DCDC4 | 关闭 |
| DCDC5 | 默认关闭，仅在开关充电版本中用于电池充电及 VSYS 供电输出 |
| ALDO1 | 1.8 V，开启 |
| ALDO2 | 1.8 V，关闭 |
| ALDO3 | 3.3 V，开启 |
| ALDO4 | 3.3 V，关闭 |
| BLDO1 / BLDO2 | 关闭 |
| DLDO1 | 3.3 V，开启 |
| DLDO2 | 1.5 V，关闭 |

出货版本使用板载 AXP2101 管理电池充电、电源轨和电源键事件。随附固件先初始化 SPI2、I²C0 与 AXP2101，再启动 RTC、IMU、TF 卡、屏幕、Wi-Fi 和 HTTP 服务。电池容量为 500 mAh，标称电压为 3.7 V；续航会随屏幕刷新频率、Wi-Fi 使用和外设负载变化。未经核对板载负载，不要随意修改电源轨电压或使能状态。

### 低功耗状态机

`main/power_mgr/` 负责整机的省电策略，状态与触发条件如下：

| 状态 | 进入条件 | 处理 |
| --- | --- | --- |
| ACTIVE | 上电、按键唤醒、维护唤醒被按键升级后 | Wi-Fi 与 HTTP 服务正常；每次网页请求、按键操作、屏幕任务都会刷新活动时间戳 |
| LOW_POWER（深睡） | 无热点客户端，且距上次活动超过 `POWER_IDLE_TIMEOUT_MS`（默认 2 分钟），策略选择深睡（电池供电且「自动」、或「强制深睡」） | 调用 `wifi_mgr_suspend()` 关闭 SoftAP、停止 Wi-Fi 射频；去掉画面底部 IP 小字并切换到下一张缓存图片（无图片时显示 LOW POWER 提示）；把系统时间写回 PCF8563；`esp_deep_sleep_start()`，由 RTC 定时器按 `interval_min` 唤醒 |
| LOW_POWER（浅睡） | 同上但策略选择浅睡（插 USB 且「自动」、或「仅浅睡」） | 旧行为：MCU 每 `POWER_SLEEP_POLL_MS`（默认 500 ms）以轻睡眠定时唤醒，通过 I²C 读取 AXP2101 中断状态判断电源键单击 |
| MAINTENANCE | 定时器唤醒（`ESP_SLEEP_WAKEUP_TIMER`）且没有锁存的电源键事件 | 不开热点；有 STA 凭据时只连 STA（10 秒上限），SNTP 校时并写回 PCF8563、记录电量，运行唤醒任务钩子；不碰屏幕；整段 45 秒上限，随后继续深睡 |
| OFF | 定时唤醒关闭时低功耗持续 `POWER_AUTO_OFF_MS`（默认 30 分钟）且未接 USB；或未接 USB 且电量有效低于 `POWER_BATT_CUTOFF_PCT`（默认 5%） | 写 AXP2101 `COMMON_CONFIG` bit0 软件关机，DCDC/LDO 全部关断，仅 PWRON 可重新开机 |

要点：

- 热点客户端数量由 `wifi_mgr` 统计（`WIFI_EVENT_AP_STACONNECTED` / `STADISCONNECTED`），数量大于 0 时 `power_mgr` 持续刷新活动时间戳，因此不会进入低功耗；断开后重新开始计时，符合「AP 有客户端时不休眠」的约定。
- 唤醒后调用 `wifi_mgr_resume()`：有保存的 STA 凭据时先尝试连接路由器（12 秒），失败则重新打开配网热点；屏幕由 `main.c` 的 `screen_task` 重绘网络页。
- 唤醒原因在 `app_main` 一开始由 `power_mgr_boot_init()` 分类：`ESP_SLEEP_WAKEUP_TIMER` 走维护会话，`EXT1` / `GPIO` 或上电、复位走交互会话。定时唤醒后第一时间读 AXP2101 短按状态，若睡眠期间按过电源键（事件被 PMIC 锁存）则把本次会话升级为交互唤醒。
- 深睡唤醒源在启动时用 `esp_sleep_is_valid_wakeup_gpio()` 自检 GPIO25/4/28/23 并打印 `deep wake pins:` 日志。ESP32-C5 只有 LP 域 GPIO（0~6）能做深睡唤醒源；GPIO25 有效时用 `ESP_EXT1_WAKEUP_ALL_LOW` 单脚唤醒，无效时降级为纯定时器唤醒 + 锁存按键升级，`/api/power` 的 `deep_ok` 反映实际能力。
- 浅睡轮询使用 `esp_light_sleep_start()` + 定时唤醒；深睡则完全断电重启，靠 PCF8563 保时（开机 `pcf8563_restore_system_time()` 只向前调表，SNTP 成功后 `pcf8563_save_system_time()` 写回）。
- 两种睡眠都在 `power_mgr` 持有 `screen_rotation` 锁、且 `web_server_screen_busy()` / `screen_rotation_busy()` 为假之后才进入，避免 SPI/Flash 任务与睡眠重叠。浅睡会冻结负责喂狗的空闲任务，因此进入低功耗前用 `esp_task_wdt_reconfigure()` 暂停任务看门狗，唤醒后按原配置恢复，避免长睡眠触发 TWDT。
- 画面底部的白底黑字小字（`AP 192.168.4.1` 或 `STA <ip>`）只在工作状态显示，由驱动在 SPI 发送前临时叠加，不会写入图片缓存；进入低功耗时先清掉小字，再按槽位顺序刷新下一张图片，因此「有无 IP 小字」就是低功耗状态的直观指示，待机画面仍是完整无水印的一张图。屏幕被电源键刷白（清屏）时保持白屏，不参与切换。
- 三个图片槽位保存在 `image_cache` 分区，掉电不丢；网页上传可指定槽位（`X-Eink-Slot`），页面通过 `/api/slots`、`/api/slot?i=N` 显示三个槽位的缩略图。
- 固件内置三张 480 × 720 六色示例帧（`main/screen_driver/defaults/pic*.bin`，由 `demo_images/pack_default_frames.py` 从示范 PNG 生成）。`screen_rotation_init()` 发现三个槽位都无效时会把它们写入缓存，因此新机器或旧缓存格式升级后开机即可显示图片而不是配网文字；已有有效图片的设备不会被覆盖。
- 充电时（`axp2101_is_usb_connected()` 为真）「自动」策略只降功耗到浅睡、不深睡也不自动关机，避免 USB 调试和充电过程中意外断电；「强制深睡」则无视 USB。
- 自动关机规则随定时唤醒变化：`interval_min > 0` 时不再执行 30 分钟关机（否则定时唤醒会被关掉），只保留电量保护；把唤醒间隔设为 0 才恢复 `POWER_AUTO_OFF_MS` 规则。
- 睡眠策略与唤醒间隔保存在 NVS 命名空间 `pwrmgr`（`interval_min`、`mode`），网页「设置」页或 `GET/POST /api/power` 读写，下一次进入低功耗时生效。
- 若产品需要更激进的续航，可进一步关闭面板电源轨或调用 `screen_driver_deep_sleep()`；注意后者会让面板进入深睡，唤醒后需要重新初始化面板才能刷新。

## 5. 开发环境与构建

随工程附带的固件位于 `https://github.com/osptek/devkit-hueleaf_base/tree/main`，基于 ESP-IDF，配置目标为 `esp32c5`。建议使用 ESP-IDF 5.5x 环境，并在该目录运行：

```powershell
idf.py set-target esp32c5
idf.py build
```

如需烧录，连接 USB Type-C 数据线后确认下载模式和串口号，再执行：

```powershell
idf.py -p COMx flash monitor
```

请将 `COMx` 替换为设备管理器中实际识别的串口。ESP32-C5HF4 内置 4 MB Flash，当前 `sdkconfig.defaults` 和分区表按 4 MiB 配置，包含 NVS、PHY 初始化数据、factory 应用及 1 MiB `image_cache`。调整应用、NVS 或图像缓存空间时，应同步检查分区表总容量及偏移，确保不超过 4 MB Flash。

建议的二次开发顺序：先构建并烧录原始工程，确认屏幕、按键和 Wi-Fi 页面工作；再从 `main/pin_map.h`、`main/screen_driver/` 与 `main/web_server/` 了解板级引脚、帧格式和上传流程；最后按需扩展业务逻辑。新增外设优先复用现有 SPI2 或 I²C0 总线与锁，新增任务时注意堆栈、静态缓冲区和屏幕刷新期间的资源占用。

## 6. 固件功能与源码入口

| 功能 | 主要源码 | 当前行为 |
| --- | --- | --- |
| 板级总线与锁 | `main/board/board.c`、`main/pin_map.h` | 初始化 SPI2、I²C0，管理共享锁和 IRQ 输入 |
| 电子紙驱动 | `main/screen_driver/` | 初始化 EPD037A02，写入六色帧，流式接收并刷新，支持休眠 |
| Wi-Fi 配网 | `main/wifi_mgr/` | SoftAP 初始开放网络名为 `Eink E6 3.7`，访问 `http://192.168.4.1`；配置 STA 后切换到局域网地址；提供 `wifi_mgr_suspend()` / `wifi_mgr_resume()` 供低功耗使用，`wifi_mgr_sta_start()` 供定时维护唤醒只连 STA |
| 低功耗管理 | `main/power_mgr/` | 2 分钟无网络活动关闭 AP：电池供电深睡 + 定时唤醒（维护会话只校时、不刷屏），插 USB 保持浅睡；电源键唤醒重开 AP；定时唤醒关闭时保留 30 分钟关机，另有 5% 电量保护 |
| Web 屏幕服务 | `main/web_server/` | 浏览器端图像上传和设备状态接口 |
| TF 卡 | `main/tf_card/` | 与屏幕共用 SPI2；以 FAT 文件系统挂载到 `/sdcard`，不自动格式化卡 |
| PMIC | `main/pmic/axp2101.c` | 初始化电源管理、读取电池/充电状态和电源键事件 |
| RTC | `main/rtc/pcf8563.c` | 当前初始化会将 RTC 写为固件编译时间并回读；不是网络校时 |
| IMU | `main/imu/msa321.c` | 当前以 ±4 g、125 Hz 配置，启动时读取一次加速度样本 |

SoftAP 当前配置为开放网络，仅建议在受控开发环境中使用。开发网页服务时注意该 AP 没有密码保护。

## 7. 开发注意事项

- 屏幕和 TF 卡共享 SPI2。并行访问需遵循板级 SPI 锁和独立片选管理，避免在文件操作期间直接操作另一设备。
- PMIC、IMU 和 RTC 共用 I²C0。不要重复初始化相同 GPIO 的独立 I²C 控制器；添加设备时沿用共享总线并加锁。
- TF 卡初始化失败不会自动格式化介质；确认 FAT 文件系统、卡片和 CS 连接后再排查。
- PCF8563 只在首次使用（VL 置位或读数不合理）时写入编译时间，之后开机读取并灌入系统时钟，SNTP 校时成功后写回；调试时注意不要用编译时间覆盖现场走时。
- 出货版本不具备 NFC 功能。扩展其他外设时，分配 GPIO 前请检查启动绑带、板上连线和当前固件占用，避免影响启动或已有功能。
- 电池容量为 500 mAh，标称电压 3.7 V。涉及充电、电池保护或续航评估时，应结合实际使用工况和电源管理配置验证。
- 固件默认会在 2 分钟无网络活动后关闭 Wi-Fi：电池供电进入深睡（定时唤醒），USB 供电保持浅睡。深睡时串口日志停止、网页断开属于预期行为，按一次电源键即可恢复；若自检显示 GPIO25 不能作为深睡唤醒源，按键会被 PMIC 锁存并在下一次定时唤醒时生效，也可以长按电源键 4 秒关机后重新开机。长时间连机调试时，把 USB 插上（「自动」策略保持浅睡）、临时调大 `POWER_IDLE_TIMEOUT_MS`，或在网页「设置」里把睡眠策略设为「仅浅睡」；USB 供电不会阻止进入低功耗，只会改变睡眠深度和自动关机规则。


