# OPD2513 Ubuntu：双摄/闪光灯兼容的 SuperVOOC 驱动试验

适用本工程 OnePlus Pad 3 Pro OPD2513、`7.3.0-opd2513-fwzstd+`。保持原厂 Recovery 和 A 槽启动组件，使用 `fastboot boot` RAM 启动。2026-11-23 接插件阈值修复版已真机短测、用户确认正常，泵端峰值 58.29 W；以下早期试验按时间保留。当前验证范围与持久化步骤见文末，完整周期和独立冷启动仍待验。

## 本轮修正与证据

旧 powered 镜像使用缺少 CAMSS 节点、禁用 flash 的 DT，导致相机/手电筒回归。现在以已验证的 camera 内核、完整 initrd 和 DT 为基线，仅替换电池模块，再增加双泵及三个温度 ADC 通道。保留音频 i2c0..4、相机 i2c9 的固定别名；新增充电控制器当前编号 10、11，驱动按 DT 路径定位，不依赖动态编号。

`pack-camera-charge.py` 校验基线 kernel/initrd SHA-256、CAMSS/flash 状态及总线别名，原 initrd 的文件/模式全部保留，只替换 `qcom_battmgr.ko`。保留 `pmic_pdcharger_ulog.ko`，避免打包时回退电源日志依赖。重新打包结果与已启动镜像逐字节哈希一致。

VOOC probe 先取得温度 provider 和 USB mux，再创建从泵 dummy，避免 provider 的 `EPROBE_DEFER` 与跨总线子设备注销形成重复探测。电池 gate 日志明确包含电压、温度、SOC、认证，不再将温度门限误报为认证失败。

本机原厂资料：OnePlusOSS `android_kernel_modules_and_devicetree_oneplus_sm8850`，分支 `oneplus/sm8850_b_16.0_pad_3_pro`，提交 `8be53e8b737a83a33512d5e0106cccb010a5c24c`（OPD2513_16.0.6.103(CN01)）。参考文件：

- `vendor/oplus/kernel/charger/v2/voocphy/oplus_ap_voocphy.c`：期望电流初始化/编码、主从泵切换。
- `vendor/oplus/kernel/charger/v2/ufcs_ic/oplus_hal_sc8547d.c`：SC8547D 操作。
- 本机原厂 DT：`voocphy_current_default=0x3d`（6.1A 编码）、从泵开启阈值 1400mA、低流退出阈值 1100mA、每泵最大 5300mA、主 OCP 0x0f/从 OCP 0x0e。

基础协议来自 dappermint/iceland-supervooc 提交 `614469fabe593981538f492baf7fa1f19e61c462`，其 Pad 4 回放资料保留为测试夹具；不作为本机原厂参数。驱动覆盖 `stock_ask=61`，不沿用 Pad 4 的 73。默认仍为单泵 1.3A/30分钟、双泵 1.8A/180秒。显式启用 `oem_policy=1` 后，双泵使用下述官方分档子集，保留认证、温度、SOC、电压和停机保护，单次会话观察上限30分钟。

## 官方接口与分档接入（2026-11-22）

准确 OEM 分支的 `oplus_hal_adsp.c` 中，SoCCP 的 `fg_sm8350_get_batt_remaining_capacity` 使用 opcode `0x301`、属性69，单位微安时；SOC 使用同一opcode、属性68，单位百分之一。原先通用 `0x30` 的计数器在带载阶段不更新。本轮 `qcom_battmgr.c` 仅对 KAANAPALI 接入这两个官方属性；容量不是写死值。板端热替换前后的同次读数约9797mAh/9307mAh，说明两条接口数据不同；接口切换前后差值不是充电增量。直充关闭buck时，状态结合实测电池电流判定，避免只反映buck状态。

`opd2513_policy.h` 使用本机原厂 `svooc_charge_strategy` 与 `oplus_spec,soc_range=0/20/35/55/75/85/90`。仅实现既有允许范围（0..80% / 21..35°C / 3450..4450mV）中各曲线的首档：SOC≤55%为61个100mA单位；SOC>55%且温度<35°C为30；恰好35°C时，SOC≤75%为35，否则32。所有首档电压断点均高于4450mV，因此本范围内无需提前推进到后续电压档位。这是原厂策略的受限子集，尚未移植完整冷热、满电和高压回落曲线。

从1.8A起步，仅在从泵确认工作、适配器接受上一档请求后，每秒最多提高0.5A；降档立即请求，2秒未接受则退出。现有双泵保护持续检查。当前70% / 30..31°C，原厂表对应的请求上限是3A（适配器侧请求单位），并非固定64W或电池侧3A。

## 当前真机验证

2026-11-22，统一镜像 SHA-256：

`4525f3e7e017d18fda3423549b9b199c7eae6357d83c267b677bfa48ca26e56d`

在双泵/ADC 模块均已加载的同一次启动中：

- 前摄 SC820CS、后摄 SC1320CS 各实际采集 8 帧，640×480 ABGR8888，1,228,800 字节/帧，约 30 fps，退出 0。
- 真实 torch socket 命令控制 `yellow:flash-0`，读回 `26 → 0`，持续 0.5 秒，结束关闭。此轮属于控制和读回验证；光学发光已有历史用户确认。
- 两颗 SC8547D ID 均为 0x49；USB/电池连接器/MOS 温度分别读到 34139/33209/33326 m°C。
- 电量 71%；普通 USB 为 SDP，驱动保持待机；电池仍净放电，因此普通 USB 充电尚未验收。`charge_type=Fast` 与充电图标均不作为验收依据。
- 后续原厂适配器双泵连续运行180秒：3918帧，平均泵输入15,763,970µW，峰值总IBUS1973mA；从泵enabled/status-confirmed均为1，最高主泵芯片34°C。电池电流为正，约1.494..2.318A。旧通用计数器未变化，故此记录不证明容量净增，也不是新3A策略的功率结果。
- 官方容量模块、`oem_policy=1` VOOC模块均已成功热加载，未重启。新策略加载后再次验证前后摄各8帧及torch读回26→0，全部退出0。普通USB后续20次/约38秒采样均为Charging、+136..262mA，计数器仍未变化；持续容量净增尚待确认。
- 校验与模块见 `ram-rehearsal/oem-policy-20261122/validation.json`、`modules.sha256`；新RAM镜像在 `/tmp/opd2513-camera-oem-policy-20261122/boot.img`，SHA为 `4034e343c27a4113cffc1c4c641dabc1acd3030d2d78c126d55f33a363ac7469`，仅打包未启动。原先已启动的镜像单独保留。

证据：`device-tools/logs/supervooc-20261122/unified-camera-torch-dual.log`、`final-oem-profile-load.log` 和 `current-source-host-tests.log`。双摄清晰度/GUI、新官方策略带载与容量净增、64W、标准 PD、普通 USB 持续净充电、独立冷启动及本轮重进 Recovery 仍待验。新接口/策略证据另见 `oem-gauge-reload.log`、`oem-policy-reload.json`、`oem-policy-camera-gauge-check.json`。

## 重建与使用

