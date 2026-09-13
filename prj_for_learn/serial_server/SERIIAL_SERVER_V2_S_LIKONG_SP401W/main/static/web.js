let hasRestarted = false;

// 配置缓存，防止工作模式切换时配置丢失
let configCache = {
    modbusItems: null,
    pollTime: null,
    lastWorkMode: null
};

const MODBUS_ALLOWED_FUNCTION_CODES = ['01', '02', '03', '04', '05', '06'];
const MODBUS_ALLOWED_DATA_FORMATS = [
    'Signed', 'Unsigned', 'HEX', 'Binary', 'Long',
    'Float', 'Double', 'LongInverse', 'FloatInverse', 'DoubleInverse'
];
const MODBUS_ALLOWED_REPORT_FORMATS = ['mqtt', 'tcp', 'http'];
const MODBUS_ALLOWED_BAUD_RATES = ['1200', '2400', '4800', '9600', '19200', '38400', '57600', '115200'];
const MODBUS_ALLOWED_DATA_BITS = ['5', '6', '7', '8'];
const MODBUS_ALLOWED_STOP_BITS = ['1', '1.5', '2'];
const MODBUS_ALLOWED_CHECK_BITS = ['None', 'Odd', 'Even'];
const MODBUS_ITEM_MAX = 50;
const WORK_MODE_PAGE_SIZE = 10;

function getWebSocketClientTag() {
    try {
        const params = new URLSearchParams(window.location.search);
        const queryTag = params.get('clientTag') || params.get('client_tag');
        if (queryTag && queryTag.trim()) {
            const normalized = queryTag.trim();
            localStorage.setItem('wsClientTag', normalized);
            return normalized;
        }
    } catch (error) {
        console.warn('无法解析URL中的clientTag参数:', error);
    }

    try {
        const savedTag = localStorage.getItem('wsClientTag');
        return savedTag ? savedTag.trim() : '';
    } catch (error) {
        console.warn('无法读取本地存储的wsClientTag:', error);
        return '';
    }
}

function buildWebSocketUrl(path) {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = new URL(`${protocol}//${window.location.host}${path}`);
    const tag = getWebSocketClientTag();
    if (tag) {
        url.searchParams.set('client_tag', tag);
    }
    return url.toString();
}

// 保存当前Modbus配置到缓存
function saveCurrentConfigToCache() {
    const container = document.getElementById('modbus_items_container');
    if (!container) return;
    
    const rows = container.querySelectorAll('.modbus-table-row');
    if (rows.length === 0) return;
    
    const items = [];
    rows.forEach((row, index) => {
        const inputs = row.querySelectorAll('.base-input');
        const switchBtn = row.querySelector('.switch-btn');
        const enabled = switchBtn && switchBtn.classList.contains('on');
        
        items.push({
            enabled: enabled,
            slave_addr: inputs[0]?.value || '',
            function_code: inputs[1]?.value || '',
            register_addr: inputs[2]?.value || '',
            register_num: inputs[3]?.value || '',
            timeout: inputs[4]?.value || '',
            data_format: inputs[5]?.value || '',
            interval_time: inputs[6]?.value || '',
            report_format: inputs[7]?.value || '',
            baud_rate: inputs[8]?.value || '',
            data_bit: inputs[9]?.value || '',
            check_bit: inputs[10]?.value || '',
            stop_bit: inputs[11]?.value || ''
        });
    });
    
    const pollTimeInput = document.getElementById('poll_time');
    const pollTime = pollTimeInput ? (parseInt(pollTimeInput.value) * 1000).toString() : '30000';
    
    configCache.modbusItems = items;
    configCache.pollTime = pollTime;
    
    console.log(`已保存 ${items.length} 个Modbus配置项到缓存`);
}

// Basic info API
const basicInfoElementIds = ['host_name', 'uptime', 'net_mode', 'eth_mac', 'eth_ip', 'sta_mac', 'sta_ip', 'is_dhcps', 'internet_status'];
let basicInfoElements = {};

// 初始化基本信息元素引用
function initBasicInfoElements() {
    if (Object.keys(basicInfoElements).length === 0) {
        console.log('Initializing basic info elements...');
        basicInfoElements = basicInfoElementIds.reduce((obj, id) => {
            const element = document.getElementById(id);
            if (element) {
                obj[id] = element;
                console.log(`✓ Found element: ${id}`);
            } else {
                console.warn(`✗ Element with id '${id}' not found`);
            }
            return obj;
        }, {});
        console.log(`Basic info elements initialized: ${Object.keys(basicInfoElements).length}/${basicInfoElementIds.length}`);
    }
}

function translateNetworkStatus(statusText) {
    const status = statusText || '';
    const t = (key) => window.i18n ? window.i18n.t(key) : key;

    if (status.includes('已连接') || status.includes('Connected')) {
        if (status.includes('外网模式') || status.includes('Internet')) {
            return {
                text: t('common.connectedExternal'),
                className: 'connected-external'
            };
        }

        if (status.includes('内网模式') || status.includes('Intranet')) {
            return {
                text: t('common.connectedInternal'),
                className: 'connected-internal'
            };
        }

        return {
            text: t('common.connected'),
            className: 'connected-external'
        };
    }

    return {
        text: t('network.notConnected'),
        className: 'disconnected'
    };
}

function updateInternetStatusElement(statusText) {
    if (!basicInfoElements.internet_status) return;

    const translatedStatus = translateNetworkStatus(statusText);
    basicInfoElements.internet_status.textContent = translatedStatus.text;

    basicInfoElements.internet_status.classList.remove('network-status', 'connected-external', 'connected-internal', 'disconnected');
    basicInfoElements.internet_status.classList.add('network-status', translatedStatus.className);
}

async function updateBasicInfo() {
    try {
        // 确保DOM元素已初始化
        initBasicInfoElements();
        
        const response = await fetch('/devinfo');
        if (!response.ok) {
            console.error('Failed to fetch data. Status:', response.status);
            return;
        }
        const data = await response.json();
        
        // 安全地更新元素内容，检查元素是否存在
        if (basicInfoElements.host_name) {
            basicInfoElements.host_name.textContent = data.host_names ||
                (window.i18n ? window.i18n.t('basicInfo.deviceName') : '以太网串口服务器');
        }
        
        // 安全地更新系统时间
        if (basicInfoElements.uptime) {
            const now = new Date();
            const timeString = now.getFullYear() + '-' +
                              String(now.getMonth() + 1).padStart(2, '0') + '-' +
                              String(now.getDate()).padStart(2, '0') + ' ' +
                              String(now.getHours()).padStart(2, '0') + ':' +
                              String(now.getMinutes()).padStart(2, '0') + ':' +
                              String(now.getSeconds()).padStart(2, '0');
            basicInfoElements.uptime.textContent = timeString;
        }
        
        // 安全地更新其他基本信息
        if (basicInfoElements.eth_mac) {
            basicInfoElements.eth_mac.textContent = data.device_eth_mac || '';
        }
        if (basicInfoElements.eth_ip) {
            basicInfoElements.eth_ip.textContent = data.eth_ip || '';
        }
        if (basicInfoElements.sta_mac) {
            basicInfoElements.sta_mac.textContent = data.device_sta_mac || '';
        }
        if (basicInfoElements.is_dhcps) {
            basicInfoElements.is_dhcps.textContent = data.is_dhcp == 2 ?
                (window.i18n ? window.i18n.t('network.staticIP') : 'STATIC(静态IP)') :
                (window.i18n ? window.i18n.t('network.dhcp') : 'DHCP(动态IP)');
        }
        if (basicInfoElements.net_mode) {
            basicInfoElements.net_mode.textContent = data.netconn == 2 ?
                (window.i18n ? window.i18n.t('network.wifiMode') : 'WIFI(无线连接)') :
                (window.i18n ? window.i18n.t('network.ethernet') : '以太网(有线连接)');
        }
        if (basicInfoElements.sta_ip) {
            basicInfoElements.sta_ip.textContent = data.sta_ip || '';
        }
        updateInternetStatusElement(data.internet_status);
    } catch (error) {
        console.error('Failed to fetch basic info data:', error);
        // 如果元素未找到，可能是DOM还未完全加载，延迟重试
        if (Object.keys(basicInfoElements).length === 0) {
            console.log('DOM elements not ready, retrying in 500ms...');
            setTimeout(() => {
                updateBasicInfo();
            }, 500);
        }
    }
}

// 延迟初始化基本信息，确保DOM已加载
setTimeout(() => {
    updateBasicInfo();
    setInterval(updateBasicInfo, 5000);
}, 100);

// Post Get basic function

async function postData(url = '', data = {}) {
    const response = await fetch(url, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(data)
    });
    
    const result = await response.json();
    
    // 检查响应状态和业务逻辑错误
    if (!response.ok || (result.code && result.code !== 200)) {
        const error = new Error(result.msg || `HTTP error! status: ${response.status}`);
        error.status = response.status;
        error.code = result.code;
        throw error;
    }
    
    return result;
}

async function fetchData(url = '') {
    try {
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), 5000); // 5秒超时

        const response = await fetch(url, {
            signal: controller.signal
        });

        clearTimeout(timeoutId); // 清除超时定时器

        if (!response.ok) {
            throw new Error(`HTTP错误! 状态码: ${response.status}`);
        }

        return await response.json();
    } catch (error) {
        // 处理不同类型的错误
        if (error.name === 'AbortError') {
            throw new Error('请求超时，请检查网络连接');
        } else if (error.name === 'TypeError' && error.message.includes('Failed to fetch')) {
            throw new Error('网络连接失败，请检查设备是否在线');
        } else {
            throw error; // 传递其他错误
        }
    }
}

async function fetchWorkModeInfoPage(offset = 0, limit = WORK_MODE_PAGE_SIZE) {
    const params = new URLSearchParams();
    params.set('items_offset', String(offset));
    params.set('items_limit', String(limit));
    const url = `/work_mode_info?${params.toString()}`;
    return await fetchData(url);
}

async function fetchWorkModeInfoAllItems() {
    const firstPage = await fetchWorkModeInfoPage(0, WORK_MODE_PAGE_SIZE);
    const total = Number.isFinite(firstPage.modbus_items_total)
        ? firstPage.modbus_items_total
        : (firstPage.modbus_items ? firstPage.modbus_items.length : 0);
    const items = Array.isArray(firstPage.modbus_items)
        ? [...firstPage.modbus_items]
        : [];

    let offset = items.length;
    while (offset < total) {
        const page = await fetchWorkModeInfoPage(offset, WORK_MODE_PAGE_SIZE);
        if (Array.isArray(page.modbus_items) && page.modbus_items.length > 0) {
            items.push(...page.modbus_items);
            offset += page.modbus_items.length;
        } else {
            break;
        }
    }

    firstPage.modbus_items = items;
    return firstPage;
}

// Serial config toggle

// 添加折叠功能
function initializeToggle(toggleId, contentId) {
    const toggleBtn = document.getElementById(toggleId);
    const contentArea = document.getElementById(contentId);
    let isCollapsed = false;

    toggleBtn.addEventListener('click', () => {
        isCollapsed = !isCollapsed;
        toggleBtn.classList.toggle('collapsed');

        if (isCollapsed) {
            contentArea.style.maxHeight = '0';
            contentArea.classList.add('collapsed');
        } else {
            contentArea.style.maxHeight = contentArea.scrollHeight + 'px';
            contentArea.classList.remove('collapsed');
        }
    });
}

// 页面加载时初始化折叠功能
document.addEventListener('DOMContentLoaded', () => {
    // 初始化基本信息显示（确保DOM已完全加载）
    initBasicInfoElements();
    updateBasicInfo();
    
    // 初始化工作模式的折叠功能
    initializeToggle('workModeToggle', 'workModeContent');

    // 初始化串口设置的折叠功能
    initializeToggle('serialConfigToggle', 'serialConfigContent');
});

window.addEventListener('languageChanged', () => {
    updateBasicInfo();
});

// 在内容变化时更新最大高度
const resizeObserver = new ResizeObserver(entries => {
    entries.forEach(entry => {
        if (!entry.target.classList.contains('collapsed')) {
            entry.target.style.maxHeight = entry.target.scrollHeight + 'px';
        }
    });
});

// 观察内容区域的变化
document.querySelectorAll('.content-area').forEach(area => {
    resizeObserver.observe(area);
});


// Work mode API


// 修改工作模式切换事件处理
document.querySelectorAll('input[name="work_mode"]').forEach(radio => {
    radio.addEventListener('change', async function () {
        // 保存当前配置到缓存（如果当前是modbus-rtu模式）
        const currentMode = configCache.lastWorkMode;
        if (currentMode === 'modbus_rtu') {
            saveCurrentConfigToCache();
        }
        
        const container = document.getElementById('modbus_items_container');
        const modbusConfig = document.getElementById('modbus_rtu_config');
        const workModeContent = document.getElementById('workModeContent');
        const buttonGroup = document.querySelector('.button-group');
        const setModbusItemButton = document.getElementById('setModbusItem');
        const setModbusItemViews = document.getElementById('setModbusItemViews');
        const serialConfigContent = document.getElementById('serialConfigContent');
        const serialConfigCard = serialConfigContent ? serialConfigContent.closest('.card-view') : null;

        // 获取TCP协议类型按钮
        const tcpServerBtn = document.getElementById('tcpServer');
        const tcpClientBtn = document.getElementById('tcpClient');
        const tcpModbusServerBtn = document.getElementById('tcpModbusServer');
        const tcpModbusClientBtn = document.getElementById('tcpModbusClient');
        const selectedTCP = document.getElementById('selectedTCP');

        // 首先隐藏Modbus-RTU配置，只在Modbus-RTU模式下显示
        modbusConfig.style.display = 'none';

        if (this.value === 'modbus_tcp') {
            // 如果是Modbus-TCP转Modbus-RTU模式
            if (tcpClientBtn) {
                tcpClientBtn.disabled = true;
            }
            if (tcpServerBtn) {
                tcpServerBtn.disabled = true;
            }
            if (tcpModbusServerBtn) {
                tcpModbusServerBtn.disabled = false;
            }
            if (tcpModbusClientBtn) {
                tcpModbusClientBtn.disabled = false;
            }
            if (typeof window.setTcpModeUI === 'function') {
                const currentTcpMode = selectedTCP && ['2', '3'].includes(selectedTCP.value)
                    ? selectedTCP.value
                    : '2';
                window.setTcpModeUI(currentTcpMode);
            } else if (selectedTCP) {
                selectedTCP.value = '2';
            }
            // 确保串口设置可见
            if (serialConfigCard) serialConfigCard.style.display = 'block';
            // 清空Modbus项容器
            container.innerHTML = '';
            // 移动设置按钮
            setModbusItemViews.appendChild(setModbusItemButton);
        } else if (this.value === 'mqtt_tcp') {
            // MQTT/TCP透传模式
            if (tcpClientBtn) {
                tcpClientBtn.disabled = false;
            }
            if (tcpServerBtn) {
                tcpServerBtn.disabled = false;
            }
            if (tcpModbusServerBtn) {
                tcpModbusServerBtn.disabled = false;
            }
            if (tcpModbusClientBtn) {
                tcpModbusClientBtn.disabled = false;
            }
            // 确保串口设置可见
            if (serialConfigCard) serialConfigCard.style.display = 'block';
            // 清空Modbus项容器
            container.innerHTML = '';
            // 移动设置按钮
            setModbusItemViews.appendChild(setModbusItemButton);
        } else if (this.value === 'modbus_rtu') {
            // Modbus-RTU网关模式
            if (tcpClientBtn) tcpClientBtn.disabled = false;
            if (tcpServerBtn) tcpServerBtn.disabled = false;
            if (tcpModbusServerBtn) tcpModbusServerBtn.disabled = false;
            if (tcpModbusClientBtn) tcpModbusClientBtn.disabled = false;
            modbusConfig.style.display = 'block';
            buttonGroup.style.display = 'flex';
            serialConfigCard.style.display = 'none';

            buttonGroup.appendChild(setModbusItemButton);
            
            // 注释：协议配置检查已移至提交时进行，支持自动启用功能
            // checkProtocolStatus();

            try {
                const responseData = await fetchWorkModeInfoAllItems();
                // 清空现有内容
                container.innerHTML = '';

                // 创建一个表格容器
                const tableResult = createModbusTableContainer();
                container.appendChild(tableResult.container);
                const tableContent = tableResult.tableContent;

                if (responseData && responseData.work_mode === 'modbus_rtu') {
                    // 将轮询间隔时间从毫秒转换为秒显示
                    const pollTimeMs = parseInt(responseData.poll_time) || 30000;
                    const pollTimeSeconds = Math.round(pollTimeMs / 1000);
                    document.getElementById('poll_time').value = pollTimeSeconds;

                    if (responseData.modbus_items && responseData.modbus_items.length > 0) {
                        responseData.modbus_items.forEach((item, index) => {
                            const row = createModbusItemTemplate(index + 1);
                            const switchBtn = row.querySelector('.switch-btn');
                            const inputs = row.querySelectorAll('.base-input');

                            const inputValues = [
                                item.slave_addr || '',
                                item.function_code || '',
                                item.register_addr || '',
                                item.register_num || '',
                                item.timeout || '',
                                item.data_format || '',
                                item.interval_time || '',
                                item.report_format || '',
                                item.baud_rate || '',
                                item.data_bit || '',
                                item.check_bit || '',
                                item.stop_bit || ''
                            ];

                            // 设置每个输入框的值
                            inputs.forEach((input, i) => {
                                if (inputValues[i]) {
                                    input.value = inputValues[i];
                                }
                                input.disabled = !item.enabled;
                            });

                            // 更新启用状态
                            if (item.enabled) {
                                switchBtn.classList.remove('off');
                                switchBtn.classList.add('on');
                                row.dataset.enabled = 'true';
                            } else {
                                switchBtn.classList.add('off');
                                switchBtn.classList.remove('on');
                                row.dataset.enabled = 'false';
                            }

                            tableContent.appendChild(row);
                        });
                    } else {
                        // 如果没有现有数据，设置默认轮询间隔时间并添加一个默认行
                        document.getElementById('poll_time').value = '30';
                        const row = createModbusItemTemplate(1);
                        tableContent.appendChild(row);
                    }
                } else {
                    // 如果没有配置数据，设置默认轮询间隔时间并添加一个默认行
                    document.getElementById('poll_time').value = '30';
                    const row = createModbusItemTemplate(1);
                    tableContent.appendChild(row);
                }
            } catch (error) {
                console.error('Error loading modbus configuration:', error);
                container.innerHTML = '';

                // 创建一个表格容器
                const tableResult = createModbusTableContainer();
                container.appendChild(tableResult.container);
                const tableContent = tableResult.tableContent;

                // 设置默认轮询间隔时间并添加一个默认行
                document.getElementById('poll_time').value = '30';
                const row = createModbusItemTemplate(1);
                tableContent.appendChild(row);
            }
        } else {
            // 其他模式下恢复TCPClient按钮
            if (tcpClientBtn) {
                tcpClientBtn.disabled = false;
            }
            if (tcpServerBtn) {
                tcpServerBtn.disabled = false;
            }
            if (tcpModbusServerBtn) {
                tcpModbusServerBtn.disabled = false;
            }
            if (tcpModbusClientBtn) {
                tcpModbusClientBtn.disabled = false;
            }
            buttonGroup.style.display = 'none';
            serialConfigCard.style.display = 'block';
            container.innerHTML = '';

            workModeContent.appendChild(setModbusItemButton);
        }

        setModbusItemButton.style.display = 'block';
        workModeContent.style.maxHeight = workModeContent.scrollHeight + 'px';
        workModeContent.classList.remove('collapsed');
        document.getElementById('workModeToggle').classList.remove('collapsed');
    });
});

// 页面加载时检查是否已选择Modbus-RTU
document.addEventListener('DOMContentLoaded', () => {
    const modbusRtuRadio = document.querySelector('input[name="work_mode"][value="modbus_rtu"]');
    const mqttTcpRadio = document.querySelector('input[name="work_mode"][value="mqtt_tcp"]');
    const modbusTcpRadio = document.querySelector('input[name="work_mode"][value="modbus_tcp"]');
    const serialConfigContent = document.getElementById('serialConfigContent');
    const serialConfigCard = serialConfigContent ? serialConfigContent.closest('.card-view') : null;
    const modbusConfig = document.getElementById('modbus_rtu_config');

    // 默认隐藏Modbus-RTU配置
    if (modbusConfig) {
        modbusConfig.style.display = 'none';
    }

    // 首先获取服务器配置的工作模式
    workModeFetchData();
});


// 创建模板函数
function createModbusItemTemplate(index) {
    const div = document.createElement('div');
    div.className = 'modbus-table-row';
    div.dataset.index = index;
    div.dataset.enabled = 'true';
    div.innerHTML = `
        <div class="modbus-table-cell modbus-checkbox-cell">
            <input type="checkbox" class="modbus-row-checkbox" id="row_checkbox_${index}">
        </div>
        <div class="modbus-table-cell" style="max-width: 48px;">
            <div class="switch-btn on" onclick="toggleModbusItem(this)">
                <div class="circle"></div>
            </div>
        </div>
        <div class="modbus-table-cell modbus-index-cell">
            <span class="modbus-index-text">${index}</span>
        </div>
        <div class="modbus-table-cell">
            <input type="text" class="base-input" placeholder="十进制"
                   id="slave_addr_${index}"
                   name="slave_addr_${index}"
                   value="01">
        </div>
        <div class="modbus-table-cell">
            <select class="base-input"
                    id="function_code_${index}"
                    name="function_code_${index}">
                <option value="01">01</option>
                <option value="02">02</option>
                <option value="03" selected>03</option>
                <option value="04">04</option>
            </select>
        </div>
        <div class="modbus-table-cell">
            <input type="text" class="base-input" placeholder="十进制或HEX(如0x10)"
                   id="register_addr_${index}"
                   name="register_addr_${index}"
                   value="00">
        </div>
        <div class="modbus-table-cell">
            <input type="text" class="base-input" placeholder="十进制"
                   id="register_num_${index}"
                   name="register_num_${index}"
                   value="10">
        </div>
        <div class="modbus-table-cell">
            <input type="text" class="base-input" placeholder="单位毫秒"
                   id="timeout_${index}"
                   name="timeout_${index}"
                   value="500">
        </div>
        <div class="modbus-table-cell">
            <select class="base-input"
                    id="data_format_${index}"
                    name="data_format_${index}">
                <option value="Signed">Signed</option>
                <option value="Unsigned">Unsigned</option>
                <option value="HEX">HEX</option>
                <option value="Binary">Binary</option>
                <option value="Long">Long</option>
                <option value="Float">Float</option>
                <option value="Double">Double</option>
                <option value="LongInverse">Long Inverse</option>
                <option value="FloatInverse">Float Inverse</option>
                <option value="DoubleInverse">Double Inverse</option>
            </select>
        </div>
        <div class="modbus-table-cell">
            <input type="text" class="base-input" placeholder="单位毫秒"
                   id="interval_time_${index}"
                   name="interval_time_${index}"
                   value="100">
        </div>
        <div class="modbus-table-cell">
            <select class="base-input"
                    id="report_format_${index}"
                    name="report_format_${index}">
                <option value="mqtt">MQTT</option>
                <option value="tcp">TCP</option>
                <option value="http">HTTP</option>
            </select>
        </div>
        <div class="modbus-table-cell">
            <select class="base-input"
                    id="baud_rate_${index}"
                    name="baud_rate_${index}">
                <option value="1200">1200</option>
                <option value="2400">2400</option>
                <option value="4800">4800</option>
                <option value="9600" selected>9600</option>
                <option value="19200">19200</option>
                <option value="38400">38400</option>
                <option value="57600">57600</option>
                <option value="115200">115200</option>
            </select>
        </div>
        <div class="modbus-table-cell">
            <select class="base-input"
                    id="data_bit_${index}"
                    name="data_bit_${index}">
                <option value="8">8</option>
                <option value="7">7</option>
                <option value="6">6</option>
                <option value="5">5</option>
            </select>
        </div>
        <div class="modbus-table-cell">
            <select class="base-input"
                    id="check_bit_${index}"
                    name="check_bit_${index}">
                <option value="None">无校验</option>
                <option value="Odd">奇校验</option>
                <option value="Even">偶校验</option>
            </select>
        </div>
        <div class="modbus-table-cell">
            <select class="base-input"
                    id="stop_bit_${index}"
                    name="stop_bit_${index}">
                <option value="1">1位停止位</option>
                <option value="1.5">1.5位停止位</option>
                <option value="2">2位停止位</option>
            </select>
        </div>
    `;
    return div;
}

// 创建表头函数
function createModbusTableHeader() {
    const header = document.createElement('div');
    header.className = 'modbus-table-header';
    header.innerHTML = `
        <div class="modbus-table-cell modbus-checkbox-cell">选择</div>
        <div class="modbus-table-cell" style="max-width: 48px;">启用</div>
        <div class="modbus-table-cell modbus-index-cell">序号</div>
        <div class="modbus-table-cell">设备地址</div>
        <div class="modbus-table-cell">功能码</div>
        <div class="modbus-table-cell">寄存器地址</div>
        <div class="modbus-table-cell">寄存器数量</div>
        <div class="modbus-table-cell">接收超时</div>
        <div class="modbus-table-cell">数据格式</div>
        <div class="modbus-table-cell">间隔时间</div>
        <div class="modbus-table-cell">上报方式</div>
        <div class="modbus-table-cell">波特率</div>
        <div class="modbus-table-cell">数据位</div>
        <div class="modbus-table-cell">校验位</div>
        <div class="modbus-table-cell">停止位</div>
    `;
    return header;
}

