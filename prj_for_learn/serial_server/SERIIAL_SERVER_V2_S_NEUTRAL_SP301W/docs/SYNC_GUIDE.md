# 代码同步指南

## 📋 概述

由于每个版本都是独立的项目目录，当你修改代码后需要同步到其他版本。本文档说明如何进行代码同步。

## 🎯 核心原则

**⚠️ 重要：只在主线项目修改代码！**

```
主线项目（SERIIAL_SERVER_V2_S）
    ↓ 同步
其他5个变体项目
```

- **主线项目**：`SERIIAL_SERVER_V2_S` - 唯一的代码维护点
- **变体项目**：其他5个项目 - 只接收同步，不直接修改

## 🔄 同步方法

### 方法1：使用同步脚本（推荐）

这是最简单安全的方法：

```bash
cd /home/master/project/serial_server/SERIIAL_SERVER_V2_S

# 交互式同步（会询问确认）
./script/sync_variants.sh

# 自动同步（不询问）
./script/sync_variants.sh -y
```

**脚本会自动：**
1. ✅ 备份各项目的配置文件
2. ✅ 从主线同步所有代码
3. ✅ 恢复各项目的配置文件
4. ✅ 保护特殊文件不被覆盖

**保护的文件（不会被覆盖）：**
- `configs/variant.h` - 变体配置
- `CMakeLists.txt` - 项目名称
- `main/CMakeLists.txt` - 编译定义

**排除的目录（不会同步）：**
- `.git` - Git仓库
- `build` - 编译输出
- `.cache` - 缓存
- `.vscode` - IDE配置

---

### 方法2：手动同步（不推荐）

如果你想手动同步某个特定文件：

```bash
# 同步单个文件
cp SERIIAL_SERVER_V2_S/main/sx_web_server.c \
   SERIIAL_SERVER_V2_S_LIKONG_SP401W/main/

# 同步整个目录
cp -r SERIIAL_SERVER_V2_S/main/static/ \
      SERIIAL_SERVER_V2_S_LIKONG_SP401W/main/
```

**⚠️ 注意：** 手动同步容易出错，不推荐使用。

---

## 📝 工作流程

### 日常开发流程

```bash
# 1. 在主线项目修改代码
cd /home/master/project/serial_server/SERIIAL_SERVER_V2_S
# 修改代码...

# 2. 在主线项目测试
idf.py build flash monitor

# 3. 确认无误后，同步到所有变体项目
./script/sync_variants.sh -y

# 4. 重新编译需要更新的变体项目
cd ../SERIIAL_SERVER_V2_S_LIKONG_SP401W
idf.py build

# 5. 测试变体项目
idf.py flash monitor
```

---

## 🔍 同步场景示例

### 场景1：修改了Web界面

```bash
# 1. 修改主线项目的HTML/CSS/JS
vim SERIIAL_SERVER_V2_S/main/static/web.html

# 2. 测试主线项目
cd SERIIAL_SERVER_V2_S
idf.py build flash

# 3. 同步到所有项目
./script/sync_variants.sh -y

# 4. 所有项目都会获得更新的Web界面
```

### 场景2：修改了串口处理逻辑

```bash
# 1. 修改主线项目的C代码
vim SERIIAL_SERVER_V2_S/main/sx_async_uart.c

# 2. 测试
cd SERIIAL_SERVER_V2_S
idf.py build flash monitor

# 3. 同步
./script/sync_variants.sh -y

# 4. 重新编译所有变体（可选，使用批量编译脚本）
./script/build_all_variants.sh
```

### 场景3：添加了新功能

```bash
# 1. 在主线项目添加新文件
vim SERIIAL_SERVER_V2_S/main/sx_new_feature.c
vim SERIIAL_SERVER_V2_S/main/include/sx_new_feature.h

# 2. 修改 main/CMakeLists.txt 添加新文件
vim SERIIAL_SERVER_V2_S/main/CMakeLists.txt

# 3. 测试
cd SERIIAL_SERVER_V2_S
idf.py build flash

# 4. 同步到所有项目
./script/sync_variants.sh -y

# 注意：main/CMakeLists.txt 是保护文件，需要手动更新各项目
# 或者修改同步脚本，临时不保护这个文件
```

### 场景4：更新了国际化翻译

```bash
# 1. 修改翻译文件
vim SERIIAL_SERVER_V2_S/main/static/i18n/zh-CN.json
vim SERIIAL_SERVER_V2_S/main/static/i18n/en-US.json

# 2. 同步（翻译文件会自动同步到所有项目）
cd SERIIAL_SERVER_V2_S
./script/sync_variants.sh -y

# 3. 不需要重新编译，因为这些是静态文件
# 直接刷新浏览器即可看到更新
```

