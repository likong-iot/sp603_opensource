# 页面标题国际化问题修复

## 问题描述
用户反馈：切换页面时，页面标题（mainTitle）默认显示中文，需要重复切换语言后才会显示英文。而其他部分能根据状态正常切换。

## 问题分析

### 根本原因：
页面切换和语言切换使用了**不统一的机制**：

1. **导航菜单文本**：使用`data-i18n`属性，由`i18n.applyTranslations()`自动翻译 ✓
2. **页面标题（mainTitle）**：由`switchView()`函数直接设置硬编码中文 ✗

### 问题流程：
```
用户切换页面 → switchView()被调用 
→ mainTitleElement.textContent = "基本信息" (硬编码中文)
→ 即使当前语言是英文，标题仍显示中文
```

## 解决方案

### 1. 修改switchView函数（web.js）

**修改前：**
```javascript
// 更新页面标题
if (mainTitleElement) {
    mainTitleElement.textContent = mainTitle; // 直接使用硬编码中文
}
```

**修改后：**
```javascript
// 更新页面标题 - 使用国际化
if (mainTitleElement) {
    // 根据mainTitle查找对应的翻译键
    const titleMap = {
        '基本信息': 'nav.basicInfo',
        '串口管理': 'nav.serialConfig',
        '系统管理': 'nav.sysConfig',
        '协议管理': 'nav.protocolConfig',
        '网络管理': 'nav.netConfig',
        '日志管理': 'nav.logConfig'
    };

    const i18nKey = titleMap[mainTitle];
    if (i18nKey && window.i18n) {
        mainTitleElement.textContent = window.i18n.t(i18nKey);
    } else {
        mainTitleElement.textContent = mainTitle;
    }
}
```

### 2. 在i18n.js中添加updateMainTitle方法

**新增方法：**
```javascript
updateMainTitle() {
    const mainTitleElement = document.getElementById('mainTitle');
    if (!mainTitleElement) return;

    // 获取当前激活的导航项
    const activeNav = document.querySelector('.nav-link.active');
    if (!activeNav) return;

    // 根据激活的导航项ID确定翻译键
    const navId = activeNav.id.replace('_m', ''); // 移除移动端后缀
    const titleMap = {
        'basicInfo': 'nav.basicInfo',
        'serialConfig': 'nav.serialConfig',
        'sysConfig': 'nav.sysConfig',
        'protocolConfig': 'nav.protocolConfig',
        'netConfig': 'nav.netConfig',
        'logConfig': 'nav.logConfig'
    };

    const i18nKey = titleMap[navId];
    if (i18nKey) {
        mainTitleElement.textContent = this.t(i18nKey);
    }
}
```

### 3. 在applyTranslations中调用updateMainTitle

```javascript
applyTranslations() {
    // ... 其他翻译逻辑 ...
    
    // 更新版本信息
    this.updateVersionInfo();

    // 更新当前页面标题（mainTitle）
    this.updateMainTitle();
}
```

## 修复效果

### 修复前：
1. 用户切换到英文界面
2. 点击"Network"导航菜单
3. 页面标题显示"网络管理"（中文）❌
4. 需要再次点击语言切换按钮才会显示"Network"

### 修复后：
1. 用户切换到英文界面
2. 点击"Network"导航菜单
3. 页面标题立即显示"Network"（英文）✓
4. 所有文本保持一致的语言状态

## 机制统一

现在所有文本都使用统一的国际化机制：

| 元素类型 | 翻译方式 | 触发时机 |
|---------|---------|---------|
| 导航菜单 | `data-i18n` + `i18n.applyTranslations()` | 页面加载、语言切换 |
| 页面标题 | `i18n.t()` + `updateMainTitle()` | 页面切换、语言切换 |
| 动态文本 | `i18n.t()` | 数据更新时 |
| 输入占位符 | `data-i18n-placeholder` + `i18n.applyTranslations()` | 页面加载、语言切换 |

## 测试验证

### 测试场景1：切换页面
1. 设置界面语言为英文
2. 点击不同的导航菜单项
3. **预期**：页面标题始终显示英文 ✓

### 测试场景2：切换语言
1. 在任意页面
2. 点击语言切换按钮
3. **预期**：页面标题立即切换语言 ✓

### 测试场景3：刷新页面
1. 设置界面语言为英文
2. 刷新浏览器
3. **预期**：页面标题显示英文（从localStorage读取语言偏好）✓

## 同步状态

✅ 已同步到所有6个固件版本：
- SERIIAL_SERVER_V2_S
- SERIIAL_SERVER_V2_S_LIKONG_SP301W
- SERIIAL_SERVER_V2_S_LIKONG_SP401W
- SERIIAL_SERVER_V2_S_NEUTRAL_SP301W
- SERIIAL_SERVER_V2_S_NEUTRAL_SP401W
- SERIIAL_SERVER_V2_S_NEUTRAL_SP501W

## 总结

通过统一页面标题的国际化机制，现在整个界面的所有文本（导航菜单、页面标题、动态内容）都能保持一致的语言状态，无论是切换页面还是切换语言，都能正确显示当前选择的语言。
