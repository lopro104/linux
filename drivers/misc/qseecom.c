#define SCM_SAVE_PARTITION_HASH_ID 0x01
#define QSEOS_VERSION_14 0x14
// SPDX-License-Identifier: GPL-2.0-only
/*
 * QTI Secure Execution Environment Communicator (QSEECOM) driver
 * Ported for Linux 7.1 / AOSP ACK Mainline (Pixel 4a Sunfish)
 */

#define pr_fmt(fmt) "QSEECOM: %s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/types.h>
#include <linux/clk.h>
#include <linux/elf.h>
#include <linux/firmware.h>
#include <linux/freezer.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/signal.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/of_reserved_mem.h>
#include <linux/iosys-map.h>
#include <linux/kthread.h>
#include <linux/arm-smccc.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/string.h>

#include <linux/qseecom.h>
#include "qseecom_kernel.h"
#include "qseecomi.h"

#define QSEECOM_DEV			"qseecom"
#define QSEECOM_SCM_EBUSY_WAIT_MS 30
#define QSEECOM_SCM_EBUSY_MAX_RETRY 67

#define PHY_ADDR_4G	(1ULL<<32)
#define QSEECOM_STATE_NOT_READY         0
#define QSEECOM_STATE_SUSPEND           1
#define QSEECOM_STATE_READY             2

#define SGLISTINFO_SET_INDEX_FLAG(c, s, i)	\
	((uint32_t)(((c & 1) << 31) | ((s & 1) << 30) | (i & 0x3fffffff)))

#define SGLISTINFO_TABLE_SIZE	(sizeof(struct sglist_info) * MAX_ION_FD)
#define MAKE_NULL(sgt, attach, dmabuf) do {\
				sgt = NULL;\
				attach = NULL;\
				dmabuf = NULL;\
				} while (0)

static struct class *driver_class;
static dev_t qseecom_device_no;

static DEFINE_MUTEX(app_access_lock);
static DEFINE_MUTEX(listener_access_lock);
static DEFINE_MUTEX(unload_app_pending_list_lock);

struct sglist_info {
	uint32_t indexAndFlags;
	uint32_t sizeOrCount;
};

struct qseecom_registered_listener_list {
	struct list_head                 list;
	struct qseecom_register_listener_req svc;
	void  *user_virt_sb_base;
	struct dma_buf             *dmabuf;
	struct dma_buf_attachment  *attach;
	struct sg_table            *sgt;
	u8                         *sb_virt;
	phys_addr_t                sb_phys;
	size_t                     sb_length;
	wait_queue_head_t          rcv_req_wq;
	int                        rcv_req_flag;
	int                        send_resp_flag;
	bool                       listener_in_use;
	wait_queue_head_t          listener_block_app_wq;
	struct sglist_info         sglistinfo_ptr[MAX_ION_FD];
	uint32_t                   sglist_cnt;
	int                        abort;
	bool                       unregister_pending;
};

struct qseecom_unregister_pending_list {
	struct list_head		list;
	struct qseecom_dev_handle	*data;
};

struct qseecom_registered_app_list {
	struct list_head                 list;
	u32  app_id;
	u32  ref_cnt;
	char app_name[MAX_APP_NAME_SIZE];
	u32  app_arch;
	bool app_blocked;
	u32  check_block;
	u32  blocked_on_listener_id;
};

struct qseecom_registered_kclient_list {
	struct list_head list;
	struct qseecom_handle *handle;
};

struct qseecom_control {
	struct list_head  registered_listener_list_head;
	struct list_head  registered_app_list_head;
	spinlock_t        registered_app_list_lock;
	struct list_head  registered_kclient_list_head;
	spinlock_t        registered_kclient_list_lock;

	wait_queue_head_t send_resp_wq;
	int               send_resp_flag;

	uint32_t          qsee_version;
	struct device *pdev;
	struct device *dev;
	bool  whitelist_support;

	struct cdev cdev;
	uint32_t qsee_reentrancy_support;

	uint32_t app_block_ref_cnt;
	wait_queue_head_t app_block_wq;
	atomic_t qseecom_state;
	bool smcinvoke_support;

	struct list_head  unregister_lsnr_pending_list_head;
	wait_queue_head_t register_lsnr_pending_wq;
	struct task_struct *unregister_lsnr_kthread_task;
	wait_queue_head_t unregister_lsnr_kthread_wq;
	atomic_t unregister_lsnr_kthread_state;

	struct list_head  unload_app_pending_list_head;
	struct task_struct *unload_app_kthread_task;
	wait_queue_head_t unload_app_kthread_wq;
	atomic_t unload_app_kthread_state;
};

struct qseecom_unload_app_pending_list {
	struct list_head		list;
	struct qseecom_dev_handle	*data;
};

struct qseecom_sec_buf_fd_info {
	bool is_sec_buf_fd;
	size_t size;
	void *vbase;
	dma_addr_t pbase;
};

struct qseecom_param_memref {
	uint32_t buffer;
	uint32_t size;
};

struct qseecom_client_handle {
	u32  app_id;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment  *attach;
	struct sg_table *sgt;
	u8 *sb_virt;
	phys_addr_t sb_phys;
	size_t sb_length;
	unsigned long user_virt_sb_base;
	char app_name[MAX_APP_NAME_SIZE];
	u32  app_arch;
	struct qseecom_sec_buf_fd_info sec_buf_fd[MAX_ION_FD];
	bool from_smcinvoke;
	bool unload_pending;
};

struct qseecom_listener_handle {
	u32               id;
	bool              register_pending;
	bool              release_called;
};

static struct qseecom_control qseecom;

enum qseecom_client_handle_type {
	QSEECOM_CLIENT_APP = 1,
	QSEECOM_LISTENER_SERVICE,
	QSEECOM_SECURE_SERVICE,
	QSEECOM_GENERIC,
	QSEECOM_UNAVAILABLE_CLIENT_APP,
};

struct qseecom_dev_handle {
	enum qseecom_client_handle_type type;
	union {
		struct qseecom_client_handle client;
		struct qseecom_listener_handle listener;
	};
	bool released;
	int               abort;
	wait_queue_head_t abort_wq;
	atomic_t          ioctl_count;
	struct sglist_info sglistinfo_ptr[MAX_ION_FD];
	uint32_t sglist_cnt;
	bool use_legacy_cmd;
};

enum qseecom_cache_ops {
	QSEECOM_CACHE_CLEAN,
	QSEECOM_CACHE_INVALIDATE,
};

enum qseecom_listener_unregister_kthread_state {
	LSNR_UNREG_KT_SLEEP = 0,
	LSNR_UNREG_KT_WAKEUP,
};

enum qseecom_unload_app_kthread_state {
	UNLOAD_APP_KT_SLEEP = 0,
	UNLOAD_APP_KT_WAKEUP,
};

static int __qseecom_unload_app(struct qseecom_dev_handle *data, uint32_t app_id);
static void __qseecom_reentrancy_check_if_no_app_blocked(uint32_t smc_id);

static void qsee_dmac_flush_range(void *vaddr, size_t len)
{
	dma_addr_t dma_handle = dma_map_single(qseecom.dev, vaddr, len, DMA_BIDIRECTIONAL);
	if (!dma_mapping_error(qseecom.dev, dma_handle)) {
		dma_unmap_single(qseecom.dev, dma_handle, len, DMA_BIDIRECTIONAL);
	}
}

/* SMC result: the secure call was preempted and must be resumed */
#define QSEECOM_SCM_INTERRUPTED		1

/*
 * Issue a QSEE SMC and resume it while TZ reports it as interrupted, like
 * qcom_scm does: re-issue with function ID QCOM_SCM_INTERRUPTED and the
 * session state TZ returned in x6 (ARM_SMCCC_QUIRK_QCOM_A6). Treating the
 * interrupted status as a result leaves the call pending in TZ, and the
 * next SMC then never returns (one CPU stuck in the secure world).
 */
#define QSEECOM_SCM_REG_ARGS		4	/* x2..x5 */
#define QSEECOM_SCM_FIRST_EXT_ARG	3	/* args[3..] via x5 when > 4 */

static int __qseecom_smc(uint32_t smc_id, struct scm_desc *desc,
			 struct arm_smccc_res *res)
{
	struct arm_smccc_quirk quirk = { .id = ARM_SMCCC_QUIRK_QCOM_A6 };
	unsigned int nargs = desc->arginfo & 0xf;
	unsigned long fn = smc_id, x5 = desc->args[3];
	dma_addr_t ext_dma = DMA_MAPPING_ERROR;
	u64 *ext = NULL;
	size_t ext_len = 0;

	/*
	 * Like scm_call2() / qcom_scm: with more than four arguments, x5
	 * points at a buffer holding args[3..] instead.
	 */
	if (nargs > QSEECOM_SCM_REG_ARGS) {
		unsigned int i, n = ARRAY_SIZE(desc->args) - QSEECOM_SCM_FIRST_EXT_ARG;

		ext_len = n * sizeof(*ext);
		ext = kzalloc(ext_len, GFP_KERNEL);
		if (!ext)
			return -ENOMEM;
		for (i = 0; i < n; i++)
			ext[i] = cpu_to_le64(desc->args[QSEECOM_SCM_FIRST_EXT_ARG + i]);
		ext_dma = dma_map_single(qseecom.dev, ext, ext_len, DMA_TO_DEVICE);
		if (dma_mapping_error(qseecom.dev, ext_dma)) {
			kfree(ext);
			return -ENOMEM;
		}
		x5 = ext_dma;
	}

	quirk.state.a6 = 0;

	do {
		arm_smccc_smc_quirk(fn, desc->arginfo, desc->args[0],
				    desc->args[1], desc->args[2], x5,
				    quirk.state.a6, 0, res, &quirk);

		if (res->a0 == QSEECOM_SCM_INTERRUPTED)
			fn = res->a0;
	} while (res->a0 == QSEECOM_SCM_INTERRUPTED);

	if (ext) {
		dma_unmap_single(qseecom.dev, ext_dma, ext_len, DMA_TO_DEVICE);
		kfree(ext);
	}

	return 0;
}

static int __qseecom_scm_call2_locked(uint32_t smc_id, struct scm_desc *desc)
{
	struct arm_smccc_res res;
	int retry_count = 0;

	do {
		if (__qseecom_smc(smc_id, desc, &res))
			return -ENOMEM;

		desc->ret[0] = res.a0;
		desc->ret[1] = res.a1;
		desc->ret[2] = res.a2;

		if ((int)res.a0 == -EBUSY) {
			mutex_unlock(&app_access_lock);
			msleep(QSEECOM_SCM_EBUSY_WAIT_MS);
			mutex_lock(&app_access_lock);
		}
		if (retry_count == 33)
			pr_warn("secure world has been busy for 1 second!\n");
	} while ((int)res.a0 == -EBUSY &&
			(retry_count++ < QSEECOM_SCM_EBUSY_MAX_RETRY));

	return (int)res.a0;
}

