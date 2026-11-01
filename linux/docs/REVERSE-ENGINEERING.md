# Goodix GXFP5130 Windows 驱动逆向分析报告

分析对象：`/home/hutao/Desktop/gfspi.inf_amd64_3868ae3d6c95a9b8/`
（`gfspi.dll`，UMDF 2.15 x64，版本 `1.1.18.25`，2024-02-27，PDB 路径
`...\huawei_win_ec\winfpcode\Milan_Watt\MilanSpi\x64\Release\gfspi\gfspi.pdb`）

本文记录从该 Windows 驱动包中还原出的硬件拓扑、线上帧格式、读写时序、
命令集合及其证据位置（RVA = 文件内相对虚拟地址，镜像基址 `0x180000000`）。
文末列出与公开资料的交叉验证结果和仍未确认的点。

---

## 1. 包结构与角色划分

| 文件 | 角色 |
| --- | --- |
| `gfspi.inf` | ACPI `GXFP5130` 的 UMDF 驱动安装描述（`WUDFRd` 反射器 + 用户态 `gfspi.dll`） |
| `gfspi.dll` | 全部硬件访问、帧协议、TLS 1.2（mbedTLS 3.4.1）、WBDI 接口 |
| `EngineAdapter.dll` | WinBio 引擎适配层 |
| `AlgoChicago*.dll` / `AlgoMilan.dll` | 指纹算法库（芝加哥/米兰系列传感器） |
| `SessionService.exe` | 会话检测服务 |
| `GoodixEventLog.dll` | ETW 事件日志提供者 |

INF 关键配置：

```ini
%DeviceName%=MyDevice_Install, ACPI\GXFP5130  ; Nuvoton EC
UmdfDirectHardwareAccess=AllowDirectHardwareAccess
UmdfHostProcessSharing=ProcessSharingDisabled
```

`UmdfDirectHardwareAccess` 说明硬件资源（内存窗口、GPIO）由 UMDF 直接映射，
不经过内核驱动；`.sys` 只是系统自带的 `WUDFRd.sys`。所以**没有内核态驱动
可供逆向**，真正的协议全在 `gfspi.dll` 中。

驱动用 ACPI 硬件 ID 字符串选择“项目类型”，再按项目类型走不同传输后端
（函数 RVA `0x16e00` 起，`movl $0x1/$0xc/$0x2/...` 写入全局 `0x1800d55c0`）：

| ACPI/USB 硬件 ID | 项目类型 | 传输 |
| --- | --- | --- |
| `ACPI\GXFP5120` | 1 | SPI（SPB IOCTL） |
| **`ACPI\GXFP5130`** | **0xC (12)** | **Nuvoton EC eSPI 邮箱（MMIO + GPIO）** |
| `USB\VID_27C6&PID_5120` 等 | 2 | USB |

也就是说：**本机的指纹传感器不在 Linux 可见的 SPI 总线上**，而是挂在
Nuvoton EC 的内部 SPI 上，主机只能通过 EC 邮箱间接访问。这与
`/sys/bus/acpi/devices/GXFP5130:00` 只生成 platform device、没有
`spi-GXFP5130:00` 子设备的现象一致。

---

## 2. 硬件资源

Windows 驱动在 `MilanEvtDevicePrepareHardware`（RVA `0x19da0`）中枚举
`_CRS` 资源，日志字符串包括：

- `Shared memory resouce found: flag 0x%x, start (0x%x) Length: (%d)`
- `Map nuvoton shared memory success/failed`
- `GPIO resources error`
- `Reset & Interrupt resource not found`
- `Open SPI Failed with 0x%x`

### 2.1 DSDT 实证（本机 ACPI 表）

`\_SB.SPBA`（`GXFP5130`）的 `_CRS` 为固定 0x77 字节模板，运行时用
`GNUM()` 把华为的全局引脚编号换算成 Intel PCH 扁平号后回填：

```
Memory32Fixed: 0xFE800000, length 0x1000      ; EC eSPI 邮箱
GpioIo  GPIN = GNUM(0x00140485)               ; -> pin 387
GpioIo  SPIN = GNUM(0x00141083)               ; -> pin 171
GpioInt CPIN = GNUM(0x0014080B)               ; -> pin 133  (IRQ 183)
```

