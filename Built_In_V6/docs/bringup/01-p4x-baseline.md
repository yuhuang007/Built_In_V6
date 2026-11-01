# P4X 基础上电诊断（第 1 件事）

## 目标

在不启动显示、音频和摄像头业务的前提下，验证 ESP32-P4X Function EV Board 的最小可复现开发闭环：构建、烧录、复位及串口日志。

## 环境基线

- 工程：`E:\zhoumian\Built-In\Built_In_V6\firmware\p4_main`
- 芯片目标：`esp32p4`
- 已安装开发环境：ESP-IDF v5.5.4
- 串口：`COM20`
- 波特率：115200

本机既有 IDF 配置的 Python 3.11 已缺失；本工程使用根目录 `.idf-build-env` 中隔离的 Python 环境，避免修改系统 Python 或全局 ESP-IDF 配置。后续使用 `tools\Invoke-P4Build.ps1` 进行构建、烧录和监视。

## 本阶段验收日志

启动后必须看到 `p4x_baseline` 标签，并包含：

1. `board_profile=p4x_function_ev_board`；
2. ESP-IDF 版本、构建日期和时间；
3. 芯片 revision、核心数、复位原因；
4. Flash 实测容量；
5. 内部堆状态；
6. PSRAM 是否注册、总容量和可用容量。

Flash 与 PSRAM 容量均以实际启动日志为准；不要仅根据套装宣传页推断。若 PSRAM 未注册，暂停后续摄像头/显示缓存开发，先排查 `sdkconfig`、模块实装及启动错误。

## 本机实测结果（2026-10-03）

- ESP32-P4 revision v3.2，双核，CPU 400 MHz；
- GD SPI Flash：16 MiB，DIO，80 MHz；
- AP HEX PSRAM：32 MiB，200 MHz，启动内存测试通过；
- 当前基础镜像约 220,720 bytes，占 2 MiB factory 槽约 11%；
- 使用 `COM20` 的 USB-Serial/JTAG 成功烧录和复位。

上述数据已由本工程启动日志验证，可作为显示帧缓冲、摄像头帧缓冲及后续 OTA 分区设计的硬件基线。

## 固件布局

当前 `partitions.csv` 以 16 MiB Flash 为设计假设，提供 3 MiB factory、一个 6 MiB OTA 槽、一个 5 MiB OTA 槽和约 1.875 MiB 本地存储区。2026-10-04 引入火山引擎 P4 RTC 客户端后，应用超过旧 2 MiB factory 槽，因此在首次云端验证前完成此布局调整；总容量仍严格落在 16 MiB Flash 内。实际日志若不是 16 MiB，必须在引入 OTA 或本地存储前调整分区表。

## 非本阶段范围

- MIPI-DSI 显示和触摸；
- 板载麦克风 / ES8311 音频；
- MIPI-CSI 摄像头；
- Wi-Fi、云端接口、S3 副控通信。