static int qseecom_scm_call2(uint32_t svc_id, uint32_t tz_cmd_id,
			const void *req_buf, void *resp_buf)
{
	int      ret = 0;
	uint32_t smc_id = 0;
	uint32_t qseos_cmd_id = 0;
	struct scm_desc desc = {0};
	struct qseecom_command_scm_resp *scm_resp = NULL;

	if (!req_buf || !resp_buf) {
		pr_err("Invalid buffer pointer\n");
		return -EINVAL;
	}
	qseos_cmd_id = *(uint32_t *)req_buf;
	scm_resp = (struct qseecom_command_scm_resp *)resp_buf;

	switch (svc_id) {
	case 6: {
		if (tz_cmd_id == 3) {
			smc_id = TZ_INFO_GET_FEATURE_VERSION_ID;
			desc.arginfo = TZ_INFO_GET_FEATURE_VERSION_ID_PARAM_ID;
			desc.args[0] = *(uint32_t *)req_buf;
		} else {
			pr_err("Unsupported svc_id %d, tz_cmd_id %d\n", svc_id, tz_cmd_id);
			return -EINVAL;
		}
		ret = __qseecom_scm_call2_locked(smc_id, &desc);
		break;
	}
	case 16: {
		switch (tz_cmd_id) {
		case SCM_SAVE_PARTITION_HASH_ID: {
			u32 tzbuflen = PAGE_ALIGN(SHA256_DIGEST_LENGTH);
			struct qseecom_save_partition_hash_req *p_hash_req =
				(struct qseecom_save_partition_hash_req *)req_buf;
			char *tzbuf = kzalloc(tzbuflen, GFP_KERNEL);

			if (!tzbuf)
				return -ENOMEM;
			memcpy(tzbuf, p_hash_req->digest, SHA256_DIGEST_LENGTH);
			qsee_dmac_flush_range(tzbuf, tzbuflen);
			smc_id = TZ_ES_SAVE_PARTITION_HASH_ID;
			desc.arginfo = TZ_ES_SAVE_PARTITION_HASH_ID_PARAM_ID;
			desc.args[0] = p_hash_req->partition_id;
			desc.args[1] = virt_to_phys(tzbuf);
			desc.args[2] = SHA256_DIGEST_LENGTH;
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			kfree_sensitive(tzbuf);
			break;
		}
		default:
			ret = -EINVAL;
			break;
		}
		break;
	}
	case 0xFC: {
		switch (qseos_cmd_id) {
		case QSEOS_APP_START_COMMAND: {
			struct qseecom_load_app_64bit_ireq *req_64bit =
					(struct qseecom_load_app_64bit_ireq *)req_buf;
			smc_id = TZ_OS_APP_START_ID;
			desc.arginfo = TZ_OS_APP_START_ID_PARAM_ID;
			desc.args[0] = req_64bit->mdt_len;
			desc.args[1] = req_64bit->img_len;
			desc.args[2] = req_64bit->phy_addr;
			__qseecom_reentrancy_check_if_no_app_blocked(smc_id);
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			break;
		}
		case QSEOS_APP_SHUTDOWN_COMMAND: {
			struct qseecom_unload_app_ireq *req = (struct qseecom_unload_app_ireq *)req_buf;
			smc_id = TZ_OS_APP_SHUTDOWN_ID;
			desc.arginfo = TZ_OS_APP_SHUTDOWN_ID_PARAM_ID;
			desc.args[0] = req->app_id;
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			break;
		}
		case QSEOS_APP_LOOKUP_COMMAND: {
			struct qseecom_check_app_ireq *req = (struct qseecom_check_app_ireq *)req_buf;
			u32 tzbuflen = PAGE_ALIGN(sizeof(req->app_name));
			char *tzbuf = kzalloc(tzbuflen, GFP_KERNEL);

			if (!tzbuf)
				return -ENOMEM;
			strscpy(tzbuf, req->app_name, sizeof(req->app_name));
			qsee_dmac_flush_range(tzbuf, tzbuflen);
			smc_id = TZ_OS_APP_LOOKUP_ID;
			desc.arginfo = TZ_OS_APP_LOOKUP_ID_PARAM_ID;
			desc.args[0] = virt_to_phys(tzbuf);
			desc.args[1] = strlen(req->app_name);
			__qseecom_reentrancy_check_if_no_app_blocked(smc_id);
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			kfree_sensitive(tzbuf);
			break;
		}
		case QSEOS_APP_REGION_NOTIFICATION: {
			struct qsee_apps_region_info_64bit_ireq *req_64bit =
				(struct qsee_apps_region_info_64bit_ireq *)req_buf;
			smc_id = TZ_OS_APP_REGION_NOTIFICATION_ID;
			desc.arginfo = TZ_OS_APP_REGION_NOTIFICATION_ID_PARAM_ID;
			desc.args[0] = req_64bit->addr;
			desc.args[1] = req_64bit->size;
			__qseecom_reentrancy_check_if_no_app_blocked(smc_id);
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			break;
		}
		case QSEOS_REGISTER_LISTENER: {
			struct qseecom_register_listener_64bit_ireq *req_64bit =
				(struct qseecom_register_listener_64bit_ireq *)req_buf;
			desc.arginfo = TZ_OS_REGISTER_LISTENER_ID_PARAM_ID;
			desc.args[0] = req_64bit->listener_id;
			desc.args[1] = req_64bit->sb_ptr;
			desc.args[2] = req_64bit->sb_len;
			qseecom.smcinvoke_support = true;
			smc_id = TZ_OS_REGISTER_LISTENER_SMCINVOKE_ID;
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			if (ret == -EIO) {
				qseecom.smcinvoke_support = false;
				smc_id = TZ_OS_REGISTER_LISTENER_ID;
				ret = __qseecom_scm_call2_locked(smc_id, &desc);
			}
			break;
		}
		case QSEOS_DEREGISTER_LISTENER: {
			struct qseecom_unregister_listener_ireq *req =
				(struct qseecom_unregister_listener_ireq *)req_buf;
			smc_id = TZ_OS_DEREGISTER_LISTENER_ID;
			desc.arginfo = TZ_OS_DEREGISTER_LISTENER_ID_PARAM_ID;
			desc.args[0] = req->listener_id;
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			break;
		}
		case QSEOS_LISTENER_DATA_RSP_COMMAND: {
			struct qseecom_client_listener_data_irsp *req =
				(struct qseecom_client_listener_data_irsp *)req_buf;
			smc_id = TZ_OS_LISTENER_RESPONSE_HANDLER_ID;
			desc.arginfo = TZ_OS_LISTENER_RESPONSE_HANDLER_ID_PARAM_ID;
			desc.args[0] = req->listener_id;
			desc.args[1] = req->status;
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			break;
		}
		case QSEOS_LISTENER_DATA_RSP_COMMAND_WHITELIST: {
			struct qseecom_client_listener_data_64bit_irsp *req_64 =
				(struct qseecom_client_listener_data_64bit_irsp *)req_buf;
			smc_id = TZ_OS_LISTENER_RESPONSE_HANDLER_WITH_WHITELIST_ID;
			desc.arginfo = TZ_OS_LISTENER_RESPONSE_HANDLER_WITH_WHITELIST_PARAM_ID;
			desc.args[0] = req_64->listener_id;
			desc.args[1] = req_64->status;
			desc.args[2] = req_64->sglistinfo_ptr;
			desc.args[3] = req_64->sglistinfo_len;
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			break;
		}
		case QSEOS_LOAD_EXTERNAL_ELF_COMMAND: {
			struct qseecom_load_app_64bit_ireq *req_64bit =
				(struct qseecom_load_app_64bit_ireq *)req_buf;
			smc_id = TZ_OS_LOAD_EXTERNAL_IMAGE_ID;
			desc.arginfo = TZ_OS_LOAD_EXTERNAL_IMAGE_ID_PARAM_ID;
			desc.args[0] = req_64bit->mdt_len;
			desc.args[1] = req_64bit->img_len;
			desc.args[2] = req_64bit->phy_addr;
			__qseecom_reentrancy_check_if_no_app_blocked(smc_id);
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			break;
		}
		case QSEOS_UNLOAD_EXTERNAL_ELF_COMMAND: {
			smc_id = TZ_OS_UNLOAD_EXTERNAL_IMAGE_ID;
			desc.arginfo = TZ_OS_UNLOAD_EXTERNAL_IMAGE_ID_PARAM_ID;
			__qseecom_reentrancy_check_if_no_app_blocked(smc_id);
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			break;
		}
		case QSEOS_CLIENT_SEND_DATA_COMMAND: {
			struct qseecom_client_send_data_64bit_ireq *req_64bit =
				(struct qseecom_client_send_data_64bit_ireq *)req_buf;
			smc_id = TZ_APP_QSAPP_SEND_DATA_ID;
			desc.arginfo = TZ_APP_QSAPP_SEND_DATA_ID_PARAM_ID;
			desc.args[0] = req_64bit->app_id;
			desc.args[1] = req_64bit->req_ptr;
			desc.args[2] = req_64bit->req_len;
			desc.args[3] = req_64bit->rsp_ptr;
			desc.args[4] = req_64bit->rsp_len;
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			break;
		}
		case QSEOS_CLIENT_SEND_DATA_COMMAND_WHITELIST: {
			struct qseecom_client_send_data_64bit_ireq *req_64bit =
				(struct qseecom_client_send_data_64bit_ireq *)req_buf;
			smc_id = TZ_APP_QSAPP_SEND_DATA_WITH_WHITELIST_ID;
			desc.arginfo = TZ_APP_QSAPP_SEND_DATA_WITH_WHITELIST_ID_PARAM_ID;
			desc.args[0] = req_64bit->app_id;
			desc.args[1] = req_64bit->req_ptr;
			desc.args[2] = req_64bit->req_len;
			desc.args[3] = req_64bit->rsp_ptr;
			desc.args[4] = req_64bit->rsp_len;
			desc.args[5] = req_64bit->sglistinfo_ptr;
			desc.args[6] = req_64bit->sglistinfo_len;
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			break;
		}
		case QSEOS_TEE_OPEN_SESSION_WHITELIST:
		case QSEOS_TEE_OPEN_SESSION: {
			struct qseecom_qteec_64bit_ireq *req_64bit = (struct qseecom_qteec_64bit_ireq *)req_buf;
			smc_id = (qseos_cmd_id == QSEOS_TEE_OPEN_SESSION) ? 
				TZ_APP_GPAPP_OPEN_SESSION_ID : TZ_APP_GPAPP_OPEN_SESSION_WITH_WHITELIST_ID;
			desc.arginfo = (qseos_cmd_id == QSEOS_TEE_OPEN_SESSION) ? 
				TZ_APP_GPAPP_OPEN_SESSION_ID_PARAM_ID : TZ_APP_GPAPP_OPEN_SESSION_WITH_WHITELIST_ID_PARAM_ID;
			desc.args[0] = req_64bit->app_id;
			desc.args[1] = req_64bit->req_ptr;
			desc.args[2] = req_64bit->req_len;
			desc.args[3] = req_64bit->resp_ptr;
			desc.args[4] = req_64bit->resp_len;
			if (qseos_cmd_id == QSEOS_TEE_OPEN_SESSION_WHITELIST) {
				desc.args[5] = req_64bit->sglistinfo_ptr;
				desc.args[6] = req_64bit->sglistinfo_len;
			}
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			break;
		}
		case QSEOS_TEE_INVOKE_COMMAND_WHITELIST:
		case QSEOS_TEE_INVOKE_COMMAND: {
			struct qseecom_qteec_64bit_ireq *req_64bit = (struct qseecom_qteec_64bit_ireq *)req_buf;
			smc_id = (qseos_cmd_id == QSEOS_TEE_INVOKE_COMMAND) ? 
				TZ_APP_GPAPP_INVOKE_COMMAND_ID : TZ_APP_GPAPP_INVOKE_COMMAND_WITH_WHITELIST_ID;
			desc.arginfo = (qseos_cmd_id == QSEOS_TEE_INVOKE_COMMAND) ? 
				TZ_APP_GPAPP_INVOKE_COMMAND_ID_PARAM_ID : TZ_APP_GPAPP_INVOKE_COMMAND_WITH_WHITELIST_ID_PARAM_ID;
			desc.args[0] = req_64bit->app_id;
			desc.args[1] = req_64bit->req_ptr;
			desc.args[2] = req_64bit->req_len;
			desc.args[3] = req_64bit->resp_ptr;
			desc.args[4] = req_64bit->resp_len;
			if (qseos_cmd_id == QSEOS_TEE_INVOKE_COMMAND_WHITELIST) {
				desc.args[5] = req_64bit->sglistinfo_ptr;
				desc.args[6] = req_64bit->sglistinfo_len;
			}
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			break;
		}
		case QSEOS_TEE_CLOSE_SESSION: {
			struct qseecom_qteec_64bit_ireq *req_64bit = (struct qseecom_qteec_64bit_ireq *)req_buf;
			smc_id = TZ_APP_GPAPP_CLOSE_SESSION_ID;
			desc.arginfo = TZ_APP_GPAPP_CLOSE_SESSION_ID_PARAM_ID;
			desc.args[0] = req_64bit->app_id;
			desc.args[1] = req_64bit->req_ptr;
			desc.args[2] = req_64bit->req_len;
			desc.args[3] = req_64bit->resp_ptr;
			desc.args[4] = req_64bit->resp_len;
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			break;
		}
		case QSEOS_TEE_REQUEST_CANCELLATION: {
			struct qseecom_qteec_64bit_ireq *req_64bit = (struct qseecom_qteec_64bit_ireq *)req_buf;
			smc_id = TZ_APP_GPAPP_REQUEST_CANCELLATION_ID;
			desc.arginfo = TZ_APP_GPAPP_REQUEST_CANCELLATION_ID_PARAM_ID;
			desc.args[0] = req_64bit->app_id;
			desc.args[1] = req_64bit->req_ptr;
			desc.args[2] = req_64bit->req_len;
			desc.args[3] = req_64bit->resp_ptr;
			desc.args[4] = req_64bit->resp_len;
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			break;
		}
		case QSEOS_CONTINUE_BLOCKED_REQ_COMMAND: {
			struct qseecom_continue_blocked_request_ireq *req =
				(struct qseecom_continue_blocked_request_ireq *)req_buf;
			smc_id = qseecom.smcinvoke_support ? 
				TZ_OS_CONTINUE_BLOCKED_REQUEST_SMCINVOKE_ID : TZ_OS_CONTINUE_BLOCKED_REQUEST_ID;
			desc.arginfo = TZ_OS_CONTINUE_BLOCKED_REQUEST_ID_PARAM_ID;
			desc.args[0] = req->app_or_session_id;
			ret = __qseecom_scm_call2_locked(smc_id, &desc);
			break;
		}
		default: {
			pr_err("qseos_cmd_id %d is not supported by armv8 smccc.\n", qseos_cmd_id);
			ret = -EINVAL;
			break;
		}
		}
		break;
	}
	default: {
		pr_err("svc_id 0x%x is not supported by armv8 smccc.\n", svc_id);
		ret = -EINVAL;
		break;
	}
	}
	scm_resp->result = desc.ret[0];
	scm_resp->resp_type = desc.ret[1];
	scm_resp->data = desc.ret[2];
	return ret;
}

