// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2013-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2024 Linaro Ltd.
 */

#include <linux/qrtr.h>
#include <linux/string.h>
#include <linux/types.h>

#include "ipa.h"

/* [crux] 由 ipa_mhi_proxy.c 导出：modem IPA QMI 服务（重新）到达时触发 0x3c */
/* [crux] 由 ipa_mhi_proxy.ko 导出；本文件编进 vmlinux，不能直接引用模块符号 */
static void crux_mhi_proxy_retrigger(void)
{
#if IS_MODULE(CONFIG_IPA_MHI_PROXY)
	extern void ipa_mhi_proxy_retrigger(void);
	void (*fn)(void) = symbol_get(ipa_mhi_proxy_retrigger);

	if (fn) {
		fn();
		symbol_put(ipa_mhi_proxy_retrigger);
	}
#endif
}
#include "ipa_mem.h"
#include "ipa_modem.h"
#include "ipa_qmi_msg.h"

/**
 * DOC: AP/Modem QMI Handshake
 *
 * The AP and modem perform a "handshake" at initialization time to ensure
 * both sides know when everything is ready to begin operating.  The AP
 * driver (this code) uses two QMI handles (endpoints) for this; a client
 * using a service on the modem, and server to service modem requests (and
 * to supply an indication message from the AP).  Once the handshake is
 * complete, the AP and modem may begin IPA operation.  This occurs
 * only when the AP IPA driver, modem IPA driver, and IPA microcontroller
 * are ready.
 *
 * The QMI service on the modem expects to receive an INIT_DRIVER request from
 * the AP, which contains parameters used by the modem during initialization.
 * The AP sends this request as soon as it is knows the modem side service
 * is available.  The modem responds to this request, and if this response
 * contains a success result, the AP knows the modem IPA driver is ready.
 *
 * The modem is responsible for loading firmware on the IPA microcontroller.
 * This occurs only during the initial modem boot.  The modem sends a
 * separate DRIVER_INIT_COMPLETE request to the AP to report that the
 * microcontroller is ready.  The AP may assume the microcontroller is
 * ready and remain so (even if the modem reboots) once it has received
 * and responded to this request.
 *
 * There is one final exchange involved in the handshake.  It is required
 * on the initial modem boot, but optional (but in practice does occur) on
 * subsequent boots.  The modem expects to receive a final INIT_COMPLETE
 * indication message from the AP when it is about to begin its normal
 * operation.  The AP will only send this message after it has received
 * and responded to an INDICATION_REGISTER request from the modem.
 *
 * So in summary:
 * - Whenever the AP learns the modem has booted and its IPA QMI service
 *   is available, it sends an INIT_DRIVER request to the modem.  The
 *   modem supplies a success response when it is ready to operate.
 * - On the initial boot, the modem sets up the IPA microcontroller, and
 *   sends a DRIVER_INIT_COMPLETE request to the AP when this is done.
 * - When the modem is ready to receive an INIT_COMPLETE indication from
 *   the AP, it sends an INDICATION_REGISTER request to the AP.
 * - On the initial modem boot, everything is ready when:
 *	- AP has received a success response from its INIT_DRIVER request
 *	- AP has responded to a DRIVER_INIT_COMPLETE request
 *	- AP has responded to an INDICATION_REGISTER request from the modem
 *	- AP has sent an INIT_COMPLETE indication to the modem
 * - On subsequent modem boots, everything is ready when:
 *	- AP has received a success response from its INIT_DRIVER request
 *	- AP has responded to a DRIVER_INIT_COMPLETE request
 * - The INDICATION_REGISTER request and INIT_COMPLETE indication are
 *   optional for non-initial modem boots, and have no bearing on the
 *   determination of when things are "ready"
 */

#define IPA_HOST_SERVICE_SVC_ID		0x31
#define IPA_HOST_SVC_VERS		1
#define IPA_HOST_SERVICE_INS_ID		1

#define IPA_MODEM_SERVICE_SVC_ID	0x31
#define IPA_MODEM_SERVICE_INS_ID	2
#define IPA_MODEM_SVC_VERS		1

#define QMI_INIT_DRIVER_TIMEOUT		60000	/* A minute in milliseconds */

/* Send an INIT_COMPLETE indication message to the modem */
static void ipa_server_init_complete(struct ipa_qmi *ipa_qmi)
{
	struct ipa *ipa = container_of(ipa_qmi, struct ipa, qmi);
	struct qmi_handle *qmi = &ipa_qmi->server_handle;
	struct sockaddr_qrtr *sq = &ipa_qmi->modem_sq;
	struct ipa_init_complete_ind ind = { };
	int ret;

	ind.status.result = QMI_RESULT_SUCCESS_V01;
	ind.status.error = QMI_ERR_NONE_V01;

	ret = qmi_send_indication(qmi, sq, IPA_QMI_INIT_COMPLETE,
				   IPA_QMI_INIT_COMPLETE_IND_SZ,
				   ipa_init_complete_ind_ei, &ind);
	if (ret)
		dev_err(ipa->dev,
			"error %d sending init complete indication\n", ret);
	else
		ipa_qmi->indication_sent = true;
}

/* If requested (and not already sent) send the INIT_COMPLETE indication */
static void ipa_qmi_indication(struct ipa_qmi *ipa_qmi)
{
	if (!ipa_qmi->indication_requested)
		return;

	if (ipa_qmi->indication_sent)
		return;

	ipa_server_init_complete(ipa_qmi);
}

