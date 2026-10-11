# Xiaomi Mi 9 Pro 5G（crux）· 主线 Linux 适配进度与状态

本页是本项目**当前实际适配进度与状态**的说明与声明（持续更新）。
最后更新：**2026-10-11**（基线：Linux 主线 **7.3**）

> 本仓库 = `torvalds/linux` 的 fork。**移植代码**在本仓库分支 **`crux-sm8150-73`**（Linux 7.3 基线，相对主线的差异共 171 个文件：108 新增 + 63 修改）；本页是**适配进度与状态说明**。


---

## 0. 硬件与基线

| 项 | 内容 |
|---|---|
| 机型 | Xiaomi Mi 9 Pro 5G / 代号 **crux** / 小米内部 SM8150 |
| SoC | Qualcomm **SM8150**（骁龙 855+），Hexagon 690(v65) + 私有 NPU 块 |
| 蜂窝 | **双 modem**：内置 X24（remoteproc modem） + **外挂 SDX50M 5G 模组**（PCIe1 + MHI） |
| 无线 | WCN3990（`ath10k_snoc`，节点 `18800000.wifi`）；蓝牙同芯片 |
| 内核 | **Linux 主线 7.3**（`VERSION=7 PATCHLEVEL=3`）；6.17 为上一代可用基线 |
| 引导 | UEFI(EDK2/Mu-Silicium) + **ESP** + SimpleInit（grub.cfg 菜单） |
| 根文件系统 | Ubuntu（KDE Plasma），自建分区 `linux-root`，不依赖 Android 框架 |
| Android 分区 | **只读参考**：固件/NV 从原厂分区投影到 Linux（绝不回写） |

---

## 1. 已经可用（实机验证）

| 子系统 | 状态 | 说明 |
|---|---|---|
| 引导链 / 关机链 | ✅ | ESP + SimpleInit；关机判定走 `shutdown-clean` → initramfs → `prev-shutdown-verdict` |
| 显示 | ✅ | DSI 面板 **ea8076**（FHD cmd mode），`drm/msm` DPU + DSI + PHY |
| 触摸 | ✅ | Focus(FTS) 触摸，含周期自恢复（touch-watchdog） |
| 存储 | ✅ | UFS（`ufs-qcom` 参数已对齐） |
| USB | ✅ | HS 模式：ECM/ACM gadget、DRD；HS PHY 参数微调已并入 DTS |
| 充电 / PMIC | ✅ | `pm8150b` charger/fg、PD 通知链（曾导致卡桌面，已收敛） |
| 电源键 / 关机 | ✅ | `pm8941-pwrkey` + `crux-shutdown` 链路、initramfs 清场 |
| WiFi | ✅ | WCN3990 加载成功、可关联热点（含 IPv4/IPv6，ULA `fd00:cafe::2`） |
| Remoteproc | ✅ | slpi / modem / cdsp / adsp 均可 running |
| 视频编解码 | ✅（基础） | `qcom,iris` VPU 驱动已启用 |
| **NPU** | ✅ **重点成果** | `msm_npu` + `npucc` + 私有用户态栈跑通：InceptionV3 109 层 / 10.31 ms / 92.5% |

---

## 2. 进行中 / 部分可用

| 项 | 现状 | 卡点 |
|---|---|---|
| **外挂 X50（SDX50M 5G）** | PCIe1 链路 + MHI 通道 + sahara 加载 + 部分 QMI 服务已起来 | ① 写 `online` **大概率崩**（偶有存活，无稳定判别变量）；② 工作模式长期 `shutting-down`；③ EFS **无常驻应答**；④ TFTP **零 RRQ**（模组从不来取 modem_pr/MCFG） |
| **内置 modem（X24）** | 早期 40s watchdog 崩已修（rmtfs client-id、MCFG 树供给） | 现崩在 `mcfg_utils.c:399 MCFG activation`，疑与 DIP/NV 平台数据缺失相关 |
| 传感器（SLPI/SSC） | SLPI 已 running | `sensors_registry` 未就绪，传感器长期 UNKNOWN |
| 音频 | DSP 侧 q6afe/q6asm 通路已建立 | 外放功放 **tas2557** 无主线驱动，需移植 |
| 蓝牙 | 未逐项验证 | 需确认 WCN3990 BT 固件加载与 `hci` 通路 |

---

## 3. 尚未开始 / 需移植驱动

