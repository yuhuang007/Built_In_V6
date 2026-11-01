# 网络与云端语音链路（第 4 件事）

> 2026-10-06 重整说明：以下较早的排障记录保留作历史依据。当前单板原型已验证 RTC 连接、ASR 字幕、云端回答和 TTS PCM 播放。固件只保留火山原生语音会话，已移除启动文字自检、ASR 文本二次提交和日期注入；详情以 [P4X 单板双向沟通原型](../architecture/01-single-board-prototype.md) 为准。`gateway/` 与千帆资产冻结，当前不运行、不参与 P4 构建或网络会话。

## 方案选择

优先采用竞赛命题提供的“硬件对话智能体”。它是面向 IoT 的端到端语音交互平台，负责 ASR、对话大模型编排和 TTS；P4 侧只保持稳定的 PCM 采集、网络会话、字幕显示和 PCM 播放。这比临时拼接三家独立服务更适合首个原型，也最贴合命题的专项资源。

文本大模型的可替换后端为同一命题页列出的火山边缘大模型网关：只在需要手语/文字上下文翻译、设备控制指令和结构化回复时使用。不要让它承担低延迟的语音流转。

## P4X 网络结构

ESP32-P4 本身不带 Wi-Fi；P4X 套装通过板载 ESP32-C6 作为 ESP-Hosted 协处理器，使用板内 SDIO 四线连接。P4 运行 ESP-IDF 的 `esp_wifi_remote` 与 `esp_hosted`，C6 保持套装预烧录的协处理器固件。

Wi-Fi 密钥位于 `secrets/p4x_wifi.h`，该目录被 Git 忽略；仓库只保留 `secrets/p4x_wifi.h.template`。日志禁止输出 SSID 与密码。

## 当前进度（2026-10-04）

- 已加入并锁定 ESP-Hosted 2.12.13、ESP Wi-Fi Remote 1.6.5；所有组件下载到工程的 `managed_components`，没有安装到 C 盘。
- 已成功构建并烧录 COM20；ESP-Hosted 实机日志确认 SDIO 四线、板载 C6 与 WLAN 通道已建立：`Card init success`、`Identified slave [esp32c6]`、`Transport active`、`wifi station started`。
- 已通过 DHCP 获取 IPv4：`10.168.29.7`（网关 `10.168.29.1`，掩码 `255.255.255.0`）。因此第 4 步的本地 Wi-Fi 联网验收通过。
- C6 预烧录协处理器版本显示为 `0.0.0`，官方主机组件提示建议后续升级以避免高频 RPC 超时。当前网络已正常工作，不升级 C6 固件；仅在后续出现可复现的 RPC 超时后，再按官方流程开展可回退的专项维护。
- 已接入本地下载的火山引擎 `ConversationalAI-Embedded-Kit-2.0` 的 P4 RTC 组件，并新增官方 `espressif/zlib` 1.3.2~1 依赖。该 Kit 包含 `esp32p4` RTC 预编译库与 C6 ESP-Hosted 配置；当前 ESP-IDF v5.5.4 已成功完成 P4 编译和链接。
- 首轮验证固件会在 Wi-Fi 获得 IPv4 后：同步 SNTP 时间、使用 Git 忽略的本地凭据动态注册设备、请求 RTC 会话；本轮暂不发送麦克风音频或播放云端音频，先隔离验证云端注册/RTC。
- 2026-10-04 已切换到用户提供的可用 2.4 GHz Wi-Fi，并稳定获得 DHCP IPv4（地址不作为固定配置记录）。P4 端已成功完成火山引擎动态注册、获取 RTC 房间配置并发起加入房间；由此验证本地私密凭据、设备/产品/智能体关联和 P4→C6→云端的 HTTPS 配置请求均正确。
- RTC 底层接入尚未触发 `VOLC_EV_CONNECTED`。首次诊断显示其 Access TLS 握手超时；随后严格采用官方 P4 高质量示例的 Wi-Fi（16/64/64、BA window 32）及 lwIP（64 KiB TCP send/window、64 项邮箱、SACK）缓冲配置后，现象未改变。因此不能把问题归因于 Wi-Fi 缓冲不足，也不应在没有官方兼容性证据时升级板载 C6 固件。
- 安全修正：供应商预编译 RTC 库会在 INFO 级别输出完整会话令牌。工程将 RTC 库日志限制为 FATAL 级，保留应用层的连接状态日志；后续不得临时调高该日志级别或将原始串口记录写入文档、Git 或聊天。
- 诊断固件新增了无凭据的 RTC 网络探针：对 `access.rtc.volcvideo.com`、`access2-hl.rtc.volcvideo.com`、`rtcg-access.volcvideos.com` 分别做 DNS 与 TCP/443 三次握手，实机均通过。故当前路由器、DNS、P4↔C6 SDIO 和到 RTC 接入点的基础 TCP 路径均不是阻塞原因；不需要用户再以手机热点重复这项测试。
- 进一步按官方高质量 P4 示例将 mbedTLS 分配从内部 RAM 切到外部 PSRAM；实机仍可完成前述探针、动态注册及 RTC join 请求，但 60 秒内仍未收到 `VOLC_EV_CONNECTED`。当前最可能的边界是 RTC 预编译库与 ESP-IDF/ESP-Hosted C6 协处理器版本组合，而不是应用层证书、密钥或普通网络配置。下一步先获得官方可恢复的 C6 固件版本对应关系，再决定是否进行受控升级。
- 已用 `esp_hosted_get_coprocessor_fwversion()` 做只读实机检查：C6 对 `Req_GetCoprocessorFwVersion` RPC 超时，返回 `ESP_FAIL`。这与启动时的 `Host [2.12.0] > Co-proc [0.0.0]` 一致，证明当前 C6 从端不支持主端 2.12 所需的版本查询 RPC；这已是可复现的 ESP-Hosted 协议版本不匹配证据。该调用没有写入、重启或改变 C6。
- 官方组件 2.12.13 对应 `esp-hosted-mcu` 提交 `dd0176e5fc959d79b306f6677a2f878123c58e9a`。其 P4X 指南说明板载 C6 原厂固件为 v0.0.6，推荐更新；但 Hosted OTA 也依赖从端支持相应 OTA RPC。当前从端连版本 RPC 都不响应，不能假定 OTA 可安全升级。应先确认是否有 ESP-Prog（或可用 USB-UART 下载器）以便通过板上 `PROG_C6` 进行可恢复的首次刷写；在此之前禁止执行 C6 OTA 或擦写。

## 云端接入的前置条件

硬件对话智能体已在控制台创建并与产品关联。Kit 使用实例 ID、ProductKey、轮换后的 ProductSecret、设备名与智能体 ID 动态注册，设备首次成功注册后才会出现在控制台激活设备列表。这些属于云账号密钥，必须像 Wi-Fi 密钥一样只放进 `secrets/`，不能写进源码、文档或 Git。完成注册后再实现音频分帧、ASR 中间/最终字幕、TTS 音频回写与断线重连。