// 创建表格容器函数
function createModbusTableContainer() {
    const container = document.createElement('div');
    container.className = 'modbus-table-container';

    // 创建一个单一的滚动容器
    const bodyWrapper = document.createElement('div');
    bodyWrapper.className = 'modbus-table-body-wrapper';

    // 创建表格内容容器
    const tableContent = document.createElement('div');
    tableContent.className = 'modbus-table-content';

    // 添加表头
    tableContent.appendChild(createModbusTableHeader());

    // 将表格内容添加到滚动容器
    bodyWrapper.appendChild(tableContent);
    container.appendChild(bodyWrapper);

    return {
        container: container,
        tableContent: tableContent,
        bodyWrapper: bodyWrapper
    };
}

// 添加按钮的事件处理
document.getElementById('addModbusItem').addEventListener('click', function () {
    const container = document.getElementById('modbus_items_container');
    const tableContainer = container.querySelector('.modbus-table-container');
    const tableContent = tableContainer.querySelector('.modbus-table-content');
    const rows = tableContent.querySelectorAll('.modbus-table-row:not(.modbus-table-header)');

    if (rows.length >= MODBUS_ITEM_MAX) {
        showCustomAlert(`最多只能添加${MODBUS_ITEM_MAX}个配置项`);
        return;
    }

    const newIndex = rows.length + 1;
    const row = createModbusItemTemplate(newIndex);
    tableContent.appendChild(row);

    // 更新内容区域高度
    const workModeContent = document.getElementById('workModeContent');
    if (workModeContent && !workModeContent.classList.contains('collapsed')) {
        workModeContent.style.maxHeight = workModeContent.scrollHeight + 'px';
    }
});

// 删除按钮的事件处理
document.getElementById('removeModbusItem').addEventListener('click', function () {
    const container = document.getElementById('modbus_items_container');
    const tableContainer = container.querySelector('.modbus-table-container');
    const tableContent = tableContainer.querySelector('.modbus-table-content');
    const rows = tableContent.querySelectorAll('.modbus-table-row:not(.modbus-table-header)');
    const checkedBoxes = tableContent.querySelectorAll('.modbus-row-checkbox:checked');

    if (checkedBoxes.length === 0) {
        showCustomAlert('请选择要删除的配置项');
        return;
    }

    if (rows.length - checkedBoxes.length < 1) {
        showCustomAlert('至少保留一个配置项');
        return;
    }

    // 删除选中的行
    checkedBoxes.forEach(checkbox => {
        const row = checkbox.closest('.modbus-table-row');
        tableContent.removeChild(row);
    });

    // 重新编号（因为去掉了命令列，这里只需要更新内部ID和name属性）
    const remainingRows = tableContent.querySelectorAll('.modbus-table-row:not(.modbus-table-header)');
    Array.from(remainingRows).forEach((row, index) => {
        const newIndex = index + 1;
        row.dataset.index = newIndex;

        const indexText = row.querySelector('.modbus-index-text');
        if (indexText) {
            indexText.textContent = newIndex;
        }

        // 更新行中的ID和name属性
        const inputs = row.querySelectorAll('input, select');
        inputs.forEach(input => {
            if (input.id && input.id.includes('_')) {
                const baseName = input.id.substring(0, input.id.lastIndexOf('_') + 1);
                input.id = baseName + newIndex;
                if (input.name) input.name = baseName + newIndex;
            }
            if (input.classList.contains('modbus-row-checkbox')) {
                input.id = `row_checkbox_${newIndex}`;
            }
        });
    });

    // 更新内容区域高度
    const workModeContent = document.getElementById('workModeContent');
    if (workModeContent && !workModeContent.classList.contains('collapsed')) {
        workModeContent.style.maxHeight = workModeContent.scrollHeight + 'px';
    }
});

// 添加开关切换函数
function toggleModbusItem(switchBtn) {
    const row = switchBtn.closest('.modbus-table-row');
    const inputs = row.querySelectorAll('.base-input');

    if (switchBtn.classList.contains('on')) {
        // 关闭
        switchBtn.classList.remove('on');
        switchBtn.classList.add('off');
        inputs.forEach(input => input.disabled = true);
        row.dataset.enabled = 'false';
    } else {
        // 开启
        switchBtn.classList.remove('off');
        switchBtn.classList.add('on');
        inputs.forEach(input => input.disabled = false);
        row.dataset.enabled = 'true';
    }
}

// 为设置按钮添加点击事件
document.getElementById('setModbusItem').addEventListener('click', workModeSubmit);

function normalizeNumericInput(rawValue, { min, max, allowHex = false, label }) {
    const trimmed = (rawValue ?? '').trim();
    if (!trimmed) {
        return { valid: false, message: `${label}不能为空` };
    }

    const sanitized = trimmed.replace(/\s+/g, '');
    if (!sanitized) {
        return { valid: false, message: `${label}不能为空` };
    }

    let parsedValue = null;

    const isDecimal = /^\d+$/.test(sanitized);
    const isPrefixedHex = /^0x[0-9a-fA-F]+$/i.test(sanitized);
    const isHexDigits = /^[0-9a-fA-F]+$/.test(sanitized);

    if (allowHex) {
        if (isPrefixedHex) {
            parsedValue = parseInt(sanitized, 16);
        } else if (isHexDigits && /[a-fA-F]/.test(sanitized)) {
            parsedValue = parseInt(`0x${sanitized}`, 16);
        } else if (isDecimal) {
            parsedValue = parseInt(sanitized, 10);
        } else {
            return { valid: false, message: `${label}只能是十进制或HEX数值` };
        }
    } else {
        if (!isDecimal) {
            return { valid: false, message: `${label}只能包含数字` };
        }
        parsedValue = parseInt(sanitized, 10);
    }

    if (!Number.isFinite(parsedValue)) {
        return { valid: false, message: `${label}不是有效的数字` };
    }

    if (parsedValue < min || parsedValue > max) {
        return { valid: false, message: `${label}需在 ${min} ~ ${max} 之间` };
    }

    return {
        valid: true,
        value: parsedValue,
        string: String(parsedValue)
    };
}

function validateModbusTemplates(rows) {
    const errors = [];
    const items = [];
    const enabledTemplatesInfo = [];

    if (!rows || rows.length === 0) {
        return { valid: false, errors: ['请至少保留一个Modbus模板'], items: [], enabledTemplatesInfo: [], hasEnabledTemplate: false };
    }

    Array.from(rows).forEach((row, index) => {
        const itemIndex = index + 1;
        const isEnabled = row.dataset.enabled !== 'false';

        const getField = (name) => row.querySelector(`#${name}_${itemIndex}`);
        const getValue = (name) => {
            const el = getField(name);
            return el ? el.value : '';
        };

        const slaveResult = normalizeNumericInput(getValue('slave_addr'), {
            min: 1,
            max: 247,
            label: `模板${itemIndex}的设备地址`
        });
        if (!slaveResult.valid) {
            errors.push(slaveResult.message);
        } else {
            const input = getField('slave_addr');
            if (input) input.value = slaveResult.string;
        }

        const functionCodeValue = (getValue('function_code') || '01').trim();
        if (!MODBUS_ALLOWED_FUNCTION_CODES.includes(functionCodeValue)) {
            errors.push(`模板${itemIndex}的功能码不受支持`);
        }
        const functionCode = parseInt(functionCodeValue, 10) || 1;

        const registerAddrResult = normalizeNumericInput(getValue('register_addr') || '0', {
            min: 0,
            max: 65535,
            label: `模板${itemIndex}的寄存器地址`,
            allowHex: true
        });
        if (!registerAddrResult.valid) {
            errors.push(registerAddrResult.message);
        } else {
            const input = getField('register_addr');
            if (input) input.value = registerAddrResult.string;
        }

        const regNumMax = (functionCode === 1 || functionCode === 2) ? 2000 :
                          (functionCode === 5 || functionCode === 6) ? 1 : 125;
        const registerNumResult = normalizeNumericInput(getValue('register_num') || '1', {
            min: 1,
            max: regNumMax,
            label: `模板${itemIndex}的寄存器数量`
        });
        if (!registerNumResult.valid) {
            errors.push(registerNumResult.message);
        } else {
            const input = getField('register_num');
            if (input) input.value = registerNumResult.string;
        }

        const timeoutResult = normalizeNumericInput(getValue('timeout') || '500', {
            min: 50,
            max: 120000,
            label: `模板${itemIndex}的接收超时`
        });
        if (!timeoutResult.valid) {
            errors.push(timeoutResult.message);
        } else {
            const input = getField('timeout');
            if (input) input.value = timeoutResult.string;
        }

        const intervalResult = normalizeNumericInput(getValue('interval_time') || '100', {
            min: 0,
            max: 600000,
            label: `模板${itemIndex}的间隔时间`
        });
        if (!intervalResult.valid) {
            errors.push(intervalResult.message);
        } else {
            const input = getField('interval_time');
            if (input) input.value = intervalResult.string;
        }

        const dataFormatValue = (getValue('data_format') || 'Signed').trim();
        if (!MODBUS_ALLOWED_DATA_FORMATS.includes(dataFormatValue)) {
            errors.push(`模板${itemIndex}的数据格式不受支持`);
        }

        const reportFormatRaw = (getValue('report_format') || 'mqtt').trim().toLowerCase();
        if (!MODBUS_ALLOWED_REPORT_FORMATS.includes(reportFormatRaw)) {
            errors.push(`模板${itemIndex}的上报方式无效`);
        }

        const baudRateValue = (getValue('baud_rate') || '9600').trim();
        if (!MODBUS_ALLOWED_BAUD_RATES.includes(baudRateValue)) {
            errors.push(`模板${itemIndex}的波特率无效`);
        }

        const dataBitValue = (getValue('data_bit') || '8').trim();
        if (!MODBUS_ALLOWED_DATA_BITS.includes(dataBitValue)) {
            errors.push(`模板${itemIndex}的数据位无效`);
        }

        const checkBitValue = (getValue('check_bit') || 'None').trim();
        if (!MODBUS_ALLOWED_CHECK_BITS.includes(checkBitValue)) {
            errors.push(`模板${itemIndex}的校验位无效`);
        }

        const stopBitValue = (getValue('stop_bit') || '1').trim();
        if (!MODBUS_ALLOWED_STOP_BITS.includes(stopBitValue)) {
            errors.push(`模板${itemIndex}的停止位无效`);
        }

        if (isEnabled) {
            enabledTemplatesInfo.push({
                index: itemIndex,
                timeout: timeoutResult.value,
                intervalTime: intervalResult.value,
                totalTime: timeoutResult.value + intervalResult.value
            });
        }

        items.push({
            enabled: isEnabled,
            slave_addr: slaveResult.valid ? slaveResult.string : (getValue('slave_addr') || ''),
            function_code: functionCodeValue,
            register_addr: registerAddrResult.valid ? registerAddrResult.string : (getValue('register_addr') || ''),
            register_num: registerNumResult.valid ? registerNumResult.string : (getValue('register_num') || ''),
            timeout: timeoutResult.valid ? timeoutResult.string : (getValue('timeout') || ''),
            data_format: dataFormatValue,
            interval_time: intervalResult.valid ? intervalResult.string : (getValue('interval_time') || ''),
            report_format: reportFormatRaw,
            baud_rate: baudRateValue,
            data_bit: dataBitValue,
            check_bit: checkBitValue,
            stop_bit: stopBitValue
        });
    });

    return {
        valid: errors.length === 0,
        errors,
        items,
        enabledTemplatesInfo,
        hasEnabledTemplate
    };
}

function normalizeNumericInput(rawValue, { min, max, allowHex = false, label }) {
    const trimmed = (rawValue ?? '').trim();
    if (!trimmed) {
        return { valid: false, message: `${label}不能为空` };
    }

    const sanitized = trimmed.replace(/\s+/g, '');
    if (!sanitized) {
        return { valid: false, message: `${label}不能为空` };
    }

    let parsedValue = null;

    const isDecimal = /^\d+$/.test(sanitized);
    const isPrefixedHex = /^0x[0-9a-fA-F]+$/i.test(sanitized);
    const isHexDigits = /^[0-9a-fA-F]+$/.test(sanitized);

    if (allowHex) {
        if (isPrefixedHex) {
            parsedValue = parseInt(sanitized, 16);
        } else if (isHexDigits && /[a-fA-F]/.test(sanitized)) {
            parsedValue = parseInt(`0x${sanitized}`, 16);
        } else if (isDecimal) {
            parsedValue = parseInt(sanitized, 10);
        } else {
            return { valid: false, message: `${label}只能是十进制或HEX数值` };
        }
    } else {
        if (!isDecimal) {
            return { valid: false, message: `${label}只能包含数字` };
        }
        parsedValue = parseInt(sanitized, 10);
    }

    if (!Number.isFinite(parsedValue)) {
        return { valid: false, message: `${label}不是有效的数字` };
    }

    if (parsedValue < min || parsedValue > max) {
        return { valid: false, message: `${label}需在 ${min} ~ ${max} 之间` };
    }

    return {
        valid: true,
        value: parsedValue,
        string: String(parsedValue)
    };
}

function validateModbusTemplates(rows) {
    const errors = [];
    const items = [];
    const enabledTemplatesInfo = [];
    let hasEnabledTemplate = false;
    const rowArray = rows ? Array.from(rows) : [];

    if (rowArray.length === 0) {
        errors.push('请至少保留一个Modbus模板');
        return { valid: false, errors, items, enabledTemplatesInfo, hasEnabledTemplate };
    }

    rowArray.forEach((row, index) => {
        const itemIndex = index + 1;
        const isEnabled = row.dataset.enabled !== 'false';

        const getField = (name) => row.querySelector(`#${name}_${itemIndex}`);
        const getValue = (name) => {
            const el = getField(name);
            return el ? el.value : '';
        };

        const slaveResult = normalizeNumericInput(getValue('slave_addr'), {
            min: 1,
            max: 247,
            label: `模板${itemIndex}的设备地址`
        });
        if (!slaveResult.valid) {
            errors.push(slaveResult.message);
        } else {
            const input = getField('slave_addr');
            if (input) input.value = slaveResult.string;
        }

        const functionCodeValue = (getValue('function_code') || '01').trim();
        if (!MODBUS_ALLOWED_FUNCTION_CODES.includes(functionCodeValue)) {
            errors.push(`模板${itemIndex}的功能码不受支持`);
        }
        const functionCode = parseInt(functionCodeValue, 10) || 1;

        const registerAddrResult = normalizeNumericInput(getValue('register_addr') || '0', {
            min: 0,
            max: 65535,
            label: `模板${itemIndex}的寄存器地址`,
            allowHex: true
        });
        if (!registerAddrResult.valid) {
            errors.push(registerAddrResult.message);
        } else {
            const input = getField('register_addr');
            if (input) input.value = registerAddrResult.string;
        }

        const regNumMax = (functionCode === 1 || functionCode === 2) ? 2000 :
                          (functionCode === 5 || functionCode === 6) ? 1 : 125;
        const registerNumResult = normalizeNumericInput(getValue('register_num') || '1', {
            min: 1,
            max: regNumMax,
            label: `模板${itemIndex}的寄存器数量`
        });
        if (!registerNumResult.valid) {
            errors.push(registerNumResult.message);
        } else {
            const input = getField('register_num');
            if (input) input.value = registerNumResult.string;
        }

        const timeoutResult = normalizeNumericInput(getValue('timeout') || '500', {
            min: 50,
            max: 120000,
            label: `模板${itemIndex}的接收超时`
        });
        if (!timeoutResult.valid) {
            errors.push(timeoutResult.message);
        } else {
            const input = getField('timeout');
            if (input) input.value = timeoutResult.string;
        }

        const intervalResult = normalizeNumericInput(getValue('interval_time') || '100', {
            min: 0,
            max: 600000,
            label: `模板${itemIndex}的间隔时间`
        });
        if (!intervalResult.valid) {
            errors.push(intervalResult.message);
        } else {
            const input = getField('interval_time');
            if (input) input.value = intervalResult.string;
        }

        const dataFormatValue = (getValue('data_format') || 'Signed').trim();
        if (!MODBUS_ALLOWED_DATA_FORMATS.includes(dataFormatValue)) {
            errors.push(`模板${itemIndex}的数据格式不受支持`);
        }

        const reportFormatRaw = (getValue('report_format') || 'mqtt').trim().toLowerCase();
        if (!MODBUS_ALLOWED_REPORT_FORMATS.includes(reportFormatRaw)) {
            errors.push(`模板${itemIndex}的上报方式无效`);
        }

        const baudRateValue = (getValue('baud_rate') || '9600').trim();
        if (!MODBUS_ALLOWED_BAUD_RATES.includes(baudRateValue)) {
            errors.push(`模板${itemIndex}的波特率无效`);
        }

        const dataBitValue = (getValue('data_bit') || '8').trim();
        if (!MODBUS_ALLOWED_DATA_BITS.includes(dataBitValue)) {
            errors.push(`模板${itemIndex}的数据位无效`);
        }

        const checkBitValue = (getValue('check_bit') || 'None').trim();
        if (!MODBUS_ALLOWED_CHECK_BITS.includes(checkBitValue)) {
            errors.push(`模板${itemIndex}的校验位无效`);
        }

        const stopBitValue = (getValue('stop_bit') || '1').trim();
        if (!MODBUS_ALLOWED_STOP_BITS.includes(stopBitValue)) {
            errors.push(`模板${itemIndex}的停止位无效`);
        }

        if (isEnabled && timeoutResult.valid && intervalResult.valid) {
            hasEnabledTemplate = true;
            enabledTemplatesInfo.push({
                index: itemIndex,
                timeout: timeoutResult.value,
                intervalTime: intervalResult.value,
                totalTime: timeoutResult.value + intervalResult.value
            });
        }

        items.push({
            enabled: isEnabled,
            slave_addr: slaveResult.valid ? slaveResult.string : (getValue('slave_addr') || ''),
            function_code: functionCodeValue,
            register_addr: registerAddrResult.valid ? registerAddrResult.string : (getValue('register_addr') || ''),
            register_num: registerNumResult.valid ? registerNumResult.string : (getValue('register_num') || ''),
            timeout: timeoutResult.valid ? timeoutResult.string : (getValue('timeout') || ''),
            data_format: dataFormatValue,
            interval_time: intervalResult.valid ? intervalResult.string : (getValue('interval_time') || ''),
            report_format: reportFormatRaw,
            baud_rate: baudRateValue,
            data_bit: dataBitValue,
            check_bit: checkBitValue,
            stop_bit: stopBitValue
        });
    });

    return {
        valid: errors.length === 0,
        errors,
        items,
        enabledTemplatesInfo,
        hasEnabledTemplate
    };
}

async function workModeSubmit() {
    const workMode = document.querySelector('input[name="work_mode"]:checked').value;
    const data = {
        work_mode: workMode
    };

    // 对于MQTT/TCP透传模式，检查至少有一个协议启用（支持用户选择）
    if (workMode === 'mqtt_tcp') {
        const protocolCheckResult = await checkProtocolsForMqttTcpMode();
        if (!protocolCheckResult.valid) {
            const detailMessage = protocolCheckResult.unconfiguredProtocols && protocolCheckResult.unconfiguredProtocols.length > 0
                ? `请先在协议管理中启用：${protocolCheckResult.unconfiguredProtocols.join('、')}。`
                : '请先完成相关协议的启用与参数配置。';
            const finalMessage = protocolCheckResult.message
                ? `${protocolCheckResult.message}\n${detailMessage}`
                : detailMessage;
            showCustomAlert(finalMessage, true);
            document.getElementById('setModbusItem').textContent = window.i18n ? window.i18n.t('message.protocolDisabled') : "协议未启用";
            setTimeout(() => document.getElementById('setModbusItem').textContent = window.i18n ? window.i18n.t('common.set') : "设置", 2000);
            return;
        }
    }

    // 对于Modbus-TCP转Modbus-RTU模式，检查TCP配置状态（必须TCP Server + 可选MQTT）
    if (workMode === 'modbus_tcp') {
        const tcpCheckResult = await checkTCPConfigurationForModbusTCP();
        if (!tcpCheckResult.valid) {
            showCustomAlert(tcpCheckResult.message || 'Modbus-TCP模式所需的TCP配置未完成，请先在协议管理中启用并配置TCP Server。', true);
            document.getElementById('setModbusItem').textContent = window.i18n ? window.i18n.t('message.protocolDisabled') : "协议未启用";
            setTimeout(() => document.getElementById('setModbusItem').textContent = window.i18n ? window.i18n.t('common.set') : "设置", 2000);
            return;
        }
    }

    if (workMode === 'modbus_rtu') {
        const container = document.getElementById('modbus_items_container');
        const tableContainer = container.querySelector('.modbus-table-container');
        const tableContent = tableContainer.querySelector('.modbus-table-content');
        const rows = tableContent.querySelectorAll('.modbus-table-row:not(.modbus-table-header)');

        const validation = validateModbusTemplates(rows);
        if (!validation.valid) {
            const detailMessage = validation.errors.map(err => `• ${err}`).join('<br>');
            showCustomAlert(`Modbus 配置存在以下问题：<br>${detailMessage}`, true);
            document.getElementById('setModbusItem').textContent = window.i18n ? window.i18n.t('message.configError') : "配置有误";
            setTimeout(() => document.getElementById('setModbusItem').textContent = window.i18n ? window.i18n.t('common.set') : "设置", 2000);
            return;
        }

        const modbusItems = validation.items;
        const modbusChunkSize = 10;
        const protocolCheckResult = await checkProtocolConfigurationForModbusRTU(modbusItems);
        if (!protocolCheckResult.valid) {
            const alertMessage = protocolCheckResult.message
                || '请先在协议管理中完成所需协议的启用与参数配置。';
            showCustomAlert(alertMessage, true);
            document.getElementById('setModbusItem').textContent = window.i18n ? window.i18n.t('message.protocolDisabled') : "协议未启用";
            setTimeout(() => document.getElementById('setModbusItem').textContent = window.i18n ? window.i18n.t('common.set') : "设置", 2000);
            return;
        }

        const pollTimeInput = document.getElementById('poll_time').value || '30';
        let pollTimeSeconds = parseInt(pollTimeInput, 10);

        if (isNaN(pollTimeSeconds) || pollTimeSeconds <= 0) {
            showCustomAlert('轮询间隔时间必须是大于0的整数（单位：秒）', true);
            return;
        }

        const enabledTemplatesInfo = validation.enabledTemplatesInfo;
        const hasEnabledTemplate = validation.hasEnabledTemplate;
        let totalTemplateTime = 0;
        enabledTemplatesInfo.forEach(info => {
            totalTemplateTime += info.totalTime;
        });

        if (hasEnabledTemplate && totalTemplateTime > 0) {
            const minPollTimeSeconds = Math.ceil(totalTemplateTime / 1000) + 1;

            if (pollTimeSeconds < minPollTimeSeconds) {
                const enabledCount = enabledTemplatesInfo.length;
                const message = `
                    <div style="margin-bottom: 12px;">
                        <p style="margin: 0 0 8px 0; color: #e74c3c; font-weight: 600;">
                            ⚠️ 轮询间隔时间设置过小
                        </p>
                        <p style="margin: 0 0 6px 0;">
                            当前设置：<strong>${pollTimeSeconds}秒</strong>
                        </p>
                        <p style="margin: 0 0 6px 0;">
                            建议最小值：<strong>${minPollTimeSeconds}秒</strong>
                        </p>
                        <p style="margin: 0; color: #555;">
                            启用模板数量：${enabledCount}，合计执行时间约 ${totalTemplateTime}ms。
                        </p>
                    </div>
                    <div style="background-color: #f8f9fa; border-radius: 6px; padding: 10px; font-size: 12px; color: #666;">
                        系统将按“总执行时间 + 1秒缓冲”自动调整轮询间隔时间。
                    </div>
                `;

                return new Promise((resolve) => {
                    showCustomConfirm(
                        message,
                        () => {
                            // 确认：自动调整
                            pollTimeSeconds = minPollTimeSeconds;
                            document.getElementById('poll_time').value = pollTimeSeconds;
                            resolve(true);
                        },
                        () => {
                            // 取消：不提交
                            resolve(false);
                        }
                    );
                }).then(shouldContinue => {
                    if (!shouldContinue) return; // 用户取消，停止执行

                    // 继续提交逻辑
                    continueSubmit();
                });

                return; // 暂停执行，等待用户确认
            }
        }

        let pollTimeMs = 0;
        // 继续提交的函数
        function continueSubmit() {
            // 将秒转换为毫秒存储
            pollTimeMs = pollTimeSeconds * 1000;
            data.poll_time = pollTimeMs.toString();

            // 在提交前显示提示信息
            console.log(`轮询间隔时间设置为: ${pollTimeSeconds}秒 (${pollTimeMs}毫秒)`);

            // 执行实际提交
            submitData();
        }

        // 实际提交数据的函数
        async function submitData() {
            try {
                if (modbusItems.length > modbusChunkSize) {
                    const totalItems = modbusItems.length;
                    for (let offset = 0; offset < totalItems; offset += modbusChunkSize) {
                        const chunk = modbusItems.slice(offset, offset + modbusChunkSize);
                        const payload = {
                            modbus_items: chunk,
                            modbus_items_offset: offset,
                            modbus_items_total: totalItems
                        };
                        if (offset === 0) {
                            payload.work_mode = workMode;
                            payload.poll_time = pollTimeMs.toString();
                        }
                        await postData('/work_mode_set', payload);
                    }
                } else {
                    data.modbus_items = modbusItems;
                    await postData('/work_mode_set', data);
                }
                document.getElementById('setModbusItem').textContent = window.i18n ? window.i18n.t('message.setSuccess') : "设置成功";

                // 根据工作模式决定是否显示重启提示框
                if (workMode === 'mqtt_tcp' || workMode === 'modbus_tcp') {
                    // 只有在MQTT/TCP透传或Modbus-TCP转Modbus-RTU模式下才显示重启提示
                    const deviceRestart = document.getElementById('deviceRestart');
                    if (deviceRestart) {
                        showSvg('deviceRestart', 6000);
                    }
                }
            } catch (error) {
                console.error('Error fetching data:', error);
                
                // 检查是否是协议未配置的错误（HTTP 400）
                if ((error.code === 400 || error.code === 422) && error.message) {
                    // 显示具体的错误信息
                    showCustomAlert(error.message, true);
                    document.getElementById('setModbusItem').textContent = window.i18n ? window.i18n.t('message.configCheckFailed') : "配置检查失败";
                } else {
                    // 其他错误显示通用失败信息
                    document.getElementById('setModbusItem').textContent = window.i18n ? window.i18n.t('message.setFailed') : "设置失败";
                }
            }
            setTimeout(() => document.getElementById('setModbusItem').textContent = window.i18n ? window.i18n.t('common.set') : "设置", 2000);
        }

        // 如果没有验证问题，直接提交
        continueSubmit();
    }

    // 如果不是modbus_rtu模式，直接提交
    if (workMode !== 'modbus_rtu') {
        try {
            await postData('/work_mode_set', data);
            document.getElementById('setModbusItem').textContent = "设置成功";

            // 根据工作模式决定是否显示重启提示框
            if (workMode === 'mqtt_tcp' || workMode === 'modbus_tcp') {
                // 只有在MQTT/TCP透传或Modbus-TCP转Modbus-RTU模式下才显示重启提示
                const deviceRestart = document.getElementById('deviceRestart');
                if (deviceRestart) {
                    showSvg('deviceRestart', 6000);
                }
            }
        } catch (error) {
            console.error('Error fetching data:', error);
            
            // 检查是否是协议未配置的错误（HTTP 400）
            if (error.code === 400 && error.message) {
                // 显示具体的错误信息
                showCustomAlert(error.message, true);
                document.getElementById('setModbusItem').textContent = "配置检查失败";
            } else {
                // 其他错误显示通用失败信息
                document.getElementById('setModbusItem').textContent = "设置失败";
            }
        }
        setTimeout(() => document.getElementById('setModbusItem').textContent = "设置", 2000);
    }
}

