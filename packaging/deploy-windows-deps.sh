#!/usr/bin/env bash
# 把 Windows 便携包所需的依赖闭包复制到指定目录。
#
# 用法：deploy-windows-deps.sh <目标目录> [exe名]
# 做两件事：
#   1) windeployqt 搬 Qt 本体 + 平台/图像/样式/tls 插件（含 qoffscreen，供无头自检）
#   2) 沿着 import table 递归补齐非 Qt 依赖（OpenSSL / libssh2 / MinGW / icu /
#      freetype / harfbuzz / glib …），否则 Qt6Gui 的传递依赖缺一个就起不来。
#
# 为什么不用 windeployqt 单独完成：它只处理 Qt 自己的直接依赖，Qt6Gui 背后
# 那串 icu/freetype/harfbuzz/glib 并不会被递归带上，双击照样弹缺 DLL。
set -euo pipefail

TARGET="${1:?用法: deploy-windows-deps.sh <目标目录> [exe名]}"
APP="${2:-termdock}"
MINGW_PREFIX="${MINGW_PREFIX:-/c/msys64/ucrt64}"
export PATH="$MINGW_PREFIX/bin:/usr/bin:$PATH"

EXE="$TARGET/$APP.exe"
[ -f "$EXE" ] || { echo "找不到 $EXE" >&2; exit 1; }

DEPLOYQT=""
for c in windeployqt6 windeployqt6.exe windeployqt; do
    if command -v "$c" >/dev/null 2>&1; then DEPLOYQT="$c"; break; fi
done
[ -n "$DEPLOYQT" ] || { echo "找不到 windeployqt" >&2; exit 1; }

"$DEPLOYQT" --release --no-translations --no-system-d3d-compiler \
    --no-system-dxc-compiler --no-opengl-sw --no-quick-import \
    --include-plugins qoffscreen,qminimal,qwindows \
    --compiler-runtime --dir "$TARGET" "$EXE" >/dev/null 2>&1 || true

deps_of() { objdump -p "$1" 2>/dev/null | awk '/DLL Name:/{print $3}'; }

is_system_dll() {
    case "$1" in api-ms-win-*.dll | ext-ms-*.dll) return 0 ;; esac
    [ -f "/c/Windows/System32/$1" ] && return 0
    [ -f "/c/Windows/System32/downlevel/$1" ] && return 0
    [ -f "/c/Windows/SysWOW64/$1" ] && return 0
    return 1
}

declare -A done_map=()
queue=()
while IFS= read -r f; do queue+=("$f"); done < <(find "$TARGET" \( -name '*.dll' -o -name '*.exe' \))

while [ ${#queue[@]} -gt 0 ]; do
    cur="${queue[0]}"; queue=("${queue[@]:1}")
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        [ -n "${done_map[$name]:-}" ] && continue
        done_map[$name]=1
        if found=$(find "$TARGET" -name "$name" | head -1) && [ -n "$found" ]; then
            queue+=("$found")
            continue
        fi
        is_system_dll "$name" && continue
        src="$MINGW_PREFIX/bin/$name"
        if [ -f "$src" ]; then
            cp -L "$src" "$TARGET/$name"
            queue+=("$TARGET/$name")
        fi
    done < <(deps_of "$cur")
done
