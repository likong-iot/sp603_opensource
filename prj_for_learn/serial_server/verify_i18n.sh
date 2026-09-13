#!/bin/bash

# 国际化验证脚本
# 检查所有项目的国际化文件是否正确同步

echo "=========================================="
echo "国际化文件验证"
echo "=========================================="
echo ""

# 项目列表
PROJECTS=(
    "SERIIAL_SERVER_V2_S"
    "SERIIAL_SERVER_V2_S_LIKONG_SP301W"
    "SERIIAL_SERVER_V2_S_LIKONG_SP401W"
    "SERIIAL_SERVER_V2_S_NEUTRAL_SP301W"
    "SERIIAL_SERVER_V2_S_NEUTRAL_SP401W"
    "SERIIAL_SERVER_V2_S_NEUTRAL_SP501W"
)

# 检查文件是否存在
check_file() {
    local project=$1
    local file=$2

    if [ -f "$project/main/static/$file" ]; then
        echo "  ✓ $file"
        return 0
    else
        echo "  ✗ $file (缺失)"
        return 1
    fi
}

# 检查翻译键数量
check_translation_keys() {
    local file=$1
    local count=$(grep -o '"[^"]*":' "$file" | wc -l)
    echo "    翻译键数量: $count"
}

# 检查每个项目
for project in "${PROJECTS[@]}"; do
    echo "检查项目: $project"

    if [ ! -d "$project" ]; then
        echo "  ⚠ 项目目录不存在"
        echo ""
        continue
    fi

    # 检查必需文件
    check_file "$project" "web.html"
    check_file "$project" "i18n.js"
    check_file "$project" "i18n/zh-CN.json"
    check_file "$project" "i18n/en-US.json"

    # 检查翻译文件内容
    if [ -f "$project/main/static/i18n/zh-CN.json" ]; then
        check_translation_keys "$project/main/static/i18n/zh-CN.json"
    fi

    # 检查brand-config.js
    if [ -f "$project/main/static/brand-config.js" ]; then
        brand=$(grep 'brand:' "$project/main/static/brand-config.js" | grep -o '"[^"]*"' | head -1)
        model=$(grep 'model:' "$project/main/static/brand-config.js" | grep -o '"[^"]*"' | tail -1)
        echo "    品牌: $brand, 型号: $model"
    fi

    echo ""
done

echo "=========================================="
echo "验证完成"
echo "=========================================="
echo ""

# 统计HTML中的data-i18n属性
echo "统计 data-i18n 属性使用情况："
for project in "${PROJECTS[@]}"; do
    if [ -f "$project/main/static/web.html" ]; then
        i18n_count=$(grep -o 'data-i18n="[^"]*"' "$project/main/static/web.html" | wc -l)
        placeholder_count=$(grep -o 'data-i18n-placeholder="[^"]*"' "$project/main/static/web.html" | wc -l)
        echo "  $project:"
        echo "    data-i18n: $i18n_count 个"
        echo "    data-i18n-placeholder: $placeholder_count 个"
    fi
done

echo ""
echo "建议："
echo "1. 编译并烧写固件到设备"
echo "2. 在浏览器中测试中英文切换功能"
echo "3. 检查所有页面的文本是否正确翻译"
