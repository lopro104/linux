#ifndef __QSEECOM_KERNEL_H_
#define __QSEECOM_KERNEL_H_

#include <linux/types.h>

#define QSEECOM_ALIGN_SIZE	0x40
#define QSEECOM_ALIGN_MASK	(QSEECOM_ALIGN_SIZE - 1)
#define QSEECOM_ALIGN(x)	((x + QSEECOM_ALIGN_MASK) & (~QSEECOM_ALIGN_MASK))

struct scm_desc {
	u32 arginfo;
	u64 args[10];
	u64 ret[3];
};

struct qseecom_handle {
	void *dev;
	unsigned char *sbuf;
	uint32_t sbuf_len;
};

int qseecom_start_app(struct qseecom_handle **handle, char *app_name, uint32_t size);
int qseecom_shutdown_app(struct qseecom_handle **handle);
int qseecom_send_command(struct qseecom_handle *handle, void *send_buf,
			uint32_t sbuf_len, void *resp_buf, uint32_t rbuf_len);
int qseecom_set_bandwidth(struct qseecom_handle *handle, bool high);
int qseecom_process_listener_from_smcinvoke(struct scm_desc *desc);

#endif /* __QSEECOM_KERNEL_H_ */
