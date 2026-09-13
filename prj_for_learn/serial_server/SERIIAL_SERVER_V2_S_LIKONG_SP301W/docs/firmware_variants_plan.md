# 串口服务器固件多版本实施方案

## 一、需求分析

### 1. 需要创建的固件版本（共6个）

**命名规则说明：**
- **W后缀**：表示WiFi产品，所有型号都带W
- **无L后缀**：去掉立控的L标识
- **型号统一**：SP501W（主线）、SP401W、SP301W
- **项目目录**：需要明确标识品牌（立控/无标）和型号

| 品牌类型 | 型号 | WiFi SSID前缀 | 项目目录名 | 界面标题 | 说明 |
|---------|------|--------------|-----------|---------|------|
| 立控 | SP501W | SP501W_XXXX | SERIIAL_SERVER_V2_S | 立控电子 串口服务器 | 当前主线 |
| 立控 | SP401W | SP401W_XXXX | SERIIAL_SERVER_V2_S_LIKONG_SP401W | 立控电子 串口服务器 | 立控新型号 |
| 立控 | SP301W | SP301W_XXXX | SERIIAL_SERVER_V2_S_LIKONG_SP301W | 立控电子 串口服务器 | 立控新型号 |
| 中性/无标 | SP501W | SP501W_XXXX | SERIIAL_SERVER_V2_S_NEUTRAL_SP501W | 串口服务器 | 中性版本 |
| 中性/无标 | SP401W | SP401W_XXXX | SERIIAL_SERVER_V2_S_NEUTRAL_SP401W | 串口服务器 | 中性新型号 |
| 中性/无标 | SP301W | SP301W_XXXX | SERIIAL_SERVER_V2_S_NEUTRAL_SP301W | 串口服务器 | 中性新型号 |

注：
- XXXX 为设备MAC地址后4位
- 主线项目 SERIIAL_SERVER_V2_S 保持不变（立控 SP501W）
- 其他5个版本需要创建新的项目目录

### 2. 其他需求

1. **界面样式统一**：将串口管理和系统管理页面改为居中对齐
2. **国际化支持**：增加英文界面，支持中英文切换

## 二、技术实施方案

### 方案选择：编译时配置 + 构建脚本

采用以下技术方案：
1. 在代码中使用宏定义来区分品牌和型号
2. 创建多个 `sdkconfig` 配置文件
3. 编写构建脚本自动编译所有版本

### 优点：
- 单一代码库，易于维护
- 编译时确定配置，无运行时开销
- 可以批量构建所有版本

## 三、需要修改的文件

### 1. 核心配置文件

#### `main/include/sx_web_server.h`
```c
// 当前定义
#define VERSION "HW:1.0.0_SDK:2.2.1"
#define DEVICE_TYPE "SP501LW"  // 需要改为 SP501W
#define CLIENT_HEAD "SP501"

// 需要改为可配置
#ifndef DEVICE_MODEL
#define DEVICE_MODEL "SP501W"  // SP501W / SP401W / SP301W
#endif

#ifndef BRAND_TYPE
#define BRAND_TYPE "LIKONG"  // LIKONG 或 NEUTRAL
#endif

#ifndef BRAND_NAME_CN
#define BRAND_NAME_CN "立控电子"  // 立控电子 或 空字符串
#endif
```

#### `main/sx_web_server.c`
```c
// 第379行
#define ESP_WIFI_SSID "SP501LW"  // 需要改为 SP501W

// 需要改为
#define ESP_WIFI_SSID DEVICE_MODEL
```

### 2. 前端文件

#### `main/static/web.html`
- 第16行：`<title>立控电子 串口服务器</title>` - 需要支持品牌切换
- 第66行：`<div class="version">LIKONG-IOT V2.0.0</div>` - 需要动态显示

#### `main/static/web.css`
- 需要查找串口管理和系统管理的样式，改为居中对齐

#### `main/static/web.js`
- 需要添加国际化支持（i18n）
- 添加语言切换功能

### 3. 构建配置

#### `CMakeLists.txt`
```cmake
# 当前
project(SP501LW)

# 需要改为
project(${DEVICE_MODEL})
```

## 四、实施步骤

### 阶段1：代码重构（支持多版本）
1. 修改头文件，使用宏定义
2. 修改C代码中的硬编码字符串
3. 修改HTML模板，支持动态内容

### 阶段2：创建构建系统
1. 创建配置文件目录 `configs/`
2. 为每个版本创建配置文件
3. 编写构建脚本 `build_all_variants.sh`

### 阶段3：界面优化
1. 统一CSS样式（居中对齐）
2. 实现国际化支持
3. 添加语言切换按钮

### 阶段4：测试验证
1. 编译所有版本
2. 验证WiFi SSID正确性
3. 验证界面显示正确性

## 五、文件结构

```
serial_server/                              # 项目根目录
├── SERIIAL_SERVER_V2_S/                   # 主线：立控 SP501W
├── SERIIAL_SERVER_V2_S_LIKONG_SP401W/     # 新增：立控 SP401W
├── SERIIAL_SERVER_V2_S_LIKONG_SP301W/     # 新增：立控 SP301W
├── SERIIAL_SERVER_V2_S_NEUTRAL_SP501W/    # 新增：中性 SP501W
├── SERIIAL_SERVER_V2_S_NEUTRAL_SP401W/    # 新增：中性 SP401W
└── SERIIAL_SERVER_V2_S_NEUTRAL_SP301W/    # 新增：中性 SP301W

每个项目目录结构：
SERIIAL_SERVER_V2_S/
├── configs/                    # 新增：配置文件目录
│   └── variant.h              # 版本配置头文件
├── script/
│   ├── build_all_variants.sh  # 新增：批量构建脚本（放在主线）
│   └── create_variants.sh     # 新增：创建变体项目脚本
├── main/
│   ├── include/
│   │   ├── sx_web_server.h    # 修改：使用宏定义
│   │   └── sx_variant.h       # 新增：变体配置
│   └── static/
│       ├── web.html           # 修改：支持动态内容
│       ├── web.css            # 修改：统一样式
│       ├── web.js             # 修改：添加i18n
│       └── i18n/              # 新增：国际化资源
│           ├── zh-CN.json
│           └── en-US.json
└── build/                     # 编译输出
```

## 六、预计工作量

- 代码重构：2-3小时
- 构建系统：1-2小时
- 界面优化：2-3小时
- 国际化实现：3-4小时
- 测试验证：2小时

**总计：10-14小时**
