# SPDX-License-Identifier: GPL-2.0
#
ccflags-y += -I$(src)/compat/include

obj-$(CONFIG_MSM_NPU) += msm_npu.o
msm_npu-y := npu_dev.o npu_mgr.o npu_hw_access.o npu_host_ipc.o \
             npu_dbg.o npu_debugfs.o compat.o