在工程根执行；先使用当前内核构建环境单独构建 `iceland-kernel/drivers/power/supply/qcom_battmgr.c`。VOOC 外部模块构建需通过 `KBUILD_EXTRA_SYMBOLS` 指向该电池模块的 `Module.symvers`，不能用旧电池模块搭配新 VOOC 驱动。原配置的模块、符号表和已启动镜像在 `ram-rehearsal/camera-charge-20261122/`。本轮官方接口/策略模块与符号表另存 `ram-rehearsal/oem-policy-20261122/`，不能混淆两批验证结果。

重打包命令（输出目录应为新目录，约需 300MB；可放 `/dev/shm`）：

```bash
python3 driver-fixes-20261119/supervooc-opd2513/pack-camera-charge.py \
  --battery-module ram-rehearsal/oem-policy-20261122/qcom_battmgr.ko \
  --output-dir /dev/shm/opd2513-camera-charge-build
```

核对设备为 OPD2513/canoe 后，仅临时启动：

```bash
fastboot boot ram-rehearsal/camera-charge-20261122/boot.img
```

此镜像内置替换后的电池模块；VOOC 与 ADC 为本轮启动后加载，不是开机自动启用的发布版本。当前内核运行时依次加载 `qcom-vadc-common.ko`、`qcom-spmi-adc5-gen3.ko`、`opd2513_vooc.ko powered_trial=1 dual_trial=1 oem_policy=1`。不要直接操作 I2C 寄存器与已绑定驱动竞争。

`observe-charge.py` 只读记录电池计数器、电流、电压、温度和 USB 分类。可由 `systemd-run` 在板端执行，断开电脑后仍记录；默认等待适配器最多900秒，识别后记录240秒，最后保存内核日志和净增 mAh；拔出适配器时提前结束，避免把换回USB后的耗电算入适配器区间。部署状态登记在 `device-tools/logs/supervooc-20261122/active-observation.json`。旧180秒采集已经结束。本轮新的一次性服务为 `opd2513-oem-policy-observe`，已确认active：最长等待3600秒，正常接入适配器后记录900秒，输出 `/var/log/opd2513-charge-oem-policy-20261122`。本轮无需为部署再换线。采集器自身不改变充电控制。

主机回放测试：

```bash
cc -O2 -Idriver-fixes-20261119/supervooc-opd2513 \
  driver-fixes-20261119/supervooc-opd2513/tests/test_logic.c -o /tmp/opd-vooc-test
/tmp/opd-vooc-test
cc -O2 -Idriver-fixes-20261119/supervooc-opd2513 \
  driver-fixes-20261119/supervooc-opd2513/tests/test_stage3b.c -o /tmp/opd-dual-test
/tmp/opd-dual-test
```

官方策略测试：

```bash
cc -O2 -Idriver-fixes-20261119/supervooc-opd2513 \
  driver-fixes-20261119/supervooc-opd2513/tests/test_oem_policy.c -o /tmp/opd-policy-test
/tmp/opd-policy-test
```

当前头文件回放通过：identification 689170 项、dual replay/门限/故障 6420 项、官方允许范围49749组，另验证SOC/温度边界、从泵条件、升降档/确认超时与真实PHY变化请求编码。旧临时目录的完整 test_stage3b 依赖已移除的 ladder/firmware API，直接重编失败；本目录保留当前接口适用的测试，旧二进制的 2195886 项通过数不用于本轮验收。

## 停止与回退

观察器可用 `systemctl stop opd2513-charge-observe-20261122` 停止，驱动可 `rmmod opd2513_vooc` 退出并执行双泵停机和固件 buck 恢复。先向两泵发 stop，再逐个确认 off；发生 I2C 故障时不会先等待从泵而延迟主泵 stop。普通 USB 下仅加载驱动不会触发 SuperVOOC。

本轮未写 boot/vendor_boot/init_boot/dtbo/vbmeta/recovery 或槽位元数据。RAM 启动退出后依旧保留原厂救援路径；独立持久启动需另行按项目 AGENTS.md 的条件验收。


## 首次电流请求协商修正（2026-11-22 续）

上一版官方分档模块的原厂适配器记录已取回：154.876秒，SoCCP剩余容量9324.259→9381.897mAh，净增57.638mAh，末电量71→72%；平均泵输入15.795W，总IBUS峰值1966mA。因此真实充电成立，但升档效果仍未证实。旧版没有保存ask/sent，不能从最终泵电流单独断言适配器是否曾收到3A请求。

对照同一官方提交 `oplus_voocphy_non_vooc20_handle_get_batt_vol_cmd`，官方`ap_need_change_current`分支不要求以前已回复过电流。当前协议代码额外要求`sent != 0`，且升档又等待上一档回复；对先发BVOL、未发首次CURR的适配器会形成互相等待。新增真实`vl3_irq`帧序列测试在旧代码断言失败（exit134）；现在policy模式允许在sent=0时发0x7f请求提示，仍只在实际CURR回复后更新sent。发送提示后启动2秒超时，重复提示不延期，从泵确认与逐档升流保护均保留。

本轮协议、双泵及49749组policy范围/边界/ACK检查通过，模块重新构建并已热加载。新增只读`/sys/bus/i2c/drivers/opd2513_vooc/*-*/session_state`，不读写I2C，可直接看到target/ask/replied、峰值、变更提示次数和从泵状态。会话结束也写入同类摘要。自动观察器采集该字段，后续能区分未发送、已回复未升流、以及泵侧电流变化。

新模块/哈希/验证位于`ram-rehearsal/oem-bootstrap-20261122/`；上一版模块保留在`ram-rehearsal/oem-policy-20261122/`。本轮新模块没有适配器带载结果，仍未验收64W。一次性观察服务`opd2513-bootstrap-observe`已active，等待下一次正常充电，日志`/var/log/opd2513-charge-bootstrap-20261122`；等待上限1小时，接入后最多记录15分钟，拔出提前结束。本轮仅热替换VOOC模块，电池模块/启动镜像/Recovery均未写入。

后续离线复验：新增串联monitor与真实PHY的完整对话测试，覆盖首次BVOL提示、CURR回复1.8A、2.3A、2.8A、3A、各档TX/predata写白名单及总流保护，并验证从泵失去确认后降至1.8A；通过。控制端当前USB枚举中断，新增内容仅为测试，已加载模块保持上一版本。本轮未取回新带载日志、未新增设备写入，不能登记实际提速通过。

## Fastboot恢复与已确认的协商结果

2026-11-22再连接Fastboot后核对canoe/slot a/unlocked，Fastboot未提供电量变量。仅RAM启动4034e343镜像成功，Ubuntu实际电量63%（加载时62%），新boot_id 65e1fef8-40dc-4414-b6f3-9a0a1aabdcff；未改任何分区。已取回前一启动bootstrap服务记录：47.454秒净增13.099mAh，实际请求18→23→28→30，4次current reply、3次change notice，最后target/ask/replied均为30，从泵confirmed。末输入8.428V、主933mA/从945mA，约15.8W。因此首次请求修正的协商行为已实测通过，但它没有解决实际电流偏低，不能继续把慢充归为未请求到3A。

