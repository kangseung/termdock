# termdock

> **看不懂就先看这几句：**
>
> 这是一个**纯娱乐、纯自用、完全散养**的玩具项目。作者没时间管它，也不打算管。
> **不为任何问题负责** —— 包括但不限于：数据丢失、连错机器、密码泄露、主机指纹被绕过、
> 把生产环境 rm -rf、内存越界、崩溃、卡死、把桌面搞崩。
> **自己用之前请先备份，重要机器请用成熟终端（Konsole / Windows Terminal / iTerm2 / kitty）。**
>
> 本项目是 **DeepSeek-V4.1-Flash 自动化生成的**（从第一行代码到最后一次提交基本都由它搓出来），
> 作者只负责提需求和点"继续"。**不接受 issue、不接 PR、不做安全审计、不保证兼容性**，
> 哪天不想要了可能直接删库。当它是"能跑的一次性玩具"就好。
>
> 灵感与参考（都只是借鉴思路/配色，没有复制代码）：
> **Konsole**（终端配色与交互习惯）、**QDarkStyleSheet**（深色皮肤的整体调性）、
> **Kuddev/pebrel**（工作区布局的视觉参考）、**Qt 6 / libssh2 / OpenSSL**（底层依赖）。

---

一个**只做三件事**的跨平台终端：

1. 好用好看的终端窗口（QPainter 自绘，不是网页套壳，输入零延迟）
2. 双击即连的会话簿（本地 JSON，密码加密保存，不用再记 IP / 账号 / 密码）
3. 自带的 SFTP 文件树（右键菜单新建文件/文件夹、上传下载、重命名、递归删除）

技术栈：**C++17 / Qt 6 Widgets** + **libssh2**（SSH / SFTP）+ **OpenSSL**（加解密）。
在 LMDE 7（Debian 13）+ KDE Wayland（125% 分数缩放）上验证过，X11 也能跑。

---

## 功能

### 终端

- 自研 VT100 / xterm 子集解析器 + 自绘渲染：
  - 256 色 / 真彩（`38;5;n`、`38;2;r;g;b`）、粗体/斜体/下划线/删除线/反显
  - 备用屏（vim、htop、less 正常）、滚回（默认 8000 行）、自动换行、插入/删除行列
  - 中文宽字符按两格对齐，逐格精确定位绘制；坐标吸附设备像素，
    **125% / 150% 分数缩放下不会出现半像素重影、错位、卡贴图**
  - 选区复制、括号粘贴（bracketed paste）、鼠标上报（1000/1002/1003 + SGR 1006，
    按住 Shift 走本地选区）、自绘滚动条（滚轮、触控板像素滚动、滚回位置提示）
  - **fcitx5 / ibus 中文输入可用**：程序启动时会把系统的 Qt 插件目录补进
    `QT_PLUGIN_PATH`（官方安装版 Qt 默认只扫自己那份），预编辑串就地显示
- 快捷键：
  - `Ctrl+Shift+C` / `Ctrl+Shift+V` 复制 / 粘贴（`Ctrl+C` 仍是 SIGINT）
  - `Ctrl+N` 新建会话；`Ctrl+W` 关闭当前标签（**会弹确认框**）
  - `Ctrl+Shift+K` 打开「安全密钥与已保存的密码」
  - `Ctrl+=` / `Ctrl+-` / `Ctrl+0` 放大 / 缩小 / 重置字号；`Ctrl+滚轮` 同样可调
- 关闭任何东西都要确认：关标签页、关窗口都会先问一句，误点不会直接断线退出

### 会话簿

- 布局默认是「**左：文件树 / 中：终端 / 右：会话簿**」，分隔条位置会被记住
- 会话卡片：颜色标记 + 名称 + 账号@主机，支持搜索、编辑、复制、删除
- **双击直接连上**，键盘焦点自动交给终端，连上就能打字

### SFTP 文件树

- 独立连接、独立线程，大文件传输不卡终端
- 懒加载目录树，显示大小 / 修改时间 / 权限
- 顶部只有「刷新 / 上传」，其余全在**右键菜单**：
  - 空白处：新建文件夹、新建文件、上传文件、刷新
  - 文件上：打开（下载后预览）、下载、重命名、删除、复制远程路径
  - 文件夹上：展开/收起、递归下载整个文件夹、重命名、递归删除、复制远程路径
  - 结构变化后自动刷新当前目录

### 界面皮肤

整套 QSS 按 **QDarkStyleSheet** 的深色调性重写过（底色 `#19232D`、控件 `#29343D`、
强调色 `#148CD2` / `#00BCD4`），标签页与终端区连成一片、滚动条细窄、
关闭叉是自己画的 12px 小图标（悬停变红）。想换肤只改 `src/app/Theme.cpp`。

---

## 首次启动

第一次运行会弹窗让你**选择数据保存目录**：

- 默认放在**软件自身目录**下的 `termdock-data/`，整个软件目录可以拷到 U 盘或另一台机器继续用
- **不会**把数据写进临时目录、开发构建目录或 `bin/` 目录（那些位置重启 / 重新构建就没了）：
  遇到这些位置或目录不可写，自动回退到 `~/.config/termdock`；遗留的临时目录数据会被自动搬回来
- `--data-dir <路径>` 只是**本次运行**的临时覆盖，不写配置、不改默认；`/tmp` 之类会被直接拒绝

同时会问一句要不要留一把**安全密钥**（见下文），可以跳过。

---

## 密码是怎么存的

