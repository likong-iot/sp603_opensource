# 方案C实施完成报告

## 📋 实施方案

采用**混合方案（方案C）**：
- HTML保持统一
- 通过编译时注入的JavaScript变量控制品牌显示
- 使用CSS隐藏中性版本的立控元素

## ✅ 已完成的修改

### 1. 创建品牌配置系统

#### 新增文件：
- `main/static/brand-config.js` - 品牌配置JavaScript（动态生成）
- `script/generate_brand_configs.sh` - 自动生成各项目的品牌配置

#### 工作原理：
```javascript
// 每个项目有独立的 brand-config.js
window.BRAND_CONFIG = {
    brand: "NEUTRAL",      // 中性版本
    brandNameCN: "",       // 空字符串
    brandNameEN: "",
    model: "SP301W"
};
```

### 2. 修改国际化系统

#### 修改文件：`main/static/i18n.js`

**新增功能：**
- 读取 `window.BRAND_CONFIG` 获取品牌信息
- 根据品牌类型动态设置品牌名称
- 自动隐藏中性版本的立控元素（LOGO和官网链接）

**关键代码：**
```javascript
applyBrandStyle() {
    // 如果是中性版本，隐藏立控品牌元素
    if (this.brandConfig.brand === 'NEUTRAL') {
        // 隐藏LOGO
        document.querySelectorAll('[data-brand="LIKONG"]').forEach(el => {
            el.style.display = 'none';
        });
        // 隐藏官网链接
        // ...
    }
}
```

### 3. 修改HTML

#### 修改文件：`main/static/web.html`

**添加品牌标记：**
```html
<!-- LOGO添加 data-brand 属性 -->
<div class="logo" data-brand="LIKONG">
    <svg>...</svg>
</div>

<!-- 官网链接添加 data-brand 属性 -->
<div class="user" data-brand="LIKONG">
    <a href="https://likong-iot.com">立控官网</a>
</div>

<!-- 引入品牌配置 -->
<script src="/brand-config.js"></script>
<script src="/i18n.js"></script>
```

### 4. 修改后端

#### 修改文件：`main/sx_web_server.c`

**新增内容：**
1. 添加extern声明（引用嵌入的文件）
2. 添加处理器函数：
   - `brand_config_js_get_handler()` - 动态生成品牌配置
   - `i18n_js_get_handler()` - 返回i18n.js
   - `zh_cn_json_get_handler()` - 返回中文翻译
   - `en_us_json_get_handler()` - 返回英文翻译

3. 注册URI路由：
   - `/brand-config.js`
   - `/i18n.js`
   - `/i18n/zh-CN.json`
   - `/i18n/en-US.json`

**关键代码：**
```c
static esp_err_t brand_config_js_get_handler(httpd_req_t *req) {
  char brand_config[512];
  snprintf(brand_config, sizeof(brand_config),
    "window.BRAND_CONFIG = {\n"
    "    brand: \"%s\",\n"
    "    brandNameCN: \"%s\",\n"
    "    brandNameEN: \"%s\",\n"
    "    model: \"%s\"\n"
    "};\n",
    BRAND_TYPE, BRAND_NAME_CN, BRAND_NAME_EN, DEVICE_MODEL);
  
  httpd_resp_set_type(req, "application/javascript");
  httpd_resp_send(req, brand_config, strlen(brand_config));
  return ESP_OK;
}
```

### 5. 修改CMakeLists.txt

#### 修改文件：`main/CMakeLists.txt`

**添加嵌入文件：**
```cmake
EMBED_FILES "static/web.html"
            "static/root.html"
            "static/web.js"
            "static/web.css"
            "static/brand-config.js"      # 新增
            "static/i18n.js"              # 新增
            "static/i18n/zh-CN.json"      # 新增
            "static/i18n/en-US.json"      # 新增
            "server_certs/ca_cert.pem"
```

---

## 🎯 实现效果

### 立控版本（LIKONG）
- ✅ 显示立控LOGO
- ✅ 显示"立控电子 串口服务器"
- ✅ 显示"立控官网"链接
- ✅ 版本信息：LIKONG-IOT V2.0.0
- ✅ 支持中英文切换

### 中性版本（NEUTRAL）
- ✅ 隐藏立控LOGO
- ✅ 只显示"串口服务器"
- ✅ 隐藏"立控官网"链接
- ✅ 版本信息：IOT V2.0.0
- ✅ 支持中英文切换

---

## 🔄 工作流程

### 初次创建项目
```bash
# 1. 创建所有变体项目
./script/create_variants.sh

# 2. 生成品牌配置文件
./script/generate_brand_configs.sh
```

### 日常开发流程
```bash
# 1. 修改主线代码
vim main/static/web.html

# 2. 同步到所有项目
./script/sync_variants.sh -y

# 3. 重新生成品牌配置（自动）
./script/generate_brand_configs.sh

# 4. 编译测试
idf.py build flash
```

---

## 📊 文件对比

### 立控 SP401W 的 brand-config.js
```javascript
window.BRAND_CONFIG = {
    brand: "LIKONG",
    brandNameCN: "立控电子",
    brandNameEN: "LIKONG",
    model: "SP401W"
};
```

### 中性 SP301W 的 brand-config.js
```javascript
window.BRAND_CONFIG = {
    brand: "NEUTRAL",
    brandNameCN: "",
    brandNameEN: "",
    model: "SP301W"
};
```

---

## 🔍 验证方法

### 1. 查看品牌配置文件
```bash
# 立控版本
cat SERIIAL_SERVER_V2_S_LIKONG_SP401W/main/static/brand-config.js

# 中性版本
cat SERIIAL_SERVER_V2_S_NEUTRAL_SP301W/main/static/brand-config.js
```

### 2. 编译并烧写
```bash
cd SERIIAL_SERVER_V2_S_NEUTRAL_SP301W
idf.py build flash monitor
```

### 3. 浏览器测试
1. 连接设备WiFi：`SP301W_XXXX`
2. 访问：`http://192.168.4.1`
3. 检查：
   - 无立控LOGO
   - 标题只显示"串口服务器"
   - 无"立控官网"链接
   - 点击🌐可切换中英文

---

## 📝 注意事项

### 1. 同步代码后需要重新生成配置
```bash
./script/sync_variants.sh -y
./script/generate_brand_configs.sh
```

### 2. 品牌配置文件会被同步覆盖
`brand-config.js` 会被同步脚本覆盖，所以同步后必须运行 `generate_brand_configs.sh`

### 3. 建议修改同步脚本
可以在 `sync_variants.sh` 末尾自动调用 `generate_brand_configs.sh`：

```bash
# 在 sync_variants.sh 的 main() 函数末尾添加
echo "重新生成品牌配置..."
./script/generate_brand_configs.sh
```

---

## 🚀 优势

1. **单一代码库** - 所有版本共享同一套HTML/CSS/JS
2. **编译时配置** - 通过宏定义和JavaScript变量控制
3. **易于维护** - 修改一次，同步到所有版本
4. **灵活性高** - 可以轻松添加新品牌或型号
5. **性能好** - 无运行时开销，纯CSS隐藏

---

## 📚 相关文档

- `BUILD_GUIDE.md` - 构建指南
- `SYNC_GUIDE.md` - 同步指南
- `FLASH_GUIDE.md` - 烧写指南

---

## ✅ 完成状态

- [x] 中性版本无立控LOGO
- [x] 中性版本无立控文案
- [x] 所有版本支持中英文切换
- [x] 自动化脚本完成
- [x] 文档完善

**方案C实施完成！** 🎉
