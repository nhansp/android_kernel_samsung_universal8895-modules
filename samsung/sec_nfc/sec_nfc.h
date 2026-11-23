/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SAMSUNG NFC N2 Controller
 *
 * Copyright (C) 2013 Samsung Electronics Co.Ltd
 * Author: Woonki Lee <woonki84.lee@samsung.com>
 */

#ifndef _SEC_NFC_H
#define _SEC_NFC_H

#include <linux/ioctl.h>
#include <linux/printk.h>

#define SEC_NFC_DRIVER_NAME		"sec-nfc"

#define SEC_NFC_MAX_BUFFER_SIZE	512

/* ioctl */
#define SEC_NFC_MAGIC	'S'
#define SEC_NFC_GET_MODE		_IOW(SEC_NFC_MAGIC, 0, unsigned int)
#define SEC_NFC_SET_MODE		_IOW(SEC_NFC_MAGIC, 1, unsigned int)
#define SEC_NFC_SLEEP			_IOW(SEC_NFC_MAGIC, 2, unsigned int)
#define SEC_NFC_WAKEUP			_IOW(SEC_NFC_MAGIC, 3, unsigned int)
#define SEC_NFC_SET_NPT_MODE		_IOW(SEC_NFC_MAGIC, 4, unsigned int)

#define SEC_NFC_DEBUG			_IO(SEC_NFC_MAGIC, 99)

/* size */
#define SEC_NFC_MSG_MAX_SIZE	(256 + 4)

/* wait for device stable */
#define SEC_NFC_VEN_WAIT_TIME	(100)

enum sec_nfc_mode {
	SEC_NFC_MODE_OFF = 0,
	SEC_NFC_MODE_FIRMWARE,
	SEC_NFC_MODE_BOOTLOADER,
	SEC_NFC_MODE_COUNT,
};

enum sec_nfc_power {
	SEC_NFC_PW_ON = 0,
	SEC_NFC_PW_OFF,
};

enum sec_nfc_firmpin {
	SEC_NFC_FW_OFF = 0,
	SEC_NFC_FW_ON,
};

enum sec_nfc_wake {
	SEC_NFC_WAKE_SLEEP = 0,
	SEC_NFC_WAKE_UP,
};

// [START] NPT
enum sec_nfc_npt_mode {
	SEC_NFC_NPT_OFF = 0,
	SEC_NFC_NPT_ON,
	SEC_NFC_NPT_CMD_ON = 0x7E,
	SEC_NFC_NPT_CMD_OFF,
};
// [END] NPT

#define NFC_I2C_LDO_ON  1
#define NFC_I2C_LDO_OFF 0

/* the definitions nfc_logger.h makes without CONFIG_SEC_NFC_LOGGER */
#define NFC_LOG_ERR(fmt, ...)		pr_err("sec-nfc: "fmt, ##__VA_ARGS__)
#define NFC_LOG_INFO(fmt, ...)		pr_info("sec-nfc: "fmt, ##__VA_ARGS__)
#define NFC_LOG_DBG(fmt, ...)		pr_debug("sec-nfc: "fmt, ##__VA_ARGS__)
#define NFC_LOG_REC(fmt, ...)		do { } while (0)

#endif /* _SEC_NFC_H */
