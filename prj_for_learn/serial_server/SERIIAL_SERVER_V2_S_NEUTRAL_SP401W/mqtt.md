# MQTT 接口文档（基于当前代码实现）

本文档按当前仓库代码整理（`main/sx_web_server.c`、`main/sx_mqtt_client.c`、`main/sx_main.c`、`main/sx_modbus.c`）。

## 1. MQTT 相关接口总览

| 接口类型 | 接口/主题 | 方向 | 作用 |
| --- | --- | --- | --- |
| HTTP | `POST /mqtt` | 前端 -> 设备 | 保存 MQTT 参数到 NVS |
| HTTP | `GET /mqtt_info` | 前端 <- 设备 | 查询 MQTT 当前配置与连接状态 |
| MQTT Topic | `mqtt_pub_topic` | 云端 -> 设备 | 设备订阅的下行主题（命令/配置） |
| MQTT Topic | `mqtt_sub_topic` | 设备 -> 云端 | 设备上报主题（数据/ACK） |
| MQTT 消息 | 配置下发 JSON | 云端 -> 设备 | 远程写 NVS（可更新 `work_mode`、`poll_time`、`modbus_items` 等） |
| MQTT 消息 | 普通二进制/文本数据 | 云端 -> 设备 | 非配置消息透传到串口 UART |
| MQTT 消息 | `{"code":200,"msg":"config updated"}` | 设备 -> 云端 | 配置写入成功 ACK |

---

## 2. HTTP 接口

## 2.1 `POST /mqtt`

- 方法：`POST`
- 作用：保存 MQTT 配置到 `nvs_namespace`
- 返回：`{"msg":"success","code":200}`
- 实现路径：`get_mqtt_post_handler -> post_handler -> save_to_nvs`

### 请求格式

前端当前使用 `application/json` 提交。后端同时兼容：

1. JSON（对象）
2. `key=value&...` 形式

### 生效规则

- `save_to_nvs()` 只处理**顶层标量字段**（字符串/数字/布尔）。
- 不做严格范围校验，基本按字符串写入 NVS。
- 若需要范围约束，依赖前端或后续业务逻辑。

## 2.2 `GET /mqtt_info`

- 方法：`GET`
- 作用：读取 MQTT 配置并返回给前端
- 返回字段：
  - `use_mqtt`
  - `mqtt_type`
  - `mqtt_server`
  - `mqtt_port`
  - `mqtt_username`
  - `mqtt_password`
  - `mqtt_clientid`
  - `mqtt_sub_topic`
  - `mqtt_pub_topic`
  - `qos`
  - `retain`
  - `mqtt_send`
  - `mqtt_time`
  - `mqttconn`

### 连接状态字段说明

- `mqttconn` 最终返回的是**运行时实际连接状态**（`get_mqtt_connection_status()`），不是单纯 NVS 旧值。
- 若 `use_mqtt != "1"`，则强制返回 `mqttconn = "0"`。

---

## 3. MQTT 连接与主题

## 3.1 连接参数来源

设备启动 MQTT 时从 NVS 读取：

- `mqtt_server`
- `mqtt_port`
- `mqtt_username`
- `mqtt_password`
- `mqtt_clientid`

连接 URI 形式：

`mqtt://<mqtt_username>:<mqtt_password>@<mqtt_server>:<mqtt_port>`

## 3.2 主题方向（非常重要）

- 设备**订阅**：`mqtt_pub_topic`（下行）
- 设备**发布**：`mqtt_sub_topic`（上行）

> 命名上 `pub/sub` 与常见云平台语义可能相反，请以“设备行为”判定。

## 3.3 默认值（首次初始化）

来自 `main/sx_main.c`：

- `use_mqtt = "1"`
- `mqtt_type = "0"`
- `mqtt_server = "mqtt.likong-iot.com"`
- `mqtt_port = "1883"`
- `mqtt_username = "public"`
- `mqtt_password = "Aa123456"`
- `mqtt_clientid = <CLIENT_HEAD + STA_MAC>`
- `mqtt_pub_topic = /public/<sta_mac>/publish`（设备订阅）
- `mqtt_sub_topic = /public/<sta_mac>/subscribe`（设备上报）
- `qos = "0"`
- `retain = "0"`
- `mqtt_send = "0"`
- `mqtt_time = "5"`

---

## 4. MQTT 下发配置接口（Broker -> 设备）

设备在 `MQTT_EVENT_DATA` 中优先尝试按“配置消息”处理。

## 4.1 识别规则

满足以下任一条件会按配置处理：

- 顶层存在 `action/cmd/type` 任一字段，且值为 `"config"`
- 配置对象位于 `data` / `config` / `nvs`
- 当 `action=\"config\"` 且无 `data/config/nvs` 时，允许扁平对象直接作为配置体

## 4.2 支持的字段类型

- 仅处理字符串/数字/布尔
- 最终统一转字符串写入 NVS

## 4.3 特殊处理字段

- `work_mode`：仅允许
  - `mqtt_tcp`
  - `modbus_tcp`
  - `modbus_rtu`