// 处理Modbus-RTU模式的显示逻辑
function handleModbusRtuModeDisplay(responseData) {
    const container = document.getElementById('modbus_items_container');
    const modbusConfig = document.getElementById('modbus_rtu_config');
    const workModeContent = document.getElementById('workModeContent');
    const buttonGroup = document.querySelector('.button-group');
    const setModbusItemButton = document.getElementById('setModbusItem');
    const serialConfigContent = document.getElementById('serialConfigContent');
    const serialConfigCard = serialConfigContent ? serialConfigContent.closest('.card-view') : null;

    // 显示Modbus-RTU配置
    if (modbusConfig) modbusConfig.style.display = 'block';
    if (buttonGroup) buttonGroup.style.display = 'flex';
    if (serialConfigCard) serialConfigCard.style.display = 'none';
    
    if (buttonGroup && setModbusItemButton) {
        buttonGroup.appendChild(setModbusItemButton);
    }

    // 注释：协议配置检查已移至提交时进行，支持自动启用功能
    // checkProtocolStatus();

    // 清空现有内容
    if (container) container.innerHTML = '';

    // 创建一个表格容器
    const tableResult = createModbusTableContainer();
    if (container) container.appendChild(tableResult.container);
    const tableContent = tableResult.tableContent;

    if (responseData && responseData.work_mode === 'modbus_rtu') {
        // 使用服务器数据或缓存数据设置轮询间隔时间
        const pollTimeMs = parseInt(responseData.poll_time) || parseInt(configCache.pollTime) || 30000;
        const pollTimeSeconds = Math.round(pollTimeMs / 1000);
        const pollTimeInput = document.getElementById('poll_time');
        if (pollTimeInput) {
            pollTimeInput.value = pollTimeSeconds;
            console.log(`设置轮询间隔时间: ${pollTimeSeconds}秒 (${pollTimeMs}ms)`);
        }

        // 使用服务器数据或缓存数据
        const modbusItems = (responseData.modbus_items && responseData.modbus_items.length > 0) 
            ? responseData.modbus_items 
            : configCache.modbusItems;
            
        if (modbusItems && modbusItems.length > 0) {
            console.log(`使用${responseData.modbus_items ? '服务器' : '缓存'}数据显示 ${modbusItems.length} 个Modbus配置项`);
            modbusItems.forEach((item, index) => {
                const row = createModbusItemTemplate(index + 1);
                const switchBtn = row.querySelector('.switch-btn');
                const inputs = row.querySelectorAll('.base-input');

                const inputValues = [
                    item.slave_addr || '',
                    item.function_code || '',
                    item.register_addr || '',
                    item.register_num || '',
                    item.timeout || '',
                    item.data_format || '',
                    item.interval_time || '',
                    item.report_format || '',
                    item.baud_rate || '',
                    item.data_bit || '',
                    item.check_bit || '',
                    item.stop_bit || ''
                ];

                // 设置每个输入框的值
                inputs.forEach((input, i) => {
                    if (inputValues[i]) {
                        input.value = inputValues[i];
                    }
                    input.disabled = !item.enabled;
                });

                // 更新启用状态
                if (item.enabled) {
                    switchBtn.classList.remove('off');
                    switchBtn.classList.add('on');
                    row.dataset.enabled = 'true';
                } else {
                    switchBtn.classList.add('off');
                    switchBtn.classList.remove('on');
                    row.dataset.enabled = 'false';
                }

                tableContent.appendChild(row);
            });
        } else {
            // 如果服务器没有返回数据，尝试使用缓存数据
            if (configCache.modbusItems && configCache.modbusItems.length > 0) {
                console.log(`服务器无数据，使用缓存数据显示 ${configCache.modbusItems.length} 个Modbus配置项`);
                configCache.modbusItems.forEach((item, index) => {
                    const row = createModbusItemTemplate(index + 1);
                    const switchBtn = row.querySelector('.switch-btn');
                    const inputs = row.querySelectorAll('.base-input');
                    const inputValues = [
                        item.slave_addr || '',
                        item.function_code || '',
                        item.register_addr || '',
                        item.register_num || '',
                        item.timeout || '',
                        item.data_format || '',
                        item.interval_time || '',
                        item.report_format || '',
                        item.baud_rate || '',
                        item.data_bit || '',
                        item.check_bit || '',
                        item.stop_bit || ''
                    ];
                    
                    inputs.forEach((input, i) => {
                        if (inputValues[i]) {
                            input.value = inputValues[i];
                        }
                        input.disabled = !item.enabled;
                    });
                    
                    if (item.enabled) {
                        switchBtn.classList.remove('off');
                        switchBtn.classList.add('on');
                        row.dataset.enabled = 'true';
                    } else {
                        switchBtn.classList.add('off');
                        switchBtn.classList.remove('on');
                        row.dataset.enabled = 'false';
                    }
                    tableContent.appendChild(row);
                });
                
                // 设置缓存的轮询时间
                if (configCache.pollTime) {
                    const pollTimeInput = document.getElementById('poll_time');
                    if (pollTimeInput) {
                        const pollTimeSeconds = Math.round(parseInt(configCache.pollTime) / 1000);
                        pollTimeInput.value = pollTimeSeconds;
                    }
                }
            } else {
                // 如果没有缓存数据，设置默认轮询间隔时间并添加一个默认行
                const pollTimeInput = document.getElementById('poll_time');
                if (pollTimeInput) pollTimeInput.value = '30';
                const row = createModbusItemTemplate(1);
                tableContent.appendChild(row);
            }
        }
    } else {
        // 如果没有配置数据，设置默认轮询间隔时间并添加一个默认行
        const pollTimeInput = document.getElementById('poll_time');
        if (pollTimeInput) pollTimeInput.value = '30';
        const row = createModbusItemTemplate(1);
        tableContent.appendChild(row);
    }

    if (setModbusItemButton) setModbusItemButton.style.display = 'block';
    if (workModeContent) {
        workModeContent.style.maxHeight = workModeContent.scrollHeight + 'px';
        workModeContent.classList.remove('collapsed');
        document.getElementById('workModeToggle').classList.remove('collapsed');
    }
}

// 页面加载时获取工作模式配置
async function workModeFetchData(retryCount = 0) {
    const maxRetries = 3;
    const retryDelay = 1000; // 1秒
    
    try {
        console.log(`正在获取工作模式配置... (尝试 ${retryCount + 1}/${maxRetries + 1})`);
        let responseData = await fetchWorkModeInfoPage(0, 0);
        if (responseData && responseData.work_mode === 'modbus_rtu') {
            const itemsData = await fetchWorkModeInfoAllItems();
            responseData = {
                ...responseData,
                poll_time: itemsData.poll_time,
                modbus_items_total: itemsData.modbus_items_total,
                modbus_items: itemsData.modbus_items
            };
        }
        const modbusConfig = document.getElementById('modbus_rtu_config');

        console.log('Response from server:', responseData);
        
        // 验证数据完整性并更新缓存
        if (responseData && responseData.work_mode === 'modbus_rtu' && responseData.modbus_items) {
            const expectedFields = ['slave_addr', 'function_code', 'register_addr', 'register_num', 'timeout', 'data_format'];
            const hasIncompleteData = responseData.modbus_items.some(item => 
                expectedFields.some(field => item[field] === undefined || item[field] === '')
            );
            
            if (hasIncompleteData && retryCount < maxRetries) {
                console.warn(`检测到数据不完整，${retryDelay}ms后重试...`);
                setTimeout(() => workModeFetchData(retryCount + 1), retryDelay);
                return;
            }
            
            // 更新缓存
            configCache.modbusItems = responseData.modbus_items;
            configCache.pollTime = responseData.poll_time;
            configCache.lastWorkMode = responseData.work_mode;
            console.log(`成功加载 ${responseData.modbus_items.length} 个Modbus配置项并缓存`);
        } else if (responseData) {
            // 非modbus-rtu模式也要缓存工作模式
            configCache.lastWorkMode = responseData.work_mode;
        }

        // 显示调试信息
        // const debugModeInfo = document.getElementById('debug_mode_info');
        // const debugWorkMode = document.getElementById('debug_work_mode');
        // if (debugModeInfo && debugWorkMode && responseData) {
        //     debugModeInfo.style.display = 'block';
        //     debugWorkMode.textContent = responseData.work_mode || '未设置';
        // }

        if (responseData) {
            // 设置工作模式
            console.log('服务器返回的工作模式:', responseData.work_mode);
            // 修复：ID应该与输入字段ID匹配，而不是value匹配
            if (responseData.work_mode === 'modbus_tcp') {
                const modbusTcpRadio = document.getElementById('modbus_tcp');
                if (modbusTcpRadio) {
                    console.log('找到modbus_tcp按钮，设置为选中');
                    modbusTcpRadio.checked = true;
                    modbusTcpRadio.dispatchEvent(new Event('change'));
                } else {
                    console.warn('未找到modbus_tcp按钮');
                }
            } else if (responseData.work_mode === 'mqtt_tcp') {
                const mqttTcpRadio = document.getElementById('mqtt_tcp_mode');
                if (mqttTcpRadio) {
                    console.log('找到mqtt_tcp按钮，设置为选中');
                    mqttTcpRadio.checked = true;
                    mqttTcpRadio.dispatchEvent(new Event('change'));
                }
            } else if (responseData.work_mode === 'modbus_rtu') {
                const modbusRtuRadio = document.querySelector('input[name="work_mode"][value="modbus_rtu"]');
                if (modbusRtuRadio) {
                    console.log('找到modbus_rtu按钮，设置为选中');
                    modbusRtuRadio.checked = true;
                    // 不触发change事件，避免循环调用，直接处理Modbus-RTU配置显示
                    handleModbusRtuModeDisplay(responseData);
                }
            } else {
                // 打印可用的工作模式按钮，用于调试
                console.warn('未找到匹配的工作模式:', responseData.work_mode);
                document.querySelectorAll('input[name="work_mode"]').forEach(radio => {
                    console.log('可用的工作模式:', radio.value, '按钮ID:', radio.id);
                });

                // 使用当前选中的工作模式
                const checkedMode = document.querySelector('input[name="work_mode"]:checked');
                if (checkedMode) {
                    console.log('使用当前选中的模式:', checkedMode.value);
                    checkedMode.dispatchEvent(new Event('change'));
                }
            }
        } else {
            // 保持当前选中的工作模式不变
            const checkedMode = document.querySelector('input[name="work_mode"]:checked');
            if (checkedMode) {
                checkedMode.dispatchEvent(new Event('change'));
            }
        }
    } catch (error) {
        console.error('Failed to fetch work mode data:', error);
        
        // 如果还有重试次数，则重试
        if (retryCount < maxRetries) {
            console.warn(`获取工作模式配置失败，${retryDelay}ms后重试...`);
            setTimeout(() => workModeFetchData(retryCount + 1), retryDelay);
            return;
        }
        
        console.error('已达到最大重试次数，使用当前选中的工作模式');
        // 保持当前选中的工作模式不变
        const checkedMode = document.querySelector('input[name="work_mode"]:checked');
        if (checkedMode) {
            checkedMode.dispatchEvent(new Event('change'));
        }
    }
}

// Serial config API

document.getElementById("serialDataForm").addEventListener("submit", event => event.preventDefault());

document.getElementById('serialButton').addEventListener('click', async () => {
    const formData = new FormData(document.getElementById('serialDataForm'));
    const data = Object.fromEntries(formData);
    try {
        await postData('/serial_set', data);
    } catch (error) {
        console.error('Error fetching data. Status:', error);
    }
});

async function serialfetchData() {
    try {
        const responseData = await fetchData('/serial_set_info');
        document.getElementById('baud_rate').value = responseData.baud_rate || '9600';
        document.getElementById('data_bit').value = responseData.data_bit || '8';

        // 根据后端返回值设置校验位
        let checkBit = 'None';  // 默认无校验
        if (responseData.check_bit === '1') {
            checkBit = 'Odd';  // 奇校验
        } else if (responseData.check_bit === '2') {
            checkBit = 'Even';  // 偶校验
        }
        document.getElementById('check_bit').value = checkBit;

        // 设置停止位
        let stopBit = '1';  // 默认1位停止位
        if (responseData.stop_bit === '1') {
            stopBit = '1';
        } else if (responseData.stop_bit === '2') {
            stopBit = '2';
        } else if (responseData.stop_bit === '1.5') {
            stopBit = '1.5';
        }
        document.getElementById('stop_bit').value = stopBit;

        document.getElementById('frame_time').value = responseData.frame_time || '50';
        document.getElementById('frame_len').value = responseData.frame_len || '512';
        console.log(responseData);
    } catch (error) {
        console.error('Failed to fetch data. Status:', error);
    }
}
serialfetchData();

// Serial control API

document.getElementById("serial_ctl").addEventListener("submit", event => event.preventDefault());

function convertInstructionToBytes(rawInstruction, isHexMode) {
    if (!rawInstruction) {
        return new Uint8Array(0);
    }

    if (isHexMode) {
        const cleanStr = rawInstruction.replace(/[^0-9A-Fa-f]/g, '');
        if (cleanStr.length === 0) {
            return new Uint8Array(0);
        }
        if (cleanStr.length % 2 !== 0) {
            throw new Error('十六进制字符数量必须为偶数');
        }
        const byteLen = cleanStr.length / 2;
        const bytes = new Uint8Array(byteLen);
        for (let i = 0; i < byteLen; i++) {
            const byte = parseInt(cleanStr.substr(i * 2, 2), 16);
            if (Number.isNaN(byte)) {
                throw new Error('检测到无效的十六进制字符');
            }
            bytes[i] = byte;
        }
        return bytes;
    }

    return textEncoder.encode(rawInstruction);
}

function bytesToHex(bytes) {
    return Array.from(bytes)
        .map((byte) => byte.toString(16).padStart(2, '0').toUpperCase())
        .join(' ');
}

function calculateCrc16Modbus(bytes) {
    let crc = 0xFFFF;

    for (const byte of bytes) {
        crc ^= byte;
        for (let bit = 0; bit < 8; bit++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc & 0xFFFF;
}

function getCrcBytes(bytes) {
    const crc = calculateCrc16Modbus(bytes);
    return new Uint8Array([crc & 0xFF, (crc >> 8) & 0xFF]);
}

function isSendCrcEnabled() {
    const crcCheckbox = document.getElementById('sendCrcEnabled');
    return Boolean(crcCheckbox && crcCheckbox.checked);
}

function setCrcPreview(text, isEnabled) {
    const previewElement = document.getElementById('crcPreview');
    if (!previewElement) return;

    previewElement.textContent = text || '-- --';
    previewElement.classList.toggle('disabled', !isEnabled);
}

function updateCrcPreview() {
    const isEnabled = isSendCrcEnabled();
    const instructionValue = document.getElementById('hiddenInput')?.value || '';
    const sendType = document.getElementById('tran')?.checked ? 'hex' : 'ascii';

    if (!isEnabled) {
        setCrcPreview('-- --', false);
        return;
    }

    try {
        const payloadBytes = convertInstructionToBytes(instructionValue, sendType === 'hex');
        if (payloadBytes.length === 0) {
            setCrcPreview('-- --', true);
            return;
        }

        setCrcPreview(bytesToHex(getCrcBytes(payloadBytes)), true);
    } catch (error) {
        setCrcPreview('ERR', true);
    }
}

function appendCrcBytes(bytes) {
    if (!isSendCrcEnabled()) {
        return bytes;
    }

    const crcBytes = getCrcBytes(bytes);
    const bytesWithCrc = new Uint8Array(bytes.length + crcBytes.length);
    bytesWithCrc.set(bytes);
    bytesWithCrc.set(crcBytes, bytes.length);
    return bytesWithCrc;
}

async function sendInstructionChunks(bytes, sendType, transferId) {
    if (bytes.length === 0) {
        throw new Error('请输入要发送的数据');
    }

    await waitForUartWebSocketOpen();

    const totalChunks = Math.max(1, Math.ceil(bytes.length / UART_CHUNK_BYTES));
    for (let chunkIndex = 0; chunkIndex < totalChunks; chunkIndex++) {
        const start = chunkIndex * UART_CHUNK_BYTES;
        const end = Math.min(start + UART_CHUNK_BYTES, bytes.length);
        const chunk = bytes.slice(start, end);
        if (!chunk.length) {
            continue;
        }

        const payload =
            sendType === 'hex'
                ? bytesToHex(chunk)
                : textDecoder.decode(chunk);

        const message = {
            type: 'uart_tx',
            instruction: payload,
            sendType,
            chunkIndex,
            chunkTotal: totalChunks,
            transferId
        };

        const ackPromise = createAckPromise(transferId, chunkIndex);
        try {
            uartWebSocket.send(JSON.stringify(message));
        } catch (error) {
            const key = buildAckKey(transferId, chunkIndex);
            const pending = pendingUartAcks.get(key);
            if (pending) {
                pending.reject(error);
            }
            throw error;
        }

        await ackPromise;
    }
}

async function pollLegacyUartResponse() {
    let attempts = 0;
    const maxAttempts = 10;
    const pollInterval = 50;

    while (attempts < maxAttempts) {
        try {
            const response = await fetch('/uart_response');
            const responseData = await response.json();
            if (responseData.tx_hex || responseData.rx_hex || responseData.tx_ascii || responseData.rx_ascii) {
                const resultElement = document.getElementById('devcie_report');

                if (responseData.tx_hex || responseData.tx_ascii) {
                    const txTimestamp = formatTimestamp(responseData.tx_timestamp);
                    if (document.getElementById('tran').checked) {
                        resultElement.innerHTML += `[${txTimestamp}]发→◇${responseData.tx_hex} <br>`;
                    } else {
                        resultElement.innerHTML += `[${txTimestamp}]发→◇${responseData.tx_ascii} <br>`;
                    }
                }

                if (responseData.rx_hex || responseData.rx_ascii) {
                    const rxTimestamp = formatTimestamp(responseData.rx_timestamp, true);
                    if (document.getElementById('tran').checked) {
                        resultElement.innerHTML += `[${rxTimestamp}]收←◆${responseData.rx_hex}<br>`;
                    } else {
                        resultElement.innerHTML += `[${rxTimestamp}]收←◆${responseData.rx_ascii}<br>`;
                    }
                }

                resultElement.scrollTop = resultElement.scrollHeight;
                return;
            }
        } catch (error) {
            console.error('Error polling response:', error);
            return;
        }

        attempts++;
        await new Promise(resolve => setTimeout(resolve, pollInterval));
    }
}

// 修改 serialSubmit 函数
async function serialSubmit() {
    const instructionValue = document.getElementById('hiddenInput').value;
    const sendType = document.getElementById('tran').checked ? 'hex' : 'ascii';
    const sendButton = document.getElementById('serialControl');

    try {
        const payloadBytes = convertInstructionToBytes(instructionValue, sendType === 'hex');
        if (payloadBytes.length === 0) {
            showCustomAlert('请输入要发送的数据', true);
            return;
        }
        const sendBytes = appendCrcBytes(payloadBytes);

        const transferId = `${Date.now()}-${Math.random().toString(36).slice(2, 10)}`;
        if (sendButton) {
            sendButton.disabled = true;
        }
        const needProgressAlert = sendBytes.length > UART_CHUNK_BYTES;
        if (needProgressAlert) {
            showCustomAlert(`开始发送串口数据，共${sendBytes.length}字节`, false);
        }

        await sendInstructionChunks(sendBytes, sendType, transferId);
        await pollLegacyUartResponse(); // 兼容老版本

        if (needProgressAlert) {
            showCustomAlert(`串口数据发送完成，共${sendBytes.length}字节`, false);
        }
    } catch (error) {
        console.error('Error fetching data:', error);
        showCustomAlert(error.message || '串口数据发送失败', true);
    } finally {
        if (sendButton) {
            sendButton.disabled = false;
        }
    }
}

document.getElementById('serialControl').addEventListener('click', async event => {
    event.preventDefault();
    const instructionElement = document.getElementById('instruction');
    const instruction = instructionElement.innerText;
    const errorMessageElement = document.getElementById('error-message');
    const isHex = /^[0-9A-Fa-f\s]+$/i.test(instruction.replace(/\s/g, ''));
    const tranElement = document.getElementById('tran');

    if (tranElement.checked && !isHex) {
        errorMessageElement.innerText = '数据格式异常，请输入十六进制数据';
        showCustomAlert('数据格式异常，请输入十六进制数据', true);
        event.preventDefault();
    } else {
        errorMessageElement.innerText = '';
        serialSubmit();
    }
});

// 添加清空按钮事件监听
document.getElementById('clearResponse').addEventListener('click', () => {
    const responseElement = document.getElementById('devcie_report');
    responseElement.innerHTML = '';
});

document.getElementById('clearInstruction').addEventListener('click', () => {
    const instructionElement = document.getElementById('instruction');
    instructionElement.innerText = '';
    document.getElementById('hiddenInput').value = '';
    document.getElementById('error-message').innerText = '';
    updateCrcPreview();
});

const instructionInput = document.getElementById('instruction');
if (instructionInput) {
    instructionInput.addEventListener('input', () => {
        const hiddenInput = document.getElementById('hiddenInput');
        if (hiddenInput) {
            hiddenInput.value = instructionInput.innerText;
        }
        updateCrcPreview();
    });
}

['tran', 'mqt', 'sendCrcEnabled'].forEach(id => {
    const element = document.getElementById(id);
    if (element) {
        element.addEventListener('change', updateCrcPreview);
    }
});
// 添加接收状态控制变量
const MAX_LINES = 512; // 最多显示行
const UART_CHUNK_BYTES = 512; // 每段发送的最大字节数
let isReceiving = true;
let pollInterval = null;
let uartWebSocket = null; // 串口WebSocket连接
const pendingUartAcks = new Map();
const uartChunkBuffer = new Map();
const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

updateCrcPreview();

function buildAckKey(transferId, chunkIndex) {
    const id = transferId && transferId.length ? transferId : '__single__';
    const idx = Number.isFinite(chunkIndex) ? chunkIndex : 0;
    return `${id}:${idx}`;
}

function rejectAllPendingAcks(message) {
    const reason = message || 'WebSocket连接已中断';
    pendingUartAcks.forEach(handler => {
        if (handler && typeof handler.reject === 'function') {
            handler.reject(reason);
        }
    });
    pendingUartAcks.clear();
    uartChunkBuffer.clear();
}

function createAckPromise(transferId, chunkIndex, timeoutMs = 5000) {
    const key = buildAckKey(transferId, chunkIndex);
    if (pendingUartAcks.has(key)) {
        pendingUartAcks.get(key).reject('存在重复的发送请求');
    }

    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            pendingUartAcks.delete(key);
            reject(new Error('串口发送超时'));
        }, timeoutMs);

        pendingUartAcks.set(key, {
            resolve: () => {
                clearTimeout(timer);
                pendingUartAcks.delete(key);
                resolve();
            },
            reject: (message) => {
                clearTimeout(timer);
                pendingUartAcks.delete(key);
                reject(message instanceof Error ? message : new Error(message || '串口发送失败'));
            }
        });
    });
}

