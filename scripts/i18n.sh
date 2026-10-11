# 开发脚本人读消息：由 GNU gettext 读取 MO，由 envsubst 替换受限变量。
# 缺少可选运行时工具时保留完整英文回退；不影响构建或机器输出。
logger_message() {
  if command -v gettext >/dev/null 2>&1 && command -v envsubst >/dev/null 2>&1; then
    if logger_translation=$(
         # glibc 版本对 C.UTF-8 + LANGUAGE 的处理不同；只归一消息子进程。
         case "${LC_ALL:-${LC_MESSAGES:-${LANG:-C}}}" in
           C.UTF-8|C.utf8) LC_ALL=C; export LC_ALL ;;
         esac
         TEXTDOMAIN=c-logger-shell TEXTDOMAINDIR="$script_dir/../locales" gettext "$1"
       ) &&
       logger_translation=$(printf '%s' "$logger_translation" | envsubst "$1"); then
      printf '%s\n' "$logger_translation"
      return
    fi
  fi
  printf '%s\n' "${2-$1}"
}
