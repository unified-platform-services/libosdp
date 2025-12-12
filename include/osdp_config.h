
/*
 * Copyright (c) 2020-2024 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OSDP_CONFIG_H_
#define _OSDP_CONFIG_H_

/**
 * @brief The following macros are defined defined from the variable in cmake
 * files. All @XXX@ are replaced by the value of XXX as resolved by cmake.
 */
#define PROJECT_VERSION "@PROJECT_VERSION@"
#define PROJECT_NAME	"@PROJECT_NAME@"
#define GIT_BRANCH	"@GIT_BRANCH@"
#define GIT_REV		"@GIT_REV@"
#define GIT_TAG		"@GIT_TAG@"
#define GIT_DIFF	"@GIT_DIFF@"
#define REPO_ROOT	"@REPO_ROOT@"

/**
 * @brief Other OSDP constants
 */
#define OSDP_PD_SC_RETRY_MS	          (5 * 1000)
#define OSDP_PD_POLL_TIMEOUT_MS	      (100)
#define OSDP_PD_SC_TIMEOUT_MS	        (2 * 1000)
#define OSDP_PD_ONLINE_TOUT_MS	      (100)
#define OSDP_RESP_TOUT_MS	            (100)
#define OSDP_CMD_MAX_RETRIES	        (3)
#define OSDP_ONLINE_RETRY_WAIT_MAX_MS (1000)
#define OSDP_CMD_RETRY_WAIT_MS	      (100)
#define OSDP_PACKET_BUF_SIZE	        (64)
#define OSDP_RX_RB_SIZE		            (64)
#define OSDP_CP_CMD_POOL_SIZE	        (2)
#define OSDP_FILE_ERROR_RETRY_MAX     (10)
#define OSDP_PD_MAX		                (16)
#define OSDP_CMD_ID_OFFSET	          (5)
#define OSDP_PCAP_LINK_TYPE	          (162)
#define OSDP_PD_NAME_MAXLEN	          (1)
#define OSDP_MINIMUM_PACKET_SIZE      (64)

#endif /* _OSDP_CONFIG_H_ */