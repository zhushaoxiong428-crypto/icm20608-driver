# devmond 协议（版本 1）

Unix 域流式套接字，默认 `/tmp/devmond.sock`（`devmond -s` 修改，界面用环境变量 `DEVMOND_SOCKET`）。
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
| `PROX state=near\|far count= ts=` | AP_INT 接近中断 | `ts` 为驱动在中断上半部记录的 CLOCK_REALTIME 纳秒；连接时补发的当前状态不带 `ts` |
| `LED state=on\|off\|unavailable auto=0\|1` | LED 或自动模式变化 | |
| `KEY state=ready\|pressed\|released\|unavailable` | 按键 | 手动模式下按下会翻转 LED |
| `CONFIG on= off= interval= imu_rate= path=` | 连接时、配置变化 | `imu_rate` 只能在 ini 中修改，重启生效 |
| `REC state=on\|off dir=` / `REC state=error msg=` | 录制开始、停止、写入失败 | |
| `AP_ERR msg=` / `IMU_ERR msg=` | 设备打开或读取失败、一个周期没有 IMU 样本 | |

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

## 信号

`SIGTERM` / `SIGINT`：停止录制（刷盘）、关 LED、关 IIO 缓冲区和接近中断、删除套接字与 pid 文件后退出。
`SIGHUP`：重新读取 ini。
