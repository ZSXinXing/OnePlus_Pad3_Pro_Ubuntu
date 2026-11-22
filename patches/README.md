# Patch inventory / 补丁清单

These patches are separate from the baseline rootfs build. Apply only to the matching upstream source tree and check with `git apply --check` first. Distribution packages, board firmware and tuning data are separate inputs. Each patch retains the licensing of the code it modifies; refer to the matching upstream tree.

这些补丁不由基线构建脚本自动应用。先定位对应源代码版本，再用 `git apply --check` 检查。发行版软件包、固件和原厂调音数据是独立输入。

| Patch | Baseline / 基线 |
| --- | --- |
| `resources-1.10.2-adreno.patch` | Ubuntu Resources `1.10.2-0ubuntu3.1`; Adreno and CPU reporting / GPU 与 CPU 信息显示 |
| `0001-lscpu-recognize-qualcomm-oryon2.patch` | util-linux `2.41.3`; Qualcomm CPU identification foundation / 高通 CPU 识别基础补丁 |
| `libcamera-0.7.0-smartsens-af.patch` | libcamera `v0.7.0` (`b7854fd07d42168f099b5ce30d1702e0e0875bf5`); SmartSens focus and software ISP / 对焦与软件 ISP |
| `focus-flash-board.patch` | OPD2513 camera development kernel after CAMSS and SmartSens bring-up; a reference delta, not standalone on the baseline. For only the flashlight, the build uses `opd2513-flash.dtso`. / 依赖相机开发内核前置补丁，仅作参考；单独手电筒使用设备树覆盖文件。 |
| `opd2513-tfa9865-tdm.patch` | TFA9865 codec on the port's Linux kernel; does not include the rest of the AudioReach stack, hardware calibration or protection profiles. / 功放 TDM 改动，不包含其余音频拓扑、校准与保护配置。 |

The reference kernel comes from `map220v/sm8850-mainline` at the commit pinned in the main README. The OPD2514 `iceland` work and the matching OPD2513 OnePlusOSS device tree were references, not interchangeable device releases.

## SuperVOOC charging — 2026-11-23 / 快充修复

- `opd2513-battmgr-powered-vooc.patch`: battery/SoCCP handoff and live capacity interfaces against kernel `b9e356dfbd7a8c70a891b0ef819ba5f5eff1774c`. / 电池与 SoCCP 交接、实时电量读取依赖。
- `opd2513-charge-pump-i2c.patch`: board charge-pump bus and primary PHY interrupt description. / 双泵总线与主泵握手中断。
- [Driver and verification record](../driver-fixes-20261119/supervooc-opd2513/README.md): fixes the incorrect connector 44°C derating threshold. Short-run pump peak 58.287 W, mean 55.621 W, net battery gain 263.301 mAh; user confirmed normal charging. / 修复接插件44°C错误降档，保留55°C保护；完整周期和冷启动仍待验。

These patches are dependencies of the experimental board port, not a complete kernel distribution. Persistence tools are prepared but were not installed on the tablet in this publication task. / 补丁是本移植的依赖，不构成完整内核；持久化工具仅准备，本次未在平板安装。
