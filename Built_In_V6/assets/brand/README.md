# 灵译通品牌资源

2026-11-02用户指定：首页使用原生图标，开机不用Logo，使用青禾9×9点阵。

## 当前固件

- native-user-icon.png：用户提供的02_lingyitong_app_icon.png原样复制，SHA256为78bf2bf8679271fcc10114ba1a3267c48a125e72a37552a4508f57f686e6ad81。双手、文字、边框和透明边缘未重绘。
- native-icon-64.png：仅Lanczos缩小为64×64，供首页及关于页使用。
- qinghe_logo.c：RGB565A8色彩平面+alpha平面，共12288像素字节，无运行时图片解码；tools/Generate-QingheLogo.py为离线Pillow生成工具，未改固件依赖。
- 开机点阵为qinghe_ui.c内11字节图案和单绘制对象，无Logo图片。

## 此前资源

original-user-logo.png、logo-mark.svg、logo-lockup.svg/.png及logo-48.png/logo-144.png是本轮先前重绘和核验资源，保留历史，但最终固件不使用。重绘未获得用户采用，不声称其比原生图更清晰或还原了不可见细节。
