# devmond 协议（版本 1）

Unix 域流式套接字，默认 `/tmp/devmond.sock`（`devmond -s` 修改，界面用环境变量 `DEVMOND_SOCKET`）；
`devmond -t 9100` 时同一协议再监听 TCP 端口，供远程访问（`devmonctl -H 板子IP`，界面用 `DEVMOND_ADDR=IP:9100`）。
TCP 没有认证和加密，只在可信的局域网中使用。
一行一条消息，以 `\n` 结尾，UTF-8。格式：`类型 key=value key=value ...`；
`msg=` 若出现总在行尾，其值可以包含空格。最多 8 个客户端同时连接。

调试：`devmonctl watch`（打印推送）、`devmonctl STATUS`、`devmonctl LED toggle` 等。

## 连接时

服务端先发 `HELLO`，再把当前完整状态发一遍（`CONFIG`、`LED`、`KEY`、`REC`，以及已知的 `PROX`、
设备打开失败时的 `AP_ERR` / `IMU_ERR`），客户端无需关心自己何时连上。

```
HELLO devmond proto=1
CONFIG on=20 off=50 interval=1000 imu_rate=100 path=/device-management/device_monitor.ini
LED state=off auto=0
KEY state=ready
REC state=off dir=/device-management/records
```

## 服务端推送

| 消息 | 时机 | 字段 |
| --- | --- | --- |
| `AP ir= als= lux= ps= flags=` | 每个采集周期、`READ` 后 | 原始值；`lux` = als × in_illuminance_scale；`flags` 位 1/2/4 = IR/ALS/PS 有效（IR 溢出时 IR、PS 无效） |
| `IMU n= rate= ax= ay= az= gx= gy= gz= temp=` | 每个采集周期 | 本周期 n 个样本的平均值，g / dps / °C；`rate` 按内核时间戳计算 |
| `SYS cpu= self_cpu= mem_used= mem_total= load= uptime= temp= rx= tx= disk_free= disk_total= time= ntp=` | 每个采集周期 | CPU %（与上周期的差值）与 devmond 自身 %（最近 10 个周期的平均，节拍分辨率所限）、内存 MB、1 分钟负载、运行秒数、SoC 温度 °C（无传感器时 -273）、除 lo 外网卡收发 KB/s、录制目录所在文件系统 MB（不可用时 -1）、板子当前时间（CLOCK_REALTIME 毫秒）、内核时钟是否已被 NTP 校准（adjtimex 无 STA_UNSYNC 为 1） |
| `PROX state=near\|far count= ts=` | AP_INT 接近中断 | `ts` 为驱动在中断上半部记录的 CLOCK_REALTIME 纳秒；连接时补发的当前状态不带 `ts` |
| `LED state=on\|off\|unavailable auto=0\|1` | LED 或自动模式变化 | |
| `KEY state=ready\|pressed\|released\|unavailable` | 按键 | 手动模式下按下会翻转 LED |
| `CONFIG on= off= interval= imu_rate= path=` | 连接时、配置变化 | `imu_rate` 只能在 ini 中修改，重启生效 |
| `REC state=on\|off dir=` / `REC state=error msg=` | 录制开始、停止、写入失败 | |
| `AP_ERR msg=` / `IMU_ERR msg=` | 设备打开或读取失败、一个周期没有 IMU 样本 | |
| `MQTT state=off` / `MQTT state=connecting\|connected broker= topic=` / `MQTT state=retry broker= topic= retry= msg=` | 连接时、MQTT 连接状态变化 | `retry` 为发出这条消息时距下次重试的秒数；`msg` 为上次失败的原因 |

## 客户端命令

| 命令 | 应答 | 说明 |
| --- | --- | --- |
| `PING` | `PONG` | |
| `STATUS` | 完整状态 + `OK STATUS` | |
| `LED on\|off\|toggle` | `OK LED` / `ERR LED msg=` | 自动模式下拒绝 |
| `AUTO on\|off` | `OK AUTO` | ALS ≤ on 开灯、≥ off 关灯、中间保持；启动时为手动 |
| `CONFIG on=N off=N interval=MS` | `OK CONFIG` / `ERR CONFIG msg=` | 可只给部分字段；校验后立即生效并原子写回 ini |
| `READ` | `OK READ` | 立即读一次 AP3216C（结果以 `AP` 广播） |
| `REC start\|stop` | `OK REC` / `ERR REC msg=` | 录制在守护进程中进行，界面退出不影响 |

状态变化（LED、CONFIG、REC）以广播通知所有客户端，命令应答只发给发命令的客户端。

## MQTT 桥接

`devmond -m 192.168.10.1[:1883] [-M 主题前缀]`（启动脚本里为 `MQTT_BROKER`、`MQTT_TOPIC`）时，devmond 同时作为 MQTT 3.1.1 客户端连接 broker，把上面的推送和命令原样桥接过去。前缀默认为 `devmon/<主机名>`，下文记作 `P`。

| 主题 | 方向 | 内容 |
| --- | --- | --- |
| `P/status` | 板 → broker | `online`（连上时发布）/ `offline`（遗嘱：断线、断电、进程被杀时由 broker 代发），都是 retained |
| `P/<类型小写>` | 板 → broker | 每条推送一条消息：`P/imu`、`P/ap`、`P/sys`、`P/led`、`P/key`、`P/prox`、`P/config`、`P/rec`、`P/ap_err`、`P/imu_err`。载荷为 JSON：字段同上表，符合 JSON 数字语法的值为数字，其余为字符串，末尾加 `ts`（板子时间，毫秒） |
| `P/cmd` | broker → 板 | 载荷就是一条客户端命令（`LED on`、`AUTO on`、`CONFIG interval=500`、`REC start`、`STATUS` …），只取第一行 |
| `P/reply` | 板 → broker | 命令应答原文（`OK LED`、`ERR LED msg=auto mode is on`、`PONG`），以及广播的 `ERR` |

```
$ mosquitto_sub -h 192.168.10.1 -v -t 'devmon/#'
devmon/alpha/status online
devmon/alpha/led {"state":"off","auto":0,"ts":1795264585605}
devmon/alpha/imu {"n":100,"rate":100.1,"ax":-0.028,"ay":0.013,"az":1.007,"gx":-0.61,"gy":-0.06,"gz":-0.18,"temp":27.47,"ts":1795264586605}
$ mosquitto_pub -h 192.168.10.1 -t devmon/alpha/cmd -m 'LED on'
```

- `led`、`config`、`rec` 为 retained：新订阅者立即拿到当前状态；连上（包括重连）时 devmond 把完整状态发一遍，retained 内容随之刷新。
- 发送全部为 QoS 0：遥测每个周期都会刷新，丢一条无妨；broker 跟不上、发送缓冲（32 KB）满时直接丢弃并在日志里计数，不会阻塞采集。
- `P/cmd` 以 QoS 1 订阅（执行后回 PUBACK）。带 retain 标志的命令被忽略：订阅时 broker 会补发保留消息，每次重连都执行一遍是错的。
- 断线检测：超过 30 s 没收到 broker 的任何报文就发 PINGREQ，再过 30 s 没有回应就断开重连；重试间隔 1、2、4 … 30 s。
- broker 只接受 IPv4 地址：解析主机名可能阻塞单线程的主循环。
- 没有 TLS 和认证，只在可信的局域网中使用（与 TCP 9100 相同）。

## 信号

`SIGTERM` / `SIGINT`：停止录制（刷盘）、关 LED、关 IIO 缓冲区和接近中断、删除套接字与 pid 文件后退出。
`SIGHUP`：重新读取 ini。
