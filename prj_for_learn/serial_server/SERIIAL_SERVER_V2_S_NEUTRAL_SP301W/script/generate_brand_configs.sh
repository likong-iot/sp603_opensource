#!/bin/bash

# 为每个变体项目生成对应的 brand-config.js 文件
# 此脚本在同步代码后自动运行

set -e

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

SERIAL_SERVER_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

echo -e "${GREEN}生成各项目的品牌配置文件...${NC}"

# 定义项目配置
declare -A PROJECTS
PROJECTS["SERIIAL_SERVER_V2_S"]="LIKONG|立控电子|LIKONG|SP501W"
PROJECTS["SERIIAL_SERVER_V2_S_LIKONG_SP401W"]="LIKONG|立控电子|LIKONG|SP401W"
PROJECTS["SERIIAL_SERVER_V2_S_LIKONG_SP301W"]="LIKONG|立控电子|LIKONG|SP301W"
PROJECTS["SERIIAL_SERVER_V2_S_NEUTRAL_SP501W"]="NEUTRAL|||SP501W"
PROJECTS["SERIIAL_SERVER_V2_S_NEUTRAL_SP401W"]="NEUTRAL|||SP401W"
PROJECTS["SERIIAL_SERVER_V2_S_NEUTRAL_SP301W"]="NEUTRAL|||SP301W"

for project_name in "${!PROJECTS[@]}"; do
    IFS='|' read -r brand brand_cn brand_en model <<< "${PROJECTS[$project_name]}"

    config_file="$SERIAL_SERVER_ROOT/$project_name/main/static/brand-config.js"

    if [ ! -d "$(dirname "$config_file")" ]; then
        echo -e "${YELLOW}跳过不存在的项目: $project_name${NC}"
        continue
    fi

    cat > "$config_file" << EOF
// 品牌配置 - 编译时自动生成
// 此文件由 generate_brand_configs.sh 自动生成，请勿手动编辑

window.BRAND_CONFIG = {
    brand: "$brand",           // LIKONG 或 NEUTRAL
    brandNameCN: "$brand_cn",  // 中文品牌名
    brandNameEN: "$brand_en",  // 英文品牌名
    model: "$model"            // 设备型号
};
EOF

    echo "  ✓ $project_name: $brand $model"
done

echo -e "${GREEN}品牌配置文件生成完成！${NC}"
