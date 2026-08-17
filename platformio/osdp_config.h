/*
 * Copyright (c) 2024-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _OSDP_CONFIG_H_
#define _OSDP_CONFIG_H_

/**
 * @brief The following macros are defined defined from the variable in cmake
 * files. All @XXX@ are replaced by the value of XXX as resolved by cmake.
 */
#define PROJECT_VERSION                "4.0.0"
/* No git here; carries the pre-release marker only (e.g. "3.2.0-dev") while a
 * cycle is prepared. Kept in lockstep with PROJECT_VERSION by make_release.py. */
#define LIBOSDP_VERSION_STR            "4.0.0-dev"
#define PROJECT_NAME                   "libosdp"
#define GIT_BRANCH                     "platformio"
#define GIT_REV                        ""
#define GIT_TAG                        ""
#define GIT_DIFF                       ""
#define REPO_ROOT                      ""

#endif /* _OSDP_CONFIG_H_ */
