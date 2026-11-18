# termdock

一个**只做两件事**的跨平台终端：好用的终端窗口 + 双击即连的会话簿 + 自带的 SFTP 文件树。

- 技术栈：**C++17 / Qt 6 Widgets**（不是网页技术，终端是 QPainter 自绘的，输入无延迟）
- SSH / SFTP：libssh2
- 密码本地加密：OpenSSL AES-256-GCM
- 目标平台：Linux（已在 LMDE 7 / Debian 13 + X11/Wayland 下验证）、macOS；Windows 需要自己补 ConPTY 层

## 功能

1. **终端 + 会话簿**
   - 自研 VT100/xterm 子集解析器 + 自绘渲染：256 色 / 真彩、粗体斜体下划线删除线、备用屏（vim/htop 正常）、滚回（默认 8000 行）、选区复制、括号粘贴、中文宽字符对齐
   - 会话保存在本地 `~/.config/termdock/sessions.json`，密码用 AES-256-GCM 加密
   - 双击会话直接连上；支持搜索、编辑、复制、删除，`+ 新建` 填写一次即可长期复用
2. **SFTP 文件树**
   - 独立连接、独立线程，大文件上传下载不会卡住终端
   - 懒加载目录树、上传 / 下载 / 新建目录 / 重命名 / 删除 / 双击文件下载并打开

## 构建

依赖：`cmake >= 3.16`、C++17 编译器、Qt 6 Widgets、libssh2、OpenSSL。

Debian/Ubuntu：

```bash
sudo apt install build-essential cmake qt6-base-dev libssh2-1-dev libssl-dev
```

若 Qt 不在系统路径（例如用 Qt 官方安装器装在 `~/Qt/6.8.2/gcc_64`）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.2/gcc_64
cmake --build build -j$(nproc)
./build/termdock
```

## 使用

- 左侧「+ 新建」填写名称 / 主机 / 端口 / 账号 / 密码 → 保存
- **双击**左侧会话即可连接；顶部标签页可关闭、可拖动排序
- 右侧文件树随当前标签页切换；「刷新」重新列目录
- 快捷键：
  - `Ctrl+Shift+C` / `Ctrl+Shift+V` 复制 / 粘贴（`Ctrl+C` 仍然是发送 SIGINT）
  - `Ctrl+N` 新建会话，`Ctrl+W` 关闭标签，`Ctrl+Q` 退出
  - `Ctrl+=` / `Ctrl+-` / `Ctrl+0` 放大 / 缩小 / 重置字号
  - `Ctrl+滚轮` 调字号，滚轮滚动回看，鼠标中键粘贴选区

## 命令行（主要用于自测）

```bash
# 终端内核自检（41 项断言，不需要网络）
./build/termdock --selftest

# 真实 SSH + SFTP 连通性测试（密码从环境变量读，不落盘、不进仓库）
TERMDOCK_PASSWORD='***' ./build/termdock --connect-test 192.0.2.10 --user user

# 启动即连某台机器（密码同样走环境变量）
TERMDOCK_PASSWORD='***' ./build/termdock --connect 192.0.2.10 --user user

# 无头截图（调试渲染用）
./build/termdock --connect 192.0.2.10 --user user --screenshot /tmp/shot.png --screenshot-delay 8000
```

`--connect-test` 会依次验证：密码认证 → 远端命令回显 → PTY 尺寸 → SFTP 初始化 → 列目录 → 上传 → 下载并校验内容 → 删除。

## 安全说明

- 会话文件 `~/.config/termdock/sessions.json` 权限 0600，密码字段形如 `gcm1:<base64(iv|ciphertext|tag)>`
- 密钥在 `~/.config/termdock/key`（0600）。**密钥与密文同机存放**，目的是防止明文被随手 `grep` / 误同步，不等价于系统钥匙串。需要更强保护时应接入 libsecret / macOS Keychain，`src/session/Crypto.{h,cpp}` 是唯一改动点
- 主机指纹采用 TOFU：首次连接记录 `SHA256:` 指纹到 `~/.config/termdock/known_hosts.json`，之后指纹变化会**直接中止连接**并提示
- 仓库里不含任何账号密码，自测凭据只通过环境变量传入

## 目录结构

```text
src/
  app/        主题配色（终端 16 色 + QSS 换肤）、配置路径
  term/       VtScreen（屏幕/滚回/宽字符）、VtParser（VT 状态机）、TerminalWidget（自绘 + 键鼠）
  ssh/        SshConnection（socket+握手+认证）、ShellSession（PTY 线程）、
              SftpWorker（SFTP 线程）、SftpModel（懒加载模型）
  session/    Session / SessionStore（JSON + 主机指纹）、Crypto（AES-256-GCM）
  ui/         MainWindow、SessionPanel（会话簿）、SessionDialog、FileTreePanel（文件树）
```

## 已知限制

- 未实现鼠标上报（vim/htop 里的鼠标点选不可用，键盘操作正常）
- 文件名含代理对/复杂 emoji 时宽度可能有 1 格偏差；组合附加符号不叠加渲染
- 「下载」只支持单个文件（整目录递归下载、目录递归删除尚未实现）
- Windows 的 ConPTY 未适配（libssh2 部分跨平台可用，本地 shell 需要额外实现）

## 许可

Apache-2.0，见 `LICENSE`。
