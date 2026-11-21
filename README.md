# i.MX6ULL 嵌入式设备监测与管理系统

基于正点原子 i.MX6ULL ALPHA 开发板的嵌入式 Linux 项目。通过自定义设备树和 Linux 驱动采集板载传感器数据，在 LCD 触摸屏上使用 Qt Widgets 显示（默认正点原子 4.3 寸 800×480，另有 480×272、1024×600 布局），并实现 LED 手动／自动控制、参数保存和 CSV 数据记录。

项目包含设备驱动、C 测试程序和 Qt 应用源码；BSP 内核、交叉工具链、Qt 运行库和字体需单独准备。

仓库由两部分组成：

| 目录 | 内容 |
| --- | --- |
| `icm20608-driver/` | ICM-20608 六轴 IMU 的 IIO 驱动：设备树 → SPI 驱动 → regmap → IIO → 数据就绪中断 + triggered buffer，v0～v4 的开发过程与负载下的实测对比见其 [README](icm20608-driver/README.md)。原独立仓库的提交历史完整保留（`git log -- icm20608-driver`） |
| 其余目录 | 设备监测与管理系统：LED、按键、AP3216C、GT911/GT1158 驱动，设备树，测试程序，Qt 界面 |

设备监测部分移植自 [z1480754235-gif/imx6ull-device-monitor](https://github.com/z1480754235-gif/imx6ull-device-monitor)（原作者的提交保留在历史中），在此基础上适配 ALPHA V2.4 + ATK-4384 屏、支持 GT1158、改用上面的 IIO 驱动采集 ICM-20608，并补充小屏布局与启动脚本。

## 第一版功能

| 模块 | 已实现内容 |
| --- | --- |
| LED | 字符设备控制、触摸按钮和实体按键控制 |
| 实体按键 | GPIO 中断、延迟工作消抖、Input 事件上报 |
| AP3216C | IIO 驱动（regmap-i2c）：照度（4 档量程，可换算 lux）、接近、红外三通道，每通道一次 I²C 传输；AP_INT 中断推送接近事件（滞回阈值，靠近/离开各一次） |
| ICM20608 | 自研 IIO 驱动（子模块）：regmap-spi，数据就绪中断 + triggered buffer，中断上半部记内核时间戳；应用默认 100 Hz 经 IIO 缓冲区批量读取，不丢样 |
| GT911 / GT1158 | I²C 通信、线程化中断、最多 5 个触点的多点触摸上报；兼容 GT9147 / GT1151 / GT1158（ATK-4384 屏为 GT1158） |
| UART3 | 使用 BSP 串口驱动进行 115200、8N1 收发回环测试 |
| Qt 界面 | Monitor / System / Settings / Record 四页（小屏布局），数据刷新、设备状态和错误提示 |
| 网络与时间 | `devmond -t 9100` 同一协议监听 TCP，PC 上 `devmonctl -H 板子IP` 或同一 Qt 界面（`DEVMOND_ADDR=IP:9100`）远程查看与控制；busybox `ntpd` 向主机校时，校准后写入 SNVS RTC，界面显示同步状态；本地时区北京时间 |
| 系统监控 | CPU（及 devmond 自身）、内存、负载、运行时间、SoC 温度、网络收发速率、录制目录剩余空间，最近 120 个周期的 CPU / 温度曲线 |
| 自动控制 | ALS 双阈值控制 LED，支持手动／自动切换 |
| 参数设置 | 修改光照阈值和采集间隔，由 devmond 校验并原子写回 ini |
| 采集守护进程 | devmond：epoll + timerfd + signalfd 单线程事件循环，Unix 域套接字文本行协议，多客户端；界面退出后采集与录制继续 |
| 数据记录 | 两路传感器分别保存带时间戳的 CSV，支持开始／停止记录 |

UART 使用内核已有的 i.MX 串口驱动，本项目提供用户态回环测试程序。

## 硬件与软件环境

| 项目 | 开发环境 |
| --- | --- |
| 开发板 | 正点原子 i.MX6ULL ALPHA，eMMC 版本 |
| CPU | ARM Cortex-A7，32 位，小端 |
| Linux | BSP 内核 4.1.15 |
| 工具链 | Linaro GCC 4.9.4，`arm-linux-gnueabihf-` |
| Qt | Qt 5.12.9，Widgets，C++11 |
| 显示屏 | 1024×600 LCD，GT911 电容触摸 |
| 显示接口 | `/dev/fb0`，当前使用 32 bpp framebuffer |
| 调试部署 | 串口终端、TFTP 内核／设备树、NFS 根文件系统 |

驱动接口按 Linux 4.1.15 编写，移植到其他内核版本时可能需要调整相关 API。

## 目录结构

```text
.
├── Makefile                       # 设备树、驱动模块和 C 应用构建入口
├── app/                           # 单外设和联动测试程序
├── daemon/                        # 采集守护进程 devmond 与命令行客户端 devmonctl（C）
│   └── protocol.md                # 界面与守护进程之间的文本行协议
├── drivers/                       # LED、按键、AP3216C、GT911 驱动
│   └── Makefile                   # 内核模块 Kbuild 配置
├── dts/
│   ├── imx6ull-device-management.dts   # 项目设备树编译入口
│   └── imx6ull-device-management.dtsi  # 项目外设配置
├── include/                       # 驱动与应用共享的数据结构
├── icm20608-driver/               # ICM20608 IIO 驱动、iio_buffer 等测试、实测报告
└── qt/device_monitor/
    ├── device_monitor.pro         # qmake 工程
    ├── main.cpp                   # 应用入口
    ├── mainwindow.h/.cpp          # 显示守护进程推送的状态，按钮转成命令
    ├── daemonclient.h/.cpp        # QLocalSocket 客户端，断线自动重连
    ├── mainwindow.ui              # Designer 界面（ui_800x480/、ui_480x272/ 为小屏布局）
    └── run_device_monitor.sh      # 开发板启动脚本：驱动 → devmond → 界面
```

`build/`、内核模块编译产物、Qt Creator 的本机配置、运行配置和采集文件不提交到仓库。

## 引脚与设备节点

以下以当前开发板原理图及项目设备树为准：

| 设备 | 接口／引脚 | 用户态入口 |
| --- | --- | --- |
| LED | GPIO1_IO03，低电平有效 | `/dev/led0` |
| 按键 | GPIO1_IO18，低电平有效，`KEY_ENTER` | `/dev/input/eventX` |
| AP3216C | I2C1，7 位地址 `0x1e`；INT（AP_INT）：GPIO1_IO01，低电平有效 | `/sys/bus/iio/devices/iio:deviceN`（`in_illuminance_*`、`in_proximity_raw`、`in_intensity_ir_raw`、`events/`） |
| ICM20608 | ECSPI3，GPIO1_IO20 软件片选，SPI Mode 0；数据就绪中断 GPIO1_IO10 上升沿 | `/sys/bus/iio/devices/iio:deviceN`、`/dev/iio:deviceN` |
| UART3 | UART3_TX_DATA / UART3_RX_DATA | `/dev/ttymxc2` |
| GT911 / GT1158 | I2C2，7 位地址 `0x14`；INT：GPIO1_IO09；RST：GPIO5_IO09，低电平复位 | `/dev/input/eventX` |
| LCD | BSP 已配置的 LCD 控制器及背光 | `/dev/fb0` |

Input 的 `eventX` 编号不是固定值。运行时使用以下命令确定按键和触摸屏对应的节点：

```sh
cat /proc/bus/input/devices
```

Qt 当前按键路径写在 `mainwindow.cpp`，默认为 `/dev/input/event2`。触摸路径由 Qt 输入环境配置，部署时需要按实际节点调整。

## BSP 与设备树依赖

项目入口包含 BSP 的 `imx6ull-alientek-emmc.dts`，再包含项目 `.dtsi`。原始板级设备树及 SoC 公共描述仍由 BSP 管理，本仓库不包含完整内核。

编译前应确认：

- BSP 的 `arch/arm/boot/dts/` 下存在板级 `.dts` 及其包含文件。
- BSP 中保留已调试好的 LCD 时序、背光和 I²C 控制器配置。
- BSP 旧 LED、按键、AP3216C、ICM20608 和触摸节点不再与项目节点重复定义或占用相同资源。
- 项目使用了 BSP 的 `pinctrl_hog_1`、`reg_sd1_vmmc` 等标签，需要和所用 BSP 对应。
- GT911 使用的 GPIO5_IO09 不再由旧 LCD reset 配置占用；GPIO1_IO09 由项目配置释放给触摸中断。

当前设备树针对本项目使用的 BSP 已做适配，不是对任意原版 NXP 设备树直接通用的覆盖文件。

## 编译

### 1. 驱动、设备树和 C 测试程序

先将交叉编译器的 `bin` 目录加入 `PATH`，并准备好与开发板运行内核一致、已配置和编译的 BSP 内核树。内核树中需要有可运行的 `scripts/dtc/dtc`。

在项目根目录执行：

```sh
make KERNEL_DIR=/path/to/bsp/linux CROSS_COMPILE=arm-linux-gnueabihf-
```

也可以分别执行：

```sh
make dtb KERNEL_DIR=/path/to/bsp/linux
make modules KERNEL_DIR=/path/to/bsp/linux
make app
```

产物位置：

- 设备树：`build/imx6ull-device-management.dtb`。
- 内核模块：`drivers/*.ko`、`icm20608-driver/driver/icm20608.ko`。
- C 测试程序：`build/led_test`、`key_test`、`key_led_test`、`ap3216c_i2c_bench`、`iio_event_monitor`、`iio_buffer`、`uart_test`、`touch_test`。

`app/touch.c` 是保留的分步调试示例，当前顶层 `make app` 不编译它。

清理命令：

```sh
make clean KERNEL_DIR=/path/to/bsp/linux
```

`clean` 会清理驱动构建产物，并删除整个项目 `build/` 目录，包括放在其中的 Qt 构建目录。

顶层 `make` 同时生成采集守护进程 `build/devmond` 和命令行客户端 `build/devmonctl`（也可单独 `make daemon`）。

### 2. Qt 应用

Qt 应用独立编译，顶层 `make` 不包含 Qt 工程（需要 QtNetwork，用于 QLocalSocket）。使用为 ARM 交叉编译好的 Qt 5.12.9 的 `qmake`：

```sh
mkdir -p build/qt-device-monitor
cd build/qt-device-monitor
/path/to/arm-qt/bin/qmake ../../qt/device_monitor/device_monitor.pro
make -j2
```

生成 `device_monitor`。也可在 Qt Creator 中打开 `.pro`，选择 ARM Qt Kit 构建。

不要使用桌面 GCC 64 位 Kit 构建要部署到开发板的程序。开发板还需安装匹配的 ARM Qt 运行库和 `linuxfb` 平台插件。

## 部署与运行

### 1. 更新设备树

将生成的 DTB 放入 TFTP 目录，并让 U-Boot 加载该项目 DTB 后启动内核。内核模块必须与开发板正在运行的内核匹配。

本项目开发环境使用：

```text
TFTP：/home/zero/linux/tftpboot
NFS 根目录：/home/zero/linux/nfs/rootfs
主机部署目录：/home/zero/linux/nfs/rootfs/device-management
开发板对应目录：/device-management
```

这些路径属于部署环境，其他机器请按自己的服务配置调整。

### 2. 加载驱动

把 `.ko`、测试程序及 Qt 可执行文件复制到部署目录。在开发板上执行：

```sh
cd /device-management
insmod led_drv.ko
insmod key_drv.ko
insmod ap3216c.ko
insmod icm20608.ko
insmod gt911_drv.ko
dmesg | tail -n 30
cat /proc/bus/input/devices
```

第一次部署时先确认驱动成功绑定、设备节点存在，再启动 Qt。各模块不要求按以上顺序加载，但 Input 节点编号可能随加载顺序改变。

推荐直接用 `qt/device_monitor/run_device_monitor.sh`（与 `devmond`、`devmonctl`、`device_monitor`、`.ko` 放在同一目录）：它加载驱动、以守护进程方式启动 `devmond`（已在运行则跳过）、按设备名查找触摸节点、设置 Qt / tslib 环境变量后启动界面，不修改 `/etc/profile`。`--daemon-only` 只启动 `devmond`，可无屏幕运行。

`devmond` 选项：`-d` 守护进程化（日志写 `-l`，默认 `/tmp/devmond.log`）、`-s` 套接字（默认 `/tmp/devmond.sock`）、`-c` 配置文件、`-r` 录制目录（默认都在可执行文件旁）、`-p` pid 文件。停止：`kill $(pidof devmond)`。

### 3. 准备 Qt 运行环境

确保动态链接器能够找到 Qt 和 tslib 的库，并设置平台插件路径。项目调试环境的 Qt、tslib 分别放在 `/usr/lib/arm-qt`、`/usr/lib/arm-tslib`，字体放在 `/usr/share/fonts`。

如果这些环境变量已经写入开发板 `/etc/profile`，重新加载后启动：

```sh
. /etc/profile
cd /device-management
./device_monitor -platform linuxfb:fb=/dev/fb0
```

使用 tslib 时，`TSLIB_TSDEVICE` 应指向 GT911 的实际事件节点，并设置 `QT_QPA_FB_TSLIB=1`。`QT_QPA_FONTDIR` 应指向已准备好的字体目录；界面指定字体为 Consolas，字体需自行准备，不随源码分发。

显示未亮时，可检查 framebuffer 消隐、背光电源和亮度：

```sh
echo 0 > /sys/class/graphics/fb0/blank
echo 0 > /sys/class/backlight/backlight/bl_power
echo 6 > /sys/class/backlight/backlight/brightness
```

背光节点名称及有效亮度范围以实际 BSP 为准。

## 操作说明

### Monitor 与设备控制

- Environment 显示 AP3216C 数据；`Read Sensor` 额外请求采集一次 AP3216C。
- Motion 显示 ICM20608 换算后的加速度、角速度和芯片温度。
- `OK` 表示本次设备数据读取成功；`ERROR` 表示打开或读取失败。AP 的各通道还会单独显示 `invalid`。
- 手动模式下，屏幕 LED 按钮或实体按键可以翻转 LED。
- 勾选 `Auto LED` 后，手动 LED 按钮禁用，实体按键只显示按下／松开状态。
- 自动模式使用 ALS 原始数值判断：小于等于开灯阈值时开灯，大于等于关灯阈值时关灯，中间保持当前状态。
- ALS 无效或读取失败时不进行自动判断，保持 LED 当前状态。

### 网络与时间

- **校时**：启动脚本运行 `ntpd -p $NTP_SERVER -S ntp_event.sh`（默认服务器 192.168.10.1，即 TFTP/NFS 主机），每次校准后 `ntp_event.sh` 执行 `hwclock -w -u` 写入 i.MX6ULL 的 SNVS RTC，断网重启时内核从 RTC 恢复时间（断电保持需要底板装纽扣电池）。内核时钟、NTP、RTC 一律 UTC，只有显示和 CSV 按 `TZ=CST-8` 换算。System 页显示板子时间和同步状态（`adjtimex()` 的 `STA_UNSYNC`）：busybox ntpd 先步进校时，约几分钟后进入 PLL 模式才清除该标志。`devmond` 的定时器都用 `CLOCK_MONOTONIC`，校时造成的时间跳变（实测一次跳了 717 天）不影响采集周期。
- **主机作为 NTP 服务器**（板子没有外网时）：

  ```sh
  sudo apt install -y chrony
  printf 'allow 192.168.10.0/24\nlocal stratum 10\n' | sudo tee /etc/chrony/conf.d/devboard.conf
  sudo systemctl restart chrony
  ```

- **远程访问**：`devmond -t 9100`（启动脚本默认开启，`DEVMOND_TCP_PORT=0` 关闭）。PC 上 `devmonctl -H 192.168.10.50 watch`，或在 PC 上用主机 Qt 编译同一个界面后 `DEVMOND_ADDR=192.168.10.50:9100 ./device_monitor`。TCP 连接开启 `TCP_NODELAY` 与 keepalive（断网约 1 分钟后释放连接）；界面的 TCP 连接不走代理（PC 上的 `ALL_PROXY` 会让 `QTcpSocket` 连到本机代理，代理不转发局域网地址，表现为 "Remote host closed"）。协议没有认证，只在可信局域网使用。

### System

由 `devmond` 每个采集周期从 `/proc`、`/sys` 采集后推送（`SYS` 消息）：CPU 占用与 devmond 自身占用（自身按最近 10 个周期平均，CPU 时间以 10 ms 节拍计，单周期分辨率只有 1 %）、内存（MemTotal − MemAvailable）、1 分钟负载、运行时间、SoC 温度（`imx_thermal`，≥ 85 °C 标红）、除 `lo` 外网卡收发速率（NFS 根文件系统的流量也计在内）、录制目录所在文件系统剩余空间（< 100 MB 标红）。右侧曲线为最近 120 个周期的 CPU % 和 SoC 温度。该页由代码创建并插入标签栏，三套 `.ui` 布局无需修改。

### Settings

| 参数 | 默认值 | 范围 |
| --- | --- | --- |
| LED ON threshold | 20 | 0～65535 |
| LED OFF threshold | 50 | 0～65535 |
| Sample interval | 1000 ms | 200～60000 ms |

开灯阈值必须小于关灯阈值。修改后点击 `Apply` 才生效。采集间隔决定 AP3216C 的读取周期和界面刷新周期；ICM20608 由数据就绪中断按自己的采样率采集，界面显示每个周期内全部样本的平均值。界面上 ALS 同时显示原始值和按 `in_illuminance_scale` 换算的 lux，Auto LED 阈值仍按原始值比较；温度为 ICM20608 芯片温度，不应直接当作环境温度。

参数保存到程序同目录的 `device_monitor.ini`，下次启动恢复。文件内容示例：

```ini
[led]
onThreshold=20
offThreshold=50

[sensor]
intervalMs=1000
; ICM20608 IIO 采样率（4～1000 Hz，界面不提供，修改后重启程序生效）
imuRateHz=100
```

参数由 `devmond` 校验、立即生效并原子写回 ini（先写临时文件、fsync，再 rename）。`devmond` 每次启动为手动模式。配置不合理时使用默认值；保存失败时，本次应用的参数仍生效，但不保证下次恢复。修改 ini 后可 `kill -HUP $(pidof devmond)` 重新读取。

### CSV 数据记录

点击 `Start Recording` 开始，点击 `Stop Recording` 停止。每次记录创建两个新文件：

```text
records/ap3216c_年月日_时分秒_毫秒.csv
records/icm20608_年月日_时分秒_毫秒.csv
```

目录位于可执行程序旁。AP 文件保存原始值、按量程换算的照度 `als_lux` 和有效标志，无效通道留空；ICM 文件保存换算后的 `g`、`dps` 和 `C`，**每个 IIO 样本一行**（默认 100 行/秒），时间取自驱动在数据就绪中断里记录的内核时间戳（`kernel_timestamp_ns` 列为纳秒原值）。

每收到一次成功的采集结果追加一行，时间戳使用界面处理该结果时的开发板系统时间。默认每路约 1 秒一行；手动读取 AP 会额外追加一行。传感器失败期间没有对应数据行。记录前应先确认开发板系统时间。

录制由 `devmond` 完成：点击 `Exit` 只关闭界面，录制、LED 状态和采集继续，重新打开界面会自动连上并显示当前状态。`devmond` 收到 `SIGTERM` / `SIGINT` 时停止录制并刷盘、关灯、关闭 IIO 缓冲区和接近中断后退出。

## 独立测试示例

以下事件节点仅为示例，先根据 `/proc/bus/input/devices` 确认实际编号：

```sh
./led_test /dev/led0 1
./led_test /dev/led0 0
./key_test /dev/input/event2
./key_led_test /dev/input/event2 /dev/led0
cat /sys/bus/iio/devices/iio:device*/in_proximity_raw   # AP3216C 各通道（先看 name 确认是哪个设备）
./iio_event_monitor ap3216c   # 遮挡/移开传感器，看 proximity rising / falling 事件
./ap3216c_i2c_bench /dev/i2c-0 200   # 连续读行为验证与读取方式开销对比
./iio_buffer 100 5 3   # 100 Hz 采集 5 秒，统计采样间隔和丢样
./touch_test /dev/input/event1
./uart_test /dev/ttymxc2
./devmonctl STATUS               # 守护进程当前状态
./devmonctl watch 5 AP IMU PROX  # 打印 5 秒内的推送
./devmonctl LED toggle; ./devmonctl REC start; ./devmonctl CONFIG on=10 off=40 interval=500
```

LED 接口使用一个字节的 ASCII 字符 `'0'` / `'1'`。UART 回环测试需短接 UART3 的 TX 与 RX；它验证本机串口收发，不包含外部传感器协议。

## 程序执行结构

```text
                      devmond（C，单线程，一个 epoll）
 驱动 / sysfs / input ─┬─ signalfd   SIGTERM/SIGINT 正常退出，SIGHUP 重读配置
                      ├─ timerfd    采集周期：读 AP3216C、自动亮灯、发布 IMU 平均值、刷新 CSV
                      ├─ timerfd    100 ms：读走 ICM20608 IIO 缓冲区，逐样本录制
                      ├─ AP 事件 fd  AP_INT 接近中断
                      ├─ 按键 fd     手动模式下翻转 LED
                      └─ Unix 域套接字 /tmp/devmond.sock（daemon/protocol.md）
                                 │  推送 AP / IMU / PROX / LED / KEY / CONFIG / REC
                                 │  命令 LED / AUTO / CONFIG / READ / REC / STATUS
                 ┌───────────────┴───────────────┐
        Qt 界面（QLocalSocket，断线重连）     devmonctl（命令行）
```

拆分的目的：采集与录制不依赖界面（界面退出或崩溃不影响，可无屏幕运行）；界面只剩一个线程，不再接触任何设备文件；同一份数据可以同时给多个客户端。

`devmond` 的设计要点：所有输入都是 fd，一个线程一个 epoll，没有锁；信号用 signalfd 同步处理，没有异步信号处理函数的重入问题；客户端套接字非阻塞，每个客户端一个输入行缓冲和输出队列，写不完的留在队列里等 `EPOLLOUT`，输出积压超过 256 KB 的慢客户端直接断开，不拖住采集；客户端断开先标记、回调结束后再释放，避免在处理某个客户端的命令时把它释放掉；`flock` 加锁的 pid 文件保证只运行一份（进程崩溃时内核自动释放锁）；`-d` 用两次 fork 脱离终端。

实测（100 Hz IMU + 1 s 采集周期，一个界面连接）：`devmond` 每秒约 25 次唤醒，与拆分前的采集线程相同，套接字推送没有带来额外开销；界面每秒约 1 次唤醒，CPU 均接近 0。界面关闭期间录制持续（ICM CSV 每秒约 100 行）；`devmond` 重启后界面 1~3 秒内自动重连。

界面重连靠 `QLocalSocket::stateChanged`：Qt 5.12 连接失败时先发 `error` 信号、之后才把状态改成 `UnconnectedState`，在 `error` 槽里判断状态再安排重连会漏掉，守护进程不在时界面就再也连不回去。

ICM20608 为什么定时批量读、不用 `poll`：4.1 内核的 `iio_push_to_buffer()` 每推入一个样本都会唤醒 `poll` 等待者（`buffer/watermark` 只决定 `poll` 是否返回），实测 100 Hz 时用 `QSocketNotifier` 监听 fd，工作线程每秒被唤醒约 126 次；改为每 100 ms 读一次后降到约 34 次，其中约 24 次来自每秒一次的 AP3216C 读取（6 次单字节 SMBus 传输）。

AP3216C 为什么每个通道读一次、不一次读 6 字节：数据手册只给了单字节读时序，`app/ap3216c_i2c_bench.c` 实测芯片地址只在一对寄存器（低、高字节）内自增，读满 2 字节回到该对开头，从 0x0A 连续读 6 字节得到的是 IR、IR、IR。每通道读一对后，一组数据从 6 次传输 / 24 次等待降到 3 次 / 15 次（耗时 3.4 ms → 2.1 ms），工作线程降到约 25 次唤醒/秒。i.MX I²C 驱动每个字节等一次中断，所以等待次数 ≈ 每次传输 3 次 + 每字节 1 次。

接近事件用 `QSocketNotifier` 而 IMU 不用：接近事件只在状态变化时由中断产生，一次遮挡两次，fd 可读才唤醒正合适；IMU 是 100 Hz 连续数据流，见上一段。

## 常见问题

- **模块加载但没有设备节点**：查看 `dmesg`，检查设备树是否实际加载、`compatible` 是否匹配及 `probe()` 是否成功。
- **提示 GPIO 被占用**：检查 BSP 是否仍启用了同引脚的旧设备或 pinctrl 配置。
- **按键或触摸没有响应**：按键由 `devmond` 按设备名 `keyinput` 查找（见 `/tmp/devmond.log`）；触摸节点由启动脚本按名字查找。
- **Qt 缺少平台插件或动态库**：确认部署的是 ARM 库及 `linuxfb` 插件，并检查运行环境路径。
- **配置或记录保存失败**：确认程序目录可写、存储空间足够。程序不会自动清理旧 CSV 文件。
- **设备在程序启动后才加载驱动**：`devmond` 打开失败的设备不会自动重试，加载驱动后重启 `devmond`（`kill $(pidof devmond)` 后再运行启动脚本），界面会自动重连。
- **界面显示 devmond not reachable**：`devmond` 没有运行或套接字路径不一致，查看 `/tmp/devmond.log`。

## 后续计划

第一版完成数据采集、显示、控制、配置和记录。后续在保留当前可用版本的基础上增加蜂鸣器等板载外设驱动。
