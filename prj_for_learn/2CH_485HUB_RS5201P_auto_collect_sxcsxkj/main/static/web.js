// Basic info API

// 校验位格式转换：统一输出 '0' | '1' | '2'
function convertParityToBackend(value) {
    if (value === undefined || value === null) return '0';
    const v = value.toString().trim().toLowerCase();
    if (['0', '1', '2'].includes(v)) return v;
    if (v === 'none' || v === '无校验' || v === 'disable') return '0';
    if (v === 'odd' || v === '奇') return '1';
    if (v === 'even' || v === '偶') return '2';
    return '0';
}

// ==================== 时间同步模块 ====================
let isTimeSynced = false;
let lastSyncTime = 0;

/**
 * 同步时间到ESP32
 */
async function syncTimeToESP32() {
    try {
        const browserTimestamp = Date.now();
        
        console.log('⏰ 开始同步时间到ESP32...', new Date(browserTimestamp).toLocaleString());
        
        const response = await fetch('/api/sync_time', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                timestamp: browserTimestamp
            })
        });

        if (!response.ok) {
            console.error('❌ 时间同步失败:', response.status);
            return false;
        }

        const result = await response.json();
        if (result.success) {
            isTimeSynced = true;
            lastSyncTime = Date.now();
            
            console.log('✅ 时间同步成功');
            console.log('   ESP32时间:', new Date(result.current_time).toLocaleString());
            
            return true;
        }
        return false;
    } catch (error) {
        console.error('❌ 时间同步异常:', error);
        return false;
    }
}

/**
 * 检查是否需要重新同步（超过1小时）
 */
function shouldResyncTime() {
    if (!lastSyncTime) return true;
    const elapsed = Date.now() - lastSyncTime;
    return elapsed > 60 * 60 * 1000; // 1小时
}
// ==================== 时间同步模块结束 ====================

const basicInfoElementIds = ['host_name', 'current_time', 'uptime', 'sta_mac', 'sta_ip', 'is_dhcps']; // 已移除net_mode
const basicInfoElements = basicInfoElementIds.reduce((obj, id) => ({ ...obj, [id]: document.getElementById(id) }), {});

async function updateBasicInfo() {
    try {
        const response = await fetch('/devinfo');
        if (!response.ok) {
            console.error('Failed to fetch data. Status:', response.status);
            return;
        }
        const data = await response.json();
        basicInfoElements.host_name.textContent = data.host_names || 'RS485-CM 模块在线配置系统';

        // 更新当前时间
        const currentTime = new Date();
        const timeString = currentTime.toLocaleString('zh-CN', {
            year: 'numeric',
            month: '2-digit',
            day: '2-digit',
            hour: '2-digit',
            minute: '2-digit',
            second: '2-digit',
            hour12: false
        });
        basicInfoElements.current_time.textContent = timeString;

        // 格式化显示系统运行时间
        if (data.uptime) {
            basicInfoElements.uptime.textContent = formatUptime(data.uptime);
        } else {
            basicInfoElements.uptime.textContent = '获取中...';
        }

        basicInfoElements.sta_mac.textContent = data.device_sta_mac;
        basicInfoElements.is_dhcps.textContent = data.is_dhcp == 2 ? 'STATIC(静态IP)' : 'DHCP(动态IP)';
        basicInfoElements.sta_ip.textContent = data.sta_ip;
    } catch (error) {
        console.error('Failed to fetch data:', error);
    }
}

// 格式化系统运行时间函数
function formatUptime(microseconds) {
    const totalSeconds = Math.floor(microseconds / 1000000);
    const days = Math.floor(totalSeconds / (24 * 3600));
    const hours = Math.floor((totalSeconds % (24 * 3600)) / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = totalSeconds % 60;

    if (days > 0) {
        return `${days}天 ${hours}小时 ${minutes}分钟`;
    } else if (hours > 0) {
        return `${hours}小时 ${minutes}分钟`;
    } else if (minutes > 0) {
        return `${minutes}分钟 ${seconds}秒`;
    } else {
        return `${seconds}秒`;
    }
}

updateBasicInfo();
setInterval(updateBasicInfo, 5000);

// Modbus手动缓存最大配置数
const MAX_MODBUS_ITEMS = 60;
// 自动采集每通道最大配置数
const AUTO_COLLECT_MAX_ITEMS = 60;
// 自动采集虚拟寄存器地址上限
const AUTO_COLLECT_MAX_VIRTUAL_REG = 49000;
const AUTO_COLLECT_ERROR_MARKER_BASE = 0x8000;

const defaultAutoCollectErrorMarker = index =>
    AUTO_COLLECT_ERROR_MARKER_BASE + index;


// Post Get basic function

async function postData(url = '', data = {}) {
    const response = await fetch(url, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(data)
    });
    const responseText = await response.text();
    let result = {};
    if (responseText) {
        try {
            result = JSON.parse(responseText);
        } catch (_) {
            if (!response.ok) {
                throw new Error(responseText);
            }
            throw new Error('服务器返回了无效的JSON数据');
        }
    }
    if (!response.ok) {
        throw new Error(result.msg || result.message ||
            `HTTP error! status: ${response.status}`);
    }

    // 检查响应内容中的错误状态
    if (result.code && result.code !== 200) {
        throw new Error(result.msg || result.message ||
            `Server error: ${result.code}`);
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

// 自动智能缓存控制相关变量和函数
let autoTransparentCacheEnabled = true; // 默认启用自动智能缓存
// 自动采集模式内存态
const createDefaultAutoCollectItem = () => ({
    enabled: true,
    real_slave_addr: 1,
    function_code: '03',
    register_addr: 0,
    mapped_register_addr: 0,
    register_num: 1,
    interval_ms: 100,
    timeout_ms: 1000,
    baudrate: 9600,
    data_bits: 8,
    parity: 0,
    stop_bits: 1,
    frame_time: 100,
    frame_len: 1024,
    error_marker: null,
    error_clear_count: 50
});

const autoCollectState = {
    ch2: { mapped_slave_addr: 2, allow_exception_response: false, items: [createDefaultAutoCollectItem()] },
    ch3: { mapped_slave_addr: 3, allow_exception_response: false, items: [createDefaultAutoCollectItem()] }
};

function setAutoCollectExceptionSwitch(channel, enabled) {
    const switchBtn = document.getElementById(`ac_${channel}_exception_switch`);
    if (!switchBtn) return;
    switchBtn.classList.toggle('on', enabled);
    switchBtn.classList.toggle('off', !enabled);
    switchBtn.setAttribute('aria-checked', enabled ? 'true' : 'false');
}

function toggleAutoCollectExceptionResponse(channel) {
    const switchBtn = document.getElementById(`ac_${channel}_exception_switch`);
    if (!switchBtn || !autoCollectState[channel]) return;
    const enabled = !switchBtn.classList.contains('on');
    autoCollectState[channel].allow_exception_response = enabled;
    setAutoCollectExceptionSwitch(channel, enabled);
}

function normalizeAutoCollectFunctionCode(value) {
    const raw = String(value ?? '').trim().toLowerCase();
    if (raw === '36' || raw === '0x36' || Number(value) === 54) return 54;
    if (raw === '01' || raw === '1' || Number(value) === 1) return 1;
    if (raw === '02' || raw === '2' || Number(value) === 2) return 2;
    if (raw === '06' || raw === '6' || Number(value) === 6) return 6;
    if (raw === '04' || raw === '4' || Number(value) === 4) return 4;
    return 3;
}

function updateAutoCollectFunctionCode(selectElement) {
    const row = selectElement ? selectElement.closest('.modbus-table-row') : null;
    if (!row) return;
    const countInput = row.querySelector('input[id^="ac_"][id*="_register_num_"]');
    if (!countInput) return;
    if (normalizeAutoCollectFunctionCode(selectElement.value) === 6) {
        countInput.value = '1';
        countInput.max = '1';
    } else {
        countInput.max = '125';
    }
}

// 初始化自动智能缓存控制功能
function initializeAutoTransparentCache() {
    // 初始化时设置正确的UI状态
    updateAutoTransparentCacheUI();
}

let skipWorkModeGuard = false;
let lastSelectedWorkMode = null;

function initializeWorkModeState() {
    const checkedMode = document.querySelector('input[name="work_mode"]:checked');
    lastSelectedWorkMode = checkedMode ? checkedMode.value : null;
}

function applyWorkModeSelection(radioElement) {
    if (!radioElement) return;
    skipWorkModeGuard = true;
    radioElement.checked = true;
    radioElement.dispatchEvent(new Event('change'));
    skipWorkModeGuard = false;
    lastSelectedWorkMode = radioElement.value;
}

function shouldProceedWithWorkModeChange(targetRadio) {
    if (!targetRadio) return true;

    if (skipWorkModeGuard) {
        lastSelectedWorkMode = targetRadio.value;
        return true;
    }

    const currentSyncMode = document.querySelector('input[name="serialSyncMode"]:checked');
    const syncModeValue = currentSyncMode ? currentSyncMode.value : null;
    const targetValue = targetRadio.value;
    const previousValue = lastSelectedWorkMode;
    const isRestrictedTarget =
        targetValue === 'master_slave' ||
        (targetValue === 'modbus_cache' && !autoTransparentCacheEnabled);

    if (syncModeValue === 'slave_follow' && isRestrictedTarget) {
        skipWorkModeGuard = true;
        if (previousValue) {
            const previousRadio = document.querySelector(`input[name="work_mode"][value="${previousValue}"]`);
            if (previousRadio) previousRadio.checked = true;
        }
        targetRadio.checked = false;
        skipWorkModeGuard = false;

        const message =
            targetValue === 'master_slave'
                ? '当前串口参数使用“从机跟随”，无法直接切换到一主多从模式。请先切换串口参数配置模式，或确认后自动调整。'
                : '当前串口参数使用“从机跟随”，在关闭自动智能缓存的Modbus缓存模式下不可用。请先切换串口参数配置模式，或确认后自动调整。';

        showCustomAlert(message, true, {
            showCancel: true,
            confirmText: '仍要切换',
            cancelText: '取消',
            onConfirm: () => {
                const applyRadio = document.querySelector(`input[name="work_mode"][value="${targetValue}"]`);
                applyWorkModeSelection(applyRadio);
            },
            onCancel: () => {
                if (previousValue) {
                    const previousRadio = document.querySelector(`input[name="work_mode"][value="${previousValue}"]`);
                    if (previousRadio) {
                        skipWorkModeGuard = true;
                        previousRadio.checked = true;
                        skipWorkModeGuard = false;
                    }
                }
            }
        });

        return false;
    }

    lastSelectedWorkMode = targetValue;
    return true;
}

// 判断当前是否允许使用“从机跟随”串口配置模式
function isSlaveFollowAllowed() {
    const currentMode = document.querySelector('input[name="work_mode"]:checked');
    if (!currentMode) return true;

    if (currentMode.value === 'master_slave') {
        return false;
    }

    if (currentMode.value === 'modbus_cache' && !autoTransparentCacheEnabled) {
        return false;
    }

    return true;
}

function getSlaveFollowRestrictionMessage() {
    const currentMode = document.querySelector('input[name="work_mode"]:checked');
    if (!currentMode) return '';

    if (currentMode.value === 'master_slave') {
        return '一主多从模式下无法使用“从机跟随”串口参数配置，请选择其他模式。';
    }

    if (currentMode.value === 'modbus_cache' && !autoTransparentCacheEnabled) {
        return '在Modbus协议缓存模式中关闭自动智能缓存后无法使用“从机跟随”配置模式。';
    }

    return '';
}

function updateSlaveFollowAvailability(options = {}) {
    const slaveFollowRadio = document.querySelector('input[name="serialSyncMode"][value="slave_follow"]');
    if (!slaveFollowRadio) return;

    const optionLabel = slaveFollowRadio.closest('.sync-option');
    const allowed = isSlaveFollowAllowed();
    const restrictionMsg = getSlaveFollowRestrictionMessage();

    if (allowed) {
        slaveFollowRadio.disabled = false;
        slaveFollowRadio.removeAttribute('title');
        if (optionLabel) optionLabel.classList.remove('disabled');
        return;
    }

    slaveFollowRadio.disabled = true;
    if (optionLabel) optionLabel.classList.add('disabled');
    if (restrictionMsg) {
        slaveFollowRadio.setAttribute('title', restrictionMsg);
    } else {
        slaveFollowRadio.removeAttribute('title');
    }

    if (slaveFollowRadio.checked) {
        const fallbackRadio =
            document.querySelector('input[name="serialSyncMode"][value="unified"]') ||
            document.querySelector('input[name="serialSyncMode"][value="separate"]');
        const fallbackValue = fallbackRadio ? fallbackRadio.value : 'unified';

        if (fallbackRadio) {
            fallbackRadio.checked = true;
        }

        if (typeof switchSerialConfigMode === 'function') {
            switchSerialConfigMode(fallbackValue);
        }

        if (typeof updateSyncModeCheckIcon === 'function') {
            updateSyncModeCheckIcon(fallbackValue);
        }

        saveSerialConfigMode(fallbackValue);
        showCustomAlert(restrictionMsg || '当前工作模式不支持“从机跟随”，已切换为其他串口配置模式。', true);
    } else if (options.notify && restrictionMsg) {
        showCustomAlert(restrictionMsg, true);
    }
}

// 切换自动智能缓存状态 - 供HTML onclick调用
function applyAutoTransparentCacheState(newState, options = {}) {
    const { skipBackendSync = false } = options;

    autoTransparentCacheEnabled = newState;
    updateAutoTransparentCacheUI();
    updateSerialConfigForCacheMode();
    updateSlaveFollowAvailability();

    const workModeContent = document.getElementById('workModeContent');
    if (workModeContent) {
        setTimeout(() => {
            workModeContent.style.maxHeight = workModeContent.scrollHeight + 'px';
        }, 100);
    }

    console.log('自动智能缓存状态已切换为:', autoTransparentCacheEnabled ? '启用' : '禁用');

    if (!skipBackendSync) {
        const currentMode = document.querySelector('input[name="work_mode"]:checked');
        if (currentMode && currentMode.value === 'modbus_cache') {
            syncAutoTransparentCacheToBackend();
        }
    }
}

function toggleAutoTransparentCache() {
    const targetState = !autoTransparentCacheEnabled;
    const currentMode = document.querySelector('input[name="work_mode"]:checked');
    const currentSyncMode = document.querySelector('input[name="serialSyncMode"]:checked');
    const isModbusCache = currentMode && currentMode.value === 'modbus_cache';
    const isSlaveFollow = currentSyncMode && currentSyncMode.value === 'slave_follow';

    if (!targetState && isModbusCache && isSlaveFollow) {
        showCustomAlert(
            '当前串口参数使用“从机跟随”，关闭自动智能缓存后该模式将不可用。请先切换串口参数配置模式，或点击“仍要关闭”继续并自动调整。',
            true,
            {
                showCancel: true,
                confirmText: '仍要关闭',
                cancelText: '取消',
                onConfirm: () => applyAutoTransparentCacheState(targetState),
                onCancel: () => updateAutoTransparentCacheUI()
            }
        );
        return;
    }

    applyAutoTransparentCacheState(targetState);
}

// 同步自动智能缓存状态到后端
async function syncAutoTransparentCacheToBackend() {
    try {
        const data = {
            work_mode: 'modbus_cache',
            auto_transparent_cache: autoTransparentCacheEnabled
        };

        await postData('/mode_set', data);
        console.log('自动智能缓存状态已同步到后端:', autoTransparentCacheEnabled);
    } catch (error) {
        console.error('同步自动智能缓存状态失败:', error);
        showCustomAlert('同步配置失败: ' + error.message, true);
    }
}

// 更新自动智能缓存UI状态
function updateAutoTransparentCacheUI() {
    const autoTransparentCacheSwitch = document.getElementById('autoTransparentCacheSwitch');

    if (!autoTransparentCacheSwitch) {
        return;
    }

    if (autoTransparentCacheEnabled) {
        // 启用状态
        autoTransparentCacheSwitch.classList.remove('off');
        autoTransparentCacheSwitch.classList.add('on');
    } else {
        // 禁用状态
        autoTransparentCacheSwitch.classList.remove('on');
        autoTransparentCacheSwitch.classList.add('off');
    }
}

// 根据自动智能缓存状态更新串口配置显示（仅控制自动智能缓存相关的UI，不影响设置按钮位置）
function updateSerialConfigForCacheMode() {
    const modbusConfig = document.getElementById('modbus_rtu_config');

    if (!modbusConfig) return;

    // 检查当前是否为modbus_cache模式
    const currentMode = document.querySelector('input[name="work_mode"]:checked');
    const isModbusCacheMode = currentMode && currentMode.value === 'modbus_cache';

    // 如果不是modbus_cache模式，直接返回
    if (!isModbusCacheMode) return;

    if (autoTransparentCacheEnabled) {
        // 自动智能缓存模式：隐藏Modbus-RTU手动配置
        const modbusItemSection = modbusConfig.querySelector('.modbus-item');
        if (modbusItemSection) modbusItemSection.style.display = 'none';
    } else {
        // 手动缓存模式：显示Modbus-RTU配置
        const modbusItemSection = modbusConfig.querySelector('.modbus-item');
        if (modbusItemSection) modbusItemSection.style.display = 'block';
    }

    // // 刷新串口标签显示状态
    // if (typeof currentSerialPort !== 'undefined') {
    //     switchSerialTab(currentSerialPort);
    // } else {
    //     switchSerialTab(1); // 默认显示CH1
    // }

    // 强制重新计算容器高度，确保显示正常
    const workModeContent = document.getElementById('workModeContent');
    if (workModeContent) {
        setTimeout(() => {
            workModeContent.style.maxHeight = workModeContent.scrollHeight + 'px';
        }, 50);
    }
}

// 页面加载时初始化折叠功能
document.addEventListener('DOMContentLoaded', () => {
    // 初始化工作模式的折叠功能
    initializeToggle('workModeToggle', 'workModeContent');

    // 初始化串口设置的折叠功能
    initializeToggle('serialConfigToggle', 'serialConfigContent');

    // 初始化自动智能缓存控制功能
    initializeAutoTransparentCache();

    updateSlaveFollowAvailability();
    initializeWorkModeState();
});

// 在内容变化时更新最大高度
const resizeObserver = new ResizeObserver(entries => {
    entries.forEach(entry => {
        // 检查是否是Modbus过滤部分的content-area
        const isModbusFilterArea = entry.target.closest('#modbusFilterCard');
        if (!isModbusFilterArea && !entry.target.classList.contains('collapsed')) {
            entry.target.style.maxHeight = entry.target.scrollHeight + 'px';
        }
    });
});

// 观察内容区域的变化（排除Modbus过滤部分）
document.querySelectorAll('.content-area').forEach(area => {
    // 检查是否是Modbus过滤部分的content-area
    const isModbusFilterArea = area.closest('#modbusFilterCard');
    if (!isModbusFilterArea) {
        resizeObserver.observe(area);
    }
});


// Work mode API


// Modbus地址过滤配置
let modbusFilterConfig = {
    enabled: false,
    mode: 0, // 0: 禁用, 1: 白名单, 2: 黑名单
    ranges: []
};

// Modbus从机地址映射配置
let modbusSlaveMapping = {
    enabled: false,
    mappings: []
};

// 加载Modbus地址过滤配置
async function loadModbusFilterConfig() {
    try {
        const response = await fetch('/filter_config');
        const data = await response.json();
        modbusFilterConfig = data;
        updateModbusFilterUI();
    } catch (error) {
        console.error('加载Modbus地址过滤配置失败:', error);
    }
}

// 保存Modbus地址过滤配置
async function saveModbusFilterConfig() {
    try {
        // 更新配置数据
        const filterEnabled = document.getElementById('modbus_filter_enabled');
        const filterMode = document.getElementById('modbus_filter_mode');

        if (filterEnabled) {
            modbusFilterConfig.enabled = filterEnabled.checked;
        }

        if (filterMode) {
            modbusFilterConfig.mode = parseInt(filterMode.value);
        }

        // 验证所有从机号
        let hasError = false;
        for (let i = 0; i < modbusFilterConfig.ranges.length; i++) {
            const range = modbusFilterConfig.ranges[i];
            // 确保寄存器地址范围为完整范围
            range.start = 0;
            range.end = 65535;
            
            const error = validateSlaveId(range.slave_id || 0);
            if (error) {
                displayErrorMessage(`从机号 ${i + 1} 验证失败: ${error}`);
                hasError = true;
                break;
            }
        }

        if (hasError) {
            return;
        }

        const response = await fetch('/filter_set', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
            },
            body: JSON.stringify(modbusFilterConfig)
        });
        const result = await response.json();
        if (result.success) {
            displaySuccessMessage('Modbus地址过滤配置已成功保存到NVS');
        } else {
            displayErrorMessage('Modbus地址过滤配置保存失败: ' + result.error);
        }
    } catch (error) {
        console.error('保存Modbus地址过滤配置失败:', error);
        displayErrorMessage('保存Modbus地址过滤配置失败');
    }
}

