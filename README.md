# 边缘温度监测节点（temp-sentry）

MCU 经串口发送温度，Linux 网关判定是否越界并经 MQTT 上报，判定参数支持上位机远程下发。

## 编译

```bash
sudo apt install gcc libmosquitto-dev
make
```

## 接线

- MCU UART ↔ 板卡串口引脚直连：TX/RX 交叉，**必须共地**
- 设备节点 `/dev/ttyS3`，不要占用调试串口 `ttyFIQ0`

## 运行

```bash
sudo ./temp-sentry -H <broker地址>          # broker 跑在 PC 上时填 PC 的局域网 IP
```

| 选项 | 说明 | 默认值 |
|------|------|--------|
| `-d` | 串口设备节点 | `/dev/ttyS3` |
| `-b` | 波特率 | `115200` |
| `-H` | MQTT broker 地址 | `127.0.0.1` |
| `-p` | MQTT broker 端口 | `1883` |
| `-t` | 节点编号（决定 topic 前缀） | `node01` |

## MQTT 接口

### 遥测 `edge/<node>/telemetry`（上行，QoS 0，每 30s 一条）

```json
{"temp": 60.5, "state": "normal", "ts": 1734567890123}
```

`state` 取 `normal` / `out_of_range`；超时收不到即为节点掉线。

### 事件 `edge/<node>/event`（上行，QoS 1，成对出现）

```json
{"event": "temp_violation", "level": "error", "nominal": 60.0, "tolerance": 2.0, "measured": 66.5, "ts": 1734567890123}
{"event": "temp_recovered", "level": "info",  "nominal": 60.0, "tolerance": 2.0, "measured": 60.8, "ts": 1734567891123}
```

进入越界发 `temp_violation`，回到范围发 `temp_recovered`，两个 `ts` 之差即越界持续时间。

### 参数下发 `edge/<node>/cmd`（下行）

```json
{"nominal": 60.0, "tolerance": 3.0}
```

正常范围 = `[nominal - tolerance, nominal + tolerance]`。两个字段必须同时存在且 `tolerance > 0`，非法下发直接拒绝；下发立即生效，无需重启。默认判定范围 60 ± 2，换工况时建议先下发参数再改变温度，避免过渡期误报。

## 串口帧格式（MCU 侧）

变长帧 = 7 字节头 + 正文 + 3 字节尾：

| 字段 | 长度 | 说明 |
|------|:----:|------|
| 帧头 | 2B | `0xA5 0xA5` |
| 类型 | 1B | 温度帧用 `0x01` |
| 长度 | 4B | 正文长度，大端 |
| 正文 | ≤1024B | 温度 ASCII 文本，如 `"58.08"` |
| 校验 | 1B | 正文逐字节 XOR |
| 帧尾 | 2B | `0xA5 0xA5` |

## 上位机操作示例（Windows PowerShell）

```powershell
# 订阅全部消息
.\mosquitto_sub.exe -h 127.0.0.1 -t "edge/#" -v

# 下发参数（PowerShell 会吞参数内嵌引号，需写成 \"）
.\mosquitto_pub.exe -h 127.0.0.1 -t edge/node01/cmd -m '{\"nominal\":40.0,\"tolerance\":2.0}'
```
