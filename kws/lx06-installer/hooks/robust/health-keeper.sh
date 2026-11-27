#!/bin/sh
# 旧路径兼容入口；不再包含探活循环、flash 心跳或整机重启逻辑。
# 真正的系统服务由 procd 直接跟踪原生 C daemon；重复调用会被其单实例锁拒绝。
# 保留本文件只为避免旧启动配置引用失效，不能绕过原生 keeper。
exec /usr/bin/lx06-keeper daemon