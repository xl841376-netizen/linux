# crux 固件放置约定（唯一规则，2026-09-21 定）

## 一、唯一真源在设备侧 /lib/firmware
所有固件的正式存放地就是设备的 /lib/firmware，路径必须与内核或用户态请求的字符串
逐字一致，不另起名字、不做副本。请求名从哪来：
- 内核侧：驱动的 request_firmware() 参数，例如 drm 的 "qcom/a630_sqe.fw"，
  mhi_pci_generic 的 "qcom/sdx50m/sbl1.mbn"（见 drivers/bus/mhi/host/pci_generic.c:423）。
- 用户态：/usr/local/bin/sahara_svc.py 的 FW 常量与 IMAGE_MAP。

## 二、只有"必须早于根挂载"的才多存一份进内核
判定条件（两条同时满足才内嵌）：
1. 该固件由内建驱动（=y）在其 probe 路径里同步请求；
2. 请求时刻早于根分区挂载。
满足就多存一份到构建树的 fw_extra/，并把相对路径写进 .config 的
CONFIG_EXTRA_FIRMWARE。fw_extra 的目录结构必须与 /lib/firmware 同构。

## 三、当前内嵌清单（6 条，fw_extra 共约 540K）
| 请求名 | 谁请求 | 大概时刻 |
|---|---|---|
| qcom/a630_sqe.fw | drm/adreno（GPU） | 1.397 s |
| qcom/a640_gmu.bin | drm/adreno（GPU GMU） | 1.397 s |
| qcom/sm8150/xiaomi/crux/a640_zap.mbn | drm/adreno（zap） | 1.4 s 段 |
| regulatory.db | cfg80211 | 启动早期 |
| ftm5_fw.ftb | 无线 FTM | 启动早期 |
| qcom/sdx50m/sbl1.mbn | mhi_pci_generic（喂 X50 的 SBL） | 3.022 s |

根分区挂载在 3.126 s，所以上表里 3.022 s 那条是踩线的，必须内嵌。

## 四、initramfs 的适用区间（默认不用）
只有同时满足才考虑放 initramfs：必须早于根挂载 + 体积大或需要频繁更换。
它相对内嵌没有速度优势（都在内存里，而 initramfs 还要多一道解压），
优势在"不用重编内核、不碰内核签名"，代价是要重打包引导链那份 cpio。
本机当前没有这种固件。

## 五、其余全部留在 /lib/firmware
尤其大件：qcom/sdx50m/qdsp6sw.mbn 71 MB、apps.mbn 2.2 MB 等，由用户态
sahara_svc.py 读取后经 MHI 喂给 modem，与内核早期无关，绝不内嵌。

## 六、加一条新固件的步骤
1. 设备上把文件放到 /lib/firmware/<请求名原样>；
2. 若满足第二节两条判定：把它拷到构建树 fw_extra/<同名路径>；
3. 在 .config 的 CONFIG_EXTRA_FIRMWARE 里追加相对路径（空格分隔）；
4. 跑 /root/crux-fw-check.sh 核对两边 md5；
5. 重编内核。

## 七、回滚
从 .config 的 CONFIG_EXTRA_FIRMWARE 里删掉该条目、删掉 fw_extra 里那份文件即可，
设备侧 /lib/firmware 不受影响。
