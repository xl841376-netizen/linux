/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __IRIS_COMMON_H__
#define __IRIS_COMMON_H__

struct iris_inst;
struct iris_core;
struct iris_buffer;

int iris_vb2_buffer_to_driver(struct vb2_buffer *vb2, struct iris_buffer *buf);
void iris_set_ts_metadata(struct iris_inst *inst, struct vb2_v4l2_buffer *vbuf);
int iris_process_streamon_input(struct iris_inst *inst);
int iris_process_streamon_output(struct iris_inst *inst);
int iris_hfi_session_open(struct iris_inst *inst);
int iris_hfi_session_close(struct iris_inst *inst);
int iris_reserve_core_load(struct iris_inst *inst, u32 frame_rate);
void iris_release_core_load(struct iris_inst *inst);
int iris_session_streamoff(struct iris_inst *inst, u32 plane);
void iris_request_core_recovery(struct iris_inst *inst);
void iris_run_pending_core_recovery(struct iris_core *core);

#endif