// 加载Modbus从机地址映射配置
async function loadSlaveMapping() {
    try {
        const response = await fetch('/slave_mapping_config');
        const data = await response.json();
        modbusSlaveMapping = data;
        updateSlaveMappingUI();
    } catch (error) {
        console.error('加载Modbus从机地址映射配置失败:', error);
    }
}

// 保存Modbus从机地址映射配置
async function saveMappingConfig() {
    try {
        // 更新配置数据
        const mappingEnabled = document.getElementById('modbus_mapping_enabled');

        if (mappingEnabled) {
            modbusSlaveMapping.enabled = mappingEnabled.checked;
        }

        // 更新映射条目数据
        const entriesContainer = document.getElementById('modbus_mapping_entries');
        const entries = entriesContainer.querySelectorAll('.mapping-entry');

        modbusSlaveMapping.mappings = [];
        entries.forEach((entry, index) => {
            const virtualAddr = parseInt(entry.querySelector('.virtual-addr').value) || 1;
            const realAddr = parseInt(entry.querySelector('.real-addr').value) || 1;

            modbusSlaveMapping.mappings.push({
                virtual_addr: virtualAddr,
                real_addr: realAddr,
                enabled: true
            });
        });

        // 发送保存请求
        const response = await fetch('/slave_mapping_set', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(modbusSlaveMapping)
        });

        if (response.ok) {
            displaySuccessMessage('从机地址映射配置保存成功');
            console.log('从机地址映射配置已保存:', modbusSlaveMapping);
        } else {
            throw new Error(`保存失败: ${response.status}`);
        }
    } catch (error) {
        console.error('保存从机地址映射配置失败:', error);
        showCustomAlert('保存从机地址映射配置失败: ' + error.message, true);
    }
}

// 更新Modbus从机地址映射UI
function updateSlaveMappingUI(options) {
    const skipRestore = options && options.skipRestore === true;
    const mappingEnabled = document.getElementById('modbus_mapping_enabled');
    const entriesContainer = document.getElementById('modbus_mapping_entries');

    if (mappingEnabled) {
        mappingEnabled.checked = modbusSlaveMapping.enabled;
    }

    if (entriesContainer) {
        // 仅当不跳过恢复时，保存当前输入框的值和焦点状态
        const currentValues = skipRestore ? null : saveCurrentInputValues();
        const focusedElement = skipRestore ? null : document.activeElement;
        const focusedIndex = skipRestore ? null : getFocusedInputIndex(focusedElement);

        entriesContainer.innerHTML = '';

        modbusSlaveMapping.mappings.forEach((mapping, index) => {
            createMappingEntry(mapping, index, entriesContainer);
        });

        // 恢复输入框的值和焦点（导入场景跳过，避免覆盖新导入的数据）
        if (!skipRestore) {
            restoreInputValues(currentValues);
            restoreFocus(focusedIndex, focusedElement ? focusedElement.className : '');
        }

        // 如果没有映射条目，添加一个默认的
        if (modbusSlaveMapping.mappings.length === 0) {
            addMappingEntry();
        }
    }
}

// 创建单个映射条目的DOM元素
function createMappingEntry(mapping, index, container) {
    const entryDiv = document.createElement('div');
    entryDiv.className = 'mapping-entry';
    entryDiv.setAttribute('data-index', index);

    entryDiv.innerHTML = `
        <span class="mapping-label">虚拟地址:</span>
        <input type="number" class="virtual-addr" value="${mapping.virtual_addr}" min="1" max="247" 
               oninput="updateMappingData(${index}, 'virtual_addr', this.value)" 
               onblur="validateAndUpdateMapping(${index}, 'virtual_addr', this)"
               onkeydown="handleMappingKeyDown(event, ${index}, 'virtual')">
        <span class="mapping-arrow">→</span>
        <span class="mapping-label">真实地址:</span>
        <input type="number" class="real-addr" value="${mapping.real_addr}" min="1" max="247" 
               oninput="updateMappingData(${index}, 'real_addr', this.value)" 
               onblur="validateAndUpdateMapping(${index}, 'real_addr', this)"
               onkeydown="handleMappingKeyDown(event, ${index}, 'real')">
        <button type="button" class="delete-btn" onclick="removeMappingEntry(${index})" title="删除此映射条目">删除</button>
    `;

    container.appendChild(entryDiv);
}

// 保存当前所有输入框的值
function saveCurrentInputValues() {
    const values = [];
    const entries = document.querySelectorAll('.mapping-entry');
    entries.forEach((entry, index) => {
        const virtualInput = entry.querySelector('.virtual-addr');
        const realInput = entry.querySelector('.real-addr');
        values.push({
            virtual: virtualInput ? virtualInput.value : '',
            real: realInput ? realInput.value : ''
        });
    });
    return values;
}

// 恢复输入框的值
function restoreInputValues(savedValues) {
    const entries = document.querySelectorAll('.mapping-entry');
    entries.forEach((entry, index) => {
        if (savedValues[index]) {
            const virtualInput = entry.querySelector('.virtual-addr');
            const realInput = entry.querySelector('.real-addr');
            if (virtualInput && savedValues[index].virtual) {
                virtualInput.value = savedValues[index].virtual;
            }
            if (realInput && savedValues[index].real) {
                realInput.value = savedValues[index].real;
            }
        }
    });
}

// 获取当前焦点输入框的索引和类型
function getFocusedInputIndex(element) {
    if (!element || !element.closest) return null;
    const entry = element.closest('.mapping-entry');
    if (!entry) return null;

    const index = parseInt(entry.getAttribute('data-index'));
    const isVirtual = element.classList.contains('virtual-addr');
    const isReal = element.classList.contains('real-addr');

    if (isVirtual || isReal) {
        return {
            index: index,
            type: isVirtual ? 'virtual' : 'real',
            cursorPosition: element.selectionStart
        };
    }
    return null;
}

// 恢复焦点
function restoreFocus(focusInfo, className) {
    if (!focusInfo) return;

    const entries = document.querySelectorAll('.mapping-entry');
    if (entries[focusInfo.index]) {
        const input = entries[focusInfo.index].querySelector(
            focusInfo.type === 'virtual' ? '.virtual-addr' : '.real-addr'
        );
        if (input) {
            input.focus();
            if (focusInfo.cursorPosition !== undefined) {
                input.setSelectionRange(focusInfo.cursorPosition, focusInfo.cursorPosition);
            }
        }
    }
}

// 实时更新映射数据（不触发UI重建）
function updateMappingData(index, field, value) {
    if (index >= 0 && index < modbusSlaveMapping.mappings.length) {
        const numValue = parseInt(value);
        if (!isNaN(numValue)) {
            modbusSlaveMapping.mappings[index][field] = numValue;
        }
    }
}

// 验证并更新映射数据
function validateAndUpdateMapping(index, field, input) {
    const value = parseInt(input.value);
    if (isNaN(value) || value < 1 || value > 247) {
        input.value = modbusSlaveMapping.mappings[index][field]; // 恢复原值
        showCustomAlert('从机地址必须在1-247之间', true);
    } else {
        // 检查重复的虚拟地址
        if (field === 'virtual_addr') {
            const duplicate = modbusSlaveMapping.mappings.find((mapping, idx) =>
                idx !== index && mapping.virtual_addr === value
            );
            if (duplicate) {
                input.value = modbusSlaveMapping.mappings[index][field]; // 恢复原值
                showCustomAlert('虚拟地址不能重复', true);
                return;
            }
        }

        // 更新数据
        modbusSlaveMapping.mappings[index][field] = value;
    }
}

// 添加映射条目（改进版 - 只添加新条目，不重建整个UI）
function addMappingEntry() {
    if (modbusSlaveMapping.mappings.length < 247) {
        const newMapping = {
            virtual_addr: getNextAvailableVirtualAddr(),
            real_addr: getNextAvailableRealAddr(),
            enabled: true
        };

        modbusSlaveMapping.mappings.push(newMapping);

        // 只添加新的DOM元素，不重建整个UI
        const entriesContainer = document.getElementById('modbus_mapping_entries');
        if (entriesContainer) {
            const newIndex = modbusSlaveMapping.mappings.length - 1;
            createMappingEntry(newMapping, newIndex, entriesContainer);

            // 聚焦到新添加的虚拟地址输入框
            const newEntry = entriesContainer.lastElementChild;
            const virtualInput = newEntry.querySelector('.virtual-addr');
            if (virtualInput) {
                virtualInput.focus();
                virtualInput.select();
            }
        }
    } else {
        showCustomAlert('最多只能添加247个映射条目（Modbus最大从机数量）', true);
    }
}

// 获取下一个可用的虚拟地址
function getNextAvailableVirtualAddr() {
    const usedAddrs = new Set(modbusSlaveMapping.mappings.map(m => m.virtual_addr));
    for (let addr = 1; addr <= 247; addr++) {
        if (!usedAddrs.has(addr)) {
            return addr;
        }
    }
    return 1; // 如果都被占用，返回1（虽然这种情况不太可能）
}

// 获取下一个可用的真实地址
function getNextAvailableRealAddr() {
    const usedAddrs = new Set(modbusSlaveMapping.mappings.map(m => m.real_addr));
    for (let addr = 1; addr <= 247; addr++) {
        if (!usedAddrs.has(addr)) {
            return addr;
        }
    }
    return 1; // 如果都被占用，返回1
}

// 删除映射条目（改进版 - 只移除对应DOM元素，不重建整个UI）
function removeMappingEntry(index) {
    if (index >= 0 && index < modbusSlaveMapping.mappings.length) {
        // 从数据中删除
        modbusSlaveMapping.mappings.splice(index, 1);

        // 从DOM中删除对应元素
        const entries = document.querySelectorAll('.mapping-entry');
        if (entries[index]) {
            entries[index].remove();
        }

        // 更新剩余条目的索引和事件处理器
        updateMappingIndices();

        // 如果删除后没有条目了，添加一个默认条目
        if (modbusSlaveMapping.mappings.length === 0) {
            addMappingEntry();
        }
    }
}

// 更新映射条目的索引和事件处理器
function updateMappingIndices() {
    const entries = document.querySelectorAll('.mapping-entry');
    entries.forEach((entry, newIndex) => {
        entry.setAttribute('data-index', newIndex);

        // 更新输入框的事件处理器
        const virtualInput = entry.querySelector('.virtual-addr');
        const realInput = entry.querySelector('.real-addr');
        const deleteBtn = entry.querySelector('.delete-btn');

        if (virtualInput) {
            virtualInput.setAttribute('oninput', `updateMappingData(${newIndex}, 'virtual_addr', this.value)`);
            virtualInput.setAttribute('onblur', `validateAndUpdateMapping(${newIndex}, 'virtual_addr', this)`);
            virtualInput.setAttribute('onkeydown', `handleMappingKeyDown(event, ${newIndex}, 'virtual')`);
        }

        if (realInput) {
            realInput.setAttribute('oninput', `updateMappingData(${newIndex}, 'real_addr', this.value)`);
            realInput.setAttribute('onblur', `validateAndUpdateMapping(${newIndex}, 'real_addr', this)`);
            realInput.setAttribute('onkeydown', `handleMappingKeyDown(event, ${newIndex}, 'real')`);
        }

        if (deleteBtn) {
            deleteBtn.setAttribute('onclick', `removeMappingEntry(${newIndex})`);
        }
    });
}

// 处理映射条目中的键盘快捷键
function handleMappingKeyDown(event, index, type) {
    const entries = document.querySelectorAll('.mapping-entry');
    const currentEntry = entries[index];

    if (!currentEntry) return;

    switch (event.key) {
        case 'Tab':
            // Tab键：移动到下一个输入框
            if (!event.shiftKey) {
                if (type === 'virtual') {
                    // 从虚拟地址移动到真实地址
                    event.preventDefault();
                    const realInput = currentEntry.querySelector('.real-addr');
                    if (realInput) {
                        realInput.focus();
                        realInput.select();
                    }
                } else if (type === 'real') {
                    // 从真实地址移动到下一行的虚拟地址
                    event.preventDefault();
                    if (index + 1 < entries.length) {
                        const nextVirtualInput = entries[index + 1].querySelector('.virtual-addr');
                        if (nextVirtualInput) {
                            nextVirtualInput.focus();
                            nextVirtualInput.select();
                        }
                    } else {
                        // 如果是最后一行，添加新条目
                        addMappingEntry();
                    }
                }
            } else {
                // Shift+Tab：移动到上一个输入框
                if (type === 'real') {
                    event.preventDefault();
                    const virtualInput = currentEntry.querySelector('.virtual-addr');
                    if (virtualInput) {
                        virtualInput.focus();
                        virtualInput.select();
                    }
                } else if (type === 'virtual' && index > 0) {
                    event.preventDefault();
                    const prevRealInput = entries[index - 1].querySelector('.real-addr');
                    if (prevRealInput) {
                        prevRealInput.focus();
                        prevRealInput.select();
                    }
                }
            }
            break;

        case 'Enter':
            // Enter键：添加新条目
            event.preventDefault();
            addMappingEntry();
            break;

        case 'Delete':
            // 如果按住Ctrl+Delete，删除当前条目
            if (event.ctrlKey && modbusSlaveMapping.mappings.length > 1) {
                event.preventDefault();
                removeMappingEntry(index);
            }
            break;

        case 'ArrowUp':
            // 上箭头：移动到上一行同类型输入框
            if (index > 0) {
                event.preventDefault();
                const prevInput = entries[index - 1].querySelector(
                    type === 'virtual' ? '.virtual-addr' : '.real-addr'
                );
                if (prevInput) {
                    prevInput.focus();
                    prevInput.select();
                }
            }
            break;

        case 'ArrowDown':
            // 下箭头：移动到下一行同类型输入框
            if (index + 1 < entries.length) {
                event.preventDefault();
                const nextInput = entries[index + 1].querySelector(
                    type === 'virtual' ? '.virtual-addr' : '.real-addr'
                );
                if (nextInput) {
                    nextInput.focus();
                    nextInput.select();
                }
            } else {
                // 如果是最后一行，添加新条目并聚焦
                event.preventDefault();
                addMappingEntry();
            }
            break;
    }
}

// 旧的验证函数已被 validateAndUpdateMapping 替代

// 切换映射模式
function toggleMappingMode() {
    const mappingEnabled = document.getElementById('modbus_mapping_enabled');
    if (mappingEnabled) {
        modbusSlaveMapping.enabled = mappingEnabled.checked;
        console.log('从机地址映射状态已切换为:', modbusSlaveMapping.enabled ? '启用' : '禁用');
    }
}

// 导出映射配置
function exportMappingConfig() {
    try {
        const config = {
            version: "1.0",
            timestamp: new Date().toISOString(),
            mapping_config: modbusSlaveMapping
        };

        const dataStr = JSON.stringify(config, null, 2);
        const dataBlob = new Blob([dataStr], { type: 'application/json' });

        const link = document.createElement('a');
        link.href = URL.createObjectURL(dataBlob);
        link.download = `modbus_slave_mapping_${new Date().toISOString().slice(0, 19).replace(/:/g, '-')}.json`;
        link.click();

        displaySuccessMessage('映射配置导出成功');
    } catch (error) {
        console.error('导出映射配置失败:', error);
        showCustomAlert('导出配置失败: ' + error.message, true);
    }
}

// 导入映射配置
function importMappingConfig(event) {
    const file = event.target.files[0];
    if (!file) return;

    const reader = new FileReader();
    reader.onload = function (e) {
        try {
            const config = JSON.parse(e.target.result);

            if (config.mapping_config) {
                modbusSlaveMapping = config.mapping_config;
                // 导入后跳过恢复旧输入值，确保UI与导入配置一致
                updateSlaveMappingUI({ skipRestore: true });
                displaySuccessMessage('映射配置导入成功');
            } else {
                throw new Error('配置文件格式错误');
            }
        } catch (error) {
            console.error('导入映射配置失败:', error);
            showCustomAlert('导入配置失败: ' + error.message, true);
        }
    };
    reader.readAsText(file);

    // 清除file input的值，以便可以重复选择同一个文件
    event.target.value = '';
}

// 更新Modbus地址过滤UI
function updateModbusFilterUI() {
    const filterEnabled = document.getElementById('modbus_filter_enabled');
    const filterMode = document.getElementById('modbus_filter_mode');
    const rangesContainer = document.getElementById('modbus_filter_ranges');

    if (filterEnabled) {
        filterEnabled.checked = modbusFilterConfig.enabled;
    }

    if (filterMode) {
        // 如果模式是0（禁用过滤），设置为白名单模式（1）作为默认值
        if (modbusFilterConfig.mode === 0) {
            modbusFilterConfig.mode = 1;
        }
        filterMode.value = modbusFilterConfig.mode;
    }

    if (rangesContainer) {
        rangesContainer.innerHTML = '';
        modbusFilterConfig.ranges.forEach((range, index) => {
            const rangeDiv = document.createElement('div');
            rangeDiv.className = 'filter-range-item';
            rangeDiv.innerHTML = `
                <span style="font-weight: bold; color: var(--FG-2);">从机号:</span>
                <input type="number" min="0" max="247" value="${range.slave_id || 0}" 
                       onchange="updateFilterRange(${index}, 'slave_id', this.value)" 
                       placeholder="从机号(0=全部)" style="width: 100px;">
                <button type="button" onclick="removeFilterRange(${index})" class="remove-btn" style="margin-left: 15px;">删除</button>
            `;
            rangesContainer.appendChild(rangeDiv);
        });
    }
}

// 添加地址范围
function addFilterRange() {
    if (modbusFilterConfig.ranges.length < 10) {
        // 自动填充完整的寄存器地址范围(0-65535)，只需要用户配置从机号
        modbusFilterConfig.ranges.push({ slave_id: 1, start: 0, end: 65535 });
        updateModbusFilterUI();
    } else {
        displayErrorMessage('从机地址数量已达上限(10个)');
    }
}

// 验证从机号
function validateSlaveId(slave_id) {
    const minSlaveId = 0;  // 0表示匹配所有从机
    const maxSlaveId = 247;

    if (slave_id < minSlaveId || slave_id > maxSlaveId) {
        return `从机号必须在 ${minSlaveId}-${maxSlaveId} 范围内 (0表示匹配所有从机)`;
    }

    return null; // 验证通过
}

// 验证地址范围（保留兼容性，但只验证从机号）
function validateAddressRange(slave_id, start, end) {
    // 现在只需要验证从机号，寄存器地址固定为0-65535
    return validateSlaveId(slave_id);
}

// 更新地址范围
function updateFilterRange(index, field, value) {
    if (index >= 0 && index < modbusFilterConfig.ranges.length) {
        if (field === 'slave_id') {
            // 只允许更新从机号，寄存器地址范围固定为0-65535
            const newValue = parseInt(value) || 0;
            modbusFilterConfig.ranges[index].slave_id = newValue;
            // 确保寄存器地址范围始终为完整范围
            modbusFilterConfig.ranges[index].start = 0;
            modbusFilterConfig.ranges[index].end = 65535;

            // 验证从机号
            const error = validateSlaveId(newValue);
            if (error) {
                console.warn(`从机号 ${index + 1} 验证失败: ${error}`);
            }
        }
        // 忽略对start和end的更新请求，因为它们固定为0-65535
    }
}