---

## ⚠️ 注意事项

### 1. 不要直接修改变体项目的代码

**❌ 错误做法：**
```bash
# 不要这样做！
vim SERIIAL_SERVER_V2_S_LIKONG_SP401W/main/sx_web_server.c
```

**✅ 正确做法：**
```bash
# 在主线修改
vim SERIIAL_SERVER_V2_S/main/sx_web_server.c
# 然后同步
./script/sync_variants.sh -y
```

### 2. 同步前先提交代码

建议在同步前先提交主线项目的代码到Git：

```bash
cd SERIIAL_SERVER_V2_S
git add .
git commit -m "修改了XXX功能"
./script/sync_variants.sh -y
```

这样如果同步出问题，可以回退。

### 3. 同步后需要重新编译

同步只是复制代码文件，不会自动编译。需要手动重新编译：

```bash
# 单个项目
cd SERIIAL_SERVER_V2_S_LIKONG_SP401W
idf.py build

# 或批量编译所有项目
cd SERIIAL_SERVER_V2_S
./script/build_all_variants.sh
```

### 4. 特殊文件的处理

如果你需要修改保护的文件（如 `main/CMakeLists.txt`），有两种方法：

**方法A：手动更新各项目**
```bash
# 修改主线
vim SERIIAL_SERVER_V2_S/main/CMakeLists.txt

# 手动复制到各项目（注意保留编译定义部分）
# 需要手动合并
```

**方法B：临时修改同步脚本**
```bash
# 编辑 sync_variants.sh
# 临时注释掉 "main/CMakeLists.txt" 这一行
# 然后运行同步
# 同步后再恢复脚本
```

---

## 🛠️ 同步脚本详解

### 脚本功能

`script/sync_variants.sh` 做了以下事情：

1. **备份配置文件**
   ```bash
   configs/variant.h
   CMakeLists.txt
   main/CMakeLists.txt
   ```

2. **同步代码**
   - 使用 `rsync` 或 `cp` 复制文件
   - 排除 `.git`、`build` 等目录
   - 排除保护的配置文件

3. **恢复配置文件**
   - 将备份的配置文件恢复回去
   - 确保各项目的配置不被覆盖

4. **清理临时文件**
   - 删除备份目录

### 脚本选项

```bash
# 显示帮助
./script/sync_variants.sh --help

# 交互式同步（会询问确认）
./script/sync_variants.sh

# 自动同步（不询问）
./script/sync_variants.sh -y
```

---

## 📊 同步检查清单

同步后，建议检查以下内容：

- [ ] 各项目的 `configs/variant.h` 配置正确
- [ ] 各项目的 `CMakeLists.txt` 项目名称正确
- [ ] 各项目的 `main/CMakeLists.txt` 编译定义正确
- [ ] 主要代码文件已同步（检查修改时间）
- [ ] 重新编译至少一个变体项目，确认无编译错误
- [ ] 烧写测试，确认功能正常

---

## 🚀 快速参考

### 日常同步命令

```bash
# 1. 修改主线代码
cd /home/master/project/serial_server/SERIIAL_SERVER_V2_S
# ... 修改代码 ...

# 2. 测试主线
idf.py build flash

# 3. 同步到所有项目
./script/sync_variants.sh -y

# 4. 批量重新编译（可选）
./script/build_all_variants.sh
```

### 验证同步结果

```bash
# 检查文件修改时间
ls -l SERIIAL_SERVER_V2_S/main/sx_web_server.c
ls -l SERIIAL_SERVER_V2_S_LIKONG_SP401W/main/sx_web_server.c

# 对比文件内容
diff SERIIAL_SERVER_V2_S/main/sx_web_server.c \
     SERIIAL_SERVER_V2_S_LIKONG_SP401W/main/sx_web_server.c
```

---

## 🔧 故障排除

### Q1: 同步后编译失败

**可能原因：**
- 配置文件被覆盖了

**解决方法：**
```bash
# 重新运行创建脚本，会重新生成配置
./script/create_variants.sh
```

### Q2: 同步脚本报错 "rsync not found"

**解决方法：**
```bash
# 安装 rsync
sudo apt-get install rsync  # Ubuntu/Debian
sudo yum install rsync      # CentOS/RHEL

# 或者脚本会自动降级使用 cp 命令
```

### Q3: 想要同步特定的几个项目

**解决方法：**
编辑 `script/sync_variants.sh`，修改 `VARIANT_PROJECTS` 数组，注释掉不需要同步的项目。

---

## 📞 技术支持

如有问题，请参考：
- `BUILD_GUIDE.md` - 构建指南
- `FLASH_GUIDE.md` - 烧写指南
- `SUMMARY.md` - 完成总结