/* Determine whether everything is ready to start normal operation.
 * We know everything (else) is ready when we know the IPA driver on
 * the modem is ready, and the microcontroller is ready.
 *
 * When the modem boots (or reboots), the handshake sequence starts
 * with the AP sending the modem an INIT_DRIVER request.  Within
 * that request, the uc_loaded flag will be zero (false) for an
 * initial boot, non-zero (true) for a subsequent (SSR) boot.
 */
static void ipa_qmi_ready(struct ipa_qmi *ipa_qmi)
{
	struct ipa *__ipa0 = container_of(ipa_qmi, struct ipa, qmi);

	dev_info(__ipa0->dev, "[crux qmi] ready-check: modem_ready=%d uc_ready=%d initial_boot=%d ind_req=%d ind_sent=%d\n",
		 ipa_qmi->modem_ready, ipa_qmi->uc_ready, ipa_qmi->initial_boot,
		 ipa_qmi->indication_requested, ipa_qmi->indication_sent);
	struct ipa *ipa;
	int ret;

	/* We aren't ready until the modem and microcontroller are */
	if (!ipa_qmi->modem_ready || !ipa_qmi->uc_ready)
		return;

	/* Send the indication message if it was requested */
	ipa_qmi_indication(ipa_qmi);

	/* The initial boot requires us to send the indication. */
	if (ipa_qmi->initial_boot) {
		if (!ipa_qmi->indication_sent)
			return;

		/* The initial modem boot completed successfully */
		ipa_qmi->initial_boot = false;
	}

	/* We're ready.  Start up normal operation */
	ipa = container_of(ipa_qmi, struct ipa, qmi);
	ret = ipa_modem_start(ipa);
	if (ret)
		dev_err(ipa->dev, "error %d starting modem\n", ret);
}

/* All QMI clients from the modem node are gone (modem shut down or crashed). */
static void ipa_server_bye(struct qmi_handle *qmi, unsigned int node)
{
	struct ipa_qmi *ipa_qmi;

	ipa_qmi = container_of(qmi, struct ipa_qmi, server_handle);

	/* The modem client and server go away at the same time */
	pr_info("[crux ipa] server_bye node=%u -> modem_sq cleared\n", node);
	memset(&ipa_qmi->modem_sq, 0, sizeof(ipa_qmi->modem_sq));

	/* initial_boot doesn't change when modem reboots */
	/* uc_ready doesn't change when modem reboots */
	ipa_qmi->modem_ready = false;
	ipa_qmi->indication_requested = false;
	ipa_qmi->indication_sent = false;
}

static const struct qmi_ops ipa_server_ops = {
	.bye		= ipa_server_bye,
};

/* Callback function to handle an INDICATION_REGISTER request message from the
 * modem.  This informs the AP that the modem is now ready to receive the
 * INIT_COMPLETE indication message.
 */
static void ipa_server_indication_register(struct qmi_handle *qmi,
					   struct sockaddr_qrtr *sq,
					   struct qmi_txn *txn,
					   const void *decoded)
{
	struct ipa_indication_register_rsp rsp = { };
	struct ipa_qmi *ipa_qmi;
	struct ipa *ipa;
	int ret;

	ipa_qmi = container_of(qmi, struct ipa_qmi, server_handle);
	ipa = container_of(ipa_qmi, struct ipa, qmi);

	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_INDICATION_REGISTER,
				IPA_QMI_INDICATION_REGISTER_RSP_SZ,
				ipa_indication_register_rsp_ei, &rsp);
	if (!ret) {
		ipa_qmi->indication_requested = true;
		/* [crux 2026-10-19] 对齐原厂 ipa3_handle_indication_req()：
		 * 缓存 modem 作为 client 的 sq，供 0x3c 等 INDICATION 使用。
		 */
		ipa_qmi->modem_client_sq = *sq;
		dev_info(ipa->dev,
			 "[crux qmi] INDICATION_REGISTER from modem (client %u:%u)\n",
			 sq->sq_node, sq->sq_port);
		ipa_qmi_ready(ipa_qmi);		/* We might be ready now */
	} else {
		dev_err(ipa->dev,
			"error %d sending register indication response\n", ret);
	}
}

/* Respond to a DRIVER_INIT_COMPLETE request message from the modem. */
static void ipa_server_driver_init_complete(struct qmi_handle *qmi,
					    struct sockaddr_qrtr *sq,
					    struct qmi_txn *txn,
					    const void *decoded)
{
	struct ipa_driver_init_complete_rsp rsp = { };
	struct ipa_qmi *ipa_qmi;
	struct ipa *ipa;
	int ret;

	ipa_qmi = container_of(qmi, struct ipa_qmi, server_handle);
	ipa = container_of(ipa_qmi, struct ipa, qmi);

	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_DRIVER_INIT_COMPLETE,
				IPA_QMI_DRIVER_INIT_COMPLETE_RSP_SZ,
				ipa_driver_init_complete_rsp_ei, &rsp);
	if (!ret) {
		ipa_qmi->uc_ready = true;
		dev_info(ipa->dev, "[crux qmi] DRIVER_INIT_COMPLETE from modem (uc_ready=1)\n");
		ipa_qmi_ready(ipa_qmi);		/* We might be ready now */
	} else {
		dev_err(ipa->dev,
			"error %d sending init complete response\n", ret);
	}
}

