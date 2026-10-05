#ifndef __QSEECOMI_H_
#define __QSEECOMI_H_

#include <uapi/linux/qseecom.h>

#define QSEECOM_KEY_ID_SIZE   32

#define QSEOS_RESULT_FAIL_SEND_CMD_NO_THREAD  -19
#define QSEOS_RESULT_FAIL_UNSUPPORTED_CE_PIPE -63
#define QSEOS_RESULT_FAIL_KS_OP               -64
#define QSEOS_RESULT_FAIL_KEY_ID_EXISTS       -65
#define QSEOS_RESULT_FAIL_MAX_KEYS            -66
#define QSEOS_RESULT_FAIL_SAVE_KS             -67
#define QSEOS_RESULT_FAIL_LOAD_KS             -68
#define QSEOS_RESULT_FAIL_KS_ALREADY_DONE     -69
#define QSEOS_RESULT_FAIL_KEY_ID_DNE          -70
#define QSEOS_RESULT_FAIL_INCORRECT_PSWD      -71
#define QSEOS_RESULT_FAIL_MAX_ATTEMPT         -72
#define QSEOS_RESULT_FAIL_PENDING_OPERATION   -73

enum qseecom_command_scm_resp_type {
	QSEOS_APP_ID = 0xEE01,
	QSEOS_LISTENER_ID
};

enum qseecom_qceos_cmd_id {
	QSEOS_APP_START_COMMAND      = 0x01,
	QSEOS_APP_SHUTDOWN_COMMAND,
	QSEOS_APP_LOOKUP_COMMAND,
	QSEOS_REGISTER_LISTENER,
	QSEOS_DEREGISTER_LISTENER,
	QSEOS_CLIENT_SEND_DATA_COMMAND,
	QSEOS_LISTENER_DATA_RSP_COMMAND,
	QSEOS_LOAD_EXTERNAL_ELF_COMMAND,
	QSEOS_UNLOAD_EXTERNAL_ELF_COMMAND,
	QSEOS_GET_APP_STATE_COMMAND,
	QSEOS_LOAD_SERV_IMAGE_COMMAND,
	QSEOS_UNLOAD_SERV_IMAGE_COMMAND,
	QSEOS_APP_REGION_NOTIFICATION,
	QSEOS_REGISTER_LOG_BUF_COMMAND,
	QSEOS_RPMB_PROVISION_KEY_COMMAND,
	QSEOS_RPMB_ERASE_COMMAND,
	QSEOS_GENERATE_KEY  = 0x11,
	QSEOS_DELETE_KEY,
	QSEOS_MAX_KEY_COUNT,
	QSEOS_SET_KEY,
	QSEOS_UPDATE_KEY_USERINFO,
	QSEOS_TEE_OPEN_SESSION,
	QSEOS_TEE_INVOKE_COMMAND,
	QSEOS_TEE_INVOKE_MODFD_COMMAND = QSEOS_TEE_INVOKE_COMMAND,
	QSEOS_TEE_CLOSE_SESSION,
	QSEOS_TEE_REQUEST_CANCELLATION,
	QSEOS_CONTINUE_BLOCKED_REQ_COMMAND,
	QSEOS_RPMB_CHECK_PROV_STATUS_COMMAND = 0x1B,
	QSEOS_CLIENT_SEND_DATA_COMMAND_WHITELIST = 0x1C,
	QSEOS_TEE_OPEN_SESSION_WHITELIST = 0x1D,
	QSEOS_TEE_INVOKE_COMMAND_WHITELIST = 0x1E,
	QSEOS_LISTENER_DATA_RSP_COMMAND_WHITELIST = 0x1F,
	QSEOS_SOTA_NOTIFICATION_CHECK_STATUS = 0x20,
	QSEOS_FSM_LTEOTA_REQ_CMD = 0x109,
	QSEOS_FSM_LTEOTA_REQ_RSP_CMD = 0x110,
	QSEOS_FSM_IKE_REQ_CMD = 0x203,
	QSEOS_FSM_IKE_REQ_RSP_CMD = 0x204,
	QSEOS_FSM_OEM_FUSE_WRITE_ROW = 0x301,
	QSEOS_FSM_OEM_FUSE_READ_ROW = 0x302,
	QSEOS_FSM_ENCFS_REQ_CMD = 0x403,
	QSEOS_FSM_ENCFS_REQ_RSP_CMD = 0x404,