static int qseecom_scm_call(u32 svc_id, u32 tz_cmd_id, const void *cmd_buf,
		size_t cmd_len, void *resp_buf, size_t resp_len)
{
	return qseecom_scm_call2(svc_id, tz_cmd_id, cmd_buf, resp_buf);
}

static struct qseecom_registered_listener_list *__qseecom_find_svc(int32_t listener_id)
{
	struct qseecom_registered_listener_list *entry = NULL;
	list_for_each_entry(entry, &qseecom.registered_listener_list_head, list) {
		if (entry->svc.listener_id == listener_id)
			break;
	}
	if ((entry != NULL) && (entry->svc.listener_id != listener_id))
		return NULL;
	return entry;
}

static int qseecom_dmabuf_cache_operations(struct dma_buf *dmabuf,
					enum qseecom_cache_ops cache_op)
{
	if (!dmabuf)
		return -EINVAL;

	switch (cache_op) {
	case QSEECOM_CACHE_CLEAN:
		dma_buf_begin_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
		dma_buf_end_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
		break;
	case QSEECOM_CACHE_INVALIDATE:
		dma_buf_begin_cpu_access(dmabuf, DMA_TO_DEVICE);
		dma_buf_end_cpu_access(dmabuf, DMA_FROM_DEVICE);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int qseecom_dmabuf_map(int ion_fd, struct sg_table **sgt,
				struct dma_buf_attachment **attach,
				struct dma_buf **dmabuf)
{
	struct dma_buf *new_dma_buf = NULL;
	struct dma_buf_attachment *new_attach = NULL;
	struct sg_table *new_sgt = NULL;
	int ret = 0;

	new_dma_buf = dma_buf_get(ion_fd);
	if (IS_ERR_OR_NULL(new_dma_buf)) {
		return -ENOMEM;
	}

	new_attach = dma_buf_attach(new_dma_buf, qseecom.dev);
	if (IS_ERR_OR_NULL(new_attach)) {
		ret = -ENOMEM;
		goto err_put;
	}

	new_sgt = dma_buf_map_attachment(new_attach, DMA_BIDIRECTIONAL);
	if (IS_ERR_OR_NULL(new_sgt)) {
		ret = PTR_ERR(new_sgt);
		goto err_detach;
	}
	*sgt = new_sgt;
	*attach = new_attach;
	*dmabuf = new_dma_buf;
	return ret;

err_detach:
	dma_buf_detach(new_dma_buf, new_attach);
err_put:
	dma_buf_put(new_dma_buf);
	return ret;
}

static void qseecom_dmabuf_unmap(struct sg_table *sgt,
			struct dma_buf_attachment *attach,
			struct dma_buf *dmabuf)
{
	dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
	dma_buf_detach(dmabuf, attach);
	dma_buf_put(dmabuf);
}

static int qseecom_vaddr_map(int ion_fd, phys_addr_t *paddr, void **vaddr,
			struct sg_table **sgt, struct dma_buf_attachment **attach,
			size_t *sb_length, struct dma_buf **dmabuf)
{
	struct dma_buf *new_dma_buf = NULL;
	struct dma_buf_attachment *new_attach = NULL;
	struct sg_table *new_sgt = NULL;
	struct iosys_map map;
	int ret = 0;

	ret = qseecom_dmabuf_map(ion_fd, &new_sgt, &new_attach, &new_dma_buf);
	if (ret)
		return ret;

	*paddr = sg_dma_address(new_sgt->sgl);
	*sb_length = new_sgt->sgl->length;

	dma_buf_begin_cpu_access(new_dma_buf, DMA_BIDIRECTIONAL);
	ret = dma_buf_vmap(new_dma_buf, &map);
	if (ret < 0) {
		goto err_unmap;
	}

	*dmabuf = new_dma_buf;
	*attach = new_attach;
	*sgt = new_sgt;
	*vaddr = map.vaddr;
	return 0;

err_unmap:
	dma_buf_end_cpu_access(new_dma_buf, DMA_BIDIRECTIONAL);
	qseecom_dmabuf_unmap(new_sgt, new_attach, new_dma_buf);
	MAKE_NULL(*sgt, *attach, *dmabuf);
	return ret;
}

static void qseecom_vaddr_unmap(void *vaddr, struct sg_table *sgt,
		struct dma_buf_attachment *attach,
		struct dma_buf *dmabuf)
{
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(vaddr);
	dma_buf_vunmap(dmabuf, &map);
	dma_buf_end_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
	qseecom_dmabuf_unmap(sgt, attach, dmabuf);
}

static int __qseecom_set_sb_memory(struct qseecom_registered_listener_list *svc,
				struct qseecom_dev_handle *handle,
				struct qseecom_register_listener_req *listener)
{
	int ret = 0;
	struct qseecom_register_listener_64bit_ireq req_64bit;
	struct qseecom_command_scm_resp resp;
	void *cmd_buf = NULL;
	size_t cmd_len;

	ret = qseecom_vaddr_map(listener->ifd_data_fd,
				&svc->sb_phys, (void **)&svc->sb_virt,
				&svc->sgt, &svc->attach,
				&svc->sb_length, &svc->dmabuf);
	if (ret) return -EINVAL;

	req_64bit.qsee_cmd_id = QSEOS_REGISTER_LISTENER;
	req_64bit.listener_id = svc->svc.listener_id;
	req_64bit.sb_len = svc->sb_length;
	req_64bit.sb_ptr = (uint64_t)svc->sb_phys;
	cmd_buf = (void *)&req_64bit;
	cmd_len = sizeof(struct qseecom_register_listener_64bit_ireq);
	resp.result = QSEOS_RESULT_INCOMPLETE;

	mutex_unlock(&listener_access_lock);
	mutex_lock(&app_access_lock);
	__qseecom_reentrancy_check_if_no_app_blocked(TZ_OS_REGISTER_LISTENER_SMCINVOKE_ID);
	ret = qseecom_scm_call(0xFC, 1, cmd_buf, cmd_len, &resp, sizeof(resp));
	mutex_unlock(&app_access_lock);
	mutex_lock(&listener_access_lock);
	if (ret || resp.result != QSEOS_RESULT_SUCCESS) {
		if (svc->dmabuf) {
			qseecom_vaddr_unmap(svc->sb_virt, svc->sgt, svc->attach, svc->dmabuf);
			MAKE_NULL(svc->sgt, svc->attach, svc->dmabuf);
		}
		return -EPERM;
	}
	return 0;
}

static int qseecom_register_listener(struct qseecom_dev_handle *data, void __user *argp)
{
	int ret = 0;
	struct qseecom_register_listener_req rcvd_lstnr;
	struct qseecom_registered_listener_list *new_entry;
	struct qseecom_registered_listener_list *ptr_svc;

	if (data->listener.register_pending) return -EINVAL;

	ret = copy_from_user(&rcvd_lstnr, argp, sizeof(rcvd_lstnr));
	if (ret) return ret;
	
	if (!access_ok((void __user *)rcvd_lstnr.virt_sb_base, rcvd_lstnr.sb_size))
		return -EFAULT;

	ptr_svc = __qseecom_find_svc(data->listener.id);
	if (ptr_svc) return -EINVAL;

	ptr_svc = __qseecom_find_svc(rcvd_lstnr.listener_id);
	if (ptr_svc) {
		if (ptr_svc->unregister_pending == false) {
			data->released = true;
			return -EBUSY;
		} else {
			mutex_unlock(&listener_access_lock);
			ret = wait_event_interruptible(qseecom.register_lsnr_pending_wq,
				list_empty(&qseecom.unregister_lsnr_pending_list_head));
			if (ret) {
				mutex_lock(&listener_access_lock);
				return -ERESTARTSYS;
			}
			mutex_lock(&listener_access_lock);
		}
	}
	new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
	if (!new_entry) return -ENOMEM;
	memcpy(&new_entry->svc, &rcvd_lstnr, sizeof(rcvd_lstnr));
	new_entry->rcv_req_flag = 0;

	new_entry->svc.listener_id = rcvd_lstnr.listener_id;
	new_entry->sb_length = rcvd_lstnr.sb_size;
	new_entry->user_virt_sb_base = rcvd_lstnr.virt_sb_base;
	data->listener.register_pending = true;
	if (__qseecom_set_sb_memory(new_entry, data, &rcvd_lstnr)) {
		kfree_sensitive(new_entry);
		data->listener.register_pending = false;
		return -ENOMEM;
	}
	data->listener.register_pending = false;

	init_waitqueue_head(&new_entry->rcv_req_wq);
	init_waitqueue_head(&new_entry->listener_block_app_wq);
	new_entry->send_resp_flag = 0;
	new_entry->listener_in_use = false;
	list_add_tail(&new_entry->list, &qseecom.registered_listener_list_head);

	data->listener.id = rcvd_lstnr.listener_id;
	return ret;
}

static int __qseecom_unregister_listener(struct qseecom_dev_handle *data,
			struct qseecom_registered_listener_list *ptr_svc)
{
	int ret = 0;
	struct qseecom_unregister_listener_ireq req;
	struct qseecom_command_scm_resp resp;

	req.qsee_cmd_id = QSEOS_DEREGISTER_LISTENER;
	req.listener_id = data->listener.id;
	resp.result = QSEOS_RESULT_INCOMPLETE;

	mutex_unlock(&listener_access_lock);
	mutex_lock(&app_access_lock);
	__qseecom_reentrancy_check_if_no_app_blocked(TZ_OS_DEREGISTER_LISTENER_ID);
	ret = qseecom_scm_call(0xFC, 1, &req, sizeof(req), &resp, sizeof(resp));
	mutex_unlock(&app_access_lock);
	mutex_lock(&listener_access_lock);
	
	if (ret || resp.result != QSEOS_RESULT_SUCCESS) {
		if (ret == -EBUSY) return ret;
		goto exit;
	}

	while (atomic_read(&data->ioctl_count) > 1) {
		if (wait_event_interruptible(data->abort_wq, atomic_read(&data->ioctl_count) <= 1)) {
			ret = -ERESTARTSYS;
		}
	}

exit:
	if (ptr_svc->dmabuf) {
		qseecom_vaddr_unmap(ptr_svc->sb_virt, ptr_svc->sgt, ptr_svc->attach, ptr_svc->dmabuf);
		MAKE_NULL(ptr_svc->sgt, ptr_svc->attach, ptr_svc->dmabuf);
	}
	list_del(&ptr_svc->list);
	kfree_sensitive(ptr_svc);
	data->released = true;
	return ret;
}

static int qseecom_unregister_listener(struct qseecom_dev_handle *data)
{
	struct qseecom_registered_listener_list *ptr_svc = NULL;
	struct qseecom_unregister_pending_list *entry = NULL;

	if (data->released) return -EINVAL;

	ptr_svc = __qseecom_find_svc(data->listener.id);
	if (!ptr_svc) return -ENODATA;

	ptr_svc->abort = 1;
	wake_up_interruptible_all(&qseecom.send_resp_wq);
	data->abort = 1;
	wake_up_all(&ptr_svc->rcv_req_wq);

	if (ptr_svc->unregister_pending) return 0;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) return -ENOMEM;
	entry->data = data;
	list_add_tail(&entry->list, &qseecom.unregister_lsnr_pending_list_head);
	ptr_svc->unregister_pending = true;
	return 0;
}

static void __qseecom_processing_pending_lsnr_unregister(void)
{
	struct qseecom_unregister_pending_list *entry = NULL;
	struct qseecom_registered_listener_list *ptr_svc = NULL;
	struct list_head *pos;

	mutex_lock(&listener_access_lock);
	while (!list_empty(&qseecom.unregister_lsnr_pending_list_head)) {
		pos = qseecom.unregister_lsnr_pending_list_head.next;
		entry = list_entry(pos, struct qseecom_unregister_pending_list, list);
		if (entry && entry->data) {
			if (!entry->data->listener.release_called) break;
			ptr_svc = __qseecom_find_svc(entry->data->listener.id);
			if (ptr_svc) {
				if (__qseecom_unregister_listener(entry->data, ptr_svc) == -EBUSY) {
					mutex_unlock(&listener_access_lock);
					return;
				}
			}
			kfree_sensitive(entry->data);
		}
		list_del(pos);
		kfree_sensitive(entry);
	}
	mutex_unlock(&listener_access_lock);
	wake_up_interruptible(&qseecom.register_lsnr_pending_wq);
}

static int __qseecom_unregister_listener_kthread_func(void *data)
{
	while (!kthread_should_stop()) {
		wait_event_interruptible(qseecom.unregister_lsnr_kthread_wq,
			atomic_read(&qseecom.unregister_lsnr_kthread_state) == LSNR_UNREG_KT_WAKEUP);
		__qseecom_processing_pending_lsnr_unregister();
		atomic_set(&qseecom.unregister_lsnr_kthread_state, LSNR_UNREG_KT_SLEEP);
	}
	return 0;
}

static int qseecom_set_client_mem_param(struct qseecom_dev_handle *data, void __user *argp)
{
	int32_t ret;
	struct qseecom_set_sb_mem_param_req req;
	size_t len;

	if (copy_from_user(&req, (void __user *)argp, sizeof(req)))
		return -EFAULT;

	if ((req.ifd_data_fd <= 0) || (req.virt_sb_base == NULL) || (req.sb_len == 0))
		return -EFAULT;
	if (!access_ok((void __user *)req.virt_sb_base, req.sb_len))
		return -EFAULT;

	ret = qseecom_vaddr_map(req.ifd_data_fd, &data->client.sb_phys,
				(void **)&data->client.sb_virt, &data->client.sgt, 
				&data->client.attach, &len, &data->client.dmabuf);
	if (ret) return -EINVAL;

	if (len < req.sb_len) {
		ret = -EINVAL;
		goto exit;
	}
	data->client.sb_length = req.sb_len;
	data->client.user_virt_sb_base = (uintptr_t)req.virt_sb_base;
	return ret;

exit:
	if (data->client.dmabuf) {
		qseecom_vaddr_unmap(data->client.sb_virt, data->client.sgt,
			 data->client.attach, data->client.dmabuf);
		MAKE_NULL(data->client.sgt, data->client.attach, data->client.dmabuf);
	}
	return ret;
}

static int __qseecom_listener_has_sent_rsp(struct qseecom_dev_handle *data,
			struct qseecom_registered_listener_list *ptr_svc)
{
	int ret = (qseecom.send_resp_flag != 0);
	return ret || data->abort || ptr_svc->abort;
}

static int __qseecom_reentrancy_listener_has_sent_rsp(struct qseecom_dev_handle *data,
			struct qseecom_registered_listener_list *ptr_svc)
{
	int ret = (ptr_svc->send_resp_flag != 0);
	return ret || data->abort || ptr_svc->abort;
}

static void __qseecom_clean_listener_sglistinfo(struct qseecom_registered_listener_list *ptr_svc)
{
	if (ptr_svc->sglist_cnt) {
		memset(ptr_svc->sglistinfo_ptr, 0, SGLISTINFO_TABLE_SIZE);
		ptr_svc->sglist_cnt = 0;
	}
}

static int __qseecom_process_incomplete_cmd(struct qseecom_dev_handle *data,
					struct qseecom_command_scm_resp *resp)
{
	int ret = 0, rc = 0;
	uint32_t lstnr, status;
	struct qseecom_client_listener_data_64bit_irsp send_data_rsp_64bit = {0};
	struct qseecom_registered_listener_list *ptr_svc = NULL;
	sigset_t new_sigset, old_sigset;
	void *cmd_buf = NULL;
	size_t cmd_len;
	struct sglist_info *table = NULL;