当前继续比对官方的电压反馈路径；新增只读session_state电压字段vbus/vbat/vout/vac及battery_code，以后直接在一次记录中核对适配器电流请求与电压反馈。没有调整电压/电流目标或保护。模块aa3c2233已编译和加载，三个温度通道和双泵正常，原厂电池模块随RAM镜像启动；自动单次观察服务opd2513-feedback-observe active，记录/var/log/opd2513-charge-feedback-20261122，等待1小时后接入采15分钟。本轮结果/回退模块位置见ram-rehearsal/oem-feedback-20261122/validation.json；64W提速问题仍开放。


## 电压反馈带载记录（2026-11-22 20:21）

本次原厂充电器记录167.615秒，电量61→63%，SoCCP计数器净增186.014mAh；161个双泵有效墙充采样的平均输入15.435W，采样峰值15.958W，内核逐帧平均15.443W。主/从泵中位电流928/926mA，VBUS中位8328mV。容量增量保持官方接口原始单位，不据此推算电池能量或额定功率。

161/161采样的battery_code与已确认的官方非双向、非parallel公式一致；VBUS−2×VBAT中位149mV。此结论仅验证采样反馈计算，不证明实际线上反馈字或适配器采用了3A。此前“协商3A完成”的措辞需收窄：已收到CURR命令并成功写入本地回复寄存器，replied不是对端电流生效确认。实际电流依旧约1.85A，64W问题保持开放。证据：工程device-tools/logs/supervooc-20261122/feedback-wall-analysis.json及同前缀原始记录。

新增只读遥测保存成功写入的last_current_tx/last_voltage_tx、VBUS低/高/正常回复次数及回复时压差；用于区分采样计算与实际TX写入、定位是否重复电压调整。控制算法、保护和电流目标均未变。模块298a126a40549e3d230d0c60ce6b9f0b65b8e1a43c4a812aa0ec3468c5d43049构建、热加载和sysfs字段读取通过，689170/6420/49749回放通过；新模块带载尚未验证。上一版aa3c2233保留可rmmod后热载回退。板端opd2513-window-observe已active，在随后正常充电时自动保存/var/log/opd2513-charge-window-20261122；本轮不要求再次换线。未写任何启动链分区或Recovery。


## 原厂升降流回复错误恢复

对照 OPD2513_16.0.6.103(CN01) 的 `oplus_ap_voocphy.c`，在收到电流调整后的 PHY 状态时检查 `TXDATA_WR_FAIL`。移植版遇到单独的 TX 写失败原先立即退出。现恢复上一个电流回复值并重新发出调整提示，最多重试三次，沿用已有响应时限。其他 PHY 错误仍终止会话。新增 `current_tx_retries` 记录实际触发次数。

ARM64 模块构建和协议 689170、双泵 6420、策略 49749 组回放通过，新增用例覆盖重试耗尽和混合 PHY 错误。模块 `7560e23af4f0f71cd8cd1e4cebfa9e29e968e2aabba79dac410ccf6ffaa2cc91`，产物和验证在 `ram-rehearsal/oem-current-tx-20261122`。本轮平板 USB 已枚举但 usb0 没有入站数据，ARP 无回复，控制端超时，主机重连后仍如此。改动未部署，保留上一轮驱动作为回退。既有 15.435W 带载记录未确认该错误发生，快充根因和修复效果待真机验证。

## 原厂协议回复时序

原厂 Android 版本的 `oplus_voocphy_pm_qos_update(400)` 在 Linux ≥ 5.10 实际使用 `0 µs` CPU 空闲延迟约束。Ubuntu 原先把 `400` 直接用作约束。现仅在带载原厂策略会话使用 `0 µs`，退出时释放，保留原电流曲线、保护和回复期限。会话结束时记录 IRQ 线程进入到回复写出的最大耗时，并单列电流回复。这个差异是否导致适配器没有升流仍需带载数据。

同一适配器的上一版记录约 109 秒，电量 59%，双泵平均输入估算 15.39 W，峰值 15.80 W，净增 83.837 mAh，电流回复重试为 0。新模块已构建、通过回放并在 Ubuntu 真机热载，自动采集服务已启动。证据和回退模块见 `ram-rehearsal/oem-phy-timing-20261122/validation.json`。

原厂适配器复测约 244 秒，239 个双泵有效采样。平均输入估算 15.39 W，峰值 16.14 W，电池计数器净增 75.977 mAh。3 A 请求期间总输入电流中位数为 1.871 A。最长电流回复写入耗时 340 µs，最长回复写入耗时 653 µs，电流回复重试为 0。按原厂设置调整 CPU 空闲延迟后，本轮平均功率和修改前的 15.39 W 相同。后续对照应关注原厂首次直接回复当前分档目标与 Ubuntu 从 1.8 A 分步请求的差别，同时保留双泵与电源保护。证据见 `device-tools/logs/supervooc-20261122/phy-timing-wall-analysis.json`。

## 原厂首帧电流回复对照

原厂 `oplus_voocphy_handle_ask_current_level_cmd()` 在首次电流查询时选取电池曲线，并直接将该档的 `current_expect` 编码到回复。Ubuntu 先前回放和真机均采用 1.8 A 起步，在从泵确认后再逐级提高到 3 A。两轮带载记录显示这个过程中本机 TX 写入的请求值改变，实际总输入电流停留在约 1.87 A。

新增默认关闭的 `oem_first_3a` 诊断开关，只针对曲线目标恰为 3 A 的会话。主泵开始工作、VBUS 正常、buck 交接和双泵保护寄存器核验通过后，尝试在首次电流回复中使用 3 A。启动宽限最长 2 秒，从泵确认后回到原有双泵策略。日志和只读状态中的 `first` 记录首次成功写入 PHY 的电流回复，供带载对照使用。实机部署和回退信息见 `ram-rehearsal/oem-first-current-20261122/validation.json`。

2026-11-22 真机确认首次回复 3 A 的效果。57% 至 59%、31–32°C 时，原厂适配器带载 240 秒，235 个双泵有效采样的平均输入估算为 26.23 W，峰值 26.76 W，电池计数器净增 225.312 mAh。上一版首帧 1.8 A 的日志记录平均为 15.26 W。修正与采集原始记录见 `ram-rehearsal/oem-first-current-v2-20261122/`。

高电流档继续以已验证的 3 A 起步，待两颗电荷泵确认工作后，才按现有逻辑逐级请求原厂曲线允许的电流，最高 6.1 A。现有温度、电压和保护检查继续生效。该版本已通过主机回放、ARM64 构建和真机热加载探测。热加载时平板为 59%，6.1 A 档带载验收仍待进入适用电量区间。部署、采集和回退记录见 `ram-rehearsal/oem-high-band-20261122/validation.json`。

## 6.1 A 首帧真机结果

54% 电量、插入时电池 34°C，原厂充电器识别为 `0x4b`。首帧直接回复原厂曲线的 6.1 A，确认双泵工作。第 5–30 秒的双泵输入平均为 55.29 W，峰值 56.96 W。全程平均 53.82 W，前 30.38 秒电池净增 45.85 mAh。会话在换回电脑 USB 附近报告 PHY `-71`，结束原因还需独立确认。原始记录见 `ram-rehearsal/oem-high-band-first61-20261122/validation.json`。