	QSEOS_CMD_MAX     = 0xEFFFFFFF
};

enum qseecom_qceos_cmd_status {
	QSEOS_RESULT_SUCCESS = 0,
	QSEOS_RESULT_INCOMPLETE,
	QSEOS_RESULT_BLOCKED_ON_LISTENER,
	QSEOS_RESULT_FAILURE  = 0xFFFFFFFF
};

enum qseecom_pipe_type {
	QSEOS_PIPE_ENC = 0x1,
	QSEOS_PIPE_ENC_XTS = 0x2,
	QSEOS_PIPE_AUTH = 0x4,
	QSEOS_PIPE_ENUM_FILL = 0x7FFFFFFF
};

enum qseecom_qsee_reentrancy_phase {
	QSEE_REENTRANCY_PHASE_0 = 0,
	QSEE_REENTRANCY_PHASE_1,
	QSEE_REENTRANCY_PHASE_2,
	QSEE_REENTRANCY_PHASE_3,
	QSEE_REENTRANCY_PHASE_MAX = 0xFF
};

struct qsee_apps_region_info_ireq {
	uint32_t qsee_cmd_id;
	uint32_t addr;
	uint32_t size;
} __attribute__((__packed__));

struct qsee_apps_region_info_64bit_ireq {
	uint32_t qsee_cmd_id;
	uint64_t addr;
	uint32_t size;
} __attribute__((__packed__));

struct qseecom_check_app_ireq {
	uint32_t qsee_cmd_id;
	char     app_name[MAX_APP_NAME_SIZE];
} __attribute__((__packed__));

struct qseecom_load_app_ireq {
	uint32_t qsee_cmd_id;
	uint32_t mdt_len;
	uint32_t img_len;
	uint32_t phy_addr;
	char     app_name[MAX_APP_NAME_SIZE];
} __attribute__((__packed__));

struct qseecom_load_app_64bit_ireq {
	uint32_t qsee_cmd_id;
	uint32_t mdt_len;
	uint32_t img_len;
	uint64_t phy_addr;
	char     app_name[MAX_APP_NAME_SIZE];
} __attribute__((__packed__));

struct qseecom_unload_app_ireq {
	uint32_t qsee_cmd_id;
	uint32_t  app_id;
} __attribute__((__packed__));

struct qseecom_load_lib_image_ireq {
	uint32_t qsee_cmd_id;
	uint32_t mdt_len;
	uint32_t img_len;
	uint32_t phy_addr;
} __attribute__((__packed__));

struct qseecom_load_lib_image_64bit_ireq {
	uint32_t qsee_cmd_id;
	uint32_t mdt_len;
	uint32_t img_len;
	uint64_t phy_addr;
} __attribute__((__packed__));

struct qseecom_unload_lib_image_ireq {
	uint32_t qsee_cmd_id;
} __attribute__((__packed__));

struct qseecom_register_listener_ireq {
	uint32_t qsee_cmd_id;
	uint32_t listener_id;
	uint32_t sb_ptr;
	uint32_t sb_len;
} __attribute__((__packed__));

struct qseecom_register_listener_64bit_ireq {
	uint32_t qsee_cmd_id;
	uint32_t listener_id;
	uint64_t sb_ptr;
	uint32_t sb_len;
} __attribute__((__packed__));

struct qseecom_unregister_listener_ireq {
	uint32_t qsee_cmd_id;
	uint32_t  listener_id;
} __attribute__((__packed__));

struct qseecom_client_send_data_ireq {
	uint32_t qsee_cmd_id;
	uint32_t app_id;
	uint32_t req_ptr;
	uint32_t req_len;
	uint32_t rsp_ptr;
	uint32_t rsp_len;
	uint32_t sglistinfo_ptr;
	uint32_t sglistinfo_len;
} __attribute__((__packed__));

