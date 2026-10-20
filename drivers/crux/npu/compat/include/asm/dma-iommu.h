/* compat: 下游 arm dma-iommu 接口 -> 主线用 DT iommus + 通用 IOMMU API */
#ifndef _COMPAT_ASM_DMA_IOMMU_H
#define _COMPAT_ASM_DMA_IOMMU_H
#include <linux/iommu.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>

struct dma_iommu_mapping {
	struct iommu_domain *domain;
};

static struct dma_iommu_mapping npu_compat_mapping;

static inline struct dma_iommu_mapping *
arm_iommu_create_mapping(struct bus_type *bus, dma_addr_t base, size_t size)
{
	return &npu_compat_mapping;
}
static inline void arm_iommu_release_mapping(struct dma_iommu_mapping *m) { }
static inline int arm_iommu_attach_device(struct device *dev,
					  struct dma_iommu_mapping *m)
{
	return 0;
}
static inline void arm_iommu_detach_device(struct device *dev) { }
#endif