	qseecom.app_block_ref_cnt++;
	while (resp->result == QSEOS_RESULT_INCOMPLETE) {
		lstnr = resp->data;
		mutex_lock(&listener_access_lock);
		list_for_each_entry(ptr_svc, &qseecom.registered_listener_list_head, list) {
			if (ptr_svc->svc.listener_id == lstnr) {
				ptr_svc->listener_in_use = true;
				ptr_svc->rcv_req_flag = 1;
				ret = qseecom_dmabuf_cache_operations(ptr_svc->dmabuf, QSEECOM_CACHE_INVALIDATE);
				if (ret) {
					rc = -EINVAL;
					status = QSEOS_RESULT_FAILURE;
					goto err_resp;
				}
				wake_up_interruptible(&ptr_svc->rcv_req_wq);
				break;
			}
		}

		if (ptr_svc == NULL || !ptr_svc->dmabuf || ptr_svc->svc.listener_id != lstnr || ptr_svc->abort == 1) {
			rc = -EINVAL;
			status = QSEOS_RESULT_FAILURE;
			goto err_resp;
		}

		sigfillset(&new_sigset);
		sigprocmask(SIG_SETMASK, &new_sigset, &old_sigset);
		mutex_unlock(&listener_access_lock);

		do {
			if (!qseecom.qsee_reentrancy_support &&
				!wait_event_interruptible(qseecom.send_resp_wq,
				__qseecom_listener_has_sent_rsp(data, ptr_svc))) {
				break;
			}
			if (qseecom.qsee_reentrancy_support &&
				!wait_event_interruptible(qseecom.send_resp_wq,
				__qseecom_reentrancy_listener_has_sent_rsp(data, ptr_svc))) {
				break;
			}
		} while (1);

		mutex_lock(&listener_access_lock);
		sigprocmask(SIG_SETMASK, &old_sigset, NULL);

		if (data->abort || ptr_svc->abort) {
			rc = -ENODEV;
			status = QSEOS_RESULT_FAILURE;
		} else {
			status = QSEOS_RESULT_SUCCESS;
		}

err_resp:
		qseecom.send_resp_flag = 0;
		if (ptr_svc) {
			ptr_svc->send_resp_flag = 0;
			table = ptr_svc->sglistinfo_ptr;
		}
		
		send_data_rsp_64bit.listener_id  = lstnr;
		send_data_rsp_64bit.status = status;
		if (table) {
			send_data_rsp_64bit.sglistinfo_ptr = virt_to_phys(table);
			send_data_rsp_64bit.sglistinfo_len = SGLISTINFO_TABLE_SIZE;
			qsee_dmac_flush_range((void *)table, SGLISTINFO_TABLE_SIZE);
		}
		cmd_buf = (void *)&send_data_rsp_64bit;
		cmd_len = sizeof(send_data_rsp_64bit);
		
		*(uint32_t *)cmd_buf = (qseecom.whitelist_support == false || table == NULL) ? 
				QSEOS_LISTENER_DATA_RSP_COMMAND : QSEOS_LISTENER_DATA_RSP_COMMAND_WHITELIST;

		if (ptr_svc) {
			ret = qseecom_dmabuf_cache_operations(ptr_svc->dmabuf, QSEECOM_CACHE_CLEAN);
			if (ret) goto exit;

			ret = qseecom_scm_call(0xFC, 1, cmd_buf, cmd_len, resp, sizeof(*resp));
			ptr_svc->listener_in_use = false;
			__qseecom_clean_listener_sglistinfo(ptr_svc);
			if (ret) goto exit;

			ret = qseecom_dmabuf_cache_operations(ptr_svc->dmabuf, QSEECOM_CACHE_INVALIDATE);
			if (ret) goto exit;
		} else {
			ret = qseecom_scm_call(0xFC, 1, cmd_buf, cmd_len, resp, sizeof(*resp));
			if (ret) goto exit;
		}

		if (resp->result != QSEOS_RESULT_SUCCESS && resp->result != QSEOS_RESULT_INCOMPLETE)
			ret = -EINVAL;
exit:
		mutex_unlock(&listener_access_lock);
	}
	qseecom.app_block_ref_cnt--;
	wake_up_interruptible_all(&qseecom.app_block_wq);
	return rc ? rc : ret;
}

static void __qseecom_reentrancy_check_if_no_app_blocked(uint32_t smc_id)
{
	if (qseecom.qsee_reentrancy_support > 0 &&
		qseecom.qsee_reentrancy_support < 3 &&
		(((smc_id & 0x3f000000) >> 24) >= 50 && ((smc_id & 0x3f000000) >> 24) <= 63)) {
		while (qseecom.app_block_ref_cnt > 0) {
			mutex_unlock(&app_access_lock);
			wait_event_interruptible(qseecom.app_block_wq, (!qseecom.app_block_ref_cnt));
			mutex_lock(&app_access_lock);
		}
	}
}

static void __qseecom_reentrancy_check_if_this_app_blocked(struct qseecom_registered_app_list *ptr_app)
{
	if (qseecom.qsee_reentrancy_support) {
		ptr_app->check_block++;
		while (ptr_app->app_blocked || qseecom.app_block_ref_cnt > 1) {
			mutex_unlock(&app_access_lock);
			wait_event_interruptible(qseecom.app_block_wq, (!ptr_app->app_blocked && qseecom.app_block_ref_cnt <= 1));
			mutex_lock(&app_access_lock);
		}
		ptr_app->check_block--;
	}
}

static int __qseecom_check_app_exists(struct qseecom_check_app_ireq *req, uint32_t *app_id)
{
	int32_t ret;
	struct qseecom_command_scm_resp resp;
	bool found_app = false;
	struct qseecom_registered_app_list *entry = NULL;
	unsigned long flags = 0;

	*app_id = 0;
	spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
	list_for_each_entry(entry, &qseecom.registered_app_list_head, list) {
		if (!strcmp(entry->app_name, req->app_name)) {
			found_app = true;
			break;
		}
	}
	spin_unlock_irqrestore(&qseecom.registered_app_list_lock, flags);
	if (found_app) {
		*app_id = entry->app_id;
		return 0;
	}

	memset((void *)&resp, 0, sizeof(resp));
	ret = qseecom_scm_call(0xFC, 1, req, sizeof(struct qseecom_check_app_ireq), &resp, sizeof(resp));
	if (ret) return -EINVAL;
	if (resp.result == QSEOS_RESULT_FAILURE) return 0;
	if (resp.resp_type == QSEOS_APP_ID) {
		*app_id = resp.data;
		return 0;
	}
	return -ENODEV;
}

static int qseecom_load_app(struct qseecom_dev_handle *data, void __user *argp)
{
	struct qseecom_registered_app_list *entry = NULL;
	unsigned long flags = 0;
	u32 app_id = 0;
	struct qseecom_load_img_req load_img_req;
	int32_t ret = 0;
	phys_addr_t pa = 0;
	void *vaddr = NULL;
	struct dma_buf_attachment *attach = NULL;
	struct dma_buf *dmabuf = NULL;
	struct sg_table *sgt = NULL;
	size_t len;
	struct qseecom_command_scm_resp resp;
	struct qseecom_check_app_ireq *req = NULL;
	struct qseecom_load_app_64bit_ireq load_req_64bit;
	void *cmd_buf = NULL;
	size_t cmd_len;
	bool first_time = false;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req) return -ENOMEM;

