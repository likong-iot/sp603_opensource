#!/bin/bash

# 串口服务器固件批量编译脚本
# 用途：自动编译所有6个版本的固件

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 项目根目录
SERIAL_SERVER_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}串口服务器固件批量编译脚本${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# 定义所有项目目录
declare -a PROJECTS=(
    "SERIIAL_SERVER_V2_S"
    "SERIIAL_SERVER_V2_S_LIKONG_SP401W"
    "SERIIAL_SERVER_V2_S_LIKONG_SP301W"
    "SERIIAL_SERVER_V2_S_NEUTRAL_SP501W"
    "SERIIAL_SERVER_V2_S_NEUTRAL_SP401W"
    "SERIIAL_SERVER_V2_S_NEUTRAL_SP301W"
)

# 编译单个项目
build_project() {
    local project_name=$1
    local project_dir="$SERIAL_SERVER_ROOT/$project_name"

    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}编译项目: $project_name${NC}"
    echo -e "${BLUE}========================================${NC}"

    if [ ! -d "$project_dir" ]; then
        echo -e "${RED}错误: 项目目录不存在: $project_dir${NC}"
        echo -e "${YELLOW}请先运行 create_variants.sh 创建项目${NC}"
        return 1
    fi

    cd "$project_dir"

    # 清理之前的编译
    if [ -d "build" ]; then
        echo "清理旧的编译文件..."
        rm -rf build
    fi

    # 编译
    echo "开始编译..."
    if idf.py build; then
        echo -e "${GREEN}✓ 编译成功: $project_name${NC}"

        # 复制固件到输出目录
        local output_dir="$SERIAL_SERVER_ROOT/firmware_output/$project_name"
        mkdir -p "$output_dir"

        if [ -f "build/${project_name}.bin" ]; then
            cp "build/${project_name}.bin" "$output_dir/"
            echo -e "${GREEN}✓ 固件已复制到: $output_dir${NC}"
        elif [ -f "build/serial_server.bin" ]; then
            cp "build/serial_server.bin" "$output_dir/${project_name}.bin"
            echo -e "${GREEN}✓ 固件已复制到: $output_dir${NC}"
        fi

        # 复制其他必要文件
        if [ -f "build/bootloader/bootloader.bin" ]; then
            cp "build/bootloader/bootloader.bin" "$output_dir/"
        fi
        if [ -f "build/partition_table/partition-table.bin" ]; then
            cp "build/partition_table/partition-table.bin" "$output_dir/"
        fi

        return 0
    else
        echo -e "${RED}✗ 编译失败: $project_name${NC}"
        return 1
    fi
}

# 主函数
main() {
    local success_count=0
    local fail_count=0
    local start_time=$(date +%s)

    echo "开始批量编译..."
    echo "项目总数: ${#PROJECTS[@]}"
    echo ""

    # 创建输出目录
    mkdir -p "$SERIAL_SERVER_ROOT/firmware_output"

    # 编译所有项目
    for project in "${PROJECTS[@]}"; do
        if build_project "$project"; then
            ((success_count++))
        else
            ((fail_count++))
        fi
        echo ""
    done

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    # 显示统计信息
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}编译完成！${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo "总项目数: ${#PROJECTS[@]}"
    echo -e "${GREEN}成功: $success_count${NC}"
    if [ $fail_count -gt 0 ]; then
        echo -e "${RED}失败: $fail_count${NC}"
    fi
    echo "耗时: ${duration}秒"
    echo ""
    echo "固件输出目录: $SERIAL_SERVER_ROOT/firmware_output"
    echo ""

    if [ $fail_count -eq 0 ]; then
        echo -e "${GREEN}所有固件编译成功！${NC}"
        return 0
    else
        echo -e "${YELLOW}部分固件编译失败，请检查错误信息${NC}"
        return 1
    fi
}

# 运行主函数
main
