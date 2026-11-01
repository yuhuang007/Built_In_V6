# 板载麦克风与外接扬声器上电验证（第 3 件事）

## 实际硬件路径

本原型使用 P4X Function EV Board 的 ES8311 音频编解码器：

- 板载模拟麦克风 → ES8311 ADC → I2S RX；
- ESP32-P4 → I2S TX → ES8311 DAC → 板载功放 → 已接入的外接扬声器；
- 配置控制：I2C，GPIO 7 (SDA) / GPIO 8 (SCL)；
- I2S：GPIO 9 (DOUT)、10 (LRCK)、11 (DIN)、12 (BCLK)、13 (MCLK)；
- 功放使能：GPIO 53，由 BSP 控制。

旧项目说明中 INMP441、MAX98357A 和 ESP32-S3 的 GPIO 编号属于计划中的 CS001 副板，不能直接用于当前 P4X 套装。此阶段不改写板级 I2S 引脚，完全使用官方 BSP 的 `bsp_audio_codec_microphone_init()` 与 `bsp_audio_codec_speaker_init()`。

## 验收固件行为

触摸屏幕任意位置后，固件会：

1. 从板载麦克风录制 3 秒 PCM；
2. 格式为 16 kHz、16-bit、单声道，这是后续语音识别的基准格式；
3. 将 96,000 字节录音暂存在已验证的 PSRAM；
4. 录制结束才经外接扬声器回放，避免实时回放的声学反馈啸叫；
5. 在屏幕显示阶段状态，串口打印 `audio:` 日志。

默认输入增益为 30 dB、输出音量为 45%。首次试听请避免把扬声器贴近麦克风；需要时再单独调整增益与音量，不在底层初始化阶段盲调。

## 构建、烧录与验收

```powershell
Set-Location E:\zhoumian\Built-In\Built_In_V6
.\tools\Invoke-P4Build.ps1 build
.\tools\Invoke-P4Build.ps1 flash -Port COM20
.\tools\Invoke-P4Build.ps1 monitor -Port COM20
```

验收条件：

1. COM20 出现 `audio: ES8311 initialized` 和 `audio verification UI ready`；
2. 屏幕点击后显示 3 秒录音状态；
3. 随后能从外接扬声器听到刚才的录音；
4. 串口出现 `audio: record and playback verification complete`，屏幕显示 `PASS`。

若只有录音或播放之一失败，保留完整 COM20 日志，并检查外接扬声器是否接在 P4X 的音频输出/功放接口、扬声器阻抗和接线极性；不要把扬声器接到 GPIO 9/10/11/12/13 的数字 I2S 信号线上。

## 本机首次结果（2026-10-03）

已构建并烧录至 COM20。实机启动日志确认：32 MiB PSRAM、ES8311 双工 I2S 通道和音频验证界面均初始化成功，且无音频初始化错误：

```text
p4x_baseline: audio: ES8311 initialized (mic gain 30 dB, speaker 45%)
p4x_baseline: audio verification UI ready
```

人工声学验收已通过：用户确认点击屏幕后，板载麦克风录音与外接扬声器回放均正常。因此麦克风、ES8311 ADC/DAC、I2S、GPIO 53 功放使能和外接扬声器接线均已验证可用。

## 结论

第 3 件事“跑通 P4X 板载麦克风和外接扬声器”已完整通过。后续 ASR 应直接复用 16 kHz、16-bit、单声道 PCM 的采集格式；TTS/提示音经同一 ES8311 播放路径输出。实时回声消除、自动增益与降噪属于后续语音算法阶段，不应在当前已验通的硬件链路中提前替换底层驱动。