async function waitForUartWebSocketOpen(timeoutMs = 5000) {
    if (uartWebSocket && uartWebSocket.readyState === WebSocket.OPEN) {
        return;
    }

    if (!uartWebSocket || uartWebSocket.readyState === WebSocket.CLOSED) {
        initUartWebSocket();
    }

    const start = Date.now();
    while (true) {
        if (uartWebSocket && uartWebSocket.readyState === WebSocket.OPEN) {
            return;
        }
        if (Date.now() - start > timeoutMs) {
            throw new Error('串口WebSocket连接失败');
        }
        await new Promise(resolve => setTimeout(resolve, 100));
    }
}
function appendUartResult(timestamp, isTx, hexPayload, asciiPayload) {
    const resultElement = document.getElementById('devcie_report');
    if (!resultElement) return;

    const formattedTime = formatTimestamp(timestamp, !isTx);
    const direction = isTx ? '发→◇' : '收←◆';
    const showHex = document.getElementById('tran').checked;
    const content = showHex ? (hexPayload || '').trim() : (asciiPayload || '');

    resultElement.innerHTML += `[${formattedTime}]${direction}${content}<br>`;

    const lines = resultElement.innerHTML.split('<br>');
    if (lines.length > MAX_LINES) {
        resultElement.innerHTML = lines.slice(-MAX_LINES).join('<br>');
    }
    resultElement.scrollTop = resultElement.scrollHeight;
}

function getFrameKey(data) {
    const frameId = data.frameId || data.timestamp || `${Date.now()}`;
    const direction = data.is_tx ? 'tx' : 'rx';
    return `${frameId}-${direction}`;
}

function handleUartDataMessage(data) {
    if (!isReceiving) return;
    const chunkTotal = data.chunkTotal || data.chunk_total || 1;
    const chunkIndex = data.chunkIndex || data.chunk_index || 0;

    if (chunkTotal <= 1) {
        appendUartResult(data.timestamp, data.is_tx, data.hex, data.ascii);
        return;
    }

    const key = getFrameKey(data);
    let entry = uartChunkBuffer.get(key);
    if (!entry) {
        entry = {
            timestamp: data.timestamp,
            isTx: data.is_tx,
            chunkTotal,
            hexChunks: new Array(chunkTotal).fill(''),
            asciiChunks: new Array(chunkTotal).fill(''),
            received: 0,
            receivedSet: new Set()
        };
        uartChunkBuffer.set(key, entry);
    }

    if (!entry.receivedSet.has(chunkIndex)) {
        entry.receivedSet.add(chunkIndex);
        entry.received++;
    }
    entry.hexChunks[chunkIndex] = data.hex || '';
    entry.asciiChunks[chunkIndex] = data.ascii || '';

    if (entry.received >= entry.chunkTotal) {
        uartChunkBuffer.delete(key);
        const fullHex = entry.hexChunks.join('').trim();
        const fullAscii = entry.asciiChunks.join('');
        appendUartResult(entry.timestamp, entry.isTx, fullHex, fullAscii);
    }
}

// 获取当前时间的小时:分钟部分
function getCurrentTimePrefix() {
    const now = new Date();
    const hours = String(now.getHours()).padStart(2, '0');
    const minutes = String(now.getMinutes()).padStart(2, '0');
    return `${hours}:${minutes}`;
}

// 格式化时间戳，只处理毫秒部分，并减去100ms的延迟
function formatTimestamp(timestamp, isRx = false) {
    if (isRx) {
        timestamp = Math.max(0, timestamp - 100);
    }

    const seconds = Math.floor(timestamp / 1000000);
    const milliseconds = Math.floor((timestamp % 1000000) / 1000);

    return `${getCurrentTimePrefix()}:${String(seconds % 60).padStart(2, '0')}.${String(milliseconds).padStart(3, '0')}`;
}

// 初始化串口WebSocket连接
function initUartWebSocket() {
    try {
        uartWebSocket = new WebSocket(buildWebSocketUrl('/ws/log'));

        uartWebSocket.onopen = () => {
            console.log('串口WebSocket连接已建立');
        };

        uartWebSocket.onmessage = (event) => {
            try {
                let data;
                try {
                    data = JSON.parse(event.data);
                } catch (e) {
                    // 不是JSON格式，可能是普通日志消息，忽略
                    return;
                }

                if (data.type === 'uart_ack') {
                    const key = buildAckKey(data.transferId || '', data.chunkIndex || 0);
                    const handler = pendingUartAcks.get(key);
                    if (handler) {
                        if (data.success) {
                            handler.resolve();
                        } else {
                            handler.reject(data.message || '串口发送失败');
                        }
                    }
                    return;
                }

                if (data.type === 'uart_data') {
                    handleUartDataMessage(data);
                }
            } catch (error) {
                console.error('处理串口WebSocket消息时出错:', error);
            }
        };

        uartWebSocket.onclose = () => {
            console.log('串口WebSocket连接已关闭');
            rejectAllPendingAcks('串口WebSocket连接已关闭');
            uartWebSocket = null;
            // 5秒后尝试重新连接
            setTimeout(initUartWebSocket, 5000);
        };

        uartWebSocket.onerror = (error) => {
            console.error('串口WebSocket错误:', error);
            rejectAllPendingAcks('串口WebSocket错误');
        };
    } catch (error) {
        console.error('初始化串口WebSocket时出错:', error);
    }
}

// 修改轮询函数，保留以兼容旧版本，但不再主动调用
async function pollUartData() {
    // 此函数保留但不再使用，改为WebSocket方式接收数据
    console.log('pollUartData函数已弃用，改为使用WebSocket接收数据');
}

document.getElementById('toggleReceive').addEventListener('click', function () {
    const button = this;
    isReceiving = !isReceiving;

    if (isReceiving) {
        button.querySelector('.btn-container').textContent = '停止接收';
    } else {
        button.querySelector('.btn-container').textContent = '继续接收';
    }
});

// 添加清空按钮事件监听
document.getElementById('clearResponse').addEventListener('click', function () {
    const resultElement = document.getElementById('devcie_report');
    resultElement.innerHTML = ''; // 清空内容
});

// 修改页面加载时的初始化
document.addEventListener('DOMContentLoaded', () => {
    // 初始化串口WebSocket连接
    initUartWebSocket();

    // 不再使用轮询
    // pollInterval = setInterval(pollUartData, 50);
});

// 页面不可见时的处理
document.addEventListener('visibilitychange', () => {
    // 不再需要处理轮询间隔
});

// Net config API

window.onload = () => autoDHCP(document.getElementById('is_dhcp'));

function autoDHCP(element) {
    element.classList.toggle('on');
    element.classList.toggle('off');
    document.getElementById('is_dhcp').value = element.classList.contains('on') ? '2' : '1';
    ['static_ip', 'static_netmask', 'static_gateway', 'static_dns1', 'static_dns2'].forEach(id => {
        if (element.classList.contains('on')) {
            document.getElementById(id).removeAttribute('disabled');
        } else {
            document.getElementById(id).setAttribute('disabled', true);
        }
    });
}

function changeNetSelect(id) {
    [...document.getElementsByClassName('network-btn')].forEach(btn => btn.style.background = '#898989a1');
    document.getElementById(id).style.background = 'var(--BRAND)';
    document.getElementById('selectedNetwork').value = (id === 'ethernet') ? 1 : 2;

    // 保持静态IP设置不变，而不是重置它
    // 获取当前静态IP设置状态
    const currentDHCPValue = document.getElementById('is_dhcp').value;

    // 显示或隐藏WiFi相关字段
    ['wifi_ssid_div', 'wifi_password_div', 'wifisearch', 'scanList'].forEach(el =>
        document.getElementById(el).style.display = (id === 'ethernet') ? 'none' : 'flex');
}

document.getElementById("net_set").addEventListener("submit", event => event.preventDefault());

function validateIP(ip) {
    const ipPattern = /^(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$/;
    return ipPattern.test(ip);
}

async function netsetSubmit() {
    let isValid = true;
    const fields = [];
    const selectedNetwork = document.getElementById('selectedNetwork').value;
    const isDHCP = document.getElementById('is_dhcp').value;
    if ((selectedNetwork == '1' && isDHCP == '2') || (selectedNetwork == '2' && isDHCP == '2')) {
        fields.push({
            id: 'static_ip',
            errMsg: 'IP地址格式不正确',
            validate: validateIP
        }, {
            id: 'static_netmask',
            errMsg: '子网掩码格式不正确',
            validate: validateIP
        },
            {
                id: 'static_gateway',
                errMsg: '网关格式不正确',
                validate: validateIP
            },
            {
                id: 'static_dns1',
                errMsg: 'DNS1格式不正确',
                validate: validateIP
            },
            {
                id: 'static_dns2',
                errMsg: 'DNS2格式不正确',
                validate: validateIP,
                optional: true // DNS2是可选的
            });
    }
    if (selectedNetwork == '2') {
        const ssid = document.getElementById('wifi_ssid').value;
        const password = document.getElementById('wifi_password').value;
        if (!ssid) {
            // 修正获取WiFi名称错误信息容器的方式
            const ssidInput = document.getElementById('wifi_ssid');
            const ssidErrorContainer = ssidInput.parentElement.querySelector('.err-text');
            if (ssidErrorContainer) {
                ssidErrorContainer.textContent = 'WiFi名称不能为空';
                ssidErrorContainer.style.color = 'red';
                ssidErrorContainer.style.display = 'block';
                ssidInput.style.borderColor = 'red';
            }
            isValid = false;
        } else {
            // 清除错误提示
            const ssidInput = document.getElementById('wifi_ssid');
            const ssidErrorContainer = ssidInput.parentElement.querySelector('.err-text');
            if (ssidErrorContainer) {
                ssidErrorContainer.textContent = '';
                ssidErrorContainer.style.display = 'none';
            }
            ssidInput.style.borderColor = '';
        }

        if (!password || password.length < 8) {
            // 修正获取WiFi密码错误信息容器的方式
            // 找到WiFi密码输入框的父级元素下的err-text元素
            const passwordInput = document.getElementById('wifi_password');
            const errorContainer = passwordInput.parentElement.querySelector('.err-text');
            if (errorContainer) {
                errorContainer.textContent = 'WiFi密码不能为空且最少8位';
                // 添加样式让错误信息更明显
                errorContainer.style.color = 'red';
                errorContainer.style.display = 'block';
                // 让密码输入框有错误提示样式
                passwordInput.style.borderColor = 'red';

                // 调整眼睛图标位置
                const eyeOpen = document.getElementById('wifieyeOpen');
                const eyeClosed = document.getElementById('wifieyeClosed');
                if (eyeOpen) eyeOpen.style.top = '15px';
                if (eyeClosed) eyeClosed.style.top = '15px';
            }
            isValid = false;
        } else {
            // 清除错误提示
            const passwordInput = document.getElementById('wifi_password');
            const errorContainer = passwordInput.parentElement.querySelector('.err-text');
            if (errorContainer) {
                errorContainer.textContent = '';
                errorContainer.style.display = 'none';
            }
            passwordInput.style.borderColor = '';

            // 恢复眼睛图标位置
            const eyeOpen = document.getElementById('wifieyeOpen');
            const eyeClosed = document.getElementById('wifieyeClosed');
            if (eyeOpen) eyeOpen.style.top = '';
            if (eyeClosed) eyeClosed.style.top = '';
        }
    }

    for (const field of fields) {
        const input = document.getElementById(field.id);
        const errText = input.nextElementSibling;
        const value = input.value.trim();

        if (!value) {
            errText.textContent = field.errMsg;
            isValid = false;
        } else if (field.validate && !field.validate(value)) {
            errText.textContent = field.errMsg;
            isValid = false;
        } else {
            errText.textContent = '';
        }
    }

    if (!isValid) {
        return; // 如果有错误，不进行提交
    }

    const formData = new FormData(document.getElementById('net_set'));
    const data = Object.fromEntries(formData);

    // 防止切换网络模式时丢失静态IP设置
    // 如果当前页面上的静态IP开关是开启的，但表单中的is_dhcp值不匹配，修正它
    const staticIPSwitchIsOn = document.getElementById('static_ip_switch').classList.contains('on');
    if (staticIPSwitchIsOn && data.is_dhcp !== '2') {
        data.is_dhcp = '2';
    } else if (!staticIPSwitchIsOn && data.is_dhcp !== '1') {
        data.is_dhcp = '1';
    }

    try {
        await postData('/net_set', data);
        document.getElementById('netSetButton').textContent = window.i18n ? window.i18n.t('message.setSuccess') : "设置成功";
        if (!hasRestarted) {  // 检查是否已经发送了重启命令
            const deviceRestart = document.getElementById('deviceRestart');
            if (deviceRestart) {
                showSvg('deviceRestart', 6000);
                hasRestarted = true;  // 标记重启命令已经发送
            }
        }
    } catch (error) {
        console.error('Error fetching data. Status:', error);
        document.getElementById('netSetButton').textContent = window.i18n ? window.i18n.t('message.setFailed') : '设置失败';
    }
    setTimeout(() => document.getElementById('netSetButton').textContent = '设置', 2000);
}

document.getElementById('netSetButton').addEventListener('click', netsetSubmit);

// 为WIFI名称和密码输入框添加回车键事件监听，阻止表单提交
document.getElementById('wifi_ssid').addEventListener('keydown', function(event) {
    if (event.key === 'Enter') {
        event.preventDefault();
        event.stopPropagation();
    }
});

document.getElementById('wifi_password').addEventListener('keydown', function(event) {
    if (event.key === 'Enter') {
        event.preventDefault();
        event.stopPropagation();
    }
});

async function netsetfetchData() {
    try {
        const responseData = await fetchData('/net_set_info');
        if (!responseData) {
            console.error('No data returned from /net_set_info');
            return;
        }
        ['static_ip', 'static_netmask', 'static_gateway', 'static_dns1', 'static_dns2'].forEach(id => document.getElementById(id).value = responseData[id]);
        if (responseData.netconn == 1 || responseData.netconn == 0) {
            document.getElementById('ethernet').click();
        }
        if (responseData.netconn == 2) {
            document.getElementById('wifi').click();
            ['wifi_ssid', 'wifi_password'].forEach(id => document.getElementById(id).value = responseData[id]);
        }

        // 确保以正确的方式处理静态IP开关
        const static_ip_switch = document.getElementById('static_ip_switch');
        const isDHCP = responseData.is_dhcp || '1'; // 默认为动态IP(1)，如果未设置
        document.getElementById('is_dhcp').value = isDHCP; // 确保隐藏字段与实际值同步

        static_ip_switch.classList.remove('off', 'on');
        static_ip_switch.classList.add(isDHCP == '2' ? 'on' : 'off');

        ['static_ip', 'static_netmask', 'static_gateway', 'static_dns1', 'static_dns2'].forEach(id => {
            if (isDHCP == '1') {
                document.getElementById(id).setAttribute('disabled', true);
            } else {
                document.getElementById(id).removeAttribute('disabled');
            }
        });
    } catch (error) {
        console.error('Failed to fetch data. Status:', error);
    }
}

netsetfetchData();



// Search wifi API


function clickwifi(element) {
    // 清除WiFi相关的错误提示
    clearWifiErrors();

    // 获取当前选中的SSID
    const currentSSID = document.getElementById('wifi_ssid').value;
    // 获取新选中的SSID
    const newSSID = element.value;

    // 更新SSID输入框
    document.getElementById('wifi_ssid').value = newSSID;

    // 如果选择了不同的SSID，清空密码输入框
    if (currentSSID !== newSSID) {
        document.getElementById('wifi_password').value = '';
    }

    // 设置选中行的背景色
    [...document.querySelectorAll('#wifitable .addwifi')].forEach(row => row.style.backgroundColor = '');
    element.parentNode.parentNode.parentNode.style.backgroundColor = '#004ba93d';
}

// 添加清除WiFi相关错误的辅助函数
function clearWifiErrors() {
    // 清除SSID错误
    const ssidInput = document.getElementById('wifi_ssid');
    if (ssidInput) {
        ssidInput.style.borderColor = '';
        const ssidErrorContainer = ssidInput.parentElement.querySelector('.err-text');
        if (ssidErrorContainer) {
            ssidErrorContainer.textContent = '';
            ssidErrorContainer.style.display = 'none';
        }
    }

    // 清除密码错误
    const passwordInput = document.getElementById('wifi_password');
    if (passwordInput) {
        passwordInput.style.borderColor = '';
        const passwordErrorContainer = passwordInput.parentElement.querySelector('.err-text');
        if (passwordErrorContainer) {
            passwordErrorContainer.textContent = '';
            passwordErrorContainer.style.display = 'none';
        }

        // 恢复眼睛图标位置
        const eyeOpen = document.getElementById('wifieyeOpen');
        const eyeClosed = document.getElementById('wifieyeClosed');
        if (eyeOpen) eyeOpen.style.top = '';
        if (eyeClosed) eyeClosed.style.top = '';
    }
}

document.getElementById('wifisearch').addEventListener('click', async () => {
    const wifiSearchBtn = document.getElementById('wifisearch');

    // 如果按钮已经禁用，直接返回
    if (wifiSearchBtn.disabled) {
        return;
    }

    // 清除WiFi相关的任何错误提示
    clearWifiErrors();

    wifiSearchBtn.textContent = '搜索中';
    wifiSearchBtn.disabled = true;

    // 移除任何现有的消息，但保留WiFi列表
    const existingMessages = document.querySelectorAll('.error-message, .info-message');
    existingMessages.forEach(msg => {
        if (msg.parentNode) msg.parentNode.removeChild(msg);
    });

    try {
        const response = await fetch('/find_wifi');

        // 检查响应状态
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }

        const data = await response.json();

        // 处理错误情况
        if (data.error) {
            // 显示错误信息
            const errorMessage = document.createElement('div');
            errorMessage.className = 'error-message';
            errorMessage.textContent = data.error;
            document.getElementById('scanList').parentNode.insertBefore(errorMessage, document.getElementById('scanList'));

            // 自动移除错误信息
            setTimeout(() => {
                if (errorMessage && errorMessage.parentNode) {
                    errorMessage.parentNode.removeChild(errorMessage);
                }
            }, 5000);

            // 延迟一段时间后允许再次点击
            const waitTime = data.wait_time || 5000;

            // 显示倒计时
            let remainingTime = Math.ceil(waitTime / 1000);
            wifiSearchBtn.textContent = `请等待 ${remainingTime}秒`;

            const countdownInterval = setInterval(() => {
                remainingTime--;
                if (remainingTime <= 0) {
                    clearInterval(countdownInterval);
                    wifiSearchBtn.textContent = '搜索';
                    wifiSearchBtn.disabled = false;
                } else {
                    wifiSearchBtn.textContent = `请等待 ${remainingTime}秒`;
                }
            }, 1000);

            return; // 保留现有WiFi列表
        }

        // 处理未找到WiFi的情况
        if (data.message) {
            const messageElement = document.createElement('div');
            messageElement.className = 'info-message';
            messageElement.textContent = data.message;
            document.getElementById('scanList').parentNode.insertBefore(messageElement, document.getElementById('scanList'));

            setTimeout(() => {
                if (messageElement && messageElement.parentNode) {
                    messageElement.parentNode.removeChild(messageElement);
                }
            }, 5000);

            setTimeout(() => {
                wifiSearchBtn.textContent = '搜索';
                wifiSearchBtn.disabled = false;
            }, 1000);

            return; // 保留现有WiFi列表
        }

        // 只有在成功获取到新的WiFi列表时才清空并更新
        // 确保数据不为空
        if (Object.keys(data).length > 0) {
            document.getElementById('scanList').style.display = 'flex';
            [...document.getElementsByClassName('addwifi')].forEach(tr => tr.remove());

            // 移除任何可能存在的错误或信息消息
            const messages = document.querySelectorAll('.error-message, .info-message');
            messages.forEach(msg => msg.parentNode && msg.parentNode.removeChild(msg));

            // 转换成数组以便排序
            const wifiList = Object.entries(data).map(([ssid, rssi]) => ({ ssid, rssi }));

            // 按信号强度从强到弱排序
            wifiList.sort((a, b) => b.rssi - a.rssi);

            // 显示排序后的结果
            for (const wifi of wifiList) {
                const tr = document.createElement("tr");
                tr.setAttribute('class', 'addwifi');
                let td = document.createElement("td");
                td.innerHTML = `<label class='bui-radios-label'><input type='radio' name='wifi'  onclick='clickwifi(this)' value='${wifi.ssid}'/><i class='bui-radios'></i> ${wifi.ssid}</label>`;
                tr.appendChild(td);
                td = document.createElement("td");
                td.innerHTML = `${wifi.rssi}dBm `;
                tr.appendChild(td);
                document.getElementById('clusss').parentNode.insertBefore(tr, document.getElementById('clusss'));
            }
        } else {
            // 数据为空但不是错误情况，显示提示信息
            const messageElement = document.createElement('div');
            messageElement.className = 'info-message';
            messageElement.textContent = "未找到任何WiFi网络";
            document.getElementById('scanList').parentNode.insertBefore(messageElement, document.getElementById('scanList'));

            setTimeout(() => {
                if (messageElement && messageElement.parentNode) {
                    messageElement.parentNode.removeChild(messageElement);
                }
            }, 5000);
        }

        wifiSearchBtn.textContent = '搜索';
        wifiSearchBtn.disabled = false;
    } catch (error) {
        console.error('Error fetching data:', error);
        const errorMessage = document.createElement('div');
        errorMessage.className = 'error-message';
        errorMessage.textContent = '网络错误，请稍后重试';
        document.getElementById('scanList').parentNode.insertBefore(errorMessage, document.getElementById('scanList'));

        setTimeout(() => {
            if (errorMessage && errorMessage.parentNode) {
                errorMessage.parentNode.removeChild(errorMessage);
            }
        }, 3000);

        setTimeout(() => {
            wifiSearchBtn.textContent = '搜索';
            wifiSearchBtn.disabled = false;
        }, 1000);
    }
});

// MQTT config API

const useMqttStates = document.getElementById('useMqttState');
const mqttInputs = document.querySelectorAll('.mqtt-input');
const mqttButtons = document.querySelectorAll('.mqtt-btn');

function toggleInputs() {
    const isDisabled = useMqttState.value == '0';
    mqttInputs.forEach(input => input.disabled = isDisabled);
    mqttButtons.forEach(button => button.disabled = isDisabled);
}

toggleInputs();
useMqttStates.addEventListener('change', toggleInputs);

function toggleMqttSwitch(element) {
    element.classList.toggle('on');
    element.classList.toggle('off');
    document.getElementById('useMqttState').value = element.classList.contains('on') ? '1' : '0';
    toggleInputs();
}

function toggleMqttRetain(element) {
    element.classList.toggle('on');
    element.classList.toggle('off');
    document.getElementById('useMqttRetain').value = element.classList.contains('on') ? '1' : '0';
    toggleInputs();
}

function changeQosSelect(id) {
    var buttons = document.getElementsByClassName('qos-btn');
    Array.from(buttons).forEach(button => button.style.background = '#898989a1');
    document.getElementById('qos' + id).style.background = 'var(--BRAND)';
    document.getElementById('selectedQos').value = id;
}

document.getElementById("mqttDataForm").addEventListener("submit", event => event.preventDefault());

