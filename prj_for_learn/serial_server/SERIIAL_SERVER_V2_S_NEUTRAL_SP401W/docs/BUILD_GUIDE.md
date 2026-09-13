# 串口服务器固件多版本构建指南

## 概述

本项目支持生成6个不同品牌和型号的固件版本：

| 品牌类型 | 型号 | WiFi SSID前缀 | 项目目录名 | 说明 |
|---------|------|--------------|-----------|------|
| 立控 | SP501W | SP501W_XXXX | SERIIAL_SERVER_V2_S | 主线版本 |
| 立控 | SP401W | SP401W_XXXX | SERIIAL_SERVER_V2_S_LIKONG_SP401W | 立控新型号 |
| 立控 | SP301W | SP301W_XXXX | SERIIAL_SERVER_V2_S_LIKONG_SP301W | 立控新型号 |
| 中性/无标 | SP501W | SP501W_XXXX | SERIIAL_SERVER_V2_S_NEUTRAL_SP501W | 中性版本 |
| 中性/无标 | SP401W | SP401W_XXXX | SERIIAL_SERVER_V2_S_NEUTRAL_SP401W | 中性新型号 |
| 中性/无标 | SP301W | SP301W_XXXX | SERIIAL_SERVER_V2_S_NEUTRAL_SP301W | 中性新型号 |

**注意：** XXXX 为设备MAC地址后4位

## 已完成的修改

### 1. ✅ 统一界面风格
- 所有页面（包括串口管理和系统管理）统一为居中对齐
- 修改文件：`main/static/web.css`

### 2. ✅ 国际化支持
- 添加中英文切换功能
- 新增文件：
  - `main/static/i18n/zh-CN.json` - 中文翻译
  - `main/static/i18n/en-US.json` - 英文翻译
  - `main/static/i18n.js` - 国际化脚本
- 修改文件：
  - `main/static/web.html` - 添加语言切换按钮和data-i18n属性
  - `main/static/web.css` - 添加语言切换按钮样式

### 3. ✅ 多版本支持
- 修改核心配置文件支持多品牌、多型号
- 修改文件：
  - `main/include/sx_web_server.h` - 使用宏定义
  - `main/sx_web_server.c` - WiFi SSID使用宏定义
  - `CMakeLists.txt` - 项目名称改为SP501W

## 使用方法

### 步骤1：创建所有变体项目

在主线项目目录下运行：

```bash
cd SERIIAL_SERVER_V2_S
./script/create_variants.sh
```

这个脚本会：
1. 在上级目录创建其他5个项目目录
2. 复制主线代码到新目录
3. 为每个项目生成配置文件 `configs/variant.h`
4. 修改 `CMakeLists.txt` 设置正确的项目名称
5. 添加编译定义到 `main/CMakeLists.txt`

### 步骤2：批量编译所有版本（可选）

如果要一次性编译所有版本：

```bash
cd SERIIAL_SERVER_V2_S
./script/build_all_variants.sh
```

编译完成后，所有固件将保存在 `../firmware_output/` 目录下。

### 步骤3：单独编译某个版本

进入对应的项目目录：

```bash
# 例如编译立控 SP401W
cd ../SERIIAL_SERVER_V2_S_LIKONG_SP401W
idf.py build
idf.py flash
```

## 项目结构

```
serial_server/
├── SERIIAL_SERVER_V2_S/                   # 主线：立控 SP501W
│   ├── script/
│   │   ├── create_variants.sh            # 创建变体项目脚本
│   │   └── build_all_variants.sh         # 批量编译脚本
│   ├── main/
│   │   ├── static/
│   │   │   ├── i18n/                     # 国际化资源
│   │   │   │   ├── zh-CN.json
│   │   │   │   └── en-US.json
│   │   │   ├── i18n.js                   # 国际化脚本
│   │   │   ├── web.html                  # 主页面（已添加i18n支持）
│   │   │   ├── web.css                   # 样式（已统一居中）
│   │   │   └── web.js
│   │   └── include/
│   │       └── sx_web_server.h           # 核心配置（支持多版本）
│   └── docs/
│       ├── firmware_variants_plan.md     # 实施方案文档
│       └── BUILD_GUIDE.md                # 本文档
├── SERIIAL_SERVER_V2_S_LIKONG_SP401W/    # 立控 SP401W
├── SERIIAL_SERVER_V2_S_LIKONG_SP301W/    # 立控 SP301W
├── SERIIAL_SERVER_V2_S_NEUTRAL_SP501W/   # 中性 SP501W
├── SERIIAL_SERVER_V2_S_NEUTRAL_SP401W/   # 中性 SP401W
├── SERIIAL_SERVER_V2_S_NEUTRAL_SP301W/   # 中性 SP301W
└── firmware_output/                       # 编译输出目录
    ├── SERIIAL_SERVER_V2_S/
    ├── SERIIAL_SERVER_V2_S_LIKONG_SP401W/
    └── ...
```

## 版本差异说明

各版本之间的主要差异：

1. **设备型号** (`DEVICE_MODEL`)
   - 影响WiFi SSID前缀
   - 影响设备标识

2. **品牌类型** (`BRAND_TYPE`)
   - `LIKONG` - 立控品牌
   - `NEUTRAL` - 中性/无标品牌

3. **品牌名称**
   - 立控版本：界面显示"立控电子 串口服务器"
   - 中性版本：界面显示"串口服务器"

4. **WiFi SSID命名**
   - 立控 SP501W: `SP501W_XXXX`
   - 立控 SP401W: `SP401W_XXXX`
   - 立控 SP301W: `SP301W_XXXX`
   - 中性 SP501W: `SP501W_XXXX`
   - 中性 SP401W: `SP401W_XXXX`
   - 中性 SP301W: `SP301W_XXXX`

## 语言切换功能

用户可以在Web界面左侧导航栏底部点击 🌐 按钮切换中英文：

- 默认语言：中文
- 语言偏好保存在浏览器 localStorage 中
- 切换后立即生效，无需刷新页面

## 注意事项

1. **ESP-IDF环境**：确保已正确安装ESP-IDF开发环境
2. **磁盘空间**：每个项目约需要500MB空间，6个项目共需约3GB
3. **编译时间**：单个项目编译约需5-10分钟，批量编译约需30-60分钟
4. **主线项目**：`SERIIAL_SERVER_V2_S` 是主线项目，其他项目都是从它复制的
5. **更新代码**：如果主线代码有更新，需要重新运行 `create_variants.sh`

## 常见问题

### Q: 如何只创建某几个版本？
A: 编辑 `script/create_variants.sh`，注释掉不需要的版本配置行。

### Q: 如何修改品牌名称或型号？
A: 编辑 `script/create_variants.sh` 中的 `VARIANTS` 数组配置。

### Q: 编译失败怎么办？
A: 
1. 检查ESP-IDF环境是否正确配置
2. 查看错误日志
3. 尝试单独编译失败的项目以获取详细错误信息

### Q: 如何添加新的型号？
A: 在 `script/create_variants.sh` 的 `VARIANTS` 数组中添加新配置行，格式为：
```
"目录名|型号|品牌类型|品牌中文名|品牌英文名"
```

## 技术支持

如有问题，请联系开发团队。
