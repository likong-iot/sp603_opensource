#!/bin/bash

# 验证所有变体项目配置的脚本

set -e

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

SERIAL_SERVER_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}验证所有变体项目配置${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# 定义所有项目及其预期配置
declare -A PROJECTS
PROJECTS["SERIIAL_SERVER_V2_S"]="SP501W|LIKONG|立控电子|LIKONG"
PROJECTS["SERIIAL_SERVER_V2_S_LIKONG_SP401W"]="SP401W|LIKONG|立控电子|LIKONG"
PROJECTS["SERIIAL_SERVER_V2_S_LIKONG_SP301W"]="SP301W|LIKONG|立控电子|LIKONG"
PROJECTS["SERIIAL_SERVER_V2_S_NEUTRAL_SP501W"]="SP501W|NEUTRAL||"
PROJECTS["SERIIAL_SERVER_V2_S_NEUTRAL_SP401W"]="SP401W|NEUTRAL||"
PROJECTS["SERIIAL_SERVER_V2_S_NEUTRAL_SP301W"]="SP301W|NEUTRAL||"

verify_project() {
    local project_name=$1
    local expected_config=$2
    local project_dir="$SERIAL_SERVER_ROOT/$project_name"

    IFS='|' read -r expected_model expected_brand expected_cn expected_en <<< "$expected_config"

    echo -e "${YELLOW}检查项目: $project_name${NC}"

    # 检查目录是否存在
    if [ ! -d "$project_dir" ]; then
        echo -e "${RED}  ✗ 目录不存在${NC}"
        return 1
    fi
    echo -e "${GREEN}  ✓ 目录存在${NC}"

    # 检查variant.h配置文件（主线项目没有这个文件）
    if [ "$project_name" != "SERIIAL_SERVER_V2_S" ]; then
        local variant_file="$project_dir/configs/variant.h"
        if [ ! -f "$variant_file" ]; then
            echo -e "${RED}  ✗ 配置文件不存在: $variant_file${NC}"
            return 1
        fi

        # 验证配置内容
        if grep -q "DEVICE_MODEL \"$expected_model\"" "$variant_file" && \
           grep -q "BRAND_TYPE \"$expected_brand\"" "$variant_file"; then
            echo -e "${GREEN}  ✓ 配置文件正确${NC}"
        else
            echo -e "${RED}  ✗ 配置文件内容不正确${NC}"
            return 1
        fi
    fi

    # 检查CMakeLists.txt
    local cmake_file="$project_dir/CMakeLists.txt"
    if [ -f "$cmake_file" ]; then
        if grep -q "project($expected_model)" "$cmake_file" || \
           grep -q "project(SP501W)" "$cmake_file"; then
            echo -e "${GREEN}  ✓ CMakeLists.txt 正确${NC}"
        else
            echo -e "${RED}  ✗ CMakeLists.txt 项目名称不正确${NC}"
            return 1
        fi
    fi

    # 检查main/CMakeLists.txt（主线项目没有编译定义）
    if [ "$project_name" != "SERIIAL_SERVER_V2_S" ]; then
        local main_cmake="$project_dir/main/CMakeLists.txt"
        if [ -f "$main_cmake" ]; then
            if grep -q "target_compile_definitions" "$main_cmake" && \
               grep -q "DEVICE_MODEL=\"$expected_model\"" "$main_cmake"; then
                echo -e "${GREEN}  ✓ main/CMakeLists.txt 编译定义正确${NC}"
            else
                echo -e "${RED}  ✗ main/CMakeLists.txt 编译定义不正确${NC}"
                return 1
            fi
        fi
    fi

    # 检查关键文件是否存在
    if [ -f "$project_dir/main/static/i18n.js" ] && \
       [ -f "$project_dir/main/static/i18n/zh-CN.json" ] && \
       [ -f "$project_dir/main/static/i18n/en-US.json" ]; then
        echo -e "${GREEN}  ✓ 国际化文件存在${NC}"
    else
        echo -e "${RED}  ✗ 国际化文件缺失${NC}"
        return 1
    fi

    echo -e "${GREEN}  ✓ 项目配置验证通过${NC}"
    echo ""
    return 0
}

# 主函数
main() {
    local success_count=0
    local fail_count=0

    for project_name in "${!PROJECTS[@]}"; do
        if verify_project "$project_name" "${PROJECTS[$project_name]}"; then
            ((success_count+=1))
        else
            ((fail_count+=1))
        fi
    done

    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}验证完成${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo "总项目数: ${#PROJECTS[@]}"
    echo -e "${GREEN}通过: $success_count${NC}"
    if [ $fail_count -gt 0 ]; then
        echo -e "${RED}失败: $fail_count${NC}"
        return 1
    else
        echo -e "${GREEN}所有项目配置正确！${NC}"
        return 0
    fi
}

main