对照原厂 `oplus_ap_voocphy.c`、`oplus_voocphy.h` 和 SC8547D ADC 换算，当前双泵电流求和、75 mΩ 补偿、3404 mV 基准及 10 mV 编码步长与本机适用分支一致。原厂适配器识别表将 `0x4b` 列为 80 W SuperVOOC 类别。这个类别与设备实际输入功率是不同的测量量。本轮泵侧 VBUS 平均 8.841 V，最高采到 8.921 V，未触及 Ubuntu 10.0 V 软件过压门限。继续定位差距需原厂系统在相近电量和温度下的输入电压及电流，或同位置、兼容 SuperVOOC 的外部实测。当前模块为热加载，重启自动加载与完整充电周期尚待验证。

### 原厂常温高段试验

`oem_normal_high=1` 在启用原厂策略的双泵模式中开放原厂设备树的常温高段，默认关闭。电池温度超过 35°C 时，试验要求电池和电池连接器均低于 44°C，并要求电池健康状态为 `Good`、认证通过。当前支持的电量和电压范围沿用原有门限。首帧电流根据接入时的档位选择 6.1、3.5 或 3.2 A。会话内的电流上限仍受接入时档位限制。

在原有 RAM 镜像打包参数中添加 `--oem-normal-high` 可携带此试验配置。回退时加载上一轮模块，或使用上一版临时启动镜像。原厂 Recovery 及其启动链保持原位。

### 原机备份设备树与 67 W 档位

2026-11-23 核对了原机 A 槽 `vendor_boot_a` 和 `dtbo_a` 备份。二者的 SHA-256 分别与用于重建 `.402` 离线设备树的原厂 `vendor_boot` 和 `dtbo` 镜像一致。对照 `16.0.6.103(CN01)` 的设备树，CPA、UFCS、主从 SC8547D 以及虚拟充电泵的充电参数一致。此比较针对设备树属性，排除了 phandle、GPIO、pinctrl 和嵌入的固件数据。

- CPA 将 UFCS 和 SuperVOOC 各列为 67 W 上限。UFCS 配置 `target_vbus_mv=11000`、`curr_max_ma=6100`。原厂 `oplus_chg_ufcs.c` 先查找适配器覆盖 11 V 的 PDO，再用 PDO 电流上限、线缆和设备策略约束请求。SC8547D 的设备树同时启用 UFCS PHY 和 VOOC PHY。
- SuperVOOC 原厂常温曲线在本轮接入电量对应的低电量档请求 6.1 A，其 VBUS 初始化保护寄存器配置与 Ubuntu SuperVOOC 路径相同。Ubuntu 此次同样首帧回复 6.1 A。85 个原厂适配器在线的有效双泵采样中，平均泵侧电压 8.878 V、平均合计电流 6.235 A、平均功率 55.414 W、峰值 57.235 W，电池 27–31°C，净充入 224.002 mAh。日志位于平板 `/var/log/opd2513-normal-high-observe`。
- Ubuntu 当前驱动实现 SuperVOOC 握手。原厂适配器的 UFCS 输出能力及同条件下原厂 Android 实际选中的协议、输入功率仍待取得。如果原厂选择 UFCS，剩余移植涉及 PDO、线缆与协议切换及独立的泵保护。若原厂选择 SuperVOOC，应在同一测量位置比较两边的电压、电流和持续功率。原厂 Recovery 可用于补电，其协议和功率需要独立测量。

`oem_ufcs_probe=1` 新增 5 V 适配器能力查询。驱动在持有 D+/D− 复用器、主泵静止时尝试 UFCS 握手，读取输出能力并记录 11 V 档的广告电流。试验只查询能力，随后恢复 PHY 并检查仍为 5 V，再进入已有 SuperVOOC 路径。该选项默认关闭。ARM64 编译、主机帧解析与原有回放已通过。试验模块 `e2fef990f413ec3e14af768185565db5e59832c32b5ac13a4c92ac21eac4a66a` 已临时加载。板端记录位于 `/var/log/opd2513-ufcs-query-observe`，回退模块为 `/run/opd2513/vooc-normal-high.ko`。适配器带载验收待测。

原厂协议引擎 `ufcs_class` 已从 OnePlusOSS `8be53e8b737a83a33512d5e0106cccb010a5c24c` 的 `vendor/oplus/kernel/charger/v2/ufcs` 移植到当前 Ubuntu 内核。将 `ufcs-class-mainline.patch` 应用于该目录后，可用当前 Ubuntu 内核输出目录执行 `make ARCH=arm64 LLVM=1 M=<ufcs 源码绝对路径> modules`。补丁仅适配外部模块构建和新版高精度定时器接口。生成模块 SHA-256 为 `f96498f9bbf4c1c4061daef0b50819ee7686dbe120ce27c0ecfb7442f1e18777`。已在真机验证注册 `/sys/class/ufcs` 后可卸载。后续带载使用还需 SC8547D 和充电策略接入。

`ufcs-class-lifecycle.patch` 修复 PHY 启动失败的返回路径，以及空闲协议线程在设备注销时的退出条件。将它与构建适配补丁一同应用后，ARM64 模块已构建通过。`tests/ufcs-lifecycle` 使用模拟 PHY，在真机上连续通过 8 次设备注册、启动失败传递和注销测试。采集中的 SuperVOOC 驱动独立于此模块。

### 2026-11-23 原厂 67 W 适配器复测

原始采样、内核日志和校验和保存在 `ram-rehearsal/ufcs-query-20261123/`。插入时电量 48%，会话结束前读到 49%，电池计数器净增 72.048 mAh。UFCS 握手成功，初次查询在 11 V 档返回 4.0 A。原厂 `oplus_chg_ufcs.c` 在 `oplus_ufcs_switch_check_work` 的启动路径先获取设备信息并配置看门狗，对支持的适配器走认证和线缆／功率信息查询，再调用 `oplus_ufcs_get_pdo_info`。原厂 `ufcs_intf.c` 会发送一次新的 `GET_OUTPUT_CAP`。本次 44 W 仅覆盖握手后、认证前公布的能力。

接着识别为 SuperVOOC `0x4b`，首帧回复 6.1 A，两路泵导通。第 8.12–37.50 秒内，30 个样本的泵侧输入平均 55.86 W、峰值 56.37 W，由主泵 VBUS 乘两路泵输入电流之和计算。平均 8.835 V、6.323 A。定时采样前期含爬升，驱动全会话报告的均值为 51.84 W。拔线时 USB 电压降到 112 mV，`online` 和 `BrickID` 暂时仍保留旧值。采集器现将低于 1 V 的读数判为已断开，回放确认剔除该样本后电量净增数值相同。

原厂设备树为 SuperVOOC 和 UFCS 配置的 67 W 是协议功率上限。已核对本机适用的原厂 SuperVOOC 6.1 A 首帧、双泵 ADC 换算及电压反馈分支，现有读数表明这些路径已工作。本轮还缺同一台平板、适配器与线缆在原厂环境且相近电量和温度下的输入读数，67 W 对齐仍待验收。充电驱动继续使用已验证的当前版本。

