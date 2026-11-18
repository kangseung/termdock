# AGENTS.md —— termdock 项目约定

> 本文件是项目级补充，与用户级 `~/.agents/AGENTS.md` 一起生效；冲突时以用户当次指令为准。
> 讲的是这个仓库的实际结构、构建方式和踩过的坑，不重复通用工作准则。

## 1. 这是什么

Qt 6 Widgets + libssh2 的 SSH 终端 + 会话簿 + SFTP 文件树。
**安全相关的改动必须谨慎**：密码落盘加密、主机指纹 TOFU、SFTP/Shell 认证顺序都在这里。

## 2. 目录

```text
src/app      AppConfig（数据目录/配置文件）、Theme（QSS + 调色板）
src/session  Session/SessionStore（会话簿 JSON）、Crypto（AES-256-GCM）、Recovery（安全密钥）
src/term     VtScreen/VtParser（VT 解析）、TerminalWidget（QPainter 自绘）
src/ssh      NetCompat（跨平台 socket：Winsock/POSIX）、SshConnection（连接+握手+认证）、
             ShellSession（PTY 线程）、SftpWorker（SFTP 操作线程）、SftpModel（懒加载树模型）
src/ui       MainWindow（标签页/布局）、SessionPanel（会话列表）、SessionDialog、
             FileTreePanel（SFTP 文件树 UI）、VaultDialog、DataDirDialog
tests/       unit_tests.cpp（ctest，81 条断言）
packaging/   Linux 桌面文件 + make-release.sh、Windows 用的 ico + make-release-windows.sh
```

## 3. 构建 / 运行 / 测试 / 安装

```bash
# 本机 Qt 不在系统路径里，必须指定
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.2/gcc_64
cmake --build build -j8
./build/termdock                                  # 启动

cd build && ctest --output-on-failure             # 单元测试
./build/termdock --selftest                       # 内核自检（63 项）

# 发布构建 + 安装到 ~/.local
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.2/gcc_64
cmake --build build-release -j8
./install.sh --bin build-release/termdock
```

命令行直连（密码只从环境变量来，不落文件）：

```bash
TERMDOCK_PASSWORD=*** ./build/termdock --connect <host> --user <user> \
    --data-dir /tmp/tddata            # --data-dir 只影响本次运行
```

调试界面：`Xvfb :99 & DISPLAY=:99 QT_QPA_PLATFORM=xcb ./build/termdock ...`，
配 `xdotool` + `ffmpeg -f x11grab` 截图；注意本机默认是 Wayland，**必须强制 `xcb`** 才能截到图。

### Windows 构建（已完整支持，2026-11 实机验证）

MSYS2 UCRT64 工具链，和 Linux 共用同一套 CMake 工程，源码里没有平台分支
（除 `NetCompat.cpp` 内部与少量字体/快捷键的条件编译）。
已在 Windows 10 x64 上实测：全量编译、81 条单元测试、65 条自检、
对真机跑通 SSH 认证/PTY/SFTP 上传下载删除递归、GUI 连接与持续会话均正常。

```cmd
:: 依赖一次装好（官方源在国内常返回 403，需改 /etc/pacman.d/mirrorlist.*）
winget install --id MSYS2.MSYS2 -e --source winget
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake ^
    mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-qt6-base ^
    mingw-w64-ucrt-x86_64-qt6-tools mingw-w64-ucrt-x86_64-libssh2 ^
    mingw-w64-ucrt-x86_64-openssl zip

set PATH=C:\msys64\ucrt64\bin;C:\msys64\usr\bin;%PATH%
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/msys64/ucrt64
cmake --build build-win
build-win\termdock-tests.exe
build-win\termdock.exe --selftest
```

构建后 `build-win/` 里的 `termdock.exe` **可以直接双击运行**：CMake 的 POST_BUILD
会调 `packaging/deploy-windows-deps.sh` 把 Qt 与整条依赖闭包摆到 exe 旁边。
（早先只搬直接依赖，双击就弹「找不到 Qt6Core.dll」，很容易被误当成程序坏了。）

