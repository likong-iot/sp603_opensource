#!/bin/bash

# 使用已编译的固件文件烧写到设备
# 用途：当你已经批量编译好所有固件后，可以用这个脚本快速烧写指定版本

set -e

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

SERIAL_SERVER_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FIRMWARE_OUTPUT="$SERIAL_SERVER_ROOT/firmware_output"

# 显示可用的固件版本
show_versions() {
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}可用的固件版本${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo "1. SERIIAL_SERVER_V2_S              (立控 SP501W)"
    echo "2. SERIIAL_SERVER_V2_S_LIKONG_SP401W (立控 SP401W)"
    echo "3. SERIIAL_SERVER_V2_S_LIKONG_SP301W (立控 SP301W)"
    echo "4. SERIIAL_SERVER_V2_S_NEUTRAL_SP501W (中性 SP501W)"
    echo "5. SERIIAL_SERVER_V2_S_NEUTRAL_SP401W (中性 SP401W)"
    echo "6. SERIIAL_SERVER_V2_S_NEUTRAL_SP301W (中性 SP301W)"
    echo ""
}

# 烧写指定版本
flash_version() {
    local version_name=$1
    local firmware_dir="$FIRMWARE_OUTPUT/$version_name"

    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}烧写固件: $version_name${NC}"
    echo -e "${BLUE}========================================${NC}"

    # 检查固件目录是否存在
    if [ ! -d "$firmware_dir" ]; then
        echo -e "${RED}错误: 固件目录不存在: $firmware_dir${NC}"
        echo -e "${YELLOW}请先运行 build_all_variants.sh 编译固件${NC}"
        return 1
    fi

    # 检查必要的固件文件
    local bootloader="$firmware_dir/bootloader.bin"
    local partition="$firmware_dir/partition-table.bin"
    local app_bin=$(find "$firmware_dir" -name "*.bin" ! -name "bootloader.bin" ! -name "partition-table.bin" | head -1)

    if [ ! -f "$bootloader" ] || [ ! -f "$partition" ] || [ -z "$app_bin" ]; then
        echo -e "${RED}错误: 固件文件不完整${NC}"
        echo "需要的文件:"
        echo "  - bootloader.bin"
        echo "  - partition-table.bin"
        echo "  - 应用程序.bin"
        return 1
    fi

    echo "固件文件:"
    echo "  Bootloader: $bootloader"
    echo "  分区表: $partition"
    echo "  应用程序: $app_bin"
    echo ""

    # 检测串口
    echo "检测串口设备..."
    local port=""
    if [ -e "/dev/ttyUSB0" ]; then
        port="/dev/ttyUSB0"
    elif [ -e "/dev/ttyUSB1" ]; then
        port="/dev/ttyUSB1"
    elif [ -e "/dev/ttyACM0" ]; then
        port="/dev/ttyACM0"
    else
        echo -e "${YELLOW}未自动检测到串口，请手动指定${NC}"
        read -p "请输入串口设备路径 (例如 /dev/ttyUSB0): " port
    fi

    echo -e "${GREEN}使用串口: $port${NC}"
    echo ""

    # 使用 esptool.py 烧写
    echo "开始烧写..."
    if command -v esptool.py &> /dev/null; then
        esptool.py --chip esp32 \
            --port "$port" \
            --baud 921600 \
            --before default_reset \
            --after hard_reset \
            write_flash -z \
            --flash_mode dio \
            --flash_freq 40m \
            --flash_size detect \
            0x1000 "$bootloader" \
            0x8000 "$partition" \
            0x10000 "$app_bin"

        echo -e "${GREEN}✓ 烧写完成！${NC}"
        echo ""
        echo "设备将自动重启..."
        echo "WiFi SSID 将显示为对应的型号前缀"
        return 0
    else
        echo -e "${RED}错误: 未找到 esptool.py${NC}"
        echo "请确保已安装 ESP-IDF 环境"
        return 1
    fi
}

# 主函数
main() {
    show_versions

    # 如果提供了参数，直接烧写
    if [ $# -eq 1 ]; then
        case $1 in
            1) flash_version "SERIIAL_SERVER_V2_S" ;;
            2) flash_version "SERIIAL_SERVER_V2_S_LIKONG_SP401W" ;;
            3) flash_version "SERIIAL_SERVER_V2_S_LIKONG_SP301W" ;;
            4) flash_version "SERIIAL_SERVER_V2_S_NEUTRAL_SP501W" ;;
            5) flash_version "SERIIAL_SERVER_V2_S_NEUTRAL_SP401W" ;;
            6) flash_version "SERIIAL_SERVER_V2_S_NEUTRAL_SP301W" ;;
            *)
                echo -e "${RED}无效的选项: $1${NC}"
                exit 1
                ;;
        esac
    else
        # 交互式选择
        read -p "请选择要烧写的版本 (1-6): " choice
        case $choice in
            1) flash_version "SERIIAL_SERVER_V2_S" ;;
            2) flash_version "SERIIAL_SERVER_V2_S_LIKONG_SP401W" ;;
            3) flash_version "SERIIAL_SERVER_V2_S_LIKONG_SP301W" ;;
            4) flash_version "SERIIAL_SERVER_V2_S_NEUTRAL_SP501W" ;;
            5) flash_version "SERIIAL_SERVER_V2_S_NEUTRAL_SP401W" ;;
            6) flash_version "SERIIAL_SERVER_V2_S_NEUTRAL_SP301W" ;;
            *)
                echo -e "${RED}无效的选择${NC}"
                exit 1
                ;;
        esac
    fi
}

main "$@"