struct qseecom_client_send_data_64bit_ireq {
	uint32_t qsee_cmd_id;
	uint32_t app_id;
	uint64_t req_ptr;
	uint32_t req_len;
	uint64_t rsp_ptr;
	uint32_t rsp_len;
	uint64_t sglistinfo_ptr;
	uint32_t sglistinfo_len;
} __attribute__((__packed__));

struct qseecom_reg_log_buf_ireq {
	uint32_t qsee_cmd_id;
	uint32_t phy_addr;
	uint32_t len;
} __attribute__((__packed__));

struct qseecom_reg_log_buf_64bit_ireq {
	uint32_t qsee_cmd_id;
	uint64_t phy_addr;
	uint32_t len;
} __attribute__((__packed__));

struct qseecom_client_listener_data_irsp {
	uint32_t qsee_cmd_id;
	uint32_t listener_id;
	uint32_t status;
	uint32_t sglistinfo_ptr;
	uint32_t sglistinfo_len;
} __attribute__((__packed__));

struct qseecom_client_listener_data_64bit_irsp {
	uint32_t qsee_cmd_id;
	uint32_t listener_id;
	uint32_t status;
	uint64_t sglistinfo_ptr;
	uint32_t sglistinfo_len;
} __attribute__((__packed__));

struct qseecom_command_scm_resp {
	uint32_t result;
	enum qseecom_command_scm_resp_type resp_type;
	unsigned int data;
} __attribute__((__packed__));

struct qseecom_rpmb_provision_key {
	uint32_t key_type;
};

struct qseecom_client_send_service_ireq {
	uint32_t qsee_cmd_id;
	uint32_t key_type;
	unsigned int req_len;
	uint32_t rsp_ptr;
	unsigned int rsp_len;
} __attribute__((__packed__));

struct qseecom_client_send_service_64bit_ireq {
	uint32_t qsee_cmd_id;
	uint32_t key_type;
	unsigned int req_len;
	uint64_t rsp_ptr;
	unsigned int rsp_len;
} __attribute__((__packed__));

struct qseecom_key_generate_ireq {
	uint32_t qsee_command_id;
	uint32_t flags;
	uint8_t key_id[QSEECOM_KEY_ID_SIZE];
	uint8_t hash32[QSEECOM_HASH_SIZE];
} __attribute__((__packed__));

struct qseecom_key_select_ireq {
	uint32_t qsee_command_id;
	uint32_t ce;
	uint32_t pipe;
	uint32_t pipe_type;
	uint32_t flags;
	uint8_t key_id[QSEECOM_KEY_ID_SIZE];
	uint8_t hash32[QSEECOM_HASH_SIZE];
} __attribute__((__packed__));

struct qseecom_key_delete_ireq {
	uint32_t qsee_command_id;
	uint32_t flags;
	uint8_t key_id[QSEECOM_KEY_ID_SIZE];
	uint8_t hash32[QSEECOM_HASH_SIZE];

} __attribute__((__packed__));

struct qseecom_key_userinfo_update_ireq {
	uint32_t qsee_command_id;
	uint32_t flags;
	uint8_t key_id[QSEECOM_KEY_ID_SIZE];
	uint8_t current_hash32[QSEECOM_HASH_SIZE];
	uint8_t new_hash32[QSEECOM_HASH_SIZE];
} __attribute__((__packed__));

struct qseecom_key_max_count_query_ireq {
	uint32_t flags;
} __attribute__((__packed__));

struct qseecom_key_max_count_query_irsp {
	uint32_t max_key_count;
} __attribute__((__packed__));

struct qseecom_qteec_ireq {
	uint32_t    qsee_cmd_id;
	uint32_t    app_id;
	uint32_t    req_ptr;
	uint32_t    req_len;
	uint32_t    resp_ptr;
	uint32_t    resp_len;
	uint32_t    sglistinfo_ptr;
	uint32_t    sglistinfo_len;
} __attribute__((__packed__));