发布用便携包：`./packaging/make-release-windows.sh`，产物在 `dist/*-windows-x64.zip`。
它复用同一个 `deploy-windows-deps.sh`，所以「build 里能跑」和「包里能跑」走同一条路径；
打包末尾还有依赖完整性门禁，删掉任何一个传递依赖都会直接失败（已用 libjpeg/libicuuc 验证过）。

移植时踩过的坑，改代码时别再踩回去：

1. **socket 必须在 `NetCompat` 里**。Windows 用 `select` 而**不用 `WSAPoll`**——
   WSAPoll 在 Windows 上漏报连接失败，会让"连不上的主机"一直等到超时。
   `WSAStartup` 由 `net::ensureInit()` 的静态守卫负责，全仓库没有别处初始化 Winsock。
2. **`<windows.h>` 不能排在 `libssh2.h` 之前**（`main.cpp` 里就是这么处理的）：
   `libssh2.h` 在 `_WIN32` 下会先带进 `winsock2.h`，顺序反了会撞 winsock 1/2 冲突。
3. **远端路径一律走 `remotepath::`（`SftpTypes.h`）**，绝不能用 `QFileInfo`/`QDir::cleanPath`：
   Windows 上它们把文件名里的 `\` 当分隔符、把 `a:b` 当盘符，于是刷新错目录、上传到错位置。
4. **`add_executable(... WIN32 ...)` 让 GUI 没有控制台**，所以 `main.cpp` 里
   `attachParentConsoleIfNeeded()` 负责在 CLI 模式下附着父控制台；但它**必须**
   在 stdout 已被重定向时原样保留，否则 `> log` 和 CI 一个字节都收不到。
5. `QKeySequence::Quit` 在 Windows 上是空序列，菜单里显示不出退出快捷键，已显式回退到 `Alt+F4`。

## 4. 代码约定

- 注释、界面文案、提交信息用中文；标识符用英文。
- 路径一律走 `AppConfig`（数据目录）或 `Path`/`QStandardPaths`，不硬编码 `/home/...`。
- 不做静默兜底：错误要一路 `emit failed...` 到界面，不要 `try/catch` 吞掉或返回空对象。
- `SftpWorker` 是单线程 FIFO：一次只处理一个请求，**批量操作靠入队顺序**保证（先 mkdir 再 upload）。
- `SftpModel` 是懒加载：`hasChildren` 要对“未加载的目录”返回 true（否则 QTreeView 不画展开箭头），
  空目录靠 `m_emptyDirs` 抑制箭头；探路只准探一层（`probeChildDirs`），不许递归。
- SFTP 请求不要用“当前选中项”当刷新目标，改结构性操作后刷新**父目录**（`m_refreshDirs`）。
- 长任务（上传/下载/批量删除）只发请求，不在 UI 线程里阻塞；进度靠 `progressed` 信号。

## 5. 安全红线

- **绝不入库**：`sessions.json`、`known_hosts.json`、`recovery.json`、`key`、`termdock-data/`、
  任何私钥/证书、`.env`。`.gitignore` 已覆盖，pre-commit 钩子会再拦一次。
- 不在代码、注释、测试、提交信息里写真实主机名 / IP / 账号 / 密码。
  自测凭据只通过环境变量（`TERMDOCK_PASSWORD`）或临时目录传入，用完删掉。
- 指纹校验必须在 `libssh2_userauth_password_ex` **之前**（`SshConnection::connectAndAuth`），别改顺序。
- 密码只以密文形式落盘（`Crypto`），明文只在内存里存在。

钩子启用（每台新机器克隆后做一次）：

```bash
git config core.hooksPath .githooks
```

## 6. 提交与发布

- 一次提交一个逻辑变更，信息写清“为什么改 + 怎么验证”，不要混入无关格式化。
- 提交前自查：`git status` / `git diff --stat`，确认没有临时脚本、截图、测试数据、凭据。
- 发布：打 `v*` 标签推上去，`.github/workflows/release.yml` 会自动跑 ctest、执行
  `packaging/make-release.sh` 生成便携包 + deb + 校验和，并发布 Release；
  发布后脚本会删掉除最新一个以外的旧 Release（连同 tag），想多留几个就改 `keep=1`。
  需要在本机手动发时用 `packaging/publish-release.sh <tag>`（要先 `gh auth login`）。
- 两个远端：`origin`（gitee，主力）、`github`（开源镜像）。推送两个都推。
