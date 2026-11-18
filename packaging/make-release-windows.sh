#!/usr/bin/env bash
# 生成 Windows 发布物（放进 dist/）：
#   termdock-<ver>-windows-x64.zip   便携版：解压即用，自带 Qt / OpenSSL / libssh2 / MinGW 运行库
#   SHA256SUMS / MD5SUMS + RELEASE_NOTES-WINDOWS.md
#
# 依赖：MSYS2 UCRT64 工具链（gcc + qt6-base + qt6-tools + libssh2 + openssl）。
# 用法：./packaging/make-release-windows.sh [--no-build]
set -euo pipefail

APP=termdock
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MINGW_PREFIX="${MINGW_PREFIX:-/c/msys64/ucrt64}"
DIST="${DIST:-$ROOT/dist}"
BUILD="${BUILD:-$ROOT/build-win-release}"

info() { printf '%s\n' "$*"; }
die() { printf '错误：%s\n' "$*" >&2; exit 1; }

NO_BUILD=0
for arg in "$@"; do
    case "$arg" in
        --no-build) NO_BUILD=1 ;;
        -h | --help) sed -n '2,9p' "$0"; exit 0 ;;
        *) die "未知参数：$arg" ;;
    esac
done

VER="$(sed -n 's/^project(termdock VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
[ -n "$VER" ] || die "读不到 CMakeLists.txt 里的版本号"
[ -d "$MINGW_PREFIX" ] || die "找不到 MSYS2 UCRT64 环境：$MINGW_PREFIX（用 MINGW_PREFIX=... 指定）"

NAME="$APP-$VER-windows-x64"
STAGE="$DIST/$NAME"

export PATH="$MINGW_PREFIX/bin:/usr/bin:$PATH"

info "== 版本 $VER / 目标 windows-x64 =="

if [ "$NO_BUILD" -eq 0 ]; then
    info "== 构建 Release =="
    cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$MINGW_PREFIX" >/dev/null
    cmake --build "$BUILD" --target termdock >/dev/null
fi
[ -f "$BUILD/$APP.exe" ] || die "没有找到 $BUILD/$APP.exe"

rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$BUILD/$APP.exe" "$STAGE/$APP.exe"

# ---------- Qt 与依赖闭包 ----------
# 和构建后步骤共用同一个脚本，保证「build/ 里双击能跑」和「便携包能跑」是同一条路径。
info "== 收集依赖（windeployqt + import 闭包）=="
MINGW_PREFIX="$MINGW_PREFIX" "$ROOT/packaging/deploy-windows-deps.sh" "$STAGE" "$APP"

# ---------- 依赖完整性门禁 ----------
# 打包漏掉某个 DLL 是静默失败：本地 PATH 里有 MSYS2 就照样能跑，
# 到了用户机器上直接 STATUS_DLL_NOT_FOUND 退出。所以发布前必须自证闭包完整。
info "== 依赖完整性检查 =="
deps_of() {
    objdump -p "$1" 2>/dev/null | awk '/DLL Name:/{print $3}'
}

# 门禁本身必须能失败：之前删掉 deps_of 时它静默变成 "command not found"，
# 结果一边打错误一边报「依赖完整」，等于没有门禁。
if command -v objdump >/dev/null 2>&1; then :; else
    die "找不到 objdump，无法校验依赖完整性（MSYS2 的 binutils 应随工具链安装）"
fi
if [ -z "$(deps_of "$STAGE/$APP.exe")" ]; then
    die "依赖校验读不出 $APP.exe 的 import table，门禁无法工作"
fi

declare -A verified=()
declare -A absent=()
check_queue=()
while IFS= read -r f; do check_queue+=("$f"); done \
    < <(find "$STAGE" \( -name '*.dll' -o -name '*.exe' \))

is_system_dll() {
    case "$1" in
        api-ms-win-*.dll | ext-ms-*.dll) return 0 ;;
    esac
    [ -f "/c/Windows/System32/$1" ] && return 0
    [ -f "/c/Windows/System32/downlevel/$1" ] && return 0
    [ -f "/c/Windows/SysWOW64/$1" ] && return 0
    return 1
}

while [ ${#check_queue[@]} -gt 0 ]; do
    current="${check_queue[0]}"
    check_queue=("${check_queue[@]:1}")
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        [ -n "${verified[$name]:-}" ] && continue
        verified[$name]=1
        if found=$(find "$STAGE" -name "$name" | head -1) && [ -n "$found" ]; then
            check_queue+=("$found")
        elif is_system_dll "$name"; then
            :
        else
            absent[$name]=1
        fi
    done < <(deps_of "$current")
done

if [ ${#absent[@]} -gt 0 ]; then
    printf '缺少依赖：%s\n' "${!absent[@]}" >&2
    die "便携包依赖不完整，用户机器上会启动失败（见上面的 DLL 列表）"
fi
info "依赖完整（包内 $(find "$STAGE" -name '*.dll' | wc -l) 个 DLL）"

# ---------- 图标 ----------
mkdir -p "$STAGE/assets/icons"
cp "$ROOT/assets/icons/termdock-16.png" "$ROOT/assets/icons/termdock-32.png" \
    "$ROOT/assets/icons/termdock-64.png" "$ROOT/assets/icons/termdock-256.png" \
    "$STAGE/assets/icons/" 2>/dev/null || true
cp "$ROOT/packaging/termdock.ico" "$STAGE/" 2>/dev/null || true

cat >"$STAGE/README.txt" <<EOF
termdock $VER —— Windows 便携版（自带 Qt 运行库，不需要装 Qt / MinGW）

用法：
    双击 termdock.exe 直接运行。
    想从命令行用（例如自检 / 直连测试），在该目录打开 PowerShell 或 cmd：
        termdock.exe --selftest
        set TERMDOCK_PASSWORD=*** & termdock.exe --connect <host> --user <user>

注意：
    - 数据默认保存在 termdock.exe 同级的 termdock-data/ 目录，整个文件夹可以拷走。
    - 首次连接某台主机会记录指纹（TOFU），之后指纹变化会拒绝连接。
    - 运行需要 Windows 10 及以上（x64）。
EOF

info "== 打包 =="
rm -f "$DIST/$NAME.zip"
( cd "$DIST" && zip -qr "$NAME.zip" "$NAME" )

info "== 校验和 =="
( cd "$DIST" && sha256sum "$NAME.zip" > SHA256SUMS-windows && md5sum "$NAME.zip" > MD5SUMS-windows )

cat >"$DIST/RELEASE_NOTES-WINDOWS.md" <<EOF
## termdock $VER —— Windows x64 便携版

解压即用，**不需要**安装 Qt、MinGW 或任何运行库。

- \`$NAME.zip\` —— 自带 Qt 6 / OpenSSL / libssh2 / MinGW 运行库
- 校验和见 \`SHA256SUMS-windows\` / \`MD5SUMS-windows\`
- 运行要求：Windows 10 及以上（x64）
EOF

info "== 完成 =="
ls -lh "$DIST/$NAME.zip"
