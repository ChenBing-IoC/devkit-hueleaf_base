# 六色墨水屏局部窗口刷新探针

本文说明固件内置的 `POST /api/panel/probe` 探针：它只用于回答一个问题——
在现有 OTP LUT 下，EPD037A02 的"局部窗口"到底能走到哪一步，能不能省下
约 23 秒的 DRF 时间。

探针是一条独立的 bring-up 路径，不改变正常传图、清屏、TF 导入导出的行为。

## 1. 为什么默认关闭

驱动现有代码只证明了整屏寻址：帧按 gate 排列（gate = 479 − x），每字节两个
纵向像素（行号 = y / 2）。`0x91 / 0x92 / 0x90` 这组 JD/IST 局部窗口命令在
本仓库里从未使用过，`0x92` 的 9 字节参数顺序也无法从现有整屏代码推断。
因此探针默认在编译期关闭：

```c
/* main/screen_driver/screen_driver.h */
#ifndef SCREEN_PARTIAL_PROBE_ENABLE
#define SCREEN_PARTIAL_PROBE_ENABLE 0
#endif
```

确认风险后再打开（或临时在编译参数里加 `-DSCREEN_PARTIAL_PROBE_ENABLE=1`），
重新构建烧录。未打开时 `POST /api/panel/probe` 返回 `501 Not Implemented`。

## 2. 调用方法

### 2.1 发起探针

```bash
curl -s -X POST http://192.168.4.1/api/panel/probe \
  -H 'Content-Type: application/json' \
  -d '{"x":0,"y":0,"w":240,"h":80,"repeat":1,"mode":"refresh"}'
```

字段：

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `x` / `y` | 是 | 窗口左上角，当前 rotation 下的逻辑坐标 |
| `w` / `h` | 是 | 窗口宽高，至少 8 像素；窗口必须完全在屏内 |
| `repeat` | 否 | 重复次数 1..10，默认 1；用于观察残影累积 |
| `mode` | 否 | `refresh`（默认）：窗口写入 + DRF；`write`：只写窗口数据，不 DRF |

HTTP 立刻返回 `202 Accepted` 和作业号，不会占用 HTTP 任务：

```json
{"ok":true,"id":7,"phase":"refresh","err":0,"kind":"panel-probe","step":"probe",
 "bytes":0,"total_ms":0,"refresh_ms":0,"display_generation":12,
 "current_display_generation":12,"sequence":"panel-probe-v1",
 "plane_fnv":["00000000"],"completed_mask":0}
```

非法参数返回 `400`（窗口越界、`w`/`h` 小于 8、`repeat` 越界、`mode` 非法）；
屏幕忙返回 `409`；探针未编译进去返回 `501`。

### 2.2 轮询与读取结果

作业状态沿用现有接口：

```bash
curl -s http://192.168.4.1/api/job
```

结果里 `phase` 回到 `idle` 且 `err=0` 表示探针完成。之后读取最近一次探针结果：

```bash
curl -s http://192.168.4.1/api/panel/probe
```

```json
{"ok":true,"enabled":true,"ran":true,
 "x":0,"y":0,"w":240,"h":80,"repeat":1,"mode":"refresh",
 "write_ms":41,"drf_ms":22980,"cleanup_ms":23150,"total_ms":46180,
 "bytes":9600,"repeats":1,"busy_asserted":true,"busy_failures":0,
 "exit_partial":true,"cleanup":true,"rotation":0,
 "gate":[240,479],"row":[0,39],"panel":[0,0,239,79],
 "err":0,"err_name":"ESP_OK","at_ms":123456}
```

耗时字段：

- `write_ms`：`0x10` + 窗口数据，所有重复次数累计；
- `drf_ms`：`0x12` 后等待 BUSY 的时间，所有重复次数累计；
- `cleanup_ms`：一次整屏白刷的耗时（`0x90` 的耗时只计入 `total_ms`）；
- `total_ms`：整个探针（含 `0x90` 和清理，不含随后重放缓存图片的整刷）。

`bytes` 是单次窗口写入的字节数（gate 数 × 字节行数）。
`busy_asserted=false` 或 `busy_failures>0` 表示某次 DRF 没有把 BUSY 拉低，
说明窗口命令很可能没有被控制器接受。

### 2.3 屏上会发生什么

1. 窗口内填充纯红（色号 3），位置应严格落在请求的逻辑窗口上；
2. 探针无论成功失败都会发送 `0x90` 退出局部模式；
3. 随后做一次整屏白刷，把画面恢复成干净白屏；
4. 作业任务再重放最近一张缓存图片（如果有）。

