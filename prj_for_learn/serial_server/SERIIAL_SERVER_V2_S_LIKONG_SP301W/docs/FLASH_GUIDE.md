# 烧写不同版本固件指南

## 📝 概述

本文档说明如何烧写6个不同版本的串口服务器固件。

## 🔥 烧写方法

### 方法1：直接在项目目录编译并烧写（推荐）

这是最简单的方法，适合开发和调试。

#### 步骤：

```bash
# 1. 进入你想烧写的版本目录
cd /home/master/project/serial_server/SERIIAL_SERVER_V2_S_LIKONG_SP401W

# 2. 连接ESP32设备到电脑

# 3. 编译并烧写
idf.py build flash monitor

# 或者分步执行：
idf.py build      # 编译
idf.py flash      # 烧写
idf.py monitor    # 查看串口输出
```

#### 烧写所有版本示例：

```bash
# 立控 SP501W（主线）
cd /home/master/project/serial_server/SERIIAL_SERVER_V2_S
idf.py flash

# 立控 SP401W
cd ../SERIIAL_SERVER_V2_S_LIKONG_SP401W
idf.py flash

# 立控 SP301W
cd ../SERIIAL_SERVER_V2_S_LIKONG_SP301W
idf.py flash

# 中性 SP501W
cd ../SERIIAL_SERVER_V2_S_NEUTRAL_SP501W
idf.py flash

# 中性 SP401W
cd ../SERIIAL_SERVER_V2_S_NEUTRAL_SP401W
idf.py flash

# 中性 SP301W
cd ../SERIIAL_SERVER_V2_S_NEUTRAL_SP301W
idf.py flash
```

---

### 方法2：使用批量编译后的固件烧写

如果你已经运行了 `build_all_variants.sh` 批量编译，可以使用这个方法。

#### 步骤：

```bash
# 1. 先批量编译所有版本（如果还没编译）
cd /home/master/project/serial_server/SERIIAL_SERVER_V2_S
./script/build_all_variants.sh

# 2. 使用烧写脚本（交互式）
./script/flash_variant.sh

# 或者直接指定版本号
./script/flash_variant.sh 2  # 烧写立控 SP401W
```

**版本号对照表：**
- 1 = 立控 SP501W
- 2 = 立控 SP401W
- 3 = 立控 SP301W
- 4 = 中性 SP501W
- 5 = 中性 SP401W
- 6 = 中性 SP301W

---

### 方法3：手动使用 esptool.py 烧写

如果你想完全手动控制烧写过程：

```bash
# 进入固件输出目录
cd /home/master/project/serial_server/firmware_output/SERIIAL_SERVER_V2_S_LIKONG_SP401W

# 使用 esptool.py 烧写
esptool.py --chip esp32 \
    --port /dev/ttyUSB0 \
    --baud 921600 \
    --before default_reset \
    --after hard_reset \
    write_flash -z \
    --flash_mode dio \
    --flash_freq 40m \
    --flash_size detect \
    0x1000 bootloader.bin \
    0x8000 partition-table.bin \
    0x10000 SP401W.bin
```

**注意：** 
- 替换 `/dev/ttyUSB0` 为你的实际串口设备
- 固件文件名可能不同，请根据实际情况调整

---

## 🔍 如何验证烧写的版本

烧写完成后，可以通过以下方式验证：

### 1. 查看WiFi SSID

设备启动后会创建WiFi热点，SSID格式为：`{型号}_{MAC后4位}`

**示例：**
- 立控 SP501W: `SP501W_A1B2`
- 立控 SP401W: `SP401W_C3D4`
- 立控 SP301W: `SP301W_E5F6`
- 中性 SP501W: `SP501W_1234`
- 中性 SP401W: `SP401W_5678`
- 中性 SP301W: `SP301W_9ABC`

### 2. 查看Web界面

连接到设备WiFi后，访问 `http://192.168.4.1`

**立控版本显示：**
```
立控电子 串口服务器
```

**中性版本显示：**
```
串口服务器
```

### 3. 查看串口输出

```bash
idf.py monitor
```

在启动日志中会显示设备型号和品牌信息。

---

## 🛠️ 常见问题

### Q1: 找不到串口设备

**Linux:**
```bash
# 查看可用串口
ls /dev/ttyUSB* /dev/ttyACM*

# 如果没有权限
sudo chmod 666 /dev/ttyUSB0
# 或者将用户加入 dialout 组
sudo usermod -a -G dialout $USER
```

**Windows:**
- 打开设备管理器查看COM端口号
- 使用 `COM3`、`COM4` 等

**Mac:**
```bash
ls /dev/cu.*
```

### Q2: 烧写失败

**可能原因：**
1. 串口被占用 - 关闭其他串口监控程序
2. 波特率不匹配 - 尝试降低波特率：`idf.py -b 115200 flash`
3. 设备未进入下载模式 - 手动按住BOOT按钮，然后按RST按钮

### Q3: 如何指定串口

```bash
# 方法1：使用环境变量
export ESPPORT=/dev/ttyUSB0
idf.py flash

# 方法2：使用命令行参数
idf.py -p /dev/ttyUSB0 flash

# 方法3：使用 menuconfig 配置
idf.py menuconfig
# 进入 Serial flasher config -> Default serial port
```

### Q4: 如何擦除Flash

如果需要完全擦除设备：

```bash
idf.py erase-flash
# 或
esptool.py --port /dev/ttyUSB0 erase_flash
```

### Q5: 烧写后设备不工作

1. **检查分区表** - 确保使用了正确的分区表
2. **完全擦除后重新烧写**
   ```bash
   idf.py erase-flash
   idf.py flash
   ```
3. **查看串口输出** - 使用 `idf.py monitor` 查看错误信息

---

## 📊 版本对照表

| 版本号 | 项目目录 | 型号 | 品牌 | WiFi SSID | 界面标题 |
|-------|---------|------|------|-----------|---------|
| 1 | SERIIAL_SERVER_V2_S | SP501W | 立控 | SP501W_XXXX | 立控电子 串口服务器 |
| 2 | SERIIAL_SERVER_V2_S_LIKONG_SP401W | SP401W | 立控 | SP401W_XXXX | 立控电子 串口服务器 |
| 3 | SERIIAL_SERVER_V2_S_LIKONG_SP301W | SP301W | 立控 | SP301W_XXXX | 立控电子 串口服务器 |
| 4 | SERIIAL_SERVER_V2_S_NEUTRAL_SP501W | SP501W | 中性 | SP501W_XXXX | 串口服务器 |
| 5 | SERIIAL_SERVER_V2_S_NEUTRAL_SP401W | SP401W | 中性 | SP401W_XXXX | 串口服务器 |
| 6 | SERIIAL_SERVER_V2_S_NEUTRAL_SP301W | SP301W | 中性 | SP301W_XXXX | 串口服务器 |

---

## 🚀 快速参考

### 开发调试（推荐）
```bash
cd [项目目录]
idf.py build flash monitor
```

### 批量生产
```bash
# 1. 批量编译
./script/build_all_variants.sh

# 2. 使用脚本烧写
./script/flash_variant.sh [版本号]
```

### 手动烧写
```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
    write_flash -z 0x1000 bootloader.bin 0x8000 partition-table.bin 0x10000 app.bin
```

---

## 📞 技术支持

如有问题，请参考：
- `BUILD_GUIDE.md` - 构建指南
- `SUMMARY.md` - 完成总结
- ESP-IDF 官方文档：https://docs.espressif.com/projects/esp-idf/
