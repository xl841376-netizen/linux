// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/pm_runtime.h>

#include "iris_core.h"
#include "iris_firmware.h"
#include "iris_state.h"
#include "iris_vpu_common.h"

static void iris_core_deinit_locked(struct iris_core *core, bool power_off)
{
	iris_fw_unload(core);
	if (power_off)
		iris_vpu_power_off(core);
	iris_hfi_queues_deinit(core);
	core->state = IRIS_CORE_DEINIT;
}

void iris_core_deinit(struct iris_core *core)
{
	int ret;

	if (READ_ONCE(core->state) == IRIS_CORE_DEINIT)
		return;

	ret = pm_runtime_resume_and_get(core->dev);

	mutex_lock(&core->lock);
	if (core->state != IRIS_CORE_DEINIT)
		iris_core_deinit_locked(core, ret >= 0);
	mutex_unlock(&core->lock);

	if (ret >= 0)
		pm_runtime_put_sync(core->dev);
}

int iris_core_deinit_for_system_suspend(struct iris_core *core)
{
	int ret = 0;

	mutex_lock(&core->lock);
	if (!list_empty(&core->instances)) {
		dev_warn(core->dev,
			 "refusing system suspend with active codec sessions\n");
		ret = -EBUSY;
	} else if (core->state != IRIS_CORE_DEINIT) {
		dev_info(core->dev, "quiescing legacy VPU5 for system suspend\n");
		iris_core_deinit_locked(core, true);
	}
	mutex_unlock(&core->lock);

	return ret;
}

static int iris_wait_for_system_response(struct iris_core *core)
{
	int ret;

	if (core->state == IRIS_CORE_ERROR)
		return -EIO;

	ret = wait_for_completion_timeout(&core->core_init_done,
					  msecs_to_jiffies(HW_RESPONSE_TIMEOUT_VALUE));
	if (!ret) {
		core->state = IRIS_CORE_ERROR;
		return -ETIMEDOUT;
	}

	return 0;
}

int iris_core_init(struct iris_core *core)
{
	int ret;

	mutex_lock(&core->lock);
	if (core->state == IRIS_CORE_INIT) {
		ret = 0;
		goto exit;
	} else if (core->state == IRIS_CORE_ERROR) {
		ret = -EINVAL;
		goto error;
	}

	core->state = IRIS_CORE_INIT;

	ret = iris_hfi_queues_init(core);
	if (ret)
		goto error;

	ret = iris_vpu_power_on(core);
	if (ret)
		goto error_queue_deinit;

	ret = iris_fw_load(core);
	if (ret)
		goto error_power_off;

	if (core->iris_platform_data->legacy_vpu5) {
		ret = iris_set_hw_state(core, true);
		if (ret)
			goto error_unload_fw;
	}

	ret = iris_vpu_boot_firmware(core);
	if (ret)
		goto error_unload_fw;

	ret = iris_vpu_switch_to_hwmode(core);
	if (ret)
		goto error_unload_fw;

	core->iris_firmware_data->init_hfi_ops(core);

	reinit_completion(&core->core_init_done);
	ret = iris_hfi_core_init(core);
	if (ret)
		goto error_unload_fw;

	mutex_unlock(&core->lock);

	return iris_wait_for_system_response(core);

error_unload_fw:
	iris_fw_unload(core);
error_power_off:
	iris_vpu_power_off(core);
error_queue_deinit:
	iris_hfi_queues_deinit(core);
error:
	core->state = IRIS_CORE_DEINIT;
exit:
	mutex_unlock(&core->lock);

	return ret;
}