	if (copy_from_user(&load_img_req, (void __user *)argp, sizeof(struct qseecom_load_img_req))) {
		ret = -EFAULT;
		goto req_free;
	}

	req->qsee_cmd_id = QSEOS_APP_LOOKUP_COMMAND;
	load_img_req.img_name[MAX_APP_NAME_SIZE-1] = '\0';
	strscpy(req->app_name, load_img_req.img_name, MAX_APP_NAME_SIZE);

	ret = __qseecom_check_app_exists(req, &app_id);
	if (ret < 0) goto loadapp_err;

	if (app_id) {
		spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
		list_for_each_entry(entry, &qseecom.registered_app_list_head, list){
			if (entry->app_id == app_id) {
				if (entry->ref_cnt == U32_MAX) {
					ret = -EINVAL;
					goto loadapp_err;
				}
				entry->ref_cnt++;
				break;
			}
		}
		spin_unlock_irqrestore(&qseecom.registered_app_list_lock, flags);
		ret = 0;
	} else {
		first_time = true;
		ret = qseecom_vaddr_map(load_img_req.ifd_data_fd, &pa, &vaddr, &sgt, &attach, &len, &dmabuf);
		if (ret) goto loadapp_err;

		if (load_img_req.mdt_len > len || load_img_req.img_len > len) {
			ret = -EINVAL;
			goto loadapp_err;
		}

		load_req_64bit.qsee_cmd_id = QSEOS_APP_START_COMMAND;
		load_req_64bit.mdt_len = load_img_req.mdt_len;
		load_req_64bit.img_len = load_img_req.img_len;
		strscpy(load_req_64bit.app_name, load_img_req.img_name, MAX_APP_NAME_SIZE);
		load_req_64bit.phy_addr = (uint64_t)pa;
		cmd_buf = (void *)&load_req_64bit;
		cmd_len = sizeof(struct qseecom_load_app_64bit_ireq);

		ret = qseecom_dmabuf_cache_operations(dmabuf, QSEECOM_CACHE_CLEAN);
		if (ret) goto loadapp_err;

		ret = qseecom_scm_call(0xFC, 1, cmd_buf, cmd_len, &resp, sizeof(resp));
		if (ret) goto loadapp_err;

		ret = qseecom_dmabuf_cache_operations(dmabuf, QSEECOM_CACHE_INVALIDATE);
		if (ret) goto loadapp_err;

		if (resp.result == QSEOS_RESULT_FAILURE) {
			ret = -EFAULT;
			goto loadapp_err;
		}

		if (resp.result == QSEOS_RESULT_INCOMPLETE) {
			ret = __qseecom_process_incomplete_cmd(data, &resp);
			if (ret) {
				__qseecom_unload_app(data, resp.data);
				ret = -EFAULT;
				goto loadapp_err;
			}
		}

		if (resp.result != QSEOS_RESULT_SUCCESS) {
			ret = -EFAULT;
			goto loadapp_err;
		}

		app_id = resp.data;
		entry = kmalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry) {
			ret = -ENOMEM;
			goto loadapp_err;
		}
		entry->app_id = app_id;
		entry->ref_cnt = 1;
		entry->app_arch = load_img_req.app_arch;
		strscpy(entry->app_name, load_img_req.img_name, MAX_APP_NAME_SIZE);
		entry->app_blocked = false;
		entry->blocked_on_listener_id = 0;
		entry->check_block = 0;

		spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
		list_add_tail(&entry->list, &qseecom.registered_app_list_head);
		spin_unlock_irqrestore(&qseecom.registered_app_list_lock, flags);
	}
	data->client.app_id = app_id;
	data->client.app_arch = load_img_req.app_arch;
	strscpy(data->client.app_name, load_img_req.img_name, MAX_APP_NAME_SIZE);
	load_img_req.app_id = app_id;

	if (copy_to_user(argp, &load_img_req, sizeof(load_img_req))) {
		ret = -EFAULT;
		if (first_time == true) {
			spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
			list_del(&entry->list);
			spin_unlock_irqrestore(&qseecom.registered_app_list_lock, flags);
			kfree_sensitive(entry);
		}
	}

loadapp_err:
	if (dmabuf) {
		qseecom_vaddr_unmap(vaddr, sgt, attach, dmabuf);
		MAKE_NULL(sgt, attach, dmabuf);
	}
req_free:
	kfree_sensitive(req);
	return ret;
}

/*
 * QSEECOM_IOCTL_APP_LOADED_QUERY_REQ: ask TZ whether an app is loaded and
 * attach this handle to it. Returns -EEXIST (with app_id filled in) when it
 * is, 0 when it is not. Ported from downstream.
 */
static int qseecom_query_app_loaded(struct qseecom_dev_handle *data,
				    void __user *argp)
{
	struct qseecom_qseos_app_load_query *query_req;
	struct qseecom_check_app_ireq *req;
	struct qseecom_registered_app_list *entry;
	unsigned long flags = 0;
	uint32_t app_arch = 0, app_id = 0;
	bool found_app = false;
	int32_t ret = 0;

	query_req = kzalloc(sizeof(*query_req), GFP_KERNEL);
	if (!query_req)
		return -ENOMEM;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto query_req_exit;
	}

	if (copy_from_user(query_req, argp, sizeof(*query_req))) {
		ret = -EFAULT;
		goto exit_free;
	}

	req->qsee_cmd_id = QSEOS_APP_LOOKUP_COMMAND;
	query_req->app_name[MAX_APP_NAME_SIZE - 1] = '\0';
	strscpy(req->app_name, query_req->app_name, MAX_APP_NAME_SIZE);

	ret = __qseecom_check_app_exists(req, &app_id);
	if (ret) {
		pr_err("scm call to check if app is loaded failed\n");
		goto exit_free;
	}

	if (!app_id)
		goto exit_free;	/* not loaded */

	spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
	list_for_each_entry(entry, &qseecom.registered_app_list_head, list) {
		if (entry->app_id == app_id) {
			app_arch = entry->app_arch;
			if (entry->ref_cnt == U32_MAX) {
				spin_unlock_irqrestore(&qseecom.registered_app_list_lock,
						       flags);
				ret = -EINVAL;
				goto exit_free;
			}
			entry->ref_cnt++;
			found_app = true;
			break;
		}
	}
	spin_unlock_irqrestore(&qseecom.registered_app_list_lock, flags);

	data->client.app_id = app_id;
	data->client.app_arch = app_arch;
	query_req->app_id = app_id;
	query_req->app_arch = app_arch;
	strscpy(data->client.app_name, query_req->app_name, MAX_APP_NAME_SIZE);

	/* Loaded earlier (e.g. by the bootloader) but not registered yet */
	if (!found_app) {
		entry = kzalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry) {
			ret = -ENOMEM;
			goto exit_free;
		}
		entry->app_id = app_id;
		entry->ref_cnt = 1;
		entry->app_arch = data->client.app_arch;
		strscpy(entry->app_name, data->client.app_name, MAX_APP_NAME_SIZE);
		spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
		list_add_tail(&entry->list, &qseecom.registered_app_list_head);
		spin_unlock_irqrestore(&qseecom.registered_app_list_lock, flags);
	}

	if (copy_to_user(argp, query_req, sizeof(*query_req))) {
		ret = -EFAULT;
		goto exit_free;
	}
	ret = -EEXIST;	/* app already loaded */