- **相机**：整条链路未适配（`camss`/`cci` + sensor 驱动 + CSI 时钟），当前 DTS 中相机节点已移除
- 马达 **aw8697**、指纹（FPC/Goodix）、NFC **nq-nci**、充电泵 **smb1390** 等：主线无驱动，需移植
- 无线充电、HDR/自适应刷新等增强项

---

## 4. 哪些是「内核侧」能处理的

以下是**在内核（驱动/DTS）里可以推进**的部分：

**显示 / 输入 / 音频**
- 背光与亮度曲线、面板 HDR/刷新率、DPU 色彩
- FTS 触摸完善、按键/马达驱动移植
- DSP 音频通路（`apr` / `q6*`）+ 外放功放与 codec 驱动、耳机检测

**相机**
- `camss` / `cci` / ISP 链路与 CSI2 时钟，sensor 驱动（前/后/景深）移植

**传感器**
- SSC/SLPI 传感器驱动与 registry（加速计/陀螺/光距/指南针）

**电源 / 充电**
- PD 策略与温控、`smb1390` 充电泵、无线充、快充协议

**存储 / 总线**
- UFS 参数调优、PCIe 链路稳定性、IPA

**蜂窝（内核侧）**
- 外挂 X50：PCIe 链路训练与重训、MHI 通道、**IP_HW0 下行环深度**、sahara 加载序列、GPIO/电源时序、SSR 恢复
- 内置 modem：PAS/remoteproc 时序、glink/QRTR 通路

**NPU**
- `msm_npu` 驱动与 `npucc` 时钟、与用户态模型容器格式的对接

**蓝牙**
- WCN3990 BT 固件加载与 `hci` 通路

---

## 5. 哪些**不归内核**（用户态 / 固件 / 外部数据）

| 项 | 为什么不在内核 |
|---|---|
| X50 的 EFS 常驻应答 | 用户态守护（rmtfs/EFS 服务）职责；参照同类项目需 `--persist` 常驻，否则模组约 2 分钟后自行优雅关机 |
| TFTP-on-QRTR 供给 modem_pr / MCFG | `tqftpserv`（需“多 qrtr 版本发布”补丁）；外挂模组的 MCFG 树不在默认搜索路径 |
| QMI / provisioning / PDC / 卡会话 | ModemManager + `libqmi` + 上层脚本（面板）职责 |
| MCFG 激活依赖的 **DIP / NV 平台数据** | 来自原厂分区数据，内核不能凭空造 |
| 各类**固件本体** | `modem.mbn`、`adsp/cdsp.mbn`、`sdx50m/*.mbn`、`wlanmdsp.mbn`、`npu.mbn` 等均由原厂提供 |
| NPU 模型容器的排布/编码 | 私有用户态栈内部格式（非公开规范） |
| persist 校准数据、WiFi MAC/board 校准 | NV / OTP 数据，内核只读取 |

---

## 6. 构建与部署（简）

```bash
# 构建（out-of-tree build 目录）
make O=build73 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image.gz
make O=build73 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- qcom/sm8150-xiaomi-crux.dtb

# 模块（如需）
make O=build73 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
make O=build73 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_install INSTALL_MOD_PATH=<rootfs>
```

- 部署：`Image.gz` 与 `sm8150-xiaomi-crux.dtb` 写入 **ESP**（由 SimpleInit 读取），模块写入 rootfs 的 `/usr/lib/modules/<kver>/`
- 设备一律用 **`crux.local`（mDNS）** 访问，不写死 IP；磁盘盘符（`sdX`）每次会变，不要写死

---

## 7. 已知坑（写给后来者）

1. 往 **usrmerge** 系统解包必须带 `--keep-directory-symlink`，否则 `/lib` 符号链接会被替换成真目录，动态链接器失效、全系统命令起不来
2. 容器根文件系统是 f2fs（`nobarrier`）：写文件后必须 `fsync`，否则可能整文件变 NUL
3. 上游自带 `tools/verification/.../test_bak_kunit` 等含 `bak` 字样的**是源码**，不要当备份删
4. Android 分区只读；EFS（`m9kefs1/2/3`）**永远只读**，持久化只走 `/var/lib/mhi-modem/efs` 普通文件
5. 外挂模组的固件真源是 **modem 分区**；`/lib/firmware` 与模拟分区只是投影，改动只发生在投影层