/*
 * [crux 2026-10-16] IPA-MHI proxy support
 *
 * The AP must tell the internal modem where the external modem's proxy MHI
 * channel/event doorbells are (QMI_IPA_MHI_READY_IND, 0x3c) before the modem
 * can hand us the MHI channel contexts (ALLOC_CHANNEL, 0x3d).  We remember
 * the IPA instance here so the ipa_mhi_proxy module can reach the QMI
 * client handle that talks to the modem.
 */
static struct ipa *ipa_mhi_proxy_ipa;

/* [crux 2026-10-17] 0x3d ALLOC_CHANNEL 的真正处理要落到外部 modem 的 MHI 控制器上，
 * 而它在 ipa_mhi_proxy 模块里。这里只留一个回调给模块注册。
 */
typedef int (*ipa_mhi_alloc_handler_t)(const u8 *tr, u32 tr_len,
				       const u8 *er, u32 er_len);
static ipa_mhi_alloc_handler_t ipa_mhi_alloc_handler;

int ipa_qmi_register_mhi_alloc_handler(ipa_mhi_alloc_handler_t fn)
{
	ipa_mhi_alloc_handler = fn;
	pr_info("[crux-qmi] alloc handler registered: %ps\n", fn);
	return 0;
}
EXPORT_SYMBOL_GPL(ipa_qmi_register_mhi_alloc_handler);


int ipa_qmi_send_mhi_ready_ind(const struct ipa_mhi_ch_init_info *ch,
			       unsigned int n_ch)
{
	struct crux_mhi_ready_ind ind;
	struct ipa_qmi *ipa_qmi;
	ssize_t ret;

	if (!ipa_mhi_proxy_ipa || !n_ch ||
	    n_ch > ARRAY_SIZE(ind.ch)) {
		pr_info("[crux mhi-proxy] helper: bad arg (ipa=%px n_ch=%u)\n",
			ipa_mhi_proxy_ipa, n_ch);
		return -EINVAL;
	}
	ipa_qmi = &ipa_mhi_proxy_ipa->qmi;
	if (!ipa_qmi->modem_sq.sq_node && !ipa_qmi->modem_sq.sq_port)
		return -ENOTCONN;

	memset(&ind, 0, sizeof(ind));
	ind.ch_info_arr_len = n_ch;
	memcpy(ind.ch, ch, n_ch * sizeof(*ch));
	/* [crux 2026-10-19] 对齐原厂 _populate_smmu_info()：SMMU 平台上必须带
	 * smmu_info TLV(0x10)。首次发 0x3c 时 ctrl/data 的 base/size 仍为 0
	 * （要等 0x3d 的 ctrl/data_addr_map_info 才填），原厂首次亦如此。
	 */
	ind.smmu_info_valid = 1;

	/* [crux 2026-10-19] 原厂语义：0x3c 是 INDICATION，发完即走，
	 * 不等 response（modem 不会回）。这里用 qmi_send_indication()，
	 * 报文用标准 TLV（crux_mhi_ready_ind_ei），与
	 * ipa_mhi_ready_indication_msg_v01 逐字节等价。
	 */
	/* qmi_send_indication() 的 len 是【最大编码缓冲长度】而非结构体大小：
	 * 编码后为 7(hdr)+3(TLV hdr)+1(count)+2*14(ch_info) = 39 字节。
	 * 传 sizeof(ind)=32 会被编码器判为 -ETOOSMALL(-525)。
	 * 取原厂 IPA_MHI_READY_INDICATION_MSG_V01_MAX_MSG_LEN = 123。
	 */
	/* 原厂：qmi_send_indication(ipa3_svc_handle, &client_sq, ...) ——用的是
	 * 【server handle】(源端口=host IPA service)，目标才是 modem。
	 * 用 client_handle 发送时源端口是临时端口，modem 不认。
	 */
	/* [crux 2026-10-19] 原厂 ipa3_qmi_send_mhi_ready_indication() 的目标是
	 * ipa3_qmi_ctx->client_sq（modem 的 client 地址），不是 modem 的
	 * server 端口。发给 server 端口 modem 只会当成普通请求回一个 resp，
	 * 不会走 MHI 通道分配流程，0x3d 永远不来。
	 */
	if (ipa_qmi->modem_client_sq.sq_node || ipa_qmi->modem_client_sq.sq_port) {
		pr_info("[crux mhi-proxy] 0x3c -> modem client %u:%u\n",
			ipa_qmi->modem_client_sq.sq_node,
			ipa_qmi->modem_client_sq.sq_port);
		ret = qmi_send_indication(&ipa_qmi->server_handle,
					  &ipa_qmi->modem_client_sq,
					  IPA_QMI_MHI_READY_IND,
					  123, crux_mhi_ready_ind_ei, &ind);
	} else {
		pr_warn("[crux mhi-proxy] no modem client sq yet, fallback to server %u:%u\n",
			ipa_qmi->modem_sq.sq_node, ipa_qmi->modem_sq.sq_port);
		ret = qmi_send_indication(&ipa_qmi->server_handle,
					  &ipa_qmi->modem_sq,
					  IPA_QMI_MHI_READY_IND,
					  123, crux_mhi_ready_ind_ei, &ind);
	}
	if (ret < 0)
		pr_err("[crux mhi-proxy] send 0x3c failed %zd\n", ret);
	else
		pr_info("[crux mhi-proxy] 0x3c IND sent (%zd bytes, %u ch)\n",
			ret, n_ch);

	return ret < 0 ? ret : 0;
}
EXPORT_SYMBOL_GPL(ipa_qmi_send_mhi_ready_ind);

/* [crux 2026-10-16] Handle MHI_ALLOC_CHANNEL (0x3d) from the modem:
 * log the payload it hands us (TR/ER contexts); programming the external
 * modem's contexts with mhi_device_configure() is the next step.
 */
