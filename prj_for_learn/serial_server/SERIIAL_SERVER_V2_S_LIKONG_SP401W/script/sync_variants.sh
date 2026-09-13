#!/bin/bash

# 串口服务器固件代码同步脚本
# 用途：将主线项目的代码更新同步到所有变体项目
#
# 工作原理：
# 1. 主线项目（SERIIAL_SERVER_V2_S）是唯一的代码维护点
# 2. 其他项目从主线同步代码，但保留各自的配置文件
# 3. 同步时会保护以下文件不被覆盖：
#    - configs/variant.h (变体配置)
#    - CMakeLists.txt (项目名称)
#    - main/CMakeLists.txt (编译定义)

set -e

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

SERIAL_SERVER_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MAIN_PROJECT="$SERIAL_SERVER_ROOT/SERIIAL_SERVER_V2_S"

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}串口服务器固件代码同步脚本${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# 定义所有变体项目（不包括主线）
declare -a VARIANT_PROJECTS=(
    "SERIIAL_SERVER_V2_S_LIKONG_SP401W"
    "SERIIAL_SERVER_V2_S_LIKONG_SP301W"
    "SERIIAL_SERVER_V2_S_NEUTRAL_SP501W"
    "SERIIAL_SERVER_V2_S_NEUTRAL_SP401W"
    "SERIIAL_SERVER_V2_S_NEUTRAL_SP301W"
)

# 需要保护的文件（不会被同步覆盖）
declare -a PROTECTED_FILES=(
    "configs/variant.h"
    "CMakeLists.txt"
    "main/CMakeLists.txt"
)

# 需要排除的目录（不同步）
declare -a EXCLUDED_DIRS=(
    ".git"
    "build"
    "configs"
    ".cache"
    ".vscode"
    ".sync_backup"
)

# 同步单个项目
sync_project() {
    local project_name=$1
    local target_dir="$SERIAL_SERVER_ROOT/$project_name"

    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}同步项目: $project_name${NC}"
    echo -e "${BLUE}========================================${NC}"

    if [ ! -d "$target_dir" ]; then
        echo -e "${RED}错误: 目标目录不存在: $target_dir${NC}"
        echo -e "${YELLOW}请先运行 create_variants.sh 创建项目${NC}"
        return 1
    fi

    # 备份保护的文件
    echo "备份配置文件..."
    local backup_dir="$target_dir/.sync_backup"
    mkdir -p "$backup_dir"

    for protected_file in "${PROTECTED_FILES[@]}"; do
        local file_path="$target_dir/$protected_file"
        if [ -f "$file_path" ]; then
            mkdir -p "$backup_dir/$(dirname "$protected_file")"
            cp "$file_path" "$backup_dir/$protected_file"
            echo "  ✓ 备份: $protected_file"
        fi
    done

    # 同步代码（排除特定目录）
    echo "同步代码..."

    # 构建 rsync 排除参数
    local exclude_args=""
    for excluded_dir in "${EXCLUDED_DIRS[@]}"; do
        exclude_args="$exclude_args --exclude=$excluded_dir"
    done

    # 排除保护的文件
    for protected_file in "${PROTECTED_FILES[@]}"; do
        exclude_args="$exclude_args --exclude=$protected_file"
    done

    # 使用 rsync 同步
    if command -v rsync &> /dev/null; then
        rsync -av --delete \
            $exclude_args \
            "$MAIN_PROJECT/" "$target_dir/"
        echo -e "${GREEN}  ✓ 代码同步完成（使用 rsync）${NC}"
    else
        # 如果没有 rsync，使用 cp（不推荐，因为不会删除多余文件）
        echo -e "${YELLOW}  警告: 未找到 rsync，使用 cp 命令（可能不完整）${NC}"

        # 复制主要目录
        cp -r "$MAIN_PROJECT/main" "$target_dir/" 2>/dev/null || true
        cp -r "$MAIN_PROJECT/components" "$target_dir/" 2>/dev/null || true
        cp -r "$MAIN_PROJECT/docs" "$target_dir/" 2>/dev/null || true
        cp -r "$MAIN_PROJECT/script" "$target_dir/" 2>/dev/null || true

        # 复制根目录文件
        cp "$MAIN_PROJECT"/*.csv "$target_dir/" 2>/dev/null || true
        cp "$MAIN_PROJECT"/*.md "$target_dir/" 2>/dev/null || true
        cp "$MAIN_PROJECT/sdkconfig"* "$target_dir/" 2>/dev/null || true

        echo -e "${GREEN}  ✓ 代码同步完成（使用 cp）${NC}"
    fi

    # 恢复保护的文件
    echo "恢复配置文件..."
    for protected_file in "${PROTECTED_FILES[@]}"; do
        local backup_file="$backup_dir/$protected_file"
        local target_file="$target_dir/$protected_file"
        if [ -f "$backup_file" ]; then
            mkdir -p "$(dirname "$target_file")"
            cp "$backup_file" "$target_file"
            echo "  ✓ 恢复: $protected_file"
        fi
    done

    # 清理备份
    rm -rf "$backup_dir"

    echo -e "${GREEN}✓ 项目同步完成: $project_name${NC}"
    echo ""
    return 0
}

# 显示同步信息
show_sync_info() {
    echo "同步说明："
    echo "  - 主线项目: SERIIAL_SERVER_V2_S"
    echo "  - 变体项目: ${#VARIANT_PROJECTS[@]} 个"
    echo ""
    echo "保护的文件（不会被覆盖）："
    for file in "${PROTECTED_FILES[@]}"; do
        echo "  - $file"
    done
    echo ""
    echo "排除的目录（不会同步）："
    for dir in "${EXCLUDED_DIRS[@]}"; do
        echo "  - $dir"
    done
    echo ""
}

# 主函数
main() {
    # 检查主线项目是否存在
    if [ ! -d "$MAIN_PROJECT" ]; then
        echo -e "${RED}错误: 主线项目不存在: $MAIN_PROJECT${NC}"
        exit 1
    fi

    show_sync_info

    # 询问是否继续
    if [ "$1" != "-y" ] && [ "$1" != "--yes" ]; then
        read -p "是否继续同步所有变体项目? (y/N): " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            echo "已取消"
            exit 0
        fi
    fi

    local success_count=0
    local fail_count=0
    local start_time=$(date +%s)

    # 同步所有变体项目
    for project in "${VARIANT_PROJECTS[@]}"; do
        if sync_project "$project"; then
            ((success_count+=1))
        else
            ((fail_count+=1))
        fi
    done

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    # 显示统计信息
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}同步完成！${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo "总项目数: ${#VARIANT_PROJECTS[@]}"
    echo -e "${GREEN}成功: $success_count${NC}"
    if [ $fail_count -gt 0 ]; then
        echo -e "${RED}失败: $fail_count${NC}"
    fi
    echo "耗时: ${duration}秒"
    echo ""

    if [ $fail_count -eq 0 ]; then
        echo -e "${GREEN}所有项目同步成功！${NC}"
        echo ""
        echo "下一步："
        echo "  1. 检查各项目的配置文件是否正确"
        echo "  2. 重新编译需要更新的项目"
        echo "  3. 测试功能是否正常"
        return 0
    else
        echo -e "${YELLOW}部分项目同步失败，请检查错误信息${NC}"
        return 1
    fi
}

# 显示帮助信息
show_help() {
    echo "用法: $0 [选项]"
    echo ""
    echo "选项:"
    echo "  -y, --yes    自动确认，不询问"
    echo "  -h, --help   显示帮助信息"
    echo ""
    echo "示例:"
    echo "  $0           # 交互式同步"
    echo "  $0 -y        # 自动同步，不询问"
}

# 解析命令行参数
case "$1" in
    -h|--help)
        show_help
        exit 0
        ;;
    *)
        main "$@"
        ;;
esac
