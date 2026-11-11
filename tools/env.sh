# 在 guest 的命令里 source：source DD.ModLoader/tools/env.sh
# guest_run 的环境里没有 xmake 和用户级环境变量，这里补上。
# 默认不设 DDPath，构建不会部署进真实安装；需要部署时先 export DD_DEPLOY=1。

export PATH="$PATH:/c/Users/Admin/AppData/Local/Microsoft/WinGet/Links"

dd_reg() {
    reg query 'HKCU\Environment' //v "$1" 2>/dev/null | tr -d '\r' | sed -n "s/^ *$1 *REG_[A-Z_]* *//p"
}

export DKUtilPath="$(dd_reg DKUtilPath)"
export DDPluginAuthor="$(dd_reg DDPluginAuthor)"

if [ "${DD_DEPLOY:-0}" = 1 ]; then
    export DDPath="$(dd_reg DDPath)"
else
    unset DDPath
fi

# Git Bash 下 xmake 默认选 mingw，配置时必须指定平台
dd_config() {
    xmake f -p windows -a x64 -m "${1:-releasedbg}" -y
}