exit_free:
	kfree(req);
query_req_exit:
	kfree(query_req);

	return ret;
}

static int __qseecom_cleanup_app(struct qseecom_dev_handle *data)
{
	int ret = 1;
	wake_up_all(&qseecom.send_resp_wq);
	if (qseecom.qsee_reentrancy_support)
		mutex_unlock(&app_access_lock);
	while (atomic_read(&data->ioctl_count) > 1) {
		if (wait_event_interruptible(data->abort_wq, atomic_read(&data->ioctl_count) <= 1)) {
			ret = -ERESTARTSYS;
			break;
		}
	}
	if (qseecom.qsee_reentrancy_support)
		mutex_lock(&app_access_lock);
	return ret;
}

static int __qseecom_unload_app(struct qseecom_dev_handle *data, uint32_t app_id)
{
	struct qseecom_unload_app_ireq req;
	struct qseecom_command_scm_resp resp;
	int ret = 0;

	req.qsee_cmd_id = QSEOS_APP_SHUTDOWN_COMMAND;
	req.app_id = app_id;

	ret = qseecom_scm_call(0xFC, 1, &req, sizeof(struct qseecom_unload_app_ireq), &resp, sizeof(resp));
	if (ret) return ret;

	if (resp.result == QSEOS_RESULT_INCOMPLETE) {
		ret = __qseecom_process_incomplete_cmd(data, &resp);
	} else if (resp.result != QSEOS_RESULT_SUCCESS) {
		ret = -EFAULT;
	}
	return ret;
}

static int qseecom_unload_app(struct qseecom_dev_handle *data, bool app_crash)
{
	unsigned long flags;
	int ret = 0;
	struct qseecom_registered_app_list *ptr_app = NULL;
	bool found_app = false;

	if (!data) return -EINVAL;

	__qseecom_cleanup_app(data);
	__qseecom_reentrancy_check_if_no_app_blocked(TZ_OS_APP_SHUTDOWN_ID);

	if (!data->client.app_id) goto unload_exit;

	spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
	list_for_each_entry(ptr_app, &qseecom.registered_app_list_head, list) {
		if ((ptr_app->app_id == data->client.app_id) && (!strcmp(ptr_app->app_name, data->client.app_name))) {
			ptr_app->ref_cnt--;
			found_app = true;
			break;
		}
	}
	spin_unlock_irqrestore(&qseecom.registered_app_list_lock, flags);
	if (!found_app) {
		ret = -EINVAL;
		goto unload_exit;
	}

	if (!ptr_app->ref_cnt) {
		ret = __qseecom_unload_app(data, data->client.app_id);
		if (ret == -EBUSY) {
			ptr_app->ref_cnt++;
			return ret;
		}
		spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
		list_del(&ptr_app->list);
		spin_unlock_irqrestore(&qseecom.registered_app_list_lock, flags);
		kfree_sensitive(ptr_app);
	}

unload_exit:
	if (data->client.dmabuf) {
		qseecom_vaddr_unmap(data->client.sb_virt, data->client.sgt, data->client.attach, data->client.dmabuf);
		MAKE_NULL(data->client.sgt, data->client.attach, data->client.dmabuf);
	}
	data->released = true;
	return ret;
}

static int qseecom_prepare_unload_app(struct qseecom_dev_handle *data)
{
	struct qseecom_unload_app_pending_list *entry = NULL;
	if (data->client.unload_pending) return 0;
	
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) return -ENOMEM;
	entry->data = data;
	list_add_tail(&entry->list, &qseecom.unload_app_pending_list_head);
	data->client.unload_pending = true;
	return 0;
}

static void __qseecom_processing_pending_unload_app(void)
{
	struct qseecom_unload_app_pending_list *entry = NULL;
	struct list_head *pos;

	mutex_lock(&unload_app_pending_list_lock);
	while (!list_empty(&qseecom.unload_app_pending_list_head)) {
		pos = qseecom.unload_app_pending_list_head.next;
		entry = list_entry(pos, struct qseecom_unload_app_pending_list, list);
		if (entry && entry->data) {
			mutex_unlock(&unload_app_pending_list_lock);
			mutex_lock(&app_access_lock);
			qseecom_unload_app(entry->data, true);
			mutex_unlock(&app_access_lock);
			mutex_lock(&unload_app_pending_list_lock);
			kfree_sensitive(entry->data);
		}
		list_del(pos);
		kfree_sensitive(entry);
	}
	mutex_unlock(&unload_app_pending_list_lock);
}

static int __qseecom_unload_app_kthread_func(void *data)
{
	while (!kthread_should_stop()) {
		wait_event_interruptible(qseecom.unload_app_kthread_wq,
			atomic_read(&qseecom.unload_app_kthread_state) == UNLOAD_APP_KT_WAKEUP);
		__qseecom_processing_pending_unload_app();
		atomic_set(&qseecom.unload_app_kthread_state, UNLOAD_APP_KT_SLEEP);
	}
	return 0;
}

static phys_addr_t __qseecom_uvirt_to_kphys(struct qseecom_dev_handle *data, unsigned long virt)
{
	return data->client.sb_phys + (virt - data->client.user_virt_sb_base);
}

static int __validate_send_cmd_inputs(struct qseecom_dev_handle *data, struct qseecom_send_cmd_req *req)
{
	if (!data || !data->client.sb_virt) return -EINVAL;
	if (((req->resp_buf == NULL) && (req->resp_len != 0)) || (req->cmd_req_buf == NULL)) return -EINVAL;
	
	if (((uintptr_t)req->cmd_req_buf < data->client.user_virt_sb_base) ||
		((uintptr_t)req->cmd_req_buf >= (data->client.user_virt_sb_base + data->client.sb_length)))
		return -EINVAL;
	if (((uintptr_t)req->resp_buf < data->client.user_virt_sb_base) ||
		((uintptr_t)req->resp_buf >= (data->client.user_virt_sb_base + data->client.sb_length)))
		return -EINVAL;
	if ((req->cmd_req_len == 0) || (req->cmd_req_len > data->client.sb_length) || (req->resp_len > data->client.sb_length))
		return -EINVAL;
	if (req->cmd_req_len > UINT_MAX - req->resp_len) return -EINVAL;
	if ((req->cmd_req_len + req->resp_len) > data->client.sb_length) return -ENOMEM;
	
	return 0;
}

static int __qseecom_send_cmd(struct qseecom_dev_handle *data, struct qseecom_send_cmd_req *req)
{
	int ret = 0;
	struct qseecom_client_send_data_64bit_ireq send_data_req_64bit = {0};
	struct qseecom_command_scm_resp resp;
	unsigned long flags;
	struct qseecom_registered_app_list *ptr_app;
	bool found_app = false;
	void *cmd_buf = NULL;
	size_t cmd_len;
	struct sglist_info *table = data->sglistinfo_ptr;

	spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
	list_for_each_entry(ptr_app, &qseecom.registered_app_list_head, list) {
		if ((ptr_app->app_id == data->client.app_id) && (!strcmp(ptr_app->app_name, data->client.app_name))) {
			found_app = true;
			break;
		}
	}
	spin_unlock_irqrestore(&qseecom.registered_app_list_lock, flags);

	if (!found_app) return -ENOENT;

	send_data_req_64bit.app_id = data->client.app_id;
	send_data_req_64bit.req_ptr = __qseecom_uvirt_to_kphys(data, (uintptr_t)req->cmd_req_buf);
	send_data_req_64bit.req_len = req->cmd_req_len;
	send_data_req_64bit.rsp_ptr = __qseecom_uvirt_to_kphys(data, (uintptr_t)req->resp_buf);
	send_data_req_64bit.rsp_len = req->resp_len;
	
	if ((data->client.app_arch == ELFCLASS32) &&
		((send_data_req_64bit.req_ptr >= PHY_ADDR_4G - send_data_req_64bit.req_len) ||
		(send_data_req_64bit.rsp_ptr >= PHY_ADDR_4G - send_data_req_64bit.rsp_len))){
		return -EFAULT;
	}
	
	send_data_req_64bit.sglistinfo_ptr = (uint64_t)virt_to_phys(table);
	send_data_req_64bit.sglistinfo_len = SGLISTINFO_TABLE_SIZE;
	qsee_dmac_flush_range((void *)table, SGLISTINFO_TABLE_SIZE);
	cmd_buf = (void *)&send_data_req_64bit;
	cmd_len = sizeof(struct qseecom_client_send_data_64bit_ireq);

	*(uint32_t *)cmd_buf = (qseecom.whitelist_support == false || data->use_legacy_cmd == true) ? 
		QSEOS_CLIENT_SEND_DATA_COMMAND : QSEOS_CLIENT_SEND_DATA_COMMAND_WHITELIST;

	if (data->client.dmabuf) {
		ret = qseecom_dmabuf_cache_operations(data->client.dmabuf, QSEECOM_CACHE_CLEAN);
		if (ret) return ret;
	}

	__qseecom_reentrancy_check_if_this_app_blocked(ptr_app);

	ret = qseecom_scm_call(0xFC, 1, cmd_buf, cmd_len, &resp, sizeof(resp));
	if (ret) goto exit;

	if (data->client.dmabuf) {
		ret = qseecom_dmabuf_cache_operations(data->client.dmabuf, QSEECOM_CACHE_INVALIDATE);
		if (ret) goto exit;
	}

	if (qseecom.qsee_reentrancy_support) {
	} else {
		if (resp.result == QSEOS_RESULT_INCOMPLETE) {
			ret = __qseecom_process_incomplete_cmd(data, &resp);
		} else if (resp.result != QSEOS_RESULT_SUCCESS) {
			ret = -EINVAL;
		}
	}
exit:
	return ret;
}

static int qseecom_send_cmd(struct qseecom_dev_handle *data, void __user *argp)
{
	int ret = 0;
	struct qseecom_send_cmd_req req;

	ret = copy_from_user(&req, argp, sizeof(req));
	if (ret) return ret;

	if (__validate_send_cmd_inputs(data, &req)) return -EINVAL;
	return __qseecom_send_cmd(data, &req);
}

#define QSEECOM_MAX_SG_ENTRY	4096