- `poll_time`：单位毫秒，自动钳制到 `1000 ~ 3600000`
- `modbus_items`：数组，最多 `MODBUS_ITEM_MAX=50` 项

## 4.4 下发时会忽略的字段

- `device_sn`
- `mqttconn`
- `mqtt_type`
- `tcp_send`

## 4.5 成功后的行为

1. 写入并 `nvs_commit`
2. 向 `mqtt_sub_topic` 发布 ACK：
   - `{"code":200,"msg":"config updated"}`
3. 延迟约 500ms 后 `esp_restart()`

> 这意味着如果命令主题上存在 retained 配置消息，设备重连后可能再次收到并循环重启。

---

## 5. MQTT 普通下发透传接口（Broker -> UART）

- 若消息**未被识别为配置消息**，则按原始 payload 透传到 UART（`tx_tasks`）。
- 用于 MQTT 下发串口指令。

---

## 6. 设备 MQTT 上报接口（设备 -> Broker）

所有上报均发送到 `mqtt_sub_topic`，QoS/retain 取 NVS 中 `qos`、`retain`。

## 6.1 配置 ACK 上报

- Payload：`{"code":200,"msg":"config updated"}`
- 场景：配置消息写入成功

## 6.2 `mqtt_tcp` 模式上报

- 函数：`mqtt_public_send_mqtt_tcp_data`
- 内容：串口原始字节流（binary）

## 6.3 `modbus_tcp` 模式上报

- 函数：`mqtt_public_send_modbus_tcp_data`
- 内容：Modbus TCP 原始字节流（含 MBAP）

## 6.4 `modbus_rtu` 模式上报

- 函数：`mqtt_public_send_device_info`
- 内容：JSON（模板配置 + 解析后的响应数据）
- 关键字段示例：
  - `enabled`
  - `command_index`
  - `slave_addr`
  - `function_code`
  - `register_addr`
  - `register_num`
  - `timeout`
  - `interval_time`
  - `data_format`
  - `report_format`
  - `response_data`

---

## 7. MQTT 配置字段取值范围与作用

| 字段 | 作用 | 取值范围（建议/实际） |
| --- | --- | --- |
| `use_mqtt` | MQTT 总开关 | `0`/`1` |
| `mqtt_type` | MQTT 协议类型 | 当前仅 `0` 生效（`1` 在代码中为 TODO） |
| `mqtt_server` | Broker 地址 | 主机名/IP 字符串 |
| `mqtt_port` | Broker 端口 | 建议 `1~65535` |
| `mqtt_username` | MQTT 用户名 | 字符串 |
| `mqtt_password` | MQTT 密码 | 字符串 |
| `mqtt_clientid` | Client ID | 字符串 |
| `mqtt_pub_topic` | 设备订阅主题（下行） | 字符串，建议 `<=127` 字节 |
| `mqtt_sub_topic` | 设备发布主题（上行） | 字符串，建议 `<=127` 字节 |
| `qos` | 上报 QoS | `0`/`1`/`2` |
| `retain` | 上报 Retain | `0`/`1` |
| `mqtt_send` | 定时上报开关 | `0`=关，`2`=开（当前任务框架存在，具体上报内容尚未实现） |
| `mqtt_time` | 定时上报周期 | 建议 `>0` 秒 |
| `mqttconn` | MQTT 连接状态 | 只读语义，运行时状态 `0/1` |

### `modbus_items`（通过 MQTT 配置时）

| 字段 | 作用 | 取值范围/备注 |
| --- | --- | --- |
| `enabled` | 模板启用 | `true/false` 或 `1/0` |
| `slave_addr` | 从站地址 | 建议 `1~247` |
| `function_code` | 功能码 | 建议 `1~6` |
| `register_addr` | 起始寄存器地址 | 建议 `0~65535` |
| `register_num` | 寄存器数量 | 建议 `1~125` |
| `timeout` | 等待超时(ms) | 运行时会修正到合理范围（过小/过大会调整） |
| `interval_time` | 命令间隔(ms) | `<=0` 时运行时会给默认值 |
| `data_format` | 数据格式 | 如 `HEX` / `Signed` / `Unsigned` / `Float` / `Long` / `Double` 等 |
| `report_format` | 上报目标协议 | 运行时分支使用小写：`mqtt`/`tcp`/`http` |
| `baud_rate` | 波特率 | 串口参数字符串 |
| `data_bit` | 数据位 | `5/6/7/8` |
| `stop_bit` | 停止位 | `1/1.5/2` |
| `check_bit` | 校验位 | `None`/`Odd`/`Even` |

---

## 8. 实际使用建议

1. 下发配置消息时，建议 `retain=0`，避免设备重启后重复消费旧配置。
2. 远程配置建议统一用 `action:"config" + data:{...}` 结构，便于兼容。
3. `report_format` 建议使用小写（`mqtt/tcp/http`），与运行时分支保持一致。
4. 若仅更新部分 MQTT 参数，使用 `POST /mqtt`（HTTP）更直接；若需要远程无人值守，使用 MQTT 配置下发。

