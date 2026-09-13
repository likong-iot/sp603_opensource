#!/bin/bash

# 兼容旧入口：实际同步逻辑统一放在主线项目的正式脚本中。
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/SERIIAL_SERVER_V2_S/script/sync_variants.sh" "$@"