function isValidIPorDomain(value) {
    const ipPattern = /^(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$/;
    const domainPattern = /^(?!-)(?:[a-zA-Z0-9-]{0,62}[a-zA-Z0-9]\.)+[a-zA-Z]{2,}$/;
    return ipPattern.test(value) || domainPattern.test(value);
}
function validateIntervalTime(value) {
    const intervalTime = parseInt(value, 10);
    return !isNaN(intervalTime) && intervalTime > 2;
}

async function mqttSubmit() {
    const fields = [
        { id: 'mqtt_server', errMsg: '请输入服务器地址' },
        { id: 'mqtt_port', errMsg: '请输入服务器端口', isNumeric: true },
        { id: 'mqtt_username', errMsg: '请输入用户名' },
        { id: 'mqtt_clientid', errMsg: '请输入ClientID' },
        { id: 'mqtt_sub_topic', errMsg: '请输入订阅主题' },
        { id: 'mqtt_pub_topic', errMsg: '请输入发布主题' }
    ];

    for (const field of fields) {
        const input = document.getElementById(field.id);
        const errText = input.nextElementSibling;
        if (!input.value.trim()) {
            errText.textContent = field.errMsg;
            return;
        } else if (field.isNumeric && isNaN(input.value)) {
            errText.textContent = '请输入数字';
            return;
        } else {
            errText.textContent = '';
        }
    }

    const formData = new FormData(document.getElementById('mqttDataForm'));
    const data = Object.fromEntries(formData);
    try {
        await postData('/mqtt', data);
        document.getElementById('mqttSubmitButton').textContent = "保存成功";
        const deviceRestart = document.getElementById('deviceRestart');
        if (deviceRestart) {
            showSvg('deviceRestart', 6000);
        }
    } catch (error) {
        console.error('Error fetching data. Status:', error);
        document.getElementById('mqttSubmitButton').textContent = '保存失败';
    }
    setTimeout(() => document.getElementById('mqttSubmitButton').textContent = '保存', 2000);
}

document.getElementById('mqttSubmitButton').addEventListener('click', mqttSubmit);

async function mqttfetchData() {
    const maxRetries = 3;
    let retryCount = 0;
    let success = false;

    while (retryCount < maxRetries && !success) {
        try {
            const responseData = await fetchData('/mqtt_info');
            success = true;

            const mqttInputs = document.querySelectorAll('.mqtt-input');
            const mqttButtons = document.querySelectorAll('.mqtt-btn');
            const mqttconnected = document.getElementById('mqttconnected');

            if (mqttconnected && responseData) {
                document.getElementById('use_mqtt').className = responseData.use_mqtt == 1 ? "switch-btn switch-btn on" : "switch-btn switch-btn off";
                mqttInputs.forEach(input => input.disabled = responseData.use_mqtt == 0);
                mqttButtons.forEach(button => button.disabled = responseData.use_mqtt == 0);

                if (responseData.qos !== undefined && document.getElementById(`qos${responseData.qos}`)) {
                    document.getElementById(`qos${responseData.qos}`).click();
                }

                ['mqtt_server', 'mqtt_port', 'mqtt_username', 'mqtt_password', 'mqtt_clientid', 'mqtt_sub_topic', 'mqtt_pub_topic'].forEach(id => {
                    const element = document.getElementById(id);
                    if (element && responseData[id] !== undefined) {
                        element.value = responseData[id];
                    }
                });

                const useMqttState = document.getElementById('useMqttState');
                if (useMqttState) useMqttState.value = responseData.use_mqtt;

                const useMqttType = document.getElementById('useMqttType');
                if (useMqttType) useMqttType.value = responseData.mqtt_type;

                const useMqttRetain = document.getElementById('useMqttRetain');
                if (useMqttRetain) useMqttRetain.value = responseData.retain;

                const selectRetain = document.getElementById('selectRetain');
                if (selectRetain) selectRetain.className = responseData.retain == 1 ? "switch-btn switch-btn on" : "switch-btn switch-btn off";

                // 添加调试信息
                console.log('MQTT Status Debug:', {
                    mqttconn: responseData.mqttconn,
                    mqttconn_type: typeof responseData.mqttconn,
                    use_mqtt: responseData.use_mqtt,
                    use_mqtt_type: typeof responseData.use_mqtt
                });

                // 修复判断逻辑，兼容字符串和数字类型
                const isMqttConnected = responseData.mqttconn == "1" || responseData.mqttconn === 1;
                const isMqttEnabled = responseData.use_mqtt == "1" || responseData.use_mqtt === 1;

                mqttconnected.textContent = isMqttConnected && isMqttEnabled ? "已连接" : "未连接";
                mqttconnected.style.color = isMqttConnected && isMqttEnabled ? "#5cb85c" : "#d9534f";
            }
        } catch (error) {
            console.error(`尝试 ${retryCount + 1}/${maxRetries} 失败，原因:`, error);
            retryCount++;

            // 如果不是最后一次重试，则等待一段时间后再重试
            if (retryCount < maxRetries) {
                await new Promise(resolve => setTimeout(resolve, 1000 * retryCount)); // 递增等待时间
            } else {
                console.error('MQTT数据获取失败，已达到最大重试次数。最终错误:', error);

                // 在UI上显示错误信息
                const mqttconnected = document.getElementById('mqttconnected');
                if (mqttconnected) {
                    mqttconnected.textContent = "获取失败";
                    mqttconnected.style.color = "#d9534f";
                }

                // 可以选择显示一个用户友好的错误提示
                showCustomAlert("MQTT配置信息获取失败，请检查网络连接或设备状态。", true);
            }
        }
    }
}

// 确保在页面加载时执行
document.addEventListener('DOMContentLoaded', () => {
    // 其他现有的DOMContentLoaded事件处理程序

    // 延迟执行mqttfetchData以确保DOM已完全加载
    setTimeout(mqttfetchData, 500);
});

// 如果已经直接调用了mqttfetchData，可以删除或注释下面这行
// mqttfetchData();


// TCP config API

    document.addEventListener('DOMContentLoaded', () => {
        const useTCPState = document.getElementById('useTCPState');
        const tcpInputs = document.querySelectorAll('.tcp-input');
        const tcpButtons = document.querySelectorAll('.tcp-btn');
        const tcpSubmitButton = document.getElementById('tcpSubmitButton');
        const tcpDataForm = document.getElementById('tcpDataForm');
        const packetFieldConfigs = [
            { selectId: 'regFormat', inputId: 'reg_packet' },
            { selectId: 'heartFormat', inputId: 'heart_packet' },
        ];

        function updatePacketInputState(selectEl, inputEl) {
            const tcpDisabled = useTCPState.value === '0';
            const disabledByFormat = selectEl.value === 'none';
            const shouldDisable = tcpDisabled || disabledByFormat;
            inputEl.disabled = shouldDisable;
            if (disabledByFormat && !tcpDisabled) {
                inputEl.classList.add('tcp-input-disabled');
            } else {
                inputEl.classList.remove('tcp-input-disabled');
            }
        }

        function applyAllPacketFieldStates() {
            packetFieldConfigs.forEach(({ selectId, inputId }) => {
                const selectEl = document.getElementById(selectId);
                const inputEl = document.getElementById(inputId);
                if (selectEl && inputEl) {
                    updatePacketInputState(selectEl, inputEl);
                }
            });
        }

        function setTcpModeUI(mode, options = {}) {
            const { preservePortValue = false } = options;
            const normalizedMode = ['0', '1', '2', '3'].includes(mode) ? mode : '1';
            const isModbusTcpClientMode = normalizedMode === '3';
            const tcpServerBtn = document.getElementById('tcpServer');
            const tcpClientBtn = document.getElementById('tcpClient');
            const tcpModbusServerBtn = document.getElementById('tcpModbusServer');
            const tcpModbusClientBtn = document.getElementById('tcpModbusClient');
            const tcpServerAddress = document.getElementById('tcpServerAddress');
            const tcpRegisterPacket = document.getElementById('tcpRegisterPacket');
            const tcpHeartPacket = document.getElementById('tcpHeartPacket');
            const tcpHeartInterval = document.getElementById('tcpHeartInterval');
            const tcpPortInput = document.getElementById('tcp_port');
            const selectedTCPInput = document.getElementById('selectedTCP');
            const regFormatSelect = document.getElementById('regFormat');
            const regPacketInput = document.getElementById('reg_packet');
            const heartFormatSelect = document.getElementById('heartFormat');
            const heartPacketInput = document.getElementById('heart_packet');

            if (selectedTCPInput) {
                selectedTCPInput.value = normalizedMode;
            }

            const buttons = [
                { btn: tcpServerBtn, id: '0' },
                { btn: tcpClientBtn, id: '1' },
                { btn: tcpModbusServerBtn, id: '2' },
                { btn: tcpModbusClientBtn, id: '3' },
            ];
            buttons.forEach(({ btn, id }) => {
                if (btn) {
                    btn.style.background = normalizedMode === id ? 'var(--BRAND)' : '#898989a1';
                    btn.disabled = false;
                }
            });

            if (tcpServerAddress) {
                tcpServerAddress.style.display = (normalizedMode === '1' || normalizedMode === '3') ? 'flex' : 'none';
            }
            if (tcpRegisterPacket) {
                tcpRegisterPacket.style.display = isModbusTcpClientMode ? 'none' : (normalizedMode === '1' ? 'flex' : 'none');
            }
            if (tcpHeartPacket) {
                tcpHeartPacket.style.display = (normalizedMode === '2' || isModbusTcpClientMode) ? 'none' : 'flex';
            }
            if (tcpHeartInterval) {
                tcpHeartInterval.style.display = (normalizedMode === '2' || isModbusTcpClientMode) ? 'none' : 'flex';
            }

            if (tcpPortInput) {
                tcpPortInput.placeholder = (normalizedMode === '2' || normalizedMode === '3')
                    ? '默认端口 502'
                    : '请输入服务器端口，如：8888';
                if (!preservePortValue) {
                    const trimmed = tcpPortInput.value.trim();
                    if ((normalizedMode === '2' || normalizedMode === '3') && trimmed === '') {
                        tcpPortInput.value = '502';
                    } else if (normalizedMode === '0' && trimmed === '') {
                        tcpPortInput.value = '8888';
                    }
                }
            }

            if (isModbusTcpClientMode) {
                if (regFormatSelect) {
                    regFormatSelect.value = 'none';
                    regFormatSelect.dataset.prevValue = 'none';
                }
                if (regPacketInput) {
                    regPacketInput.value = '';
                    regPacketInput.dataset.hexBackup = '';
                    regPacketInput.dataset.asciiBackup = '';
                    regPacketInput.dataset.convertStatus = '';
                }
                if (heartFormatSelect) {
                    heartFormatSelect.value = 'none';
                    heartFormatSelect.dataset.prevValue = 'none';
                }
                if (heartPacketInput) {
                    heartPacketInput.value = '';
                    heartPacketInput.dataset.hexBackup = '';
                    heartPacketInput.dataset.asciiBackup = '';
                    heartPacketInput.dataset.convertStatus = '';
                }
            }

            applyAllPacketFieldStates();
        }

        window.setTcpModeUI = setTcpModeUI;

        function toggleInputs(isDisabled) {
            tcpInputs.forEach(input => input.disabled = isDisabled);
            tcpButtons.forEach(button => button.disabled = isDisabled);
        }

        function tcptoggleInputs() {
            toggleInputs(useTCPState.value === '0');
            applyAllPacketFieldStates();
        }

        useTCPState.addEventListener('change', tcptoggleInputs);
        tcptoggleInputs();

        window.toggleTCPSwitch = function (element) {
            element.classList.toggle('on');
            element.classList.toggle('off');
            const useTCPState = document.getElementById('useTCPState');
            useTCPState.value = element.classList.contains('on') ? '1' : '0';

            // 更新输入框状态
            const isDisabled = !element.classList.contains('on');
            tcpInputs.forEach(input => input.disabled = isDisabled);
            tcpButtons.forEach(button => button.disabled = isDisabled);
            applyAllPacketFieldStates();
        }


        window.changeTCPSelect = function (id) {
            const modeMap = {
                tcpServer: '0',
                tcpClient: '1',
                tcpModbusServer: '2',
                tcpModbusClient: '3',
            };
            const mode = modeMap[id] || '1';
            setTcpModeUI(mode);

            const useTCPState = document.getElementById('useTCPState');
            const tcpSwitchBtn = document.getElementById('use_tcp_server');
            if (tcpSwitchBtn.classList.contains('on')) {
                useTCPState.value = '1';
            }
        }

        function convertPacketValue(inputEl, targetFormat) {
            const currentValue = inputEl.value.trim();
            const previousHex = inputEl.dataset.hexBackup || '';
            const previousAscii = inputEl.dataset.asciiBackup || '';
            const convertStatus = inputEl.dataset.convertStatus || '';

            if (targetFormat === 'ascii') {
                if (currentValue) {
                    const asciiValue = hexToAscii(currentValue);
                    if (asciiValue !== currentValue) {
                        inputEl.dataset.hexBackup = currentValue;
                        inputEl.dataset.asciiBackup = asciiValue;
                        inputEl.dataset.convertStatus = 'converted';
                        inputEl.value = asciiValue;
                    } else {
                        inputEl.dataset.hexBackup = currentValue;
                        inputEl.dataset.asciiBackup = '';
                        inputEl.dataset.convertStatus = 'failed';
                        showCustomAlert('已切换为 ASCII 格式，但当前内容无法自动转换，请手动输入有效的 ASCII 文本。', true);
                    }
                } else {
                    inputEl.dataset.hexBackup = '';
                    inputEl.dataset.asciiBackup = '';
                    inputEl.dataset.convertStatus = '';
                }
            } else if (targetFormat === 'hex') {
                let nextValue = currentValue;

                if (convertStatus === 'converted' && previousAscii && currentValue === previousAscii && previousHex) {
                    nextValue = previousHex;
                } else if (convertStatus === 'failed') {
                    if (previousHex && currentValue === previousHex) {
                        nextValue = previousHex;
                    } else {
                        nextValue = asciiToHex(currentValue);
                        inputEl.dataset.hexBackup = nextValue;
                    }
                } else if (currentValue) {
                    nextValue = asciiToHex(currentValue);
                    inputEl.dataset.hexBackup = nextValue;
                }

                inputEl.value = nextValue;
                inputEl.dataset.asciiBackup = '';
                inputEl.dataset.convertStatus = '';
            }
        }

        function setupPacketFormatField(selectId, inputId) {
            const selectEl = document.getElementById(selectId);
            const inputEl = document.getElementById(inputId);
            if (!selectEl || !inputEl) return;

            selectEl.dataset.prevValue = selectEl.value;
            updatePacketInputState(selectEl, inputEl);

            selectEl.addEventListener('change', () => {
                const prevValue = selectEl.dataset.prevValue || 'hex';
                const currentValue = selectEl.value;

                if (currentValue !== 'none' && prevValue !== currentValue && prevValue !== 'none') {
                    convertPacketValue(inputEl, currentValue);
                }

                selectEl.dataset.prevValue = currentValue;
                updatePacketInputState(selectEl, inputEl);
            });
        }

        packetFieldConfigs.forEach(({ selectId, inputId }) => setupPacketFormatField(selectId, inputId));

    // HEX转ASCII函数
    function hexToAscii(hexString) {
        try {
            // 移除空格和非十六进制字符
            const cleanHex = hexString.replace(/[^0-9A-Fa-f]/g, '');
            if (cleanHex.length % 2 !== 0) {
                return hexString; // 如果不是有效的HEX字符串，返回原值
            }

            let ascii = '';
            for (let i = 0; i < cleanHex.length; i += 2) {
                const hexByte = cleanHex.substr(i, 2);
                const charCode = parseInt(hexByte, 16);
                // 只转换可打印字符(32-126)
                if (charCode >= 32 && charCode <= 126) {
                    ascii += String.fromCharCode(charCode);
                } else {
                    return hexString; // 如果包含不可打印字符，返回原值
                }
            }
            return ascii;
        } catch (e) {
            return hexString; // 转换失败返回原值
        }
    }

    // ASCII转HEX函数
    function asciiToHex(asciiString) {
        try {
            let hex = '';
            for (let i = 0; i < asciiString.length; i++) {
                const charCode = asciiString.charCodeAt(i);
                hex += charCode.toString(16).toUpperCase().padStart(2, '0');
            }
            return hex;
        } catch (e) {
            return asciiString; // 转换失败返回原值
        }
    }

    tcpDataForm.addEventListener('submit', event => event.preventDefault());

    async function tcpSubmit() {
        const backendApiUrl = '/tcp';
        const formData = new FormData(tcpDataForm);
        const selectedTcpMode = document.getElementById('selectedTCP')?.value || '1';

        // 检查TCP开关的状态，确保use_tcp值正确
        const tcpSwitchBtn = document.getElementById('use_tcp_server');
        if (tcpSwitchBtn.classList.contains('on')) {
            formData.set('use_tcp', '1');
        }

        // ModbusTCPClient 模式固定无注册包/无心跳包
        if (selectedTcpMode === '3') {
            formData.set('reg_format', 'none');
            formData.set('reg_packet', '');
            formData.set('heart_format', 'none');
            formData.set('heart_packet', '');
            formData.set('heart_interval', '30');
        }

        const json = JSON.stringify(Object.fromEntries(formData.entries()));

        try {
            const response = await fetch(backendApiUrl, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: json
            });

            const data = await response.json();
            tcpSubmitButton.textContent = data.code === 200 ? '保存成功' : '保存失败';

            if (data.code === 200) {
                const deviceRestart = document.getElementById('deviceRestart');
                if (deviceRestart) {
                    showSvg('deviceRestart', 6000);
                }
            }

            setTimeout(() => {
                tcpSubmitButton.textContent = '保存';
            }, 2000);
        } catch (error) {
            console.error('Error submitting data:', error);
        }
    }

    tcpSubmitButton.addEventListener('click', tcpSubmit);

    async function tcpfetchData() {
        const apiUrl = '/tcp_info';

        try {
            const response = await fetch(apiUrl);
            if (!response.ok) throw new Error(`Failed to fetch data. Status: ${response.status}`);

            const responseData = await response.json();
            document.getElementById('tcp_server').value = responseData.tcp_server;

            // 获取设备MAC地址
            let deviceMac = '';
            try {
                const devInfoResponse = await fetch('/devinfo');
                if (devInfoResponse.ok) {
                    const devInfo = await devInfoResponse.json();
                    // 使用STA MAC地址（去掉冒号）
                    if (devInfo.device_sta_mac) {
                        deviceMac = devInfo.device_sta_mac.replace(/:/g, '');
                    }
                }
            } catch (error) {
                console.error('Error fetching device info:', error);
            }

            const tcpMode = responseData.tcpconn || '0';

            // 设置注册包和心跳包的值
            // 如果服务器没有返回注册包值，使用设备MAC地址作为默认值
            document.getElementById('reg_packet').value =
                tcpMode === '3' ? '' : (responseData.reg_packet || deviceMac || '');

            // 如果服务器没有返回心跳包值，使用设备MAC地址作为默认值
            document.getElementById('heart_packet').value = responseData.heart_packet
                || (tcpMode === '3' ? '' : (deviceMac || ''));

            if (responseData.heart_interval) {
                document.getElementById('heart_interval').value = responseData.heart_interval;
            } else {
                document.getElementById('heart_interval').value = '30'; // 默认30秒
            }

            const regFormatSelect = document.getElementById('regFormat');
            const heartFormatSelect = document.getElementById('heartFormat');
            const allowedFormats = ['none', 'hex', 'ascii'];

            if (regFormatSelect) {
                const regValue = tcpMode === '3'
                    ? 'none'
                    : (allowedFormats.includes(responseData.reg_format) ? responseData.reg_format : 'hex');
                regFormatSelect.value = regValue || 'hex';
                regFormatSelect.dataset.prevValue = regFormatSelect.value;
            }

            if (heartFormatSelect) {
                const heartValue = allowedFormats.includes(responseData.heart_format)
                    ? responseData.heart_format
                    : (tcpMode === '3' ? 'none' : 'ascii');
                heartFormatSelect.value = heartValue || 'ascii';
                heartFormatSelect.dataset.prevValue = heartFormatSelect.value;
            }

            applyAllPacketFieldStates();

            if (responseData.use_tcp == 0) {
                document.getElementById('use_tcp_server').className = "switch-btn tcp-switch-btn off";
                document.getElementById('useTCPState').value = '0';
                toggleInputs(true);
            } else {
                document.getElementById('use_tcp_server').className = "switch-btn tcp-switch-btn on";
                document.getElementById('useTCPState').value = '1';
                toggleInputs(false);
            }

            const tcpPortInput = document.getElementById('tcp_port');
            const resolvedPort = responseData.tcp_port && responseData.tcp_port.length > 0
                ? responseData.tcp_port
                : ((tcpMode === '2' || tcpMode === '3') ? '502' : '8888');
            if (tcpPortInput) {
                tcpPortInput.value = resolvedPort;
            }

            setTcpModeUI(tcpMode, { preservePortValue: true });

            const tcpConnected = document.getElementById('tcpconnected');
            if (tcpConnected) {
                if (tcpMode === '1' || tcpMode === '3') {
                    tcpConnected.textContent = responseData.tcp_server_conn == 1 ? '已连接' : '未连接';
                    tcpConnected.style.color = responseData.tcp_server_conn == 1 ? "#5cb85c" : "#d9534f";
                } else {
                    tcpConnected.textContent = responseData.tcp_client_count > 0
                        ? `当前有${responseData.tcp_client_count}个客户端连接`
                        : '没有设备连接';
                    tcpConnected.style.color = responseData.tcp_client_count > 0 ? "#5cb85c" : "#d9534f";
                }
            }
        } catch (error) {
            console.error('Failed to fetch data:', error);
        }
    }

    tcpfetchData();
    // setInterval(tcpfetchData, 12000);
});

// HTTP config API

document.addEventListener('DOMContentLoaded', () => {
    const useHTTPState = document.getElementById('useHTTPState');
    const httpInputs = document.querySelectorAll('.http-input');
    const httpButtons = document.querySelectorAll('.http-btn');
    const httpSubmitButton = document.getElementById('httpSubmitButton');
    const httpDataForm = document.getElementById('httpDataForm');
    const httpSwitchElement = document.getElementById('use_http');
    const httpModeWarningMessage = '建议在工作模式中开启Modbus-RTU网关模式，以确保HTTP功能正常使用';
    let httpModeWarningShown = false;

    function toggleInputs(isDisabled) {
        httpInputs.forEach(input => input.disabled = isDisabled);
        httpButtons.forEach(button => button.disabled = isDisabled);
    }

    function httptoggleInputs() {
        toggleInputs(useHTTPState.value === '0');
    }

    useHTTPState.addEventListener('change', httptoggleInputs);
    httptoggleInputs();

    window.toggleHTTPSwitch = async function (element) {
        let shouldShowWarning = false;

        if (!element.classList.contains('on')) {
            try {
                const workModeData = await fetchWorkModeInfoPage(0, 0);
                if (!workModeData || workModeData.work_mode !== 'modbus_rtu') {
                    shouldShowWarning = true;
                }
            } catch (error) {
                console.error('获取工作模式失败:', error);
                shouldShowWarning = true;
            }
        }

        element.classList.toggle('on');
        element.classList.toggle('off');
        useHTTPState.value = element.classList.contains('on') ? '1' : '0';
        httptoggleInputs();

        if (useHTTPState.value === '0') {
            httpModeWarningShown = false;
        } else if (shouldShowWarning) {
            showCustomAlert(httpModeWarningMessage, true);
            httpModeWarningShown = true;
        } else {
            httpModeWarningShown = false;
        }
    }

    window.changeHTTPSelect = function (id) {
        httpButtons.forEach(button => button.style.background = '#898989a1');
        if (id === 'http_post') {
            document.getElementById('http_post').style.background = 'var(--BRAND)';
        }
        document.getElementById('selectedHTTP').value = '1';
    }

    httpDataForm.addEventListener('submit', event => event.preventDefault());

    async function httpSubmit() {
        // 注释：移除工作模式检查以避免循环依赖
        // 用户可以先配置HTTP协议，再设置工作模式为Modbus-RTU
        // 后端会在实际使用时进行验证
        
        const fields = [
            { id: 'http_url', errMsg: '请输入有效的HTTP URI，格式如：https://likong-iot.com:8080', validate: validateHttpUri },
            // { id: 'http_time', errMsg: '请输入间隔时间，且必须大于2秒', validate: validateIntervalTime },
        ];

        for (const field of fields) {
            const input = document.getElementById(field.id);
            const errText = input.nextElementSibling;
            if (!input.value.trim()) {
                errText.textContent = field.errMsg;
                return;
            } else if (field.validate && !field.validate(input.value)) {
                errText.textContent = field.errMsg;
                return;
            } else {
                errText.textContent = '';
            }
        }

        const backendApiUrl = '/http';
        const formData = new FormData(httpDataForm);
        formData.set('use_http', useHTTPState.value);
        const json = JSON.stringify(Object.fromEntries(formData.entries()));
        try {
            const response = await fetch(backendApiUrl, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                    'Connection': 'close'  // 禁用Keep-Alive
                },
                body: json
            });

            const data = await response.json();
            httpSubmitButton.textContent = data.code === 200 ?
                (window.i18n ? window.i18n.t('message.setSuccess') : '设置成功') :
                (window.i18n ? window.i18n.t('message.setFailed') : '设置失败');
            if (data.code === 200) {
                if (!hasRestarted) {
                    const deviceRestart = document.getElementById('deviceRestart');
                    if (deviceRestart) {
                        showSvg('deviceRestart', 6000);
                        hasRestarted = true;
                    }
                }
            }
            setTimeout(() => {
                httpSubmitButton.textContent = '设置';
            }, 2000);
        } catch (error) {
            console.error('Error submitting data:', error);
        }
    }

    httpSubmitButton.addEventListener('click', httpSubmit);

    async function httpfetchData() {
        const apiUrl = '/http_info';

        try {
            // 先检查工作模式
            const workModeData = await fetchWorkModeInfoPage(0, 0);
            const isModbusRtuMode = workModeData && workModeData.work_mode === 'modbus_rtu';

            const response = await fetch(apiUrl);
            if (!response.ok) throw new Error(`Failed to fetch data. Status: ${response.status}`);
            const responseData = await response.json();

            const httpEnabled = responseData.use_http == 1;
            httpSwitchElement.className = `switch-btn http-switch-btn ${httpEnabled ? 'on' : 'off'}`;
            useHTTPState.value = httpEnabled ? '1' : '0';
            httptoggleInputs();

            if (!httpEnabled) {
                httpModeWarningShown = false;
            } else if (!isModbusRtuMode) {
                httpModeWarningShown = false;
            } else {
                httpModeWarningShown = false;
            }

            document.getElementById('http_url').value = responseData.http_url || '';
            // document.getElementById('http_time').value = responseData.http_time || '';
            document.getElementById('http_post').style.background = 'var(--BRAND)';

        } catch (error) {
            console.error('Failed to fetch data:', error);
        }
    }
    httpfetchData();
});

