# 原装显示和触摸上电验证（第 2 件事）

## 范围

验证 P4X 套装自带的原装显示路径，不接 HDMI：

- AML070JGI50-07403L，7 英寸、1024 x 600；
- EK79007AD + EK73217BCGA；
- 2-lane MIPI-DSI；
- GT911 电容触摸。

显示排线由套装的 DSI FPC 接口连接。该接口同时提供 DSI 双数据通道、I2C、触摸复位/中断和背光控制；不要在此阶段改接 GPIO 或自行编写面板初始化序列。

## 固件方案

- BSP：`espressif/esp32_p4_function_ev_board` 5.2.3；
- 屏驱动：`espressif/esp_lcd_ek79007` 2.0.2~1；
- 触摸驱动：`espressif/esp_lcd_touch_gt911` 1.2.1；
- 图形库：LVGL 9.6.0~1；
- LVGL Port：锁定 2.5.0。2.9.0 需要 ESP-IDF v5.5.4 中没有的 DPI 回调字段，不能在当前 IDF 上使用。

依赖锁文件为 `firmware/p4_main/dependencies.lock`。下载组件保存在工程内的 `managed_components`，无需向系统安装 BSP。

## 运行

```powershell
Set-Location E:\zhoumian\Built-In\Built_In_V6
.\tools\Invoke-P4Build.ps1 build
.\tools\Invoke-P4Build.ps1 flash -Port COM20
.\tools\Invoke-P4Build.ps1 monitor -Port COM20
```

## 验收

1. 屏幕显示深蓝色验证页，包含 `Built_In_V6`、`1024 x 600`、`EK79007` 和 `GT911`；
2. 触摸任意位置后，页面文字改为 `Touch detected: x=... y=...`；
3. 串口至少出现以下初始化日志：

   ```text
   Install EK79007 LCD control panel
   Display initialized
   Display resolution 1024x600
   GT911: TouchPad_ID:0x39,0x31,0x31
   display and touch verification UI ready
   ```

4. 每次触摸会输出 `p4x_baseline: touch: event=..., x=..., y=...`。

## 本机首次结果（2026-10-03）

- 已通过显示验收：用户在 P4X 原装套装屏上确认看见深蓝色 `Built_In_V6` 验证页；标题、`1024 x 600`、`EK79007` 与 `GT911` 文本均正常显示。因此 MIPI-DSI 排线、面板初始化、背光和 LVGL 渲染链路已确认可用。
- COM20 实机日志已完成第 3 项，屏幕与触摸控制器均被识别。
- 已通过触摸验收：用户确认点击后坐标回显正常。GT911 的触摸事件、LVGL 输入设备注册和 1024 x 600 触摸坐标映射均已确认可用。

启动时出现 `GPIO 26 is not usable, maybe conflict with others` 的 LEDC 警告；这是 BSP 设置背光时发出的警告，尚不表示背光失败。当前实际背光和画面正常，故记录为非阻塞告警。后续在整机功耗和背光调光阶段单独处理，避免在显示链路已正常的情况下过早改动板级配置。

## 结论

第 2 件事“跑通原装屏幕和触摸”已完整通过。后续界面开发应继续使用 BSP 的 `bsp_display_start()`、`bsp_display_get_input_dev()` 和 `bsp_display_lock()` 路径，不重复配置 MIPI-DSI、面板寄存器或 GT911 引脚。
