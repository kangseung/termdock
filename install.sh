#!/usr/bin/env bash
# termdock 安装脚本：装二进制 + 桌面图标 + 快捷方式，自动定位可执行文件。
#
#   ./install.sh                    # 装到 ~/.local（不需要 sudo）
#   ./install.sh --system           # 装到 /usr/local（需要 sudo）
#   ./install.sh --prefix /opt/x    # 自定义前缀
#   ./install.sh --bin /path/termdock
#   ./install.sh --dry-run          # 只打印将执行的动作
#   ./install.sh --uninstall        # 卸载（同样支持 --system/--prefix）
set -euo pipefail

APP="termdock"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MODE="user"
ACTION="install"
PREFIX=""
BIN=""
DRY_RUN=0

die() {
    printf '错误：%s\n' "$*" >&2
    exit 1
}

info() {
    printf '%s\n' "$*"
}

run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '[dry-run] %s\n' "$*"
    else
        "$@"
    fi
}

usage() {
    cat <<EOF
用法：./install.sh [选项]

  --user            装到 ~/.local（默认，无需 sudo）
  --system          装到 /usr/local（需要 sudo）
  --prefix <目录>   自定义安装前缀
  --bin <路径>      指定 $APP 可执行文件（默认自动在 build/、当前目录、PATH 里找）
  --uninstall       卸载（删除二进制、快捷方式和图标）
  --dry-run         只打印将要执行的动作，不落盘
  -h, --help        显示本帮助
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --user) MODE="user" ;;
        --system) MODE="system" ;;
        --prefix)
            [ $# -ge 2 ] || die "--prefix 缺少目录参数"
            PREFIX="$2"
            shift
            ;;
        --bin)
            [ $# -ge 2 ] || die "--bin 缺少路径参数"
            BIN="$2"
            shift
            ;;
        --uninstall | -u) ACTION="uninstall" ;;
        --dry-run) DRY_RUN=1 ;;
        -h | --help)
            usage
            exit 0
            ;;
        *) die "未知参数：$1（用 --help 查看用法）" ;;
    esac
    shift
done

if [ -z "$PREFIX" ]; then
    if [ "$MODE" = "system" ]; then
        PREFIX="/usr/local"
    else
        PREFIX="$HOME/.local"
    fi
fi

SUDO=""
if [ "$MODE" = "system" ] && [ "$(id -u)" -ne 0 ]; then
    command -v sudo >/dev/null 2>&1 || die "需要 root 权限，但没有 sudo"
    SUDO="sudo"
fi

BIN_DIR="$PREFIX/bin"
APP_DIR="$PREFIX/share/applications"
ICON_ROOT="$PREFIX/share/icons/hicolor"

find_binary() {
    if [ -n "$BIN" ]; then
        [ -x "$BIN" ] || die "$BIN 不是可执行文件"
        printf '%s\n' "$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
        return
    fi
    local candidate
    for candidate in \
        "$SCRIPT_DIR/build/$APP" \
        "$SCRIPT_DIR/build-release/$APP" \
        "$SCRIPT_DIR/$APP" \
        "$SCRIPT_DIR/bin/$APP"; do
        if [ -x "$candidate" ] && [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return
        fi
    done
    if candidate="$(command -v "$APP" 2>/dev/null)"; then
        printf '%s\n' "$candidate"
        return
    fi
    die "找不到 $APP 可执行文件：先构建（cmake --build build），或用 --bin 指定"
}

refresh_caches() {
    if command -v update-desktop-database >/dev/null 2>&1; then
        run $SUDO update-desktop-database "$APP_DIR" >/dev/null 2>&1 || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1 && [ -d "$ICON_ROOT" ]; then
        run $SUDO gtk-update-icon-cache -f -t "$ICON_ROOT" >/dev/null 2>&1 || true
    fi
}

do_uninstall() {
    info "卸载 $APP（前缀：$PREFIX）"
    run $SUDO rm -f "$BIN_DIR/$APP"
    run $SUDO rm -f "$APP_DIR/$APP.desktop"
    local size
    for size in 16 32 64 256; do
        run $SUDO rm -f "$ICON_ROOT/${size}x${size}/apps/$APP.png"
    done
    # 这个前缀下没有别的桌面应用/图标了就把缓存一起清掉，否则只刷新缓存
    if [ "$DRY_RUN" -eq 0 ]; then
        local has_desktop=0
        local entry
        for entry in "$APP_DIR"/*.desktop; do
            if [ -e "$entry" ]; then
                has_desktop=1
                break
            fi
        done
        if [ "$has_desktop" -eq 0 ]; then
            $SUDO rm -f "$APP_DIR/mimeinfo.cache" "$ICON_ROOT/icon-theme.cache"
        else
            refresh_caches
        fi
    fi
    info "已卸载。"
}

do_install() {
    local source_bin
    source_bin="$(find_binary)"
    info "安装 $APP"
    info "  可执行文件：$source_bin"
    info "  安装前缀：  $PREFIX"

    [ -f "$SCRIPT_DIR/packaging/$APP.desktop" ] || die "缺少 packaging/$APP.desktop"

    local size icon
    for size in 16 32 64 256; do
        icon="$SCRIPT_DIR/assets/icons/$APP-$size.png"
        [ -f "$icon" ] || die "缺少图标 $icon"
    done

    # 1) 二进制
    run $SUDO install -d "$BIN_DIR"
    run $SUDO install -m 0755 "$source_bin" "$BIN_DIR/$APP"

    # 2) 图标（hicolor 多尺寸）
    for size in 16 32 64 256; do
        run $SUDO install -d "$ICON_ROOT/${size}x${size}/apps"
        run $SUDO install -m 0644 "$SCRIPT_DIR/assets/icons/$APP-$size.png" \
            "$ICON_ROOT/${size}x${size}/apps/$APP.png"
    done

    # 3) 快捷方式：Exec 写绝对路径，即使 ~/.local/bin 不在 PATH 也能从菜单启动
    run $SUDO install -d "$APP_DIR"
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '[dry-run] 生成 %s（Exec="%s"）\n' "$APP_DIR/$APP.desktop" "$BIN_DIR/$APP"
    else
        local tmp
        tmp="$(mktemp)"
        sed "s|^Exec=.*|Exec=\"$BIN_DIR/$APP\"|" "$SCRIPT_DIR/packaging/$APP.desktop" >"$tmp"
        $SUDO install -m 0644 "$tmp" "$APP_DIR/$APP.desktop"
        rm -f "$tmp"
        if command -v desktop-file-validate >/dev/null 2>&1; then
            desktop-file-validate "$APP_DIR/$APP.desktop" || info "提示：desktop-file-validate 报了警告，通常不影响使用"
        fi
    fi

    refresh_caches

    info ""
    info "完成："
    info "  命令：      $BIN_DIR/$APP"
    info "  快捷方式：  $APP_DIR/$APP.desktop（应用菜单里搜 termdock）"
    info "  图标：      $ICON_ROOT/{16,32,64,256}x*/apps/$APP.png"
    case ":$PATH:" in
        *":$BIN_DIR:"*) ;;
        *) info "  提示：$BIN_DIR 不在 PATH 里，想在终端直接敲 $APP 需要把 export PATH=\"$BIN_DIR:\$PATH\" 写进 ~/.bashrc" ;;
    esac
    info "  数据目录：软件目录下的 termdock-data/，临时或构建目录会自动回退到 ~/.config/termdock"
}

if [ "$ACTION" = "uninstall" ]; then
    do_uninstall
else
    do_install
fi
