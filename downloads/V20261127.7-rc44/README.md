# LX06 V20261127.7-rc44 下载

| 文件 | 适用场景 |
| --- | --- |
| [Windows 一键刷机包](https://github.com/DataBase-Cooper/LX06_WebAdmin_Release/releases/download/V20261127.7-rc44/LX06Installer-V20261127.7-rc44.zip) | 完整解压后运行 LX06Installer.exe。需要 Web88 时选择「官方固件 + SSH」。 |
| [固件在线升级包](https://github.com/DataBase-Cooper/LX06_WebAdmin_Release/releases/download/V20261127.7-rc44/V20261127.7-rc44.lx06ota) | 已有 Web88 时，在固件升级窗口上传。保留 DATA 可保留账号、录音和设置。 |
| [八个配套脚本](https://github.com/DataBase-Cooper/LX06_WebAdmin_Release/releases/download/V20261127.7-rc44/LX06Scripts-V20261127.7-rc44.zip) | 解压后在 Web88 按需上传。含新版小智、新增米家 MCP 及其他六个配套脚本。 |
| [SHA-256 校验清单](SHA256SUMS.txt) | 下载后可用 Windows Get-FileHash 核对文件。 |

RC44 的 Web88 在安装应答丢失时继续检查启动结果，升级窗口提供可复制的报错日志。小智米家控制默认关闭。在扩展中启用并选择已上传的 mijia_mcp.sh，即可复用原厂小爱登录，不用逐台添加设备。

已在 RC42 真机确认新版小智能关闭客厅窗帘，空调模式切换、关机和回复正常。整机刷写、物理 USB 驱动安装及实际切槽待设备验收。

[发行说明](https://github.com/DataBase-Cooper/LX06_WebAdmin_Release/releases/tag/V20261127.7-rc44) · [本版源码](https://github.com/DataBase-Cooper/LX06_WebAdmin_Release/tree/V20261127.7-rc44)
