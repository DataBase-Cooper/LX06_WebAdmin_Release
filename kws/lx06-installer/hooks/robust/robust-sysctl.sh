#!/bin/sh
# V14.3 开机：内核参数加固（幂等；内核没编译的 sysctl 自动跳过）
# panic=15：panic 后 15s 自动重启（原 3s 偏短，留 pstore/syslog 落盘窗口）
[ -w /proc/sys/kernel/panic ] && echo 15 > /proc/sys/kernel/panic 2>/dev/null
# HUNG_TASK/SOFTLOCKUP：4.9.61 内核未编译这两个 config（sysctl 不存在），
# 将来重编内核带上后自动生效，脚本不用改。
[ -w /proc/sys/kernel/hung_task_panic ] && echo 1 > /proc/sys/kernel/hung_task_panic 2>/dev/null
[ -w /proc/sys/kernel/hung_task_timeout_secs ] && echo 60 > /proc/sys/kernel/hung_task_timeout_secs 2>/dev/null
[ -w /proc/sys/kernel/softlockup_panic ] && echo 1 > /proc/sys/kernel/softlockup_panic 2>/dev/null
# 软看门狗 kernel.watchdog=1：不设。内核 4.9.61 无用户态喂狗进程时，
# 软狗 watchdog_thresh(30s) 到点直接 reboot → 每次开机 30s 死循环。
# 硬件狗 aml_wdt 已由 procd(PID1) 持有并喂狗（真机验证），无需软狗。
exit 0
