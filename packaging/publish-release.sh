#!/usr/bin/env bash
# 把 dist/ 里的产物发布成 GitHub Release。
# 前置：先跑 ./packaging/make-release.sh 生成 dist/
#      并 gh auth login（或用 GITHUB_TOKEN 环境变量）
#
# 用法：./packaging/publish-release.sh [tag]      # 默认 tag = v<版本号>
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DIST="${DIST:-$ROOT/dist}"
REPO="${REPO:-kangseung/termdock}"

die() { printf '错误：%s\n' "$*" >&2; exit 1; }

VER="$(sed -n 's/^project(termdock VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
TAG="${1:-v$VER}"

[ -f "$DIST/RELEASE_NOTES.md" ] || die "没有 $DIST/RELEASE_NOTES.md，先跑 ./packaging/make-release.sh"
[ -f "$DIST/SHA256SUMS" ] || die "没有校验和文件，先跑 ./packaging/make-release.sh"

if ! command -v gh >/dev/null 2>&1; then
    die "没有 gh（GitHub CLI）。装一个：sudo apt install gh，或者手动在网页上传 $DIST 里的文件"
fi
if ! gh auth status >/dev/null 2>&1; then
    die "gh 未登录：先执行 gh auth login（或用 GITHUB_TOKEN=... 运行本脚本）"
fi

assets=()
for pattern in "termdock-$VER-linux-x86_64.tar.gz" "termdock-$VER-linux-x86_64.zip" \
    "termdock_${VER}_amd64.deb" "SHA256SUMS" "MD5SUMS"; do
    [ -f "$DIST/$pattern" ] && assets+=("$DIST/$pattern")
done
[ ${#assets[@]} -gt 0 ] || die "$DIST 里没有可上传的产物"

printf '发布 %s 到 %s（%d 个附件）\n' "$TAG" "$REPO" "${#assets[@]}"
gh release create "$TAG" "${assets[@]}" \
    --repo "$REPO" \
    --title "termdock $VER" \
    --notes-file "$DIST/RELEASE_NOTES.md"
printf '完成：https://github.com/%s/releases/tag/%s\n' "$REPO" "$TAG"