function validateHttpUri(value) {
    const httpPattern = /^(https?:\/\/)((\d{1,3}\.){3}\d{1,3}|([a-zA-Z0-9-]+\.)+[a-zA-Z]{2,})(:\d{1,5})?(\/.*)?$/;
    return httpPattern.test(value);
}


// System data API

const systemElements = {
    version: document.getElementById('version'),
    free_heap_size: document.getElementById('free_heap_size'),
    min_ever_free_heap_size: document.getElementById('min_ever_free_heap_size'),
    time_since_boot: document.getElementById('time_since_boot'),
    res_reason: document.getElementById('res_reason'),
    active_sockets: document.getElementById('active_sockets'),
    nvs_usage: document.getElementById('nvs_usage')
};

function formatTime(microseconds) {
    const totalSeconds = Math.floor(microseconds / 1000000);
    const days = Math.floor(totalSeconds / (24 * 3600));
    const hours = Math.floor((totalSeconds % (24 * 3600)) / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = totalSeconds % 60;

    return `${days > 0 ? days + " day " : ""}${hours > 0 ? hours + " hour " : ""}${minutes > 0 ? minutes + " minute " : ""}${seconds > 0 ? seconds + " second" : ""}`.trim();
}

async function sysfetchData() {
    try {
        const responseData = await fetchData('/sys');
        systemElements.version.textContent = responseData.version;
        systemElements.free_heap_size.textContent = (responseData.free_heap_size).toFixed(2) + " KB ";
        systemElements.min_ever_free_heap_size.textContent = (responseData.min_ever_free_heap_size).toFixed(2) + " KB ";
        systemElements.time_since_boot.textContent = formatTime(responseData.time_since_boot);
        systemElements.res_reason.textContent = responseData.res_reason;
        if (systemElements.active_sockets && responseData.active_sockets !== undefined) {
            systemElements.active_sockets.textContent = responseData.active_sockets;
        }
            // 处理NVS使用率
        if (systemElements.nvs_usage && responseData.nvs_usage !== undefined) {
            if (typeof responseData.nvs_usage === 'number') {
                systemElements.nvs_usage.textContent = responseData.nvs_usage.toFixed(2) + "%";
                // 根据使用率设置颜色警告
                if (responseData.nvs_usage > 80) {
                    systemElements.nvs_usage.style.color = 'red';
                } else if (responseData.nvs_usage > 60) {
                    systemElements.nvs_usage.style.color = 'orange';
                } else {
                    systemElements.nvs_usage.style.color = 'green';
                }
            } else {
                systemElements.nvs_usage.textContent = responseData.nvs_usage;
                systemElements.nvs_usage.style.color = 'gray';
            }
        }
    } catch (error) {
        console.error('Failed to fetch data. Status:', error);
        Object.values(systemElements).forEach(element => {
            if (element) element.textContent = "---";
        });
    }
}



setInterval(sysfetchData, 5000);
sysfetchData();

// Module set API

document.getElementById("module_set").addEventListener("submit", event => event.preventDefault());

async function modulesetSubmit() {
    const formData = new FormData(document.getElementById('module_set'));
    const data = Object.fromEntries(formData);
    try {
        await postData('/module_set', data);
        document.getElementById('moduleSetButton').textContent = window.i18n ? window.i18n.t('message.setSuccess') : "设置成功";
    } catch (error) {
        console.error('Error fetching data. Status:', error);
        document.getElementById('moduleSetButton').textContent = window.i18n ? window.i18n.t('message.setFailed') : '设置失败';
    }
    setTimeout(() => document.getElementById('moduleSetButton').textContent = '设置', 2000);
}

document.getElementById('moduleSetButton').addEventListener('click', modulesetSubmit);

async function modulesetfetchData() {
    try {
        const responseData = await fetchData('/module_set_info');
        ['host_names', 'lgname', 'lgpwd'].forEach(id => document.getElementById(id).value = responseData[id]);
        document.getElementById('moduleSetButton').textContent = responseData.host_names != "以太网串口服务器" ? '修改' : '保存';
    } catch (error) {
        console.error('Failed to fetch data. Status:', error);
    }
}

modulesetfetchData();

// OTA API

// document.getElementById("otaDataForm").addEventListener("submit", event => event.preventDefault());


async function otaSubmit() {
    const formData = new FormData(document.getElementById('otaDataForm'));
    const data = Object.fromEntries(formData);
    const progressDiv = document.getElementById('otaProgress');
    const progressBar = document.getElementById('progressBar');
    const progressText = document.getElementById('progressText');
    const otaStatus = document.getElementById('otaStatus');

    try {
        // 显示进度条
        progressDiv.style.display = 'block';
        progressBar.style.width = '0%';
        otaStatus.className = 'status-message status-progress';

        document.getElementById('otaSubmitButton').disabled = true;
        document.getElementById('ota_url').disabled = true;

        // 发送OTA请求
        const response = await postData('/ota', data);
        if (response.code !== 200) {
            throw new Error('OTA启动失败');
        }

        // 开始轮询进度
        const checkProgress = async () => {
            try {
                const progressResponse = await fetch('/ota_progress');
                const progressData = await progressResponse.json();

                // 更新进度条
                progressBar.style.width = `${progressData.progress}%`;
                progressText.textContent = `${progressData.progress}%`;
                otaStatus.textContent = progressData.status;

                if (progressData.progress >= 100) {
                    otaStatus.textContent = '更新成功，设备即将重启...';
                    otaStatus.className = 'status-message status-success';
                    progressBar.style.background = 'linear-gradient(90deg, #4CAF50, #45a049)';
                    setTimeout(() => location.reload(), 5000);
                    return;
                }

                if (progressData.status.includes('失败') || progressData.status.includes('error')) {
                    otaStatus.className = 'status-message status-error';
                    progressBar.style.background = '#f44336';
                    document.getElementById('otaSubmitButton').disabled = false;
                    document.getElementById('ota_url').disabled = false;
                    return;
                }

                // 继续轮询
                setTimeout(checkProgress, 1000);
            } catch (error) {
                console.error('获取进度失败:', error);
                otaStatus.textContent = '获取升级进度失败';
                otaStatus.className = 'status-message status-error';
                progressBar.style.background = '#f44336';
            }
        };

        // 开始检查进度
        checkProgress();

    } catch (error) {
        console.error('Error:', error);
        progressDiv.style.display = 'none';
        document.getElementById('otaSubmitButton').disabled = false;
        document.getElementById('ota_url').disabled = false;
        otaStatus.textContent = '升级失败: ' + error.message;
        otaStatus.className = 'status-message status-error';
    }
}

// 确保表单提交事件被正确处理
document.getElementById('otaDataForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    await otaSubmit();
});

document.getElementById('otaSubmitButton').addEventListener('click', otaSubmit);


// Opreate API

const observer = new MutationObserver(mutations => {
    mutations.forEach(mutation => {
        if (mutation.type === 'childList') {
            const deviceRestart = document.getElementById('deviceRestart');
            const deviceReset = document.getElementById('deviceReset');
            if (deviceRestart && deviceRestart.style.display !== 'none') {
                handleDeviceRestart();
            }
            if (deviceReset && deviceReset.style.display !== 'none') {
                handleDeviceReset();
            }
        }
    });
});

const config = { attributes: true, childList: true, subtree: true };
observer.observe(document.body, config);

async function handleDeviceRestart() {
    document.getElementById('cancelRestart').addEventListener('click', () => {
        deviceRestart.style.display = 'none';
    });
    document.getElementById('restart').addEventListener('click', async () => {
        deviceRestart.style.display = 'none';
        try {
            await fetchData('/operate');
            displaySuccessMessage('重启中，请稍后...');
            setTimeout(() => location.reload(), 5000);
        } catch (error) {
            console.error('Error:', error);
        }
    });
}

async function handleDeviceReset() {
    document.getElementById('cancelReset').addEventListener('click', () => {
        deviceReset.style.display = 'none';
    });
    document.getElementById('restore').addEventListener('click', async () => {
        deviceReset.style.display = 'none';
        try {
            await fetchData('/restore');
            displaySuccessMessage('重置中，请稍后...');
            setTimeout(() => location.reload(), 5000);
        } catch (error) {
            console.error('Error:', error);
        }
    });
}

function displaySuccessMessage(message) {
    let svg = document.getElementById('successMessage');
    svg.style.display = 'flex';
    document.getElementById('msgValue').textContent = message;
    const clone = svg.cloneNode(true);
    svg.parentNode.replaceChild(clone, svg);
    svg = clone;
    setTimeout(() => svg.style.display = 'none', 1200);
}

function displayErrorMessage(message) {
    let svg = document.getElementById('successMessage');
    svg.style.display = 'flex';
    document.getElementById('msgValue').textContent = message;
    const clone = svg.cloneNode(true);
    svg.parentNode.replaceChild(clone, svg);
    svg = clone;
    setTimeout(() => svg.style.display = 'none', 1200);
}

function displayInfoMessage(message) {
    if (!message) return;
    displaySuccessMessage(message);
}


// log API

const detailLogTab = document.getElementById('detailLogTab');
const simpleLogTab = document.getElementById('simpleLogTab');
let currentLogType = 'detail';
let ws = null;
let reconnectAttempts = 0;
const MAX_RECONNECT_ATTEMPTS = 5;
const MAX_LOG_SIZE = 100 * 1024; // 100KB

// 日志存储
let detailLogs = '';
let simpleLogs = '';
let autoScroll = true;

// 添加滚动事件监听
const logContent = document.getElementById('logContent');
if (logContent) {
    logContent.addEventListener('scroll', function () {
        // 检查是否滚动到底部（添加1px的容差）
        const isScrolledToBottom = logContent.scrollHeight - logContent.clientHeight <= logContent.scrollTop + 1;
        autoScroll = isScrolledToBottom;
    });
}

// WebSocket初始化函数
function initWebSocket() {
    try {
        ws = new WebSocket(buildWebSocketUrl('/ws/log'));

        ws.onopen = () => {
            console.log('WebSocket连接已建立');
            reconnectAttempts = 0;
        };

        ws.onmessage = (event) => {
            // console.log('收到WebSocket消息:', event.data);

            const logContent = document.getElementById('logContent');
            if (!logContent) {
                console.warn('找不到logContent元素');
                return;
            }

            let line = event.data.trim();
            if (!line) {
                console.log('收到空消息');
                return;
            }

            // 清理ANSI转义序列和日志前缀
            line = line.replace(/\u001b\[\d+(?:;\d+)*m/g, '')
                .replace(/\\u001b\[\d+(?:;\d+)*m/g, '')
                .replace(/\\n/g, '\n')
                .replace(/\\r/g, '\r')
                .replace(/^[EWID]\s*\(\d+\)\s*/gm, '')
                .trim();

            // 获取当前时间
            const now = new Date();
            const timeStr = `【${String(now.getMonth() + 1).padStart(2, '0')}-${String(now.getDate()).padStart(2, '0')} ${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}:${String(now.getSeconds()).padStart(2, '0')}】`;

            // 根据日志类型存储日志
            if (line.includes('[SIMPLE]')) {
                // 移除[SIMPLE]标记并处理格式
                line = line.replace('[SIMPLE]', '').trim();
                // 添加时间戳，去掉多余空格
                line = timeStr + line.replace(/【\s+/g, '【').replace(/\s+】/g, '】');

                // 处理错误日志的颜色
                if (line.toLowerCase().includes('error')) {
                    line = `<span style="color: #d9534f">${line}</span>`;
                }

                // 添加新日志并检查大小
                simpleLogs += line + '\n';
                if (simpleLogs.length > MAX_LOG_SIZE) {
                    const firstNewLine = simpleLogs.indexOf('\n', simpleLogs.length - MAX_LOG_SIZE);
                    if (firstNewLine !== -1) {
                        simpleLogs = simpleLogs.substring(firstNewLine + 1);
                    }
                }

                if (currentLogType === 'simple') {
                    logContent.innerHTML = simpleLogs;
                    if (autoScroll) {
                        logContent.scrollTop = logContent.scrollHeight;
                    }
                }
            } else {
                // 处理详细日志，去掉多余空格
                line = timeStr + (line.includes('【') ?
                    line.replace(/【\s+/g, '【').replace(/\s+】/g, '】') :
                    line);

                // 处理错误日志的颜色
                if (line.toLowerCase().includes('error')) {
                    line = `<span style="color: #d9534f">${line}</span>`;
                }

                // 添加新日志并检查大小
                detailLogs += line + '\n';
                if (detailLogs.length > MAX_LOG_SIZE) {
                    const firstNewLine = detailLogs.indexOf('\n', detailLogs.length - MAX_LOG_SIZE);
                    if (firstNewLine !== -1) {
                        detailLogs = detailLogs.substring(firstNewLine + 1);
                    }
                }

                if (currentLogType === 'detail') {
                    logContent.innerHTML = detailLogs;
                    if (autoScroll) {
                        logContent.scrollTop = logContent.scrollHeight;
                    }
                }
            }
        };

        ws.onclose = () => {
            console.log('WebSocket连接已关闭');
            if (reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
                setTimeout(() => {
                    reconnectAttempts++;
                    console.log(`尝试重新连接 (${reconnectAttempts}/${MAX_RECONNECT_ATTEMPTS})`);
                    initWebSocket();
                }, 3000);
            }
        };

        ws.onerror = (error) => {
            console.error('WebSocket错误:', error);
        };
    } catch (error) {
        console.error('初始化WebSocket时发生错误:', error);
        setTimeout(() => {
            if (reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
                reconnectAttempts++;
                console.log(`尝试重新连接 (${reconnectAttempts}/${MAX_RECONNECT_ATTEMPTS})`);
                initWebSocket();
            }
        }, 3000);
    }
}

// 切换日志类型的函数
function switchLogType(type) {
    currentLogType = type;
    const logContent = document.getElementById('logContent');

    if (type === 'detail') {
        detailLogTab.style.borderBottom = '2px solid #009ee1';
        detailLogTab.style.color = '#009ee1';
        simpleLogTab.style.borderBottom = 'none';
        simpleLogTab.style.color = '#333';
        logContent.innerHTML = detailLogs;
    } else {
        simpleLogTab.style.borderBottom = '2px solid #009ee1';
        simpleLogTab.style.color = '#009ee1';
        detailLogTab.style.borderBottom = 'none';
        detailLogTab.style.color = '#333';
        logContent.innerHTML = simpleLogs;
    }

    // 切换日志类型时保持当前滚动状态
    if (autoScroll) {
        logContent.scrollTop = logContent.scrollHeight;
    }
}

// 手动滚动到底部的函数
function scrollToBottom() {
    const logContent = document.getElementById('logContent');
    if (logContent) {
        logContent.scrollTop = logContent.scrollHeight;
        autoScroll = true;
    }
}

// 清空日志函数
async function clearLog() {
    try {
        const response = await fetch('/clear_log');
        if (response.ok) {
            if (currentLogType === 'detail') {
                detailLogs = '';
            } else {
                simpleLogs = '';
            }
            document.getElementById('logContent').innerHTML = '';
            displaySuccessMessage(translations[currentLang].clearLogSuccess);
        } else {
            throw new Error('Failed to clear log');
        }
    } catch (error) {
        console.error('Failed to clear log:', error);
        displayErrorMessage(translations[currentLang].clearLogFail);
    }
}

// 导出日志函数
async function exportLog() {
    try {
        const logContent = document.getElementById('logContent');
        if (!logContent || !logContent.innerHTML) {
            displayErrorMessage(translations[currentLang].noLog || '暂无日志');
            return;
        }

        const tempDiv = document.createElement('div');
        tempDiv.innerHTML = logContent.innerHTML;
        let logText = tempDiv.textContent;

        const blob = new Blob([logText], { type: 'text/plain' });
        const url = window.URL.createObjectURL(blob);
        const link = document.createElement('a');

        const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
        const filename = `WXNG1_${timestamp}${currentLogType === 'simple' ? '_simple' : ''}.log`;

        link.href = url;
        link.download = filename;
        document.body.appendChild(link);
        link.click();
        document.body.removeChild(link);
        window.URL.revokeObjectURL(url);

        displaySuccessMessage(translations[currentLang].exportLogSuccess || '导出日志成功');
    } catch (error) {
        console.error('导出日志失败:', error);
        displayErrorMessage(translations[currentLang].exportLogFail || '导出日志失败');
    }
}

// 日志开关切换函数
window.toggleLogSwitch = function (element) {
    element.classList.toggle('on');
    element.classList.toggle('off');
    const enabled = element.classList.contains('on');
    document.getElementById('logEnabledState').value = enabled ? '1' : '0';

    // 调用API保存状态
    saveLogSwitchState(enabled);
}

// 保存日志开关状态到后端
async function saveLogSwitchState(enabled) {
    try {
        const response = await fetch('/api/log_switch', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                log_enabled: enabled
            })
        });

        const data = await response.json();
        if (data.code === 200) {
            console.log('日志开关状态已保存:', enabled ? '开启' : '关闭');
            displaySuccessMessage(enabled ? '日志已开启' : '日志已关闭');
        } else {
            console.error('保存日志开关状态失败');
            displayErrorMessage('保存失败');
        }
    } catch (error) {
        console.error('保存日志开关状态出错:', error);
        displayErrorMessage('保存失败');
    }
}

// 加载日志开关状态
async function loadLogSwitchState() {
    try {
        const response = await fetch('/api/log_switch');
        const data = await response.json();

        if (data.code === 200) {
            const switchElement = document.getElementById('logEnableSwitch');
            const stateInput = document.getElementById('logEnabledState');

            if (switchElement && stateInput) {
                if (data.log_enabled) {
                    switchElement.classList.remove('off');
                    switchElement.classList.add('on');
                    stateInput.value = '1';
                } else {
                    switchElement.classList.remove('on');
                    switchElement.classList.add('off');
                    stateInput.value = '0';
                }
                console.log('日志开关状态已加载:', data.log_enabled ? '开启' : '关闭');
            }
        }
    } catch (error) {
        console.error('加载日志开关状态出错:', error);
    }
}

// 页面加载完成后初始化
document.addEventListener('DOMContentLoaded', function () {
    // 初始化WebSocket
    initWebSocket();

    // 初始化日志标签页样式
    if (detailLogTab && simpleLogTab) {
        detailLogTab.style.borderBottom = '2px solid #009ee1';
        detailLogTab.style.color = '#009ee1';
        simpleLogTab.style.borderBottom = 'none';
        simpleLogTab.style.color = '#333';
    }

    // 设置事件监听器
    detailLogTab?.addEventListener('click', () => switchLogType('detail'));
    simpleLogTab?.addEventListener('click', () => switchLogType('simple'));
    document.getElementById('clearLogButton')?.addEventListener('click', clearLog);
    document.getElementById('exportLogButton')?.addEventListener('click', exportLog);

    // 加载日志开关状态
    loadLogSwitchState();

    // 设置日志配置按钮事件
    const logConfig = document.getElementById('logConfig');
    const logConfig_m = document.getElementById('logConfig_m');

    [logConfig, logConfig_m].forEach(element => {
        element?.addEventListener('click', () => {
            if (!ws || ws.readyState !== WebSocket.OPEN) {
                initWebSocket();
            }
        });
    });

    // 监听视图切换
    const logView = document.getElementById('logView');
    if (logView) {
        const observer = new MutationObserver(mutations => {
            mutations.forEach(mutation => {
                if (mutation.target.style.display === 'none') {
                    if (ws) {
                        ws.close();
                        ws = null;
                    }
                }
            });
        });

        observer.observe(logView, {
            attributes: true,
            attributeFilter: ['style']
        });
    }
});

// System banner tree

window.onload = function () {
    // Password visibility toggle
    const passwordFields = [
        { input: 'lgpwd', openEye: 'eyeOpen', closedEye: 'eyeClosed' },
        { input: 'mqtt_password', openEye: 'mqtteyeOpen', closedEye: 'mqtteyeClosed' },
        { input: 'wifi_password', openEye: 'wifieyeOpen', closedEye: 'wifieyeClosed' }
    ];

    function togglePasswordVisibility(input, openEye, closedEye) {
        input.type = input.type === 'password' ? 'text' : 'password';
        openEye.style.display = input.type === 'password' ? 'none' : 'block';
        closedEye.style.display = input.type === 'password' ? 'block' : 'none';
    }

    passwordFields.forEach(field => {
        const input = document.getElementById(field.input);
        const openEye = document.getElementById(field.openEye);
        const closedEye = document.getElementById(field.closedEye);
        openEye.addEventListener('click', () => togglePasswordVisibility(input, openEye, closedEye));
        closedEye.addEventListener('click', () => togglePasswordVisibility(input, openEye, closedEye));
    });

    // Form validation
    var otaUrlInput = document.getElementById('ota_url');
    var otaSubmitButton = document.getElementById('otaSubmitButton');
    otaUrlInput.addEventListener('input', function () {
        if (otaUrlInput.value) {
            otaSubmitButton.disabled = false;
        } else {
            otaSubmitButton.disabled = true;
        }
    });
    var hostNamesInput = document.getElementById('host_names');
    var lgnameInput = document.getElementById('lgname');
    var lgpwdInput = document.getElementById('lgpwd');
    var moduleSetButton = document.getElementById('moduleSetButton');
    function checkInput() {
        if (hostNamesInput.value && lgnameInput.value && lgpwdInput.value) {
            moduleSetButton.disabled = false;
        } else {
            moduleSetButton.disabled = true;
        }
    }
    hostNamesInput.addEventListener('input', checkInput);
    lgnameInput.addEventListener('input', checkInput);
    lgpwdInput.addEventListener('input', checkInput);
    checkInput();
};

function toggleClass(elementId, ...classNames) {
    var element = document.getElementById(elementId);
    classNames.forEach(function (className) {
        element.classList.toggle(className);
    });
}

// Switch view
function switchView(viewId, linkId, mainClass, mainTitle, isMobile) {
    // 获取所有视图
    const views = document.querySelectorAll('[id$="View"]');
    const links = document.querySelectorAll('.nav-link');
    const main = document.getElementById('main');
    const mainTitleElement = document.getElementById('mainTitle');

    // 先将所有视图设置为不可见，并移除动画类
    views.forEach(view => {
        if (view.id !== viewId) {
            view.style.opacity = '0';
            setTimeout(() => {
                view.style.display = 'none';
                view.classList.remove('active');
            }, 300); // 等待淡出动画完成
        }
    });

    // 移除所有链接的active类
    links.forEach(link => link.classList.remove('active', 'router-link-active', 'router-link-exact-active'));

    // 获取目标视图和链接
    const targetView = document.getElementById(viewId);
    const targetLink = document.getElementById(linkId);

    if (!targetView || !targetLink || !main) return;

    // 更新链接状态和主要类
    targetLink.classList.add('active', 'router-link-active', 'router-link-exact-active');
    main.className = mainClass;

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

    // 显示目标视图并添加动画
    targetView.style.display = 'block';

    // 触发重排以确保动画正常运行
    void targetView.offsetWidth;

    // 添加active类以触发动画
    setTimeout(() => {
        targetView.style.opacity = '1';
        targetView.classList.add('active');
    }, 10);

    // 为视图内的所有卡片添加动画
    const cards = targetView.querySelectorAll('.card-view');
    cards.forEach((card, index) => {
        card.style.opacity = '0';
        card.style.transform = 'translateY(20px)';
        setTimeout(() => {
            card.style.opacity = '1';
            card.style.transform = 'translateY(0)';
            card.classList.add('show');
        }, 100 + index * 100); // 级联动画效果
    });
}

// 更新CSS样式
const style = document.createElement('style');
style.textContent = `
    [id$="View"] {
        transition: opacity 0.3s ease-out;
        opacity: 0;
    }

    [id$="View"].active {
        opacity: 1;
    }

    .card-view {
        transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
    }

    .nav-link {
        transition: all 0.3s ease;
    }

    .nav-link.active {
        color: var(--BRAND);
        background: var(--BG-1);
    }
`;
document.head.appendChild(style);

// 页面加载完成后初始化动画
document.addEventListener('DOMContentLoaded', () => {
    const currentView = document.querySelector('[id$="View"]:not([style*="display: none"])');
    if (currentView) {
        currentView.classList.add('active');
        currentView.style.opacity = '1';

        const cards = currentView.querySelectorAll('.card-view');
        cards.forEach((card, index) => {
            setTimeout(() => {
                card.style.opacity = '1';
                card.style.transform = 'translateY(0)';
                card.classList.add('show');
            }, index * 100);
        });
    }
});

