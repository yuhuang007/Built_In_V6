# Built_In_V6

聋哑人智能翻译终端的原型工程。

当前阶段是 P4X 单板原型：火山硬件对话智能体处理 ASR、对话和 TTS；P4X 负责音频采集/播放、7 寸大字字幕、触摸交互与后续本地静态手势识别。语音交互为“按住说话、松开发送”，同一语音只走一次云端会话。

完整的当前架构、资源边界、手语 MVP 和冻结项见 [P4X 单板双向沟通原型](docs/architecture/01-single-board-prototype.md)。

## 当前目录

```text
firmware/p4_main/        ESP32-P4 主控固件
docs/bringup/            板级上电与诊断记录
protocol/                P4 与后续 S3 副控间的通信协议（待实现）
gateway/                 后期实验资产（当前冻结、不得由固件启动/调用）
tests/                   测试资产（待实现）
tools/                   开发辅助工具（待实现）
```

## 构建与烧录

首次构建前，项目会使用根目录下隔离的 `.idf-build-env`。本机原有 ESP-IDF v5.5.4 配置指向一个缺失的 Python 3.11，因此不要直接依赖其全局 PowerShell 配置。使用项目构建入口：

```powershell
Set-Location E:\zhoumian\Built-In\Built_In_V6
.\tools\Invoke-P4Build.ps1 build
.\tools\Invoke-P4Build.ps1 flash -Port COM20
.\tools\Invoke-P4Build.ps1 monitor -Port COM20
```

详细验收标准见 [P4X 基础上电诊断](docs/bringup/01-p4x-baseline.md) 与 [显示和触摸上电验证](docs/bringup/02-display-touch.md)。
