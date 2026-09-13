# 集线器设备接口文档

## MQTT 部分

### MQTT HTTP 接口

| 接口 | 方法 | 描述 |
| --- | --- | --- |
| `/mqtt` / `/mqtt_info` | POST / GET | 配置 MQTT 客户端、Topic、QoS、retain 等 |

所有 POST 体为 JSON；成功 `{ "code": 200 }`。

### MQTT 下发配置

设备在连接后会订阅 `mqtt_pub_topic`；向该 Topic 发布配置 JSON 即可写入 NVS（仅 `nvs_namespace`）。

#### MQTT 下发 JSON 格式

```json
{
  "action": "config",
  "data": {
    "use_mqtt": "1",
    "mqtt_server": "mqtt.likong-iot.com",
    "mqtt_port": "1883",
    "mqtt_username": "demo",
    "mqtt_password": "demo",
    "mqtt_clientid": "client-id"
  }
}
```

也支持扁平格式（当 `action="config"` 且无 `data/config/nvs` 时）：

```json
{
  "action": "config",
  "use_mqtt": "1",
  "mqtt_server": "mqtt.likong-iot.com",
  "mqtt_port": "1883"
}
```

工作模式与模板配置示例：

```json
{
  "action": "config",
  "data": {
    "work_mode": "modbus_rtu",
    "poll_time": "30000",
    "modbus_items": [
      {
        "enabled": true,
        "slave_addr": "01",
        "function_code": "03",
        "register_addr": "0",
        "register_num": "12",
        "timeout": "1000",
        "interval_time": "2000",
        "data_format": "HEX",
        "report_format": "mqtt",
        "baud_rate": "9600",
        "data_bit": "8",
        "stop_bit": "1",
        "check_bit": "None"
      }
    ]
  }
}
```

MQTT 配置示例模板：

示例 1：基础 MQTT 参数

```json
{
  "action": "config",
  "data": {
    "use_mqtt": "1",
    "mqtt_server": "mqtt.likong-iot.com",
    "mqtt_port": "1883",
    "mqtt_username": "demo",
    "mqtt_password": "demo",
    "mqtt_clientid": "client-id",
    "mqtt_sub_topic": "device/resp",
    "mqtt_pub_topic": "device/cmd",
    "qos": "1",
    "retain": "0",
    "mqtt_send": "0"
  }
}
```

示例 2：Modbus-RTU 网关 + 模板

```json
{
  "action": "config",
  "data": {
    "work_mode": "modbus_rtu",
    "poll_time": "30000",
    "modbus_items": [
      {
        "enabled": true,
        "slave_addr": "01",
        "function_code": "03",
        "register_addr": "0",
        "register_num": "2",
        "timeout": "1000",
        "interval_time": "2000",
        "data_format": "HEX",
        "report_format": "mqtt",
        "baud_rate": "9600",
        "data_bit": "8",
        "stop_bit": "1",
        "check_bit": "None"
      }
    ]
  }
}
```

示例 3：综合配置（有线 + 通信协议 + 工作模式 + 串口参数）

```json
{
  "action": "config",
  "data": {
    "netconn": "1",
    "is_dhcp": "2",
    "static_ip": "192.168.1.50",
    "static_netmask": "255.255.255.0",
    "static_gateway": "192.168.1.1",
    "static_dns1": "8.8.8.8",
    "static_dns2": "114.114.114.114",
    "use_mqtt": "1",
    "mqtt_server": "mqtt.likong-iot.com",
    "mqtt_port": "1883",
    "mqtt_username": "demo",
    "mqtt_password": "demo",
    "mqtt_clientid": "client-id",
    "mqtt_sub_topic": "device/resp",
    "mqtt_pub_topic": "device/cmd",
    "qos": "1",
    "retain": "0",
    "use_tcp": "1",
    "tcpconn": "1",
    "tcp_server": "192.168.1.100",
    "tcp_port": "8888",
    "use_http": "0",
    "work_mode": "modbus_rtu",
    "poll_time": "30000",
    "baud_rate": "9600",
    "data_bit": "8",
    "check_bit": "None",
    "stop_bit": "1",
    "frame_time": "50",
    "frame_len": "512"
  }
}
```

说明：