// 移除地址范围
function removeFilterRange(index) {
    if (index >= 0 && index < modbusFilterConfig.ranges.length) {
        modbusFilterConfig.ranges.splice(index, 1);
        updateModbusFilterUI();
    }
}

// 切换过滤模式
function toggleFilterMode() {
    const filterEnabled = document.getElementById('modbus_filter_enabled');
    if (filterEnabled) {
        modbusFilterConfig.enabled = filterEnabled.checked;
    }
}

// 切换过滤类型
function changeFilterMode() {
    const filterMode = document.getElementById('modbus_filter_mode');
    if (filterMode) {
        modbusFilterConfig.mode = parseInt(filterMode.value);
    }
}

// 修改工作模式切换事件处理
document.querySelectorAll('input[name="work_mode"]').forEach(radio => {
    radio.addEventListener('change', async function () {
        if (!shouldProceedWithWorkModeChange(this)) {
            return;
        }

        const container = document.getElementById('modbus_items_container');
        const modbusConfig = document.getElementById('modbus_rtu_config');
        const workModeContent = document.getElementById('workModeContent');
        const buttonGroup = document.querySelector('.button-group');
        let setModbusItemButton = document.getElementById('setModbusItem');
        const setModbusItemViews = document.getElementById('setModbusItemViews');
        const serialConfigContent = document.getElementById('serialConfigContent');
        const serialConfigCard = serialConfigContent ? serialConfigContent.closest('.card-view') : null;
        const modbusFilterCard = document.getElementById('modbusFilterCard');
        const workModeContainer = document.querySelector('.work-mode-container');
        const autoCacheControl = document.getElementById('auto_cache_control');
        const autoCollectConfig = document.getElementById('auto_collect_config');

        // 如果设置按钮不存在，创建一个
        if (!setModbusItemButton) {
            setModbusItemButton = document.createElement('button');
            setModbusItemButton.type = 'button';
            setModbusItemButton.id = 'setModbusItem';
            setModbusItemButton.className = 'main-btn';
            setModbusItemButton.innerHTML = '<div class="btn-container">配置</div>';
            setModbusItemButton.addEventListener('click', workModeSubmit);
        }

        // 获取TCP协议类型按钮
        const tcpServerBtn = document.getElementById('tcpServer');
        const tcpClientBtn = document.getElementById('tcpClient');
        const selectedTCP = document.getElementById('selectedTCP');

        // 首先隐藏Modbus-RTU配置，只在Modbus-RTU模式下显示
        modbusConfig.style.display = 'none';
        if (autoCollectConfig) autoCollectConfig.style.display = 'none';

        if (this.value === 'modbus_queue') {
            // Modbus协议排队模式
            // 移除modbus-rtu-mode类，添加其他模式的样式
            if (workModeContainer) {
                workModeContainer.classList.remove('modbus-rtu-mode');
            }

            if (autoCacheControl) {
                autoCacheControl.style.display = 'none';
            }

            if (tcpServerBtn && tcpClientBtn) {
                // 强制选择TCPServer
                tcpServerBtn.style.background = '#2b6ec0';
                tcpClientBtn.style.background = '#898989a1';
                tcpClientBtn.disabled = true; // 禁用TCPClient按钮
                selectedTCP.value = '1'; // 设置为TCPServer模式
            }
            // 确保串口设置可见
            if (serialConfigCard) serialConfigCard.style.display = 'block';
            // 显示Modbus地址过滤配置（仅在Modbus协议排队模式下启用）
            if (modbusFilterCard) modbusFilterCard.style.display = 'block';
            // 显示Modbus从机地址映射配置（仅在Modbus协议排队模式下启用）
            const modbusSlaveMapCard = document.getElementById('modbusSlaveMapCard');
            if (modbusSlaveMapCard) modbusSlaveMapCard.style.display = 'block';
            // 清空Modbus项容器
            container.innerHTML = '';
            // 移动设置按钮
            setModbusItemViews.appendChild(setModbusItemButton);
            // 更新按钮文本为"配置"
            setModbusItemButton.querySelector('.btn-container').textContent = '配置';
            // 更新串口标签显示为Modbus协议模式
            updateSerialTabsForModbus();
            // 刷新串口标签状态
            refreshSerialTab();
            // 显示回复超时时间字段
            showReplyTimeoutFields();
        } else if (this.value === 'transparent_queue') {
            // TCP透传模式
            // 移除modbus-rtu-mode类，添加其他模式的样式
            if (workModeContainer) {
                workModeContainer.classList.remove('modbus-rtu-mode');
            }

            if (autoCacheControl) {
                autoCacheControl.style.display = 'none';
            }

            // 恢复TCPClient按钮
            if (tcpClientBtn) {
                tcpClientBtn.disabled = false;
            }
            // 确保串口设置可见
            if (serialConfigCard) serialConfigCard.style.display = 'block';
            // 隐藏Modbus地址过滤配置（只在Modbus协议排队模式下显示）
            if (modbusFilterCard) modbusFilterCard.style.display = 'none';
            // 隐藏Modbus从机地址映射配置（只在Modbus协议排队模式下显示）
            const modbusSlaveMapCard1 = document.getElementById('modbusSlaveMapCard');
            if (modbusSlaveMapCard1) modbusSlaveMapCard1.style.display = 'none';
            // 清空Modbus项容器
            container.innerHTML = '';
            // 移动设置按钮
            setModbusItemViews.appendChild(setModbusItemButton);
            // 更新按钮文本为"配置"
            setModbusItemButton.querySelector('.btn-container').textContent = '配置';
            // 更新串口标签显示为透传模式
            updateSerialTabsForTransparent();
            // 刷新串口标签状态
            refreshSerialTab();
            // 显示回复超时时间字段
            showReplyTimeoutFields();
        } else if (this.value === 'modbus_cache') {
            // Modbus-RTU网关模式
            // 添加modbus-rtu-mode类，保持原位置样式
            if (workModeContainer) {
                workModeContainer.classList.add('modbus-rtu-mode');
            }

            modbusConfig.style.display = 'block';
            if (autoCacheControl) {
                autoCacheControl.style.display = 'flex';
            }
            // 显示串口配置
            serialConfigCard.style.display = 'block';
            // 隐藏Modbus地址过滤配置
            if (modbusFilterCard) modbusFilterCard.style.display = 'none';
            // 隐藏Modbus从机地址映射配置
            const modbusSlaveMapCard3 = document.getElementById('modbusSlaveMapCard');
            if (modbusSlaveMapCard3) modbusSlaveMapCard3.style.display = 'none';

            // 清空Modbus项容器
            container.innerHTML = '';
            // 移动设置按钮到右下角（与其他模式保持一致）
            setModbusItemViews.appendChild(setModbusItemButton);
            // 更新按钮文本为"配置"
            setModbusItemButton.querySelector('.btn-container').textContent = '配置';
            // 更新串口标签显示为Modbus协议缓存模式
            updateSerialTabsForModbus();
            // 刷新串口标签状态
            refreshSerialTab();
            // 显示回复超时时间字段
            showReplyTimeoutFields();
            // 根据自动智能缓存状态更新配置显示（仅控制自动智能缓存相关的UI）
            updateSerialConfigForCacheMode();
            updateAutoTransparentCacheUI();

            try {
                const responseData = await fetchData('/mode_info');
                // 清空现有内容
                container.innerHTML = '';

                // 创建一个表格容器
                const tableResult = createModbusTableContainer();
                container.appendChild(tableResult.container);
                const tableContent = tableResult.tableContent;

                if (responseData && responseData.work_mode === 'modbus_cache') {
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
                                item.mapped_slave_addr || '',
                                item.mapped_register_addr || '',
                                item.timeout || '',
                                item.interval_time || '',
                                item.baud_rate || '',
                                item.data_bit || '',
                                item.check_bit || '0',
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
                        // 如果没有现有数据，添加一个默认行
                        const row = createModbusItemTemplate(1);
                        tableContent.appendChild(row);
                    }
                } else {
                    // 如果没有配置数据，添加一个默认行
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

                // 添加一个默认行
                const row = createModbusItemTemplate(1);
                tableContent.appendChild(row);
            }
        } else if (this.value === 'auto_collect') {
            // 自动采集模式：双通道手动缓存样式
            if (workModeContainer) {
                workModeContainer.classList.add('modbus-rtu-mode');
            }

            if (autoCacheControl) {
                autoCacheControl.style.display = 'none';
            }

            if (autoCollectConfig) {
                autoCollectConfig.style.display = 'block';
            }

            // 显示串口配置
            if (serialConfigCard) {
                serialConfigCard.style.display = 'block';
            }
            // 隐藏Modbus地址过滤/映射
            if (modbusFilterCard) modbusFilterCard.style.display = 'none';
            const modbusSlaveMapCardAc = document.getElementById('modbusSlaveMapCard');
            if (modbusSlaveMapCardAc) modbusSlaveMapCardAc.style.display = 'none';

            // 清空Modbus表格容器（此模式不复用）
            container.innerHTML = '';
            // 设置按钮位置与文案（自动采集模式暂不显示）
            // setModbusItemViews.appendChild(setModbusItemButton);
            // setModbusItemButton.querySelector('.btn-container').textContent = '配置';

            // 串口标签沿用主站/从站样式
            updateSerialTabsForModbus();
            refreshSerialTab();
            showReplyTimeoutFields();

            if (!autoCollectState.ch2.items || autoCollectState.ch2.items.length === 0) {
                autoCollectState.ch2.items = [createDefaultAutoCollectItem()];
            }
            if (!autoCollectState.ch3.items || autoCollectState.ch3.items.length === 0) {
                autoCollectState.ch3.items = [createDefaultAutoCollectItem()];
            }
            renderAutoCollectChannel('ch2');
            renderAutoCollectChannel('ch3');
        } else if (this.value === 'master_slave') {
            // 一主多从模式
            // 移除modbus-rtu-mode类，添加其他模式的样式
            if (workModeContainer) {
                workModeContainer.classList.remove('modbus-rtu-mode');
            }

            if (autoCacheControl) {
                autoCacheControl.style.display = 'none';
            }

            // 恢复TCPClient按钮
            if (tcpClientBtn) {
                tcpClientBtn.disabled = false;
            }
            // 确保串口设置可见
            if (serialConfigCard) serialConfigCard.style.display = 'block';
            // 隐藏Modbus地址过滤配置
            if (modbusFilterCard) modbusFilterCard.style.display = 'none';
            // 隐藏Modbus从机地址映射配置
            const modbusSlaveMapCard4 = document.getElementById('modbusSlaveMapCard');
            if (modbusSlaveMapCard4) modbusSlaveMapCard4.style.display = 'none';
            // 清空Modbus项容器
            container.innerHTML = '';
            // 移动设置按钮
            setModbusItemViews.appendChild(setModbusItemButton);
            // 更新按钮文本为"配置"
            setModbusItemButton.querySelector('.btn-container').textContent = '配置';
            // 更新串口标签显示为一主多从模式
            updateSerialTabsForMasterSlave();
            // 刷新串口标签状态
            refreshSerialTab();
            // 隐藏回复超时时间字段
            hideReplyTimeoutFields();
        } else {
            // 其他模式下恢复TCPClient按钮
            // 移除modbus-rtu-mode类，添加其他模式的样式
            if (workModeContainer) {
                workModeContainer.classList.remove('modbus-rtu-mode');
            }

            if (tcpClientBtn) {
                tcpClientBtn.disabled = false;
            }
            buttonGroup.style.display = 'none';
            serialConfigCard.style.display = 'block';

            if (autoCacheControl) {
                autoCacheControl.style.display = 'none';
            }
            // 隐藏Modbus地址过滤配置
            if (modbusFilterCard) modbusFilterCard.style.display = 'none';
            // 隐藏Modbus从机地址映射配置
            const modbusSlaveMapCard5 = document.getElementById('modbusSlaveMapCard');
            if (modbusSlaveMapCard5) modbusSlaveMapCard5.style.display = 'none';
            container.innerHTML = '';
            // 恢复默认串口标签显示
            updateSerialTabsForDefault();
            // 刷新串口标签状态
            refreshSerialTab();
            // 显示回复超时时间字段
            showReplyTimeoutFields();

            if (setModbusItemViews) {
                setModbusItemViews.appendChild(setModbusItemButton);
            } else {
                workModeContent.appendChild(setModbusItemButton);
            }
            // 更新按钮文本为"配置"
            setModbusItemButton.querySelector('.btn-container').textContent = '配置';
        }

        if (setModbusItemButton) {
            setModbusItemButton.style.display = this.value === 'auto_collect' ? 'none' : 'block';
        }
        workModeContent.style.maxHeight = workModeContent.scrollHeight + 'px';
        workModeContent.classList.remove('collapsed');
        document.getElementById('workModeToggle').classList.remove('collapsed');

        updateSlaveFollowAvailability();
    });
});

// 页面加载时检查是否已选择Modbus-RTU
document.addEventListener('DOMContentLoaded', () => {
    const modbusRtuRadio = document.querySelector('input[name="work_mode"][value="modbus_rtu"]');
    const tcpTransparentRadio = document.querySelector('input[name="work_mode"][value="tcp_transparent"]');
    const modbusTcpRadio = document.querySelector('input[name="work_mode"][value="modbus_tcp"]');
    const serialConfigContent = document.getElementById('serialConfigContent');
    const serialConfigCard = serialConfigContent ? serialConfigContent.closest('.card-view') : null;
    const modbusConfig = document.getElementById('modbus_rtu_config');
    const modbusFilterCard = document.getElementById('modbusFilterCard');
    const workModeContainer = document.querySelector('.work-mode-container');
    const autoCollectConfig = document.getElementById('auto_collect_config');

    // 默认隐藏Modbus-RTU配置
    if (modbusConfig) {
        modbusConfig.style.display = 'none';
    }

    // 默认隐藏Modbus地址过滤配置
    if (modbusFilterCard) {
        modbusFilterCard.style.display = 'none';
    }

    // 默认隐藏Modbus从机地址映射配置
    const modbusSlaveMapCard = document.getElementById('modbusSlaveMapCard');
    if (modbusSlaveMapCard) {
        modbusSlaveMapCard.style.display = 'none';
    }
    // 默认隐藏自动采集配置区域
    if (autoCollectConfig) {
        autoCollectConfig.style.display = 'none';
    }

    const autoCacheControl = document.getElementById('auto_cache_control');
    if (autoCacheControl) {
        autoCacheControl.style.display = 'none';
    }

    // 确保设置按钮存在
    let setModbusItemButton = document.getElementById('setModbusItem');
    const setModbusItemViews = document.getElementById('setModbusItemViews');

    if (!setModbusItemButton) {
        setModbusItemButton = document.createElement('button');
        setModbusItemButton.type = 'button';
        setModbusItemButton.id = 'setModbusItem';
        setModbusItemButton.className = 'main-btn';
        setModbusItemButton.innerHTML = '<div class="btn-container">配置</div>';
        setModbusItemButton.addEventListener('click', workModeSubmit);
    }
    if (setModbusItemViews && !setModbusItemViews.contains(setModbusItemButton)) {
        setModbusItemViews.appendChild(setModbusItemButton);
    }

    // 初始化串口标签页
    initializeSerialTabs();
    // 初始化自动采集表格（默认隐藏，但保持结构）
    renderAutoCollectChannel('ch2');
    renderAutoCollectChannel('ch3');

    // 首先获取服务器配置的工作模式
    workModeFetchData();

    // 加载Modbus地址过滤配置
    loadModbusFilterConfig();

    // 加载Modbus从机地址映射配置
    loadSlaveMapping();
});

// 更新串口标签显示文本
function updateSerialTabsText(ch1Text, ch2Text, ch3Text) {
    const serialTab1 = document.getElementById('serialTab1');
    const serialTab2 = document.getElementById('serialTab2');
    const serialTab3 = document.getElementById('serialTab3');

    if (serialTab1) serialTab1.textContent = ch1Text;
    if (serialTab2) serialTab2.textContent = ch2Text;
    if (serialTab3) serialTab3.textContent = ch3Text;
}

// 快捷函数
const updateSerialTabsForTransparent = () => updateSerialTabsText('CH1（主站）', 'CH2（主站）', 'CH3（从站）');
const updateSerialTabsForModbus = () => updateSerialTabsText('CH1 (主站)', 'CH2 (主站)', 'CH3 (从站)');
const updateSerialTabsForMasterSlave = () => updateSerialTabsText('CH1 (主站)', 'CH2 (从站)', 'CH3 (从站)');
const updateSerialTabsForDefault = () => updateSerialTabsText('CH1', 'CH2', 'CH3');


// 隐藏回复超时时间字段
function hideReplyTimeoutFields() {
    // 隐藏分别配置模式的回复超时时间字段
    for (let port = 1; port <= 3; port++) {
        const replyTimeoutField = document.getElementById(`reply_timeout_${port}`)?.closest('.label-view');
        if (replyTimeoutField) {
            replyTimeoutField.style.display = 'none';
        }
    }

    // 隐藏统一配置模式的回复超时时间字段
    const unifiedReplyTimeoutField = document.getElementById('unified_reply_timeout')?.closest('.label-view');
    if (unifiedReplyTimeoutField) {
        unifiedReplyTimeoutField.style.display = 'none';
    }
}

// 显示回复超时时间字段
function showReplyTimeoutFields() {
    // 显示分别配置模式的回复超时时间字段
    for (let port = 1; port <= 3; port++) {
        const replyTimeoutField = document.getElementById(`reply_timeout_${port}`)?.closest('.label-view');
        if (replyTimeoutField) {
            replyTimeoutField.style.display = 'flex';
        }
    }

    // 显示统一配置模式的回复超时时间字段
    const unifiedReplyTimeoutField = document.getElementById('unified_reply_timeout')?.closest('.label-view');
    if (unifiedReplyTimeoutField) {
        unifiedReplyTimeoutField.style.display = 'flex';
    }
}


// 刷新串口标签页到当前选中的端口
function refreshSerialTab() {
    if (typeof currentSerialPort !== 'undefined') {
        switchSerialTab(currentSerialPort);
    } else {
        switchSerialTab(1);
    }
}

// 初始化串口标签页
function initializeSerialTabs() {
    // 确保第一个标签页是激活状态
    switchSerialTab(1);

    // 添加标签页点击事件（如果还没有添加）
    const tab1 = document.getElementById('serialTab1');
    const tab2 = document.getElementById('serialTab2');
    const tab3 = document.getElementById('serialTab3');

    if (tab1 && !tab1.hasAttribute('data-initialized')) {
        tab1.addEventListener('click', () => switchSerialTab(1));
        tab1.setAttribute('data-initialized', 'true');
    }

    if (tab2 && !tab2.hasAttribute('data-initialized')) {
        tab2.addEventListener('click', () => switchSerialTab(2));
        tab2.setAttribute('data-initialized', 'true');
    }

    if (tab3 && !tab3.hasAttribute('data-initialized')) {
        tab3.addEventListener('click', () => switchSerialTab(3));
        tab3.setAttribute('data-initialized', 'true');
    }
}


// ==================== 自动采集配置（双通道） ====================
function createAutoCollectTableHeader() {
    const header = document.createElement('div');
    header.className = 'modbus-table-header';
    header.innerHTML = `
        <div class="modbus-table-cell modbus-checkbox-cell">选择</div>
        <div class="modbus-table-cell" style="max-width: 60px;">启用</div>
        <div class="modbus-table-cell" style="max-width: 60px;">序号</div>
        <div class="modbus-table-cell" style="max-width: 80px;">错误标记</div>
        <div class="modbus-table-cell" style="max-width: 100px;">错误清零次数</div>
        <div class="modbus-table-cell">从机地址</div>
        <div class="modbus-table-cell">功能码</div>
        <div class="modbus-table-cell">寄存器<br>地址</div>
        <div class="modbus-table-cell">映射寄存器<br>地址</div>
        <div class="modbus-table-cell">寄存器<br>数量</div>
        <div class="modbus-table-cell">采集间隔<br>(ms)</div>
        <div class="modbus-table-cell">接收超时<br>(ms)</div>
        <div class="modbus-table-cell">波特率</div>
        <div class="modbus-table-cell">数据位</div>
        <div class="modbus-table-cell">校验位</div>
        <div class="modbus-table-cell">停止位</div>
        <div class="modbus-table-cell">帧间隔<br>(ms)</div>
        <div class="modbus-table-cell">最大帧长<br>(bytes)</div>
    `;
    return header;
}