struct qseecom_qteec_64bit_ireq {
	uint32_t    qsee_cmd_id;
	uint32_t    app_id;
	uint64_t    req_ptr;
	uint32_t    req_len;
	uint64_t    resp_ptr;
	uint32_t    resp_len;
	uint64_t    sglistinfo_ptr;
	uint32_t    sglistinfo_len;
} __attribute__((__packed__));

struct qseecom_client_send_fsm_key_req {
	uint32_t qsee_cmd_id;
	uint32_t req_ptr;
	uint32_t req_len;
	uint32_t rsp_ptr;
	uint32_t rsp_len;
} __attribute__((__packed__));

struct qseecom_continue_blocked_request_ireq {
	uint32_t qsee_cmd_id;
	uint32_t app_or_session_id;
} __attribute__((__packed__));

#define TZ_SVC_APP_MGR                   1
#define TZ_SVC_LISTENER                  2
#define TZ_SVC_EXTERNAL                  3
#define TZ_SVC_RPMB                      4
#define TZ_SVC_KEYSTORE                  5
#define TZ_SVC_ES                        16
#define TZ_SVC_MDTP                      18
#define TZ_SVC_FUSE                      8

#define TZ_OWNER_ARM                     0
#define TZ_OWNER_CPU                     1
#define TZ_OWNER_SIP                     2
#define TZ_OWNER_OEM                     3
#define TZ_OWNER_STD                     4

#define TZ_OWNER_TZ_APPS                 48
#define TZ_OWNER_TZ_APPS_RESERVED        49
#define TZ_OWNER_QSEE_OS                 50
#define TZ_OWNER_MOBI_OS                 51

#define TZ_SVC_INFO                      6
#define TZ_SVC_APP_ID_PLACEHOLDER        0
#define TZ_MASK_BITS(h, l)     ((0xffffffff >> (32 - ((h - l) + 1))) << l)

#define TZ_SYSCALL_CREATE_SMC_ID(o, s, f) \
	((uint32_t)((((o & 0x3f) << 24) | (s & 0xff) << 8) | (f & 0xff)))

#define TZ_SYSCALL_PARAM_NARGS_MASK  TZ_MASK_BITS(3, 0)
#define TZ_SYSCALL_PARAM_TYPE_MASK   TZ_MASK_BITS(1, 0)

#define TZ_SYSCALL_CREATE_PARAM_ID(nargs, p1, p2, p3, \
	p4, p5, p6, p7, p8, p9, p10) \
	((nargs&TZ_SYSCALL_PARAM_NARGS_MASK)+ \
	((p1&TZ_SYSCALL_PARAM_TYPE_MASK)<<4)+ \
	((p2&TZ_SYSCALL_PARAM_TYPE_MASK)<<6)+ \
	((p3&TZ_SYSCALL_PARAM_TYPE_MASK)<<8)+ \
	((p4&TZ_SYSCALL_PARAM_TYPE_MASK)<<10)+ \
	((p5&TZ_SYSCALL_PARAM_TYPE_MASK)<<12)+ \
	((p6&TZ_SYSCALL_PARAM_TYPE_MASK)<<14)+ \
	((p7&TZ_SYSCALL_PARAM_TYPE_MASK)<<16)+ \
	((p8&TZ_SYSCALL_PARAM_TYPE_MASK)<<18)+ \
	((p9&TZ_SYSCALL_PARAM_TYPE_MASK)<<20)+ \
	((p10&TZ_SYSCALL_PARAM_TYPE_MASK)<<22))

#define TZ_SYSCALL_CREATE_PARAM_ID_0 0
#define TZ_SYSCALL_CREATE_PARAM_ID_1(p1) \
	TZ_SYSCALL_CREATE_PARAM_ID(1, p1, 0, 0, 0, 0, 0, 0, 0, 0, 0)
