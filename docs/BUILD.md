# 编译与打包

以下以仓库根目录为起点。下载包、设备配置和编译输出使用本地独立目录。

## Windows 刷机工具

在 Windows 安装 Go 1.25+ 和 Git。页面、图标与 WebView2 加载器随源码提供。Go 测试中的设备脚本语法检查会调用 WSL。

```powershell
Set-Location kws/lx06-installer
go test ./...
go vet ./...
go build -trimpath '-ldflags=-s -w -H=windowsgui -X main.version=V20261127.5-rc42 -X main.buildDate=2026-11-27' -o LX06Installer.exe .
go build -trimpath '-ldflags=-s -w -H=windowsgui' -o elevhelper.exe ./elevhelper
```

把两个新 EXE 放入一个完整安装包的本地副本。保留 flash-tool、tools 和三个镜像目录；界面会展示两种选择。要生成新的 ZIP，参考 scripts/build_release.py --help，提供镜像目录、完整安装包输入及对应的文件清单哈希。重新生成图标需 Python、Pillow 和 rsrc。

## Linux / WSL 与 ARM

安装 Python 3.11+、make、GCC 交叉编译器 arm-linux-gnueabihf-gcc、对应 binutils 与 libc 开发工具、squashfs-tools 和 qemu-user-static。宿主测试还需要 libssl-dev、libopus-dev 与 Python websockets。交叉编译时，交叉编译器需要能找到 OpenSSL 和 Opus 的头文件。

从合法取得的 LX06 增强固件包解压镜像，提取目标运行库。目标为 ARM hard-float、glibc 2.25、OpenSSL 1.0。编译宿主所用的新系统库和要部署的目标运行库分别处理。

```bash
python3 kws/lx06-installer/scripts/extract_release_libraries.py \
  --image /path/to/official-ssh/root.squashfs \
  --output "$PWD/build/lx06-arm-libs"
export LX06_LIB="$PWD/build/lx06-arm-libs"
export LX06_SYSROOT_LIB="$LX06_LIB" LX06_LEGACY_LIB="$LX06_LIB" LEGACY_LIB_DIR="$LX06_LIB"
export OPUS_LIBRARY="$LX06_LIB/libopus.so.0.6.1"
```

## 小智和豆包客户端

```bash
(cd kws/scratch/xz-client-rewrite && make test && bash scripts/build-armhf.sh)
(cd kws/lx06-installer/runtime/doubao && bash build.sh host && bash build.sh)
```

输出分别是小智目录下的 build/armhf/xz_client 和豆包目录下的 build/doubao-client。可用 GCC_LIB_DIR、CRT_DIR 指向本机交叉工具链的启动对象。修改 ARM 客户端后，需要重做对应脚本载荷以及脚本内的客户端 SHA-256。

```bash
cd kws/lx06-installer
python3 scripts/build_xz_pause_update.py \
  scripts/examples/user-scripts/V20261127.5-rc42/xz_watch.sh \
  ../scratch/xz-client-rewrite/build/armhf/xz_client \
  build/repacked/xz_watch.sh
```

豆包请使用 scripts/replace_script_payload.py 的 replace_payload 函数。输入原脚本字节、目标字面量 $WORK/client.gz 和新 ARM 客户端字节。将返回值写到新脚本路径。生成后执行 sh -n，并校验解出的客户端 SHA-256。

## 设备端 C 组件

在 kws/lx06-installer 中可按模块编译：

```bash
mkdir -p build
bash scripts/build_ota_verify.sh build/lx06-ota-verify
bash scripts/build_keeper.sh build/lx06-keeper
bash scripts/build_script_settings.sh build/script-settings
bash scripts/build_processed_mic.sh
(cd hooks/wakeword && sh build_ww_mfcc.sh robust)
```

要构建云端复核工具 wake_cloud_asr，再为唤醒构建设置 OPUS_A 和 OPUS_INC，分别指向目标 ARM 静态 Opus 库和头文件。Web88 构建要求 ARM OpenSSL 1.0 前缀，其中 include/openssl 是同版头文件，lib 下包含从镜像提取的 libssl.so.1.0.0 和 libcrypto.so.1.0.0。

```bash
LX06_TLS_PREFIX=/path/to/arm-openssl-1.0 LX06_FW_VERSION=V20261127.5-rc42 \
  bash scripts/build_webd_standalone.sh build/lx06webd
bash scripts/build_model_adapter.sh build/model-adapter
```

用 arm-linux-gnueabihf-readelf -hV FILE 检查 ARM 与依赖符号。动态加载验证使用同一版固件的目标库。不同工具链或固件基底会改变重编产物的哈希。

## SquashFS 与在线升级包

保留合法取得的原厂内核。以对应增强镜像为基线，在 Linux 文件系统解包，替换目标组件并同步版本和组件清单。压缩配置为 XZ、128 KiB block 和 ARM BCJ。构建后再解包核对权限、符号链接和文件差异。已有发行镜像包含上游厂商和继承的二进制组件，见 [NOTICE](../NOTICE.md)。

```bash
python3 scripts/online_upgrade_package.py build \
  --kernel /path/to/stock-boot.img --rootfs /path/to/root.squashfs \
  --version V20261127.5-rc42-local --output build/local.lx06ota
python3 scripts/online_upgrade_package.py verify build/local.lx06ota
```

从通过校验的旧 .lx06ota 取得内核时，前 4096 字节为头部，接下来 kernel_size 字节为 Android boot.img。先验证包，再在可恢复的自有设备上检验升级与切槽。

## 重新打包本版变更

Web88 通用条件比较由 hooks/webd/app-extension.h 解析，由 hooks/webd/index.html 呈现。修改该声明协议时，需要一起重编 lx06webd 与 script-settings。增强镜像将它们和页面安装到 usr/sbin/lx06webd、usr/lib/lx06/script-settings、usr/lib/lx06/web/index.html。同步更新 usr/lib/lx06/wake-stack.json 中相应文件的 SHA-256，再构建 SquashFS 和 .lx06ota。

Windows 刷机组件和签名目录作为外部依赖，文件逐字节核对 driver_payload_windows.go 的哈希。CER 取自 WorldCup_Device.cat 的实际 PKCS#7 签名者。改变外部驱动或证书时需要重新审核完整签名链和目标系统。

## rc42 脚本与 Web88 验证

七份配套脚本在 `kws/lx06-installer/scripts/examples/user-scripts/V20261127.5-rc42/`。在 Web88 按需上传并单独配置。`mcp_task_watch.sh` 直接由 Shell 解释。修改后先运行 `sh -n` 检查语法。

rc42 的 Web88 镜像改动集中在 `hooks/webd/index.html` 的通用控件刷新。基于已带通用 `notEquals` 协议的 rc41 增强镜像时，替换该页面并更新 `/lx06-firmware-version` 和 `/usr/lib/lx06/wake-stack.json` 的版本即可。rc37 的扩展解析器需要随页面升级。镜像保留文件属性，并用 `mksquashfs` 的 `-comp xz -b 131072 -Xbcj arm -tailends -no-xattrs -no-exports` 选项重建。

在仓库根目录运行以下回归。`PYTHON=python3` 用于 Linux 或 WSL 上的脚本生成测试。

```bash
PYTHON=python3 node --test kws/lx06-installer/hooks/webd/test_web_extensions.cjs
(cd kws/scratch/xz-client-rewrite && make test)
```

固件镜像、原厂内核、配套驱动和烧录工具是完整成品打包的外部输入。源码版的编译步骤可分别生成安装器、ARM 组件和脚本。
