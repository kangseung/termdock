# termdock

一个**只做三件事**的跨平台终端：

1. 好用好看的终端窗口（QPainter 自绘，不是网页套壳，输入零延迟）
2. 双击即连的会话簿（本地 JSON，密码加密保存，不用再记 IP / 账号 / 密码）
3. 自带的 SFTP 文件树（右键菜单新建文件/文件夹、上传下载、重命名、递归删除）

技术栈：**C++17 / Qt 6 Widgets** + **libssh2**（SSH / SFTP）+ **OpenSSL**（加解密）。

已在 LMDE 7（Debian 13）上验证：X11 与 Wayland（Qt 原生 wayland 插件）都能跑。

---

## 功能

### 终端

- 自研 VT100 / xterm 子集解析器 + 自绘渲染：
  - 256 色 / 真彩（`38;5;n`、`38;2;r;g;b`）、粗体/斜体/下划线/删除线/反显
  - 备用屏（vim、htop、less 正常）、滚回（默认 8000 行）、自动换行、插入/删除行列
  - 中文宽字符按两格对齐，逐格精确定位绘制（长行不漂移、不错位）
  - 选区复制、括号粘贴（bracketed paste）、输入法预编辑显示
  - 鼠标上报（xterm 1000 / 1002 / 1003 + SGR 1006），**按住 Shift 强制走本地选区**
  - 自绘滚动条：鼠标拖动、滚轮（含触控板像素级滚动）、滚回位置提示、顶部渐隐阴影
- 快捷键：
  - `Ctrl+Shift+C` / `Ctrl+Shift+V` 复制 / 粘贴（`Ctrl+C` 仍然是发送 SIGINT）
  - `Ctrl+Shift+K` 打开「已保存的密码」；`Ctrl+N` 新建会话；`Ctrl+W` 关闭标签
  - `Ctrl+=` / `Ctrl+-` / `Ctrl+0` 放大 / 缩小 / 重置字号；`Ctrl+滚轮` 也可调字号

### 会话簿

- 布局默认是「左：文件树 / 中：终端 / 右：会话簿」，分隔条位置会被记住
- 会话列表：自绘卡片（颜色标记 + 名称 + 账号@主机），支持搜索、编辑、复制、删除
- **双击直接连上**，键盘焦点自动交给终端，连上就能打字
- 每个会话保存：名称、主机、端口、账号、密码、备注、标记色

### SFTP 文件树

- 独立连接、独立线程，大文件传输不会卡住终端
- 懒加载目录树，显示大小 / 修改时间 / 权限
- 顶部只留「刷新 / 上传」两个按钮，其余全部走**右键菜单**：
  - 空白处：新建文件夹、新建文件、上传文件、刷新
  - 文件上：打开（下载后预览）、下载、重命名、删除、复制远程路径
  - 文件夹上：展开/收起、下载整个文件夹（递归）、重命名、删除（递归）、复制远程路径
  - 结构变化后自动刷新当前目录

---

## 首次启动

第一次运行会弹窗让你**选择数据保存目录**：

- 默认放在**软件自身目录**下的 `termdock-data/`，这样整个软件目录可以拷到 U 盘或另一台机器继续用
- **不会**把数据写进临时目录或开发构建目录（重启 / 重新构建就没了）：如果软件目录属于这两类，
  或者不可写（例如装在 `/usr/bin`），自动回退到 `~/.config/termdock`；已存在的临时目录数据会被自动搬回来
- 如果旧位置 `~/.config/termdock` 里有历史会话，会问你要不要导入
- `--data-dir <路径>` 只是**本次运行**的临时覆盖，不写进配置、也不改默认；`/tmp` 这类临时路径会被直接拒绝

同时会问一句要不要留一把**安全密钥**（见下文），可以跳过，以后随时在菜单里设置或删除。

---

## 密码是怎么存的

| 文件 | 内容 |
| --- | --- |
| `<数据目录>/sessions.json` | 会话列表，密码字段形如 `gcm1:<base64(iv\|密文\|tag)>`（0600） |
| `<数据目录>/key` | 32 字节主密钥（0600）。AES-256-GCM 的实际密钥 |
| `<数据目录>/recovery.json` | 可选。用安全密钥经 PBKDF2-SHA256（21 万次）派生的 KEK 包住主密钥；与 `key` 同目录、同样 0600 |
| `<数据目录>/known_hosts.json` | 主机指纹（TOFU），指纹变化会**直接中止连接**并提示 |