#define TZ_SYSCALL_CREATE_PARAM_ID_2(p1, p2) \
	TZ_SYSCALL_CREATE_PARAM_ID(2, p1, p2, 0, 0, 0, 0, 0, 0, 0, 0)
#define TZ_SYSCALL_CREATE_PARAM_ID_3(p1, p2, p3) \
	TZ_SYSCALL_CREATE_PARAM_ID(3, p1, p2, p3, 0, 0, 0, 0, 0, 0, 0)
#define TZ_SYSCALL_CREATE_PARAM_ID_4(p1, p2, p3, p4) \
	TZ_SYSCALL_CREATE_PARAM_ID(4, p1, p2, p3, p4, 0, 0, 0, 0, 0, 0)
#define TZ_SYSCALL_CREATE_PARAM_ID_5(p1, p2, p3, p4, p5) \
	TZ_SYSCALL_CREATE_PARAM_ID(5, p1, p2, p3, p4, p5, 0, 0, 0, 0, 0)
#define TZ_SYSCALL_CREATE_PARAM_ID_6(p1, p2, p3, p4, p5, p6) \
	TZ_SYSCALL_CREATE_PARAM_ID(6, p1, p2, p3, p4, p5, p6, 0, 0, 0, 0)
#define TZ_SYSCALL_CREATE_PARAM_ID_7(p1, p2, p3, p4, p5, p6, p7) \
	TZ_SYSCALL_CREATE_PARAM_ID(7, p1, p2, p3, p4, p5, p6, p7, 0, 0, 0)

#define TZ_SYSCALL_OWNER_ID(r0)   ((r0 & TZ_MASK_BITS(29, 24)) >> 24)
#define IS_OWNER_TRUSTED_OS(owner_id) (((owner_id >= 50) && (owner_id <= 63)) ? 1:0)

#define TZ_SYSCALL_PARAM_TYPE_VAL              0x0
#define TZ_SYSCALL_PARAM_TYPE_BUF_RO           0x1
#define TZ_SYSCALL_PARAM_TYPE_BUF_RW           0x2

#define TZ_OS_APP_START_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 0x01)
#define TZ_OS_APP_START_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_3( \
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_VAL, \
	TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_OS_APP_SHUTDOWN_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 0x02)
#define TZ_OS_APP_SHUTDOWN_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_1(TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_OS_APP_LOOKUP_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 0x03)
#define TZ_OS_APP_LOOKUP_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_2( \
	TZ_SYSCALL_PARAM_TYPE_BUF_RW, TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_OS_APP_REGION_NOTIFICATION_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 0x05)
#define TZ_OS_APP_REGION_NOTIFICATION_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_2( \
	TZ_SYSCALL_PARAM_TYPE_BUF_RW, TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_OS_REGISTER_LISTENER_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x01)
#define TZ_OS_REGISTER_LISTENER_SMCINVOKE_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x06)
#define TZ_OS_REGISTER_LISTENER_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_3( \
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW, \
	TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_OS_DEREGISTER_LISTENER_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x02)
#define TZ_OS_DEREGISTER_LISTENER_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_1(TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_OS_LISTENER_RESPONSE_HANDLER_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x03)
#define TZ_OS_LISTENER_RESPONSE_HANDLER_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_2( \
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_OS_LOAD_EXTERNAL_IMAGE_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_EXTERNAL, 0x01)
#define TZ_OS_LOAD_EXTERNAL_IMAGE_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_3( \
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_VAL, \
	TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_APP_QSAPP_SEND_DATA_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_TZ_APPS, \
	TZ_SVC_APP_ID_PLACEHOLDER, 0x01)
#define TZ_APP_QSAPP_SEND_DATA_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_5( \
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW, \
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW, \
	TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_OS_UNLOAD_EXTERNAL_IMAGE_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_EXTERNAL, 0x02)
#define TZ_OS_UNLOAD_EXTERNAL_IMAGE_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_0

