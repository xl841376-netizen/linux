/* compat: NPU DDR BW devfreq（下游专有）——主线先空实现 */
#ifndef _COMPAT_DEVFREQ_DEVBW_H
#define _COMPAT_DEVFREQ_DEVBW_H
#include <linux/device.h>
static inline int devfreq_add_devbw(struct device *dev) { return 0; }
static inline int devfreq_remove_devbw(struct device *dev) { return 0; }
static inline int devfreq_suspend_devbw(struct device *dev) { return 0; }
static inline int devfreq_resume_devbw(struct device *dev) { return 0; }
#endif