| 文件 | 内容 |
| --- | --- |
| `<数据目录>/sessions.json` | 会话列表，密码字段形如 `gcm1:<base64(iv\|密文\|tag)>`（0600） |
| `<数据目录>/key` | 32 字节主密钥（0600），AES-256-GCM 的实际密钥 |
| `<数据目录>/recovery.json` | 可选。用安全密钥经 PBKDF2-SHA256（21 万次）派生的 KEK 包住主密钥 |
| `<数据目录>/known_hosts.json` | 主机指纹（TOFU），指纹变化会**直接中止连接** |

要点：

- 日常连接**不需要**输口令，主密钥自动从 `key` 文件读
- 会话文件里没有明文密码，`grep` 不到，云同步也不至于直接泄露
- 主密钥和密文同机存放，属于「防手滑」级别，**不等价于系统钥匙串**
- 这几类文件都在 `.gitignore` 里，不会进仓库

### 安全密钥（可选，可设可删）

设置后记牢这把密钥，就能在「文件 → 安全密钥与已保存的密码…」(`Ctrl+Shift+K`) 里：

- 用密钥解锁，看/复制当时保存的 SSH 账号密码（默认遮罩，勾选后显示明文，只在内存里，不写盘）
- 即使 `key` 文件丢了或换了机器，也能把历史密码解出来

删除就是移除 `recovery.json`，随时可以重设。密钥错了会被 GCM 校验直接拒绝。

---

## 构建与安装

依赖：`cmake >= 3.16`、C++17 编译器、Qt 6 Widgets、libssh2、OpenSSL。

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

Wayland 原生需要 `qtwayland` 模块；中文输入需要系统的 `fcitx5-frontend-qt6`（程序会自动找到它）。

### 一键安装（不需要 sudo）

```bash
./install.sh              # 装到 ~/.local：二进制、快捷方式、图标一次到位
./install.sh --system     # 装到 /usr/local（需要 sudo）
./install.sh --uninstall  # 卸载（同样支持 --system / --prefix）
./install.sh --dry-run    # 先看看会做什么，不动手
```

脚本会自动找可执行文件（`build/termdock` → 当前目录 → `PATH`），也可以用 `--bin <路径>` 指定；
生成的 `termdock.desktop` 里 `Exec` 写的是绝对路径，`~/.local/bin` 不在 `PATH` 也能从菜单启动。

也可以走 CMake 安装（系统级，需要 sudo）：

```bash
sudo cmake --install build --prefix /usr/local
```

---

## 命令行（自测用）

```bash
# 终端内核 + 加解密 + 安全密钥 + 输入法提交路径自检（60 项断言，不需要网络）
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
上传 → 下载校验 → 删除 → 建目录 → 传两个文件 → 递归下载目录并校验内容 → 递归删除目录。

---

## 目录结构

```text
assets/icons/       应用图标（多尺寸 PNG + 标签关闭叉 + 复选框勾）+ packaging/termdock.ico
packaging/          termdock.desktop、Windows 用的 ico
install.sh          一键安装 / 卸载脚本
src/app/            AppConfig（数据目录解析）、Theme（终端 16 色 + 整套 QSS 皮肤）
src/term/           VtScreen（屏幕/滚回/宽字符）、VtParser（VT 状态机）、
                    TerminalWidget（自绘 + 键鼠 + 滚动条 + 输入法）
src/ssh/            SshConnection（socket/握手/认证/指纹）、ShellSession（PTY 线程）、
                    SftpWorker（SFTP 线程，含递归下载/删除）、SftpModel（懒加载模型）
src/session/        Session / SessionStore（JSON + 主机指纹）、Crypto（AES-256-GCM + PBKDF2）、
                    Recovery（可选的安全密钥）
src/ui/             MainWindow、SessionPanel、SessionDialog、FileTreePanel、DataDirDialog、VaultDialog
```

---

## 已知限制

- 不做 reflow：窗口变大只在下方加空白行、变小从底部裁剪（有内容的裁掉行进滚回），
  已有输出的换行位置不会跟着宽度重排；但**变窄不会丢弃行尾字符**，再变宽能原样接上
  （"缩小再放大后提示符重复、错位"和"右侧被截断"这两个坑都已修）
- 连接会在布局完成后才建立 PTY，保证远端按真实列宽生成 MOTD / 欢迎信息，不会一登录就缺右半截
- 组合附加符号（如 é 的分音符）不叠加渲染；文件名里的复杂 emoji 宽度可能偏一格
- 鼠标上报不含 1005（UTF-8 坐标）/ 1015（urxvt）两种旧编码
- Windows 的本地 shell（ConPTY）未适配；SSH 部分依赖 libssh2，理论上可用
- **没有自动更新，没有崩溃上报，没有测试矩阵**；它就是个玩具

---

## 免责声明

1. 本项目是 **DeepSeek-V4.1-Flash 自动化生成**的练手作品，**纯娱乐、纯自用**，作者**散养、不维护**。
2. 作者**不对软件的质量、稳定性、安全性、正确性、合法性做任何担保**，
   也不为使用本软件造成的任何直接或间接损失（数据丢失、命令误执行、密码泄露、
   主机被入侵、业务中断等）承担责任。
3. 密码加密只是**防手滑**级别的本地保护，不等价于系统钥匙串 / 硬件令牌；
   涉及生产环境、重要凭据时，请使用成熟工具并自行做安全评估。
4. 代码里没有任何真实凭据；自测凭据一律通过环境变量传入。
   你要往里塞自己的密码，风险自担。
5. **不提供支持**：不提 issue 也会被无视，提了也一样；能用就用，不能用就删。

如果你需要一个真正靠谱的终端，请去用 **Konsole / GNOME Terminal / Windows Terminal /
Alacritty / kitty / WezTerm / iTerm2**。本项目只是某人某天想让"双击就连上，还带个文件树"。

## 许可

Apache-2.0，见 `LICENSE`。