function addEventListenerToElement(elementId, viewId, linkId, mainClass, mainTitle, isMobile) {
    document.getElementById(elementId).addEventListener("click", function () {
        switchView(viewId, linkId, mainClass, mainTitle, isMobile);
        if (isMobile) {
            setTimeout(function () {
                toggleClass("nav-top", "nav-off", "nav-on");
                toggleClass("app-icon", "icondaohang", "iconchahao");
                toggleClass("nav-lists", "nav-on");
                toggleClass("version-div", "nav-on");
            }, 100);
        }
    });
}

document.getElementById("app-icon-btn").addEventListener("click", function () {
    toggleClass("nav-top", "nav-off", "nav-on");
    toggleClass("app-icon", "icondaohang", "iconchahao");
    toggleClass("nav-lists", "nav-on");
    toggleClass("version-div", "nav-on");
});

function showSvg(svgId, duration) {
    var svg = document.getElementById(svgId);
    svg.style.display = 'flex';
    var clone = svg.cloneNode(true);
    svg.parentNode.replaceChild(clone, svg);
    svg = clone;
    setTimeout(function () {
        svg.style.display = 'none';
    }, duration);
}

addEventListenerToElement("basicInfo", 'infoView', 'basicInfo', 'base-info', '基本信息', false);
addEventListenerToElement("basicInfo_m", 'infoView', 'basicInfo', 'base-info', '基本信息', true);
addEventListenerToElement("serialConfig", 'serialView', 'serialConfig', 'serial', '串口管理', false);
addEventListenerToElement("serialConfig_m", 'serialView', 'serialConfig', 'serial', '串口管理', true);
addEventListenerToElement("sysConfig", 'sysView', 'sysConfig', 'security', '系统管理', false);
addEventListenerToElement("sysConfig_m", 'sysView', 'sysConfig', 'security', '系统管理', true);
addEventListenerToElement("protocolConfig", 'protocolView', 'protocolConfig', 'security', '协议管理', false);
addEventListenerToElement("protocolConfig_m", 'protocolView', 'protocolConfig', 'security', '协议管理', true);
addEventListenerToElement("netConfig", 'netView', 'netConfig', 'base-info', '网络管理', false);
addEventListenerToElement("netConfig_m", 'netView', 'netConfig', 'base-info', '网络管理', true);
addEventListenerToElement("logConfig", 'logView', 'logConfig', 'security', '日志管理', false);
addEventListenerToElement("logConfig_m", 'logView', 'logConfig', 'security', '日志管理', true);



document.getElementById("serialButton").addEventListener("click", function () {
    showSvg('successMessage', 1200);
});

document.getElementById("restartButton").addEventListener("click", function () {
    showSvg('deviceRestart', 6000);
});

document.getElementById("resetButton").addEventListener("click", function () {
    showSvg('deviceReset', 6000);
});

let currentLang = 'zh';

// i18n.js
const translations = {
    en: {
        basic_info: "Basic Information",
        sensor_info: "Sensor Information",
        device_name: "Device Name",
        current_time: "Current Time",
        net_mode: "Network Mode",
        eth_mac: "Ethernet MAC",
        eth_ip: "Ethernet IP Address",
        wifi_mac: "WiFi MAC",
        wifi_ip: "WiFi IP Address",
        ip_acquisition: "IP Acquisition Method",
        sensor_dashboard: "Sensor Dashboard",
        network_settings: "Network Settings",
        network_mode_selection: "Network Mode Selection",
        ethernet: "Ethernet",
        wifi: "WiFi",
        static_ip: "Static IP",
        wifi_name: "WiFi Name",
        wifi_password: "WiFi Password",
        scan_list: "Scan List",
        signal_strength: "Signal Strength",
        ip_address: "IP Address",
        subnet_mask: "Subnet Mask",
        gateway: "Gateway",
        search: "Search",
        submit: "Submit",
        mqttSettings: "MQTT Settings",
        mqttsubmit: "Submit",
        mqttenable: "Enable",
        mqttServerAddress: "Server Address",
        mqttServerPort: "Server Port",
        mqttUsername: "Username",
        mqttPassword: "Password",
        mqttClientID: "Client ID",
        subscribeTopic: "Subscribe Topic",
        publishTopic: "Publish Topic",
        qos: "QoS",
        retain: "Retain",
        timedReport: "Timed Report",
        mqttIntervalTime: "Interval Time",
        mqttConnectionStatus: "Connection Status",
        mqttnConnected: "Connected",
        tcpSettings: "TCP Settings",
        tcpSubmit: "Submit",
        tcpUse: "Enable",
        tcpProtocolType: "Protocol Type",
        tcpServer: "TCP Server",
        tcpClient: "TCP Client",
        modbusTcp: "ModbusTCPServer",
        modbusTcpClient: "ModbusTCPClient",
        tcpIntervalTime: "Interval Time",
        tcpAddress: "Server Address",
        tcpPort: "Server Port",
        tcpConn: "Connection Status",
        tcpnConn: "Connected",
        httpSettings: "HTTP/HTTPS Settings",
        requestType: "Request Type",
        http: "HTTP",
        https: "HTTPS",
        post: "POST",
        get: "GET",
        deviceTitle: "Serial Device Server",
        deviceBasicInfo: "Basic Information",
        deviceConfigurationDashbord: "Configuration Dashboard",
        deviceProtocolConfig: "Protocol Management",
        deviceWiFiConfig: "Network Management",
        deviceAdvancedSet: "Advanced Settings",
        deviceSysConfig: "System Management",
        lkWEB: "LK-WEB",
        ztList: "Configuration Dashboard",
        deviceHDvalue: "Hardware Parameters",
        sysVeren: "System Version",
        deviceFreeHeap: "Available Heap Memory",
        deviceMinHeap: "Minimum Available Heap Size",
        deviceRunTime: "System Uptime",
        deviceResReason: "Last Restart Reason",
        loginSet: "Login Settings",
        loginSubmit: "Set",
        deviceName: "Device Name",
        deviceUname: "Username",
        devicePwd: "Login Password",
        deviceOTA: "OTA Firmware Upgrade",
        otaSubmit: "Upgrade",
        otaURL: "OTA URL",
        deviceRestart: "Restart Device",
        isrestart: "Restart",
        deviceReset: "Reset Device",
        isreset: "Reset",
        isdreset: "Restart Device",
        isqreset: "Are you sure you want to restart?",
        dcancel: "Cancel",
        dreq: "Confirm",
        isdres: "Reset Device",
        isqres: "Are you sure you want to reset? The device will be reset after confirmation.",
        scancel: "Cancel",
        sreq: "Confirm",
        opsuccess: "Operation Successful",
        setopsuccess: "Settings Updated Successfully",
        valueCompensate: "Parameter Compensation",
        vcSubmit: "Set",
        httpConn: "HTTP/HTTPS Settings",
        httpSet: "Settings",
        httpIsUse: "Enable",
        httpWay: "Request Method",
        http_time: "Interval Time",
        second: "Second",
        thpsensor: 'Sensor',
        deviceLogConfig: "Log Management",
        logManage: "Log Management",
        clearLog: "Clear Log",
        noLog: "No logs available",
        clearLogSuccess: "Logs cleared successfully",
        clearLogFail: "Failed to clear logs"
    },
    zh: {
        basic_info: "设备状态",
        sensor_info: "传感器信息",
        device_name: "设备名称",
        current_time: "当前时间",
        net_mode: "网络模式",
        eth_mac: "以太网MAC",
        eth_ip: "以太网IP地址",
        wifi_mac: "WiFi MAC",
        wifi_ip: "WiFi IP地址",
        ip_acquisition: "IP获取方式",
        sensor_dashboard: "传感器仪表",
        network_settings: "网络设置",
        network_mode_selection: "网络模式选择",
        ethernet: "以太网",
        wifi: "WiFi",
        static_ip: "静态IP",
        wifi_name: "WIFI名称",
        wifi_password: "WIFI密码",
        scan_list: "扫描列表",
        signal_strength: "信号强度",
        ip_address: "IP地址",
        subnet_mask: "子网掩码",
        gateway: "网关",
        search: "搜索",
        submit: "设置",
        mqttSettings: "MQTT设置",
        mqttsubmit: "设置",
        mqttenable: "是否启用",
        mqttServerAddress: "服务器地址",
        mqttServerPort: "服务器端口",
        mqttUsername: "用户名",
        mqttPassword: "密码",
        mqttClientID: "ClientID",
        subscribeTopic: "订阅主题",
        publishTopic: "发布主题",
        qos: "QOS",
        retain: "保留(Retain)",
        timedReport: "定时上报",
        mqttIntervalTime: "间隔时间",
        mqttConnectionStatus: "连接状态",
        mqttnConnected: "未连接",
        tcpSettings: "TCP设置",
        tcpSubmit: "设置",
        tcpUse: "是否启用",
        tcpProtocolType: "协议类型",
        tcpServer: "TCPServer",
        tcpClient: "TCPClient",
        modbusTcp: "ModbusTCPServer",
        modbusTcpClient: "ModbusTCPClient",
        tcpIntervalTime: "间隔时间",
        tcpAddress: "服务器地址",
        tcpPort: "服务器端口",
        tcpConn: "连接状态",
        tcpnConn: "未连接",
        httpSettings: "HTTP/HTTPS设置",
        requestType: "请求方式",
        http: "HTTP",
        https: "HTTPS",
        post: "POST",
        get: "GET",
        deviceTitle: "物小能G1",
        deviceBasicInfo: "基本信息",
        deviceConfigurationDashbord: "组态仪表",
        deviceProtocolConfig: "协议管理",
        deviceWiFiConfig: "网络管理",
        deviceAdvancedSet: "高级设置",
        deviceSysConfig: "系统管理",
        lkWEB: "立控官网",
        ztList: "组态仪表",
        deviceHDvalue: "硬件参数",
        sysVeren: "系统版本",
        deviceFreeHeap: "系统可用堆内存",
        deviceMinHeap: "最小可用堆大小",
        deviceRunTime: "系统运行时间",
        deviceResReason: "上一次重启原因",
        loginSet: "登录设置",
        loginSubmit: "设置",
        deviceName: "设备名称",
        deviceUname: "用户名",
        devicePwd: "登录密码",
        deviceOTA: "OTA固件升级",
        otaSubmit: "升级",
        otaURL: "OTA地址",
        deviceRestart: "重启设备",
        isrestart: "重启",
        deviceReset: "恢复出厂参数",
        isreset: "重置",
        isdreset: "重启设备",
        isqreset: "是否确定重启？",
        dcancel: "取消",
        dreq: "确认",
        isdres: "恢复出厂参数",
        isqres: "是否确定重置？确认后设备将被重置",
        scancel: "取消",
        sreq: "确认",
        opsuccess: "操作成功",
        setopsuccess: "设置参数成功",
        valueCompensate: "参数补偿",
        vcSubmit: "设置",
        httpConn: "HTTP/HTTPS设置",
        httpSet: "设置",
        httpIsUse: "是否启用",
        httpWay: "请求方式",
        http_time: "间隔时间",
        second: "秒",
        thpsensor: "传感器",
        deviceLogConfig: "日志管理",
        logManage: "日志管理",
        clearLog: "清除日志",
        noLog: "暂无日志",
        clearLogSuccess: "清除日志成功",
        clearLogFail: "清除日志失败"
    }
};

// 导出配置功能
document.getElementById('exportModbusItem').addEventListener('click', function() {
    // 检查是否处于Modbus-RTU模式
    const workMode = document.querySelector('input[name="work_mode"]:checked').value;
    if (workMode !== 'modbus_rtu') {
        showCustomAlert('只有在Modbus-RTU模式下才能导出配置', true);
        return;
    }

    // 收集当前的Modbus配置
    const container = document.getElementById('modbus_items_container');
    const tableContainer = container.querySelector('.modbus-table-container');
    const tableContent = tableContainer.querySelector('.modbus-table-content');
    const rows = tableContent.querySelectorAll('.modbus-table-row:not(.modbus-table-header)');

    if (rows.length === 0) {
        showCustomAlert('没有可导出的配置项', true);
        return;
    }

    const modbusItems = [];
    // 获取轮询时间（秒）并转换为毫秒用于导出
    const pollTimeSeconds = parseInt(document.getElementById('poll_time').value) || 30;
    const pollTime = (pollTimeSeconds * 1000).toString();

    try {
        Array.from(rows).forEach((row, index) => {
            const itemIndex = index + 1;
            const registerAddrRaw = row.querySelector(`#register_addr_${itemIndex}`).value || '0';
            const registerAddrResult = normalizeNumericInput(registerAddrRaw, {
                min: 0,
                max: 65535,
                allowHex: true,
                label: `模板${itemIndex}的寄存器地址`
            });

            if (!registerAddrResult.valid) {
                throw new Error(registerAddrResult.message);
            }

            modbusItems.push({
                enabled: row.dataset.enabled === 'true',
                slave_addr: row.querySelector(`#slave_addr_${itemIndex}`).value || '1',
                function_code: row.querySelector(`#function_code_${itemIndex}`).value || '01',
                register_addr: registerAddrResult.string,
                register_num: row.querySelector(`#register_num_${itemIndex}`).value || '1',
                timeout: row.querySelector(`#timeout_${itemIndex}`).value || '1000',
                data_format: row.querySelector(`#data_format_${itemIndex}`).value || 'Signed',
                interval_time: row.querySelector(`#interval_time_${itemIndex}`).value || '100',
                report_format: row.querySelector(`#report_format_${itemIndex}`).value || 'mqtt',
                baud_rate: row.querySelector(`#baud_rate_${itemIndex}`).value || '9600',
                data_bit: row.querySelector(`#data_bit_${itemIndex}`).value || '8',
                check_bit: row.querySelector(`#check_bit_${itemIndex}`).value || 'None',
                stop_bit: row.querySelector(`#stop_bit_${itemIndex}`).value || '1'
            });
        });
    } catch (error) {
        console.error('导出配置失败:', error);
        showCustomAlert(error.message || '导出失败，请检查模板配置', true);
        return;
    }

    // 创建导出对象
    const exportConfig = {
        work_mode: 'modbus_rtu',
        poll_time: pollTime,
        modbus_items: modbusItems
    };

    // 转换为JSON并下载
    const jsonData = JSON.stringify(exportConfig, null, 2);
    const blob = new Blob([jsonData], { type: 'application/json' });
    const url = URL.createObjectURL(blob);

    // 创建下载链接
    const downloadLink = document.createElement('a');
    const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
    downloadLink.href = url;
    downloadLink.download = `modbus_rtu_config_${timestamp}.json`;
    document.body.appendChild(downloadLink);
    downloadLink.click();
    document.body.removeChild(downloadLink);
    URL.revokeObjectURL(url);

    displaySuccessMessage('配置导出成功');
});

// 导入配置功能
document.getElementById('importModbusItem').addEventListener('click', function() {
    // 检查是否处于Modbus-RTU模式
    const workMode = document.querySelector('input[name="work_mode"]:checked').value;
    if (workMode !== 'modbus_rtu') {
        showCustomAlert('只有在Modbus-RTU模式下才能导入配置', true);
        return;
    }

    // 创建文件输入元素
    const fileInput = document.createElement('input');
    fileInput.type = 'file';
    fileInput.accept = '.json';
    fileInput.style.display = 'none';
    document.body.appendChild(fileInput);

    fileInput.onchange = function(event) {
        const file = event.target.files[0];
        if (!file) {
            document.body.removeChild(fileInput);
            return;
        }

        const reader = new FileReader();
        reader.onload = function(e) {
            try {
                const importConfig = JSON.parse(e.target.result);

                // 验证导入的JSON格式是否正确
                if (!importConfig.work_mode || importConfig.work_mode !== 'modbus_rtu' || !Array.isArray(importConfig.modbus_items)) {
                    throw new Error('无效的配置文件格式，必须包含work_mode和modbus_items数组');
                }

                // 校验poll_time (假设导入的配置文件中轮询时间是毫秒单位)
                if (importConfig.poll_time) {
                    const pollTimeMs = parseInt(importConfig.poll_time);
                    if (isNaN(pollTimeMs) || pollTimeMs <= 0) {
                        throw new Error('轮询时间格式无效，必须是正整数（毫秒）');
                    }
                    // 转换为秒用于界面显示
                    const pollTimeSeconds = Math.round(pollTimeMs / 1000);
                    if (pollTimeSeconds < 1) {
                        throw new Error('轮询时间太小，最少为1秒');
                    }
                }

                // 验证每个modbus项目的字段格式
                const modbusItems = importConfig.modbus_items;
                modbusItems.forEach((item, index) => {
                    // 验证必填字段是否存在
                    const requiredFields = ['slave_addr', 'function_code', 'register_addr', 'register_num', 'timeout', 'interval_time'];
                    for (const field of requiredFields) {
                        if (item[field] === undefined || item[field] === '') {
                            throw new Error(`配置项 #${index+1} 缺少必填字段: ${field}`);
                        }
                    }

                    // 验证slave_addr (设备地址)是否为有效的十进制数字
                    if (!/^\d+$/.test(item.slave_addr) || parseInt(item.slave_addr) < 1 || parseInt(item.slave_addr) > 255) {
                        throw new Error(`配置项 #${index+1} 的设备地址无效，必须是1-255之间的十进制数字`);
                    }

                    // 验证function_code (功能码)是否为有效的值
                    const validFunctionCodes = ['01', '02', '03', '04'];
                    if (!validFunctionCodes.includes(item.function_code)) {
                        throw new Error(`配置项 #${index+1} 的功能码无效，有效值为: ${validFunctionCodes.join(', ')}`);
                    }

                    // 验证并归一化寄存器地址（支持十进制与0x前缀HEX）
                    const registerAddrResult = normalizeNumericInput(item.register_addr ?? '0', {
                        min: 0,
                        max: 65535,
                        allowHex: true,
                        label: `配置项 #${index+1} 的寄存器地址`
                    });
                    if (!registerAddrResult.valid) {
                        throw new Error(registerAddrResult.message);
                    }
                    item.register_addr = registerAddrResult.string;

                    // 验证register_num (寄存器数量)是否为有效的十进制数字
                    if (!/^\d+$/.test(item.register_num) || parseInt(item.register_num) < 1) {
                        throw new Error(`配置项 #${index+1} 的寄存器数量无效，必须是大于0的十进制数字`);
                    }

                    // 验证timeout (接收超时)是否为有效的数字
                    if (!/^\d+$/.test(item.timeout) || parseInt(item.timeout) <= 0) {
                        throw new Error(`配置项 #${index+1} 的接收超时无效，必须是大于0的数字`);
                    }

                    // 验证interval_time (间隔时间)是否为有效的数字
                    if (!/^\d+$/.test(item.interval_time) || parseInt(item.interval_time) <= 0) {
                        throw new Error(`配置项 #${index+1} 的间隔时间无效，必须是大于0的数字`);
                    }

                    // 验证data_format (数据格式)是否为有效的值
                    const validDataFormats = ['Signed', 'Unsigned', 'HEX', 'Binary', 'Long', 'Float', 'Double', 'LongInverse', 'FloatInverse', 'DoubleInverse'];
                    if (item.data_format && !validDataFormats.includes(item.data_format)) {
                        throw new Error(`配置项 #${index+1} 的数据格式无效，有效值为: ${validDataFormats.join(', ')}`);
                    }

                    // 验证report_format (上报方式)是否为有效的值
                    const validReportFormats = ['mqtt', 'tcp', 'http'];
                    if (item.report_format && !validReportFormats.includes(item.report_format)) {
                        throw new Error(`配置项 #${index+1} 的上报方式无效，有效值为: ${validReportFormats.join(', ')}`);
                    }

                    // 验证baud_rate (波特率)是否为有效的值
                    const validBaudRates = ['1200', '2400', '4800', '9600', '19200', '38400', '57600', '115200'];
                    if (item.baud_rate && !validBaudRates.includes(item.baud_rate)) {
                        throw new Error(`配置项 #${index+1} 的波特率无效，有效值为: ${validBaudRates.join(', ')}`);
                    }

                    // 验证data_bit (数据位)是否为有效的值
                    const validDataBits = ['5', '6', '7', '8'];
                    if (item.data_bit && !validDataBits.includes(item.data_bit)) {
                        throw new Error(`配置项 #${index+1} 的数据位无效，有效值为: ${validDataBits.join(', ')}`);
                    }

                    // 验证check_bit (校验位)是否为有效的值
                    const validCheckBits = ['None', 'Odd', 'Even'];
                    if (item.check_bit && !validCheckBits.includes(item.check_bit)) {
                        throw new Error(`配置项 #${index+1} 的校验位无效，有效值为: ${validCheckBits.join(', ')}`);
                    }

                    // 验证stop_bit (停止位)是否为有效的值
                    const validStopBits = ['1', '1.5', '2'];
                    if (item.stop_bit && !validStopBits.includes(item.stop_bit)) {
                        throw new Error(`配置项 #${index+1} 的停止位无效，有效值为: ${validStopBits.join(', ')}`);
                    }
                });

                if (modbusItems.length > MODBUS_ITEM_MAX) {
                    showCustomAlert(`配置项不能超过${MODBUS_ITEM_MAX}个，将只导入前${MODBUS_ITEM_MAX}个`);
                    modbusItems.length = MODBUS_ITEM_MAX;
                }

                // 设置轮询时间（从毫秒转换为秒显示）
                if (importConfig.poll_time) {
                    const pollTimeMs = parseInt(importConfig.poll_time);
                    const pollTimeSeconds = Math.round(pollTimeMs / 1000);
                    document.getElementById('poll_time').value = pollTimeSeconds;
                }

                // 清空现有内容
                const container = document.getElementById('modbus_items_container');
                const tableContainer = container.querySelector('.modbus-table-container');
                const tableContent = tableContainer.querySelector('.modbus-table-content');
                const header = tableContent.querySelector('.modbus-table-header');

                // 保留表头，删除所有行
                while (tableContent.firstChild) {
                    tableContent.removeChild(tableContent.firstChild);
                }
                tableContent.appendChild(createModbusTableHeader());

                // 添加导入的配置项
                modbusItems.forEach((item, index) => {
                    const row = createModbusItemTemplate(index + 1);
                    const switchBtn = row.querySelector('.switch-btn');
                    const inputs = row.querySelectorAll('.base-input');

                    // 设置各个字段的值
                    if (item.slave_addr) row.querySelector(`#slave_addr_${index + 1}`).value = item.slave_addr;
                    if (item.function_code) row.querySelector(`#function_code_${index + 1}`).value = item.function_code;
                    if (item.register_addr) {
                        row.querySelector(`#register_addr_${index + 1}`).value = item.register_addr;
                    }
                    if (item.register_num) row.querySelector(`#register_num_${index + 1}`).value = item.register_num;
                    if (item.timeout) row.querySelector(`#timeout_${index + 1}`).value = item.timeout;
                    if (item.data_format) row.querySelector(`#data_format_${index + 1}`).value = item.data_format;
                    if (item.interval_time) row.querySelector(`#interval_time_${index + 1}`).value = item.interval_time;
                    if (item.report_format) row.querySelector(`#report_format_${index + 1}`).value = item.report_format;
                    if (item.baud_rate) row.querySelector(`#baud_rate_${index + 1}`).value = item.baud_rate;
                    if (item.data_bit) row.querySelector(`#data_bit_${index + 1}`).value = item.data_bit;
                    if (item.check_bit) row.querySelector(`#check_bit_${index + 1}`).value = item.check_bit;
                    if (item.stop_bit) row.querySelector(`#stop_bit_${index + 1}`).value = item.stop_bit;

                    // 设置启用状态
                    if (item.enabled === false) {
                        switchBtn.classList.remove('on');
                        switchBtn.classList.add('off');
                        inputs.forEach(input => input.disabled = true);
                        row.dataset.enabled = 'false';
                    } else {
                        switchBtn.classList.remove('off');
                        switchBtn.classList.add('on');
                        inputs.forEach(input => input.disabled = false);
                        row.dataset.enabled = 'true';
                    }

                    tableContent.appendChild(row);
                });

                // 如果没有配置项，添加一个默认行
                if (modbusItems.length === 0) {
                    const row = createModbusItemTemplate(1);
                    tableContent.appendChild(row);
                }

                // 更新内容区域高度
                const workModeContent = document.getElementById('workModeContent');
                if (workModeContent && !workModeContent.classList.contains('collapsed')) {
                    workModeContent.style.maxHeight = workModeContent.scrollHeight + 'px';
                }

                displaySuccessMessage('配置导入成功');
            } catch (error) {
                console.error('配置导入失败:', error);
                showCustomAlert('配置导入失败: ' + error.message, true);
            }
            document.body.removeChild(fileInput);
        };
        reader.onerror = function() {
            showCustomAlert('读取文件失败', true);
            document.body.removeChild(fileInput);
        };
        reader.readAsText(file);
    };

    fileInput.click();
});