要点：

- 日常连接**不需要**输口令，主密钥自动从 `key` 文件读
- 会话文件里没有明文密码，`grep` 不到、云同步也不至于直接泄露
- 主密钥和密文同机存放，属于「防手滑」级别，不等价于系统钥匙串；要更强保护请接 libsecret / Keychain

### 安全密钥（可选，可设可删）

设置后记牢这把安全密钥，就能在「文件 → 安全密钥与已保存的密码…」(`Ctrl+Shift+K`) 里：

- 用安全密钥解锁，表格里看/复制当时保存的 SSH 账号密码（默认遮罩，勾选后显示明文，只在内存里，不写盘）
- 即使 `key` 文件丢了或换了机器，也能把历史密码解出来

删除就是移除 `recovery.json`，随时可以重设。密钥错了会被 GCM 校验直接拒绝。

---

## 构建

依赖：`cmake >= 3.16`、C++17 编译器、Qt 6 Widgets、libssh2、OpenSSL。

Debian / Ubuntu：

```bash
sudo apt install build-essential cmake qt6-base-dev libssh2-1-dev libssl-dev
```

若 Qt 用官方安装器装在别处：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.2/gcc_64
cmake --build build -j$(nproc)
./build/termdock
```

Wayland 原生需要 `qtwayland` 模块（Qt 安装器里勾选，或 `apt install qt6-wayland`）。
安装到系统（含桌面图标与程序图标）：

```bash
sudo cmake --install build --prefix /usr/local
```

---

## 命令行（自测用）

```bash
# 终端内核 + 加解密 + 恢复保险自检（58 项断言，不需要网络）
./build/termdock --selftest

# 真实 SSH + SFTP 连通性测试（密码只从环境变量读，不落盘、不进仓库）
TERMDOCK_PASSWORD='***' ./build/termdock --connect-test 192.0.2.10 --user user

# 启动即连某台机器
TERMDOCK_PASSWORD='***' ./build/termdock --connect 192.0.2.10 --user user

# 指定数据目录 / 无头截图（调试渲染用）
./build/termdock --data-dir /tmp/td --connect 192.0.2.10 --user user \
                 --screenshot /tmp/shot.png --screenshot-delay 8000
```

`--connect-test` 会依次验证：密码认证 → 远端命令回显 → PTY 尺寸 → SFTP 初始化 → 列目录 →
上传 → 下载校验 → 删除 → 建目录 → 传两个文件 → **递归下载目录并校验内容** → **递归删除目录**。

---

## 目录结构

```text
assets/icons/       应用图标（PNG 多尺寸 + packaging/termdock.ico）
packaging/          termdock.desktop、Windows 用的 ico
src/app/            AppConfig（数据目录解析）、Theme（终端 16 色 + QSS 换肤）
src/term/           VtScreen（屏幕/滚回/宽字符）、VtParser（VT 状态机）、TerminalWidget（自绘 + 键鼠 + 滚动条）
src/ssh/            SshConnection（socket + 握手 + 认证 + 指纹）、ShellSession（PTY 线程）、
                    SftpWorker（SFTP 线程，含递归下载/删除）、SftpModel（懒加载模型）
src/session/        Session / SessionStore（JSON + 主机指纹）、Crypto（AES-256-GCM + PBKDF2）、
                    Recovery（可选的密码恢复保险）
src/ui/             MainWindow、SessionPanel、SessionDialog、FileTreePanel、DataDirDialog、VaultDialog
```

---

## 已知限制

- 窗口变窄时不重排（reflow）已有输出，只按当前宽度续写；这是为了滚动位置稳定
- 组合附加符号（如 é 的分音符）不叠加渲染；文件名里的复杂 emoji 宽度可能偏一格
- 鼠标上报不包含 1005（UTF-8 坐标）/ 1015（urxvt）两种旧编码
- Windows 的本地 shell（ConPTY）尚未适配；SSH 部分依赖 libssh2，理论上可用

## 许可

Apache-2.0，见 `LICENSE`。
