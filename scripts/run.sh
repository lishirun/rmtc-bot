#!/usr/bin/env bash
# 启动参赛程序。所有接入信息从环境变量读取，代码里不写死（换场换席位只改环境变量）。
#
# 用法：
#   chmod +x scripts/run.sh
#   RMTC_TEAM_TOKEN=practice-A ./scripts/run.sh
set -euo pipefail

export RMTC_SEAT="${RMTC_SEAT:-A}"
export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-27}"
export ROS_LOCALHOST_ONLY=0
export RMW_IMPLEMENTATION="${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}"
export RMTC_AI_URL="${RMTC_AI_URL:-http://127.0.0.1:8080/v1/chat/completions}"

if [[ -z "${RMTC_TEAM_TOKEN:-}" ]]; then
  echo "[错误] 缺少 RMTC_TEAM_TOKEN（本场席位令牌），参考裁判教程里的 practice-A/B/C/D" >&2
  exit 1
fi

source /opt/ros/humble/setup.bash
source "${ROS_WS:-$HOME/ros2_ws}/install/setup.bash"

echo "[info] seat=$RMTC_SEAT domain=$ROS_DOMAIN_ID ai=$RMTC_AI_URL"
exec ros2 run rmtc_bot contestant