`_INI` 对这三根 pad 调用 `SHPO(pad, 1)`，即置位 PAD_OWN 寄存器里的
host-owner 位，把 pad 交给操作系统；`_STA` 依赖 `HFTE && HWEP`。

Windows 端把 done 线的翻转实现为 **GPIO 类驱动 IOCTL**
（`0x480000` = 读引脚、`0x480004` = 写引脚），等价于 Linux 的
`gpiod_set_value()`；不需要走 ACPI `_GPIORW`（本机 DSDT/SSDT 中也没有
该方法）。

本机（MateBook，`GXFP5130:00`）实测 probe 结果：

| 资源 | 实测值 | 说明 |
| --- | --- | --- |
| 邮箱窗口 | `0xFE800000`，长度 `0x1000` | ACPI 的 memory resource 为 0 长度占位，驱动回退到该地址 |
| 帧缓冲偏移 | `0x200` | 与 ISR 中 `add $0x200`（RVA `0x1beea`、`0x1c11b`）一致 |
| write-done GPIO | ACPI GpioIo pin 387 → gpio 899（ACPI GPIO 索引 1） | 输出，脉冲通知 EC 取命令 |
| read-done GPIO | ACPI GpioIo pin 171（ACPI GPIO 索引 2） | 输出，每读完一块回应一个脉冲 |
| 中断 | ACPI GpioInt pin 133 → IRQ 183 | `WdfInterruptCreate` / `MilanEvtInterruptIsr` |

关键点：`_CRS` 中 **GpioInt 排在两个 GpioIo 之前**，`gpiod_get_index()`
的索引把 GpioInt 也计算在内——索引 0 取不到（-ENOENT），索引 1、2 才是两根
done 线。驱动默认使用 `gpio_wr_index=1`、`gpio_rd_index=2`。

（write/read 两根线的先后顺序仍需实测确认，可用 `swap_done=1` 交换。）

`Linux` 侧需要注意：华为 DSDT 对非 Windows OS 可能返回占位 `_CRS`，
因此驱动在 ACPI 资源不可用时回退到固定地址 `0xFE800000`，并允许用模块
参数覆盖。

---

## 3. 线上帧格式

线上是三层结构：**eSPI 包装 → MP 帧 → Goodix 命令体**。

### 3.0 eSPI 包装层（8 字节）

命令区（邮箱 +0x000）先写 8 字节包装，再接 MP 帧：

```
+--------+---------+---------+--------+--------+--------+--------+--------+
| 0xF0   | mp_lo   | mp_hi   | cksum  | seq_lo | seq_hi |   0    |   0    |
+--------+---------+---------+--------+--------+--------+--------+--------+
  cksum = (0xF0 + mp_lo + mp_hi) & 0xFF
  seq   = 每次发送递增的 16 位序号（初值 0x8881）
  mp_len= 其后 MP 帧的字节数
```

整帧按 8 字节对齐补齐，命令区总长不超过 `0x200`（512 字节）。
**缺少这层包装时 EC 会“吃掉”数据但不作任何回应**——这是本驱动最初
无法握手的原因。

### 3.1 MP 包装层（4 字节）

```
+--------+--------+--------+--------+
| flags  | len_lo | len_hi | cksum  |
+--------+--------+--------+--------+
  flags   = 0xA0 | seq        (type A = 0xA，seq 为低 4 位)
  len     = 其后 Goodix 帧的字节数
  cksum   = (flags + len_lo + len_hi) & 0xFF
```

构建函数 RVA `0x65864` 的实测指令序列：

```
buf[0] = (buf[0] & 0x0f) | 0xA0;
*(u16 *)&buf[1] = body_len;
buf[3] = buf[0] + buf[1] + buf[2];
```

校验函数 RVA `0x156e8` 对收到的帧做同样检查：高半字节必须是
`0xA/0xB/0xC`，`len < 0x6C00`，且 `buf[3] == flags + len_lo + len_hi`。
另一处 RVA `0x158c0` 是 `0xB0` 变体（响应方向）。

响应方向（邮箱 +0x200）只包含 MP 帧，不带 eSPI 包装；长度取自
MP 头部的 `len` 字段，总长 = `4 + len`。

