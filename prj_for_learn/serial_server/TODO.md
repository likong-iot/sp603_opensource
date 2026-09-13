# OpenLK Serial Server - 待办任务清单

## 🎯 当前任务：删除工作模式，适配Web界面

### 1. 简化Core层 ✅
**目标：** 将Core Services从"框架"简化为"工具类"
- [ ] 删除预定义的50+配置项，改为通用NVS封装
- [ ] 删除预定义的50+事件类型，改为通用事件机制
- [ ] 保留Task Manager作为可选工具

### 2. 删除工作模式业务逻辑 ⚠️
**目标：** 移除所有work_mode相关代码

**文件：** `main/sx_web_server.c` (33处)
- [ ] 删除 work_mode 配置读写
- [ ] 删除 work_mode_set_handler
- [ ] 删除 work_mode 结构体定义
- [ ] 保留基础的HTTP Server和REST API

**其他文件：**
- [ ] `main/sx_async_uart.c` - 删除工作模式判断
- [ ] `main/sx_tcp_server.c` - 删除工作模式判断
- [ ] `main/sx_mqtt_client.c` - 删除工作模式判断
- [ ] `main/sx_main.c` - 删除工作模式初始化

### 3. 适配Web前端 ⚠️
**文件：** `main/static/`
- [ ] `web.html` - 删除工作模式选择界面
- [ ] `web.js` - 删除工作模式相关逻辑
- [ ] 保留：串口调试、网络配置、MQTT配置

### 4. 整理SDK组件 ⚠️
**目标：** 确保基础设施完整

**已有但需要整理：**
- [ ] WiFi初始化 (sx_init_wifi.c) → 提取为HAL或Protocol层
- [ ] Ethernet初始化 (sx_init_eth.c) → 提取为HAL层
- [ ] HTTP Server (sx_web_server.c) → 提取为Protocol层
- [ ] Modbus协议 → 检查是否完整

**缺失需要补充：**
- [ ] Modbus TCP协议封装
- [ ] UDP协议（已有sx_udp_server.c，需要提取）

### 5. 文档更新 ⚠️
- [ ] 更新README：说明Web界面使用
- [ ] 更新API文档：新的接口
- [ ] 创建快速开始指南

---

## 📊 当前项目状态

### ✅ 已完成
- HAL层：UART、GPIO
- Protocol层：TCP Server、MQTT Client
- Core层：Config/Event/Task Manager（需要简化）
- 示例：4个完整示例

### ⚠️ 正在进行
- 删除工作模式
- 简化Core层
- 适配Web界面

### ❌ 待开始
- 提取WiFi/HTTP/Ethernet为SDK组件
- Modbus TCP协议完整性检查
- 最终测试和验证