- `action`/`cmd`/`type` 任一字段为 `"config"` 时触发配置写入；配置内容放在 `data`（或 `config`/`nvs`）对象内。
- 仅处理字符串/数字/布尔字段，值会转成字符串存入 NVS。
- 下表列出固件当前实际使用的配置键；未列出但为字符串/数字/布尔的字段仍会被保存，但可能不会生效。
- `device_sn` / `mqttconn` / `mqtt_type` / `tcp_send` 通过 MQTT 下发会被忽略。
- 可通过 MQTT 配置 `work_mode` / `poll_time` / `modbus_items`（等同 `/work_mode_set`）。
- 成功后不会再把该消息透传到串口。
- 成功写入后设备会向 `mqtt_sub_topic` 返回 `{"code":200,"msg":"config updated"}`。
- 成功写入后设备会延迟重启（约 500ms）。

#### MQTT 下发可配置键与取值范围（按类别）

网络配置键（有线/Wi-Fi/AP）：

| 键 | 说明 | 取值范围 |
| --- | --- | --- |
| `netconn` | 网络模式 | `1`=以太网，`2`=WiFi |
| `is_dhcp` | IP 模式 | `1`=DHCP，`2`=静态IP |
| `static_ip` | 静态 IP | IPv4 字符串 |
| `static_netmask` | 子网掩码 | IPv4 字符串 |
| `static_gateway` | 网关 | IPv4 字符串 |
| `static_dns1` | 主 DNS | IPv4 字符串 |
| `static_dns2` | 备 DNS | IPv4 字符串 |
| `wifi_ssid` | WiFi SSID | 字符串 |
| `wifi_password` | WiFi 密码 | 字符串 |
| `ap_name` | AP 名称 | 字符串 |
| `ap_password` | AP 密码 | 字符串 |
| `ap_wait_time` | AP 等待时间 | `> 0` 秒 |

通信协议配置键：

MQTT：

| 键 | 说明 | 取值范围 |
| --- | --- | --- |
| `use_mqtt` | 是否启用 MQTT | `0`/`1` |
| `mqtt_server` | 服务器地址 | 主机名或 IP |
| `mqtt_port` | 服务器端口 | `1` ~ `65535` |
| `mqtt_username` | 用户名 | 字符串 |
| `mqtt_password` | 密码 | 字符串 |
| `mqtt_clientid` | Client ID | 字符串 |
| `mqtt_sub_topic` | 发布/上报 Topic | 字符串，建议 `<= 127` 字节 |
| `mqtt_pub_topic` | 订阅/下发 Topic | 字符串，建议 `<= 127` 字节 |
| `qos` | QoS | `0`/`1`/`2` |
| `retain` | retain 标志 | `0`/`1` |
| `mqtt_send` | 定时上报开关 | `0`=关闭，`2`=开启（触发 `mqtt_time`） |
| `mqtt_time` | 定时上报周期 | `> 0` 秒 |

TCP：

| 键 | 说明 | 取值范围 |
| --- | --- | --- |
| `use_tcp` | 是否启用 TCP | `0`/`1` |
| `tcpconn` | TCP 模式 | `0`=服务器，`1`=客户端，`2`=Modbus（等同服务器） |
| `tcp_server` | 服务器地址 | 主机名或 IP |
| `tcp_port` | 端口 | `1` ~ `65535` |
| `tcp_time` | 发送周期 | `> 0` 秒 |
| `reg_packet` | 注册包内容 | 字符串（空则使用设备 MAC） |
| `reg_format` | 注册包格式 | `none`/`hex`/`ascii` |
| `heart_packet` | 心跳包内容 | 字符串（空则使用设备 MAC） |
| `heart_format` | 心跳包格式 | `none`/`hex`/`ascii` |
| `heart_interval` | 心跳间隔 | `> 0` 秒（默认 30） |

UDP（预留）：

| 键 | 说明 | 取值范围 |
| --- | --- | --- |
| `use_udp` | 是否启用 UDP | `0`/`1` |
| `udpconn` | UDP 模式 | `0`=服务器，`1`=客户端 |

HTTP：

| 键 | 说明 | 取值范围 |
| --- | --- | --- |
| `use_http` | 是否启用 HTTP | `0`/`1` |
| `http_port` | 端口 | `1` ~ `65535` |
| `httpconn` | HTTP 上报开关 | `0`/`1` |
| `http_url` | 上报 URL | URL 字符串 |
| `http_header` | 自定义 Header | 字符串 |
| `http_time` | 上报周期 | `> 0` 秒 |

工作配置键（含串口参数）：