static uintptr_t __qseecom_uvirt_to_kvirt(struct qseecom_dev_handle *data,
					  unsigned long virt)
{
	return (uintptr_t)data->client.sb_virt +
		(virt - data->client.user_virt_sb_base);
}

/*
 * Check that patching @size bytes at ifd_data[i].cmd_buf_offset stays in the
 * command and doesn't overlap another fd's field.
 */
static int __boundary_checks_offset(struct qseecom_send_modfd_cmd_req *req,
				    int i, size_t size)
{
	char *curr_field, *temp_field;
	int j;

	if (req->cmd_req_len < size ||
	    req->ifd_data[i].cmd_buf_offset > req->cmd_req_len - size)
		return -EINVAL;

	curr_field = (char *)req->cmd_req_buf + req->ifd_data[i].cmd_buf_offset;
	for (j = 0; j < MAX_ION_FD; j++) {
		if (req->ifd_data[j].fd <= 0 || i == j)
			continue;
		temp_field = (char *)req->cmd_req_buf +
			     req->ifd_data[j].cmd_buf_offset;
		if (temp_field >= curr_field && temp_field < curr_field + size)
			return -EINVAL;
	}

	return 0;
}

/*
 * Patch the physical addresses of the dma-bufs passed in ifd_data into the
 * client app command (or clear them again on @cleanup), as downstream's
 * __qseecom_update_cmd_buf() does for client apps.
 */
static int __qseecom_update_cmd_buf(struct qseecom_send_modfd_cmd_req *req,
				    bool cleanup,
				    struct qseecom_dev_handle *data)
{
	struct dma_buf_attachment *attach = NULL;
	struct dma_buf *dmabuf = NULL;
	struct sg_table *sg_ptr = NULL;
	struct scatterlist *sg;
	int i, j, ret = 0;
	char *field;

	if (data->type != QSEECOM_CLIENT_APP)
		return -EFAULT;
	if (data->client.app_arch != ELFCLASS32 &&
	    data->client.app_arch != ELFCLASS64) {
		pr_err("QSEE app arch %u is not supported\n",
		       data->client.app_arch);
		return -EINVAL;
	}

	for (i = 0; i < MAX_ION_FD; i++) {
		if (req->ifd_data[i].fd <= 0)
			continue;
		field = (char *)req->cmd_req_buf + req->ifd_data[i].cmd_buf_offset;

		ret = qseecom_dmabuf_map(req->ifd_data[i].fd, &sg_ptr, &attach,
					 &dmabuf);
		if (ret)
			return ret;
		if (sg_ptr->nents == 0 || sg_ptr->nents > QSEECOM_MAX_SG_ENTRY) {
			ret = -EINVAL;
			goto err;
		}

		sg = sg_ptr->sgl;
		if (sg_ptr->nents == 1) {
			if (__boundary_checks_offset(req, i, sizeof(uint32_t))) {
				ret = -EINVAL;
				goto err;
			}
			if (!cleanup &&
			    (u64)sg_dma_address(sg) >= PHY_ADDR_4G - sg->length) {
				pr_err("App %s buffer above 4G\n",
				       data->client.app_name);
				ret = -EINVAL;
				goto err;
			}
			*(uint32_t *)field = cleanup ? 0 :
				(uint32_t)sg_dma_address(sg);
		} else {
			struct qseecom_sg_entry *update =
				(struct qseecom_sg_entry *)field;

			if (__boundary_checks_offset(req, i,
					SG_ENTRY_SZ * sg_ptr->nents)) {
				ret = -EINVAL;
				goto err;
			}
			for (j = 0; j < sg_ptr->nents; j++) {
				if (!cleanup &&
				    (u64)sg_dma_address(sg) >= PHY_ADDR_4G - sg->length) {
					ret = -EINVAL;
					goto err;
				}
				update->phys_addr = cleanup ? 0 :
					(uint32_t)sg_dma_address(sg);
				update->len = cleanup ? 0 : sg->length;
				update++;
				sg = sg_next(sg);
			}
		}

		ret = qseecom_dmabuf_cache_operations(dmabuf, cleanup ?
				QSEECOM_CACHE_INVALIDATE : QSEECOM_CACHE_CLEAN);
		if (ret)
			goto err;

		if (!cleanup) {
			data->sglistinfo_ptr[i].indexAndFlags =
				SGLISTINFO_SET_INDEX_FLAG((sg_ptr->nents == 1), 0,
					req->ifd_data[i].cmd_buf_offset);
			data->sglistinfo_ptr[i].sizeOrCount =
				sg_ptr->nents == 1 ? sg_ptr->sgl->length :
						     sg_ptr->nents;
			data->sglist_cnt = i + 1;
		}

		qseecom_dmabuf_unmap(sg_ptr, attach, dmabuf);
		MAKE_NULL(sg_ptr, attach, dmabuf);
	}

	return 0;

err:
	qseecom_dmabuf_unmap(sg_ptr, attach, dmabuf);
	return ret;
}

/* QSEECOM_IOCTL_SEND_MODFD_CMD_REQ, ported from downstream */
static int qseecom_send_modfd_cmd(struct qseecom_dev_handle *data,
				  void __user *argp)
{
	struct qseecom_send_modfd_cmd_req req;
	struct qseecom_send_cmd_req send_cmd_req;
	int i, ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	send_cmd_req.cmd_req_buf = req.cmd_req_buf;
	send_cmd_req.cmd_req_len = req.cmd_req_len;
	send_cmd_req.resp_buf = req.resp_buf;
	send_cmd_req.resp_len = req.resp_len;

	if (__validate_send_cmd_inputs(data, &send_cmd_req))
		return -EINVAL;

	for (i = 0; i < MAX_ION_FD; i++) {
		if (req.ifd_data[i].cmd_buf_offset >= req.cmd_req_len)
			return -EINVAL;
	}

	/* The fields to patch live in the kernel mapping of the shared buffer */
	req.cmd_req_buf = (void *)__qseecom_uvirt_to_kvirt(data,
					(uintptr_t)req.cmd_req_buf);
	req.resp_buf = (void *)__qseecom_uvirt_to_kvirt(data,
					(uintptr_t)req.resp_buf);

	ret = __qseecom_update_cmd_buf(&req, false, data);
	if (ret)
		return ret;
	ret = __qseecom_send_cmd(data, &send_cmd_req);
	if (ret)
		return ret;

	return __qseecom_update_cmd_buf(&req, true, data);
}

static int qseecom_receive_req(struct qseecom_dev_handle *data)
{
	int ret = 0;
	struct qseecom_registered_listener_list *this_lstnr;

	mutex_lock(&listener_access_lock);
	this_lstnr = __qseecom_find_svc(data->listener.id);
	if (!this_lstnr) {
		mutex_unlock(&listener_access_lock);
		return -ENODATA;
	}
	mutex_unlock(&listener_access_lock);

	while (1) {
		if (wait_event_interruptible(this_lstnr->rcv_req_wq,
				(this_lstnr->rcv_req_flag == 1 || data->abort))) {
			return -ERESTARTSYS;
		}
		if (data->abort) return -ENODEV;
		
		mutex_lock(&listener_access_lock);
		this_lstnr->rcv_req_flag = 0;
		mutex_unlock(&listener_access_lock);
		break;
	}
	return ret;
}

static int qseecom_get_qseos_version(struct qseecom_dev_handle *data,
				     void __user *argp)
{
	struct qseecom_qseos_version_req req;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	req.qseos_version = QSEOS_VERSION_14;

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static long qseecom_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct qseecom_dev_handle *data = file->private_data;
	void __user *argp = (void __user *) arg;

	if (!data || data->abort) return -ENODEV;

	switch (cmd) {
	case QSEECOM_IOCTL_REGISTER_LISTENER_REQ:
		mutex_lock(&listener_access_lock);
		atomic_inc(&data->ioctl_count);
		data->type = QSEECOM_LISTENER_SERVICE;
		ret = qseecom_register_listener(data, argp);
		atomic_dec(&data->ioctl_count);
		wake_up_all(&data->abort_wq);
		mutex_unlock(&listener_access_lock);
		break;
	case QSEECOM_IOCTL_UNREGISTER_LISTENER_REQ:
		mutex_lock(&listener_access_lock);
		atomic_inc(&data->ioctl_count);
		ret = qseecom_unregister_listener(data);
		atomic_dec(&data->ioctl_count);
		wake_up_all(&data->abort_wq);
		mutex_unlock(&listener_access_lock);
		break;
	case QSEECOM_IOCTL_SEND_CMD_REQ:
		mutex_lock(&app_access_lock);
		atomic_inc(&data->ioctl_count);
		ret = qseecom_send_cmd(data, argp);
		atomic_dec(&data->ioctl_count);
		wake_up_all(&data->abort_wq);
		mutex_unlock(&app_access_lock);
		break;
	case QSEECOM_IOCTL_SEND_MODFD_CMD_REQ:
		if (!data->client.app_id || data->type != QSEECOM_CLIENT_APP)
			return -EINVAL;
		mutex_lock(&app_access_lock);
		atomic_inc(&data->ioctl_count);
		ret = qseecom_send_modfd_cmd(data, argp);
		atomic_dec(&data->ioctl_count);
		wake_up_all(&data->abort_wq);
		mutex_unlock(&app_access_lock);
		break;
	case QSEECOM_IOCTL_RECEIVE_REQ:
		atomic_inc(&data->ioctl_count);
		ret = qseecom_receive_req(data);
		atomic_dec(&data->ioctl_count);
		wake_up_all(&data->abort_wq);
		break;
	case QSEECOM_IOCTL_SEND_RESP_REQ:
		mutex_lock(&listener_access_lock);
		atomic_inc(&data->ioctl_count);
		qseecom.send_resp_flag = 1;
		wake_up_interruptible(&qseecom.send_resp_wq);
		atomic_dec(&data->ioctl_count);
		wake_up_all(&data->abort_wq);
		mutex_unlock(&listener_access_lock);
		break;
	case QSEECOM_IOCTL_GET_QSEOS_VERSION_REQ:
		atomic_inc(&data->ioctl_count);
		ret = qseecom_get_qseos_version(data, argp);
		atomic_dec(&data->ioctl_count);
		break;
	case QSEECOM_IOCTL_LOAD_APP_REQ:
		data->type = QSEECOM_CLIENT_APP;
		mutex_lock(&app_access_lock);
		atomic_inc(&data->ioctl_count);
		ret = qseecom_load_app(data, argp);
		atomic_dec(&data->ioctl_count);
		mutex_unlock(&app_access_lock);
		break;
	case QSEECOM_IOCTL_APP_LOADED_QUERY_REQ:
		if (data->type != QSEECOM_GENERIC &&
		    data->type != QSEECOM_CLIENT_APP)
			return -EINVAL;
		data->type = QSEECOM_CLIENT_APP;
		mutex_lock(&app_access_lock);
		atomic_inc(&data->ioctl_count);
		ret = qseecom_query_app_loaded(data, argp);
		atomic_dec(&data->ioctl_count);
		mutex_unlock(&app_access_lock);
		break;
	case QSEECOM_IOCTL_UNLOAD_APP_REQ:
		mutex_lock(&app_access_lock);
		atomic_inc(&data->ioctl_count);
		ret = qseecom_unload_app(data, false);
		atomic_dec(&data->ioctl_count);
		mutex_unlock(&app_access_lock);
		break;
	case QSEECOM_IOCTL_SET_MEM_PARAM_REQ:
		mutex_lock(&app_access_lock);
		atomic_inc(&data->ioctl_count);
		ret = qseecom_set_client_mem_param(data, argp);
		atomic_dec(&data->ioctl_count);
		mutex_unlock(&app_access_lock);
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

static int qseecom_open(struct inode *inode, struct file *file)
{
	struct qseecom_dev_handle *data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) return -ENOMEM;
	file->private_data = data;
	data->abort = 0;
	data->type = QSEECOM_GENERIC;
	data->released = false;
	init_waitqueue_head(&data->abort_wq);
	atomic_set(&data->ioctl_count, 0);
	return 0;
}

static int qseecom_release(struct inode *inode, struct file *file)
{
	struct qseecom_dev_handle *data = file->private_data;
	bool free_private_data = true;

	if (!data->released) {
		switch (data->type) {
		case QSEECOM_LISTENER_SERVICE:
			mutex_lock(&listener_access_lock);
			if (!qseecom_unregister_listener(data)) free_private_data = false;
			data->listener.release_called = true;
			mutex_unlock(&listener_access_lock);
			break;
		case QSEECOM_CLIENT_APP:
			if (data->client.app_id) {
				free_private_data = false;
				mutex_lock(&unload_app_pending_list_lock);
				qseecom_prepare_unload_app(data);
				mutex_unlock(&unload_app_pending_list_lock);
			}
			break;
		case QSEECOM_SECURE_SERVICE:
		case QSEECOM_GENERIC:
			if (data->client.dmabuf) {
				qseecom_vaddr_unmap(data->client.sb_virt, data->client.sgt, data->client.attach, data->client.dmabuf);
				MAKE_NULL(data->client.sgt, data->client.attach, data->client.dmabuf);
			}
			break;
		default:
			break;
		}
	}
	if (free_private_data) kfree_sensitive(data);
	return 0;
}

static const struct file_operations qseecom_fops = {
		.owner = THIS_MODULE,
		.unlocked_ioctl = qseecom_ioctl,
		.open = qseecom_open,
		.release = qseecom_release
};


/*
 * "qseecom" DMA-BUF heap: buffers shared with TrustZone (listener and app
 * shared buffers) must come from the qseecom carveout (memory-region,
 * no-map), like downstream's ION QSECOM heap. Buffers from the generic CMA
 * area wedge a CPU in TZ when registered. Userspace (the libion shim)
 * allocates here; qseecom itself is the only importer.
 */
struct qseecom_heap_buffer {
	size_t len;
	void *vaddr;
	dma_addr_t dma;
};

static struct sg_table *qseecom_heap_map(struct dma_buf_attachment *attach,
					 enum dma_data_direction dir)
{
	struct qseecom_heap_buffer *buf = attach->dmabuf->priv;
	struct sg_table *sgt;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);
	if (sg_alloc_table(sgt, 1, GFP_KERNEL)) {
		kfree(sgt);
		return ERR_PTR(-ENOMEM);
	}
	/* no-map carveout: no struct page, only the bus/physical address */
	sgt->sgl->length = buf->len;
	sg_dma_address(sgt->sgl) = buf->dma;
	sg_dma_len(sgt->sgl) = buf->len;

	return sgt;
}

