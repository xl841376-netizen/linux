# Xiaomi Mi 9 Pro 5G（crux）· 主线 Linux 适配状态

> 本页**只记录当前设备上实际可用**的部分，逐项都有实机证据。
> 采集时间：**2026-10-11 08:30**（设备连续运行中）
> 仓库定位：本仓库为 `torvalds/linux` 的 fork；移植代码在分支 `crux-sm8150-73`。

---

## 0. 设备与基线（实测）

| 项 | 实测值 |
|---|---|
| 机型 | `Xiaomi Mi 9 Pro 5G (crux)`（/sys/firmware/devicetree/base/model） |
| 内核 | `7.3.0-rc5-00037-g551c722f4080-dirty #103 SMP PREEMPT`（2026-10-09 构建） |
| 根文件系统 | `Ubuntu 26.04.1 LTS`（aarch64，KDE 桌面） |
| 引导 | UEFI + ESP：`BOOT_IMAGE=(hd5,gpt35)/EFI/crux/Image73`（SimpleInit）；内核 cmdline 含 `clk_ignore_unused pd_ignore_unused deferred_probe_timeout=-1` |
| 根分区 | `root=UUID=ed36cd26-…` = `/dev/sda37`（`linux-root26`，24G） |
| 稳定性 | 本次开机 **1 小时 25 分未重启**，load average 0.09（四颗 DSP + WiFi 常驻下） |

---

## 1. 当前可用（逐项实机核对）

| 子系统 | 实测状态 | 证据 |
|---|---|---|
| **显示** | 可用 | DRM `card0`，`card0-DSI-1 = connected`；显示控制器 `ae01000.display-controller` 已注册；桌面正常出图 |
| **GPU** | 可用 | `2c00000.gpu` devfreq 已注册、`renderD128` 存在（运行中每约 60 秒会打印一条 `gpu fault … source=CCU type=TRANSLATION` 告警，不影响出图） |
| **触摸** | 可用 | `fts` 触摸已注册为输入设备（i2c-0 @0x49），日志 `[FTS] SET Input Device Property` |
| **物理按键** | 可用 | `pm8941_pwrkey`、`pm8941_resin`、`gpio-keys` 三个输入设备已注册 |
| **存储（UFS）** | 可用 | 控制器 `1d84000.ufshc`；`/dev/sda` 475.6G 全部 37 个分区可读；`sda36`=`linux-root`(64G)、`sda37`=`linux-root26`(24G，当前根)、`sda35`=`efi`(512M) |
| **读取 Android 分区** | 可用 | `sda32`=`vendor` 挂 `/vendor`，`sda33`=`system` 挂 `/mnt/sys`（只读参考，不回写） |
| **WiFi** | 可用 | `ath10k_snoc` 已加载；`wlan0 UP 192.168.1.157/24`，关联 `CMCC-GuFS`（NM 连接 `cmcc24`），另有两个全局 IPv6 地址 |
| **远程访问** | 可用 | 经 WiFi 以 `crux.local`（mDNS）SSH 直连；USB ECM 通道 UDC `a600000.usb` 与 `usb0`（192.168.2.2/24）已就位 |
| **四颗 DSP（remoteproc）** | 可用 | `slpi / modem / cdsp / adsp` 全部 `running`；固件自 `qcom/sm8150/xiaomi/crux/*.mbn` 加载（如 `slpi.mbn` 8.06MB 已起） |
| **Modem 用户态服务** | 可用 | `rmtfs.service`、`tqftpserv.service`、`ModemManager.service` 常驻 active |
| **NPU** | 可用 | 驱动 `msm_npu` + `npucc_sm8150` 已加载；字符设备 `/dev/msm_npu`（505:0）存在；`npu-svc.service` 运行中、接口 `/run/npu.sock`；已实测跑通 InceptionV3（109 层 / 10.31 ms / 92.5%） |
| **视频编解码** | 可用 | `qcom_iris` 已加载，`/dev/video0`、`/dev/video1` 存在 |
| **温控** | 可用 | 26 个 thermal zone 已注册（cpu0-7 / cluster / gpu / modem / video / wlan / compute / camera / mem / aoss 等） |
| **电源 & 关机链** | 可用 | `pm8150b` charger/fuel-gauge、PD 通知链；电源键与关机判定链（`shutdown-clean` → initramfs → `prev-shutdown-verdict`） |
| **USB 供电** | 可用 | `qcom-battmgr-usb`（USB）、`qcom-battmgr-wls`（无线充）、`qcom-battmgr-bat` 三个 power_supply 已注册 |

---

## 2. 连机即可用的入口

```bash
# 设备一律用 mDNS 名访问（不要写死 IP）
ssh root@crux.local

# NPU 服务接口（设备侧本地 unix socket）
/run/npu.sock
```