对照原厂 `oplus_ufcs_switch_check_work()` 增加 `GET_DEVICE_INFO`。采集器先记录适配器厂商 ID，再请求 11 V 档位，供下一轮判断原厂认证分支是否适用。该查询阶段保持 5 V，完成后释放 UFCS PHY 并回到 SuperVOOC。厂商 ID 解析及异常帧主机测试通过。匹配板端内核的 ARM64 模块已热加载，SHA-256 为 `424993d1eb0da6cab30cc19b901216671fd8123ab98b5bacfcfba2b041d954c1`。USB 连接及充电泵待机已验证，原厂适配器带载结果待采。板端服务 `opd2513-ufcs-identity-observe` 记录到 `/var/log/opd2513-ufcs-identity-observe`，回退模块为 `/run/opd2513/vooc-ufcs-query.ko`。

### UFCS 厂商识别和原厂守护进程

`ram-rehearsal/ufcs-identity-20261123/` 保存了两分钟真机采样及校验和。适配器返回厂商 ID `0x22d9`，与原厂 `UFCS_OPLUS_DEV_ID` 一致。认证前 11 V 档公布 4.0 A。随后 SuperVOOC 双泵在 112 个有效稳定样本内平均 56.46 W，峰值 57.42 W。电量从 47% 到 50%，计数器净增 328.798 mAh。

从本地 `.402` OTA 的 `odm` 分区定位到 `/odm/bin/hw/ufcsd`，其 SHA-256 为 `77c4ad34117fc6b04048d0c2848faeb25741df43b2fad0d5031de451ce33fcec`。原厂 init 在开机和充电模式启动此守护进程，内核通过 `/dev/ufcs_dev` 与它通信。使用同一 OTA 的 Android linker 和依赖，主机 ARM64 模拟运行已到达设备节点打开操作。平板 Ubuntu 中以非特权用户验证了原厂动态依赖与进程启动。原厂守护进程只临时存放在平板 `/run`，未随补丁分发。Ubuntu 端缺少 `/dev/ufcs_dev` 兼容接口和联动 UFCS 协商的 SC8547D 驱动，认证后档位及实际功率待验证。

新增 `ufcs_oem_auth.c` 提供原厂守护进程使用的设备接口。模块默认仅 root 可打开，认证数据保存在内核，卸载时清除，状态节点只显示是否已提交。使用 `UFCS_CLASS_SRC` 指定已构建的原厂协议引擎源码目录，同时在 `KBUILD_EXTRA_SYMBOLS` 中加入该引擎和电池模块的 `Module.symvers`，可构建 `ufcs_oem_auth.ko`。本轮 ARM64 模块 SHA-256 为 `bd8bb0fe17daac0a887ef4db105c740aec9a1de1b62815bc3f473381616e1cd9`。板端三次加载、执行原厂守护进程、确认提交、卸载均通过。原厂守护进程以非特权用户运行，测试期间向该用户的组授予设备节点访问权限，卸载后节点消失。下一阶段由 SC8547D UFCS 传输层调用原厂验证函数，并读取认证后的线缆和 PDO 信息。此验证覆盖守护进程到内核的接口，适配器端的认证与 67 W 实测仍待完成。

### UFCS 真机认证诊断

SC8547D 传输层已接入原厂 UFCS 协议引擎。修复硬件握手失败标志与原厂状态枚举同名的问题，并允许接收 3 字节控制帧。诊断期间保持充电泵待机并拦截 UFCS 功率请求，退出时关闭 PHY、同步释放 IRQ，并回读核对寄存器恢复结果。后续 SuperVOOC 会话继续使用原厂电量与温度档位。

ARM64 构建和现有四组主机测试通过。模块已在 OPD2513 的 `7.3.0-opd2513-fwzstd+` 上热加载，SHA-256 为 `d5443a91df8462240d24af11516434512edb810dea518f0e959550ee7aa038ec`。已验证电脑 USB 在 `[SDP]` 状态下控制连接正常、充电泵待机、原厂守护进程提交认证数据。板端单轮采集写入 `/var/log/opd2513-ufcs-auth-observe`。需连接原厂适配器才能测得认证结果、认证后 PDO 以及 SuperVOOC 输入功率。回退模块为板端 `/run/opd2513/vooc-ufcs-identity.ko`。

同一台平板接入原厂适配器后，UFCS 握手、3 秒看门狗配置、适配器认证和退出确认均成功。适配器在 11 V 档公布的最大电流从认证前的 4.0 A 变为认证后的 7.3 A。原厂线缆返回一加 E‑Marker 类型 `0xaa`，数据额定为 20 V / 8 A。`.402` 设备树将平板 UFCS 设置为 11 V / 6.1 A，将 UFCS 和 SuperVOOC 两个协议的最高功率都配为 67 W。原厂认证成功后走 `oplus_ufcs_charge_start` 和 `oplus_ufcs_update_current` 的分阶段带载路径。这轮 Ubuntu 退出 UFCS 后进入 SuperVOOC，稳定双泵输入平均 56.29 W、峰值 57.00 W（插入 8 秒后的 38 个板端样本），45.65 秒电池净增 98.247 mAh。原始证据在 `ram-rehearsal/ufcs-auth-20261123/`。

真实交换暴露出协议退出后仍有发送延时回调运行。`ufcs-class-timer-teardown.patch` 在停止事件源后同步清理线程、定时器和溢出工作。`ufcs-class-exit-status.patch` 处理 Ubuntu 电源设备命名并保留退出、源端硬复位与停用失败状态。应用顺序为 `ufcs-class-mainline.patch`、`ufcs-class-lifecycle.patch`、`ufcs-class-timer-teardown.patch`、`ufcs-class-exit-status.patch`。传输层诊断现记录全部 PDO 及 11 V 的线缆、适配器和设备限流交集，保持 UFCS 电压电流请求锁闭。

最新 ARM64 协议引擎和驱动已通过构建并热加载。板端 `/run/opd2513/ufcs-class-exit-status.ko` 哈希为 `f5202daaca7760535235b0e4edc048c7a90355d0fde13ece3619ccd599113362`，`/run/opd2513/vooc-ufcs-exit-status.ko` 哈希为 `b09d38e60405e25a3ba1870c1249d809459bd9dbd997c1dbcf74c9272af9e53b`。模拟 PHY 的 8 次注册、启动失败传递、注销验证通过。当前 USB 控制和双泵待机正常，新增退出故障分支仍待真实协议测试。下一阶段针对 UFCS 实现低电流启动、双泵带载、SOC/温度曲线和确认回落 5 V 的退出路径。原厂在双泵启动时设定充电泵工作模式并通过 PDO 调节输出。已有 SuperVOOC 软件 VBUS 上限和状态机适用于其现有协议，UFCS 带载需要独立验证。67 W 真机目标仍待验收。

