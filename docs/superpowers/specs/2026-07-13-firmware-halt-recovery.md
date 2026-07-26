# Cornix 固件停机自恢复设计

## 背景

2026-07-13 复盘偶发断连：右半蓝灯呼吸（peripheral 广播等待重连），左半对右半、
对主机、对按键全部无响应，仅手动重启左半可恢复。

[2026-07-04 的双断链兜底](2026-07-04-ble-reconnect-recovery-design.md)只覆盖
“固件存活、链路卡死”的情形。本次故障是固件本体停机，兜底策略没有机会运行：

- Zephyr 4.1 默认 fatal 处理为 `arch_system_halt()`——锁中断后死循环，直到断电。
- `CONFIG_BT_ASSERT=y`（Zephyr 默认）使 BLE 主机栈与链路层的内部断言经
  `k_oops()` 进入上述停机路径。nRF52 链路层在同时维持两条连接加广播/扫描时
  偶发断言属于已知问题。
- 固件未启用任何看门狗，停机后无硬件手段复位。

## 方案

在 cornix_indicator 模块内新增 `cornix_recovery.c`，两层互补：

### Fatal 错误改为冷重启

以强符号覆盖 weak 的 `k_sys_fatal_error_handler`：`LOG_PANIC()` 后
`sys_reboot(SYS_REBOOT_COLD)`。BLE 断言、硬错误、栈溢出等从“永久停机”
变为数秒内重启回归。板级 `zephyr_library` 以 whole-archive 方式链接，
覆盖可靠。

### 硬件看门狗

- nRF52840 `wdt0`，超时 30 秒（指示灯空闲轮询间隔 5 秒的 6 倍），
  `WDT_FLAG_RESET_SOC`，调试器挂起时暂停。
- 由指示灯工作循环（低优先级工作队列）喂狗：工作队列卡死、死锁、
  中断风暴等不经过 fatal 路径的挂死也会被硬件复位。
- 懒启动：首次喂狗时才武装。若指示灯初始化失败导致喂狗循环不存在，
  退化为“无看门狗”，而不是 30 秒重启循环。
- 深睡为 System OFF，LFCLK 停止，看门狗随之暂停；唤醒即复位并重新武装，
  不会因深睡误触发。

### 崩溃循环保护

冷重启把“永久停机”换成了“重启”，需要防止确定性崩溃退化为无限重启循环：

- 崩溃计数保存在 GPREGRET2（GPREGRET1 留给 UF2 bootloader magic），带
  魔数校验，软复位与看门狗复位均可幸存，不写 flash；fatal 路径无锁操作。
- fatal 冷重启在 fatal 处理器内计数；看门狗复位与 CPU lockup 不经过 fatal
  处理器，由 EARLY 初始化阶段采样 RESETREAS 计数（仅寄存器访问，无内核与
  驱动依赖），采样后清零。确定性早期 lockup 因此也逃不过计数。
- 连续 3 次崩溃后经 retention bootmode 进入 UF2 bootloader 等待刷机，
  而不是继续重启。该检查在 APPLICATION 初始化路径执行；`bootmode_set`
  可能取锁，不能放进 fatal 或 EARLY 上下文。
- 若崩溃发生在 APPLICATION 初始化之前（bootloader 检查永远执行不到），
  fatal 处理器与 EARLY 采样器都会在计数严格超过阈值（第 4 次）时以无锁的
  System OFF 断开重启循环并保电；“严格超过”给 APPLICATION 路径留出一次
  进 bootloader 的机会。
- 稳定运行 60 秒后计数清零。
- 进入 bootloader 失败时清零计数并继续正常启动——绝不锁中断空转：持续尝试
  或干净断电都好过耗电的砖。

## 边界

- 各层机制左右半均生效（shield 同时用于 central 与 peripheral）。
- 确定性崩溃最多重启 3 次即停在 bootloader；连初始化都到不了的早期崩溃
  在第 4 次进入 System OFF，均不会无限循环耗电。
- 不修改 ZMK/Zephyr 上游源码。

## 验证

- Docker 构建左右半固件，确认 Kconfig（`CONFIG_WATCHDOG=y`、
  `CONFIG_WDT_NRFX=y`）与链接无错误。
- 实机长期观察：复现同类故障时左半应在最迟 30 秒内自行重启，
  右半蓝灯随后熄灭恢复连接。
