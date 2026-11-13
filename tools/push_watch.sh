#!/bin/bash
# 轮询等待远端仓库被创建，一旦可推送就自动推送 main 分支
# 用法: tools/push_watch.sh [最大等待秒数]
cd "$(dirname "$0")/.." || exit 1

MAX="${1:-1800}"
LOG=/tmp/gh_push_watch.log
: > "$LOG"
t0=$(date +%s)

export GIT_TERMINAL_PROMPT=0
export GIT_SSH_COMMAND="ssh -o StrictHostKeyChecking=no -o ConnectTimeout=15"

while :; do
  now=$(date +%s)
  elapsed=$(( now - t0 ))
  [ "$elapsed" -ge "$MAX" ] && { echo "TIMEOUT: 等了 ${MAX}s 仓库仍未出现"; exit 2; }

  if git push -u origin main >"$LOG" 2>&1; then
    echo "=== PUSH OK (用时 ${elapsed}s) ==="
    cat "$LOG"
    echo "=== 远端确认 ==="
    git ls-remote --heads origin 2>&1 | head -5
    exit 0
  fi

  if grep -qi "Repository not found" "$LOG"; then
    printf "[%3ds] 仓库尚未创建，5 秒后重试...\n" "$elapsed"
  else
    printf "[%3ds] 推送失败，原因：\n" "$elapsed"
    head -5 "$LOG"
  fi
  sleep 5
done
