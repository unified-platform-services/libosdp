/*
 * Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Prints the ABI of the libosdp it is linked against as key=value lines, for
 * build-native.sh to stamp into abi.txt. Linking through the installed CMake
 * package also proves the install tree is consumable.
 */

#include <stdio.h>
#include <osdp.h>

int main(void)
{
	struct osdp_abi_info info;

	info.size = sizeof(info);
	osdp_get_abi_info(&info);
	printf("version=%s\n", osdp_get_version());
	printf("abi_size=%u\n", (unsigned)info.size);
	printf("options=0x%08x\n", (unsigned)info.options);
	printf("sizeof_cmd=%u\n", (unsigned)info.sizeof_cmd);
	printf("sizeof_event=%u\n", (unsigned)info.sizeof_event);
	printf("sizeof_channel=%u\n", (unsigned)info.sizeof_channel);
	printf("sizeof_pd_info=%u\n", (unsigned)info.sizeof_pd_info);
	return 0;
}
