# crux 固件放置约定

## 一、存放位置

固件存放地为设备 `/lib/firmware`，路径与请求字符串逐字一致，不另起名字、不做副本。

请求名来源：

- 内核侧：驱动的 `request_firmware()` 参数，如 drm 的 `qcom/a630_sqe.fw`、`drivers/bus/mhi/host/pci_generic.c` 里的 `qcom/sdx50m/sbl1.mbn`
- 用户态：`/usr/local/bin/sahara_svc.py` 的 `FW` 常量与 `IMAGE_MAP`

## 二、内嵌进内核的条件

两条同时满足才内嵌：

1. 该固件由内建驱动（`=y`）在 probe 路径里同步请求；
2. 请求时刻早于根分区挂载。

满足则拷一份到构建树 `fw_extra/`（目录结构与 `/lib/firmware` 同构），并把相对路径写进 `.config` 的 `CONFIG_EXTRA_FIRMWARE`。

## 三、当前内嵌清单（6 条，`fw_extra` 共约 540K）

| 请求名 | 请求方 | 请求时刻 |
|---|---|---|
| `qcom/a630_sqe.fw` | drm/adreno（GPU） | 1.397 s |
| `qcom/a640_gmu.bin` | drm/adreno（GPU GMU） | 1.397 s |
| `qcom/sm8150/xiaomi/crux/a640_zap.mbn` | drm/adreno（zap） | 1.4 s 段 |
| `regulatory.db` | cfg80211 | 启动早期 |
| `ftm5_fw.ftb` | 无线 FTM | 启动早期 |
| `qcom/sdx50m/sbl1.mbn` | mhi_pci_generic（X50 的 SBL） | 3.022 s |

根分区挂载约在 3.126 s，上表最后一条在其之前请求。

## 四、initramfs

仅在「早于根挂载」且「体积大或需频繁更换」时放入 initramfs。相对内嵌无速度优势（都在内存里，initramfs 多一道解压），优点是无需重编内核、不碰内核签名。当前无此类固件。

## 五、其余固件留在 `/lib/firmware`

大件如 `qcom/sdx50m/qdsp6sw.mbn`（71 MB）、`apps.mbn`（2.2 MB）由用户态 `sahara_svc.py` 读取后经 MHI 传给 modem，与内核早期无关，不内嵌。

## 六、新增固件

1. 设备上把文件放到 `/lib/firmware/<请求名原样>`；
2. 若满足第二节两条判定，拷到构建树 `fw_extra/<相同路径>`；
3. 在 `.config` 的 `CONFIG_EXTRA_FIRMWARE` 追加相对路径（空格分隔）；
4. 跑 `/root/crux-fw-check.sh` 核对两边 md5；
5. 重编内核。

## 七、回滚

从 `CONFIG_EXTRA_FIRMWARE` 删掉该条目、删掉 `fw_extra` 里对应文件。设备侧 `/lib/firmware` 不受影响。