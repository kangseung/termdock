#!/usr/bin/env bash
# 敏感信息检查的共用逻辑：被 pre-commit（暂存内容）和 commit-msg（提交信息）复用。
# 只做“明显不该出现的东西”的拦截，误报请用 git commit --no-verify 或改规则。

# 检查一个文本文件里的敏感内容，命中就调用 msg() 报错
check_sensitive_text() {
    local file="$1"
    [ -s "$file" ] || return 0

    if grep -qE -- '-----BEGIN [A-Z ]*PRIVATE KEY-----' "$file"; then
        msg "私钥内容"
    fi
    if grep -qiE -- '(pass(word|wd)?|secret|token|api[_-]?key)[[:space:]]*[:=][[:space:]]*"[^"]{3,}"' "$file"; then
        msg "疑似明文口令 / 密钥"
    fi
    if grep -qE -- 'sshpass[[:space:]]+-p' "$file"; then
        msg "sshpass 带明文密码"
    fi
    if grep -qE -- '(^|[^0-9.])(192\.168\.[0-9]{1,3}\.[0-9]{1,3}|10\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}|172\.(1[6-9]|2[0-9]|3[01])\.[0-9]{1,3}\.[0-9]{1,3})([^0-9]|$)' "$file"; then
        msg "私网 IP 地址"
    fi
    return 0
}

# 文件名黑名单：运行期数据、凭据、私钥
check_sensitive_path() {
    case "$1" in
        *sessions.json | *known_hosts.json | *recovery.json | */key | *termdock-data/* | \
            *.pem | *.key | *.p12 | *id_rsa* | *id_ed25519* | *.env)
            msg "不允许入库的文件 $1"
            ;;
    esac
    return 0
}