static void ipa_server_mhi_alloc_channel(struct qmi_handle *qmi,
					 struct sockaddr_qrtr *sq,
					 struct qmi_txn *txn,
					 const void *decoded)
{
	const struct ipa_mhi_alloc_ch_req *req = decoded;
	struct ipa_mhi_alloc_ch_rsp rsp = {
		.rsp.result = QMI_RESULT_SUCCESS_V01,
		.rsp.error = QMI_ERR_NONE_V01,
	};
	u32 len = min_t(u32, req->payload_len, sizeof(req->payload));
	int ret;

	pr_info("[crux mhi-proxy] MHI_ALLOC_CHANNEL from %u:%u payload_len=%u er_len=%u\n",
		sq->sq_node, sq->sq_port, req->payload_len, req->er_payload_len);
	print_hex_dump(KERN_INFO, "[crux mhi-proxy] alloc TR: ",
		       DUMP_PREFIX_OFFSET, 16, 1, req->payload, len, true);
	print_hex_dump(KERN_INFO, "[crux mhi-proxy] alloc ER: ",
		       DUMP_PREFIX_OFFSET, 16, 1, req->er_payload,
		       min_t(u32, req->er_payload_len, sizeof(req->er_payload)), true);

	if (ipa_mhi_alloc_handler) {
		int cfg = ipa_mhi_alloc_handler(req->payload, req->payload_len,
					 req->er_payload, req->er_payload_len);
		if (cfg) {
			pr_err("[crux mhi-proxy] alloc handler failed %d\n", cfg);
			rsp.rsp.result = QMI_RESULT_FAILURE_V01;
			rsp.rsp.error = QMI_ERR_INTERNAL_V01;
		} else {
			pr_info("[crux mhi-proxy] alloc handler OK (CCA/ECA programmed)\n");
		}
	} else {
		pr_err("[crux mhi-proxy] no alloc handler registered!\n");
	}

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_MHI_ALLOC_CHANNEL,
				sizeof(rsp), ipa_mhi_alloc_ch_rsp_ei, &rsp);
	if (ret)
		pr_err("[crux mhi-proxy] alloc resp failed %d\n", ret);
}

/* [crux 2026-10-17] 最小实现：主线没实现的 modem -> AP 请求
 *   （实测见到的：0x27 IPA_CONFIG_REQ ~61B、0x23 INSTALL_FILTER_RULE_REQ 1031B）
 * 处理方式：收下 -> 原始 hex 已在 qmi_interface.c 里 dump -> 回一个通用成功响应。
 * 目的只有一个：看 modem 的 IPA 初始化会不会因此继续走（进而发 0x3d
 * MHI_ALLOC_CHANNEL）。不做任何实际配置，不搬 vendor 的 EI 表 / 40KB 结构。
 */
struct ipa_generic_rsp {
	struct qmi_response_type_v01 rsp;
};

/* qmi_send_response() 的 len 是“TLV 编码后的最大长度”，不是 sizeof(结构)！
 * 主线其它响应都传常量（如 IPA_QMI_INDICATION_REGISTER_RSP_SZ = 7）。
 * 传 sizeof(rsp)=4 会得到 qmi_encode: Too Small Buffer -> -525。
 */
#define IPA_QMI_GENERIC_RSP_SZ	7	/* TLV header(3) + result/error(4) */

static const struct qmi_elem_info ipa_generic_rsp_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.array_type	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_generic_rsp, rsp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{ }
};

/* [crux 2026-10-17] 默认只 dump、**不**伪造成功响应。
 * 原因：给 modem 的 0x27 (IPA_CONFIG_REQ) / 0x23 (INSTALL_FILTER_RULE)
 * 回“成功”会让 modem 误以为 IPA 已经配置好；之后 modem 一切 online 去用那套
 * 并不存在的配置，就可能把 IPA/GSI 卡死 -> AP 整机 hang（实机复现两次）。
 * 需要探测“完全不回响应 modem 会不会继续”时，运行时开开关：
 *   insmod ipa.ko ack_unknown=1  或  echo 1 > /sys/module/ipa/parameters/ack_unknown
 */
static bool ack_unknown;
/* 只对 IPA 初始化两类请求（0x27 CONFIG / 0x23 INSTALL_FILTER_RULE）回成功；
 * 与 ack_unknown 分开，避免把未知请求一概 fake 成功。
 */
static bool ack_ipa_init = true;
module_param(ack_ipa_init, bool, 0644);
MODULE_PARM_DESC(ack_ipa_init,
		 "Ack IPA_CONFIG/INSTALL_FILTER_RULE from modem (no real filter install)");

module_param(ack_unknown, bool, 0644);
MODULE_PARM_DESC(ack_unknown,
		 "Reply success to unhandled modem IPA requests (fakes IPA config, risky)");

