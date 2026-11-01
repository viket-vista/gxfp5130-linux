# gxfp5130 — Goodix GXFP5130 Linux 传输驱动

面向华为 MateBook（ACPI `GXFP5130`，Nuvoton EC eSPI 邮箱）的内核传输驱动。
传感器不挂在 Linux 可见的 SPI/I2C 总线上，必须经 EC 邮箱间接访问。

本仓库内容：

```
linux/
├── gxfp5130.c             内核模块（eSPI 传输 + 三层帧 + /dev/gxfp）
├── gxfp5130_proto.h       帧编解码（内核与主机测试共用）
├── gxfp5130_uapi.h        用户态 ABI
├── dkms.conf              DKMS 打包配置
├── Makefile
├── tests/test_proto.c     帧编解码单元测试（make test）
├── tools/gxfp-tool.c      诊断工具（info/cmd/send/listen/peek/pulse/setline）
├── scripts/install-boot.sh  DKMS 安装 + 开机自动加载
├── scripts/uninstall-boot.sh
├── scripts/test.sh        一键实机探测
├── scripts/dump-acpi.sh   ACPI 表导出
├── docs/REVERSE-ENGINEERING.md
└── README.md
```

## 状态

**已在实机验证通过**（Huawei MateBook，ACPI `GXFP5130:00`）：

```
ready         : 1
fw_version    : GF_GCC_EC_20068
mailbox       : 0xfe800000 (4096 bytes)
gpio write-done: 899 (ACPI index 1, pad 387)
gpio read-done : 683 (ACPI index 2, pad 171)
irq           : 183 (ACPI GpioInt, pad 133)
```

- 固件版本握手 `0xA8` → `GF_GCC_EC_20068`；
- `0xAF/0xAE` 取 MCU 状态返回 23 字节；
- IRQ 正常计数，命令/响应全程走中断；
- 多次 `rmmod`/`insmod` 无 GPIO 泄漏（devm 管理）；
- 社区 gxfpmoc 用户态栈可直接使用：`chip id 0x2504`、
  `GF_GCC_EC_20068`、`tls_connected=1`；
- **fprintd 已能识别设备**：
  `found 1 devices … Goodix GXFP5130 eSPI Fingerprint Sensor`，
  `fprintd-enroll` 可进入采集流程（需手指按压完成登记）。

会话初始化由用户态负责，因此内核 `probe_handshake` 默认关闭；
固件版本请用 `userspace/build/gxfp_info` 或在模块参数中开启握手查看。

线上帧分三层：eSPI 包装（`0xF0` + 长度 + 校验 + 序号）、MP 帧
（`0xA0` + 长度 + 校验）、Goodix 命令体（`cmd` + 长度 + payload + `0xAA-Σ`）。
命令写邮箱偏移 `0x000`，响应在 `0x200`。

## 用户态接口（与 gxfpmoc 兼容）

内核只做传输，帧由用户态构造，ABI 与社区 gxfpmoc 栈一致：

```
write(fd, frame, len)   提交完整 eSPI 帧（<= 0x200 字节），内核写入
                        TX 窗口并拉 write-done
read(fd, ...)           返回一条 0xE10 字节的窗口快照：
                        struct gxfp_rx_record { len, reserved, ts_ns, data[0xE00] }
                        每次中断产生一条记录，多块帧由用户态拼接
poll(fd)                有记录可读时 POLLIN
ioctl(GXFP_IOCTL_FLUSH_RX)  清空接收队列
```

调试 ioctl（magic `'G'`）：`GET_INFO`、`PEEK`、`PULSE`、`SET_LINE`，
对应 `gxfp-tool info/peek/pulse/setline`。

## 构建

```bash
cd linux
make                 # 生成 gxfp5130.ko
make -C tools        # 生成 tools/gxfp-tool
```

## 社区用户态栈（userspace/）

