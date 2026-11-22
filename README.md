# 一加平板 3 Pro 原生 Ubuntu

[English](README.en.md)

面向 **OnePlus Pad 3 Pro（OPD2513，SM8850P）** 的 Ubuntu 26.04.1 LTS arm64 移植。开发机已通过 RAM 临时引导进入原生 GNOME，并从内置 UFS 运行 Ubuntu 根文件系统。其他一加平板即使共用 SoC，也不可混刷。

这是开发版源码发布。2026-11-23 的 SuperVOOC 修复已通过真机短时带载验证，用户确认充电恢复正常；独立冷启动与完整充电周期仍待验收。保留原厂 Recovery 及其启动依赖；开发机已实测 Recovery 可以充电。

## 驱动与功能

| 模块 | 当前状态 |
| --- | --- |
| 显示与触控 | 内屏与 GNOME Wayland 可用，触控用于实际桌面操作；笔、休眠唤醒、各刷新率待单独验证。 |
| CPU、内存与虚拟化 | 8 核运行，CPU 名称可正确识别；实际可用内存与标称容量的差异仍待解决。测试内核没有 `/dev/kvm`，KVM 未启用。 |
| GPU | Adreno 840 硬件 EGL 渲染和 GNOME Wayland 已真机验证，需要匹配的签名固件；部分遥测仍无驱动数据。 |
| 存储与 USB | 内置 UFS 根文件系统与 USB 维护链路已验证。USB 数据链路正常不代表充电正常。 |
| Wi-Fi / 蓝牙 | WCN786x 的 Linux 驱动与板级固件已适配到开发环境；蓝牙曾扫描到 BLE。稳定重连、键盘配对和休眠恢复尚未完整验收。 |
| 内置音频 | 麦克风录音和扬声器播放已验证；六路功放有逐路测试信号，八个物理单元尚未逐个声学验收。 |
| 前后摄像头 | 已接入系统相机，后摄可取帧。自动对焦路径已实现，最终清晰度和前摄画质待真机验收。 |
| 手电筒 | GNOME 截图按钮左侧的系统级开关已部署；修订档位的驱动读回通过，目视发光待确认。 |
| 充电 | 原厂 SuperVOOC 双泵短测约 100 秒，泵端峰值 58.29 W、平均 55.62 W，净充入 263.301 mAh；6.1 A 请求保持，用户确认正常。普通 USB、标准 PD、完整周期与冷启动仍待验收。 |

上表是开发机的验证记录，不表示重新构建的基线镜像自动包含所有试验改动。补丁入口和各自基线见 [补丁说明](patches/README.md)。

## 2026-11-23 快充修复

修正了将电池 44°C 分界误用于接插件温度的错误，避免约 14 秒后从 6.1 A 突降 3 A。接插件沿用 55°C 退出保护，保留电池温度、电压、SOC、过流及双泵故障保护。额定 67 W 不等于全电量恒功率；以上功率来自泵端 VBUS × 双泵 IBUS，区别于适配器输入或电池净功率。

[驱动、持久化安装与镜像打包说明](driver-fixes-20261119/supervooc-opd2513/README.md#已验证版本的持久化2026-11-23)包含开机服务、校验值、回退方法和验证范围。[机器可读实测摘要](driver-fixes-20261119/supervooc-opd2513/validation-20261123.json)随源码保存。提供 Ubuntu 根文件系统持久化脚本及内置同一模块和参数的 RAM 镜像打包方法；本次按用户要求仅发布源码与说明，尚未安装持久化或重启验证。继续使用 `fastboot boot`，保留原厂 Recovery；尚未验收独立冷启动。

## 构建与安装

1. **准备主机和恢复路径。** 使用 Linux 主机、可用的 `fastboot`、ARM64 交叉编译工具链、`qemu-system-aarch64`、`squashfs-tools`、`cpio`、`zstd`、`e2fsprogs`、`device-tree-compiler` 和 `ripgrep`，预留至少 20 GiB 空间。先备份平板数据，确认设备确为 OPD2513、已能进入原厂 Recovery 且可充电。建议充足电量后再做引导测试。
2. **准备基础文件。** 获取并校验官方 Ubuntu 26.04.1 Desktop arm64 ISO；从 `casper/` 提取 `minimal.squashfs` 和 `minimal.standard.squashfs` 到同一目录，并为两者生成 `SHA256SUMS`。固件须从有权使用的、匹配 OPD2513 的来源获取，见下方目录清单；不要复用其他机型的校准数据。
3. **构建参考内核和根文件系统。** 从下方命令开始。输出是 `ubuntu-desktop-root.ext4`，先看 `assembly.log` 中的 `OPD2513_DESKTOP_ASSEMBLY_OK`，再校验 `SHA256SUMS`。这是**根文件系统镜像，不是 Fastboot 启动镜像**。
4. **仅临时引导验证。** 已有单独构建并校验过的 OPD2513 专用 RAM 启动镜像、且供应好其所需的根文件系统后，才使用 `fastboot -s "$FASTBOOT_SERIAL" boot "$OPD2513_RAM_BOOT_IMG"`。变量需设成实际目标选择器和 RAM 镜像路径。此仓库没有预制 RAM 启动镜像，也没有通用写盘脚本；永久安装步骤等独立启动、充电和原厂 Recovery 回归通过后再发布。不要向 `boot`、`recovery` 或整盘刷入上述 ext4。

```bash
git clone https://github.com/map220v/sm8850-mainline.git iceland-kernel
git -C iceland-kernel checkout --detach b9e356dfbd7a8c70a891b0ef819ba5f5eff1774c
bash build-kernel-baseline.sh kernel-build

build="$PWD/kernel-build"
stage="$PWD/kernel-baseline-artifacts"
release=$(make -s -C iceland-kernel O="$build" ARCH=arm64 kernelrelease)
mkdir -p "$stage"
make -C iceland-kernel O="$build" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  INSTALL_MOD_PATH="$stage" INSTALL_MOD_STRIP=1 modules_install
cp "$build/arch/arm64/boot/Image.gz" "$stage/"
cp "$build/arch/arm64/boot/dts/qcom/kaanapali-oneplus-iceland.dtb" "$stage/"
cp "$build/.config" "$stage/"
tar -C "$stage" --exclude="lib/modules/$release/build" \
  --exclude="lib/modules/$release/source" --zstd \
  -cf "$stage/$release-modules.tar.zst" "lib/modules/$release"
(cd "$stage" && sha256sum Image.gz kaanapali-oneplus-iceland.dtb .config \
  "$release-modules.tar.zst" > SHA256SUMS)

# Set ISO_LAYER_DIR to your verified ISO layer directory first.
bash assemble-desktop-rootfs.sh "$ISO_LAYER_DIR" "$(command -v qemu-system-aarch64)" ubuntu-output
(cd ubuntu-output && sha256sum -c SHA256SUMS)
```

本地固件输入：`local-firmware/novatek/novatek_nt36536_fw.bin`、`local-firmware/qca/{gngbtfw20.mbn,gngbtnv20.bin}`、`local-firmware/qcom/kaanapali/iceland/{gen80200_zap.mbn,adsp.mbn,adsp_dtb.mbn,cdsp.mbn,cdsp_dtb.mbn}`。脚本验证签名 ZAP 输入的预期 SHA-256；不要跳过型号、版本或校验检查。固件、分区备份及构建镜像未随源码发布。

已运行 Ubuntu 且有 `/sys/class/leds/white:flash-1/brightness` 的设备可单独安装[系统手电筒](driver-fixes-20261119/torch-quick-settings/README.md)；下一次登录 GNOME 后加载。新根文件系统默认内置该功能。