function createAutoCollectTableContainer() {
    const container = document.createElement('div');
    container.className = 'modbus-table-container';

    const bodyWrapper = document.createElement('div');
    bodyWrapper.className = 'modbus-table-body-wrapper';

    const tableContent = document.createElement('div');
    tableContent.className = 'modbus-table-content';

    tableContent.appendChild(createAutoCollectTableHeader());
    bodyWrapper.appendChild(tableContent);
    container.appendChild(bodyWrapper);

    return { container, tableContent };
}

function createAutoCollectRow(channel, index, item) {
    const row = document.createElement('div');
    row.className = 'modbus-table-row';
    row.dataset.index = index;
    row.dataset.enabled = item.enabled ? 'true' : 'false';
    const switchClass = item.enabled ? 'on' : 'off';

    const slaveAddr = item.real_slave_addr ?? 1;
    const fc = normalizeAutoCollectFunctionCode(item.function_code);
    const regAddr = item.register_addr ?? 0;
    const mappedRegAddr = item.mapped_register_addr ?? regAddr;
    const regNum = item.register_num ?? 1;
    const interval = item.interval_ms ?? 100;
    const timeout = item.timeout_ms ?? 1000;
    const baud = item.baudrate ?? 9600;
    const dataBits = item.data_bits ?? 8;
    const parity = item.parity ?? 0;
    const stopBits = item.stop_bits ?? 1;
    const frameTime = item.frame_time ?? 100;
    const frameLen = item.frame_len ?? 1024;
    const errorMarker = (item.error_marker !== null && item.error_marker !== undefined)
        ? item.error_marker
        : defaultAutoCollectErrorMarker(index);
    const errorClearCount = item.error_clear_count ?? 50;

    row.innerHTML = `
        <div class="modbus-table-cell modbus-checkbox-cell">
            <input type="checkbox" class="ac-row-checkbox" id="ac_${channel}_row_checkbox_${index}">
        </div>
        <div class="modbus-table-cell" style="max-width: 48px;">
            <div class="switch-btn ${switchClass}" onclick="toggleAutoCollectItem(this)">
                <div class="circle"></div>
            </div>
        </div>
        <div class="modbus-table-cell" style="max-width: 60px;">${index}</div>
        <div class="modbus-table-cell">
            <input type="number" class="base-input"
                   id="ac_${channel}_error_marker_${index}"
                   name="ac_${channel}_error_marker_${index}"
                   min="2" max="65535" value="${errorMarker}">
        </div>
        <div class="modbus-table-cell">
            <input type="number" class="base-input"
                   id="ac_${channel}_error_clear_count_${index}"
                   name="ac_${channel}_error_clear_count_${index}"
                   min="1" max="65535" value="${errorClearCount}">
        </div>
        <div class="modbus-table-cell">
            <input type="number" class="base-input"
                   id="ac_${channel}_slave_addr_${index}"
                   name="ac_${channel}_slave_addr_${index}"
                   min="1" max="247" value="${slaveAddr}">
        </div>
        <div class="modbus-table-cell">
            <select class="base-input"
                    id="ac_${channel}_function_code_${index}"
                    name="ac_${channel}_function_code_${index}"
                    onchange="updateAutoCollectFunctionCode(this)">
                <option value="01" ${fc == 1 || fc == '01' ? 'selected' : ''}>01</option>
                <option value="02" ${fc == 2 || fc == '02' ? 'selected' : ''}>02</option>
                <option value="03" ${fc == 3 || fc == '03' ? 'selected' : ''}>03</option>
                <option value="54" ${fc === 54 ? 'selected' : ''}>03&06</option>
                <option value="06" ${fc === 6 ? 'selected' : ''}>06</option>
                <option value="04" ${fc == 4 || fc == '04' ? 'selected' : ''}>04</option>
            </select>
        </div>
        <div class="modbus-table-cell">
            <input type="number" class="base-input"
                   id="ac_${channel}_register_addr_${index}"
                   name="ac_${channel}_register_addr_${index}"
                   min="0" max="65535" value="${regAddr}">
        </div>
        <div class="modbus-table-cell">
            <input type="number" class="base-input"
                   id="ac_${channel}_mapped_register_addr_${index}"
                   name="ac_${channel}_mapped_register_addr_${index}"
                   min="0" max="65535" value="${mappedRegAddr}">
        </div>
        <div class="modbus-table-cell">
            <input type="number" class="base-input"
                   id="ac_${channel}_register_num_${index}"
                   name="ac_${channel}_register_num_${index}"
                   min="1" max="${fc === 6 ? 1 : 125}" value="${fc === 6 ? 1 : regNum}">
        </div>
        <div class="modbus-table-cell">
            <input type="number" class="base-input"
                   id="ac_${channel}_interval_${index}"
                   name="ac_${channel}_interval_${index}"
                   min="10" value="${interval}">
        </div>
        <div class="modbus-table-cell">
            <input type="number" class="base-input"
                   id="ac_${channel}_timeout_${index}"
                   name="ac_${channel}_timeout_${index}"
                   min="10" value="${timeout}">
        </div>
        <div class="modbus-table-cell">
            <select class="base-input"
                    id="ac_${channel}_baud_rate_${index}"
                    name="ac_${channel}_baud_rate_${index}">
                <option value="1200" ${baud == 1200 ? 'selected' : ''}>1200</option>
                <option value="2400" ${baud == 2400 ? 'selected' : ''}>2400</option>
                <option value="4800" ${baud == 4800 ? 'selected' : ''}>4800</option>
                <option value="9600" ${baud == 9600 ? 'selected' : ''}>9600</option>
                <option value="19200" ${baud == 19200 ? 'selected' : ''}>19200</option>
                <option value="38400" ${baud == 38400 ? 'selected' : ''}>38400</option>
                <option value="57600" ${baud == 57600 ? 'selected' : ''}>57600</option>
                <option value="115200" ${baud == 115200 ? 'selected' : ''}>115200</option>
            </select>
        </div>
        <div class="modbus-table-cell">
            <select class="base-input"
                    id="ac_${channel}_data_bit_${index}"
                    name="ac_${channel}_data_bit_${index}">
                <option value="8" ${dataBits == 8 ? 'selected' : ''}>8</option>
                <option value="7" ${dataBits == 7 ? 'selected' : ''}>7</option>
                <option value="6" ${dataBits == 6 ? 'selected' : ''}>6</option>
                <option value="5" ${dataBits == 5 ? 'selected' : ''}>5</option>
            </select>
        </div>
        <div class="modbus-table-cell">
            <select class="base-input"
                    id="ac_${channel}_check_bit_${index}"
                    name="ac_${channel}_check_bit_${index}">
                <option value="0" ${parity == 0 ? 'selected' : ''}>无校验</option>
                <option value="1" ${parity == 1 ? 'selected' : ''}>奇校验</option>
                <option value="2" ${parity == 2 ? 'selected' : ''}>偶校验</option>
            </select>
        </div>
        <div class="modbus-table-cell">
            <select class="base-input"
                    id="ac_${channel}_stop_bit_${index}"
                    name="ac_${channel}_stop_bit_${index}">
                <option value="1" ${stopBits == 1 ? 'selected' : ''}>1位停止位</option>
                <option value="2" ${stopBits == 2 ? 'selected' : ''}>2位停止位</option>
            </select>
        </div>
        <div class="modbus-table-cell">
            <input type="number" class="base-input"
                   id="ac_${channel}_frame_time_${index}"
                   name="ac_${channel}_frame_time_${index}"
                   min="1" value="${frameTime}">
        </div>
        <div class="modbus-table-cell">
            <input type="number" class="base-input"
                   id="ac_${channel}_frame_len_${index}"
                   name="ac_${channel}_frame_len_${index}"
                   min="1" value="${frameLen}">
        </div>
    `;

    if (!item.enabled) {
        row.querySelectorAll('.base-input').forEach(input => input.disabled = true);
    }

    return row;
}

function renderAutoCollectChannel(channel) {
    const container = document.getElementById(`ac_${channel}_items_container`);
    if (!container) return;

    container.innerHTML = '';
    const { container: tableContainer, tableContent } = createAutoCollectTableContainer();
    container.appendChild(tableContainer);

    const items = (autoCollectState[channel].items && autoCollectState[channel].items.length > 0)
        ? autoCollectState[channel].items
        : [createDefaultAutoCollectItem()];

    autoCollectState[channel].items = items;

    items.forEach((item, idx) => {
        const row = createAutoCollectRow(channel, idx + 1, item);
        tableContent.appendChild(row);
    });

    const workModeContent = document.getElementById('workModeContent');
    if (workModeContent && !workModeContent.classList.contains('collapsed')) {
        workModeContent.style.maxHeight = workModeContent.scrollHeight + 'px';
    }
}

function toggleAutoCollectItem(switchBtn) {
    const row = switchBtn.closest('.modbus-table-row');
    const inputs = row.querySelectorAll('.base-input');
    const enabled = switchBtn.classList.contains('on');

    if (enabled) {
        switchBtn.classList.remove('on');
        switchBtn.classList.add('off');
        row.dataset.enabled = 'false';
        inputs.forEach(input => input.disabled = true);
    } else {
        switchBtn.classList.remove('off');
        switchBtn.classList.add('on');
        row.dataset.enabled = 'true';
        inputs.forEach(input => input.disabled = false);
    }
}

function addAutoCollectItem(channel) {
    const current = collectAutoChannelConfig(channel);
    const stateItems = current.items ? [...current.items] : [];
    if (stateItems.length >= AUTO_COLLECT_MAX_ITEMS) {
        showCustomAlert(`每个通道最多配置${AUTO_COLLECT_MAX_ITEMS}条`, true);
        return;
    }
    stateItems.push(createDefaultAutoCollectItem());
    autoCollectState[channel].items = stateItems;
    renderAutoCollectChannel(channel);
}

function removeAutoCollectSelected(channel) {
    const container = document.getElementById(`ac_${channel}_items_container`);
    if (!container) return;
    const rows = container.querySelectorAll('.modbus-table-row:not(.modbus-table-header)');
    const checked = container.querySelectorAll('.ac-row-checkbox:checked');
    const current = collectAutoChannelConfig(channel);

    if (checked.length === 0) {
        showCustomAlert('请选择要删除的条目', true);
        return;
    }
    if (rows.length - checked.length < 1) {
        showCustomAlert('至少保留一条配置', true);
        return;
    }

    const remain = [];
    rows.forEach((row, idx) => {
        if (!row.querySelector('.ac-row-checkbox').checked) {
            const item = current.items ? current.items[idx] : null;
            if (item) remain.push(item);
        }
    });
    autoCollectState[channel].items = remain;
    renderAutoCollectChannel(channel);
}

function collectAutoChannelConfig(channel) {
    const mappedInput = document.getElementById(`ac_${channel}_maddr`);
    const exceptionSwitch = document.getElementById(`ac_${channel}_exception_switch`);
    const fallbackMappedAddr = channel === 'ch3' ? 3 : 2;
    const mapped_slave_addr = mappedInput ? (parseInt(mappedInput.value) || fallbackMappedAddr) : fallbackMappedAddr;
    const allow_exception_response = exceptionSwitch
        ? exceptionSwitch.classList.contains('on')
        : false;
    const container = document.getElementById(`ac_${channel}_items_container`);
    const rows = container ? container.querySelectorAll('.modbus-table-row:not(.modbus-table-header)') : [];
    const items = [];

    const safeNumber = (val, def) => {
        const num = parseInt(val);
        return isNaN(num) ? def : num;
    };

    rows.forEach((row, idx) => {
        const index = idx + 1;
        const enabled = row.dataset.enabled === 'true';
        const getValue = (id, def) => {
            const el = row.querySelector(id);
            return el ? el.value : def;
        };

        items.push({
            enabled,
            real_slave_addr: safeNumber(getValue(`#ac_${channel}_slave_addr_${index}`, 1), 1),
            function_code: normalizeAutoCollectFunctionCode(
                getValue(`#ac_${channel}_function_code_${index}`, 3)
            ),
            register_addr: safeNumber(getValue(`#ac_${channel}_register_addr_${index}`, 0), 0),
            mapped_register_addr: safeNumber(getValue(`#ac_${channel}_mapped_register_addr_${index}`, getValue(`#ac_${channel}_register_addr_${index}`, 0)), 0),
            register_num: safeNumber(getValue(`#ac_${channel}_register_num_${index}`, 1), 1),
            interval_ms: safeNumber(getValue(`#ac_${channel}_interval_${index}`, 100), 100),
            timeout_ms: safeNumber(getValue(`#ac_${channel}_timeout_${index}`, 1000), 1000),
            baudrate: safeNumber(getValue(`#ac_${channel}_baud_rate_${index}`, 9600), 9600),
            data_bits: safeNumber(getValue(`#ac_${channel}_data_bit_${index}`, 8), 8),
            parity: safeNumber(getValue(`#ac_${channel}_check_bit_${index}`, 0), 0),
            stop_bits: safeNumber(getValue(`#ac_${channel}_stop_bit_${index}`, 1), 1),
            frame_time: safeNumber(getValue(`#ac_${channel}_frame_time_${index}`, 100), 100),
            frame_len: safeNumber(getValue(`#ac_${channel}_frame_len_${index}`, 1024), 1024),
            error_marker: safeNumber(
                getValue(`#ac_${channel}_error_marker_${index}`, defaultAutoCollectErrorMarker(index)),
                defaultAutoCollectErrorMarker(index)
            ),
            error_clear_count: Math.max(
                1,
                safeNumber(getValue(`#ac_${channel}_error_clear_count_${index}`, 50), 50)
            )
        });
    });

    if (items.length === 0) {
        items.push(createDefaultAutoCollectItem());
    }

    autoCollectState[channel].mapped_slave_addr = mapped_slave_addr;
    autoCollectState[channel].allow_exception_response = allow_exception_response;
    autoCollectState[channel].items = items;

    return { mapped_slave_addr, allow_exception_response, items };
}

function populateAutoCollectConfig(data) {
    const ch2Input = document.getElementById('ac_ch2_maddr');
    const ch3Input = document.getElementById('ac_ch3_maddr');

    const normalizeItems = (items) => {
        if (!Array.isArray(items) || items.length === 0) return [createDefaultAutoCollectItem()];
        return items.map((it, idx) => ({
            enabled: it.enabled !== false,
            real_slave_addr: it.real_slave_addr ?? it.slave_addr ?? 1,
            function_code: normalizeAutoCollectFunctionCode(it.function_code),
            register_addr: it.register_addr ?? 0,
            mapped_register_addr: it.mapped_register_addr ?? it.register_addr ?? 0,
            register_num: it.register_num ?? 1,
            interval_ms: it.interval_ms ?? it.interval_time ?? 100,
            timeout_ms: it.timeout_ms ?? it.timeout ?? 1000,
            baudrate: it.baudrate ?? it.baud_rate ?? 9600,
            data_bits: it.data_bits ?? it.data_bit ?? 8,
            parity: it.parity ?? it.check_bit ?? 0,
            stop_bits: it.stop_bits ?? it.stop_bit ?? 1,
            frame_time: it.frame_time ?? 100,
            frame_len: it.frame_len ?? 1024,
            error_marker: Number(it.error_marker) >= 2
                ? Number(it.error_marker)
                : defaultAutoCollectErrorMarker(idx + 1),
            error_clear_count: it.error_clear_count ?? 50
        }));
    };

    const boolValue = value => value === true || value === 1 || value === '1' || value === 'true';
    autoCollectState.ch2.mapped_slave_addr = data?.ch2?.mapped_slave_addr || 2;
    autoCollectState.ch3.mapped_slave_addr = data?.ch3?.mapped_slave_addr || 3;
    autoCollectState.ch2.allow_exception_response = boolValue(data?.ch2?.allow_exception_response);
    autoCollectState.ch3.allow_exception_response = boolValue(data?.ch3?.allow_exception_response);
    autoCollectState.ch2.items = normalizeItems(data?.ch2?.items);
    autoCollectState.ch3.items = normalizeItems(data?.ch3?.items);

    if (ch2Input) ch2Input.value = autoCollectState.ch2.mapped_slave_addr;
    if (ch3Input) ch3Input.value = autoCollectState.ch3.mapped_slave_addr;
    setAutoCollectExceptionSwitch('ch2', autoCollectState.ch2.allow_exception_response);
    setAutoCollectExceptionSwitch('ch3', autoCollectState.ch3.allow_exception_response);

    renderAutoCollectChannel('ch2');
    renderAutoCollectChannel('ch3');
}