| 键 | 说明 | 取值范围 |
| --- | --- | --- |
| `work_mode` | 工作模式 | `mqtt_tcp` / `modbus_tcp` / `modbus_rtu` |
| `poll_time` | 轮询间隔 | `1000` ~ `3600000` 毫秒 |
| `modbus_items` | Modbus 模板数组 | 结构见 `/work_mode_set` 中 `modbus_items` 定义 |
| `baud_rate` | 波特率 | `1200`/`2400`/`4800`/`9600`/`19200`/`38400`/`57600`/`115200` |
| `data_bit` | 数据位 | `5`/`6`/`7`/`8` |
| `check_bit` | 校验位 | `None`/`Odd`/`Even` |
| `stop_bit` | 停止位 | `1`/`1.5`/`2` |
| `frame_time` | 帧间隔 | 正整数（毫秒） |
| `frame_len` | 帧长度 | 正整数（建议 `<= 4096`） |

系统配置键：

| 键 | 说明 | 取值范围 |
| --- | --- | --- |
| `host_names` | 设备名称 | 字符串 |
| `ntp_server` | NTP 服务器 | 域名或 IP |
| `lgname` | 登录用户名 | 字符串 |
| `lgpwd` | 登录密码 | 字符串 |
| `device_type` | 设备型号 | 字符串 |

---

## 其他接口

### 认证

| 接口 | 方法 | 描述 |
| --- | --- | --- |
| `/login` | POST | 设备 Web 登录，所有配置接口使用前需登录 |

**请求**

```json
{ "username": "admin", "password": "******" }
```

**响应**

```json
{ "code": 200, "msg": "ok" }
```

---

### 串口配置

| 接口 | 方法 | 描述 |
| --- | --- | --- |
| `/serial_set` | POST | 设置串口参数并写入 NVS，立即触发 `uart_reinit()` |
| `/serial_set_info` | GET | 获取当前串口参数 |

**字段（字符串形式）**

`baud_rate`, `data_bit`, `check_bit`, `stop_bit`, `frame_time`, `frame_len`

示例：

```json
{
  "baud_rate": "9600",
  "data_bit": "8",
  "check_bit": "None",
  "stop_bit": "1",
  "frame_time": "50",
  "frame_len": "512"
}
```

响应成功：`{ "msg": "success", "code": 200 }`

---

### 串口数据下发/监听

| 接口 | 方法 | 描述 |
| --- | --- | --- |
| `/serial_ctl` | POST | 下发指令；支持 HEX/ASCII 与分片上传 |
| `/ws/log` | WS | 串口收发数据与系统日志推送 |
| `/uart_response` | GET | 兼容接口，当前始终返回 `{}` |

**`/serial_ctl` 请求**

```json
{
  "instruction": "010300000002C40B",
  "sendType": "hex",        // hex 或 ascii
  "chunkIndex": 0,
  "chunkTotal": 1,
  "transferId": "optional-id"
}
```

成功响应：`{ "code": 200 }`

WebSocket `message` 示例：

```json
{
  "type": "uart_data",
  "is_tx": false,
  "hex": "0103020000f9f6",
  "ascii": "\u0001\u0003\u0002\u0000\u0000ùö",
  "timestamp": 1720000123456
}
```

---

### 工作模式与 Modbus 模板

| 接口 | 方法 | 描述 |
| --- | --- | --- |
| `/work_mode_set` | POST | 设置工作模式、轮询间隔、模板；会校验协议启用状态 |
| `/work_mode_info` | GET | 获取当前配置（支持分页返回模板） |

`/work_mode_info` 查询参数（可选）：

- `items_offset`：起始索引（从 0 开始）
- `items_limit`：返回条数（`0` 表示不返回 `modbus_items`），默认 `10`

响应新增字段：

- `modbus_items_total`：模板总数
- `modbus_items_offset`：本次返回起始索引
- `modbus_items_limit`：本次返回条数

**`/work_mode_set` 请求示例**

```json
{
  "work_mode": "modbus_rtu",
  "poll_time": "30000",   // 单位 ms
  "modbus_items": [
    {
      "enabled": true,
      "slave_addr": "01",
      "function_code": "03",
      "register_addr": "0",
      "register_num": "12",
      "timeout": "1000",
      "interval_time": "2000",
      "data_format": "HEX",
      "report_format": "mqtt",
      "baud_rate": "9600",
      "data_bit": "8",
      "stop_bit": "1",
      "check_bit": "None"
    }
  ]
}
```

注意：

- `work_mode` 可选值：
  - `mqtt_tcp`：MQTT/TCP 透传
  - `modbus_tcp`：Modbus-TCP 转 Modbus-RTU
  - `modbus_rtu`：Modbus-RTU 网关