static void ipa_server_unhandled_req(struct qmi_handle *qmi,
				     struct sockaddr_qrtr *sq,
				     struct qmi_txn *txn, u16 msg_id,
				     const void *buf, size_t len)
{
	struct ipa_generic_rsp rsp = {
		.rsp.result = QMI_RESULT_SUCCESS_V01,
		.rsp.error = QMI_ERR_NONE_V01,
	};
	int ret;

	/* [crux 2026-10-17] 只对“初始化类”请求单独放行（不开启危险的全量 ack）：
	 *   0x27 IPA_CONFIG_REQ     —— 原厂也只是接收并缓存配置，然后回成功
	 *   0x23 INSTALL_FILTER_RULE —— 原厂会真的装 UL 过滤规则；主线没有对应 API，
	 *                                这里只回成功（rule 列表置空），属于已知局限
	 */
	if (!ack_unknown) {
		if (ack_ipa_init && (msg_id == IPA_QMI_IPA_CONFIG ||
				     msg_id == IPA_QMI_INSTALL_FILTER_RULE)) {
			pr_info("[crux-qmi] ack init req id=0x%04x len=%zu -> success (filters NOT programmed)\n",
				msg_id, len);
		} else {
			pr_info("[crux-qmi] no ack for id=0x%04x (ack_unknown=0)\n",
				msg_id);
			return;
		}
	}

	pr_info("[crux-qmi] generic ack id=0x%04x len=%zu -> %u:%u\n",
		msg_id, len, sq->sq_node, sq->sq_port);

	ret = qmi_send_response(qmi, sq, txn, msg_id, IPA_QMI_GENERIC_RSP_SZ,
				ipa_generic_rsp_ei, &rsp);
	if (ret)
		pr_err("[crux-qmi] generic ack id=0x%04x failed %d\n",
		       msg_id, ret);
}

/* The server handles two request message types sent by the modem. */
static const struct qmi_msg_handler ipa_server_msg_handlers[] = {
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_INDICATION_REGISTER,
		.ei		= ipa_indication_register_req_ei,
		.decoded_size	= IPA_QMI_INDICATION_REGISTER_REQ_SZ,
		.fn		= ipa_server_indication_register,
	},
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_DRIVER_INIT_COMPLETE,
		.ei		= ipa_driver_init_complete_req_ei,
		.decoded_size	= IPA_QMI_DRIVER_INIT_COMPLETE_REQ_SZ,
		.fn		= ipa_server_driver_init_complete,
	},
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_MHI_ALLOC_CHANNEL,
		.ei		= ipa_mhi_alloc_ch_req_ei,
		.decoded_size	= sizeof(struct ipa_mhi_alloc_ch_req),
		.fn		= ipa_server_mhi_alloc_channel,
	},
	{ },
};

/* Handle an INIT_DRIVER response message from the modem. */
static void ipa_client_init_driver(struct qmi_handle *qmi,
				   struct sockaddr_qrtr *sq,
				   struct qmi_txn *txn, const void *decoded)
{
	struct ipa_qmi *__ipa_qmi = container_of(qmi, struct ipa_qmi, client_handle);
	struct ipa *__ipa = container_of(__ipa_qmi, struct ipa, qmi);

	dev_info(__ipa->dev, "[crux qmi] INIT_DRIVER response received\n");
	txn->result = 0;	/* IPA_QMI_INIT_DRIVER request was successful */
	complete(&txn->completion);
}

/* The client handles one response message type sent by the modem. */
/* [crux 2026-10-17] 0x3c 的响应处理：modem 回的是通用 result/error */
static void ipa_client_mhi_ready_ind_rsp(struct qmi_handle *qmi,
					 struct sockaddr_qrtr *sq,
					 struct qmi_txn *txn,
					 const void *decoded)
{
	const struct qmi_response_type_v01 *rsp = decoded;

	pr_info("[crux-qmi] 0x3c RESPONSE: result=%u error=%u (from %u:%u)\n",
		rsp->result, rsp->error, sq->sq_node, sq->sq_port);
}

static const struct qmi_msg_handler ipa_client_msg_handlers[] = {
	{
		.type		= QMI_RESPONSE,
		.msg_id		= IPA_QMI_MHI_READY_IND,
		.fn		= ipa_client_mhi_ready_ind_rsp,
	},
	{
		.type		= QMI_RESPONSE,
		.msg_id		= IPA_QMI_INIT_DRIVER,
		.ei		= ipa_init_modem_driver_rsp_ei,
		.decoded_size	= IPA_QMI_INIT_DRIVER_RSP_SZ,
		.fn		= ipa_client_init_driver,
	},
	{ },
};

/* Return a pointer to an init modem driver request structure, which contains
 * configuration parameters for the modem.  The modem may be started multiple
 * times, but generally these parameters don't change so we can reuse the
 * request structure once it's initialized.  The only exception is the
 * skip_uc_load field, which will be set only after the microcontroller has
 * reported it has completed its initialization.
 */