`oem_ufcs_startup_trial=1` 启用原厂带载流程的第一条 PDO 请求。适配器认证、线缆识别及限流检查通过后，驱动允许一笔 5.0–5.5 V、800 mA 的启动请求，充电泵保持待机。传输层在发送前核对实际帧的电流和电压。板端已加载此次 ARM64 构建，模块 SHA-256 为 `0abba7fc895ec2af66b8a6f664102086fb58aea70c21b60eb5d165401869a0f3`。采集服务 `opd2513-ufcs-startup-observe` 已启动，启动 PDO 回执待原厂适配器连接。通过此阶段后才进入充电泵启用和逐级升压。

原厂启动请求已在平板上通过。适配器返回 `POWER_READY`，5.0 V / 800 mA 请求和 UFCS 退出均成功。认证后读到两个 PDO，电压分别覆盖 3.3–5.0 V 和 5.0–11.0 V。两个档位均公布 0.5–7.3 A。适配器上限为 7.3 A，线缆上限为 8.0 A，设备上限为 6.1 A。采样存档位于 `ram-rehearsal/ufcs-startup-20261123/`。

`oem_ufcs_powered_trial=1` 在启动请求成功后保持 UFCS，暂停普通充电支路，按主泵输出电压计算 2:1 启动电压。在核对保护寄存器与泵状态后启用主泵及副泵，再分步提高电流至原厂策略允许的档位。本轮试验限时 30 秒，请求电压最高 9.8 V，保留已验证的 10 V 软件保护和充电泵硬件保护。结束后核对两泵关闭，退出 UFCS，确认输入回到 5 V 或已拔线后恢复普通充电支路。此模块已编译并在电脑 USB 模式热加载，SHA-256 为 `b1501d8bb54ac1652c601285c02c62f3051026fe477d1257a6eb143e704d0ee5`。四组已有主机测试通过，板端采集服务为 `opd2513-ufcs-loaded-observe`。UFCS 双泵切换及功率仍待原厂充电器实测。

上一轮原厂适配器采集保存在 `ram-rehearsal/ufcs-loaded-20261123/`。UFCS 认证与 800 mA 启动请求成功，主泵输入升至 8.047 V，首次开关状态确认返回 `-EAGAIN`，副泵未启动。驱动关闭双泵并成功回退到 SuperVOOC。38.52 秒窗口内电量计净增 56.328 mAh，最后一个双泵样本为 8.951 V × 6.308 A，约 56.46 W。

本轮将 UFCS 启泵确认改为原厂 `UFCS_START_CHECK_DELAY_MS` 的 500 ms，并在等待期间继续检查双泵保护读回、ADC、总电流、温度及在线状态。确认时记录 0x06、0x07、电压和电流。该改动保留试验电压、电流和温度门限。ARM64 构建和四组既有主机测试通过。模块已在电脑 USB 下热加载，SHA-256 为 `7c2e36ca4c4c24af19a47e5acd0fcfc975be92c471760076435a99908e5f2c09`，采集服务为 `opd2513-ufcs-start-timing-observe`。真机 UFCS 双泵带载和 67 W 目标均待验收。

500 ms 版本的真机采样位于 `ram-rehearsal/ufcs-start-timing-20261123/`。UFCS 在两泵启用前因主泵 0x0f=0x06 退出，保护寄存器读回正常，回退到 SuperVOOC 后，45.69 秒窗口内电量计净增 108.727 mAh，观察到的双泵输入峰值为 57.53 W。

原厂在第一笔 5 V / 800 mA PDO 之前配置充电泵和 ADC，再暂停普通充电支路。试验模块现按此顺序执行，并保持已有故障判断。按 `.402` 正常偏低温度区的 SOC 曲线，55% 以上至 75% 的试验限流取 3 A，仍受原有电压和温度门限约束。ARM64 构建及四组主机测试通过，模块 SHA-256 为 `1891e22e4c2384ad0b98369a1fce9c6170638aa534e771988c2b25d1e5e23e67`。已热加载并启动 `opd2513-ufcs-oem-order-observe`，待原厂适配器触发新一轮验证。

按原厂顺序配置的版本已完成 UFCS 双泵启动验证。主泵和副泵的开关确认分别耗时 557 ms 和 501 ms，双泵 36 个样本的平均泵端输入为 44.31 W，包含电流爬升，峰值为 56.54 W。峰值阶段 PDO 为 9.8 V / 6.1 A，泵端约为 8.9 V。该轮随后报码 -34，关闭双泵并回落 SuperVOOC。45.58 秒内电量计净增 72.047 mAh，含 UFCS 和 SuperVOOC 两段。采集保存在 `ram-rehearsal/ufcs-oem-order-20261123/`。

下一版按原厂逻辑在启动窗口之外至少调整 100 mV，修复 20 mV 请求量化后低于启泵窗口却原档重发的情况。双泵到达试验目标档位时读取一次适配器 Source Info，并同时记录泵端 ADC 和 PDO。退出时记录具体阶段。ARM64 构建及四组既有主机测试通过，模块 SHA-256 为 `47632454f1fcd17c9ef4865e59e26038af575665ecc4d3acf930ca22356ff959`。已热加载并启动 `opd2513-ufcs-source-observe`。67 W 仍待真机验收。

58% 电量的真机对照保存在 `ram-rehearsal/ufcs-source-20261123/`。UFCS 请求 9.8 V / 3 A 时，适配器上报 8.88 V / 3.03 A，泵端同时读到 8.441 V / 3.146 A，泵端双泵峰值为 26.91 W。30 秒试验结束后正常回到 SuperVOOC，同一插入电量和温度下的 SuperVOOC 策略也选择 3 A。已在电脑 USB 下重新加载同一模块并关闭两个 UFCS 试验参数，启用 SuperVOOC 双泵，采集服务 `opd2513-supervooc-comparison` 已启动。低电量档位的 67 W 对齐仍缺同一测点的原厂基线。

Ubuntu 的 UPower 历史记录也确认了预计时间跳变。01:47–01:49 的 55% 接入轮次，稳定充电时电池侧读数为 47–48 W，预计还需 31–33 分钟。01:53 的 58% 接入轮次，带载阶段读到 19.37 W，预计还需 74 分钟，协议切换阶段读到 6.76 W，预计还需约 3 小时 28 分钟。后一轮从接入到最后一个有效样本约 36 秒，电量计净增 23.58 mAh。这两轮接入电量和电流档位不同，且后一轮含 UFCS 超时切换，无法据此判定同档稳态回退或实际充满耗时。纯 SuperVOOC 对照采集仍在等待原厂适配器。

对照 `oplus_ufcs_charge_allow_check()` 和 `oplus_ufcs_charge_start()`，原厂使用准入时记录的 SOC 初始化曲线。Ubuntu UFCS 候选版现在为每次会话选定曲线，避免 55% 起充后刚到 56% 就以超出新档位为由退出。实时电量范围、电池电压、健康状态、温度及双泵保护继续检查。ARM64 构建和四组既有主机测试通过。候选模块 SHA-256 为 `7b5e06a2bb3d00c080e34a0e97167d0fa47ac2490fe1b172703aefa9b2ffb174`，尚待真机验证。当前平板使用 SuperVOOC，保留正在运行的采集服务。

