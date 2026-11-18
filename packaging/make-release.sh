#!/usr/bin/env bash
# 生成 Linux 发布物（放进 dist/）：
#   termdock-<ver>-linux-x86_64.tar.gz   便携版：解压即用，自带 Qt 库，不依赖系统 Qt
#   termdock-<ver>-linux-x86_64.zip      同上（给习惯 zip 的人）
#   termdock_<ver>_amd64.deb             系统安装包：体积小，Qt 用系统的
#   SHA256SUMS / MD5SUMS + RELEASE_NOTES.md
#
# 用法：./packaging/make-release.sh [--no-build]
set -euo pipefail

APP=termdock
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
QT_DIR="${QT_DIR:-$HOME/Qt/6.8.2/gcc_64}"
DIST="${DIST:-$ROOT/dist}"
BUILD="${BUILD:-$ROOT/build-dist}"
ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"

info() { printf '%s\n' "$*"; }
die() { printf '错误：%s\n' "$*" >&2; exit 1; }

NO_BUILD=0
for arg in "$@"; do
    case "$arg" in
        --no-build) NO_BUILD=1 ;;
        -h | --help) sed -n '2,10p' "$0"; exit 0 ;;
        *) die "未知参数：$arg" ;;
    esac
done

VER="$(sed -n 's/^project(termdock VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
[ -n "$VER" ] || die "读不到 CMakeLists.txt 里的版本号"
[ -d "$QT_DIR" ] || die "找不到 Qt：$QT_DIR（用 QT_DIR=... 指定）"

NAME="$APP-$VER-linux-x86_64"
STAGE="$DIST/$NAME"

info "== 版本 $VER / 架构 $ARCH =="

if [ "$NO_BUILD" -eq 0 ]; then
    info "== 构建 Release（RUNPATH 只保留 \$ORIGIN/lib）=="
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$QT_DIR" \
        -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON >/dev/null
    cmake --build "$BUILD" -j"$(nproc)" >/dev/null
fi
[ -x "$BUILD/$APP" ] || die "没有找到 $BUILD/$APP"

rm -rf "$STAGE"
mkdir -p "$STAGE/lib"

# ---------- 1. 便携版 ----------
info "== 组装便携目录 =="
cp "$BUILD/$APP" "$STAGE/$APP"
chmod 0755 "$STAGE/$APP"

# 插件：平台/图片/图标引擎/xcb 集成/输入法（wayland 有就一起带）
for plugin_dir in platforms imageformats iconengines xcbglintegrations \
    platforminputcontexts wayland-decoration-client wayland-graphics-integration-client \
    wayland-shell-integration; do
    if [ -d "$QT_DIR/plugins/$plugin_dir" ]; then
        cp -r "$QT_DIR/plugins/$plugin_dir" "$STAGE/lib/"
    fi
done
# 精简掉用不上的平台插件（eglfs/linuxfb/vnc 之类留在里面也能用，但没必要）
rm -f "$STAGE/lib/platforms/libqeglfs.so" "$STAGE/lib/platforms/libqlinuxfb.so" \
    "$STAGE/lib/platforms/libqvnc.so" "$STAGE/lib/platforms/libqminimalegl.so" \
    "$STAGE/lib/platforms/libqvkkhrdisplay.so" 2>/dev/null || true

is_bundled_lib() {
    case "$1" in
        "$QT_DIR"/lib/*) return 0 ;;
        */libicu* | */libssh2.so* | */libssl.so* | */libcrypto.so*) return 0 ;;
    esac
    return 1
}

deps_of() {
    # 必须让 ldd 先看到 Qt 安装目录：否则它会把系统的 Qt 当成依赖，
    # 结果一个 Qt 库都打不进便携包里
    LD_LIBRARY_PATH="$QT_DIR/lib" ldd "$1" 2>/dev/null | awk '/=> \//{print $3}'
}

# 从可执行文件和每个插件出发，把 Qt/ICU/libssh2/OpenSSL 的依赖闭包拷进 lib/
declare -A seen=()
queue=("$STAGE/$APP")
while IFS= read -r -d '' plugin; do
    queue+=("$plugin")
done < <(find "$STAGE/lib" -name '*.so' -print0)

