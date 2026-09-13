// 国际化支持 - 重构版本
const i18n = {
    currentLang: 'zh-CN',
    translations: {},
    brandConfig: null,
    translatedElements: new WeakSet(), // 跟踪已翻译的元素，避免重复

    async init() {
        // 获取品牌配置（从全局变量）
        this.brandConfig = window.BRAND_CONFIG || {
            brand: 'LIKONG',
            brandNameCN: '立控电子',
            brandNameEN: 'LIKONG',
            model: 'SP501W'
        };

        // 从localStorage读取用户语言偏好
        const savedLang = localStorage.getItem('language') || 'zh-CN';
        await this.loadLanguage(savedLang);

        // 应用品牌样式
        this.applyBrandStyle();
    },

    async loadLanguage(lang) {
        try {
            const response = await fetch(`/i18n/${lang}.json?v=${Date.now()}`, {
                cache: 'no-store'
            });
            this.translations = await response.json();

            // 根据品牌类型覆盖品牌名称
            if (this.brandConfig.brand === 'NEUTRAL') {
                this.translations.brandName = '';
            } else {
                this.translations.brandName = lang === 'zh-CN' ?
                    this.brandConfig.brandNameCN : this.brandConfig.brandNameEN;
            }

            this.currentLang = lang;
            localStorage.setItem('language', lang);

            // 清除已翻译标记，重新翻译
            this.translatedElements = new WeakSet();
            this.applyTranslations();

            // 更新语言切换按钮
            this.updateLangButton();

            window.dispatchEvent(new CustomEvent('languageChanged', {
                detail: { lang }
            }));
        } catch (error) {
            console.error('Failed to load language file:', error);
        }
    },

    t(key) {
        const keys = key.split('.');
        let value = this.translations;
        for (const k of keys) {
            value = value?.[k];
        }
        return value || key;
    },

    applyTranslations() {
        // 更新页面标题
        const title = this.t('title');
        if (this.brandConfig.brand === 'NEUTRAL') {
            document.title = title;
        } else {
            const brandName = this.currentLang === 'zh-CN' ? '立控' : 'LIKONG';
            document.title = `${brandName}${title}`;
        }

        // 更新所有带data-i18n属性的元素
        // 每个元素只更新自己的直接文本节点，嵌套的data-i18n子元素继续单独翻译
        const elements = document.querySelectorAll('[data-i18n]');

        elements.forEach(el => {
            const key = el.getAttribute('data-i18n');
            const translation = this.t(key);

            // 检查元素是否有子元素
            const hasChildElements = el.children.length > 0;

            if (hasChildElements) {
                // 如果有子元素，只更新直接文本节点
                this.updateTextNodes(el, translation);
            } else {
                // 如果没有子元素，直接设置textContent
                el.textContent = translation;
            }
        });

        // 更新所有带data-i18n-placeholder属性的元素
        document.querySelectorAll('[data-i18n-placeholder]').forEach(el => {
            const key = el.getAttribute('data-i18n-placeholder');
            el.placeholder = this.t(key);
        });

        // 更新导航栏标题
        const navTitles = document.querySelectorAll('.logo-title .title');
        navTitles.forEach(el => {
            if (this.brandConfig.brand === 'NEUTRAL') {
                el.textContent = title;
            } else {
                const brandName = this.currentLang === 'zh-CN' ? '立控' : 'LIKONG';
                el.textContent = `${brandName}${title}`;
            }
        });

        // 更新版本信息
        this.updateVersionInfo();

        // 更新当前页面标题（mainTitle）
        this.updateMainTitle();
    },

    // 只更新元素的直接文本节点，保留子元素
    updateTextNodes(element, text) {
        // 遍历所有直接子节点
        const childNodes = Array.from(element.childNodes);
        let textNodeFound = false;

        childNodes.forEach(node => {
            // 如果是文本节点（nodeType === 3）
            if (node.nodeType === 3) {
                const trimmedText = node.textContent.trim();
                // 只更新非空的文本节点
                if (trimmedText) {
                    node.textContent = text;
                    textNodeFound = true;
                }
            }
        });

        // 如果没有找到文本节点，在开头插入一个
        if (!textNodeFound) {
            // 在第一个子元素之前插入文本节点
            const textNode = document.createTextNode(text);
            element.insertBefore(textNode, element.firstChild);
        }
    },

    updateVersionInfo() {
        const versionDivs = document.querySelectorAll('.version');
        versionDivs.forEach(div => {
            let versionText = '';
            if (this.brandConfig.brand === 'LIKONG') {
                versionText = this.currentLang === 'zh-CN' ?
                    'LIKONG-IOT V2.0.0' : 'LIKONG-IOT V2.0.0';
            } else {
                versionText = 'IOT V2.0.0';
            }
            div.textContent = versionText;
        });
    },

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
    },

    applyBrandStyle() {
        // 根据品牌类型显示或隐藏品牌元素
        const brandElements = document.querySelectorAll('[data-brand="LIKONG"]');

        if (this.brandConfig.brand === 'LIKONG') {
            // 立控版本：显示LOGO和官网链接
            brandElements.forEach(el => {
                el.style.display = '';
            });
        } else {
            // 中性版本：隐藏LOGO和官网链接
            brandElements.forEach(el => {
                el.style.display = 'none';
            });
        }
    },

    async switchLanguage() {
        const newLang = this.currentLang === 'zh-CN' ? 'en-US' : 'zh-CN';
        await this.loadLanguage(newLang);

        // 更新按钮文字（在语言加载完成后）
        this.updateLangButton();
    },

    updateLangButton() {
        const langBtn = document.getElementById('langText');
        if (langBtn) {
            // 显示目标语言（当前是中文，显示EN表示点击后切换到英文）
            langBtn.textContent = this.currentLang === 'zh-CN' ? 'EN' : '中文';
        }
    },

    // 提供给外部调用的方法，用于动态添加的内容
    translateElement(element) {
        if (!element) return;

        const key = element.getAttribute('data-i18n');
        if (key) {
            const translation = this.t(key);
            const hasChildElements = element.children.length > 0;

            if (hasChildElements) {
                this.updateTextNodes(element, translation);
            } else {
                element.textContent = translation;
            }
        }

        // 递归翻译子元素
        element.querySelectorAll('[data-i18n]').forEach(child => {
            this.translateElement(child);
        });
    }
};

// 页面加载完成后初始化国际化
document.addEventListener('DOMContentLoaded', () => {
    i18n.init().then(() => {
        // 初始化完成后更新按钮
        i18n.updateLangButton();
    });

    // 添加语言切换按钮事件
    const langBtn = document.getElementById('langSwitch');
    if (langBtn) {
        langBtn.addEventListener('click', () => {
            i18n.switchLanguage();
        });
    }
});

// 导出到全局，供其他脚本使用
window.i18n = i18n;
