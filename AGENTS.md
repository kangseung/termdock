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
src/ssh      SshConnection（socket+libssh2）、ShellSession（PTY 线程）、
             SftpWorker（SFTP 操作线程）、SftpModel（懒加载树模型）
src/ui       MainWindow（标签页/布局）、SessionPanel（会话列表）、SessionDialog、
             FileTreePanel（SFTP 文件树 UI）、VaultDialog、DataDirDialog
tests/       unit_tests.cpp（ctest，59+ 断言）
packaging/   桌面文件、发布打包脚本
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
