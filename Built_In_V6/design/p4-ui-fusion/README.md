# 灵译通 · 和光融合界面

直接双击 `index.html`。无依赖安装、CDN、云 API、麦克风或摄像头访问；刷新恢复虚构样例。保持本目录完整即可离线使用。

这是一套重新组合页面结构、配色、图标和控件的灵译通交互设计，不是第三方作品展示页。先前四套概念与参考板保留。本轮没有把 HTML 移植到 P4。

## 融合方式

- 借鉴 [Artilate / Rizki Padilah for Scalla](https://dribbble.com/shots/24234097-Artilate-Translate-AI-App) 的奶白、浅蓝、浅黄协调关系。实际颜色、版式和组件在本项目重新组织；没有使用作者图片或图标资产。
- 借鉴 [Language Translator / Sahil Dobariya](https://dribbble.com/shots/23823142-Language-Translator-Mobile-App) 的明确主操作与功能层级。首页将语音对话置于黑色主入口，转写、相机使用低饱和辅助入口。
- 借鉴 [Echoic / Sahil Dobariya for Heloxone](https://dribbble.com/shots/27184382-Voice-Recording-App-Echoic) 的专用录音控制思想。识别状态、录音操作和清空分开，危险动作进入统一确认框；未复制其点阵字体、螺丝装饰和拟物阴影。

## 页面与体验

| 页面 | 设计与操作 |
| --- | --- |
| 首页 | 一个语音主入口，两个辅助功能入口；底部首页/对话/转写/设置四入口保持一致 |
| 对话 | 白色回复、浅蓝用户消息；按住说话/松开发送、回复等待/打断、文字输入 |
| 转写 | 连续正文、轻量段落编号；底部跟随、上翻保位、持续变长/缩短修订；回到最新只在需要时出现在正文下方专用栏 |
| 清空 | 统一奶白确认框、浅杏提示标记、低饱和红色确认；取消保留位置，确认清空；识别期间禁用 |
| 输入 | 白色嵌入式输入、黑色发送；顶部拼/E、上浮候选及翻页；USB 模拟接入收起软键盘，电脑打字不自动展开 |
| 相机 | 抽象取景示意、暂停、辅助线；没有真实相机或手语识别 |
| 设置 | 白色分组、统一列表、亮度/音量/字号/息屏与连接；Wi-Fi 为失败/恢复/成功模拟 |

左侧体验面板可载入长文、修订结果、模拟 USB/离线/发送失败。发送失败保留文字。候选是固定两页样例，不是真实拼音算法。样例密码 `demo1234`，不要输入真实密码。

快捷查看：

- [首页](index.html?page=menu)
- [对话](index.html?page=home)
- [上翻阅读与回到最新](index.html?page=transcript&scenario=reading)
- [候选与软键盘](index.html?page=transcript&scenario=keyboard)
- [清空确认](index.html?page=transcript&scenario=confirm)
- [设置](index.html?page=settings)
- [相机](index.html?page=camera)
- [Wi-Fi](index.html?page=wifi)

`embed=1` 可用于嵌入预览。上述 scenario 是可分享的本地设计状态；不调用真实服务，不保存输入，也不加入板端菜单。

## P4 边界

逻辑画布 600×1024，默认正文/标题 28px，转写行高 54px，输入框 64px，候选行至少 62px。大部分主要触控区域不小于48px。页面使用布局、纯色与内联 SVG；首页静态条形为本地示意，不代表正在录音。

浏览器字体与板端 SD 字库的基线、字形和换行不同；HTML 不能证明旧缺字问题解决。新增首页、圆角、底部导航及图标均仍需选型后确认 LVGL 对象、资源和实际字库度量。外部预览框的阴影不是板端核心效果。没有改相机方向、CPU 复制回退、模式资源交接、依赖或分区。

## 验证

`ui.js` 语法检查通过。Tabbit 实际运行 `verify-interactions.js`，通过阅读跟随与修订、清空保护及继续使用、软键盘与候选、USB 不抬页、失败保留与重试、语音/打断、相机控制、设置和 Wi-Fi 模拟；未捕获页面脚本或控制台错误。键盘及候选同时显示时，正文阅读区约 240 逻辑像素，候选与录音控件分离。

Tabbit 在 file/HTTP、后台/用户打开标签后均出现截图超时；不将其描述为实屏或画面验收通过。代表性 PNG 改用独立 Edge 无头进程渲染本地 HTML，使用专用临时 profile，不访问用户登录会话。最终实际看图结果及截图见 `previews/`；浏览器交互结果不代替 P4 触屏、字库或真实语音验收。

无固件提交、构建、烧录或 COM20 操作。额度申请已按用户后续指令停止；本原型不受其状态影响。

## 预览图

[四页总览](previews/overview.png) · [首页](previews/menu.png) · [对话](previews/home.png) · [转写与回到最新](previews/transcript.png) · [设置](previews/settings.png) · [键盘候选](previews/keyboard.png) · [清空确认](previews/confirm.png) · [相机](previews/camera.png) · [Wi-Fi](previews/wifi.png)

最终离线布局核验：6个页面无横向溢出，标题行高54px；390/900/1200窗口可用；首页实际入口、返回路径和3种可分享状态通过。确认框无内容溢出。已查看8张页面图与总览。临时预览服务已停止，打开本地文件无需服务。浏览器核验任务已结束，保留离线原型入口。