- `poll_time` 自动限制在 1s ~ 3600s（毫秒存储）
- 若启用模板引用的协议（MQTT/TCP/HTTP）未配置，返回 `{code:400,"msg":"请先在协议管理中启用..."}`。
- 切换模式会停止旧任务并启动新的 Modbus 任务。

**字段取值范围（`modbus_items`）**

- `enabled`：`true`/`false`
- `slave_addr`：`1` ~ `255`（十进制字符串）
- `function_code`：`01` / `02` / `03` / `04`
- `register_addr`：`0` ~ `65535`（支持十进制或 `0x` 前缀十六进制）
- `register_num`：`>= 1`（十进制字符串）
- `timeout`：`> 0`（毫秒字符串）
- `interval_time`：`> 0`（毫秒字符串）
- `data_format`：`Signed` / `Unsigned` / `HEX` / `Binary` / `Long` / `Float` / `Double` / `LongInverse` / `FloatInverse` / `DoubleInverse`
- `report_format`：`mqtt` / `tcp` / `http`
- `baud_rate`：`1200` / `2400` / `4800` / `9600` / `19200` / `38400` / `57600` / `115200`
- `data_bit`：`5` / `6` / `7` / `8`
- `check_bit`：`None` / `Odd` / `Even`
- `stop_bit`：`1` / `1.5` / `2`

备注：Web 端导入时最多 50 条 `modbus_items`，超出会截断。

---

### TCP/HTTP 协议配置

| 接口 | 方法 | 描述 |
| --- | --- | --- |
| `/tcp` / `/tcp_info` | POST / GET | 配置 TCP 透传/注册包/心跳包等 |
| `/http` / `/http_info` | POST / GET | 配置 HTTP 上报 URL、Header |

所有 POST 体为 JSON；成功 `{ "code": 200 }`。

---

### 网络 & 模块信息

| 接口 | 方法 | 描述 |
| --- | --- | --- |
| `/net_set` / `/net_set_info` | POST / GET | 设置/获取 DHCP/静态 IP、网关、DNS、Wi-Fi SSID/密码 |
| `/module_set` / `/module_set_info` | POST / GET | 设置/读取设备名称、登录账号 |
| `/devinfo` | GET | 返回 MAC、IP、网关、联网状态、版本等概览 |
| `/find_wifi` | GET | Wi-Fi 扫描结果 |

`/find_wifi` 响应示例：

```json
{
  "count": 3,
  "list": [
    { "ssid": "AP1", "rssi": -45, "channel": 6, "auth": "WPA2" },
    ...
  ]
}
```

---

### 日志控制

| 接口 | 方法 | 描述 |
| --- | --- | --- |
| `/api/log_switch` | GET / POST | 获取/设置日志推送开关（保存在 `nvs_namespace`） |

POST 请求：`{ "log_enabled": true }`。

---

### OTA

| 接口 | 方法 | 描述 |
| --- | --- | --- |
| `/ota` | POST | 启动 HTTP/HTTPS OTA |
| `/ota_progress` | GET | 轮询 OTA 进度百分比和状态文本 |

`/ota` 请求：`{ "ota_url": "https://example.com/fw.bin" }`。

---

### PC 端设备发现（UDP 37210）

在 `search/main.py` 中实现的桌面工具与设备约定 UDP 协议：

| Payload | 说明 | 设备响应 |
| --- | --- | --- |
| `{"device":"scan"}` | 广播扫描 | 返回设备 JSON（`type/name/mac/ip/netmask/gateway/version/DNS`） |
| `{"device":"exec","parm":"device_info"}` | 执行远程命令 | 返回命令结果（JSON 或文本） |
| `{"username":"admin","password":"***"}` | 认证 | 返回 `{"result":"ok"}` 表示通过 |
| 认证成功后发送 `{ "device":"change", "name":"xxx", "ip":"...", "mask":"...", "gateway":"...", "dns1":"...", "dns2":"..." }` | 修改网络参数并重启 | 成功 `{"change":"ok"}` |

该工具 UI 由 `scan_device_ui.py` 定义，支持扫描、Web 打开、远程命令、在线更新等。

---

### 其他

- 所有 HTTP JSON 字段在 `nvs_namespace` 中以字符串保存，请按字符串格式传递。
- WebSocket `/ws/log` 同时输出串口数据和系统日志，可配合 `/api/log_switch` 控制推送。
- HTTP 接口基于 ESP-IDF `httpd`，需浏览器先调用 `/login` 建立会话后再访问配置端点。