### 3.2 Goodix 命令层

```
+--------+--------+--------+------------------+--------+
|  cmd   | decl_lo| decl_hi|    payload ...   | cksum  |
+--------+--------+--------+------------------+--------+
  decl_len = payload_len + 1
  cksum    = 0xAA - (cmd + decl_lo + decl_hi + sum(payload))   (mod 256)
```

依据：

- RVA `0x62ca0` 的校验函数：`0xAA - (base + Σpayload)`，`base` 为
  `cmd + decl_hi + decl_lo`（调用点 `0x65c3f`，`r9b = 0xAA`）。
- 某些命令类型不校验 payload，尾部写常量 `0x88`（指令 `movb $0x88,0x62(rsp)`）；
  接收端对 `0x88` 应宽松处理。
- `cmd` 字节的位域与 MP flags 相同：`cmd = (class << 4) | (sub << 1) | no_ack`。
  例如公开实现中的 `0xA8` = class `0xA`, sub `4`；`0x01` = class 0, sub 0,
  `no_ack=1`。

---

## 4. 读写时序

### 4.1 发送（offset `0x000`）

1. 组装 eSPI 包装（8 字节，含递增序号）+ MP 帧 + Goodix 命令体。
2. 按 8 字节对齐写入邮箱 `+0x000`。
3. 拉 write-done GPIO（pad 387）：`0 → 1 → 4 ms → 0`，前后各留
   50 µs / 200 µs。
4. EC 取走命令并转发给传感器；实测写完后命令区被清零。

Windows 端把 4 字节 MP 头与其余部分分两次写、中间 2 ms；本驱动也保留
`tx_mode` 参数（0=一次写，1=分两次写一次脉冲，2=分两次写两次脉冲），
默认 0 已在实机验证通过。

### 4.2 接收（offset `0x200`）

1. 中断（GpioInt pad 133 → IRQ 183）触发后，从 `邮箱 + 0x200` 读窗口。
2. 校验 MP 头，得到 `len`，总长 `total = len + 4`。
3. 若还有剩余，每次最多 `0xE00` 字节、按 8 对齐继续从 `+0x200` 读
   （EC 会滚动更新窗口），每块之间拉一次 read-done GPIO（pad 171，
   `1 → 50 µs → 0 → 200 µs`）。
4. 把完整帧交给上层；`0xE00 + 0x200 = 0x1000`，正好等于邮箱窗口大小。

### 4.3 复位与电源

- `HardResetMcu`（RVA `0xf660`）对项目类型 12 直接打印
  `not support hard reset for EC projects %d` 并返回成功——**GXFP5130 没有
  硬件复位线**，只能软复位。
- 另一条分支（项目类型 9/10/11/13/14/17）通过 ACPI `_DSM` 复位，GUID 常量
  `{EA9A9C7B-7BAB-468C-84FA-A9A59F8AD1CB}`（字节序列
  `7B 9C 9A EA AB 7B 8C 46 84 FA A9 A5 9F 8A D1 CB`，RVA `0xf02a` 起）。
  该路径不适用于本机。
- 上电后建议 `msleep(150)` 再做首次命令；睡眠/关机前发送 `0x0E`（电源状态）
  与 `0xD2`（cleanup）。

---

## 5. 命令集合

命令字节来自 `cmd = (class << 4) | (sub << 1) | no_ack`，与公开实现交叉核对：

| 命令 | 含义 | 备注 |
| --- | --- | --- |
| `0xA8` | 读取固件版本 | 响应 payload 为 ASCII 版本串 |
| `0xAF` | 触发 MCU 状态查询 | payload 5 字节：`55 TS_lo TS_hi 00 00` |
| `0xAE` | 读取 MCU 状态 | 20 字节，byte1 bit1 = TLS connected |
| `0xB0` | ACK | 配置/初始化类命令的应答 |
| `0x90` | 上传 MCU 配置 | 可选 |
| `0x0E` | 通知电源状态 | 休眠前 |
| `0x01` | 协议初始化 | `no_ack=1` |
| `0xD2` | sleep/cleanup | 卸载时 |
| `0xA1` | 软复位设备 | 谨慎使用 |
| `0xD4` | TLS 解锁 | 需要用户态 PSK |