核对原厂 SC8547D 的 `CP_GET_VOUT` 回调后，UFCS 启泵目标改用 `0x1b` 的 VBAT ADC。此前误用了 `0x19` 的 VOUT ADC。同一条真机样本的 VOUT 3892 mV 和 VBAT 3933 mV 分别算出 8150 mV 和 8250 mV 的窗口下界。这项修正只影响启泵窗口的计算。ARM64 编译和四组既有测试通过，模块 SHA-256 为 `9af0e600fffeb3e5d99b74b4b4a79640275acb87d4422fd9b1870e81f2cd45d2`，待真机验证。

### ColorOS .402 UFCS 副泵控制

原厂 `oplus,virtual_cp` 每 500 ms 比较实测输入与目标输入对应的泵状态。副泵在 1900 mA 开启，在 1600 mA 关闭。两种判断不一致时，连续三次确认后才允许下一次切换。Ubuntu 候选版实现该滞回，并在关副泵前先将已协商电流降到现有单泵上限 2.2 A，再确认副泵进入待机。启动和切换仍使用既有电池、温度、过压、限流与运行状态检查。

原厂 UFCS 的 11 V 是 PDO 请求目标。此前 9.8 V 请求时，适配器上报 8.88 V，泵端实测 8.441 V，因此请求值与实际输入需分别核对。候选版按原厂测点记录适配器输出、充电泵 VAC、输入电流及其推算的输入阻抗。9.8 V 请求上限和 10 V 实测电压保护仍然有效。已有记录在 6.1 A 高档保持双泵开启，副泵滞回尚无证据能解释该档的功率差。

四组主机测试通过，包括副泵阈值和延迟切换边界。ARM64 模块用 Clang 22.0.1 构建，当前内核由 Clang 21.0.0 构建，模块 `vermagic` 与前版一致。候选模块已传至平板 `/run/opd2513/vooc-ufcs-pump-policy.ko`，SHA-256 `f13dfdea6779d216420dbe7bf7dae3ae00f6bfa45330caa8c1debbc7f91cee53`，电脑 USB 下已临时加载，驱动成功绑定到 `10-006a`。UFCS 带载开关保持关闭，原厂适配器带载验证待完成。纯 SuperVOOC 采集仍由原服务进程运行。

### 恢复 57 W SuperVOOC 实测基线

以 `ram-rehearsal/supervooc-57w-baseline-20261123/` 中保存的源码、原始采样、RAM 镜像清单和原模块为后续 SuperVOOC 修改基线。模块 SHA-256 为 `0d0e90952aeb91bb5a30811650563b4e15aaa8baf973af6a003c14ae3d93d60c`，与真机 57.235 W 峰值轮次对应的模块一致。已于 2026-11-23 02:36 热加载，核对驱动绑定及 `powered_trial=1 dual_trial=1 oem_policy=1 oem_first_3a=1 oem_high_band_first_6a1=1 oem_normal_high=1`，保留所有充电保护。

56% 接入的实测中，纯 SuperVOOC 双泵在第 5 秒后平均 26.13 W，约 44.57 秒净充入 41.919 mAh。UFCS 试验的双泵阶段平均 25.81 W，约 43.56 秒净充入 37.988 mAh，包含切回 SuperVOOC 的过渡。该电量下，两条路径均选择 3 A。57.235 W 峰值轮次从 50% 接入，首帧选择 6.1 A。原始 UFCS 试验数据位于 `ram-rehearsal/ufcs-pump-policy-20261123/`。后续用同等接入电量与温度对比原厂与 Ubuntu 的同测点功率。

本轮复测原始记录位于 `ram-rehearsal/supervooc-57w-retest-20261123/`。接入时为 56%，首帧请求 3.0 A。接入 5 秒后的 85 个双泵采样平均输入 3.107 A、26.200 W，峰值 26.632 W。90.127 秒净充入 99.557 mAh。

复测后核对了运行中模块的 ELF Build ID `dac43d8c6c683736f2abc1883d5bc39cb2658664`、驱动绑定及六项模块参数，均与 57 W 基线一致。已校验的 RAM 临时启动镜像保存于 `/home/jixing/.local/share/opd2513/supervooc-57w-baseline-20261123/boot.img`。

### 取消 55% 接入降档的真机试验

临时加载时启用 `svooc_skip_soc_step=1`。正常偏低温区且电池接插件温度有效时，SuperVOOC 在已有的充电运行范围内将目标电流设为 6.1 A。原厂高档之外的接入保留低电流启动，副泵确认工作后才逐级提高。保留原有高电量退出、温度、电压、电流与双泵保护。此试验只改变 SuperVOOC 策略。

主机四组测试和 ARM64 模块构建通过，编译器版本为 Clang 21.1.8，内核构建版本为 Clang 21.0.0。平板当前已热加载试验模块，Build ID `9de7d576849057a3d7c6bd6c4e6d4af610238b90`，保留 `/run/opd2513/vooc-normal-high.ko` 作为回滚模块。模块、源码和加载记录位于 `ram-rehearsal/supervooc-soc-step-20261123/`。板端采集已启动，带载验证需要接上原厂适配器。


### 56% 接入后的首帧电流对比

取消接入电量降档的首轮试验已经取回：49.755 秒净充入 41.918 mAh，内核记录平均泵输入 25.734 W。目标与末次回复均为 6.1 A，但实测泵电流约 3.1 A。首帧为 3.0 A，随后经历七次电流变更，约第 9 秒回复达到 6.1 A。回复值只证明驱动发出了请求，不证明适配器实际执行。数据位于 `ram-rehearsal/supervooc-soc-step-20261123/`。

此前 57 W 基线首帧为 6.1 A，没有中间变更。下一版恢复基线 `opd2513_first_current_target()` 的调用，取消由旧电量档位额外限制首帧的条件。保留所有已有启动准入、双泵、温度、电压、电流、时限和高电量退出检查。此差异是否解释输入电流停在 3 A，尚待带载验证。

四组主机测试和 ARM64 模块构建通过。试验模块 SHA-256 为 `baade2ffa1d8328205b1e341a0a323e139319b37d674e16451f450c5ee7386d8`，Build ID 为 `e22df351eaf922b4520325e9bed7a275fc802a93`。平板已临时加载并核对绑定与开关；采集服务为 `opd2513-svooc-first61-observe`。模块、源码和加载证据位于 `ram-rehearsal/supervooc-soc-first61-20261123/`。未写启动分区；原厂 Recovery 配置保持原状。本版带载结果待取回。


### 首帧 6.1 A 后掉速的证据

首帧修正版在 56% 接入时，7.136..13.216 秒的七个采样实测平均泵输入 56.788 W、峰值 57.031 W，平均总 IBUS 6.333 A。原始数据与分析位于 `ram-rehearsal/supervooc-soc-first61-20261123/`。第 14.228 秒 USB 分类变成 Unknown，同时目标/回复降至 3 A；内核随后报告按新低档检查的总 IBUS 超限并退出。采集的 charge_counter 尚未增加，因此本轮短测不证明持续净充入。

