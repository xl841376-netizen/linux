# Xiaomi Mi 9 Pro 5G（crux）· 主线 Linux 适配状态

> 本页**只记录当前设备上实际可用**的部分，逐项都有实机证据。
> 采集时间：**2026-10-20 09:30**（设备连续运行中）
> 仓库定位：本仓库为 `torvalds/linux` 的 fork；移植代码在分支 `crux-sm8150-73`。
> 相关仓库：[`crux-grub`](https://github.com/xl841376-netizen/crux-grub)（GRUB 引导层：aarch64-efi standalone + cruxdisplay 显示交接模块）｜UEFI 侧 fork [`Mu-Silicium` 分支 `crux-port-next`](https://github.com/xl841376-netizen/Mu-Silicium/tree/crux-port-next)（Mu/Silicium 固件，基于云端最新 main + 仅本机改动）｜[`crux-minimal-rootfs`](https://github.com/xl841376-netizen/crux-minimal-rootfs)（最小 rootfs）。

---

## 0. 设备与基线（实测）

| 项 | 实测值 |
|---|---|
| 机型 | `Xiaomi Mi 9 Pro 5G (crux)`（/proc/device-tree/model） |
| 内核 | `7.3.0-rc5-crux73`（本地版本串；对应分支 `crux-sm8150-73` @ `6178fbc`） |
| 根文件系统 | Ubuntu（aarch64，KDE/GNOME 桌面），`/dev/sda37`（`linux-root26`，23.5G，已用 18.3G / 余 3.9G） |
| 引导 | UEFI + ESP（`/dev/sda35`，510.8M，已用 114.9M）+ **GRUB**：`EFI/BOOT/BOOTAA64.efi`，菜单 `EFI/crux/grub.cfg`。默认条目先跑 `cruxdisplay.mod` 的 `crux_display_init` 完成显示交接，再 `linux/initrd/devicetree` 交给内核：`Image74` + `sm8150-xiaomi-crux-73.dtb` + `initramfs.cpio.gz`；另有 6.17 回退条目（`Image` + `sm8150-xiaomi-crux.dtb`）与 Windows(WoA) 链式引导条目 |
| 稳定性 | 本次开机持续运行、无重启；四颗 DSP + WiFi + 音频常驻 |

---

## 1. 当前可用（逐项实机核对）

| 子系统 | 实测状态 | 证据 |
|---|---|---|
| **显示** | 可用 | DRM `card0`，`card0-DSI-1 = connected`；`ae01000.display-controller` 已注册；桌面正常出图 |
| **GPU** | 可用 | `2c00000.gpu` devfreq、`renderD128` 存在（每约 60 秒一条 `gpu fault … source=CCU type=TRANSLATION`，不影响出图） |
| **触摸** | 可用 | `fts` 触摸（i2c-0 @0x49）；固件 `ftm5_fw.ftb` |
| **物理按键** | 可用 | `pm8941_pwrkey`、`pm8941_resin`、`gpio-keys` |
| **存储（UFS）** | 可用 | `1d84000.ufshc`；`/dev/sda` 全部 37 个分区可读；`sda35`=efi、`sda36`=linux-root(64G)、`sda37`=linux-root26(24G，当前根) |
| **读取 Android 分区** | 可用 | `sda32`=vendor 挂 `/vendor`、`sda33`=system 挂 `/mnt/sys`（只读参考，不回写） |
| **WiFi** | 可用 | `ath10k_snoc`（WCN3990，节点 `18800000.wifi`）；`wlan0 = 192.168.1.240/24`（NM 连接 `cmcc24`，**静态地址**），有外网。固件偶发自复位产生日志噪声（见 §4），不影响使用 |
| **远程访问** | 可用 | WiFi 直连 `192.168.1.240`；USB ECM 备用 `192.168.2.2`（UDC `a600000.usb`） |
| **音频（扬声器 / 听筒）** | **可用（双设备）** | 声卡 `0 [sm8150]`，ALSA 卡 id = **`sm8150`**；PipeWire/ACP 经 UCM 出 `HiFi (Speaker)` / `HiFi (Earpiece)` 两个 profile，桌面可选、可切。`hw_params = S16_LE / 2ch / 48000`。Speaker：`QUAT_MI2S_RX Audio Mixer MultiMedia1 = on` → CS35L41 → 外放；Earpiece：`SLIMBUS_0_RX … = on` → RX INT0 → EAR。**均已实听确认出声** |
| **四颗 DSP（remoteproc）** | 可用 | `remoteproc0..3` 全部 `running`（slpi / modem / cdsp / adsp），固件自 `qcom/sm8150/xiaomi/crux/*.mbn` 加载 |
| **内置 Modem 用户态服务** | 可用 | `rmtfs`、`tqftpserv`、`ModemManager`、`modem-mnt` 均 active；`modem` 为按需启动（`auto_boot=false`，由 `modem-mnt`/开机链拉起） |
| **NPU** | 可用 | `msm_npu` + `npucc_sm8150`；`/dev/msm_npu` 存在；`npu-svc` active、`/run/npu.sock`；已跑通 InceptionV3（109 层 / 10.31 ms / 92.5%） |
| **视频编解码** | 可用 | `qcom_iris` 已加载，`/dev/video0`、`/dev/video1` 存在，`iris-vpu` active |
| **温控** | 可用 | 26 个 thermal zone（cpu0-7 / cluster / gpu / modem / video / wlan / compute / camera / mem / aoss 等） |
| **电源 & 关机链** | 可用 | `pm8150b` charger/fuel-gauge、PD 通知链；`shutdown-clean` → initramfs → `prev-shutdown-verdict` |
| **USB 供电** | 可用 | `qcom-battmgr-usb` / `-wls` / `-bat` 三个 power_supply |

---

## 2. 音频通路（2026-10-20 打通，三个必要条件）

设备树里 `&sound { model = "Xiaomi Mi9 Pro5G sm8150"; }`，**末尾那个词决定 ALSA 卡 id**：

```c
/* sound/core/init.c: retrieve_id_from_card_name() 取 model 最后一个空格后的词 */
model = "Xiaomi Mi9 Pro"        -> 卡 id = "Pro"    // UCM 找不到，ACP 建不出 sink
model = "Xiaomi Mi9 Pro5G sm8150" -> 卡 id = "sm8150" // ACP 按驱动名命中 conf.d/sm8150/
```

（`ucm.conf` 里 `Define.V2Name ""` 关闭了按卡名查找，只按驱动名查 `conf.d/`，所以卡 id 必须等于目录名。）

出声需要同时满足三条，缺任何一条都是**静默失败**（FE 显示 RUNNING、dmesg 无报错、但听不到）：

| # | 条件 | 缺了会怎样 |
|---|---|---|
| 1 | ALSA 卡 id = `sm8150`，ACP 能加载 UCM | 只有 `pro-audio`，桌面无 Speaker/Earpiece |
| 2 | UCM 用 **Syntax 3**、播放路由写在 `SectionVerb` 的 `EnableSequence` | 逐 PCM 探测行为异常 / `_ucmXXXX.hw:` 打不开 |
| 3 | PipeWire 侧强制 **S16_LE / 2ch / 48k** | 以 `S24_LE / 8ch` 打开 → 全程无声且不报错 |

配套文件（当前在设备上，尚未进 rootfs 仓库）：

- `/usr/share/alsa/ucm2/conf.d/sm8150/sm8150.conf`、`HiFi.conf`（Syntax 3；Speaker 优先级 200，Earpiece 优先级 100）
- `/etc/wireplumber/wireplumber.conf.d/51-crux-fmt.conf`（`node.name = ~alsa_output.*` → `S16LE/48k/2ch`）
- `/etc/wireplumber/wireplumber.conf.d/52-crux-noproaudio.conf`（`api.acp.disable-pro-audio = true`，否则默认选到 pro-audio）
- `/usr/local/bin/crux-audio-route.sh` + `crux-audio-route.service`（开机把 `QUAT_MI2S_RX Audio Mixer MultiMedia1` 打开，做保险）

排查口诀：**先看 dmesg 有没有 `no backend DAIs enabled for MultiMedia1`（BE 没开），再看 `hw_params` 是不是 `S24_LE/8ch`。**

---

## 3. 连机即可用的入口

```bash
ssh root@192.168.1.240        # WiFi（静态地址）
ssh root@192.168.2.2          # USB ECM 备用
/run/npu.sock                 # NPU 服务接口（设备本地 unix socket）
```

---

## 4. 内嵌固件（`CONFIG_EXTRA_FIRMWARE`，共 8 个 / ≈14 MB）

设备上的固件**有两份、内容同一套**。内嵌的唯一目的是：① 加载更快；② 根文件系统挂载之前也能提前拿到固件。

| 位置 | 作用 | 说明 |
|---|---|---|
| 内核内嵌 | 编译期打进内核镜像（`CONFIG_EXTRA_FIRMWARE`） | 只放**驱动 probe 路径同步请求、且请求早于根挂载**的固件 |
| rootfs `/lib/firmware/` | 运行时常规 firmware_class 加载 | 其余全部固件在这里；路径必须与请求字符串逐字一致 |

源码树里对应目录是 `fw_extra/`（`CONFIG_EXTRA_FIRMWARE_DIR="fw_extra"`）。该目录里只有下面 8 个进入内核：

| # | 路径（`fw_extra/` 下） | 大小 | 供谁用 |
|---|---|---|---|
| 1 | `qcom/sm8150/xiaomi/crux/adsp.mbn` | 14 663 828 B | ADSP remoteproc |
| 2 | `qcom/sdx50m/sbl1.mbn` | 328 324 B | 外挂 X50 基带 boot 链（MHI/PCIe 侧） |
| 3 | `ftm5_fw.ftb` | 101 720 B | `fts` 触摸屏固件 |
| 4 | `qcom/a640_gmu.bin` | 37 680 B | Adreno 640 GMU |
| 5 | `qcom/a630_sqe.fw` | 32 304 B | Adreno 命令队列固件 |
| 6 | `qcom/sm8150/a640_zap.mbn` | 14 256 B | Adreno zap（安全处理器） |
| 7 | `qcom/sm8150/xiaomi/crux/a640_zap.mbn` | 14 256 B | 同上，机型定制路径副本 |
| 8 | `regulatory.db` | 6 380 B | 无线管制域数据库 |

> 不想要内嵌可自行关闭：`scripts/config --disable EXTRA_FIRMWARE`，届时改由 rootfs 提供。

### 可用性声明

- 上述固件取自**一台 crux 实机**，用于本项目构建与自用，**未在其它 crux / 同型号设备上逐一验证**。
- **请自行确定其在你的设备上是否可用。** 若不可用（DSP 起不来、触摸不通、WiFi 无管制域数据等），请从对应设备的官方刷机包中提取同名、同路径固件替换后重新构建。
- 提取时注意：① 路径必须与请求字符串**逐字一致**；② 机型定制路径（`xiaomi/crux/`）与通用路径（`qcom/sm8150/`）可能是同一内容的两个副本，两处都要有效。
- MAC 地址、RF 校准、传感器标定、EFS/persist/NV 等逐台唯一数据本就在**分区**里，不在这些固件文件中。

> 云端 CI（`.github/workflows/crux73-build.yml`，分支 `crux-sm8150-73`）已把这 8 个一并入库，构建前自检：缺任一文件直接失败。

---

## 5. 已知噪声（不影响连接与使用）

| 现象 | 出处 | 说明 |
|---|---|---|
| **WiFi 固件偶发自复位**：`firmware crashed! (guid …)`，数十秒后 `device successfully recovered`；伴随少量 `failed to transmit packet, dropping: -108`、`failed to submit/push frame`、`chan info: invalid frequency 0 (idx 41 out of bounds)` | `ath10k`（`snoc.c` / `mac.c` / `wmi.c`） | **不影响正常连接与使用**。已把这 5 条由 `ath10k_err/warn/info` 降级为 `ath10k_dbg`；需要看回日志时在 `ath10k_core.debug_mask` 加 `0x10` / `0x800` / `0x100000` |
| **GPU fault 告警**：每约 60 秒一条 `gpu fault … source=CCU type=TRANSLATION` | `drm/msm` | 不影响出图，属调试噪声 |
| **内置 Modem 自愈**：`crash detected … fatal error` → `recovering`（按 MCFG 激活有意重启一次） | `remoteproc` / `mcfg_utils` | 不影响联网，起来后 `running` |

> 以上均为**装饰性/无害**问题，标注为“不影响正常连接与使用”；**要修请自行处理**。

---

## 6. 未完成 / 已知限制

| 项 | 状态 | 说明 |
|---|---|---|
| **外挂 X50 基带注册** | 未完成 | MHI / IPA 通道（`mhi_sahara`、`ipa_mhi_proxy`、`ipa_data-v4.1`）已进 `crux-sm8150-73`；QMI 侧仍卡在 SIM provisioning session（`0x38`）与注册后拖死阶段，**未通过** |
| **Headphones / 耳机口** | 未验证 | UCM 里已移除 Headphones profile（`RX INT1/2 → HPHL/HPHR` 未见实测出声） |
| **摄像头** | 未支持 | 无可用通路 |
| **sda36（64G linux-root）** | 闲置 | 计划删除并入 sda37，未执行 |