所以有缓存图片时，真机上会看到"红块 → 白屏 → 原图"，总时间约为
`窗口 DRF + 白刷 + 重放原图` 三次整屏刷新量级（约 70 秒）；这是预期行为，
不是卡死。`total_ms` 只统计到白刷结束，不含重放。探针全程持有 rotation 锁，
低功耗不会插入。

## 3. 判断标准

### 3.1 窗口位置与形状

- 红块位置、宽高与请求一致 → 逻辑坐标到 gate/byte 的换算正确；
- 红块左右镜像 → gate = 479 − x 的方向或 `0x92` 的 x 起止顺序反了；
- 红块整体平移 → `0x92` 的单位（gate/byte）或起始字节顺序不对；
- 整屏花屏、整屏红、或完全没有变化 → `0x92` 布局不被接受，或控制器要求
  不同的 partial 进入/退出序列。

### 3.2 DRF 耗时

- `drf_ms` 与整屏刷新（约 23000 ms）同量级 → 现有 OTP LUT 只按整屏波形
  刷新，局部窗口只省了数据传输；
- `drf_ms` 明显缩短（例如 2–3 秒）→ 控制器确实按窗口驱动，值得继续做
  局部刷新 UI；
- `busy_asserted=false` → DRF 根本没有启动，先怀疑窗口命令，而不是 LUT。

### 3.3 残影

用同一窗口 `repeat=5` 或 `10` 连续刷新，观察红块边缘和窗口外区域：

- 窗口外始终干净、红块边缘不扩散 → 窗口隔离良好；
- 重复次数增加后窗口外出现淡红/灰影 → 波形有累积，后续必须配局部清屏或
  降低局部刷新频率。

## 4. 三种结论与下一步

| 结论 | 现象 | 下一步 |
| --- | --- | --- |
| ① 窗口无效 | 花屏、错位严重、BUSY 不拉低 | 放弃在现有固件上做局部刷新；保留本探针作为证据，向厂商索取该面板的 partial 初始化/窗口命令文档 |
| ② 窗口有效但 DRF 仍约 23 秒 | 红块位置正确，`drf_ms≈23000` | 局部窗口只能省 SPI 数据量，省不了刷新时间；要 2 秒级局部刷必须向厂商要快速 LUT（RAM LUT 或 OTP 快速波形） |
| ③ DRF 明显变短 | 红块正确，`drf_ms` 明显小于整屏 | 值得继续做局部刷新 UI：先做局部时钟/状态条，再评估残影与功耗 |

## 5. 记录表模板

| 日期 | 固件版本 / commit | 窗口 x,y,w,h | rotation | mode | repeat | write_ms | drf_ms | cleanup_ms | total_ms | busy | 现象 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-11-27 |  | 0,0,240,80 | 0 | refresh | 1 |  |  |  |  |  |  |
|  |  | 0,0,240,80 | 0 | refresh | 5 |  |  |  |  |  |  |
|  |  | 300,600,120,60 | 0 | write | 1 |  |  |  |  |  |  |

## 6. 已知不确定点

1. **`0x92` 的 9 字节布局是族级推测，不是从本仓库代码推导出来的。**
   探针按 JD/IST 常见的"每轴两个 9-bit 字段、小端、末尾 1 个控制字节"实现：

   ```
   [x_start_lo, x_start_hi, x_end_lo, x_end_hi,
    y_start_lo, y_start_hi, y_end_lo, y_end_hi, 0x00]
   ```

   其中 x 用 gate 单位（0..479），y 用 RAM 字节行单位（0..359）。
   如果真机表现与预期不符，优先怀疑这里。
2. **`0x91` / `0x90` 是否带数据字节。** 当前按族内常见写法：`0x91` 后发
   `0x00`，`0x90` 后不发数据。若控制器要求不同，可能表现为无法进入/退出
   局部模式。
3. **PON/POF 的位置。** DRF 前插入了 `0x04` PON、DRF 后 `0x02` POF，与现有
   整屏路径一致；不排除该面板的局部 DRF 不需要 PON，或需要不同的电源时序。
4. **窗口 Y 边界按 2 像素对齐。** 帧缓冲一个字节覆盖两行，探针会把窗口
   上下边缘扩到完整字节行，最多多画 1 个像素。
5. **OTP LUT 是否理会窗口。** 即使窗口命令正确，OTP 波形也可能按整屏时序
   执行；这正是 `drf_ms` 要记录的核心数据。

任何一步失败后，探针都会尝试 `0x90`，并在清理阶段做一次硬件复位 + 整屏
白刷，因此即使窗口命令把控制器留在异常状态，也会被复位拉回。
