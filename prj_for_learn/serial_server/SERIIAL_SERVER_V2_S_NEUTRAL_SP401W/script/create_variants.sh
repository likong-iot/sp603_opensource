#!/bin/bash

# 串口服务器固件多版本构建脚本
# 用途：根据配置自动生成6个不同品牌和型号的固件版本

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 项目根目录
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_DIR="$PROJECT_ROOT/script"

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}串口服务器固件多版本构建脚本${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# 定义版本配置
# 格式: "目录名|型号|品牌类型|品牌中文名|品牌英文名"
declare -a VARIANTS=(
    "SERIIAL_SERVER_V2_S|SP501W|LIKONG|立控电子|LIKONG"
    "SERIIAL_SERVER_V2_S_LIKONG_SP401W|SP401W|LIKONG|立控电子|LIKONG"
    "SERIIAL_SERVER_V2_S_LIKONG_SP301W|SP301W|LIKONG|立控电子|LIKONG"
    "SERIIAL_SERVER_V2_S_NEUTRAL_SP501W|SP501W|NEUTRAL||"
    "SERIIAL_SERVER_V2_S_NEUTRAL_SP401W|SP401W|NEUTRAL||"
    "SERIIAL_SERVER_V2_S_NEUTRAL_SP301W|SP301W|NEUTRAL||"
)

# 创建变体项目目录
create_variant_project() {
    local dir_name=$1
    local model=$2
    local brand_type=$3
    local brand_cn=$4
    local brand_en=$5

    local target_dir="$PROJECT_ROOT/../$dir_name"

    # 如果是主线项目，跳过复制
    if [ "$dir_name" == "SERIIAL_SERVER_V2_S" ]; then
        echo -e "${YELLOW}跳过主线项目: $dir_name${NC}"
        return 0
    fi

    echo -e "${GREEN}创建项目: $dir_name${NC}"
    echo "  型号: $model"
    echo "  品牌: $brand_type"

    # 如果目录已存在，询问是否覆盖
    if [ -d "$target_dir" ]; then
        echo -e "${YELLOW}目录已存在: $target_dir${NC}"
        read -p "是否删除并重新创建? (y/N): " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            rm -rf "$target_dir"
        else
            echo "跳过创建"
            return 0
        fi
    fi

    # 复制主线项目
    echo "  复制文件..."
    cp -r "$PROJECT_ROOT" "$target_dir"

    # 删除不需要的文件
    rm -rf "$target_dir/.git"
    rm -rf "$target_dir/build"
    rm -rf "$target_dir/.cache"

    # 创建配置文件
    local config_file="$target_dir/configs/variant.h"
    mkdir -p "$target_dir/configs"

    cat > "$config_file" << EOF
/*
 * 固件变体配置文件
 * 自动生成 - 请勿手动编辑
 */

#ifndef SX_VARIANT_H
#define SX_VARIANT_H

// 设备型号
#define DEVICE_MODEL "$model"

// 品牌类型
#define BRAND_TYPE "$brand_type"

// 品牌名称（中文）
#define BRAND_NAME_CN "$brand_cn"

// 品牌名称（英文）
#define BRAND_NAME_EN "$brand_en"

#endif // SX_VARIANT_H
EOF

    # 修改CMakeLists.txt
    sed -i "s/project(SP501W)/project($model)/" "$target_dir/CMakeLists.txt"

    # 修改main/CMakeLists.txt，添加编译定义
    if [ -f "$target_dir/main/CMakeLists.txt" ]; then
        # 在文件末尾添加编译定义
        cat >> "$target_dir/main/CMakeLists.txt" << EOF

# 变体配置
target_compile_definitions(\${COMPONENT_LIB} PRIVATE
    DEVICE_MODEL="$model"
    BRAND_TYPE="$brand_type"
    BRAND_NAME_CN="$brand_cn"
    BRAND_NAME_EN="$brand_en"
)
EOF
    fi

    echo -e "${GREEN}  ✓ 项目创建完成${NC}"
    echo ""
}

# 主函数
main() {
    echo "开始创建所有变体项目..."
    echo ""

    for variant in "${VARIANTS[@]}"; do
        IFS='|' read -r dir_name model brand_type brand_cn brand_en <<< "$variant"
        create_variant_project "$dir_name" "$model" "$brand_type" "$brand_cn" "$brand_en"
    done

    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}所有项目创建完成！${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo "项目列表:"
    for variant in "${VARIANTS[@]}"; do
        IFS='|' read -r dir_name model brand_type brand_cn brand_en <<< "$variant"
        echo "  - $dir_name ($brand_type $model)"
    done
    echo ""
    echo "下一步："
    echo "  1. 进入各个项目目录"
    echo "  2. 运行 idf.py build 编译固件"
    echo "  3. 运行 idf.py flash 烧录固件"
}

# 运行主函数
main