static const struct ipa_init_modem_driver_req *
init_modem_driver_req(struct ipa_qmi *ipa_qmi)
{
	struct ipa *ipa = container_of(ipa_qmi, struct ipa, qmi);
	u32 modem_route_count = ipa->modem_route_count;
	static struct ipa_init_modem_driver_req req;
	const struct ipa_mem *mem;

	/* The microcontroller is initialized on the first boot */
	req.skip_uc_load_valid = 1;
	req.skip_uc_load = ipa->uc_loaded ? 1 : 0;

	/* We only have to initialize most of it once */
	if (req.platform_type_valid)
		return &req;

	req.platform_type_valid = 1;
	req.platform_type = IPA_QMI_PLATFORM_TYPE_MSM_ANDROID;

	mem = ipa_mem_find(ipa, IPA_MEM_MODEM_HEADER);
	if (mem->size) {
		req.hdr_tbl_info_valid = 1;
		req.hdr_tbl_info.start = ipa->mem_offset + mem->offset;
		req.hdr_tbl_info.end = req.hdr_tbl_info.start + mem->size - 1;
	}

	mem = ipa_mem_find(ipa, IPA_MEM_V4_ROUTE);
	req.v4_route_tbl_info_valid = 1;
	req.v4_route_tbl_info.start = ipa->mem_offset + mem->offset;
	req.v4_route_tbl_info.end = modem_route_count - 1;

	mem = ipa_mem_find(ipa, IPA_MEM_V6_ROUTE);
	req.v6_route_tbl_info_valid = 1;
	req.v6_route_tbl_info.start = ipa->mem_offset + mem->offset;
	req.v6_route_tbl_info.end = modem_route_count - 1;

	mem = ipa_mem_find(ipa, IPA_MEM_V4_FILTER);
	req.v4_filter_tbl_start_valid = 1;
	req.v4_filter_tbl_start = ipa->mem_offset + mem->offset;

	mem = ipa_mem_find(ipa, IPA_MEM_V6_FILTER);
	req.v6_filter_tbl_start_valid = 1;
	req.v6_filter_tbl_start = ipa->mem_offset + mem->offset;

	mem = ipa_mem_find(ipa, IPA_MEM_MODEM);
	if (mem->size) {
		req.modem_mem_info_valid = 1;
		req.modem_mem_info.start = ipa->mem_offset + mem->offset;
		req.modem_mem_info.size = mem->size;
	}

	req.ctrl_comm_dest_end_pt_valid = 1;
	req.ctrl_comm_dest_end_pt =
		ipa->name_map[IPA_ENDPOINT_AP_MODEM_RX]->endpoint_id;

	/* skip_uc_load_valid and skip_uc_load are set above */

	mem = ipa_mem_find(ipa, IPA_MEM_MODEM_PROC_CTX);
	if (mem->size) {
		req.hdr_proc_ctx_tbl_info_valid = 1;
		req.hdr_proc_ctx_tbl_info.start =
			ipa->mem_offset + mem->offset;
		req.hdr_proc_ctx_tbl_info.end =
			req.hdr_proc_ctx_tbl_info.start + mem->size - 1;
	}

	/* Nothing to report for the compression table (zip_tbl_info) */

	mem = ipa_mem_find(ipa, IPA_MEM_V4_ROUTE_HASHED);
	if (mem->size) {
		req.v4_hash_route_tbl_info_valid = 1;
		req.v4_hash_route_tbl_info.start =
				ipa->mem_offset + mem->offset;
		req.v4_hash_route_tbl_info.end = modem_route_count - 1;
	}

	mem = ipa_mem_find(ipa, IPA_MEM_V6_ROUTE_HASHED);
	if (mem->size) {
		req.v6_hash_route_tbl_info_valid = 1;
		req.v6_hash_route_tbl_info.start =
			ipa->mem_offset + mem->offset;
		req.v6_hash_route_tbl_info.end = modem_route_count - 1;
	}

	mem = ipa_mem_find(ipa, IPA_MEM_V4_FILTER_HASHED);
	if (mem->size) {
		req.v4_hash_filter_tbl_start_valid = 1;
		req.v4_hash_filter_tbl_start = ipa->mem_offset + mem->offset;
	}

	mem = ipa_mem_find(ipa, IPA_MEM_V6_FILTER_HASHED);
	if (mem->size) {
		req.v6_hash_filter_tbl_start_valid = 1;
		req.v6_hash_filter_tbl_start = ipa->mem_offset + mem->offset;
	}

	/* The stats fields are only valid for IPA v4.0+ */
	if (ipa->version >= IPA_VERSION_4_0) {
		mem = ipa_mem_find(ipa, IPA_MEM_STATS_QUOTA_MODEM);
		if (mem->size) {
			req.hw_stats_quota_base_addr_valid = 1;
			req.hw_stats_quota_base_addr =
				ipa->mem_offset + mem->offset;
			req.hw_stats_quota_size_valid = 1;
			req.hw_stats_quota_size = ipa->mem_offset + mem->size;
		}

		/* If the DROP stats region is defined, include it */
		mem = ipa_mem_find(ipa, IPA_MEM_STATS_DROP);
		if (mem && mem->size) {
			req.hw_stats_drop_base_addr_valid = 1;
			req.hw_stats_drop_base_addr =
				ipa->mem_offset + mem->offset;
			req.hw_stats_drop_size_valid = 1;
			req.hw_stats_drop_size = ipa->mem_offset + mem->size;
		}
	}

	return &req;
}

/* [crux 2026-10-19] 与基带配合：INIT_DRIVER 失败后自动延时重试。
 * 基带（X50）从 SBL 进 mission mode 后，它的 IPA 服务 / QRTR 端点需要
 * 几秒到几十秒才真正就绪，期间发的 INIT_DRIVER 会石沉大海（-110 超时）。
 * 所以失败后不清盘，3 秒后重来，最多 6 次。
 */
static struct delayed_work ipa_qmi_rehs_work;
static bool ipa_qmi_rehs_active;
static unsigned int ipa_qmi_rehs_retry;
#define IPA_QMI_REHS_MAX_RETRY	6
#define IPA_QMI_REHS_DELAY_MS	3000

static void ipa_qmi_rehs_worker(struct work_struct *work)
{
	struct ipa *ipa = ipa_mhi_proxy_ipa;

	if (!ipa)
		return;

	dev_info(ipa->dev, "[crux qmi] rehandshake retry %u\n",
		 ipa_qmi_rehs_retry);
	ipa->uc_loaded = false;
	schedule_work(&ipa->qmi.init_driver_work);
}