// 添加自定义提示框函数
// 创建美观的确认对话框
function showCustomConfirm(message, onConfirm, onCancel) {
    // 创建遮罩层
    const overlay = document.createElement('div');
    overlay.style.position = 'fixed';
    overlay.style.top = '0';
    overlay.style.left = '0';
    overlay.style.width = '100%';
    overlay.style.height = '100%';
    overlay.style.backgroundColor = 'rgba(0, 0, 0, 0.5)';
    overlay.style.zIndex = '10000';
    overlay.style.display = 'flex';
    overlay.style.justifyContent = 'center';
    overlay.style.alignItems = 'center';

    // 创建对话框容器
    const confirmBox = document.createElement('div');
    confirmBox.style.backgroundColor = '#fff';
    confirmBox.style.borderRadius = '12px';
    confirmBox.style.boxShadow = '0 8px 32px rgba(0, 0, 0, 0.3)';
    confirmBox.style.padding = '0';
    confirmBox.style.minWidth = '400px';
    confirmBox.style.maxWidth = '500px';
    confirmBox.style.fontFamily = '-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif';
    confirmBox.style.overflow = 'hidden';
    confirmBox.style.transform = 'scale(0.9)';
    confirmBox.style.transition = 'all 0.3s ease';

    // 创建头部
    const header = document.createElement('div');
    header.style.padding = '24px 24px 16px 24px';
    header.style.borderBottom = '1px solid #e5e5e5';

    const title = document.createElement('h3');
    title.textContent = '轮询时间调整';
    title.style.margin = '0';
    title.style.fontSize = '18px';
    title.style.fontWeight = '600';
    title.style.color = '#333';
    title.style.display = 'flex';
    title.style.alignItems = 'center';

    const icon = document.createElement('span');
    icon.innerHTML = '⚠️';
    icon.style.marginRight = '8px';
    icon.style.fontSize = '20px';

    title.prepend(icon);
    header.appendChild(title);

    // 创建内容区域
    const content = document.createElement('div');
    content.style.padding = '20px 24px';
    content.style.lineHeight = '1.6';
    content.style.color = '#555';
    content.style.fontSize = '14px';
    content.innerHTML = message;

    // 创建按钮区域
    const buttonArea = document.createElement('div');
    buttonArea.style.padding = '16px 24px 24px 24px';
    buttonArea.style.display = 'flex';
    buttonArea.style.gap = '12px';
    buttonArea.style.justifyContent = 'flex-end';

    // 创建取消按钮
    const cancelButton = document.createElement('button');
    cancelButton.textContent = '取消';
    cancelButton.style.backgroundColor = '#f5f5f5';
    cancelButton.style.color = '#666';
    cancelButton.style.border = '1px solid #ddd';
    cancelButton.style.borderRadius = '8px';
    cancelButton.style.padding = '10px 20px';
    cancelButton.style.fontSize = '14px';
    cancelButton.style.cursor = 'pointer';
    cancelButton.style.transition = 'all 0.2s ease';
    cancelButton.style.fontWeight = '500';

    cancelButton.onmouseover = function() {
        this.style.backgroundColor = '#e8e8e8';
        this.style.borderColor = '#ccc';
    };
    cancelButton.onmouseout = function() {
        this.style.backgroundColor = '#f5f5f5';
        this.style.borderColor = '#ddd';
    };

    // 创建确认按钮
    const confirmButton = document.createElement('button');
    confirmButton.textContent = '自动调整';
    confirmButton.style.backgroundColor = '#007bff';
    confirmButton.style.color = '#fff';
    confirmButton.style.border = 'none';
    confirmButton.style.borderRadius = '8px';
    confirmButton.style.padding = '10px 20px';
    confirmButton.style.fontSize = '14px';
    confirmButton.style.cursor = 'pointer';
    confirmButton.style.transition = 'all 0.2s ease';
    confirmButton.style.fontWeight = '500';

    confirmButton.onmouseover = function() {
        this.style.backgroundColor = '#0056b3';
        this.style.transform = 'translateY(-1px)';
    };
    confirmButton.onmouseout = function() {
        this.style.backgroundColor = '#007bff';
        this.style.transform = 'translateY(0)';
    };

    // 关闭对话框函数
    function closeDialog() {
        confirmBox.style.transform = 'scale(0.9)';
        overlay.style.opacity = '0';
        setTimeout(() => {
            document.body.removeChild(overlay);
        }, 300);
    }

    // 绑定事件
    cancelButton.onclick = function() {
        closeDialog();
        if (onCancel) onCancel();
    };

    confirmButton.onclick = function() {
        closeDialog();
        if (onConfirm) onConfirm();
    };

    // 按ESC关闭
    const handleEscape = function(e) {
        if (e.key === 'Escape') {
            closeDialog();
            if (onCancel) onCancel();
            document.removeEventListener('keydown', handleEscape);
        }
    };
    document.addEventListener('keydown', handleEscape);

    // 组装对话框
    buttonArea.appendChild(cancelButton);
    buttonArea.appendChild(confirmButton);
    confirmBox.appendChild(header);
    confirmBox.appendChild(content);
    confirmBox.appendChild(buttonArea);
    overlay.appendChild(confirmBox);

    // 添加到页面
    document.body.appendChild(overlay);

    // 显示动画
    setTimeout(() => {
        overlay.style.opacity = '1';
        confirmBox.style.transform = 'scale(1)';
    }, 10);

    // 自动聚焦确认按钮
    setTimeout(() => {
        confirmButton.focus();
    }, 300);
}

function showCustomAlert(message, isError = false) {
    // 如果已有提示框，先移除所有相关元素
    const existingAlerts = document.querySelectorAll('#customAlertBox');
    const existingOverlays = document.querySelectorAll('.custom-alert-overlay');
    
    existingAlerts.forEach(alert => {
        if (alert.parentNode) {
            document.body.removeChild(alert);
        }
    });
    
    existingOverlays.forEach(overlay => {
        if (overlay.parentNode) {
            document.body.removeChild(overlay);
        }
    });
    
    // 等待DOM清理完成
    setTimeout(() => createAlert(), 10);
    
    function createAlert() {
        // 创建提示框容器
        const alertBox = document.createElement('div');
        alertBox.id = 'customAlertBox';
        alertBox.style.position = 'fixed';
        alertBox.style.top = '50%';
        alertBox.style.left = '50%';
        alertBox.style.transform = 'translate(-50%, -50%)';
        alertBox.style.zIndex = '10001'; // 提高z-index确保在最顶层
        alertBox.style.backgroundColor = '#fff';
        alertBox.style.borderRadius = '5px';
        alertBox.style.boxShadow = '0 0 20px rgba(0, 0, 0, 0.3)';
        alertBox.style.padding = '20px 25px';
        alertBox.style.minWidth = '350px';
        alertBox.style.maxWidth = '80%';
        alertBox.style.textAlign = 'center';
        alertBox.style.border = isError ? '1px solid #d9534f' : '1px solid #2b6ec0';
        alertBox.style.fontFamily = '-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif';

        // 添加标题
        const title = document.createElement('div');
        title.style.fontWeight = 'bold';
        title.style.fontSize = '18px';
        title.style.marginBottom = '15px';
        title.style.color = isError ? '#d9534f' : '#2b6ec0';
        title.textContent = isError ? '错误提示' : '温馨提示';
        alertBox.appendChild(title);

        // 添加图标
        const icon = document.createElement('div');
        icon.style.fontSize = '24px';
        icon.style.marginBottom = '10px';
        icon.innerHTML = isError ? '&#9888;' : '&#8505;'; // 警告图标或信息图标
        icon.style.color = isError ? '#d9534f' : '#2b6ec0';
        title.insertBefore(icon, title.firstChild);

        // 添加分隔线
        const divider = document.createElement('div');
        divider.style.height = '1px';
        divider.style.backgroundColor = isError ? '#d9534f' : '#2b6ec0';
        divider.style.opacity = '0.3';
        divider.style.margin = '0 -25px 15px -25px';
        alertBox.appendChild(divider);

        // 添加消息内容
        const content = document.createElement('div');
        content.style.marginBottom = '20px';
        content.style.color = '#333';
        content.style.fontSize = '15px';
        content.style.lineHeight = '1.5';
        const normalizedMessage = (message || '').toString().replace(/<br\s*\/?>/gi, '\n');
        const safeMessage = normalizedMessage
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/\n/g, '<br>');
        content.innerHTML = safeMessage;
        alertBox.appendChild(content);

        // 添加确定按钮
        const confirmButton = document.createElement('button');
        confirmButton.textContent = '确定';
        confirmButton.style.backgroundColor = isError ? '#d9534f' : '#2b6ec0';
        confirmButton.style.color = '#fff';
        confirmButton.style.border = 'none';
        confirmButton.style.borderRadius = '5px';
        confirmButton.style.padding = '10px 20px';
        confirmButton.style.fontSize = '14px';
        confirmButton.style.cursor = 'pointer';
        confirmButton.style.transition = 'all 0.2s ease';
        confirmButton.style.boxShadow = '0 2px 5px rgba(0,0,0,0.2)';

        // 添加鼠标悬停效果
        confirmButton.onmouseover = function() {
            this.style.backgroundColor = isError ? '#c9302c' : '#1a5aa0';
            this.style.boxShadow = '0 4px 8px rgba(0,0,0,0.2)';
        };
        confirmButton.onmouseout = function() {
            this.style.backgroundColor = isError ? '#d9534f' : '#2b6ec0';
            this.style.boxShadow = '0 2px 5px rgba(0,0,0,0.2)';
        };

        confirmButton.onclick = function() {
            // 淡出效果
            alertBox.style.opacity = '0';
            overlay.style.opacity = '0';
            setTimeout(() => {
                document.body.removeChild(alertBox);
                document.body.removeChild(overlay);
            }, 300);
        };
        alertBox.appendChild(confirmButton);

        // 添加遮罩层
        const overlay = document.createElement('div');
        overlay.className = 'custom-alert-overlay';
        overlay.style.position = 'fixed';
        overlay.style.top = '0';
        overlay.style.left = '0';
        overlay.style.width = '100%';
        overlay.style.height = '100%';
        overlay.style.backgroundColor = 'rgba(0, 0, 0, 0.5)';
        overlay.style.zIndex = '10000'; // 保持与confirmButton一致

        // 添加到文档中
        document.body.appendChild(overlay);
        document.body.appendChild(alertBox);

        // 设置淡入效果
        alertBox.style.opacity = '0';
        overlay.style.opacity = '0';
        setTimeout(() => {
            alertBox.style.transition = 'opacity 0.3s ease';
            overlay.style.transition = 'opacity 0.3s ease';
            alertBox.style.opacity = '1';
            overlay.style.opacity = '1';
        }, 10);

        // 点击遮罩层关闭提示框
        overlay.onclick = function() {
            alertBox.style.opacity = '0';
            overlay.style.opacity = '0';
            setTimeout(() => {
                document.body.removeChild(alertBox);
                document.body.removeChild(overlay);
            }, 300);
        };

        // 添加键盘事件监听，按ESC键关闭
        const escKeyHandler = function(e) {
            if (e.key === 'Escape') {
                confirmButton.click();
                document.removeEventListener('keydown', escKeyHandler);
            }
        };
        document.addEventListener('keydown', escKeyHandler);

        // 返回overlay和alertBox，方便后续操作
        return {
            overlay,
            alertBox,
            close: function() {
                document.body.removeChild(alertBox);
                document.body.removeChild(overlay);
                document.removeEventListener('keydown', escKeyHandler);
            }
        };
    } // createAlert函数的结尾
}

// 检查Modbus-TCP转Modbus-RTU模式下的TCP配置状态
async function checkTCPConfigurationForModbusTCP() {
    try {
        // 获取TCP配置信息
        const tcpInfo = await fetchData('/tcp_info').catch(() => null);
        
        if (!tcpInfo) {
            return {
                valid: false,
                message: 'TCP配置信息获取失败，请稍后重试'
            };
        }
        
        // 检查TCP是否启用
        if (!tcpInfo.use_tcp || tcpInfo.use_tcp !== '1') {
            return {
                valid: false,
                message: 'Modbus-TCP转Modbus-RTU模式需要启用TCP功能，请先在协议管理中启用TCP设置'
            };
        }
        
        // 检查TCP是否设置为Server/ModbusServer/ModbusClient模式
        // tcpconn: "0"=TCPServer, "1"=TCPClient, "2"=ModbusTCPServer, "3"=ModbusTCPClient
        if (tcpInfo.tcpconn !== "0" && tcpInfo.tcpconn !== "2" && tcpInfo.tcpconn !== "3") {
            return {
                valid: false,
                message: 'Modbus-TCP转Modbus-RTU模式要求TCP设置为TCPServer、ModbusTCPServer或ModbusTCPClient，请在协议管理中调整协议类型'
            };
        }
        
        return {
            valid: true,
            message: 'TCP配置检查通过'
        };
    } catch (error) {
        console.error('TCP配置检查失败:', error);
        return {
            valid: false,
            message: 'TCP配置检查失败，请稍后重试'
        };
    }
}

// 检查Modbus-RTU模式下具体使用的协议配置状态
async function checkProtocolConfigurationForModbusRTU(modbusItemsSnapshot) {
    try {
        let templateItems = [];
        if (Array.isArray(modbusItemsSnapshot)) {
            templateItems = modbusItemsSnapshot;
        } else {
            const container = document.getElementById('modbus_items_container');
            const tableContainer = container ? container.querySelector('.modbus-table-container') : null;
            const tableContent = tableContainer ? tableContainer.querySelector('.modbus-table-content') : null;
            const rows = tableContent ? tableContent.querySelectorAll('.modbus-table-row:not(.modbus-table-header)') : [];
            templateItems = Array.from(rows).map((row, index) => {
                const itemIndex = index + 1;
                const reportSelect = row.querySelector(`#report_format_${itemIndex}`);
                return {
                    enabled: row.dataset.enabled !== 'false',
                    report_format: reportSelect ? reportSelect.value : 'mqtt'
                };
            });
        }

        const protocolMeta = {
            MQTT: {
                code: 'MQTT',
                name: 'MQTT协议',
                tip: '请在协议管理 > MQTT设置 中启用，并完善服务器地址、Topic等参数。'
            },
            TCP: {
                code: 'TCP',
                name: 'TCP协议',
                tip: '请在协议管理 > TCP设置 中启用，并确认工作在 TCP Server 模式。'
            },
            HTTP: {
                code: 'HTTP',
                name: 'HTTP协议',
                tip: '请在协议管理 > HTTP设置 中启用，并配置目标 HTTP URI。'
            }
        };
        
        // 收集使用的上报方式
        const usedProtocols = new Set();
        templateItems.forEach((item) => {
            const isEnabled = item && item.enabled !== false;
            if (!isEnabled) {
                return;
            }
            const formatValue = (item.report_format || '').toString().trim().toUpperCase();
            if (formatValue && protocolMeta[formatValue]) {
                usedProtocols.add(formatValue);
            }
        });
        
        if (usedProtocols.size === 0) {
            return {
                valid: true,
                message: '无需协议检查'
            };
        }

        // 并行获取协议配置状态
        const [mqttInfo, tcpInfo, httpInfo] = await Promise.all([
            fetchData('/mqtt_info').catch(() => null),
            fetchData('/tcp_info').catch(() => null),
            fetchData('/http_info').catch(() => null)
        ]);
        
        const protocolStatus = {
            MQTT: !!(mqttInfo && mqttInfo.use_mqtt === '1'),
            TCP: !!(tcpInfo && tcpInfo.use_tcp === '1'),
            HTTP: !!(httpInfo && httpInfo.use_http === '1')
        };
        
        // 检查使用的协议是否已配置
        const unconfiguredProtocols = [];
        const unconfiguredDetails = [];
        
        const markMissing = (code) => {
            if (unconfiguredProtocols.includes(code)) {
                return;
            }
            unconfiguredProtocols.push(code);
            const meta = protocolMeta[code] || { code, name: `${code}协议`, tip: '请在协议管理界面启用并配置该协议。' };
            unconfiguredDetails.push(meta);
        };
        
        usedProtocols.forEach(code => {
            if (!protocolStatus[code]) {
                markMissing(code);
            }
        });
        
        if (unconfiguredProtocols.length > 0) {
            const detailLines = unconfiguredDetails.map(detail => {
                const tipText = detail.tip ? `（${detail.tip}）` : '';
                return `• ${detail.name}${tipText}`;
            });
            const message = `Modbus-RTU网关模式所选模板需要以下协议已启用：\n${detailLines.join('\n')}`;
            return {
                valid: false,
                message: message,
                unconfiguredProtocols: unconfiguredProtocols,
                unconfiguredDetails: unconfiguredDetails
            };
        }
        
        return {
            valid: true,
            message: '协议配置检查通过'
        };
    } catch (error) {
        console.error('协议配置检查失败:', error);
        return {
            valid: false,
            message: '协议配置检查失败，请稍后重试'
        };
    }
}

// 显示多协议选择对话框（支持用户勾选要启用的协议）
function showProtocolSelectionDialog(options) {
    return new Promise((resolve) => {
        const { title, message, protocols, onConfirm, onCancel } = options;
        
        // 创建遮罩层
        const overlay = document.createElement('div');
        overlay.style.cssText = `
            position: fixed; top: 0; left: 0; width: 100%; height: 100%;
            background-color: rgba(136, 136, 136, 0.3); z-index: 9999999;
        `;
        
        // 创建对话框容器
        const dialog = document.createElement('div');
        dialog.style.cssText = `
            position: fixed; top: 50%; left: 50%; transform: translate(-50%, -50%);
            width: 90%; max-width: 450px; background: var(--BG-1);
            box-shadow: 0 20px 60px 0 rgba(0, 0, 0, 0.1);
            border-radius: var(--RADIUS-0); z-index: 99999999;
            padding: 32px;
        `;
        
        // 标题
        const titleElem = document.createElement('h3');
        titleElem.textContent = title;
        titleElem.style.cssText = `
            margin: 0 0 16px 0; font-size: 18px; line-height: 26px;
            color: var(--FG-0); text-align: center;
        `;
        dialog.appendChild(titleElem);
        
        // 消息内容
        const messageElem = document.createElement('p');
        messageElem.innerHTML = message.replace(/\n/g, '<br>');
        messageElem.style.cssText = `
            margin: 0 0 20px 0; font-size: 14px; line-height: 20px;
            color: var(--FG-2);
        `;
        dialog.appendChild(messageElem);
        
        // 协议选择区域
        const protocolsContainer = document.createElement('div');
        protocolsContainer.style.cssText = `
            background: var(--BG-AREA); border-radius: var(--RADIUS-1);
            padding: 16px; margin-bottom: 20px;
        `;
        
        const protocolsTitle = document.createElement('div');
        protocolsTitle.textContent = '请选择要启用的协议：';
        protocolsTitle.style.cssText = `
            font-size: 14px; color: var(--FG-1); margin-bottom: 12px;
            font-weight: 500;
        `;
        protocolsContainer.appendChild(protocolsTitle);
        
        // 创建复选框
        const checkboxes = {};
        protocols.forEach(protocol => {
            const checkboxDiv = document.createElement('div');
            checkboxDiv.style.cssText = `
                display: flex; align-items: center; margin-bottom: 12px;
                padding: 10px; border-radius: 4px;
                background: ${protocol.required ? 'rgba(43, 110, 192, 0.05)' : 'transparent'};
                border: ${protocol.required ? '1px solid rgba(43, 110, 192, 0.2)' : '1px solid transparent'};
            `;
            
            const checkbox = document.createElement('input');
            checkbox.type = 'checkbox';
            checkbox.id = `protocol_${protocol.name}`;
            checkbox.checked = true; // 默认全选
            checkbox.disabled = protocol.required === true; // 必选项禁用复选框（强制选中）
            checkbox.style.cssText = `margin-right: 10px;`;
            checkboxes[protocol.name] = checkbox;
            
            const label = document.createElement('label');
            label.htmlFor = checkbox.id;
            
            // 构建标签内容
            let labelContent = `<strong>${protocol.name}</strong>`;
            if (protocol.required) {
                labelContent += ` <span style="color: #e74c3c; font-size: 12px;">（必选）</span>`;
            } else {
                labelContent += ` <span style="color: #95a5a6; font-size: 12px;">（可选）</span>`;
            }
            labelContent += `<br><span style="font-size: 12px; color: var(--FG-3);">${protocol.description}</span>`;
            
            label.innerHTML = labelContent;
            label.style.cssText = `
                font-size: 13px; color: var(--FG-1); cursor: pointer; flex: 1;
            `;
            
            checkboxDiv.appendChild(checkbox);
            checkboxDiv.appendChild(label);
            protocolsContainer.appendChild(checkboxDiv);
        });
        
        dialog.appendChild(protocolsContainer);
        
        // 按钮容器
        const buttonContainer = document.createElement('div');
        buttonContainer.style.cssText = `
            display: flex; gap: 10px; justify-content: flex-end;
        `;
        
        // 取消按钮
        const cancelBtn = document.createElement('button');
        cancelBtn.textContent = '取消';
        cancelBtn.className = 'normal-btn';
        cancelBtn.onclick = () => {
            document.body.removeChild(dialog);
            document.body.removeChild(overlay);
            if (onCancel) onCancel();
            resolve(null);
        };
        buttonContainer.appendChild(cancelBtn);
        
        // 确认按钮
        const confirmBtn = document.createElement('button');
        confirmBtn.textContent = '启用选中的协议';
        confirmBtn.className = 'main-btn';
        confirmBtn.onclick = () => {
            // 获取选中的协议
            const selectedProtocols = [];
            protocols.forEach(protocol => {
                const checkbox = checkboxes[protocol.name];
                if (checkbox && checkbox.checked) {
                    selectedProtocols.push(protocol.name);
                }
            });
            
            // 检查必选协议是否被选中
            const requiredProtocols = protocols.filter(p => p.required).map(p => p.name);
            const missingRequired = requiredProtocols.filter(name => !selectedProtocols.includes(name));
            
            if (missingRequired.length > 0) {
                alert(`请选择必选协议：${missingRequired.join('、')}`);
                return;
            }
            
            if (selectedProtocols.length === 0 && !options.allowEmpty) {
                alert('请至少选择一个协议');
                return;
            }
            
            document.body.removeChild(dialog);
            document.body.removeChild(overlay);
            if (onConfirm) onConfirm(selectedProtocols);
            resolve(selectedProtocols);
        };
        buttonContainer.appendChild(confirmBtn);
        
        dialog.appendChild(buttonContainer);
        
        // 添加到页面
        document.body.appendChild(overlay);
        document.body.appendChild(dialog);
        
        // ESC键关闭
        const escHandler = (e) => {
            if (e.key === 'Escape') {
                cancelBtn.click();
                document.removeEventListener('keydown', escHandler);
            }
        };
        document.addEventListener('keydown', escHandler);
    });
}

// 注释：此函数已废弃，新的检查逻辑在workModeSubmit中实现
// 检查逻辑已整合到提交时进行，并支持自动启用协议功能
// 保留函数定义以避免调用错误，但不执行任何操作
async function checkProtocolStatus() {
    // 此函数已被新的自动调整逻辑取代
    console.log('checkProtocolStatus: 此函数已废弃，检查逻辑已移至提交时');
}

// 检查MQTT/TCP透传模式的协议配置
async function checkProtocolsForMqttTcpMode() {
    try {
        const [mqttInfo, tcpInfo] = await Promise.all([
            fetchData('/mqtt_info').catch(() => null),
            fetchData('/tcp_info').catch(() => null)
        ]);
        
        const mqttEnabled = mqttInfo && mqttInfo.use_mqtt === '1';
        const tcpEnabled = tcpInfo && tcpInfo.use_tcp === '1';
        
        // 至少需要一个协议启用
        if (!mqttEnabled && !tcpEnabled) {
            const unconfiguredProtocols = [];
            if (!mqttEnabled) unconfiguredProtocols.push('MQTT');
            if (!tcpEnabled) unconfiguredProtocols.push('TCP');
            
            return {
                valid: false,
                message: 'MQTT/TCP透传模式至少需要启用MQTT或TCP之一',
                unconfiguredProtocols: unconfiguredProtocols,
                mqttEnabled: mqttEnabled,
                tcpEnabled: tcpEnabled
            };
        }
        
        return {
            valid: true,
            message: '协议配置检查通过'
        };
    } catch (error) {
        console.error('协议配置检查失败:', error);
        return {
            valid: false,
            message: '协议配置检查失败，请稍后重试',
            unconfiguredProtocols: []
        };
    }
}

// 滚动到协议管理区域
function scrollToProtocolSection() {
    // 根据实际页面结构调整，这里假设协议管理区域有相应的ID或类名
    const protocolSections = [
        document.getElementById('mqttConfig'),
        document.getElementById('tcpConfig'), 
        document.getElementById('httpConfig'),
        document.querySelector('.mqtt-config'),
        document.querySelector('.tcp-config'),
        document.querySelector('.http-config')
    ];
    
    // 找到第一个存在的协议配置区域并滚动到那里
    for (const section of protocolSections) {
        if (section) {
            section.scrollIntoView({ behavior: 'smooth', block: 'start' });
            // 可以添加高亮效果
            section.style.outline = '2px solid #2b6ec0';
            setTimeout(() => {
                section.style.outline = '';
            }, 3000);
            break;
        }
    }
}
