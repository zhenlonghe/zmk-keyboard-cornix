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

## 边界

- 两层机制左右半均生效（shield 同时用于 central 与 peripheral）。
- 确定性启动崩溃会表现为重启循环而非停机；对键盘而言两者同样不可用，
  但偶发故障可自愈。
- 不修改 ZMK/Zephyr 上游源码。

## 验证

- Docker 构建左右半固件，确认 Kconfig（`CONFIG_WATCHDOG=y`、
  `CONFIG_WDT_NRFX=y`）与链接无错误。
- 实机长期观察：复现同类故障时左半应在最迟 30 秒内自行重启，
  右半蓝灯随后熄灭恢复连接。
