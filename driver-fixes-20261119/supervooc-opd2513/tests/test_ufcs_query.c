/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ufcs_query.h"

static void put_mode(unsigned char *buf, unsigned int id, unsigned int step,
		     unsigned int lo_mv, unsigned int hi_mv,
		     unsigned int hi_ma)
{
	unsigned long long mode = ((unsigned long long)id << 60) |
		((unsigned long long)(step == 20) << 56) |
		((unsigned long long)(hi_mv / 10) << 40) |
		((unsigned long long)(lo_mv / 10) << 24) |
		((unsigned long long)(hi_ma / 10) << 8) | 10;
	int i;

	for (i = 7; i >= 0; i--, mode >>= 8)
		buf[i] = mode & 0xff;
}

int main(void)
{
	unsigned char frame[OPD_UFCS_CAP_BYTES] = { 0x46, 0x09, 0x01, 16 };
	unsigned char identity[] = { 0x4e, 0x09, 0x06, 8, 0x22, 0xd9, 0, 1, 0, 2, 0, 3 };
	unsigned int i;

	assert(opd_ufcs_device_vendor(identity, sizeof(identity)) == 0x22d9);
	for (i = 0; i < sizeof(identity); i++)
		assert(opd_ufcs_device_vendor(identity, i) < 0);
	assert(opd_ufcs_cap_current(identity, sizeof(identity), 11000) < 0);
	assert(opd_ufcs_device_vendor(frame, 12) < 0);
	identity[3] = 7;
	assert(opd_ufcs_device_vendor(identity, sizeof(identity)) < 0);
	identity[3] = 8;
	identity[0] = 0x2e;
	assert(opd_ufcs_device_vendor(identity, sizeof(identity)) < 0);

	put_mode(frame + 4, 1, 10, 5000, 9000, 3000);
	put_mode(frame + 12, 2, 20, 9020, 11000, 6100);
	assert(opd_ufcs_cap_current(frame, 20, 11000) == 6100);
	assert(opd_ufcs_cap_current(frame, 20, 9000) == 3000);
	assert(opd_ufcs_cap_current(frame, 20, 10990) == 0);
	assert(opd_ufcs_cap_current(frame, 20, 11010) == 0);
	for (i = 0; i < 20; i++)
		assert(opd_ufcs_cap_current(frame, i, 11000) < 0);
	assert(opd_ufcs_cap_current(frame, sizeof(frame) + 1, 11000) < 0);
	frame[1] = 0x08;
	assert(opd_ufcs_cap_current(frame, 20, 11000) < 0);
	frame[1] = 0x09;
	memcpy(frame + 12, frame + 4, 8);
	assert(opd_ufcs_cap_current(frame, 20, 11000) < 0);
	put_mode(frame + 12, 2, 20, 11000, 9020, 6100);
	assert(opd_ufcs_cap_current(frame, 20, 11000) < 0);
	puts("UFCS device identity and capability frame tests passed");
}