while [ ${#queue[@]} -gt 0 ]; do
    current="${queue[0]}"
    queue=("${queue[@]:1}")
    while IFS= read -r lib; do
        [ -n "$lib" ] || continue
        [ -n "${seen[$lib]:-}" ] && continue
        seen[$lib]=1
        if is_bundled_lib "$lib"; then
            cp -L "$lib" "$STAGE/lib/"
            queue+=("$lib")
        fi
    done < <(deps_of "$current")
done

# Qt 用 qt.conf 找插件：插件就放在 lib/ 下
cat >"$STAGE/qt.conf" <<'EOF'
[Paths]
Prefix = .
Plugins = lib
EOF

# 桌面文件 + 图标 + 安装脚本（便携包里也能 ./install.sh 装进系统）
mkdir -p "$STAGE/share/applications" "$STAGE/assets/icons" "$STAGE/packaging"
cp "$ROOT/packaging/termdock.desktop" "$STAGE/packaging/"
cp "$ROOT/assets/icons/termdock-16.png" "$ROOT/assets/icons/termdock-32.png" \
    "$ROOT/assets/icons/termdock-64.png" "$ROOT/assets/icons/termdock-256.png" \
    "$STAGE/assets/icons/"
cp "$ROOT/install.sh" "$STAGE/"
chmod 0755 "$STAGE/install.sh"

cat >"$STAGE/README.txt" <<EOF
termdock $VER —— 便携版（自带 Qt 运行库，不需要装 Qt）

直接运行：
    ./termdock

装进系统（应用菜单里出现 termdock，可选）：
    ./install.sh --bin ./termdock

依赖：glibc 2.34+ 的 x86_64 Linux（Debian 12 / Ubuntu 22.04 及更新版本），
      需要图形环境（X11 或 Wayland+XWayland）。

命令行直连（密码只从环境变量读）：
    TERMDOCK_PASSWORD=*** ./termdock --connect <host> --user <user> --data-dir /tmp/td
EOF

# 冒烟测试：在干净环境里跑自检，确认库和插件都从 lib/ 来
info "== 便携包自检 =="
( cd "$STAGE" && env -i HOME=/tmp PATH=/usr/bin:/bin QT_QPA_PLATFORM=offscreen \
    ./termdock --selftest >/tmp/td-selftest.log 2>&1 ) || {
    tail -5 /tmp/td-selftest.log >&2
    die "便携包自检失败"
}
tail -1 /tmp/td-selftest.log

info "== 打包 =="
tar -C "$DIST" -czf "$DIST/$NAME.tar.gz" "$NAME"
if command -v zip >/dev/null 2>&1; then
    ( cd "$DIST" && rm -f "$NAME.zip" && zip -qr "$NAME.zip" "$NAME" )
fi

# ---------- 2. deb ----------
info "== 生成 deb =="
PKG="$DIST/pkgroot-$ARCH"
rm -rf "$PKG"
mkdir -p "$PKG/DEBIAN" "$PKG/usr/bin" "$PKG/usr/share/applications"
for size in 16 32 64 256; do
    mkdir -p "$PKG/usr/share/icons/hicolor/${size}x${size}/apps"
    install -m 0644 "$ROOT/assets/icons/termdock-$size.png" \
        "$PKG/usr/share/icons/hicolor/${size}x${size}/apps/termdock.png"
done
install -m 0755 "$BUILD/$APP" "$PKG/usr/bin/termdock"
install -m 0644 "$ROOT/packaging/termdock.desktop" "$PKG/usr/share/applications/termdock.desktop"
installed_kb="$(du -sk --exclude=DEBIAN "$PKG" | cut -f1)"
cat >"$PKG/DEBIAN/control" <<EOF
Package: termdock
Version: $VER
Section: x11
Priority: optional
Architecture: $ARCH
Depends: libc6 (>= 2.34), libstdc++6, libqt6core6, libqt6gui6, libqt6widgets6, libssh2-1, libssl3 | libssl3t64
Recommends: libxcb-cursor0
Installed-Size: $installed_kb
Maintainer: kangseung <kangseung@users.noreply.github.com>
Homepage: https://github.com/kangseung/termdock
Description: Qt terminal with session book and SFTP file tree
 终端 + 双击直连的会话簿 + SFTP 文件树。SFTP 支持拖拽上传、上传下载进度与
 剩余时间、目录懒加载；密码 AES-256-GCM 加密落盘，主机指纹首次连接记录。
EOF
if command -v dpkg-deb >/dev/null 2>&1; then
    dpkg-deb --root-owner-group -Zxz --build "$PKG" \
        "$DIST/termdock_${VER}_${ARCH}.deb" >/dev/null
    info "  $(basename "$DIST/termdock_${VER}_${ARCH}.deb")"
else
    info "  跳过 deb：没有 dpkg-deb"
fi
rm -rf "$PKG"

# ---------- 3. 校验和 + Release 说明 ----------
info "== 校验和 =="
(
    cd "$DIST"
    rm -f SHA256SUMS MD5SUMS
    for f in "$NAME.tar.gz" "$NAME.zip" "termdock_${VER}_${ARCH}.deb"; do
        [ -f "$f" ] && sha256sum "$f" >>SHA256SUMS
    done
    for f in "$NAME.tar.gz" "$NAME.zip" "termdock_${VER}_${ARCH}.deb"; do
        [ -f "$f" ] && md5sum "$f" >>MD5SUMS
    done
    cat SHA256SUMS
)

gen_notes() {
    local tar_size deb_size
    tar_size="$(du -h "$DIST/$NAME.tar.gz" | cut -f1)"
    deb_size="$(du -h "$DIST/termdock_${VER}_${ARCH}.deb" 2>/dev/null | cut -f1 || echo '-')"
    cat >"$DIST/RELEASE_NOTES.md" <<EOF
一份“够用就好”的 SSH 终端：终端窗口 + 双击直连的会话簿 + 自带 SFTP 文件树。
C++17 / Qt 6 Widgets / libssh2 / OpenSSL，密码加密落盘，主机指纹 TOFU 校验。

## 这次有什么

- **终端**：自绘渲染，256 色 / 真彩、备用屏、8000 行回滚、中文宽字符按两格对齐，
  125% / 150% 分数缩放下不糊
- **会话簿**：双击即连，同一个会话可以重复双击开多个终端（多个独立 SSH 连接），
  密码 AES-256-GCM 加密保存，可选“安全密钥”找回
- **SFTP 文件树**：懒加载目录（展开一层加载一层）、拖拽上传（支持整个文件夹）、
  上传下载显示速度和剩余时间、覆盖前确认、右键新建/重命名/递归删除
- **标签页**：右键标签可直接编辑对应的会话；关闭标签自动断开连接

## 使用方式

| 文件 | 架构 | 适用系统 |
| --- | --- | --- |
| termdock-$VER-linux-x86_64.tar.gz | x86_64 | 通用 Linux（glibc 2.34+，自带 Qt，解压即用） |
| termdock_$VER\_$ARCH.deb | $ARCH | Debian 12+ / Ubuntu 22.04+ / Linux Mint（体积小，用系统 Qt） |

**便携版**（推荐，下载即用）：

\`\`\`bash
tar xzf termdock-$VER-linux-x86_64.tar.gz
cd termdock-$VER-linux-x86_64
./termdock
\`\`\`

想让它出现在应用菜单里：\`./install.sh --bin ./termdock\`

**deb 包**：

\`\`\`bash
sudo apt install ./termdock_$VER\_$ARCH.deb
termdock
\`\`\`

**从源码构建**：

\`\`\`bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j\$(nproc)
\`\`\`

## 已知限制

- 只提供 Linux x86_64 构建；Windows / macOS 没打（代码本身没平台相关依赖，但没验证）
- 主机密钥校验是 TOFU（首次信任并记录指纹，之后不一致就拒绝连接）
- 不做 SSH 密钥登录、端口转发、ZMODEM 这些高级功能

## 校验和

\`\`\`text
$(cd "$DIST" && cat SHA256SUMS)
\`\`\`

md5 见仓库 \`dist/MD5SUMS\`（发布附件里的 SHA256SUMS 同此）。
体积：便携版 $tar_size，deb $deb_size
EOF
}
gen_notes
info "== Release 说明 =="
info "  $DIST/RELEASE_NOTES.md"

info ""
info "完成，产物在 $DIST ："
ls -lh "$DIST" | sed 's/^/  /'
