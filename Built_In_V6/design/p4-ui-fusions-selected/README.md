# 灵译通 · 四套融合界面

打开 [四套并排对比](index.html)，或分别进入：

| 参考编号 | 融合方案 | 主要设计 |
| --- | --- | --- |
| 01 | [澄页](01-chengye/index.html) | 暖纸白、钴蓝主操作；首页纵向列表，细线与留白组织正文，用户消息用蓝色侧线识别。 |
| 04 | [青禾](04-qinghe/index.html) | 鼠尾草绿、奶油白；居中收音区、双辅助卡片、宽圆角按钮，正文置于柔和的纸面。 |
| 05 | [蓝桥](05-lanqiao/index.html) | 冰蓝、靛蓝；分层白色面板，双方对话以色面区别，转写使用独立白色阅读区。 |
| 06 | [墨语](06-moyu/index.html) | 纯白、炭黑；大圆形麦克风、对比鲜明的文字和细分隔线，以形状与字重区分操作。 |

用户在本轮制作中要求去掉屏幕下方导航，四套均已移除。首页功能卡片进入页面；内页左上返回首页，右上设置；Wi-Fi 左上返回设置。底部空间留给阅读、收音和输入。没有新增替代底栏或悬浮导航。

## 体验范围

每套包含首页、语音对话、独立转写、相机预览、设置和 Wi-Fi 六页。左侧原型面板用于切页、载入长文和模拟 USB、离线、发送失败；它在设备画布之外，不是正式产品菜单。

- 对话：按住说话、松开发送、等待回复与打断；文字草稿跨页面保留，失败后可重试。
- 转写：底部跟随；上翻保留阅读位置，结果持续变长/变短不强制跳底；“回到最新”仅需时显示在独立栏。录音期间清空禁用。
- 清空：取消保留正文及位置，确认才清空，之后可继续输入和录音；对话框默认聚焦取消，Escape 可取消。
- 输入：嵌入式输入、顶部“拼 / E”、向上候选及翻页；电脑输入不自动展开软键盘，USB 模拟接入收起软键盘。候选仅为固定样例，不是真实拼音算法。
- 相机、屏幕/声音设置和 Wi-Fi 均为本地状态模拟；不申请系统相机或麦克风权限。Wi-Fi 样例密码 `demo1234`，请勿输入真实密码。

打开后无需安装依赖、启动服务或联网。输入仅保存在当前页面内存，刷新恢复虚构样例；不调用云 API，不接触用户聊天记录或账户。

对比页可同时切换首页、对话、转写阅读、输入候选、清空确认和设置。进入单套后可点击设备中的实际入口，也可使用画布之外的原型面板。

单套快速查看：在任一方案入口后加 `?page=home`、`?page=transcript&scenario=reading`、`?page=transcript&scenario=keyboard`、`?page=transcript&scenario=confirm`、`?page=camera` 或 `?page=settings`。`embed=1` 只隐藏电脑评审侧栏，保持设备逻辑画布 600×1024。

## 融合来源与实现

采用用户选择的四种新参考方向，分别重排为本项目的页面与操作流程，未使用第三方原图、插画或图标作为原型页面资源。图标由本地内联 SVG 绘制；没有外部字体、CDN 或依赖升级。四套共用已有原型的本地交互模型；新的首页布局、页面展示和主题样式位于本目录，旧设计与旧参考均保留。

- 01：[Lingua · Translator App UI — sleek.design](https://dribbble.com/shots/27660235-Lingua-Translator-App-UI)，借鉴纸白、单一蓝色重点、上下文本分区。
- 04：[AI Translator App — Imran Jakir / Kites Design](https://dribbble.com/shots/27266355-AI-Translator-App-Voice-Text-Translation-UI)，借鉴绿白配色、语音状态和集中操作。
- 05：[AI Real-Time Translator App — Imran Jakir / Kites Design](https://dribbble.com/shots/27334203-AI-Real-Time-Translator-App-Language-Translation-App-Redesign)，借鉴冰蓝层次、输入/结果独立面板。
- 06：[AI Language Learning & Voice Translator App — Farhan](https://dribbble.com/shots/27809016-AI-Language-Learning-Voice-Translator-App)，借鉴黑白层级与大麦克风主操作。

## 画布与实机边界

逻辑尺寸 600×1024；内页标题和正文 28px / 54px 行高，输入框 64px、候选行至少 62px。首页宣传语属于单独的展示层级。移除底栏后增加正文可用面积；软键盘及候选展开时仍独立保留录音操作空间。

这些是电脑端设计原型。没有修改固件、设备身份、相机链路、CPU/PPA 路径、模式资源交接、字库、网络配置、依赖或分区；没有构建、烧录或使用 COM20。板端基线仍为此前用户验收声音正常的 `1d56520` / `p4x-built-in-v6-004`，本轮没有重新读取板上版本。

浏览器字体不同于 P4 的 SD 字库；原型不能证明旧汉字缺字问题解决，也不能代替触屏、真实键盘、语音、相机和 LVGL 内存/刷新验收。不自动进入固件移植。

## 预览与核验

预览图位于 `previews/`，包括四套首页、对话、转写、输入候选与清空确认。四张总览分别为：

- [首页](previews/home-overview.png)
- [对话](previews/chat-overview.png)
- [转写](previews/transcript-overview.png)
- [输入候选](previews/keyboard-overview.png)

`render-previews.ps1` 使用独立临时 profile 的 Edge 无头渲染本地文件，不访问已登录浏览器会话。交互核验使用 Tabbit 和 `verify-interactions.js`；真实结果在 `browser-verification.json`。图片检查与交互检查分别记录，不以截图代表硬件通过。

本轮检查中发现：快速切到设置等非正文页面时，已移除 reader 的排队滚动事件仍会访问新的正文节点，产生空对象错误。已在本目录原型的回调中绑定原 reader，检查节点仍连接且页面未切换后再处理；没有修改固件中的阅读逻辑。浏览器缩放检查也改为等待布局更新并按可见宽度核验，避免将异步 resize 前的尺寸当成最终结果。
