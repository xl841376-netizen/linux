/* compat: cdsprm cxlimit（下游专有，与 CDSP 侧服务耦合）——主线先空实现 */
#ifndef _COMPAT_CDSPRM_CXLIMIT_H
#define _COMPAT_CDSPRM_CXLIMIT_H

enum cdsprm_npu_corner {
	CDSPRM_NPU_CLK_OFF = 0,
	CDSPRM_NPU_MIN_SVS,
	CDSPRM_NPU_LOW_SVS,
	CDSPRM_NPU_SVS,
	CDSPRM_NPU_SVS_L1,
	CDSPRM_NPU_NOM,
	CDSPRM_NPU_NOM_L1,
	CDSPRM_NPU_TURBO,
	CDSPRM_NPU_TURBO_L1,
};

struct cdsprm_npu_limit_cbs {
	int (*set_corner_limit)(enum cdsprm_npu_corner);
};

enum cdsprm_compute_priority {
	CDSPRM_COMPUTE_HVX_MAX = 1,
	CDSPRM_COMPUTE_AIX_MAX = 2,
	CDSPRM_COMPUTE_HVX_OVER_AIX = 3,
	CDSPRM_COMPUTE_AIX_OVER_HVX = 4,
	CDSPRM_COMPUTE_BALANCED = 5,
};

static inline int cdsprm_compute_core_set_priority(enum cdsprm_compute_priority p) { return 0; }
static inline int cdsprm_cxlimit_npu_limit_register(const struct cdsprm_npu_limit_cbs *a) { return 0; }
static inline int cdsprm_cxlimit_npu_limit_deregister(void) { return 0; }
static inline int cdsprm_cxlimit_npu_activity_notify(unsigned int b) { return 0; }
static inline enum cdsprm_npu_corner cdsprm_cxlimit_npu_corner_notify(enum cdsprm_npu_corner c) { return c; }
static inline int cdsprm_cxlimit_camera_activity_notify(unsigned int b) { return 0; }
#endif