static void qseecom_heap_unmap(struct dma_buf_attachment *attach,
			       struct sg_table *sgt,
			       enum dma_data_direction dir)
{
	sg_free_table(sgt);
	kfree(sgt);
}

static int qseecom_heap_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct qseecom_heap_buffer *buf = dmabuf->priv;

	return dma_mmap_coherent(qseecom.dev, vma, buf->vaddr, buf->dma,
				 buf->len);
}

static int qseecom_heap_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct qseecom_heap_buffer *buf = dmabuf->priv;

	iosys_map_set_vaddr(map, buf->vaddr);
	return 0;
}

static void qseecom_heap_release(struct dma_buf *dmabuf)
{
	struct qseecom_heap_buffer *buf = dmabuf->priv;

	dma_free_coherent(qseecom.dev, buf->len, buf->vaddr, buf->dma);
	kfree(buf);
}

static const struct dma_buf_ops qseecom_heap_buf_ops = {
	.map_dma_buf = qseecom_heap_map,
	.unmap_dma_buf = qseecom_heap_unmap,
	.mmap = qseecom_heap_mmap,
	.vmap = qseecom_heap_vmap,
	.release = qseecom_heap_release,
};

static struct dma_buf *qseecom_heap_allocate(struct dma_heap *heap,
					     unsigned long len, u32 fd_flags,
					     u64 heap_flags)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct qseecom_heap_buffer *buf;
	struct dma_buf *dmabuf;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	buf->len = PAGE_ALIGN(len);
	buf->vaddr = dma_alloc_coherent(qseecom.dev, buf->len, &buf->dma,
					GFP_KERNEL);
	if (!buf->vaddr) {
		kfree(buf);
		return ERR_PTR(-ENOMEM);
	}

	exp_info.exp_name = "qseecom";
	exp_info.ops = &qseecom_heap_buf_ops;
	exp_info.size = buf->len;
	exp_info.flags = fd_flags;
	exp_info.priv = buf;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		dma_free_coherent(qseecom.dev, buf->len, buf->vaddr, buf->dma);
		kfree(buf);
	}

	return dmabuf;
}

static const struct dma_heap_ops qseecom_heap_ops = {
	.allocate = qseecom_heap_allocate,
};

/* Heaps can't be removed again, so register it only once per boot */
static struct dma_heap *qseecom_heap;

static int qseecom_heap_init(struct device *dev)
{
	struct dma_heap_export_info exp_info = {
		.name = "qseecom",
		.ops = &qseecom_heap_ops,
	};
	int ret;

	if (qseecom_heap)
		return 0;

	ret = of_reserved_mem_device_init(dev);
	if (ret) {
		dev_warn(dev, "no qseecom memory-region (%d), not adding the heap\n",
			 ret);
		return 0;
	}

	qseecom_heap = dma_heap_add(&exp_info);
	if (IS_ERR(qseecom_heap)) {
		ret = PTR_ERR(qseecom_heap);
		qseecom_heap = NULL;
		of_reserved_mem_device_release(dev);
		return ret;
	}

	return 0;
}

static int qseecom_probe(struct platform_device *pdev)
{
	int rc;
	uint32_t feature = 10;
	struct qseecom_command_scm_resp resp;

	atomic_set(&qseecom.qseecom_state, QSEECOM_STATE_NOT_READY);
	qseecom.app_block_ref_cnt = 0;
	init_waitqueue_head(&qseecom.app_block_wq);

	rc = alloc_chrdev_region(&qseecom_device_no, 0, 1, QSEECOM_DEV);
	if (rc < 0) return rc;

	driver_class = class_create(QSEECOM_DEV);
	if (IS_ERR(driver_class)) {
		rc = -ENOMEM;
		goto exit_unreg_chrdev_region;
	}

	qseecom.pdev = &pdev->dev;
	qseecom.dev = &pdev->dev;
	device_create(driver_class, NULL, qseecom_device_no, NULL, QSEECOM_DEV);

	cdev_init(&qseecom.cdev, &qseecom_fops);
	qseecom.cdev.owner = THIS_MODULE;
	rc = cdev_add(&qseecom.cdev, MKDEV(MAJOR(qseecom_device_no), 0), 1);
	if (rc < 0) goto exit_destroy_device;

	INIT_LIST_HEAD(&qseecom.registered_listener_list_head);
	INIT_LIST_HEAD(&qseecom.registered_app_list_head);
	spin_lock_init(&qseecom.registered_app_list_lock);
	INIT_LIST_HEAD(&qseecom.unregister_lsnr_pending_list_head);
	init_waitqueue_head(&qseecom.send_resp_wq);
	init_waitqueue_head(&qseecom.register_lsnr_pending_wq);
	init_waitqueue_head(&qseecom.unregister_lsnr_kthread_wq);
	INIT_LIST_HEAD(&qseecom.unload_app_pending_list_head);
	init_waitqueue_head(&qseecom.unload_app_kthread_wq);

	mutex_lock(&app_access_lock);
	rc = qseecom_scm_call(6, 3, &feature, sizeof(feature), &resp, sizeof(resp));
	mutex_unlock(&app_access_lock);
	
	if (rc == 0) qseecom.qsee_version = resp.result;

	rc = dma_set_mask(qseecom.dev, DMA_BIT_MASK(64));
	if (rc) goto exit_del_cdev;

	rc = qseecom_heap_init(qseecom.dev);
	if (rc) goto exit_del_cdev;

	qseecom.unregister_lsnr_kthread_task = kthread_run(__qseecom_unregister_listener_kthread_func, NULL, "qseecom-unreg-lsnr");
	atomic_set(&qseecom.unregister_lsnr_kthread_state, LSNR_UNREG_KT_SLEEP);

	qseecom.unload_app_kthread_task = kthread_run(__qseecom_unload_app_kthread_func, NULL, "qseecom-unload-ta");
	atomic_set(&qseecom.unload_app_kthread_state, UNLOAD_APP_KT_SLEEP);

	atomic_set(&qseecom.qseecom_state, QSEECOM_STATE_READY);
	return 0;

exit_del_cdev:
	cdev_del(&qseecom.cdev);
exit_destroy_device:
	device_destroy(driver_class, qseecom_device_no);
	class_destroy(driver_class);
exit_unreg_chrdev_region:
	unregister_chrdev_region(qseecom_device_no, 1);
	return rc;
}

static void qseecom_remove(struct platform_device *pdev)
{
	atomic_set(&qseecom.qseecom_state, QSEECOM_STATE_NOT_READY);
	kthread_stop(qseecom.unload_app_kthread_task);
	kthread_stop(qseecom.unregister_lsnr_kthread_task);
	cdev_del(&qseecom.cdev);
	device_destroy(driver_class, qseecom_device_no);
	class_destroy(driver_class);
	unregister_chrdev_region(qseecom_device_no, 1);
	
}

static const struct of_device_id qseecom_match[] = {
	{ .compatible = "qcom,qseecom", },
	{}
};
MODULE_DEVICE_TABLE(of, qseecom_match);

static struct platform_driver qseecom_plat_driver = {
	.probe = qseecom_probe,
	.remove = qseecom_remove,
	.driver = {
		.name = "qseecom",
		.owner = THIS_MODULE,
		.of_match_table = qseecom_match,
	},
};

static int qseecom_init(void)
{
	return platform_driver_register(&qseecom_plat_driver);
}

static void qseecom_exit(void)
{
	platform_driver_unregister(&qseecom_plat_driver);
}

MODULE_IMPORT_NS("DMA_BUF");
MODULE_IMPORT_NS("DMA_BUF_HEAP");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("QTI Secure Execution Environment Communicator");
module_init(qseecom_init);
module_exit(qseecom_exit);