/* Send an INIT_DRIVER request to the modem, and wait for it to complete. */
static void ipa_client_init_driver_work(struct work_struct *work)
{
	unsigned long timeout = msecs_to_jiffies(QMI_INIT_DRIVER_TIMEOUT);
	const struct ipa_init_modem_driver_req *req;
	struct ipa_qmi *ipa_qmi;
	struct qmi_handle *qmi;
	struct qmi_txn txn;
	struct device *dev;
	struct ipa *ipa;
	int ret;

	ipa_qmi = container_of(work, struct ipa_qmi, init_driver_work);
	qmi = &ipa_qmi->client_handle;

	ipa = container_of(ipa_qmi, struct ipa, qmi);
	dev = ipa->dev;

	ret = qmi_txn_init(qmi, &txn, NULL, NULL);
	if (ret < 0) {
		dev_err(dev, "error %d preparing init driver request\n", ret);
		return;
	}

	/* Send the request, and if successful wait for its response */
	req = init_modem_driver_req(ipa_qmi);
	dev_info(dev, "[crux qmi] sending INIT_DRIVER to node=%u port=%u skip_uc_load=%u\n",
		 ipa_qmi->modem_sq.sq_node, ipa_qmi->modem_sq.sq_port,
		 req->skip_uc_load);
	ret = qmi_send_request(qmi, &ipa_qmi->modem_sq, &txn,
			       IPA_QMI_INIT_DRIVER, IPA_QMI_INIT_DRIVER_REQ_SZ,
			       ipa_init_modem_driver_req_ei, req);
	if (ret)
		dev_err(dev, "error %d sending init driver request\n", ret);
	else if ((ret = qmi_txn_wait(&txn, timeout)))
		dev_err(dev, "error %d awaiting init driver response\n", ret);
	dev_info(dev, "[crux qmi] INIT_DRIVER send/wait ret=%d\n", ret);

	if (!ret) {
		ipa_qmi->modem_ready = true;
		ipa_qmi_ready(ipa_qmi);		/* We might be ready now */
		if (ipa_qmi_rehs_active) {
			ipa_qmi_rehs_active = false;
			dev_info(dev, "[crux qmi] handshake done after %u retries\n",
				 ipa_qmi_rehs_retry);
		}
	} else {
		/* If any error occurs we need to cancel the transaction */
		qmi_txn_cancel(&txn);

		/* [crux 2026-10-19] 与基带配合：基带还没就绪，延时重试 */
		if (ipa_qmi_rehs_active &&
		    ipa_qmi_rehs_retry < IPA_QMI_REHS_MAX_RETRY) {
			ipa_qmi_rehs_retry++;
			dev_info(dev,
				 "[crux qmi] INIT_DRIVER failed %d, retry %u/%d\n",
				 ret, ipa_qmi_rehs_retry, IPA_QMI_REHS_MAX_RETRY);
			schedule_delayed_work(&ipa_qmi_rehs_work,
				msecs_to_jiffies(IPA_QMI_REHS_DELAY_MS));
		} else if (ipa_qmi_rehs_active) {
			ipa_qmi_rehs_active = false;
			dev_err(dev, "[crux qmi] handshake gave up after %u retries\n",
				ipa_qmi_rehs_retry);
		}
	}
}

/* [crux 2026-10-19] 外部触发一次重新握手。
 * 场景：X50 在 crux 启动后才 pwr_on/feed，QRTR 里 modem 的 IPA service
 * 端口没变（node0 port31），QRTR 不会再报一次 NEW_SERVER，于是内核不会
 * 重发 INIT_DRIVER、也不会收到新的 INDICATION_REGISTER —— modem_client_sq
 * 一直是上一个实例的 client 端口，INDICATION 发过去无人接收，0x3d 不回。
 * 这里手动重发 INIT_DRIVER，让 modem 重新走一遍握手并重新登记 client sq。
 */
int ipa_qmi_rehandshake(u32 node, u32 port)
{
	struct ipa *ipa = ipa_mhi_proxy_ipa;
	struct ipa_qmi *ipa_qmi;

	if (!ipa)
		return -ENODEV;

	ipa_qmi = &ipa->qmi;
	/* node/port 传 0 时沿用当前已协商的 modem server 地址；
	 * 若已被 server_bye 清空，退回 node0 port31（X50 的 IPA control service）。
	 */
	if (!port) {
		if (ipa_qmi->modem_sq.sq_port) {
			node = ipa_qmi->modem_sq.sq_node;
			port = ipa_qmi->modem_sq.sq_port;
		} else {
			node = 0;
			port = 31;
		}
	}
	ipa_qmi->modem_sq.sq_family = AF_QIPCRTR;
	ipa_qmi->modem_sq.sq_node = node;
	ipa_qmi->modem_sq.sq_port = port;
	/* 让下一次 INDICATION_REGISTER 一定覆盖旧值 */
	memset(&ipa_qmi->modem_client_sq, 0, sizeof(ipa_qmi->modem_client_sq));

	/* [crux 2026-10-19] X50 是新启动的实例，uC 固件需要重新加载。
	 * init_modem_driver_req() 里 skip_uc_load = ipa->uc_loaded ? 1 : 0，
	 * 首次 boot 后 uc_loaded 一直是 1，重握手就会带 skip_uc_load=1，
	 * modem 认为不用加载 uC 而**不回应 INIT_DRIVER**（实测 60s 超时）。
	 * 所以这里清掉，让它重新走一遍 uC 加载。
	 */
	ipa->uc_loaded = false;

	dev_info(ipa->dev, "[crux qmi] rehandshake -> %u:%u\n", node, port);

	ipa_qmi_rehs_active = true;
	ipa_qmi_rehs_retry = 0;
	INIT_DELAYED_WORK(&ipa_qmi_rehs_work, ipa_qmi_rehs_worker);
	schedule_work(&ipa_qmi->init_driver_work);

	return 0;
}
EXPORT_SYMBOL_GPL(ipa_qmi_rehandshake);