用户明确说明掉速时没有换线，原厂线缆正常。Unknown 是系统分类异常的观测，未将其解释为用户换线或线材故障。降档发生时电池与接插件温度的精确输入尚缺，故新增 `SuperVOOC policy derate` 日志记录本次决策使用的实时输入，并在采集器补录三路充电热敏数据。仅补诊断；策略和保护阈值沿用首帧修正版。

诊断模块 SHA-256 `ad5b09c52fe76665a236f8909a000e0f0dc87a17b261f95e7b51cc97d6b4cbc2`，Build ID `bd5fb38df39f72edc8608417da69f93bbde7b501`，已临时加载并核对绑定。采集进程 `opd2513-svooc-derate-diag-observe` 等待本轮接入，采样窗口 120 秒；模块与加载证据位于 `ram-rehearsal/supervooc-derate-diagnostic-20261123/`。持续运行及掉速原因仍待验。


### 接插件 44 C 阈值混用修正

诊断日志在掉速当刻明确记录：`target=3000 mA plug_SOC=56 live_SOC=56 battery_temp=270 dC connector=44236 mC battery_voltage=4254000 uV`。本项目新增的 `opd2513_supervooc_session_ask()` 将电池 warm 分界 44 C 额外用于接插件，导致低电芯温度下接插件略过 44 C 便从 6.1 A 突降至 3 A，继而触发低档总电流保护并退出。该根因来自实测日志，见 `ram-rehearsal/supervooc-derate-diagnostic-20261123/derate-evidence.log`。

原厂 `oplus_ap_voocphy.c:6145` 的 BTB/USB 温度检测与电池温区分别处理，该函数的接插件过温判断为 80 C。修正未采用该较高阈值，而是让新增 SOC 覆盖逻辑与本项目原有三路传感器 55 C 退出检查共用 `OPD_CONNECTOR_GUARD_MC`。电池 normal-high 限制、接插件无效读数、55 C 退出、电压、电流、双泵及高电量退出均保留。用户要求的正常温区 6.1 A 不再因接插件 44.236 C 错降至 3 A。

四组主机测试和 ARM64 构建通过；补充真实故障输入与 54.999/55 C 边界断言。模块 SHA-256 `a5d65b3215bf62591caea5f2442dcb0f3ad0258d4984f6478434b67474027377`，Build ID `00a26bd05187a5b1157e5b6decf6440c776d88d5`，已在电脑 USB 下热加载，绑定成功。源码、模块和加载日志位于 `ram-rehearsal/supervooc-connector-fix-20261123/`。采集进程 `opd2513-svooc-connector-fix-observe` 已启动；带载持续功率与净充入量尚待验证。


## 已验证版本的持久化（2026-11-23）

**本次交付范围：仅上传 GitHub 与更新说明。持久化脚本和 RAM 镜像已在主机准备，尚未在平板安装，也未执行重启验证。当前真机已通过的是热加载修复版的带载测试。**

用户确认本轮充电恢复正常。采集窗口约 99.503 秒，电量 56% → 58%，电池计数器净增加 263.301 mAh；98 个双泵活动样本的泵端功率平均 55.621 W，峰值 58.287 W。窗口结束前仍为 `target=61 ask=61 replied=61 first=61`，未出现此前约 14 秒的错误降档。接插件三路最高 52.897°C。数据口径是泵端 VBUS × 两路 IBUS，实际电流与协议请求分别记录，未宣称恒定 67 W。详见 [validation-20261123.json](validation-20261123.json)。

本版本模块 SHA-256：`a5d65b3215bf62591caea5f2442dcb0f3ad0258d4984f6478434b67474027377`；Build ID：`00a26bd05187a5b1157e5b6decf6440c776d88d5`。要求内核 `7.3.0-opd2513-fwzstd+`、匹配的电池模块及已启用的双泵/ADC DT。源码默认保留原分档；已验证运行配置显式启用以下参数：

```text
powered_trial=1 dual_trial=1 oem_policy=1 oem_first_3a=1
oem_high_band_first_6a1=1 oem_normal_high=1 svooc_skip_soc_step=1
```

**Ubuntu 根文件系统持久化。** 将本目录及上述精确模块复制到平板，连接普通电脑 USB 后，在平板执行：

```bash
sudo python3 install-persistent.py --module /path/to/opd2513_vooc.ko
systemctl is-enabled opd2513-supervooc.service
systemctl status opd2513-supervooc.service
journalctl -u opd2513-supervooc.service -b
```

安装器核对型号、内核和模块哈希；写入 `/usr/local/lib/opd2513-supervooc/` 与 `/etc/systemd/system/opd2513-supervooc.service`，备份已有文件至 `/var/lib/opd2513-supervooc/backups/`，启用并立即验证。下次 Ubuntu 启动自动核对 Build ID、七项参数与实际绑定；匹配时直接保留当前驱动。替换旧驱动仅在普通 USB 低压或拔出适配器后执行；其余状态等待重试。该服务保存在内置 UFS 根文件系统，区别于 `/run` 的临时模块；开机自动加载不等于已经验收独立冷启动。

**启动镜像固化。** 在主机工程根执行以下命令，输出路径需至少约 300 MB 空间：

```bash
python3 driver-fixes-20261119/supervooc-opd2513/pack-camera-charge.py \
  --battery-module ram-rehearsal/oem-policy-20261122/qcom_battmgr.ko \
  --vooc-module ram-rehearsal/supervooc-connector-fix-20261123/opd2513_vooc.ko \
  --adc-module-dir ram-rehearsal/camera-charge-20261122 \
  --oem-normal-high --svooc-skip-soc-step \
  --output-dir /home/jixing/.local/share/opd2513/supervooc-connector-fix-20261123
```

本轮产物 `boot.img` SHA-256：`01b7ebbabdd02ff54ace9c1ae17270bb7827352f462387a8c4b1cfabc29c14c4`。initrd 已内置上述模块及七项参数，保留相机、闪光灯、音频总线和原厂 Recovery 启动依赖；启动方式仍为 `fastboot -s "$FASTBOOT_SERIAL" boot /path/to/boot.img`。镜像仅为本机现有 UFS 根文件系统配套 RAM 引导产物，未作为通用固件上传 Git；其本机控制配置需单独审查后才适合分发。

**回退。** 先执行 `sudo systemctl disable --now opd2513-supervooc.service`，再使用保留的旧 RAM 镜像启动；停用服务本身不会强行卸载正在工作的模块。若要恢复旧持久化文件，按 `installation.json` 将备份中原有文件放回记录路径，对 `previously_existed=false` 的文件删除本轮新增项，再运行 `sudo systemctl daemon-reload`。原厂 Recovery 与 A 槽启动组件在本轮均未写入。

**验证边界。** 本轮短测及用户反馈已确认；持久化服务当前启动的安装核验与重启后的自动加载分别记录，不将前者当成后者。普通 USB 持续净充电、标准 PD、完整充电周期、30 分钟会话之后行为、脱离电脑冷启动及再次进入原厂 Recovery 仍需独立验收。继续保留温度、电压、80% 高 SOC 退出及 30 分钟会话上限。