`userspace/` 是社区 GPL-2.0 工程
[`Void755/gxfpmoc`](https://github.com/Void755/gxfpmoc) 的副本
（含 `libfprint` 分支），用于 TLS-PSK 会话、图像采集和 libfprint 集成。
它通过上面的 `/dev/gxfp` ABI 与本驱动通信。

```bash
cd userspace
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo ./build/gxfp_info          # 读 chip id / 固件 / MCU 状态
```

实测输出：

```
chip id: 0x2504
firmware version: GF_GCC_EC_20068
mcu state:
  tls_connected: 1   tls_session: 1   pov: 1
```

## PSK（TLS 预共享密钥）

采集/认证需要与传感器一致的 32 字节 PSK。本机采用**从 Windows 缓存解封**
的方式（机器作用域 DPAPI，离线可用）：

1. 只读挂载 Windows 分区，取 `ProgramData/Goodix/Goodix_Cache.bin`、
   `Windows/System32/config/{SYSTEM,SECURITY}`、
   `Windows/System32/Microsoft/Protect/S-1-5-18/*`；
2. 用 SYSTEM+SECURITY hive 解出 `DPAPI_SYSTEM` 机器密钥；
3. 用缓存末尾 8 字节派生 entropy（该工程的 `derive_entropy`），解封得到 PSK；
4. 安装到 `/var/lib/fprintd/gxfp/psk_raw32.bin`（`0600 root:root`）。

```bash
sudo install -m 600 -o root -g root psk_raw32.bin /var/lib/fprintd/gxfp/psk_raw32.bin
sudo ./build/gxfp_capture --psk-raw32 /var/lib/fprintd/gxfp/psk_raw32.bin --pov
```

另一条路是在 Linux 侧重新烧录 PSK（`gxfp_psk_tool --build-bb010002` /
`--upload-bb010002`），会改变传感器配对信息，Windows 下次启动会自动重新配对。

## 开机自动加载（DKMS，推荐）

```bash
sudo ./scripts/install-boot.sh
```

脚本会：

1. 把源码安装到 `/usr/src/gxfp5130-0.1/`；
2. `dkms add/build/install`（内核升级后自动重编，模块由 DKMS 的 MOK 密钥签名）；
3. 写入 `/etc/modules-load.d/gxfp5130.conf`，开机由 `systemd-modules-load`
   自动 `modprobe gxfp5130`；
4. 立即切换到 DKMS 版本并打印 `/dev/gxfp` 状态。

卸载：

```bash
sudo ./scripts/uninstall-boot.sh
```

验证（无需重启）：

```bash
sudo rmmod gxfp5130
sudo systemctl restart systemd-modules-load.service
lsmod | grep gxfp && ls -l /dev/gxfp
```

## 装载与探测

```bash
sudo ./scripts/test.sh
```

脚本会：

1. 卸载旧模块、`insmod` 新模块；
2. 打印 probe 阶段解析到的 ACPI/GPIO/邮箱资源；
3. 读取 `/dev/gxfp` 信息；
4. 依次用 `tx_mode=0/1/2` 发送固件版本命令 `0xA8`；
5. 发送 MCU 状态命令 `0xAE`；
6. 输出保存到 `/tmp/gxfp-test.log`。

正常时应看到：

```
gxfp5130 GXFP5130:00: mailbox phys=0xfe800000 size=0x1000 off=0x200
gxfp5130 GXFP5130:00: ACPI GpioIo[0] pin=...
gxfp5130 GXFP5130:00: firmware version: ... "GF_GCC_EC_..."
```

## 模块参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `mbox_phys` | 0 | 0 = 用 ACPI 资源，否则回退 `0xFE800000` |
| `mbox_size` | 0 | 0 = ACPI 资源或 `0x1000` |
| `tx_off` | `0x0` | 命令缓冲区偏移（512 字节） |
| `rx_off` | `0x200` | 响应缓冲区偏移 |
| `gpio_wr_index` | 1 | write-done 的 ACPI GPIO 索引（GpioInt 也占一个索引） |
| `gpio_rd_index` | 2 | read-done 的 ACPI GPIO 索引 |
| `swap_done` | 0 | 交换两个 GpioIo 的 write/read done 角色 |
| `invert_done` | 0 | 反转 done 线的逻辑电平 |
| `probe_handshake` | 0 | probe 时是否查询固件版本（默认关，会话初始化由用户态负责） |
| `probe_timeout_ms` | 800 | 握手超时（仅 probe_handshake=1 时） |
| `poll_ms` | 0 | 无中断时轮询间隔；0 = 自动（无 IRQ 时 20 ms） |
| `tx_mode` | 0 | 0 整帧一次写；1 先写 4 字节头、2 ms 后写体；2 同 1 但每段各脉冲一次 |
| `frame_log` | 0 | 打印每个收发帧 |

运行时可写：

```bash
echo 1 | sudo tee /sys/module/gxfp5130/parameters/tx_mode
```

## /dev/gxfp 用户态接口

```c
write(fd, &cmd, 1 + payload_len);   // [cmd][payload...]
read(fd, buf, sizeof(buf));         // 返回 [cmd][payload...]，一帧一次
poll(fd, POLLIN, timeout);          // 有响应可读
ioctl(fd, GXFP_IOCTL_GET_INFO, &info);
ioctl(fd, GXFP_IOCTL_FLUSH, 0);
```

`cmd`、`read/write` 的语义见 `gxfp5130_uapi.h` 与逆向报告第 3 节。
驱动负责 MP 包装、Goodix 校验、GPIO 握手与 IRQ/轮询收帧；TLS 与业务
命令由用户态完成。

## 诊断示例

```bash
gxfp-tool info
gxfp-tool cmd a8 "" 1500          # 取固件版本
gxfp-tool cmd ae 5500000000 800   # 取 MCU 状态
gxfp-tool listen 4 500            # 收 4 帧
gxfp-tool peek 0x200 32           # 直接读邮箱窗口（调试）
gxfp-tool pulse wr                # 手动拉一次 write-done（调试）
```

## fprintd 集成

依赖（Debian/Ubuntu）：

```bash
sudo apt install cmake meson ninja-build libmbedtls-dev libgusb-dev \
                 libudev-dev systemd-dev libopencv-dev doctest-dev
```

一键安装：

```bash
sudo ./scripts/install-fprintd.sh
```

脚本会编译 `userspace/` 的库与 demo、按 `-Ddrivers=gxfp` 构建 libfprint
分支并安装到 `/usr/local`（不覆盖发行版包），再给 fprintd 加一个
`DeviceAllow=/dev/gxfp rw` 的 drop-in（发行版单元的 `DeviceAllow` 白名单
默认会拦掉我们的字符设备），最后重启 fprintd。

验证：

```bash
fprintd-list "$USER"          # 应列出 Goodix GXFP5130 eSPI Fingerprint Sensor
fprintd-enroll -f right-index-finger "$USER"   # 按提示放手指
fprintd-verify "$USER"
```

可选：把指纹接入 sudo 与 KDE 锁屏。本机最终采用：

```
# /etc/pam.d/sudo
auth  sufficient  pam_fprintd.so timeout=8 max-tries=1

# /etc/pam.d/kde（锁屏/解锁真正生效的那条）
auth    sufficient      pam_fprintd.so timeout=8 max-tries=3
@include common-auth
```

KDE 注意事项：

- kscreenlocker 6.7 会额外跑自己的 `kde-fingerprint` PAM 服务，和上面
  `kde` 里的 `pam_fprintd` 形成**两个指纹提示**，其中自带的那个在本机
  对话不兼容、只会超时。已把它停用：
  `mv /etc/pam.d/kde-fingerprint /etc/pam.d/kde-fingerprint.disabled-gxfp`。
- 停用后锁屏仍可能残留一条静态提示，但实际生效的是 `kde` 栈那条；
  失败后可以继续按（最多 3 次），超时后直接输入密码即可。
- 回退：`mv /etc/pam.d/kde-fingerprint.disabled-gxfp /etc/pam.d/kde-fingerprint`
  并恢复 `/etc/pam.d/kde.bak.gxfp`、`/etc/pam.d/sudo.bak.gxfp`。

回退：删除 `/usr/local/lib/libfprint-2.so*`、执行 `ldconfig`、删除
`/etc/systemd/system/fprintd.service.d/10-gxfp.conf` 并重启 fprintd。

## 故障排查

| 现象 | 处理 |
| --- | --- |
| `/dev/gxfp` 不存在 | `dmesg` 看 probe 是否失败；确认 `insmod` 成功 |
| 有 `/dev/gxfp` 但握手超时 | `scripts/test.sh` 会自动遍历 `tx_mode`、`swap_done`、`invert_done`、`poll_ms` 组合 |
| ACPI 未给出 GPIO | `dmesg` 会打印 pin 号；用 `mbox_phys=` 等参数覆盖 |
| 收到乱码帧 | 打开 `frame_log=1`，对照逆向报告第 3 节的头/校验 |

## 后续工作（用户态，超出本驱动）

1. TLS-PSK 会话：在 `/dev/gxfp` 之上实现 mbedTLS 客户端，PSK 从 Windows
   侧提取或重新配对。
2. 指纹采集/模板/匹配：接入 libfprint 与 fprintd（可参考公开的
   `gxfp5130-linux` 工程，但需自行确认许可与固件兼容性）。
3. 电源管理：S0ix/S3 前后发送 `0x0E`、`0xD2`。

## 许可

内核模块与工具：GPL-2.0。协议事实来自对用户自有 Windows 驱动的互操作性
逆向分析；文档中提及的公开工程仅作交叉验证，未复制其代码。