#define TZ_INFO_GET_FEATURE_VERSION_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_SIP, TZ_SVC_INFO, 0x03)
#define TZ_INFO_GET_FEATURE_VERSION_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_1(TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_ES_SAVE_PARTITION_HASH_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_SIP, TZ_SVC_ES, 0x01)
#define TZ_ES_SAVE_PARTITION_HASH_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_3( \
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW, \
	TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_APP_GPAPP_OPEN_SESSION_ID					\
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_TZ_APPS,			\
	TZ_SVC_APP_ID_PLACEHOLDER, 0x02)
#define TZ_APP_GPAPP_OPEN_SESSION_ID_PARAM_ID				\
	TZ_SYSCALL_CREATE_PARAM_ID_5(					\
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW,	\
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW,	\
	TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_APP_GPAPP_CLOSE_SESSION_ID					\
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_TZ_APPS,			\
	TZ_SVC_APP_ID_PLACEHOLDER, 0x03)
#define TZ_APP_GPAPP_CLOSE_SESSION_ID_PARAM_ID				\
	TZ_SYSCALL_CREATE_PARAM_ID_5(					\
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW,	\
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW,	\
	TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_APP_GPAPP_INVOKE_COMMAND_ID					\
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_TZ_APPS,			\
	TZ_SVC_APP_ID_PLACEHOLDER, 0x04)
#define TZ_APP_GPAPP_INVOKE_COMMAND_ID_PARAM_ID				\
	TZ_SYSCALL_CREATE_PARAM_ID_5(					\
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW,	\
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW,	\
	TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_APP_GPAPP_REQUEST_CANCELLATION_ID				\
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_TZ_APPS,			\
	TZ_SVC_APP_ID_PLACEHOLDER, 0x05)
#define TZ_APP_GPAPP_REQUEST_CANCELLATION_ID_PARAM_ID			\
	TZ_SYSCALL_CREATE_PARAM_ID_5(					\
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW,	\
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW,	\
	TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_OS_CONTINUE_BLOCKED_REQUEST_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x04)
#define TZ_OS_CONTINUE_BLOCKED_REQUEST_SMCINVOKE_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x07)
#define TZ_OS_CONTINUE_BLOCKED_REQUEST_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_1(TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_APP_QSAPP_SEND_DATA_WITH_WHITELIST_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_TZ_APPS, \
	TZ_SVC_APP_ID_PLACEHOLDER, 0x06)
#define TZ_APP_QSAPP_SEND_DATA_WITH_WHITELIST_ID_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_7( \
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW, \
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW, \
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW, \
	TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_APP_GPAPP_OPEN_SESSION_WITH_WHITELIST_ID			\
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_TZ_APPS,			\
	TZ_SVC_APP_ID_PLACEHOLDER, 0x07)
#define TZ_APP_GPAPP_OPEN_SESSION_WITH_WHITELIST_ID_PARAM_ID		\
	TZ_SYSCALL_CREATE_PARAM_ID_7(					\
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW,	\
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW,	\
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW,	\
	TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_APP_GPAPP_INVOKE_COMMAND_WITH_WHITELIST_ID			\
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_TZ_APPS,			\
	TZ_SVC_APP_ID_PLACEHOLDER, 0x09)
#define TZ_APP_GPAPP_INVOKE_COMMAND_WITH_WHITELIST_ID_PARAM_ID		\
	TZ_SYSCALL_CREATE_PARAM_ID_7(					\
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW,	\
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW,	\
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_BUF_RW,	\
	TZ_SYSCALL_PARAM_TYPE_VAL)

#define TZ_OS_LISTENER_RESPONSE_HANDLER_WITH_WHITELIST_ID \
	TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x05)
#define TZ_OS_LISTENER_RESPONSE_HANDLER_WITH_WHITELIST_PARAM_ID \
	TZ_SYSCALL_CREATE_PARAM_ID_4( \
	TZ_SYSCALL_PARAM_TYPE_VAL, TZ_SYSCALL_PARAM_TYPE_VAL, \
	TZ_SYSCALL_PARAM_TYPE_BUF_RW, TZ_SYSCALL_PARAM_TYPE_VAL)

#endif /* __QSEECOMI_H_ */