固件版本响应：本机实测为 `GF_GCC_EC_20068`（16 字节 ASCII，尾部带 `\0`），
是判断驱动通路是否正确的直接依据。

---

## 6. TLS 与上层协议

`gfspi.dll` 静态链接了 **mbed TLS 3.4.1**，字符串可见：

```
mbed TLS 3.4.1
TLS-PSK-WITH-AES-128-GCM-SHA256 / ...-256-GCM-SHA384 / ...CBC-SHA256
tls handshake package, state: %d
tls_callback_recv / tls_callback_send
```

结论：

- 命令通道在 TLS 握手完成后承载 TLS 1.2 record，密码套件为 PSK 系列。
- PSK 不硬编码在驱动中，而是来自传感器 OTP/配对流程；Windows 侧首次使用
  时下发给 MCU，并把副本缓存（`PSKCacheSwitch`、`PSKSimulationSwitch`）。
- 指纹模板、匹配、活体判断在 `AlgoMilan.dll` / `AlgoChicago.dll` 中完成，
  不属于内核驱动职责。

因此 Linux 侧正确的分层是：

```
fprintd/libfprint  ← TLS-PSK + 模板匹配（用户态）
        ↓  /dev/gxfp（本驱动）
内核 gxfp5130.ko   ← 邮箱传输、GPIO、帧校验
        ↓
EC eSPI 邮箱 0xFE800000
```

---

## 7. 与公开资料的交叉验证

分析过程中检索到同型号芯片的公开工作（可作为独立验证，非本驱动代码来源）：

- LKML 补丁系列：`drivers/misc: add Goodix GXFP5130 eSPI fingerprint sensor driver`
  <https://lkml.iu.edu/hypermail/linux/kernel/2607.2/04941.html>
- 社区整合仓库：<https://github.com/Han-Haocheng/gxfp5130-linux>

其公开描述中的关键事实与本逆向结果一致：

- 传感器经 EC eSPI 邮箱访问，邮箱 `0xFE800000`；
- 3 个 GPIO：write-done、read-done、irq；
- 4 字节 eSPI/MP 包装 + Goodix MP 帧；
- 命令 `0xA8/0xAE/0xAF/0xB0/0x90/0x0E/0x01/0xD2/0xA1/0xD4`；
- Goodix 校验 `0xAA - Σ`，MP 校验 `flags + len_lo + len_hi`；
- 读块上限 `0xE00`、邮箱最小 `0x1000`。

本驱动是独立实现（未复制上述工程代码），仅使用这些功能性协议事实。

---

## 8. 实机验证结论

| 项目 | 结果 |
| --- | --- |
| 邮箱 | `0xFE800000`，`0x1000` 字节，`tx=0x000` / `rx=0x200` |
| write-done | ACPI GpioIo #1（pad 387）→ gpio 899 |
| read-done | ACPI GpioIo #2（pad 171）→ gpio 683 |
| 中断 | ACPI GpioInt（pad 133）→ IRQ 183，正常计数 |
| 固件握手 | `0xA8` → `b0 03 00 a8 01 4e`（ACK）+ `a8 11 00 "GF_GCC_EC_20068" f2` |
| MCU 状态 | `0xAF` 触发 + `0xAE` 查询 → 23 字节状态 |
| 卸载/重载 | 多次 `rmmod`/`insmod` 无 GPIO 泄漏 |

仍未覆盖（属于用户态，不在本驱动范围）：

1. TLS-PSK 会话与 PSK 提取/配对；
2. 指纹采集、模板与匹配（libfprint/fprintd 侧）；
3. 休眠/唤醒的完整电源状态机（`0x0E`/`0xD2` 已实现基础收发）。

---

## 9. 复现分析方法

```bash
# 定位字符串、函数与交叉引用
python3 tools/pe.py gfspi.dll "SpiSendDataToDevice" w
objdump -d --no-show-raw-insn gfspi.dll > gfspi.asm
python3 tools/xref.py gfspi.dll func_strings.json
```

辅助脚本见 `../tools/`（PE 段解析、字符串搜索、函数-字符串交叉引用）。