/* [crux 2026-10-19] 供 ipa_mhi_proxy 查询：modem 的 client 端点是否已就绪。
 * 只有拿到 modem 发来的 INDICATION_REGISTER 之后，0x3c 才有正确去处。
 */
bool ipa_qmi_modem_client_ready(void)
{
	struct ipa *ipa = ipa_mhi_proxy_ipa;

	if (!ipa)
		return false;

	return ipa->qmi.modem_client_sq.sq_port != 0;
}
EXPORT_SYMBOL_GPL(ipa_qmi_modem_client_ready);

/* The modem server is now available.  We will send an INIT_DRIVER request
 * to the modem, but can't wait for it to complete in this callback thread.
 * Schedule a worker on the global workqueue to do that for us.
 */
static int
ipa_client_new_server(struct qmi_handle *qmi, struct qmi_service *svc)
{
	struct ipa_qmi *ipa_qmi;

	ipa_qmi = container_of(qmi, struct ipa_qmi, client_handle);

	ipa_qmi->modem_sq.sq_family = AF_QIPCRTR;
	ipa_qmi->modem_sq.sq_node = svc->node;
	ipa_qmi->modem_sq.sq_port = svc->port;
	crux_mhi_proxy_retrigger();

	{
		struct ipa *__ipa1 = container_of(ipa_qmi, struct ipa, qmi);

		dev_info(__ipa1->dev, "[crux qmi] modem IPA service arrived: node=%u port=%u\n", svc->node, svc->port);
	}

	schedule_work(&ipa_qmi->init_driver_work);

	return 0;
}

static const struct qmi_ops ipa_client_ops = {
	.new_server	= ipa_client_new_server,
};

/* Set up for QMI message exchange */
int ipa_qmi_setup(struct ipa *ipa)
{
	struct ipa_qmi *ipa_qmi = &ipa->qmi;
	int ret;

	ipa_mhi_proxy_ipa = ipa;
	ipa_qmi->initial_boot = true;

	/* The server handle is used to handle the DRIVER_INIT_COMPLETE
	 * request on the first modem boot.  It also receives the
	 * INDICATION_REGISTER request on the first boot and (optionally)
	 * subsequent boots.  The INIT_COMPLETE indication message is
	 * sent over the server handle if requested.
	 */
	ret = qmi_handle_init(&ipa_qmi->server_handle,
			      IPA_QMI_SERVER_MAX_RCV_SZ, &ipa_server_ops,
			      ipa_server_msg_handlers);
	if (ret)
		return ret;

	/* [crux] bring-up: 接住主线没实现的 modem->AP 请求（只 dump + 通用成功应答） */
	ipa_qmi->server_handle.unhandled_req = ipa_server_unhandled_req;

	ret = qmi_add_server(&ipa_qmi->server_handle, IPA_HOST_SERVICE_SVC_ID,
			     IPA_HOST_SVC_VERS, IPA_HOST_SERVICE_INS_ID);
	if (ret)
		goto err_server_handle_release;

	/* The client handle is only used for sending an INIT_DRIVER request
	 * to the modem, and receiving its response message.
	 */
	ret = qmi_handle_init(&ipa_qmi->client_handle,
			      IPA_QMI_CLIENT_MAX_RCV_SZ, &ipa_client_ops,
			      ipa_client_msg_handlers);
	if (ret)
		goto err_server_handle_release;

	/* We need this ready before the service lookup is added */
	INIT_WORK(&ipa_qmi->init_driver_work, ipa_client_init_driver_work);

	ret = qmi_add_lookup(&ipa_qmi->client_handle, IPA_MODEM_SERVICE_SVC_ID,
			     IPA_MODEM_SVC_VERS, IPA_MODEM_SERVICE_INS_ID);

	dev_info(ipa->dev, "[crux qmi] setup done: server 0x31/%u/%u + lookup 0x31/%u/%u\n",
		 IPA_HOST_SVC_VERS, IPA_HOST_SERVICE_INS_ID,
		 IPA_MODEM_SVC_VERS, IPA_MODEM_SERVICE_INS_ID);
	if (ret)
		goto err_client_handle_release;

	return 0;

err_client_handle_release:
	/* Releasing the handle also removes registered lookups */
	qmi_handle_release(&ipa_qmi->client_handle);
	memset(&ipa_qmi->client_handle, 0, sizeof(ipa_qmi->client_handle));
err_server_handle_release:
	/* Releasing the handle also removes registered services */
	qmi_handle_release(&ipa_qmi->server_handle);
	memset(&ipa_qmi->server_handle, 0, sizeof(ipa_qmi->server_handle));

	return ret;
}

/* Tear down IPA QMI handles */
void ipa_qmi_teardown(struct ipa *ipa)
{
	cancel_work_sync(&ipa->qmi.init_driver_work);

	qmi_handle_release(&ipa->qmi.client_handle);
	memset(&ipa->qmi.client_handle, 0, sizeof(ipa->qmi.client_handle));

	qmi_handle_release(&ipa->qmi.server_handle);
	memset(&ipa->qmi.server_handle, 0, sizeof(ipa->qmi.server_handle));
}