// 导出自动采集配置
function exportAutoCollectConfig() {
    const payload = {
        work_mode: 'auto_collect',
        ch2: collectAutoChannelConfig('ch2'),
        ch3: collectAutoChannelConfig('ch3')
    };
    const jsonData = JSON.stringify(payload, null, 2);
    const blob = new Blob([jsonData], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const downloadLink = document.createElement('a');
    const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
    downloadLink.href = url;
    downloadLink.download = `auto_collect_config_${timestamp}.json`;
    document.body.appendChild(downloadLink);
    downloadLink.click();
    document.body.removeChild(downloadLink);
    URL.revokeObjectURL(url);
    displaySuccessMessage('自动采集配置导出成功');
}

// 导入自动采集配置
function importAutoCollectConfig() {
    const fileInput = document.createElement('input');
    fileInput.type = 'file';
    fileInput.accept = '.json';
    fileInput.style.display = 'none';
    document.body.appendChild(fileInput);

    fileInput.onchange = function (event) {
        const file = event.target.files[0];
        if (!file) {
            document.body.removeChild(fileInput);
            return;
        }

        const reader = new FileReader();
        reader.onload = function (e) {
            try {
                const cfg = JSON.parse(e.target.result);
                if (!cfg || typeof cfg !== 'object') throw new Error('无效的配置文件');
                if (cfg.work_mode && cfg.work_mode !== 'auto_collect') {
                    throw new Error('配置文件工作模式不是auto_collect');
                }
                populateAutoCollectConfig(cfg);
                displaySuccessMessage('自动采集配置导入成功');
            } catch (err) {
                showCustomAlert('导入失败: ' + err.message, true);
            }
            document.body.removeChild(fileInput);
        };
        reader.onerror = function () {
            showCustomAlert('读取文件失败', true);
            document.body.removeChild(fileInput);
        };
        reader.readAsText(file);
    };

    fileInput.click();
}

// 保存自动采集配置到后端
async function saveAutoCollectConfig() {
    const payload = {
        ch2: collectAutoChannelConfig('ch2'),
        ch3: collectAutoChannelConfig('ch3')
    };

    if (Number(payload.ch2.mapped_slave_addr) === Number(payload.ch3.mapped_slave_addr)) {
        showCustomAlert('CH2和CH3的映射从机地址不能相同，请修改后再保存', true);
        return false;
    }

    // 重叠检查：同一通道的映射寄存器范围是否冲突
    const hasOverlap = (cfg) => {
        if (!cfg || !cfg.items || cfg.items.length === 0) return false;
        const ranges = cfg.items
            .filter(it => it.enabled !== false)
            .map(it => {
                const start = Number(it.mapped_register_addr) || 0;
                const len = Number(it.register_num) || 0;
                return { start, end: start + len - 1 };
            })
            .filter(r => r.end >= r.start);
        for (let i = 0; i < ranges.length; i++) {
            for (let j = i + 1; j < ranges.length; j++) {
                const a = ranges[i], b = ranges[j];
                if (!(a.end < b.start || b.end < a.start)) {
                    return true;
                }
            }
        }
        return false;
    };
    if (hasOverlap(payload.ch2) || hasOverlap(payload.ch3)) {
        showCustomAlert('检测到映射寄存器区间重叠，请调整后再保存', true);
        return false;
    }

    const checkVirtualRange = (cfg, channelName) => {
        if (!cfg || !cfg.items || cfg.items.length === 0) return null;
        for (let i = 0; i < cfg.items.length; i++) {
            const it = cfg.items[i];
            if (it.enabled === false) continue;
            const mappedValue = Number(it.mapped_register_addr);
            const hasMappedValue = it.mapped_register_addr !== null
                && it.mapped_register_addr !== undefined
                && it.mapped_register_addr !== ''
                && Number.isFinite(mappedValue);
            const fallbackStart = Number(it.register_addr) || 0;
            const regStart = hasMappedValue ? mappedValue : fallbackStart;
            const count = Number(it.register_num) || 0;
            if (count <= 0) {
                return `${channelName} 第${i + 1}条寄存器数量无效`;
            }
            const errorMarker = Number(it.error_marker);
            if (!Number.isInteger(errorMarker) || errorMarker < 2 || errorMarker > 65535) {
                return `${channelName} 第${i + 1}条错误标记必须在2-65535范围内`;
            }
            const end = regStart + count - 1;
            if (end > AUTO_COLLECT_MAX_VIRTUAL_REG) {
                return `${channelName} 第${i + 1}条虚拟寄存器范围超出${AUTO_COLLECT_MAX_VIRTUAL_REG}，${AUTO_COLLECT_MAX_VIRTUAL_REG + 1}-${AUTO_COLLECT_MAX_VIRTUAL_REG + 60}用于错误标志位 (start=${regStart}, count=${count}, end=${end})`;
            }
        }
        return null;
    };

    const rangeError =
        checkVirtualRange(payload.ch2, 'CH2') ||
        checkVirtualRange(payload.ch3, 'CH3');
    if (rangeError) {
        showCustomAlert(rangeError, true);
        return false;
    }

    try {
        const resp = await postData('/auto_collect_set', payload);
        if (resp && resp.code === 200) {
            displaySuccessMessage('自动采集配置保存成功');
            return true;
        } else {
            throw new Error(resp && resp.msg ? resp.msg : '保存失败');
        }
    } catch (err) {
        showCustomAlert('保存失败: ' + err.message, true);
        return false;
    }
}

async function applyAutoCollectConfigAndRestart() {
    const saved = await saveAutoCollectConfig();
    if (!saved) {
        return;
    }

    pendingRestartAction = async () => {
        await fetchData('/operate');
        displaySuccessMessage('重启中，请稍后...');
        setTimeout(() => location.reload(), 5000);
    };

    const deviceRestart = document.getElementById('deviceRestart');
    if (deviceRestart) {
        showSvg('deviceRestart', 6000);
    } else if (confirm('是否确定重启？')) {
        await pendingRestartAction();
        pendingRestartAction = null;
    }
}
// ==================== 自动采集配置结束 ====================


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
        <div class="modbus-table-cell">
            <input type="text" class="base-input" placeholder="十进制"
                   id="slave_addr_${index}"
                   name="slave_addr_${index}"
                   value="01"
                   title="真实从机设备地址">
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
            <input type="text" class="base-input" placeholder="十六进制"
                   id="register_addr_${index}"
                   name="register_addr_${index}"
                   value="00"
                   title="真实寄存器起始地址">
        </div>
        <div class="modbus-table-cell">
            <input type="text" class="base-input" placeholder="十进制"
                   id="register_num_${index}"
                   name="register_num_${index}"
                   value="10">
        </div>
        <div class="modbus-table-cell">
            <input type="text" class="base-input" placeholder="十进制"
                   id="mapped_slave_addr_${index}"
                   name="mapped_slave_addr_${index}"
                   value="01"
                   title="映射到虚拟从机地址">
        </div>
        <div class="modbus-table-cell">
            <input type="text" class="base-input" placeholder="十六进制"
                   id="mapped_register_addr_${index}"
                   name="mapped_register_addr_${index}"
                   value="00"
                   title="映射到虚拟寄存器地址">
        </div>
        <div class="modbus-table-cell">
            <input type="text" class="base-input" placeholder="单位毫秒"
                   id="timeout_${index}"
                   name="timeout_${index}"
                   value="500">
        </div>

        <div class="modbus-table-cell">
            <input type="text" class="base-input" placeholder="单位毫秒"
                   id="interval_time_${index}"
                   name="interval_time_${index}"
                   value="100">
        </div>

        <div class="modbus-table-cell">
            <select class="base-input"
                    id="modbus_baud_rate_${index}"
                    name="modbus_baud_rate_${index}">
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
                    id="modbus_data_bit_${index}"
                    name="modbus_data_bit_${index}">
                <option value="8">8</option>
                <option value="7">7</option>
                <option value="6">6</option>
                <option value="5">5</option>
            </select>
        </div>
        <div class="modbus-table-cell">
            <select class="base-input"
                    id="modbus_check_bit_${index}"
                    name="modbus_check_bit_${index}">
                <option value="0">无校验</option>
                <option value="1">奇校验</option>
                <option value="2">偶校验</option>
            </select>
        </div>
        <div class="modbus-table-cell">
            <select class="base-input"
                    id="modbus_stop_bit_${index}"
                    name="modbus_stop_bit_${index}">
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
        <div class="modbus-table-cell" style="max-width: 60px;">启用</div>
        <div class="modbus-table-cell">从机地址</div>
        <div class="modbus-table-cell">功能码</div>
        <div class="modbus-table-cell">寄存器<br>地址</div>
        <div class="modbus-table-cell">寄存器<br>数量</div>
        <div class="modbus-table-cell">映射从机<br>地址</div>
        <div class="modbus-table-cell">映射寄存器<br>地址</div>
        <div class="modbus-table-cell">接收超时<br>(ms)</div>
        <div class="modbus-table-cell">间隔时间<br>(ms)</div>
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

// 自动采集按钮的事件处理
const acCh2AddBtn = document.getElementById('ac_ch2_add_item');
if (acCh2AddBtn) {
    acCh2AddBtn.addEventListener('click', () => addAutoCollectItem('ch2'));
}
const acCh3AddBtn = document.getElementById('ac_ch3_add_item');
if (acCh3AddBtn) {
    acCh3AddBtn.addEventListener('click', () => addAutoCollectItem('ch3'));
}
const acCh2RemoveBtn = document.getElementById('ac_ch2_remove_item');
if (acCh2RemoveBtn) {
    acCh2RemoveBtn.addEventListener('click', () => removeAutoCollectSelected('ch2'));
}
const acCh3RemoveBtn = document.getElementById('ac_ch3_remove_item');
if (acCh3RemoveBtn) {
    acCh3RemoveBtn.addEventListener('click', () => removeAutoCollectSelected('ch3'));
}
const acImportBtn = document.getElementById('ac_import_btn');
if (acImportBtn) {
    acImportBtn.addEventListener('click', importAutoCollectConfig);
}
const acExportBtn = document.getElementById('ac_export_btn');
if (acExportBtn) {
    acExportBtn.addEventListener('click', exportAutoCollectConfig);
}
const acSaveBtn = document.getElementById('ac_save_btn');
if (acSaveBtn) {
    acSaveBtn.addEventListener('click', applyAutoCollectConfigAndRestart);
}

// 添加按钮的事件处理
document.getElementById('addModbusItem').addEventListener('click', function () {
    const container = document.getElementById('modbus_items_container');
    const tableContainer = container.querySelector('.modbus-table-container');
    const tableContent = tableContainer.querySelector('.modbus-table-content');
    const rows = tableContent.querySelectorAll('.modbus-table-row:not(.modbus-table-header)');

    if (rows.length >= MAX_MODBUS_ITEMS) {
        showCustomAlert(`最多只能添加${MAX_MODBUS_ITEMS}个配置项`);
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

// 保存Modbus配置到NVS并应用到工作模式
async function saveModbusConfig() {
    // 检查是否处于Modbus Cache配置的模式
    const workMode = document.querySelector('input[name="work_mode"]:checked').value;
    if (workMode !== 'modbus_cache') {
        showCustomAlert('只有在Modbus Cache模式下才能保存配置', true);
        return;
    }

    // 检查是否启用了自动智能缓存
    if (autoTransparentCacheEnabled) {
        showCustomAlert('当前为自动智能缓存模式，无需手动保存配置', true);
        return;
    }

    try {
        // 收集当前的Modbus配置数据，使用与workModeSubmit相同的逻辑
        const data = {
            work_mode: 'modbus_cache',
            auto_transparent_cache: false
        };



        // 收集Modbus配置项
        const modbusItems = [];
        const container = document.getElementById('modbus_items_container');
        const tableContainer = container.querySelector('.modbus-table-container');

        if (tableContainer) {
            const tableContent = tableContainer.querySelector('.modbus-table-content');
            if (tableContent) {
                const rows = tableContent.querySelectorAll('.modbus-table-row:not(.modbus-table-header)');

                if (rows.length === 0) {
                    showCustomAlert('没有可保存的配置项', true);
                    return;
                }

                Array.from(rows).forEach((row, index) => {
                    const itemIndex = index + 1;
                    modbusItems.push({
                        enabled: row.dataset.enabled === 'true',
                        slave_addr: row.querySelector(`#slave_addr_${itemIndex}`).value || '1',
                        function_code: row.querySelector(`#function_code_${itemIndex}`).value || '03',
                        register_addr: row.querySelector(`#register_addr_${itemIndex}`).value || '0',
                        register_num: row.querySelector(`#register_num_${itemIndex}`).value || '1',
                        mapped_slave_addr: row.querySelector(`#mapped_slave_addr_${itemIndex}`).value || '1',
                        mapped_register_addr: row.querySelector(`#mapped_register_addr_${itemIndex}`).value || '0',
                        timeout: row.querySelector(`#timeout_${itemIndex}`).value || '1000',
                        interval_time: row.querySelector(`#interval_time_${itemIndex}`).value || '1000',
                        baud_rate: row.querySelector(`#modbus_baud_rate_${itemIndex}`).value || '9600',
                        data_bit: row.querySelector(`#modbus_data_bit_${itemIndex}`).value || '8',
                        check_bit: row.querySelector(`#modbus_check_bit_${itemIndex}`).value || '0',
                        stop_bit: row.querySelector(`#modbus_stop_bit_${itemIndex}`).value || '1'
                    });
                });

                data.modbus_items = modbusItems;
            }
        }

        console.log('保存Modbus配置:', data);

        // 发送配置到服务器
        const response = await fetch('/mode_set', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(data)
        });

        if (response.ok) {
            const result = await response.json();
            if (result.code === 200) {
                displaySuccessMessage('Modbus配置保存成功并已应用到工作模式');
                console.log('Modbus配置保存成功');
            } else {
                throw new Error(result.msg || '保存失败');
            }
        } else {
            throw new Error(`HTTP错误: ${response.status}`);
        }
    } catch (error) {
        console.error('保存Modbus配置失败:', error);
        showCustomAlert('保存配置失败: ' + error.message, true);
    }
}

async function workModeSubmit() {
    const workMode = document.querySelector('input[name="work_mode"]:checked').value;
    const data = {
        work_mode: workMode
    };

    // 处理 modbus_cache 模式的配置
    if (workMode === 'modbus_cache') {
        // 添加自动智能缓存开关状态
        data.auto_transparent_cache = autoTransparentCacheEnabled;

        // 如果是手动缓存模式，收集Modbus配置项
        if (!autoTransparentCacheEnabled) {
            const modbusItems = [];
            const container = document.getElementById('modbus_items_container');
            const tableContainer = container.querySelector('.modbus-table-container');

            if (tableContainer) {
                const tableContent = tableContainer.querySelector('.modbus-table-content');
                if (tableContent) {
                    const rows = tableContent.querySelectorAll('.modbus-table-row:not(.modbus-table-header)');

                    Array.from(rows).forEach((row, index) => {
                        const itemIndex = index + 1;
                        
                        // 安全获取元素值的辅助函数
                        const safeGetValue = (selector, defaultValue) => {
                            const element = row.querySelector(selector);
                            if (!element) {
                                console.error(`Element not found: ${selector} in row ${itemIndex}`);
                                return defaultValue;
                            }
                            return element.value || defaultValue;
                        };
                        
                        modbusItems.push({
                            enabled: row.dataset.enabled === 'true',
                            slave_addr: safeGetValue(`#slave_addr_${itemIndex}`, '1'),
                            function_code: safeGetValue(`#function_code_${itemIndex}`, '03'),
                            register_addr: safeGetValue(`#register_addr_${itemIndex}`, '0'),
                            register_num: safeGetValue(`#register_num_${itemIndex}`, '1'),
                            mapped_slave_addr: safeGetValue(`#mapped_slave_addr_${itemIndex}`, '1'),
                            mapped_register_addr: safeGetValue(`#mapped_register_addr_${itemIndex}`, '0'),
                            timeout: safeGetValue(`#timeout_${itemIndex}`, '1000'),
                            interval_time: safeGetValue(`#interval_time_${itemIndex}`, '1000'),
                            baud_rate: safeGetValue(`#modbus_baud_rate_${itemIndex}`, '9600'),
                            data_bit: safeGetValue(`#modbus_data_bit_${itemIndex}`, '8'),
                            check_bit: safeGetValue(`#modbus_check_bit_${itemIndex}`, '0'),
                            stop_bit: safeGetValue(`#modbus_stop_bit_${itemIndex}`, '1')
                        });
                    });

                    data.modbus_items = modbusItems;
                }
            }
        }

        console.log('Modbus缓存模式配置:', {
            auto_transparent_cache: data.auto_transparent_cache,
            modbus_items_count: data.modbus_items ? data.modbus_items.length : 0
        });
    } else if (workMode === 'modbus_rtu') {
        const modbusItems = [];
        const container = document.getElementById('modbus_items_container');
        const tableContainer = container.querySelector('.modbus-table-container');
        const tableContent = tableContainer.querySelector('.modbus-table-content');
        const rows = tableContent.querySelectorAll('.modbus-table-row:not(.modbus-table-header)');

        Array.from(rows).forEach((row, index) => {
            const itemIndex = index + 1;
            // 保存所有配置项，并添加enabled标记
            modbusItems.push({
                enabled: row.dataset.enabled === 'true', // 添加启用状态标记
                slave_addr: row.querySelector(`#slave_addr_${itemIndex}`).value || '1',
                function_code: row.querySelector(`#function_code_${itemIndex}`).value || '01',
                register_addr: row.querySelector(`#register_addr_${itemIndex}`).value || '0',
                register_num: row.querySelector(`#register_num_${itemIndex}`).value || '1',
                timeout: row.querySelector(`#timeout_${itemIndex}`).value || '1000',
                interval_time: row.querySelector(`#interval_time_${itemIndex}`).value || '1000',
                baud_rate: row.querySelector(`#modbus_baud_rate_${itemIndex}`).value || '9600',
                data_bit: row.querySelector(`#modbus_data_bit_${itemIndex}`).value || '8',
                check_bit: convertParityToBackend(row.querySelector(`#modbus_check_bit_${itemIndex}`).value || 'None'),
                stop_bit: row.querySelector(`#modbus_stop_bit_${itemIndex}`).value || '1'
            });
        });

        data.modbus_items = modbusItems;
    } else if (workMode === 'auto_collect') {
        // 自动采集模式下，“配置”仅设置工作模式，采集条目由独立按钮保存
    }

    try {
        const result = await postData('/mode_set', data);
        console.log('工作模式配置成功:', result);
        document.getElementById('setModbusItem').textContent = '配置成功';

        // 根据工作模式决定是否显示重启提示框
        if (workMode === 'transparent_queue' || workMode === 'modbus_queue' || workMode === 'modbus_cache' || workMode === 'master_slave' || workMode === 'auto_collect') {
            // 所有工作模式配置成功后都显示重启提示
            const deviceRestart = document.getElementById('deviceRestart');
            if (deviceRestart) {
                showSvg('deviceRestart', 6000);
            }
        }
    } catch (error) {
        console.error('工作模式配置失败:', error);
        document.getElementById('setModbusItem').textContent = '配置失败';

        // 显示详细错误信息
        let errorMsg = "配置失败";
        if (error.message) {
            errorMsg = error.message;
        }

        // 创建错误提示元素
        const errorDiv = document.createElement('div');
        errorDiv.style.cssText = `
            position: fixed;
            top: 20px;
            right: 20px;
            background: #ff4444;
            color: white;
            padding: 15px;
            border-radius: 5px;
            z-index: 1000;
            max-width: 300px;
            word-wrap: break-word;
        `;
        errorDiv.textContent = `工作模式配置失败: ${errorMsg}`;
        document.body.appendChild(errorDiv);

        // 3秒后自动移除错误提示
        setTimeout(() => {
            if (errorDiv.parentNode) {
                errorDiv.parentNode.removeChild(errorDiv);
            }
        }, 3000);
    }
    setTimeout(() => {
        const currentMode = document.querySelector('input[name="work_mode"]:checked').value;
        const buttonText = (currentMode === 'modbus_cache' || currentMode === 'auto_collect') ? '配置' : '设置';
        document.getElementById('setModbusItem').textContent = buttonText;
    }, 2000);
}

// 页面加载时获取工作模式配置
async function workModeFetchData() {
    try {
        const responseData = await fetchData('/mode_info');
        const modbusConfig = document.getElementById('modbus_rtu_config');

        console.log('Response from server:', responseData);


    if (responseData) {
        // 设置工作模式
        console.log('服务器返回的工作模式:', responseData.work_mode);
            // 修复：ID应该与输入字段ID匹配，而不是value匹配
            if (responseData.work_mode === 'modbus_queue') {
                const modbusTcpRadio = document.getElementById('modbus_tcp');
                if (modbusTcpRadio) {
                    console.log('找到modbus_tcp按钮，设置为选中');
                    applyWorkModeSelection(modbusTcpRadio);
                } else {
                    console.warn('未找到modbus_tcp按钮');
                }
            } else if (responseData.work_mode === 'transparent_queue') {
                const tcpTransparentRadio = document.getElementById('tcp_transparent_mode');
                if (tcpTransparentRadio) {
                    console.log('找到tcp_transparent按钮，设置为选中');
                    applyWorkModeSelection(tcpTransparentRadio);
                }
            } else if (responseData.work_mode === 'modbus_cache') {
                const modbusRtuRadio = document.querySelector('input[name="work_mode"][value="modbus_cache"]');
                if (modbusRtuRadio) {
                    console.log('找到modbus_cache按钮，设置为选中');

                    // 加载自动智能缓存开关状态
                    if (responseData.hasOwnProperty('auto_transparent_cache')) {
                        console.log('从服务器加载自动智能缓存状态:', responseData.auto_transparent_cache);
                        applyAutoTransparentCacheState(responseData.auto_transparent_cache, { skipBackendSync: true });
                    }

                    applyWorkModeSelection(modbusRtuRadio);
                }
            } else if (responseData.work_mode === 'master_slave') {
                const masterSlaveRadio = document.getElementById('master_slave_mode');
                if (masterSlaveRadio) {
                    console.log('找到master_slave按钮，设置为选中');
                    applyWorkModeSelection(masterSlaveRadio);
                }
            } else if (responseData.work_mode === 'auto_collect') {
                const autoCollectRadio = document.getElementById('auto_collect_mode');
                if (autoCollectRadio) {
                    console.log('找到auto_collect按钮，设置为选中');
                    applyWorkModeSelection(autoCollectRadio);
                    // 回填自动采集配置
                    populateAutoCollectConfig(responseData);
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
                    applyWorkModeSelection(checkedMode);
                }
            }
        } else {
            // 保持当前选中的工作模式不变
            const checkedMode = document.querySelector('input[name="work_mode"]:checked');
            if (checkedMode) {
                applyWorkModeSelection(checkedMode);
            }
        }
    } catch (error) {
        console.error('Failed to fetch work mode data:', error);
        // 保持当前选中的工作模式不变
        const checkedMode = document.querySelector('input[name="work_mode"]:checked');
        if (checkedMode) {
            applyWorkModeSelection(checkedMode);
        }
    }
}

// 导出配置功能
document.getElementById('exportModbusItem').addEventListener('click', function () {
    // 检查是否处于ModbusCache配置的模式
    const workMode = document.querySelector('input[name="work_mode"]:checked').value;
    if (workMode !== 'modbus_cache') {
        showCustomAlert('只有在Modbus-Cache模式下才能导出配置', true);
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

    Array.from(rows).forEach((row, index) => {
        const itemIndex = index + 1;
        
        // 安全获取元素值的辅助函数
        const safeGetValue = (selector, defaultValue) => {
            const element = row.querySelector(selector);
            if (!element) {
                console.error(`Element not found: ${selector} in row ${itemIndex}`);
                return defaultValue;
            }
            return element.value || defaultValue;
        };
        
        modbusItems.push({
            enabled: row.dataset.enabled === 'true',
            slave_addr: safeGetValue(`#slave_addr_${itemIndex}`, '1'),
            function_code: safeGetValue(`#function_code_${itemIndex}`, '01'),
            register_addr: safeGetValue(`#register_addr_${itemIndex}`, '0'),
            register_num: safeGetValue(`#register_num_${itemIndex}`, '1'),
            mapped_slave_addr: safeGetValue(`#mapped_slave_addr_${itemIndex}`, '1'),
            mapped_register_addr: safeGetValue(`#mapped_register_addr_${itemIndex}`, '0'),
            timeout: safeGetValue(`#timeout_${itemIndex}`, '1000'),
            interval_time: safeGetValue(`#interval_time_${itemIndex}`, '1000'),
            baud_rate: safeGetValue(`#modbus_baud_rate_${itemIndex}`, '9600'),
            data_bit: safeGetValue(`#modbus_data_bit_${itemIndex}`, '8'),
            check_bit: convertParityToBackend(safeGetValue(`#modbus_check_bit_${itemIndex}`, 'None')),
            stop_bit: safeGetValue(`#modbus_stop_bit_${itemIndex}`, '1')
        });
    });

    // 创建导出对象
    const exportConfig = {
        work_mode: 'modbus_cache',
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
    downloadLink.download = `modbus_cache_config_${timestamp}.json`;
    document.body.appendChild(downloadLink);
    downloadLink.click();
    document.body.removeChild(downloadLink);
    URL.revokeObjectURL(url);

    displaySuccessMessage('配置导出成功');
});

// 保存配置功能
document.getElementById('saveModbusItem').addEventListener('click', function () {
    saveModbusConfig();
});

// 导入配置功能
document.getElementById('importModbusItem').addEventListener('click', function () {
    // 检查是否处于Modbus Cache配置的模式
    const workMode = document.querySelector('input[name="work_mode"]:checked').value;
    if (workMode !== 'modbus_cache') {
        showCustomAlert('只有在Modbus Cache模式下才能导入配置', true);
        return;
    }

    // 创建文件输入元素
    const fileInput = document.createElement('input');
    fileInput.type = 'file';
    fileInput.accept = '.json';
    fileInput.style.display = 'none';
    document.body.appendChild(fileInput);

    fileInput.onchange = function (event) {
        const file = event.target.files[0];
        if (!file) {
            document.body.removeChild(fileInput);
            return;
        }

        const reader = new FileReader();
        reader.onload = function (e) {
            try {
                const importConfig = JSON.parse(e.target.result);

                // 验证导入的JSON格式是否正确
                // 兼容 modbus_rtu 和 modbus_cache 两种模式名称
                if (!importConfig.work_mode || 
                    (importConfig.work_mode !== 'modbus_rtu' && importConfig.work_mode !== 'modbus_cache') || 
                    !Array.isArray(importConfig.modbus_items)) {
                    throw new Error('无效的配置文件格式，必须包含work_mode和modbus_items数组');
                }



                // 验证每个modbus项目的字段格式
                const modbusItems = importConfig.modbus_items;
                modbusItems.forEach((item, index) => {
                    // 验证必填字段是否存在
                    const requiredFields = ['slave_addr', 'function_code', 'register_addr', 'register_num', 'timeout', 'interval_time'];
                    for (const field of requiredFields) {
                        if (item[field] === undefined || item[field] === '') {
                            throw new Error(`配置项 #${index + 1} 缺少必填字段: ${field}`);
                        }
                    }

                    // 验证slave_addr (设备地址)是否为有效的十进制数字
                    if (!/^\d+$/.test(item.slave_addr) || parseInt(item.slave_addr) < 1 || parseInt(item.slave_addr) > 255) {
                        throw new Error(`配置项 #${index + 1} 的设备地址无效，必须是1-255之间的十进制数字`);
                    }

                    // 验证function_code (功能码)是否为有效的值
                    const validFunctionCodes = ['01', '02', '03', '04'];
                    if (!validFunctionCodes.includes(item.function_code)) {
                        throw new Error(`配置项 #${index + 1} 的功能码无效，有效值为: ${validFunctionCodes.join(', ')}`);
                    }

                    // 验证register_addr (寄存器地址)是否为有效的十六进制数字
                    if (!/^[0-9A-Fa-f]+$/.test(item.register_addr)) {
                        throw new Error(`配置项 #${index + 1} 的寄存器地址无效，必须是十六进制格式`);
                    }

                    // 验证register_num (寄存器数量)是否为有效的十进制数字
                    if (!/^\d+$/.test(item.register_num) || parseInt(item.register_num) < 1) {
                        throw new Error(`配置项 #${index + 1} 的寄存器数量无效，必须是大于0的十进制数字`);
                    }

                    // 验证timeout (接收超时)是否为有效的数字
                    if (!/^\d+$/.test(item.timeout) || parseInt(item.timeout) <= 0) {
                        throw new Error(`配置项 #${index + 1} 的接收超时无效，必须是大于0的数字`);
                    }

                    // 验证interval_time (间隔时间)是否为有效的数字
                    if (!/^\d+$/.test(item.interval_time) || parseInt(item.interval_time) <= 0) {
                        throw new Error(`配置项 #${index + 1} 的间隔时间无效，必须是大于0的数字`);
                    }

                    // // 验证data_format (数据格式)是否为有效的值
                    // const validDataFormats = ['Signed', 'Unsigned', 'HEX', 'Binary', 'Long', 'Float', 'Double', 'LongInverse', 'FloatInverse', 'DoubleInverse'];
                    // if (item.data_format && !validDataFormats.includes(item.data_format)) {
                    //     throw new Error(`配置项 #${index+1} 的数据格式无效，有效值为: ${validDataFormats.join(', ')}`);
                    // }

                    // // 验证report_format (上报方式)是否为有效的值
                    // const validReportFormats = ['tcp', 'http'];
                    // if (item.report_format && !validReportFormats.includes(item.report_format)) {
                    //     throw new Error(`配置项 #${index+1} 的上报方式无效，有效值为: ${validReportFormats.join(', ')}`);
                    // }

                    // 验证baud_rate (波特率)是否为有效的值
                    const validBaudRates = ['1200', '2400', '4800', '9600', '19200', '38400', '57600', '115200'];
                    if (item.baud_rate && !validBaudRates.includes(item.baud_rate)) {
                        throw new Error(`配置项 #${index + 1} 的波特率无效，有效值为: ${validBaudRates.join(', ')}`);
                    }

                    // 验证data_bit (数据位)是否为有效的值
                    const validDataBits = ['5', '6', '7', '8'];
                    if (item.data_bit && !validDataBits.includes(item.data_bit)) {
                        throw new Error(`配置项 #${index + 1} 的数据位无效，有效值为: ${validDataBits.join(', ')}`);
                    }

                    // 验证check_bit (校验位)是否为有效的值 (数字字符串格式)
                    const validCheckBits = ['0', '1', '2'];
                    if (item.check_bit && !validCheckBits.includes(item.check_bit)) {
                        throw new Error(`配置项 #${index + 1} 的校验位无效，有效值为: 0(无校验), 1(奇校验), 2(偶校验)`);
                    }

                    // 验证stop_bit (停止位)是否为有效的值
                    const validStopBits = ['1', '1.5', '2'];
                    if (item.stop_bit && !validStopBits.includes(item.stop_bit)) {
                        throw new Error(`配置项 #${index + 1} 的停止位无效，有效值为: ${validStopBits.join(', ')}`);
                    }
                });

                if (modbusItems.length > MAX_MODBUS_ITEMS) {
                    showCustomAlert(`配置项不能超过${MAX_MODBUS_ITEMS}个，将只导入前${MAX_MODBUS_ITEMS}个`);
                    modbusItems.length = MAX_MODBUS_ITEMS;
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
                    if (item.register_addr) row.querySelector(`#register_addr_${index + 1}`).value = item.register_addr;
                    if (item.register_num) row.querySelector(`#register_num_${index + 1}`).value = item.register_num;
                    if (item.timeout) row.querySelector(`#timeout_${index + 1}`).value = item.timeout;
                    if (item.interval_time) row.querySelector(`#interval_time_${index + 1}`).value = item.interval_time;
                    if (item.baud_rate) row.querySelector(`#modbus_baud_rate_${index + 1}`).value = item.baud_rate;
                    if (item.data_bit) row.querySelector(`#modbus_data_bit_${index + 1}`).value = item.data_bit;
                    if (item.check_bit) row.querySelector(`#modbus_check_bit_${index + 1}`).value = item.check_bit;
                    if (item.stop_bit) row.querySelector(`#modbus_stop_bit_${index + 1}`).value = item.stop_bit;

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
        reader.onerror = function () {
            showCustomAlert('读取文件失败', true);
            document.body.removeChild(fileInput);
        };
        reader.readAsText(file);
    };

    fileInput.click();
});

// 添加自定义提示框函数
function showCustomAlert(message, isError = false, options = {}) {
    const {
        showCancel = false,
        confirmText = '确定',
        cancelText = '取消',
        onConfirm = null,
        onCancel = null
    } = options || {};

    // 如果已有提示框，先移除
    const existingAlert = document.getElementById('customAlertBox');
    const existingOverlay = document.querySelector('.custom-alert-overlay');
    if (existingAlert) {
        document.body.removeChild(existingAlert);
    }
    if (existingOverlay) {
        document.body.removeChild(existingOverlay);
    }

    // 创建提示框容器
    const alertBox = document.createElement('div');
    alertBox.id = 'customAlertBox';
    alertBox.style.position = 'fixed';
    alertBox.style.top = '50%';
    alertBox.style.left = '50%';
    alertBox.style.transform = 'translate(-50%, -50%)';
    alertBox.style.zIndex = '10000';
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
    content.textContent = message;
    alertBox.appendChild(content);

    // 创建按钮容器
    const buttonContainer = document.createElement('div');
    buttonContainer.style.display = 'flex';
    buttonContainer.style.justifyContent = 'flex-end';
    buttonContainer.style.gap = '12px';
    buttonContainer.style.marginTop = '10px';

    // 添加确定按钮
    const confirmButton = document.createElement('button');
    confirmButton.textContent = confirmText;
    confirmButton.style.backgroundColor = isError ? '#d9534f' : '#2b6ec0';
    confirmButton.style.color = '#fff';
    confirmButton.style.border = 'none';
    confirmButton.style.borderRadius = '5px';
    confirmButton.style.padding = '10px 20px';
    confirmButton.style.fontSize = '14px';
    confirmButton.style.cursor = 'pointer';
    confirmButton.style.transition = 'all 0.2s ease';
    confirmButton.style.boxShadow = '0 2px 5px rgba(0,0,0,0.2)';

    confirmButton.onmouseover = function () {
        this.style.backgroundColor = isError ? '#c9302c' : '#1a5aa0';
        this.style.boxShadow = '0 4px 8px rgba(0,0,0,0.2)';
    };
    confirmButton.onmouseout = function () {
        this.style.backgroundColor = isError ? '#d9534f' : '#2b6ec0';
        this.style.boxShadow = '0 2px 5px rgba(0,0,0,0.2)';
    };

    let isClosing = false;

    const overlay = document.createElement('div');
    overlay.className = 'custom-alert-overlay';
    overlay.style.position = 'fixed';
    overlay.style.top = '0';
    overlay.style.left = '0';
    overlay.style.width = '100%';
    overlay.style.height = '100%';
    overlay.style.backgroundColor = 'rgba(0, 0, 0, 0.5)';
    overlay.style.zIndex = '9999';

    const closeAlert = (action = 'confirm') => {
        if (isClosing) return;
        isClosing = true;

        alertBox.style.opacity = '0';
        overlay.style.opacity = '0';

        setTimeout(() => {
            if (alertBox.parentNode) {
                document.body.removeChild(alertBox);
            }
            if (overlay.parentNode) {
                document.body.removeChild(overlay);
            }
            document.removeEventListener('keydown', escKeyHandler);
        }, 300);

        if (action === 'confirm' && typeof onConfirm === 'function') {
            onConfirm();
        } else if (action === 'cancel' && typeof onCancel === 'function') {
            onCancel();
        }
    };

    confirmButton.onclick = () => closeAlert('confirm');

    if (showCancel) {
        const cancelButton = document.createElement('button');
        cancelButton.textContent = cancelText;
        cancelButton.style.backgroundColor = '#f3f3f3';
        cancelButton.style.color = '#333';
        cancelButton.style.border = '1px solid #d0d0d0';
        cancelButton.style.borderRadius = '5px';
        cancelButton.style.padding = '10px 20px';
        cancelButton.style.fontSize = '14px';
        cancelButton.style.cursor = 'pointer';
        cancelButton.style.transition = 'all 0.2s ease';

        cancelButton.onmouseover = function () {
            this.style.backgroundColor = '#e2e2e2';
        };
        cancelButton.onmouseout = function () {
            this.style.backgroundColor = '#f3f3f3';
        };

        cancelButton.onclick = () => closeAlert('cancel');
        buttonContainer.appendChild(cancelButton);
    }

    buttonContainer.appendChild(confirmButton);
    alertBox.appendChild(buttonContainer);

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

    const overlayCloseAction = showCancel ? 'cancel' : 'confirm';

    // 点击遮罩层关闭提示框
    overlay.onclick = function () {
        closeAlert(overlayCloseAction);
    };

    // 添加键盘事件监听，按ESC键关闭
    const escKeyHandler = function (e) {
        if (e.key === 'Escape') {
            closeAlert(overlayCloseAction);
        }
    };
    document.addEventListener('keydown', escKeyHandler);

    // 返回overlay和alertBox，方便后续操作
    return {
        overlay,
        alertBox,
        close: function () {
            closeAlert('cancel');
        }
    };
}


// Serial config API

// 当前选中的串口
let currentSerialPort = 1;

// 阻止表单默认提交
document.getElementById("serialDataForm1").addEventListener("submit", event => event.preventDefault());
document.getElementById("serialDataForm2").addEventListener("submit", event => event.preventDefault());
document.getElementById("serialDataForm3").addEventListener("submit", event => event.preventDefault());

// 串口标签页切换功能
function switchSerialTab(portNumber) {
    // 隐藏所有配置面板
    for (let i = 1; i <= 3; i++) {
        const panel = document.getElementById(`serialConfig${i}`);
        const tab = document.getElementById(`serialTab${i}`);
        if (panel && tab) {
            panel.style.display = 'none';
            tab.style.borderBottom = 'none';
            tab.style.color = '#333';
            tab.style.cursor = 'pointer';
            tab.style.opacity = '1';
        }
    }

    // 显示选中的配置面板
    const selectedPanel = document.getElementById(`serialConfig${portNumber}`);
    const selectedTab = document.getElementById(`serialTab${portNumber}`);
    if (selectedPanel && selectedTab) {
        selectedPanel.style.display = 'block';
        selectedTab.style.borderBottom = '2px solid #009ee1';
        selectedTab.style.color = '#009ee1';
    } else {
        console.error(`找不到CH${portNumber}配置面板或标签`);
    }

    currentSerialPort = portNumber;
}



// 串口配置提交处理 - 使用模块化版本的正确实现
async function handleSerialConfigSubmit() {
    if (SerialUIState.isSubmitting) return;
    SerialUIState.isSubmitting = true;
    const btn = document.getElementById('serialButton');
    const originalText = btn.textContent;
    btn.disabled = true;

    try {
        // 首先保存配置模式（静默）
        const selectedMode = document.querySelector('input[name="serialSyncMode"]:checked');
        if (selectedMode) {
            try {
                await postData('/serial_config_mode_set', { config_mode: selectedMode.value });
            } catch (e) {
                console.warn('保存配置模式失败(静默):', e);
            }
        }

        // 根据配置模式决定提交逻辑
        let successCount = 0;
        const configMode = selectedMode ? selectedMode.value : 'separate';

        if (configMode === 'unified') {
            // 统一配置：使用统一配置表单的参数提交到三个串口
            const unifiedData = SerialConfigManager.getUnifiedConfig();

            // 为每个串口提交相同的统一配置
            for (let port = 1; port <= 3; port++) {
                const data = { ...unifiedData, serial_port: port, defer_apply: port < 3 };
                await postData('/serial_set', data);
                successCount++;
            }
        } else if (configMode === 'separate') {
            // 分别配置：正常分别提交各自的串口参数
            for (let port = 1; port <= 3; port++) {
                const data = SerialConfigManager.getPortConfig(port);
                data.defer_apply = port < 3;
                await postData('/serial_set', data);
                successCount++;
            }
        } else if (configMode === 'slave_follow') {
            // 从机跟随：提交CH1、CH2，CH3使用默认参数
            await submitPortsWithCachedCH3([1, 2]);
            successCount = 3; // CH1、CH2正常提交，CH3使用默认参数，总共3个端口
        }

        // 只在所有端口配置保存成功后显示一次成功提示
        btn.textContent = '配置成功';
        if (successCount > 0) {
            displaySuccessMessage('串口配置保存成功');
        }
    } catch (error) {
        console.error('Error saving serial config:', error);
        btn.textContent = '配置失败';
        displayErrorMessage('串口配置保存失败');
    } finally {
        setTimeout(() => {
            btn.textContent = originalText || '保存配置';
            btn.disabled = false;
        }, 2000);
        SerialUIState.isSubmitting = false;
    }
}

// 绑定串口配置提交按钮事件
document.getElementById('serialButton').addEventListener('click', handleSerialConfigSubmit);

// 提交指定端口并为CH3使用默认参数的辅助函数 - 使用模块化版本的正确实现
async function submitPortsWithCachedCH3(ports) {
    // 首先保存指定的端口（通常是CH1、CH2）
    for (const port of ports) {
        const data = SerialConfigManager.getPortConfig(port);
        data.defer_apply = true;
        await postData('/serial_set', data);
    }

    // 为CH3使用默认参数
    try {
        const ch3Data = {
            serial_port: 3,
            baud_rate: '9600',
            data_bit: '8',
            check_bit: '0',  // 修正：使用数字字符串格式，与其他端口保持一致
            stop_bit: '1',
            frame_time: '50',
            frame_len: '512',
            reply_timeout: '500'
        };
        await postData('/serial_set', ch3Data);
        console.log('CH3使用默认参数保存成功:', ch3Data);
    } catch (error) {
        console.warn('CH3默认参数保存失败:', error);
    }
}

// 串口配置管理系统 - 使用模块化版本
const SerialConfigManager = {
    // 参数字段映射和默认值
    fieldMappings: {
        baud_rate: { default: '9600', transform: null },
        data_bit: { default: '8', transform: null },
        check_bit: {
            default: '0',
            transform: null  // 不需要转换，前端和后端都使用数字字符串格式
        },
        stop_bit: { default: '1', transform: null },
        frame_time: { default: '50', transform: null },
        frame_len: { default: '512', transform: null },
        reply_timeout: { default: '500', transform: null }
    },

    // 为指定端口设置配置数据
    setPortConfig(port, data, useDefaults = false) {
        Object.entries(this.fieldMappings).forEach(([field, config]) => {
            const elementId = `${field}_${port}`;
            const element = document.getElementById(elementId);
            if (element) {
                let value = useDefaults ? config.default : (data[field] || config.default);

                // 应用数据转换
                if (config.transform && config.transform.toFrontend && !useDefaults) {
                    value = config.transform.toFrontend(value);
                }

                element.value = value;
            }
        });
    },

    // 为统一配置设置数据
    setUnifiedConfig(data, useDefaults = false) {
        Object.entries(this.fieldMappings).forEach(([field, config]) => {
            const elementId = `unified_${field}`;
            const element = document.getElementById(elementId);
            if (element) {
                let value = useDefaults ? config.default : (data[field] || config.default);

                // 应用数据转换
                if (config.transform && config.transform.toFrontend && !useDefaults) {
                    value = config.transform.toFrontend(value);
                }

                element.value = value;
            }
        });

        console.log(`已为统一配置设置数据${useDefaults ? '(默认值)' : ''}:`, data);
    },

    // 获取端口配置数据（用于提交）
    getPortConfig(port) {
        const data = { serial_port: port };
        
        // 在modbus_cache模式下，总是使用分别配置模式
        Object.entries(this.fieldMappings).forEach(([field, config]) => {
            const elementId = `${field}_${port}`;
            const element = document.getElementById(elementId);
            
            if (element) {
                let value = element.value;


                // 应用数据转换
                if (config.transform && config.transform.toBackend) {
                    value = config.transform.toBackend(value);
                }

                data[field] = value;
            } else {
                console.warn(`串口${port}找不到元素: ${elementId}`);
            }
        });
        return data;
    },

    // 获取统一配置数据（用于提交）
    getUnifiedConfig() {
        const data = {};
        Object.entries(this.fieldMappings).forEach(([field, config]) => {
            const elementId = `unified_${field}`;
            const element = document.getElementById(elementId);
            if (element) {
                let value = element.value;

                // 应用数据转换
                if (config.transform && config.transform.toBackend) {
                    value = config.transform.toBackend(value);
                }

                data[field] = value;
            }
        });
        return data;
    }
};

// 更新配置模式选项卡的对勾图标显示
function updateSyncModeCheckIcon(activeMode) {
    // 隐藏所有对勾图标
    const allCheckIcons = document.querySelectorAll('.sync-check-icon');
    allCheckIcons.forEach(icon => {
        icon.style.display = 'none';
    });

    // 显示当前激活模式的对勾图标
    const activeModeRadio = document.querySelector(`input[name="serialSyncMode"][value="${activeMode}"]`);
    if (activeModeRadio) {
        const checkIcon = activeModeRadio.parentElement.querySelector('.sync-check-icon');
        if (checkIcon) {
            checkIcon.style.display = 'inline';
        }
    }

}

// 获取串口参数配置模式
async function fetchSerialConfigMode() {
    try {
        const response = await fetchData('/serial_config_mode_info');
        const mode = response.config_mode || 'unified'; // 默认为统一配置

        // 设置前端选中状态
        const modeRadio = document.querySelector(`input[name="serialSyncMode"][value="${mode}"]`);
        if (modeRadio) {
            modeRadio.checked = true;
            // 触发切换模式的逻辑（函数在HTML中定义）
            const applySwitchMode = () => {
                if (typeof switchSerialConfigMode === 'function') {
                    switchSerialConfigMode(mode);
                    updateSyncModeCheckIcon(mode);
                    return true;
                }
                return false;
            };

            // 立即尝试
            if (!applySwitchMode()) {
                // 如果函数还未定义，多次重试
                let retryCount = 0;
                const retryInterval = setInterval(() => {
                    retryCount++;
                    if (applySwitchMode() || retryCount > 10) {
                        clearInterval(retryInterval);
                        if (retryCount > 10) {
                            console.error('switchSerialConfigMode函数未找到，配置模式可能无法正确显示');
                        }
                    }
                }, 100);
            }
        }

        updateSlaveFollowAvailability();

        return mode;
    } catch (error) {
        console.warn('获取串口配置模式失败，使用默认值:', error);
        console.error('错误详情:', error.message, error.stack);
        // 默认使用统一配置模式
        const defaultMode = 'unified';
        const modeRadio = document.querySelector(`input[name="serialSyncMode"][value="${defaultMode}"]`);
        if (modeRadio) {
            modeRadio.checked = true;
            const applyDefaultMode = () => {
                if (typeof switchSerialConfigMode === 'function') {
                    console.log('应用默认配置模式:', defaultMode);
                    switchSerialConfigMode(defaultMode);
                    updateSyncModeCheckIcon(defaultMode);
                    return true;
                }
                return false;
            };

            if (!applyDefaultMode()) {
                let retryCount = 0;
                const retryInterval = setInterval(() => {
                    retryCount++;
                    if (applyDefaultMode() || retryCount > 10) {
                        clearInterval(retryInterval);
                    }
                }, 100);
            }
        }
        updateSlaveFollowAvailability();
        return defaultMode;
    }
}

// 保存串口参数配置模式
async function saveSerialConfigMode(mode) {
    try {
        const data = {
            config_mode: mode
        };
        console.log('正在保存串口配置模式:', mode, '数据:', data);
        const response = await postData('/serial_config_mode_set', data);
        console.log('串口配置模式保存成功:', mode, '响应:', response);
        return true;
    } catch (error) {
        console.error('保存串口配置模式失败:', error);
        console.error('错误详情:', error.message, error.stack);
        showCustomAlert('配置模式保存失败: ' + error.message, true);
        return false;
    }
}

// 获取串口配置数据 - 使用模块化版本的正确实现
async function serialfetchData() {
    try {
        // 先获取配置模式
        await fetchSerialConfigMode();

        // 获取所有串口的配置数据
        for (let port = 1; port <= 3; port++) {
            try {
                const responseData = await fetchData(`/serial_set_info?port=${port}`);

                // 使用配置管理系统设置端口数据
                SerialConfigManager.setPortConfig(port, responseData);

                // 同时设置统一配置表单（使用第一个端口的数据作为统一配置的默认值）
                if (port === 1) {
                    SerialConfigManager.setUnifiedConfig(responseData);
                }

            } catch (error) {
                console.warn(`Failed to fetch data for serial port ${port}:`, error);
                // 如果获取失败，使用默认值
                SerialConfigManager.setPortConfig(port, {}, true);
                if (port === 1) {
                    SerialConfigManager.setUnifiedConfig({}, true);
                }
            }
        }
    } catch (error) {
        console.error('Failed to fetch serial data. Status:', error);
        // 为所有串口设置默认值
        for (let port = 1; port <= 3; port++) {
            SerialConfigManager.setPortConfig(port, {}, true);
        }
        SerialConfigManager.setUnifiedConfig({}, true);
    }
}


// 页面加载时获取串口配置 - 延迟执行确保DOM准备就绪
document.addEventListener('DOMContentLoaded', () => {
    // 等待一下确保HTML中的函数都已经定义
    setTimeout(() => {
        
        // 确保CH1相关元素存在
        const ch1Elements = {
            panel: document.getElementById('serialConfig1'),
            tab: document.getElementById('serialTab1'),
            baud_rate: document.getElementById('baud_rate_1'),
            data_bit: document.getElementById('data_bit_1'),
            check_bit: document.getElementById('check_bit_1'),
            stop_bit: document.getElementById('stop_bit_1'),
            frame_time: document.getElementById('frame_time_1'),
            frame_len: document.getElementById('frame_len_1'),
            reply_timeout: document.getElementById('reply_timeout_1')
        };
        
        
        // 强制设置currentSerialPort为1
        if (typeof currentSerialPort === 'undefined' || currentSerialPort !== 1) {
            currentSerialPort = 1;
        }
        
        // 获取串口配置数据
        serialfetchData();
    }, 300);
});

// Serial control API

document.getElementById("serial_ctl").addEventListener("submit", event => event.preventDefault());

function normalizeSerialHexText(text) {
    return (text || '').replace(/\s+/g, '').toUpperCase();
}

function isSerialHexTextValid(text) {
    const normalized = normalizeSerialHexText(text);
    return normalized.length > 0 && (normalized.length % 2 === 0) && /^[0-9A-F]+$/.test(normalized);
}

function serialHexTextToBytes(text) {
    const normalized = normalizeSerialHexText(text);
    const bytes = [];
    for (let i = 0; i < normalized.length; i += 2) {
        bytes.push(parseInt(normalized.slice(i, i + 2), 16));
    }
    return bytes;
}

function calculateModbusCrc16(bytes) {
    let crc = 0xFFFF;
    for (const byte of bytes) {
        crc ^= byte;
        for (let i = 0; i < 8; i++) {
            crc = (crc & 0x0001) ? ((crc >>> 1) ^ 0xA001) : (crc >>> 1);
        }
    }
    return crc & 0xFFFF;
}

function formatSerialCrc(crc) {
    const low = (crc & 0xFF).toString(16).padStart(2, '0').toUpperCase();
    const high = ((crc >> 8) & 0xFF).toString(16).padStart(2, '0').toUpperCase();
    return `${low} ${high}`;
}

function getSerialCrcState() {
    return {
        enabled: !!document.getElementById('serial_crc_enable')?.checked,
        isHex: !!document.getElementById('tran')?.checked,
        instruction: document.getElementById('instruction')?.value || ''
    };
}

function updateSerialCrcDisplay() {
    const crcInput = document.getElementById('serial_crc_value');
    if (!crcInput) {
        return;
    }

    const { enabled, isHex, instruction } = getSerialCrcState();
    const normalized = normalizeSerialHexText(instruction);
    if (!enabled || !isHex || normalized.length === 0 || !isSerialHexTextValid(instruction)) {
        crcInput.value = '';
        return;
    }

    const crc = calculateModbusCrc16(serialHexTextToBytes(instruction));
    crcInput.value = formatSerialCrc(crc);
}

function buildSerialInstructionPayload() {
    const { enabled, isHex, instruction } = getSerialCrcState();
    if (!enabled || !isHex || !isSerialHexTextValid(instruction)) {
        return instruction;
    }

    const crc = calculateModbusCrc16(serialHexTextToBytes(instruction));
    return `${instruction.trim()} ${formatSerialCrc(crc)}`;
}

// 串口指令提交 - 使用模块化版本的正确实现
async function serialSubmit() {
    // 获取选中的调试通道
    const selectedChannel = document.querySelector('input[name="debug_channel"]:checked');
    const channel = selectedChannel ? parseInt(selectedChannel.value) : 1;

    const data = {
        instruction: buildSerialInstructionPayload(),
        sendType: document.getElementById('tran').checked ? 'hex' : 'ascii',
        channel: channel
    };

    try {
        // 发送串口指令
        const response = await postData('/serial_ctl', data);
        console.log('串口指令发送成功');
    } catch (error) {
        console.error('Error sending serial command:', error);
    }
}

// 串口控制按钮处理 - 使用模块化版本的正确实现
function handleSerialControl(event) {
    const instructionElement = document.getElementById('instruction');
    const instruction = instructionElement.value;
    const errorMessageElement = document.getElementById('error-message');
    const tranElement = document.getElementById('tran');
    const normalized = normalizeSerialHexText(instruction);
    const isHex = /^[0-9A-Fa-f\s]+$/i.test(instruction.replace(/\s/g, ''));

    if (tranElement.checked && (!isHex || (normalized.length % 2 !== 0))) {
        const message = !isHex
            ? '数据格式异常，请输入十六进制数据'
            : '数据格式异常，请输入偶数长度的十六进制数据';
        errorMessageElement.innerText = message;
        showCustomAlert(message, true);
        event.preventDefault();
    } else {
        errorMessageElement.innerText = '';
        serialSubmit();
    }
}

// 清空响应 - 使用模块化版本的正确实现
function clearResponse() {
    const responseElement = document.getElementById('devcie_report');
    responseElement.innerHTML = '';
}

// 绑定串口控制相关事件
document.getElementById('serialControl').addEventListener('click', handleSerialControl);
document.getElementById('clearResponse').addEventListener('click', clearResponse);
const serialCrcEnable = document.getElementById('serial_crc_enable');
const serialInstructionInput = document.getElementById('instruction');
const serialHexRadio = document.getElementById('tran');
if (serialCrcEnable) serialCrcEnable.addEventListener('change', updateSerialCrcDisplay);
if (serialInstructionInput) serialInstructionInput.addEventListener('input', updateSerialCrcDisplay);
if (serialHexRadio) serialHexRadio.addEventListener('change', updateSerialCrcDisplay);
updateSerialCrcDisplay();
// 添加接收状态控制变量
const MAX_LINES = 512; // 最多显示行
let isReceiving = true;
let pollInterval = null;
let uartWebSocket = null; // 串口WebSocket连接

// 通道消息统计
let channelStats = {
    ch1: 0,
    ch2: 0,
    ch3: 0
};
// ==================== 时间格式化（使用ESP32统一时间）====================
/**
 * 格式化时间戳（使用ESP32统一时间）
 * @param {number} timestamp_us - ESP32时间戳（微秒）
 * @param {boolean} isRx - 是否是接收时间戳（保留参数兼容性）
 * @returns {string} 格式化的时间字符串 HH:MM:SS.mmm
 */
function formatTimestamp(timestamp_us, isRx = false) {
    // 转换为毫秒
    const timestamp_ms = timestamp_us / 1000;
    
    // 如果时间未同步，显示相对时间
    if (!isTimeSynced) {
        const seconds = Math.floor(timestamp_ms / 1000);
        const milliseconds = Math.floor(timestamp_ms % 1000);
        return `[未同步] ${seconds}.${String(milliseconds).padStart(3, '0')}s`;
    }
    
    // 使用ESP32的绝对时间戳创建Date对象
    const date = new Date(timestamp_ms);
    
    // 格式化为 HH:MM:SS.mmm
    const hours = String(date.getHours()).padStart(2, '0');
    const minutes = String(date.getMinutes()).padStart(2, '0');
    const seconds = String(date.getSeconds()).padStart(2, '0');
    const milliseconds = String(date.getMilliseconds()).padStart(3, '0');
    
    return `${hours}:${minutes}:${seconds}.${milliseconds}`;
}

// 保留旧函数以防其他地方使用（已弃用）
function getCurrentTimePrefix() {
    const now = new Date();
    const hours = String(now.getHours()).padStart(2, '0');
    const minutes = String(now.getMinutes()).padStart(2, '0');
    return `${hours}:${minutes}`;
}

// 初始化通道过滤器
function initChannelFilters() {
    // 绑定过滤器复选框事件
    ['ch1', 'ch2', 'ch3'].forEach(channel => {
        const checkbox = document.getElementById(`filter_${channel}`);
        if (checkbox) {
            checkbox.addEventListener('change', applyChannelFilter);
        }
    });
}

// 应用通道过滤器
function applyChannelFilter() {
    const resultElement = document.getElementById('devcie_report');
    if (!resultElement) return;

    const lines = resultElement.querySelectorAll('.uart-data-line');
    lines.forEach(line => {
        const channel = line.dataset.channel;
        const checkbox = document.getElementById(`filter_${channel}`);
        if (checkbox) {
            line.style.display = checkbox.checked ? 'block' : 'none';
        }
    });
}

// 清空调试日志
function clearDebugLog() {
    const resultElement = document.getElementById('devcie_report');
    if (resultElement) {
        resultElement.innerHTML = '';
    }

    // 重置统计
    channelStats = { ch1: 0, ch2: 0, ch3: 0 };
    updateChannelCounts();
}

// 更新通道统计数量
function updateChannelCounts() {
    ['ch1', 'ch2', 'ch3'].forEach(channel => {
        const countElement = document.getElementById(`count_${channel}`);
        if (countElement) {
            countElement.textContent = channelStats[channel];
        }
    });
}

// 创建格式化的UART数据行 - 使用模块化版本的正确实现
function createFormattedUartRow(data, timestamp) {
    const channel = (data.channel !== undefined && data.channel !== null) ? data.channel : '?';
    const channelKey = `ch${channel}`;
    const isHexMode = document.getElementById('tran').checked;

    // 更新统计
    if (channelStats[channelKey] !== undefined) {
        channelStats[channelKey]++;
        updateChannelCounts();
    }

    // 创建数据行元素
    const lineDiv = document.createElement('div');
    lineDiv.className = `uart-data-line ${data.is_tx ? 'tx' : 'rx'} ${channelKey}`;
    lineDiv.dataset.channel = channelKey;

    // 检查是否应该显示此通道
    const checkbox = document.getElementById(`filter_${channelKey}`);
    if (checkbox && !checkbox.checked) {
        lineDiv.style.display = 'none';
    }

    // 创建内容
    const timestampSpan = document.createElement('span');
    timestampSpan.className = 'uart-timestamp';
    timestampSpan.textContent = `[${timestamp}]`;

    const channelSpan = document.createElement('span');
    channelSpan.className = `uart-channel ${channelKey}`;
    channelSpan.textContent = `CH${channel}`;

    const directionSpan = document.createElement('span');
    directionSpan.className = `uart-direction ${data.is_tx ? 'tx' : 'rx'}`;
    directionSpan.textContent = data.is_tx ? '发→◇' : '收←◆';

    const dataSpan = document.createElement('span');
    dataSpan.className = 'uart-data';
    dataSpan.dataset.hex = data.hex;
    dataSpan.dataset.ascii = data.ascii;
    dataSpan.textContent = isHexMode ? data.hex : data.ascii;

    // 组装内容
    lineDiv.appendChild(timestampSpan);
    lineDiv.appendChild(channelSpan);
    lineDiv.appendChild(directionSpan);
    lineDiv.appendChild(dataSpan);

    return lineDiv;
}

// 更新显示模式（十六进制/ASCII）
function updateDisplayMode() {
    const resultElement = document.getElementById('devcie_report');
    if (!resultElement) return;

    const isHexMode = document.getElementById('tran').checked;
    const lines = resultElement.querySelectorAll('.uart-data-line');

    lines.forEach(line => {
        const dataSpan = line.querySelector('.uart-data');
        if (dataSpan) {
            // 从数据属性中获取原始数据并重新显示
            const hexData = dataSpan.dataset.hex;
            const asciiData = dataSpan.dataset.ascii;
            if (hexData && asciiData) {
                dataSpan.textContent = isHexMode ? hexData : asciiData;
            }
        }
    });
}

// 初始化发送通道选择样式
function initDebugChannelSelection() {
    const channelRadios = document.querySelectorAll('input[name="debug_channel"]');

    // 为每个radio绑定change事件
    channelRadios.forEach(radio => {
        radio.addEventListener('change', updateChannelSelection);
    });

    // 初始化选中状态
    updateChannelSelection();
}

// 更新通道选择样式
function updateChannelSelection() {
    const channelItems = document.querySelectorAll('.debug-channel-item');
    const checkedRadio = document.querySelector('input[name="debug_channel"]:checked');

    // 移除所有选中样式
    channelItems.forEach(item => {
        item.classList.remove('selected');
    });

    // 为选中的项添加样式
    if (checkedRadio) {
        const parentItem = checkedRadio.closest('.debug-channel-item');
        if (parentItem) {
            parentItem.classList.add('selected');
        }
    }
}

// 初始化串口WebSocket连接
function initUartWebSocket() {
    try {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        uartWebSocket = new WebSocket(`${protocol}//${window.location.host}/ws/log`);

        uartWebSocket.onopen = () => {
            console.log('串口WebSocket连接已建立');
        };

        uartWebSocket.onmessage = (event) => {
            if (!isReceiving) return;

            try {
                // 尝试解析JSON，如果失败则认为是普通日志消息
                let data;
                try {
                    data = JSON.parse(event.data);
                } catch (e) {
                    // 不是JSON格式，可能是普通日志消息，忽略
                    return;
                }

                // 检查是否是UART数据 - 修复数据类型匹配问题
                if (data.type === 'uart_data') {
                    const timestamp = formatTimestamp(data.timestamp || Date.now());
                    const newRow = createFormattedUartRow(data, timestamp);

                    const resultElement = document.getElementById('devcie_report');
                    if (resultElement) {
                        resultElement.appendChild(newRow);

                        // 限制显示行数
                        const lines = resultElement.querySelectorAll('.uart-data-line');
                        if (lines.length > MAX_LINES) {
                            // 删除最旧的行
                            const linesToRemove = lines.length - MAX_LINES;
                            for (let i = 0; i < linesToRemove; i++) {
                                resultElement.removeChild(lines[i]);
                            }
                        }

                        // 自动滚动到底部
                        resultElement.scrollTop = resultElement.scrollHeight;
                    }
                }
            } catch (error) {
                console.error('处理串口WebSocket消息时出错:', error);
            }
        };

        uartWebSocket.onclose = () => {
            console.log('串口WebSocket连接已关闭');
            // 5秒后尝试重新连接
            setTimeout(initUartWebSocket, 5000);
        };

        uartWebSocket.onerror = (error) => {
            console.error('串口WebSocket错误:', error);
        };
    } catch (error) {
        console.error('初始化串口WebSocket时出错:', error);
    }
}

// 修改轮询函数，保留以兼容旧版本，但不再主动调用

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
document.addEventListener('DOMContentLoaded', async () => {
    console.log('====================================');
    console.log('页面初始化开始');
    console.log('====================================');
    
    // 🔥 首先同步时间（优先级最高）
    await syncTimeToESP32();
    
    // 设置定期重新同步（每10分钟检查一次，如果超过1小时则重新同步）
    setInterval(async () => {
        if (shouldResyncTime()) {
            console.log('⏰ 定期重新同步时间...');
            await syncTimeToESP32();
        }
    }, 10 * 60 * 1000); // 每10分钟检查
    
    // 页面重新可见时检查时间同步
    document.addEventListener('visibilitychange', async () => {
        if (!document.hidden && shouldResyncTime()) {
            console.log('⏰ 页面重新可见，检查时间同步...');
            await syncTimeToESP32();
        }
    });
    
    // 初始化串口WebSocket连接
    initUartWebSocket();

    // 初始化通道过滤器
    initChannelFilters();

    // 绑定显示模式切换事件
    const hexRadio = document.getElementById('tran');
    const asciiRadio = document.getElementById('mqt');
    if (hexRadio) hexRadio.addEventListener('change', updateDisplayMode);
    if (asciiRadio) asciiRadio.addEventListener('change', updateDisplayMode);

    // 初始化发送通道选择样式
    initDebugChannelSelection();
    
    console.log('✅ 页面初始化完成');
});

// 页面不可见时的处理
document.addEventListener('visibilitychange', () => {
    // 不再需要处理轮询间隔
});

// Net config API

window.onload = () => autoDHCP(document.getElementById('is_dhcp'));

function autoDHCP(element) {
    if (element.classList.contains('off')) {
        element.classList.remove('off');
        element.classList.add('on');
        document.getElementById('is_dhcp').value = '2';
    } else {
        element.classList.remove('on');
        element.classList.add('off');
        document.getElementById('is_dhcp').value = '1';
    }

    // 如果是DHCP（开关是OFF状态），禁用静态IP字段
    if (element.classList.contains('off')) {
        ['static_ip', 'static_netmask', 'static_gateway', 'static_dns1', 'static_dns2'].forEach(id => {
            document.getElementById(id).setAttribute('disabled', true);
        });
    } else {
        // 如果是静态IP（开关是ON状态），启用静态IP字段
        ['static_ip', 'static_netmask', 'static_gateway', 'static_dns1', 'static_dns2'].forEach(id => {
            document.getElementById(id).removeAttribute('disabled');
        });
    }
}

document.getElementById("net_set").addEventListener("submit", event => event.preventDefault());

async function netsetSubmit() {
    const formData = new FormData(document.getElementById('net_set'));
    const data = Object.fromEntries(formData);
    try {
        await postData('/net_set', data);
        document.getElementById('netSetButton').textContent = "保存成功";
        const deviceRestart = document.getElementById('deviceRestart');
        if (deviceRestart) {
            showSvg('deviceRestart', 6000);
        }
    } catch (error) {
        console.error('Error fetching data. Status:', error);
        document.getElementById('netSetButton').textContent = '保存失败';
    }
    setTimeout(() => document.getElementById('netSetButton').textContent = '保存', 2000);
}

document.getElementById('netSetButton').addEventListener('click', netsetSubmit);

// AP管理相关处理
async function apsetSubmit(event) {
    event.preventDefault();
    const formData = new FormData(document.getElementById('ap_management_set'));
    const data = Object.fromEntries(formData.entries());
    try {
        await postData('/ap_set', data);
        showSvg('netSet', 3000);
    } catch (error) {
        console.error('AP设置保存失败:', error);
    }
}

async function apsetfetchData() {
    try {
        const responseData = await fetchData('/ap_set_info');
        if (!responseData) {
            console.error('No data returned from /ap_set_info');
            return;
        }

        // 设置AP关闭时间字段
        const apTimeoutSelect = document.getElementById('ap_timeout');
        if (responseData.ap_timeout !== undefined) {
            const timeoutValue = responseData.ap_timeout.toString();
            // 检查返回的值是否在下拉选项中
            const option = apTimeoutSelect.querySelector(`option[value="${timeoutValue}"]`);
            if (option) {
                apTimeoutSelect.value = timeoutValue;
            } else {
                // 如果返回的值不在预设选项中，默认选择10分钟
                apTimeoutSelect.value = '10';
                console.warn(`AP关闭时间值 ${timeoutValue} 不在预设选项中，使用默认值10分钟`);
            }
        } else {
            apTimeoutSelect.value = '10'; // 默认10分钟
        }
    } catch (error) {
        console.error('获取AP设置失败:', error);
        // 设置默认值
        document.getElementById('ap_timeout').value = '10';
    }
}

document.getElementById("ap_management_set").addEventListener("submit", event => event.preventDefault());
document.getElementById('apSetButton').addEventListener('click', apsetSubmit);


async function netsetfetchData() {
    try {
        const responseData = await fetchData('/net_set_info');
        if (!responseData) {
            console.error('No data returned from /net_set_info');
            return;
        }

        // 设置静态IP相关字段
        ['static_ip', 'static_netmask', 'static_gateway', 'static_dns1', 'static_dns2'].forEach(id =>
            document.getElementById(id).value = responseData[id]);

        // 默认使用WiFi，设置WiFi相关字段
        ['wifi_ssid', 'wifi_password'].forEach(id =>
            document.getElementById(id).value = responseData[id]);


        // 设置静态IP开关状态
        const static_ip_switch = document.getElementById('static_ip_switch');
        static_ip_switch.classList.toggle('off', responseData.is_dhcp == 1);
        static_ip_switch.classList.toggle('on', responseData.is_dhcp == 2);

        // 根据DHCP状态启用/禁用静态IP字段
        ['static_ip', 'static_netmask', 'static_gateway', 'static_dns1', 'static_dns2'].forEach(id => {
            if (responseData.is_dhcp == 1) {
                document.getElementById(id).setAttribute('disabled', true);
            } else {
                document.getElementById(id).removeAttribute('disabled');
            }
        });

        // 默认显示WiFi相关字段
        ['wifi_ssid_div', 'wifi_password_div', 'wifisearch', 'scanList'].forEach(el => {
            const element = document.getElementById(el);
            if (element) {
                element.style.display = 'flex';
            }
        });

    } catch (error) {
        console.error('Failed to fetch data. Status:', error);
    }
}

netsetfetchData();
apsetfetchData();


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

// WiFi诊断测试函数
async function testWifiHardware() {
    try {
        console.log('Testing WiFi hardware...');
        const response = await fetch('/wifi_test');
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
        const data = await response.json();
        console.log('WiFi hardware test results:', data);

        // 显示测试结果
        const testInfo = `WiFi硬件测试结果:\n` +
            `模式状态: ${data.wifi_mode_status}\n` +
            `当前模式: ${data.wifi_mode}\n` +
            `MAC地址: ${data.mac_address}\n` +
            `上次扫描状态: ${data.last_scan_status}\n` +
            `上次扫描数量: ${data.last_scan_count}`;

        console.log(testInfo);
        return data;
    } catch (error) {
        console.error('WiFi hardware test failed:', error);
        return null;
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

    // 首先进行WiFi硬件测试
    console.log('=== Starting WiFi scan process ===');
    const testResult = await testWifiHardware();

    // 移除任何现有的消息，但保留WiFi列表
    const existingMessages = document.querySelectorAll('.error-message, .info-message');
    existingMessages.forEach(msg => {
        if (msg.parentNode) msg.parentNode.removeChild(msg);
    });
    try {
        console.log('Starting WiFi scan request...');
        // WiFi扫描需要更长时间，设置15秒超时
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), 15000);
        const response = await fetch('/find_wifi', {
            signal: controller.signal
        });
        clearTimeout(timeoutId);
        // 检查响应状态
        if (!response.ok) {
            console.error('HTTP error:', response.status, response.statusText);
            throw new Error(`HTTP error! status: ${response.status}`);
        }

        const data = await response.json();
        console.log('WiFi scan response:', data);

        // 处理错误情况
        if (data.error) {
            console.log('WiFi scan error:', data.error, 'wait_time:', data.wait_time);
            // 显示错误信息
            const errorMessage = document.createElement('div');
            errorMessage.className = 'error-message';
            errorMessage.style.color = '#ff4444';
            errorMessage.style.padding = '10px';
            errorMessage.style.marginBottom = '10px';
            errorMessage.style.backgroundColor = '#fff2f2';
            errorMessage.style.border = '1px solid #ffcccc';
            errorMessage.style.borderRadius = '4px';
            errorMessage.textContent = data.error;
            document.getElementById('scanList').parentNode.insertBefore(errorMessage, document.getElementById('scanList'));
            // 自动移除错误信息
            setTimeout(() => {
                if (errorMessage && errorMessage.parentNode) {
                    errorMessage.parentNode.removeChild(errorMessage);
                }
            }, 8000); // 增加显示时间到8秒
            // 延迟一段时间后允许再次点击
            const waitTime = data.wait_time || 3000;
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
        console.error('Error fetching WiFi list:', error);
        const errorMessage = document.createElement('div');
        errorMessage.className = 'error-message';
        errorMessage.style.color = '#ff4444';
        errorMessage.style.padding = '10px';
        errorMessage.style.marginBottom = '10px';
        errorMessage.style.backgroundColor = '#fff2f2';
        errorMessage.style.border = '1px solid #ffcccc';
        errorMessage.style.borderRadius = '4px';
        if (error.name === 'AbortError') {
            errorMessage.textContent = 'WiFi扫描超时，请稍后重试（扫描中请等待15秒）';
        } else if (error.name === 'TypeError' && error.message.includes('Failed to fetch')) {
            errorMessage.textContent = '网络连接失败，请检查设备连接状态';
        } else {
            errorMessage.textContent = `WiFi扫描失败：${error.message}`;
        }
        document.getElementById('scanList').parentNode.insertBefore(errorMessage, document.getElementById('scanList'));
        setTimeout(() => {
            if (errorMessage && errorMessage.parentNode) {
                errorMessage.parentNode.removeChild(errorMessage);
            }
        }, 5000);
        setTimeout(() => {
            wifiSearchBtn.textContent = '搜索';
            wifiSearchBtn.disabled = false;
        }, 2000);
    }
});



// System data API

const systemElements = {
    version: document.getElementById('version'),
    free_heap_internal: document.getElementById('free_heap_internal'),
    min_heap_internal: document.getElementById('min_heap_internal'),
    free_heap_psram: document.getElementById('free_heap_psram'),
    min_heap_psram: document.getElementById('min_heap_psram'),
    time_since_boot: document.getElementById('time_since_boot'),
    res_reason: document.getElementById('res_reason'),
    active_sockets: document.getElementById('active_sockets')
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
        const formatKb = (val) => (val ?? 0).toFixed(2) + " KB ";
        systemElements.free_heap_internal.textContent = formatKb(responseData.free_heap_internal_kb);
        systemElements.min_heap_internal.textContent = formatKb(responseData.min_heap_internal_kb);
        systemElements.free_heap_psram.textContent = formatKb(responseData.free_heap_psram_kb);
        systemElements.min_heap_psram.textContent = formatKb(responseData.min_heap_psram_kb);
        systemElements.time_since_boot.textContent = formatTime(responseData.time_since_boot);
        systemElements.res_reason.textContent = responseData.res_reason;
        if (systemElements.active_sockets && responseData.active_sockets !== undefined) {
            systemElements.active_sockets.textContent = responseData.active_sockets;
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
        document.getElementById('moduleSetButton').textContent = "配置成功";
    } catch (error) {
        console.error('Error fetching data. Status:', error);
        document.getElementById('moduleSetButton').textContent = '配置失败';
    }
    setTimeout(() => document.getElementById('moduleSetButton').textContent = '保存配置', 2000);
}

document.getElementById('moduleSetButton').addEventListener('click', modulesetSubmit);

async function modulesetfetchData() {
    try {
        const responseData = await fetchData('/module_set_info');
        ['host_names', 'lgname', 'lgpwd'].forEach(id => document.getElementById(id).value = responseData[id]);
        document.getElementById('moduleSetButton').textContent = responseData.host_names != "RS485-CM 模块在线配置系统" ? '修改' : '保存';
    } catch (error) {
        console.error('Failed to fetch data. Status:', error);
    }
}

modulesetfetchData();

// OTA API

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

let pendingRestartAction = null;

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
    const deviceRestart = document.getElementById('deviceRestart');
    const cancelBtn = document.getElementById('cancelRestart');
    const restartBtn = document.getElementById('restart');

    if (!deviceRestart || !cancelBtn || !restartBtn) {
        return;
    }

    cancelBtn.addEventListener('click', () => {
        deviceRestart.style.display = 'none';
        pendingRestartAction = null;
    });
    restartBtn.addEventListener('click', async () => {
        deviceRestart.style.display = 'none';
        try {
            if (pendingRestartAction) {
                const action = pendingRestartAction;
                pendingRestartAction = null;
                await action();
                return;
            }
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
    const svg = document.getElementById('successMessage');
    if (!svg) return;

    svg.style.display = 'flex';
    document.getElementById('msgValue').textContent = message;

    // 克隆元素以重新触发动画
    const clone = svg.cloneNode(true);
    svg.parentNode.replaceChild(clone, svg);

    setTimeout(() => {
        clone.style.display = 'none';
    }, 1200);
}

function displayErrorMessage(message) {
    showCustomAlert(message, true);
}

// System banner tree

window.onload = function () {
    // Password visibility toggle
    const passwordFields = [
        { input: 'lgpwd', openEye: 'eyeOpen', closedEye: 'eyeClosed' },
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

    // 更新页面标题
    if (mainTitleElement) {
        mainTitleElement.textContent = mainTitle;
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
addEventListenerToElement("netConfig", 'netView', 'netConfig', 'base-info', '网络管理', false);
addEventListenerToElement("netConfig_m", 'netView', 'netConfig', 'base-info', '网络管理', true);




// 已移除重复的 serialButton 点击事件监听器，避免双次显示成功动画

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
        wifi_mac: "WiFi MAC",
        wifi_ip: "WiFi IP Address",
        ip_acquisition: "IP Acquisition Method",
        sensor_dashboard: "Sensor Dashboard",
        network_settings: "Network Settings",
        network_mode_selection: "Network Mode Selection",
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

        subscribeTopic: "Subscribe Topic",
        publishTopic: "Publish Topic",
        qos: "QoS",
        retain: "Retain",
        timedReport: "Timed Report",

        tcpSettings: "TCP Settings",
        tcpSubmit: "Submit",
        tcpUse: "Enable",
        tcpProtocolType: "Protocol Type",
        tcpServer: "TCP Server",
        tcpClient: "TCP Client",
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

    },
    zh: {
        basic_info: "设备状态",
        sensor_info: "传感器信息",
        device_name: "设备名称",
        current_time: "当前时间",
        // net_mode: "网络模式", // 已移除网络模式选择

        wifi_mac: "WiFi MAC",
        wifi_ip: "WiFi IP地址",
        ip_acquisition: "IP获取方式",
        sensor_dashboard: "传感器仪表",
        network_settings: "网络设置",
        network_mode_selection: "网络连接", // 已更新为网络连接

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

        subscribeTopic: "订阅主题",
        publishTopic: "发布主题",
        qos: "QOS",
        retain: "保留(Retain)",
        timedReport: "定时上报",

        tcpSettings: "TCP设置",
        tcpSubmit: "设置",
        tcpUse: "是否启用",
        tcpProtocolType: "协议类型",
        tcpServer: "TCPServer",
        tcpClient: "TCPClient",
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

    }
};

// Modbus地址过滤配置导入导出功能
function exportFilterConfig() {
    try {
        // 获取当前过滤配置
        const filterEnabled = document.getElementById('modbus_filter_enabled').checked;
        const filterMode = document.getElementById('modbus_filter_mode').value;

        // 获取所有从机地址（使用当前配置数据，因为界面已简化）
        const ranges = modbusFilterConfig.ranges.map(range => ({
            slave_id: range.slave_id || 0,
            start: 0,      // 固定为完整寄存器范围
            end: 65535     // 固定为完整寄存器范围
        }));

        const config = {
            enabled: filterEnabled,
            mode: parseInt(filterMode),
            ranges: ranges,
            exportTime: new Date().toISOString(),
            version: "2.0"  // 版本2.0 - 简化为只控制从机号
        };

        // 创建并下载文件
        const dataStr = JSON.stringify(config, null, 2);
        const dataBlob = new Blob([dataStr], { type: 'application/json' });
        const url = URL.createObjectURL(dataBlob);

        const link = document.createElement('a');
        link.href = url;
        link.download = `modbus_filter_config_${new Date().toISOString().slice(0, 19).replace(/:/g, '-')}.json`;
        document.body.appendChild(link);
        link.click();
        document.body.removeChild(link);
        URL.revokeObjectURL(url);

        displaySuccessMessage('过滤配置导出成功');
    } catch (error) {
        console.error('导出过滤配置失败:', error);
        showCustomAlert('导出过滤配置失败: ' + error.message, true);
    }
}

function importFilterConfig(event) {
    const file = event.target.files[0];
    if (!file) return;

    const reader = new FileReader();
    reader.onload = function (e) {
        try {
            const config = JSON.parse(e.target.result);

            // 验证配置格式
            if (typeof config.enabled !== 'boolean' ||
                typeof config.mode !== 'number' ||
                !Array.isArray(config.ranges)) {
                throw new Error('配置文件格式不正确');
            }

            // 应用配置
            document.getElementById('modbus_filter_enabled').checked = config.enabled;
            document.getElementById('modbus_filter_mode').value = config.mode.toString();

            // 清除现有地址范围
            const rangesContainer = document.getElementById('modbus_filter_ranges');
            rangesContainer.innerHTML = '';

            // 清除现有配置并添加导入的从机地址
            modbusFilterConfig.ranges = [];
            config.ranges.forEach(range => {
                // 添加从机地址，寄存器范围固定为0-65535
                modbusFilterConfig.ranges.push({
                    slave_id: range.slave_id || 0,
                    start: 0,
                    end: 65535
                });
            });
            
            // 更新UI显示
            updateModbusFilterUI();

            // 触发相关事件
            toggleFilterMode();
            changeFilterMode();

            displaySuccessMessage(`成功导入 ${config.ranges.length} 个过滤地址范围`);
        } catch (error) {
            console.error('导入过滤配置失败:', error);
            showCustomAlert('导入过滤配置失败: ' + error.message, true);
        }
    };

    reader.readAsText(file);
    // 清除文件选择，允许重复选择同一文件
    event.target.value = '';
}



// 串口配置 API 与状态
const SerialUIState = {
    isSubmitting: false,
    toastLocked: false,
    toastTimer: null
};

function serialSuccessOnce(message) {
    if (SerialUIState.toastLocked) return;
    SerialUIState.toastLocked = true;
    if (SerialUIState.toastTimer) clearTimeout(SerialUIState.toastTimer);
    displaySuccessMessage(message);
    SerialUIState.toastTimer = setTimeout(() => {
        SerialUIState.toastLocked = false;
    }, 1500);
}
