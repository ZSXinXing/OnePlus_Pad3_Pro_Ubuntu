/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OPD2513_UFCS_QUERY_H
#define OPD2513_UFCS_QUERY_H

/* UFCS 1.0 Output_Capabilities. See OnePlusOSS ufcs_msg.h and
 * oplus_chg_ufcs.c at 8be53e8b737a83a33512d5e0106cccb010a5c24c.
 */
#define OPD_UFCS_CAP_BYTES (4 + 7 * 8)
#define OPD_UFCS_TARGET_MV 11000
#define OPD_UFCS_DEVICE_INFO 0x06
#define OPD_UFCS_OUTPUT_CAPABILITIES 0x01

static inline int opd_ufcs_data_valid(const unsigned char *buf,
		unsigned int len, unsigned int command)
{
	/* Destination sink, revision 1, data message with the expected command. */
	return len >= 4 && (buf[0] & 0xe0) == 0x40 && buf[1] == 0x09 &&
		buf[2] == command && buf[3] == len - 4;
}

static inline int opd_ufcs_device_vendor(const unsigned char *buf,
		unsigned int len)
{
	if (len != 12 || !opd_ufcs_data_valid(buf, len, OPD_UFCS_DEVICE_INFO))
		return -1;
	return (buf[4] << 8) | buf[5];
}

static inline int opd_ufcs_cap_current(const unsigned char *buf,
		unsigned int len, unsigned int target_mv)
{
	unsigned int i, j, ids = 0, best = 0;

	if (len < 12 || len > OPD_UFCS_CAP_BYTES ||
	    !opd_ufcs_data_valid(buf, len, OPD_UFCS_OUTPUT_CAPABILITIES) ||
	    buf[3] % 8)
		return -1;

	for (i = 4; i < len; i += 8) {
		unsigned long long mode = 0;
		unsigned int id, min_mv, max_mv, min_ma, max_ma, step_mv;

		for (j = 0; j < 8; j++)
			mode = (mode << 8) | buf[i + j];
		id = mode >> 60;
		max_mv = ((mode >> 40) & 0xffff) * 10;
		min_mv = ((mode >> 24) & 0xffff) * 10;
		max_ma = ((mode >> 8) & 0xffff) * 10;
		min_ma = (mode & 0xff) * 10;
		step_mv = (((mode >> 56) & 1) + 1) * 10;
		if (!id || id > 7 || (ids & (1U << id)) || !min_mv ||
		    min_mv > max_mv || !min_ma || min_ma > max_ma)
			return -1;
		ids |= 1U << id;
		if (target_mv >= min_mv && target_mv <= max_mv &&
		    (target_mv - min_mv) % step_mv == 0 && max_ma > best)
			best = max_ma;
	}

	return best;
}
#endif
