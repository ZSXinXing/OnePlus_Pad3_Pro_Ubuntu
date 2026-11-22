/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SUPERVOOC identification logic from dappermint/iceland-supervooc,
 * commit 614469fabe593981538f492baf7fa1f19e61c462. Adapted for OPD2513.
 *
 * The frame logic mirrors OnePlus's GPL charger v2 for SVOOC adapters. What the comments refer to:
 *   V.c       vendor/oplus/kernel/charger/v2/voocphy/oplus_ap_voocphy.c
 *   D.c       vendor/oplus/kernel/charger/v2/ufcs_ic/oplus_hal_sc8547d.c
 *             both in OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8850, line numbers as of 07ca84e
 *   live.dts  the device tree of the stock firmware (11.A.38), decompiled
 *   stock     that firmware, as seen in an i2c and kernel-log trace of a full 80 W session on a Pad 4
 *
 * Round 1 (vl_irq) follows V.c's handlers 2032-2473, dispatcher 3670-3751, tx encoding 3753-3774 and counters 3921-3938. Its
 * 0x04 reply never carries the allow bit.
 */
#ifndef VOOC_LOGIC_H
#define VOOC_LOGIC_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
#endif

#define VF_RXDONE	0x02
#define VF_TX_WRITE_FAIL	0x08
#define VF_ABORT	0xd8	/* 0x80 pulse filtered, 0x40 ninth clock, 0x10 transfer error, 0x08 TX write fail */
#define VL_FIRST	0xa8
#define VL_MAX_IGNORE	6

enum { VL_WAIT, VL_LIVE, VL_DONE, VL_STOP };

enum {
	VL_NONE = 0,		/* nothing to send */
	VL_TX = 1,		/* write *tx */
	VL_LAST = 2,		/* write *tx; the dialogue is complete (state VL_DONE) */
	VL_E_FLAG = -1,
	VL_E_START = -2,
	VL_E_HEAD = -3,
	VL_E_CMD = -4,
	VL_E_ECHO = -5,
	VL_E_ID = -6,
	VL_E_PROT = -7,		/* stage 3: protection read-back mismatch, enable refused */
	VL_E_VBUS = -8,		/* stage 3: VBUS window unusable (no ADC, or VBUS / VBAT below 2000 mV three times) */
};

enum { VC_ASK = 4, VC_IDENT = 0x0c, VC_MODEL = 0x05, VC_BATT = 0x0d };	/* adapter commands */
enum { VP_IDENT = 0xf1, VP_MODEL = 0xf2, VP_BATT = 0xf3 };		/* pending follow-ups */

struct vl {
	u8 rl, rh;		/* the tablet's identification code, 7 bits each, drawn by the caller */
	u8 state;
	u8 ignored;		/* RXDATA_DONE frames before the first frame */
	u8 pend;		/* pending follow-up (stock fastchg_adapter_ask_cmd), 0 = none */
	u8 now;			/* command answered by the last call (for the tests) */
	u8 seen;		/* commands already served, bit per VC_* index below */
	u8 n_id, n_model, n_batt;	/* adapter_check_cmmu_count, adapter_model_count, ask_batt_sys */
	u8 nx_l, nx_h;		/* identification bits for the next reply */
	u8 model;
	u16 far, local;
};

/* OPD2513 stock svooc_batt_sys_curve, with row zero transmitted last. */
static const u8 vl_batt_row[6] = { 0x5e, 0x40, 0x2c, 0x3f, 0x68, 0x3f };
static const u8 vl_batt_echo[6] = { 1, 1, 2, 2, 3, 3 };		/* real_bat_sys_msg, V.c:159 */

/*
 * the pump is idle while CHG_EN (07.7) and the switching status (06.2) are clear. the low bits of 0x07 are not part of
 * it: they read 0x07 after the soft reset, on stock too (its block read at 0x06 after the init: 22 07 e0 10)
 */
static inline int vl_pump_idle(u8 r06, u8 r07)
{
	return !(r07 & 0x80) && !(r06 & 0x04);
}

static inline int vl_id_ok(u8 id)
{
	return (id >= 0x4b && id <= 0x4e) || id == 0x65;
}

static inline void vl_init(struct vl *s, u8 rl, u8 rh)
{
	u8 *p = (u8 *)s;
	unsigned int i;

	for (i = 0; i < sizeof(*s); i++)
		p[i] = 0;
	s->rl = rl & 0x7f;
	s->rh = rh & 0x7f;
	s->state = VL_WAIT;
	s->local = (s->rh << 9) | (s->rl << 1);
}

/* tx0 bits 2:0 = head (0x01), 7:3 = data; tx1 bits 1:0 = data; then the 10 bits are mirrored (V.c:3753-3774) */
static inline u16 vl_word(u8 tx0, u8 tx1)
{
	unsigned int src = ((tx1 & 3) << 8) | tx0, des = 0, i;

	for (i = 0; i < 10; i++)
		if (src & (1u << i))
			des |= 1u << (9 - i);
	return ((des & 0xff) << 8) | ((des >> 8) & 3);
}

static inline const char *vl_why(int r)
{
	switch (r) {
	case VL_E_FLAG:	return "PHY flag 0x80/0x40/0x10/0x08";
	case VL_E_START:return "6 frames without a 0xa8 first frame";
	case VL_E_HEAD:	return "frame head is not SVOOC";
	case VL_E_CMD:	return "adapter command out of order";
	case VL_E_ECHO:	return "adapter echo does not match";
	case VL_E_ID:	return "adapter id not in {4b 4c 4d 4e 65}";
	case VL_E_PROT:	return "protection register differs from the expected image: enable refused";
	case VL_E_VBUS:	return "VBUS window unusable";
	}
	return "?";
}

static inline void vl_ident_code(struct vl *s, u8 *l, u8 *h)
{
	u8 rl = s->rl, rh = s->rh;

	*l = ((rl >> 6) & 1) | (((rl >> 4) & 1) << 1) | (((rh >> 6) & 1) << 2) | ((rl & 1) << 3) | ((rh & 1) << 4);
	*h = ((rh >> 4) & 1) | (((rl >> 2) & 1) << 1);
	s->nx_l = ((rl >> 3) & 1) | (((rh >> 5) & 1) << 1) | (((rl >> 5) & 1) << 2) | (((rh >> 1) & 1) << 3) |
		  (((rh >> 2) & 1) << 4);
	s->nx_h = ((rl >> 1) & 1) | (((rh >> 3) & 1) << 1);
}

/*
 * One PHY interrupt: rx byte and flags are the two bytes of the word read at 0x2e.
 * Returns VL_NONE / VL_TX / VL_LAST, or a negative VL_E_* that ends the session.
 */
static inline int vl_irq(struct vl *s, u8 rx, u8 flags, u16 *tx)
{
	u8 mesg = (rx & 0x1e) >> 1, tx0 = 1, tx1 = 0, cmd;

	s->now = 0;
	if (flags & VF_ABORT)
		return VL_E_FLAG;
	if (!(flags & VF_RXDONE) || s->state >= VL_DONE)
		return VL_NONE;

	if (s->state == VL_WAIT) {
		if (rx != VL_FIRST)
			return ++s->ignored >= VL_MAX_IGNORE ? VL_E_START : VL_NONE;
		s->state = VL_LIVE;
	} else if ((rx & 0xe0) != 0x80) {
		return VL_E_HEAD;
	}

	cmd = s->pend ? s->pend : mesg;
	s->now = cmd;
	switch (cmd) {
	case VC_ASK:
		if (s->seen & 1) {		/* the second 0x04 is never answered */
			s->state = VL_STOP;
			return VL_NONE;
		}
		s->seen |= 1;
		tx0 |= 0x06 << 3;		/* R bits 0x06, allow bit clear */
		break;
	case VC_IDENT:
		if (!(s->seen & 1) || (s->seen & 2))
			return VL_E_CMD;
		s->seen |= 2;
		s->pend = VP_IDENT;
		s->n_id = 6;
		vl_ident_code(s, &tx0, &tx1);
		tx0 = (tx0 << 3) | 1;
		break;
	case VP_IDENT:
		switch (s->n_id) {
		case 5:
			tx0 |= s->nx_l << 3;
			tx1 = s->nx_h;
			break;
		case 4:
			s->far = (u16)mesg << 12;
			tx0 |= 0x20;
			break;
		case 3:
			s->far |= (u16)mesg << 8;
			tx0 |= 0x20;
			break;
		case 2:
			s->far |= (u16)mesg << 4;
			tx0 |= 0x20;
			break;
		default:
			s->far = (s->far | mesg) & 0xfefe;
			if (s->far != s->local)
				return VL_E_ECHO;
			tx0 |= 0x20;
			s->pend = 0;
			s->seen |= 4;
		}
		break;
	case VC_MODEL:
		if (!(s->seen & 4) || (s->seen & 8))
			return VL_E_CMD;
		s->seen |= 8;
		s->pend = VP_MODEL;
		s->n_model = 3;
		tx0 |= 0x10 | 0x40;
		break;
	case VP_MODEL:
		tx0 |= 0x20;
		if (s->n_model == 2) {
			s->model = mesg << 4;
		} else {
			s->model = (s->model | mesg) & 0x7f;
			if (!vl_id_ok(s->model))
				return VL_E_ID;
			s->pend = 0;
			s->seen |= 16;
		}
		break;
	case VC_BATT:
		if (!(s->seen & 16) || (s->seen & 32))
			return VL_E_CMD;
		s->seen |= 32;
		s->pend = VP_BATT;
		s->n_batt = 7;
		tx0 |= 0x0c << 3;
		break;
	case VP_BATT:
		if (mesg != vl_batt_echo[6 - s->n_batt])
			return VL_E_ECHO;
		tx0 |= (vl_batt_row[s->n_batt - 1] & 0x1f) << 3;
		tx1 = (vl_batt_row[s->n_batt - 1] >> 5) & 3;
		break;
	default:
		return VL_E_CMD;
	}

	*tx = vl_word(tx0, tx1);
	if (s->n_id)
		s->n_id--;
	else if (s->n_model)
		s->n_model--;
	else if (s->n_batt)
		s->n_batt--;
	if (cmd == VP_BATT && s->n_batt == 0) {
		s->pend = 0;
		s->state = VL_DONE;
		return VL_LAST;
	}
	return VL_TX;
}

/*
 * Pump write whitelist of the handshake: the init rows 1..19, the in-session rows and the exit rows.
 * Every pump write goes through vl_write_ok(); nothing outside this table can be written.
 * w = 1: byte write; w = 2: SMBus word write (low byte to reg, high byte to reg + 1).
 */
#define VW_INIT	1
#define VW_SESS	2
#define VW_EXIT	4

/* what a row is for: the host tests print it, the module does not carry it */
#ifdef __KERNEL__
#define VL_DOC(s)
#else
#define VL_DOC(s) s
#endif

struct vl_w {
	u8 reg, w, phase;
	u16 mask, val;
#ifndef __KERNEL__
	const char *what;
#endif
};

static const struct vl_w vl_wl[] = {
	{ 0x07, 1, VW_INIT,		0xff,   0x40,   VL_DOC("row 1: soft reset (CHG_EN is 0)") },
	{ 0x07, 1, VW_EXIT,		0xff,   0x00,   VL_DOC("exit: CHG_EN off") },
	{ 0x11, 1, VW_INIT | VW_EXIT,	0xff,   0x00,   VL_DOC("row 2: ADC off") },
	{ 0x02, 1, VW_INIT | VW_EXIT,	0xff,   0x01,   VL_DOC("row 3: VAC OVP") },
	{ 0x04, 1, VW_INIT | VW_EXIT,	0xff,   0x5a,   VL_DOC("row 4: VBUS OVP 10500 mV") },
	{ 0x00, 1, VW_INIT | VW_EXIT,	0xff,   0x36,   VL_DOC("row 5: VBAT OVP 4850 mV") },
	{ 0x05, 1, VW_INIT | VW_EXIT,	0xff,   0x2f,   VL_DOC("row 6: IBUS OCP 5700 mA, UCP on") },
	{ 0x01, 1, VW_INIT,		0xff,   0xbf,   VL_DOC("row 7: IBAT OCP") },
	{ 0x2b, 1, VW_INIT | VW_EXIT,	0xff,   0x00,   VL_DOC("row 8: VOOC PHY off") },
	{ 0x34, 1, VW_INIT,		0xff,   0x60,   VL_DOC("rows 9, 13: hold time") },
	{ 0x09, 1, VW_INIT | VW_EXIT,	0xff,   0x10,   VL_DOC("row 10: 2:1, UCP-rise masked, watchdog off") },
	{ 0x10, 1, VW_INIT | VW_EXIT,	0xff,   0x02,   VL_DOC("row 11: mask ADAPTER_INSERT") },
	{ 0x3b, 1, VW_INIT,		0xff,   0x04,   VL_DOC("OTG_EN back on after the reset, as stock (always_otg_en)") },
	{ 0x31, 2, VW_INIT | VW_EXIT,	0xffff, 0x0000, VL_DOC("row 12: predata 0 (0x31/0x32)") },
	{ 0x21, 1, VW_INIT,		0xff,   0x21,   VL_DOC("row 14: DM 20k pull-down, DPDM_EN") },
	{ 0x22, 1, VW_INIT,		0xff,   0x00,   VL_DOC("row 15: D+/D- buffers off") },
	{ 0x33, 1, VW_INIT,		0xff,   0xd1,   VL_DOC("row 16: VOOC timing") },
	{ 0x30, 1, VW_INIT,		0xff,   0x05,   VL_DOC("row 17: PHY interrupt mask") },
	{ 0x11, 1, VW_INIT | VW_SESS,	0xff,   0x80,   VL_DOC("row 18 / session: ADC on, continuous / unfreeze") },
	{ 0x2b, 1, VW_INIT,		0xff,   0x81,   VL_DOC("row 19: VOOC_EN + SEND_SEQ, the handshake") },
	{ 0x11, 1, VW_SESS,		0xff,   0xa0,   VL_DOC("session: ADC freeze") },
	{ 0x2c, 2, VW_SESS,		0x00ff, 0x0002, VL_DOC("session: TX word, 0x2c = 0x02, 0x2d = reply byte") },
	{ 0x2c, 2, VW_EXIT,		0xffff, 0x0000, VL_DOC("exit: clear TX (0x2c/0x2d)") },
	{ 0x21, 1, VW_EXIT,		0xff,   0xc0,   VL_DOC("exit: D+/D- pull-downs") },
	{ 0x30, 1, VW_EXIT,		0xff,   0x7f,   VL_DOC("exit: mask all PHY interrupts") },
};
#define VL_NW (sizeof(vl_wl) / sizeof(vl_wl[0]))

/* phases in which the write is allowed, 0 = refused */
static inline int vl_write_phase(u8 reg, u8 w, u16 val)
{
	unsigned int i;
	int ph = 0;

	if (w == 1 && val > 0xff)
		return 0;
	for (i = 0; i < VL_NW; i++)
		if (vl_wl[i].reg == reg && vl_wl[i].w == w && (val & vl_wl[i].mask) == vl_wl[i].val)
			ph |= vl_wl[i].phase;
	return ph;
}

static inline int vl_write_ok(u8 reg, u8 w, u16 val)
{
	return vl_write_phase(reg, w, val) != 0;
}

struct vl_row {
	u8 reg, w;
	u16 val;
};

/*
 * stock's init in stock's order, primary only. 0x3b = 0x04 (OTG_EN): the reset clears it, stock sets it on every init
 * (D.c:1165, always_otg_en) and before usb host mode (D.c:3781), and the tablet has never run with it clear
 */
static const struct vl_row vl_init_rows[] = {
	{ 0x07, 1, 0x40 }, { 0x11, 1, 0x00 }, { 0x02, 1, 0x01 }, { 0x04, 1, 0x5a }, { 0x00, 1, 0x36 },
	{ 0x05, 1, 0x2f }, { 0x01, 1, 0xbf }, { 0x2b, 1, 0x00 }, { 0x34, 1, 0x60 }, { 0x09, 1, 0x10 },
	{ 0x10, 1, 0x02 }, { 0x3b, 1, 0x04 }, { 0x31, 2, 0x0000 }, { 0x34, 1, 0x60 }, { 0x21, 1, 0x21 },
	{ 0x22, 1, 0x00 },
	{ 0x33, 1, 0xd1 }, { 0x30, 1, 0x05 }, { 0x11, 1, 0x80 },
};
/* row 19, the handshake, comes after the interrupt is armed */
static const struct vl_row vl_handshake = { 0x2b, 1, 0x81 };

/* the exit rows: stock's, with its 0x10 write moved to the end */
static const struct vl_row vl_exit_rows[] = {
	{ 0x07, 1, 0x00 }, { 0x09, 1, 0x10 }, { 0x00, 1, 0x36 }, { 0x02, 1, 0x01 }, { 0x04, 1, 0x5a },
	{ 0x05, 1, 0x2f }, { 0x11, 1, 0x00 }, { 0x2c, 2, 0x0000 }, { 0x30, 1, 0x7f }, { 0x21, 1, 0xc0 },
	{ 0x2b, 1, 0x00 }, { 0x31, 2, 0x0000 }, { 0x10, 1, 0x02 },
};

#define V3_CAP_MAX	13		/* 100 mA units: the maximum with pumps = 1 */
#define V3_HOLD_MAX_S	300
#define V3B_CAP_MAX	73		/* pumps = 2: stock's highest ask, 7.3 A total (live.dts svooc_charge_strategy 0x49) */
#define V3B_HOLD_MAX_S	600
#define V3_STOCK_ASK	73		/* stock's first ask-current reply: 7.3 A (voocphy_current_default, tx 0x4902) */
#define V3_CHG_CODE	0x7f		/* battery-voltage reply that asks the adapter for a new 0x03 (V.c:3381-3390, ap_need_change_current) */
#define V3_CHG_TRIES	5		/* stock sets ap_need_change_current = 5 and sends the code that many times at most */

enum { VB_LOW = 1, VB_HIGH, VB_NORMAL };		/* oplus_ap_voocphy.c vooc_vbus_status: LOW 1, HIGH 2, NORMAL 3 */
enum { V3_ASK, V3_PRE, V3_RUN };			/* second 0x04 pending / allowed, VBUS window / pump enabled */
#define V3A_ENABLE	1			/* write 0x07 = 0x80 before the TX word, as stock */
#define V3A_PREDATA	2			/* write the predata word r->predata after the TX word */
#define V3A_HW		4			/* hw_setting(SVOOC) rows, after the predata */

enum { VC3_BVOL = 1, VC3_VBUS = 2, VC3_CURR = 3, VC3_APST = 0x0a };	/* adapter commands after the allow reply */

/* module configuration bits for the write tables: the handshake's rows are always allowed */
#define VM_S2		1
#define VM_S3		2
#define VM_S3S		4			/* stock_start = 1 */
#define VM_S3B		8			/* pumps = 2: the secondary's table */

/* pump ADC block 0x13..0x20 (stock reads only 0x13..0x1c; TDIE comes from the 0x1f/0x20 pair as the reference driver reads it) */
struct vl_adc {
	u8 raw[14];
	int ibus, vbus, vac, vout, vbat, tdie;		/* mA, mV, mV, mV, mV, 0.1 C; integer math as stock */
};

static inline void vl_adc_decode(struct vl_adc *a, const u8 *b)
{
	unsigned int i;

	for (i = 0; i < 14; i++)
		a->raw[i] = b[i];
	a->ibus = (((b[0] & 0x0f) << 8) | b[1]) * 1875 / 1000;
	a->vbus = (((b[2] & 0x0f) << 8) | b[3]) * 375 / 100;
	a->vac = (((b[4] & 0x0f) << 8) | b[5]) * 5;
	a->vout = (((b[6] & 0x0f) << 8) | b[7]) * 125 / 100;
	a->vbat = (((b[8] & 0x0f) << 8) | b[9]) * 125 / 100;
	a->tdie = (((b[12] & 0x01) << 8) | b[13]) * 5;
}

/* the secondary pump's state, see the block at the end of this file */
struct vl3s {
	struct vl_adc adc;
	u8 adc_ok, prot_ok, hw_done, enabled, done, sw_seen, sw_now, checked, tick_init, tick_now, fresh;
	u8 n_lost, n_same, n_tot, n_low;
	u8 blk[17], fl_or[4];
	int t_en, next_tick;
	int ibus_max, tdie_max, tot_max, nrun;
};

struct vl3 {
	/* configuration */
	u8 cap, stock_ask, stock_start, pumps;
	u8 stage4, ask4;			/* stage 4: the ask the monitor currently allows (100 mA units); vl4_setup() */
	u8 lim4;				/* stage 4: the ask the total-current limit uses (vl4.lim: the adapter has had time to take it) */
	u16 vbat_hi;				/* stage 4: the pump's VBAT limit, 0 = V3_LIM_VBAT_MV */
	/* inputs the caller sets before vl3_irq() / vl3_post() */
	u8 suspend_ok;			/* the buck input suspend was confirmed */
	u8 prot_ok;			/* the last register image read matched (vl3_post sets it) */
	struct vl_adc adc;		/* the block read after the previous frame, as stock's cp_vbus / cp_vbat / cp_vsys / cp_ichg */
	u8 adc_ok;
	/* state */
	u8 ph, vbus_st, vb_err;
	u8 enabled, restored, sw_seen, i600;
	u8 n_ibus, n_vdiff, n_same, n_lost;
	u8 tx_pending, tx_previous, tx_retries;	/* changed-current reply awaiting the next PHY status */
	u8 sent, want, chg;		/* the last ask-current reply (0 = none yet), the last ask seen, change codes left for it */
	/* outputs of the last vl3_irq() */
	u8 act;
	u16 predata;
	/* statistics for the summary */
	int vdiff_min, vdiff_max, ibus_max, tdie_max, nrun;
	struct vl3s s;
};

static inline void vl3_init(struct vl3 *r, u8 cap, u8 stock_start)
{
	u8 *p = (u8 *)r;
	unsigned int i;

	for (i = 0; i < sizeof(*r); i++)
		p[i] = 0;
	r->cap = cap;
	r->stock_ask = V3_STOCK_ASK;
	r->stock_start = stock_start;
	r->pumps = 1;
	r->ph = V3_ASK;
}

/* oplus_vooc_vbus_vbatt_detect(), SVOOC: diff = vbus - 2 x vbat (vbat = gauge_vbatt = cp_vbat), thresholds 100 / 200 mV */
static inline int vl_window(int vbus, int vbat)
{
	int d = vbus - 2 * vbat;

	return d < 100 ? VB_LOW : d > 200 ? VB_HIGH : VB_NORMAL;
}

/* the 7-bit data field of an SVOOC reply (ask-current and battery voltage use the same bit order, V.c:3328-3341, :3548-3556) */
static inline u16 vl3_data_word(u8 v)
{
	u8 l = ((v >> 6) & 1) | (((v >> 5) & 1) << 1) | (((v >> 4) & 1) << 2) | (((v >> 3) & 1) << 3) | (((v >> 2) & 1) << 4);
	u8 h = ((v >> 1) & 1) | ((v & 1) << 1);

	return vl_word((l << 3) | 1, h);
}

/* stock's battery-voltage reply for this tablet (non-bidirectional cp, no parallel switch): V.c:3498-3500, 3548-3552 */
static inline int vl_batt_code2(const struct vl_adc *a, int ibus)
{
	int v1 = a->vbus / 2 - (a->vout - a->vbat) / 4 - ibus * 75 / 1000;

	if (v1 < 3404)
		return 0;
	if (v1 > 4654)
		return 125;
	return (v1 - 3404) / 10;
}

static inline int vl_batt_code(const struct vl_adc *a)
{
	return vl_batt_code2(a, a->ibus);
}

/* The stock AP PHY retries an isolated write failure after a changed CURR
 * reply. Preserve all other abort flags and the caller's request deadline.
 */
static inline int vl3_current_tx_status(struct vl3 *r, u8 *flags)
{
	if (!r->tx_pending)
		return 0;
	if ((*flags & VF_ABORT) == VF_TX_WRITE_FAIL && r->tx_retries < 3) {
		r->tx_pending = 0;
		r->tx_retries++;
		r->sent = r->tx_previous;
		r->chg = V3_CHG_TRIES;
		*flags &= ~VF_TX_WRITE_FAIL;
		return 1;
	}
	if ((*flags & (VF_RXDONE | VF_ABORT)) == VF_RXDONE) {
		r->tx_pending = 0;
		r->tx_retries = 0;
	}
	return 0;
}

/* the ask-current reply: the cap and stock's 7.3 A, in stage 4 also the monitor's current ask (fails low: 0 until it is set) */
static inline u8 vl3_ask(const struct vl3 *r)
{
	u8 a = r->cap < r->stock_ask ? r->cap : r->stock_ask;

	return r->stage4 && r->ask4 < a ? r->ask4 : a;
}

/* the total IBUS limit of both pumps: cap + 300 mA, in stage 4 the ask in force (lim4) + 300 mA when that is lower */
static inline int vl3_tot_lim(const struct vl3 *r)
{
	u8 c = r->stage4 && r->lim4 < r->cap ? r->lim4 : r->cap;

	return c * 100 + 300;
}

/*
 * One PHY interrupt of a powered session. Round 1 is vl_irq() unchanged. After the last battery-model row (state VL_DONE) the second 0x04
 * gets the allow reply only if the caller says the input suspend is confirmed (r->suspend_ok); otherwise it is not answered
 * (state VL_STOP, where a handshake-only session ends). Then: 0x0a, VBUS window answers (the first NORMAL asks for the enable), ask-current, battery voltage.
 */
static inline int vl3_irq(struct vl *s, struct vl3 *r, u8 rx, u8 flags, u16 *tx)
{
	u8 mesg = (rx & 0x1e) >> 1;
	int st;

	r->act = 0;
	if (s->state < VL_DONE)
		return vl_irq(s, rx, flags, tx);
	s->now = 0;
	if (flags & VF_ABORT)
		return VL_E_FLAG;
	if (!(flags & VF_RXDONE) || s->state != VL_DONE)
		return VL_NONE;
	if ((rx & 0xe0) != 0x80)
		return VL_E_HEAD;
	s->now = mesg;

	switch (mesg) {
	case VC_ASK:
		if (r->ph != V3_ASK)
			return VL_E_CMD;
		if (!r->suspend_ok) {
			s->state = VL_STOP;
			return VL_NONE;
		}
		r->ph = V3_PRE;
		r->predata = 0x2002;
		r->act = V3A_PREDATA | V3A_HW;
		*tx = vl_word(1 | (0x07 << 3), 0);
		return VL_TX;
	case VC3_APST:
		if (r->ph == V3_ASK)
			return VL_E_CMD;
		*tx = vl_word(1, 0);
		return VL_TX;
	case VC3_VBUS:
		if (r->ph == V3_ASK)
			return VL_E_CMD;
		if (!r->adc_ok)
			return VL_E_VBUS;
		st = vl_window(r->adc.vbus, r->adc.vbat);
		if (st == VB_NORMAL && (r->adc.vbus < 2000 || r->adc.vbat < 2000)) {	/* NORMAL_VBUS_MIN / NORMAL_VBAT_MIN, 3 times */
			if (r->vb_err >= 3)
				return VL_E_VBUS;
			r->vb_err++;
			st = VB_HIGH;
		} else {
			r->vb_err = 0;
		}
		r->vbus_st = st;
		if (st == VB_NORMAL && r->ph == V3_PRE) {
			if (!r->prot_ok)
				return VL_E_PROT;
			r->ph = V3_RUN;
			r->enabled = 1;
			r->act = V3A_ENABLE;
		}
		*tx = vl_word(1 | (st == VB_HIGH ? 0x10 : st == VB_LOW ? 0x20 : 0x30), 0);
		return VL_TX;
	case VC3_CURR:
		if (r->ph != V3_RUN)
			return VL_E_CMD;
		r->sent = vl3_ask(r);
		*tx = vl3_data_word(r->sent);
		return VL_TX;
	case VC3_BVOL:
		if (r->ph == V3_ASK || !r->adc_ok)
			return VL_E_CMD;
		/* stock's cp_ichg is the sum of both pumps once the secondary is on (V.c:3409, 1801; a secondary that is off counts 0): the reply, 7 codes lower, follows it */
		/* the adapter asks for current only when told to: a lower ask goes out as the change code, then its 0x03 (stock's V.c:3381) */
		if (vl3_ask(r) != r->want) {
			r->want = vl3_ask(r);
			r->chg = V3_CHG_TRIES;
		}
		/* OEM ap_need_change_current does not require an earlier CURR.
		 * In policy mode solicit the initial request too; otherwise both
		 * ramping and this notification wait forever for the other side.
		 */
		if ((r->sent || r->stage4) && r->want != r->sent && r->chg) {
			r->chg--;
			*tx = vl3_data_word(V3_CHG_CODE);
		} else {
			*tx = vl3_data_word(vl_batt_code2(&r->adc, r->adc.ibus + (r->pumps == 2 && r->s.enabled ? r->s.adc.ibus : 0)));
		}
		r->predata = *tx;
		r->act = V3A_PREDATA;
		return VL_TX;
	}
	return VL_E_CMD;
}

/* ---- write tables of the powered session: the handshake's table (vl_wl) stays valid, these are added ---- */

#define VW_HW		8
#define VW_RUN		16
#define VW_ENAB		32
#define VW_START	64
#define VW_LAD		128

/* the primary pump */
static const struct vl_w vl3_wl[] = {
	{ 0x31, 2, VW_HW | VW_RUN, 0x00ff, 0x0002, VL_DOC("predata word: 0x2002 at the allow frame, the reply word after battery-voltage replies") },
	{ 0x02, 1, VW_HW, 0xff, 0x01, VL_DOC("hw_setting: VAC OVP") },
	{ 0x04, 1, VW_HW, 0xff, 0x5a, VL_DOC("hw_setting: VBUS OVP 10500 mV") },
	{ 0x05, 1, VW_HW, 0xff, 0x2f, VL_DOC("hw_setting: IBUS OCP 5700 mA, UCP on") },
	{ 0x09, 1, VW_HW, 0xff, 0x13, VL_DOC("hw_setting: 2:1, UCP-rise masked, watchdog 1 s") },
	{ 0x11, 1, VW_HW, 0xff, 0x80, VL_DOC("hw_setting: ADC on") },
	{ 0x0d, 1, VW_HW, 0xff, 0x70, VL_DOC("hw_setting: PMID2OUT UVP 100 mV, OVP 500 mV") },
	{ 0x33, 1, VW_HW, 0xff, 0xd1, VL_DOC("hw_setting: VOOC timing") },
	{ 0x34, 1, VW_HW, 0xff, 0x60, VL_DOC("hw_setting: hold time") },
	{ 0x07, 1, VW_ENAB, 0xff, 0x80, VL_DOC("pump enable (only vl3_enable_ok(), from the one enable function)") },
};
#define VL3_NW (sizeof(vl3_wl) / sizeof(vl3_wl[0]))

/* stock_start = 1: stock's start pattern and its end */
static const struct vl_w vl3s_wl[] = {
	{ 0x05, 1, VW_START, 0xff, 0xaf, VL_DOC("start: UCP off (stock hw_setting step 9)") },
	{ 0x08, 1, VW_START, 0xff, 0x00, VL_DOC("start: soft-start timeout off") },
	{ 0x05, 1, VW_RUN, 0xff, 0x2f, VL_DOC("UCP back on when IBUS > 600 mA") },
	{ 0x08, 1, VW_RUN | VW_LAD, 0xff, 0xe0, VL_DOC("soft-start timeout 81.92 s back (also in the exit ladder if the start pattern was not undone)") },
};
#define VL3S_NW (sizeof(vl3s_wl) / sizeof(vl3s_wl[0]))

static inline int vl3_tab_phase(const struct vl_w *t, unsigned int n, u8 reg, u8 w, u16 val)
{
	unsigned int i;
	int ph = 0;

	if (w == 1 && val > 0xff)
		return 0;
	for (i = 0; i < n; i++)
		if (t[i].reg == reg && t[i].w == w && (val & t[i].mask) == t[i].val)
			ph |= t[i].phase;
	return ph;
}

/* phases in which a write is allowed in the given mode (VM_*), 0 = refused; 0x07 = 0x80 is not part of it (VW_ENAB) */
static inline int vl_write_phase_m(u8 reg, u8 w, u16 val, u8 mode)
{
	int ph = vl_write_phase(reg, w, val);

	if (mode & VM_S3)
		ph |= vl3_tab_phase(vl3_wl, VL3_NW, reg, w, val) & ~VW_ENAB;
	if ((mode & (VM_S3 | VM_S3S)) == (VM_S3 | VM_S3S))
		ph |= vl3_tab_phase(vl3s_wl, VL3S_NW, reg, w, val);
	return ph;
}

static inline int vl_write_ok_m(u8 reg, u8 w, u16 val, u8 mode)
{
	return vl_write_phase_m(reg, w, val, mode) != 0;
}

/* the enable: the powered stages only, the one row, from the one enable function */
static inline int vl3_enable_ok(u8 reg, u8 w, u16 val, u8 mode)
{
	return (mode & VM_S3) && reg == 0x07 && w == 1 && val == 0x80;
}

/* hw_setting(SVOOC) for the primary in stock's order, then stock's start pattern; the RMW reads are left out */
static const struct vl_row vl3_hw_rows[] = {
	{ 0x02, 1, 0x01 }, { 0x04, 1, 0x5a }, { 0x05, 1, 0x2f }, { 0x09, 1, 0x13 }, { 0x11, 1, 0x80 }, { 0x0d, 1, 0x70 },
	{ 0x33, 1, 0xd1 }, { 0x34, 1, 0x60 },
};
static const struct vl_row vl3_start_rows[] = { { 0x05, 1, 0xaf }, { 0x08, 1, 0x00 } };
static const struct vl_row vl3_restore_rows[] = { { 0x05, 1, 0x2f }, { 0x08, 1, 0xe0 } };
#define VL3_NHW		(sizeof(vl3_hw_rows) / sizeof(vl3_hw_rows[0]))
#define VL3_NSTART	2
#define VL3_NEXIT	(sizeof(vl_exit_rows) / sizeof(vl_exit_rows[0]))

/* total IBUS above 600 mA with the window NORMAL: stock turns UCP and the soft-start timeout back on (V.c:1805-1808) */
static inline int vl3_restore_due(const struct vl3 *r)
{
	return r->stock_start && r->enabled && !r->restored && r->vbus_st == VB_NORMAL && r->adc_ok && r->adc.ibus > 600;
}

/* ---- the register image read back before the enable and in every frame ---- */

/*
 * blk[reg] = the registers 0x00..0x10 read in one block (stock reads 0x06..0x0f every frame). 0 = as expected, else the register + 1.
 * Values: read back on this tablet after the init rows (0x00 0x36, 0x01 0xbf, 0x02 0x01, 0x04 0x5a, 0x05 0x2f, 0x08 0xe0,
 * 0x10 0x02); 0x09 0x13 and 0x0d 0x70 after hw_setting, 0x05 0xaf and 0x08 0x00 inside stock's start pattern.
 * Flag bits that stock's own blocks show set (0x09.5 UCP-rise at the start of the current, 0x02.4) are masked here and judged in vl3_post().
 */
static inline int vl3_image(const u8 *blk, int stock_start, int restored)
{
	int ucp_off = stock_start && !restored;

	if (blk[0x00] != 0x36)
		return 0x00 + 1;
	if (blk[0x01] != 0xbf)
		return 0x01 + 1;
	if ((blk[0x02] & 0xef) != 0x01)
		return 0x02 + 1;
	if (blk[0x04] != 0x5a)
		return 0x04 + 1;
	if (blk[0x05] != (ucp_off ? 0xaf : 0x2f))
		return 0x05 + 1;
	if (blk[0x08] != (ucp_off ? 0x00 : 0xe0))
		return 0x08 + 1;
	if ((blk[0x09] & 0x97) != 0x13)
		return 0x09 + 1;
	if ((blk[0x0d] & 0xf0) != 0x70)
		return 0x0d + 1;
	if (blk[0x10] != 0x02)
		return 0x10 + 1;
	return 0;
}

enum {
	V3E_NONE, V3E_FLAGS, V3E_IMAGE, V3E_IDLE, V3E_CHGEN, V3E_VERR, V3E_LOST, V3E_VBUS, V3E_IBUS_MAX, V3E_IBUS_CAP,
	V3E_VDIFF, V3E_TDIE, V3E_VBAT, V3E_STALE,
	V3E_SFLAGS, V3E_SIMAGE, V3E_SIDLE, V3E_SCHGEN, V3E_SVERR, V3E_SLOST, V3E_SIBUS_MAX, V3E_SCURR, V3E_STDIE, V3E_SSTALE,
	V3E_TOTAL, V3E_STROUBLE, V3E_SLOW,
};
#define V3_ENABLE_GRACE_MS	100	/* stock: 0x07 reads 0x00 half a millisecond after the enable and 0x80 45 ms later */
#define V3_LIM_VBUS_MV		10000	/* after the allow bit; stock's pump OVP is 10500 */
#define V3_LIM_VBUS_PRE_MV	6000	/* until then */
#define V3_LIM_VDIFF_MV		500
/*
 * stock's VBUS - 2 x VBAT grows with the pump's current (in its trace, by primary IBUS: max 492 mV at 1.5..2 A, 747 at
 * 2.5..3 A, 970 at 3.5..4 A): 500 mV up to 1 A, then 0.30 mV per mA. The closest stock came to the limit is 251 mV under it
 */
static inline int vl3_vdiff_max(int ibus)
{
	return V3_LIM_VDIFF_MV + (ibus > 1000 ? (ibus - 1000) * 3 / 10 : 0);
}
#define V3_LIM_TDIE_DC		700
#define V3_LIM_VBAT_MV		4450
#define V3_LIM_IBUS_MAX_MA	5300	/* stock's voocphy_cp_max_ibus, per pump */
#define V3_STALE_N		6	/* stock's longest run of identical ADC blocks is 2 (91483 blocks) */

static inline const char *vl3_why(int c)
{
	switch (c) {
	case V3E_FLAGS:		return "pump fault flag (0x06 0xc9 / 0x09.3 / 0x0d 0x0c / 0x0e 0xf8 / 0x0f 0xfc)";
	case V3E_IMAGE:		return "protection register differs from the expected image";
	case V3E_IDLE:		return "pump not idle before the enable";
	case V3E_CHGEN:		return "CHG_EN (0x07.7) clear while enabled";
	case V3E_VERR:		return "VBUS error status (0x06 0x30) while enabled";
	case V3E_LOST:		return "pump stopped switching (0x06.2 clear on 3 reads)";
	case V3E_VBUS:		return "VBUS ADC above its limit";
	case V3E_IBUS_MAX:	return "IBUS above 5300 mA";
	case V3E_IBUS_CAP:	return "IBUS above cap + 300 mA on 3 reads";
	case V3E_VDIFF:		return "VBUS - 2 x VBAT above 500 mV + 0.30 mV per mA of IBUS over 1 A, on 3 reads";
	case V3E_TDIE:		return "pump die temperature above 70.0 C";
	case V3E_VBAT:		return "pump VBAT above 4450 mV";
	case V3E_STALE:		return "ADC block identical on 6 reads";
	case V3E_SFLAGS:	return "secondary pump fault flag (0x06 0xc9 / 0x09.3 / 0x0d 0x0c / 0x0e 0xf8 / 0x0f 0xfc)";
	case V3E_SIMAGE:	return "secondary protection register differs from the expected image";
	case V3E_SIDLE:		return "secondary pump not idle before its enable";
	case V3E_SCHGEN:	return "secondary CHG_EN (0x07.7) clear while enabled";
	case V3E_SVERR:		return "secondary VBUS error status (0x06 0x30) while enabled";
	case V3E_SLOST:		return "secondary pump stopped switching (0x06.2 clear on 3 reads)";
	case V3E_SIBUS_MAX:	return "secondary IBUS above 5300 mA";
	case V3E_SCURR:		return "secondary IBUS above 600 mA while it is not enabled";
	case V3E_STDIE:		return "secondary die temperature above 70.0 C";
	case V3E_SSTALE:	return "secondary ADC block identical on 6 reads";
	case V3E_TOTAL:		return "total IBUS (both pumps) above cap + 300 mA on 3 reads";
	case V3E_STROUBLE:	return "secondary pump in trouble after its enable (stock's slave check)";
	case V3E_SLOW:		return "total IBUS below 1100 mA on 4 ticks with the secondary on (stock would stop only the secondary)";
	}
	return "?";
}

/* the fault flags of one pump's block blk[0x06..0x0f]; the same masks for both pumps */
static inline int vl3_flags(const u8 *blk)
{
	return (blk[0x06] & 0xc9) || (blk[0x09] & 0x08) || (blk[0x0d] & 0x0c) || (blk[0x0e] & 0xf8) || (blk[0x0f] & 0xfc);
}

/*
 * After every frame (IRQ thread, after the reply): the flag/protection block blk[0x00..0x10] and the ADC block a, ms_on = ms since the
 * enable write. Returns 0 or a V3E_*; counters live in r.
 */
static inline int vl3_post(struct vl3 *r, const u8 *blk, const struct vl_adc *a, int ms_on)
{
	unsigned int i;
	int img, same = 1, vdiff;

	r->prot_ok = 0;
	if (vl3_flags(blk))
		return V3E_FLAGS;
	img = vl3_image(blk, r->stock_start, r->restored);
	r->prot_ok = img == 0;
	if (img)
		return V3E_IMAGE;
	if (!r->enabled) {
		if (!vl_pump_idle(blk[0x06], blk[0x07]))
			return V3E_IDLE;
	} else if (ms_on >= V3_ENABLE_GRACE_MS) {
		if (!(blk[0x07] & 0x80))
			return V3E_CHGEN;
		if (blk[0x06] & 0x30)
			return V3E_VERR;
	}
	if (r->enabled) {
		if (blk[0x06] & 0x04) {
			r->sw_seen = 1;
			r->n_lost = 0;
		} else if (r->sw_seen && ++r->n_lost >= 3) {
			return V3E_LOST;
		}
	}
	if (a->vbus > (r->ph == V3_ASK ? V3_LIM_VBUS_PRE_MV : V3_LIM_VBUS_MV))
		return V3E_VBUS;
	if (a->tdie > V3_LIM_TDIE_DC)
		return V3E_TDIE;
	if (a->vbat > (r->vbat_hi ? r->vbat_hi : V3_LIM_VBAT_MV))
		return V3E_VBAT;
	if (a->ibus > V3_LIM_IBUS_MAX_MA)
		return V3E_IBUS_MAX;
	if (!r->enabled)
		return 0;

	vdiff = a->vbus - 2 * a->vbat;
	if (!r->nrun || vdiff < r->vdiff_min)
		r->vdiff_min = vdiff;
	if (!r->nrun || vdiff > r->vdiff_max)
		r->vdiff_max = vdiff;
	if (a->ibus > r->ibus_max)
		r->ibus_max = a->ibus;
	if (a->tdie > r->tdie_max)
		r->tdie_max = a->tdie;
	r->nrun++;
	if (a->ibus > 600)
		r->i600 = 1;

	if (a->ibus > r->cap * 100 + 300) {
		if (++r->n_ibus >= 3)
			return V3E_IBUS_CAP;
	} else {
		r->n_ibus = 0;
	}
	if (vdiff > vl3_vdiff_max(a->ibus)) {
		if (++r->n_vdiff >= 3)
			return V3E_VDIFF;
	} else {
		r->n_vdiff = 0;
	}
	for (i = 0; i < 14; i++)
		if (a->raw[i] != r->adc.raw[i])
			same = 0;
	if (r->adc_ok && same) {
		if (++r->n_same >= V3_STALE_N - 1)
			return V3E_STALE;
	} else {
		r->n_same = 0;
	}
	return 0;
}

struct vl3_io {
	void *c;
	int (*wr)(void *c, u8 reg, u8 w, u16 val);	/* checked pump write, < 0 on error */
	int (*rd)(void *c, u8 reg, u8 n, u8 *buf);	/* block read, returns n or < 0 */
	void (*dly)(void *c, int ms);
	int (*en)(void *c);				/* the secondary's enable (C glue: lock, end check, the one write); optional */
	int (*stop)(void *c);				/* non-zero once an end reason is set; optional */
};
/* stock's secondary logic, all of it from the curr event handler (V.c:5382-5500), a 1 s monitor event (VOOC_CURR_EVENT_TIME, 1000):
 * the first run is at the monitor start = the primary's enable, then 1 s after the previous run ended */
#define V3B_TICK_MS		1000
#define V3B_ENABLE_MA		1400	/* oplus_spec,slave_cp_enable_thr = 0x578 (live.dts:26314), compared with cp_ichg = primary + secondary ADC (V.c:1801) */
#define V3B_LOW_MA		1100	/* oplus_spec,slave_cp_disable_thr_high = 0x44c (live.dts:26315): below it on 4 ticks stock disables the secondary */
#define V3B_LOW_TICKS		4	/* disable_sub_cp_count 0, 1, 2, 3, then the disable (V.c:5480-5496) */
#define V3B_MIN_MA		500	/* oplus,slave_cp_enable_thr_low, not in live.dts: default 500 (V.c:6804): slave IBUS below it = trouble */
#define V3B_DEV_MA		1000	/* oplus,cp_ibus_devation, not in live.dts: DEFAULT_CP_IBUS_DEVATION 1000: |primary - secondary| above it = trouble */
#define V3B_STATUS_DELAY_MS	300	/* OPLUS_VOOCPHY_SLAVE_CHECK_DELAY_MS (V.c:2816) */
#define V3B_STATUS_MS		3000	/* 60 x OPLUS_VOOCPHY_SLAVE_STATUS_CHECK_DELAY_MS (50 ms), V.c:2817-2818 */
#define V3B_PUMP_MAX_MA		5300	/* oplus_spec,voocphy_cp_max_ibus = 0x14b4 (live.dts:26337), per pump (V.c:6398) */
#define V3B_IDLE_MA		600	/* a pump that is not enabled carries less (stock: at most 2 mA before the secondary's enable) */

enum { V3S_RD = 1, V3S_WR, V3S_EN };		/* negative returns of the secondary's functions: read failed, write failed / refused, enable not written */

/* the secondary's write table (stock's whole session: no word write, no other register). Byte rows only; phases as the primary's */
static const struct vl_w vl3b_wl[] = {
	{ 0x07, 1, VW_INIT,			0xff, 0x41, VL_DOC("init: soft reset, bit 0 as stock's slave") },
	{ 0x07, 1, VW_EXIT,			0xff, 0x01, VL_DOC("exit: CHG_EN off, bit 0 kept (D.c:2228-2266)") },
	{ 0x07, 1, VW_ENAB,			0xff, 0x81, VL_DOC("secondary enable (only vl3b_enable_ok(), from the one enable function)") },
	{ 0x11, 1, VW_INIT | VW_EXIT | VW_SESS,	0xff, 0x00, VL_DOC("ADC off; round 1: unfreeze with the ADC off") },
	{ 0x11, 1, VW_SESS,			0xff, 0x20, VL_DOC("round 1: ADC freeze with the ADC off") },
	{ 0x11, 1, VW_HW | VW_RUN,		0xff, 0x80, VL_DOC("hw_setting: ADC on / unfreeze") },
	{ 0x11, 1, VW_RUN,			0xff, 0xa0, VL_DOC("ADC freeze for the block read") },
	{ 0x02, 1, VW_INIT | VW_HW | VW_EXIT,	0xff, 0x01, VL_DOC("VAC OVP") },
	{ 0x04, 1, VW_INIT | VW_HW | VW_EXIT,	0xff, 0x5a, VL_DOC("VBUS OVP 10500 mV") },
	{ 0x00, 1, VW_INIT | VW_EXIT,		0xff, 0x36, VL_DOC("VBAT OVP 4850 mV") },
	{ 0x05, 1, VW_INIT | VW_HW | VW_EXIT,	0xff, 0x2e, VL_DOC("IBUS OCP 5400 mA, UCP on (stock's secondary, DT ocp_reg 0xe)") },
	{ 0x01, 1, VW_INIT | VW_EXIT,		0xff, 0xbf, VL_DOC("IBAT OCP") },
	{ 0x2b, 1, VW_INIT | VW_EXIT,		0xff, 0x00, VL_DOC("VOOC PHY off") },
	{ 0x34, 1, VW_INIT | VW_HW | VW_EXIT,	0xff, 0x60, VL_DOC("hold time") },
	{ 0x09, 1, VW_INIT | VW_EXIT,		0xff, 0x10, VL_DOC("2:1, UCP-rise masked, watchdog off") },
	{ 0x09, 1, VW_HW,			0xff, 0x13, VL_DOC("hw_setting: watchdog 1 s armed") },
	{ 0x10, 1, VW_INIT | VW_EXIT,		0xff, 0x02, VL_DOC("mask ADAPTER_INSERT") },
	{ 0x0d, 1, VW_HW,			0xff, 0x70, VL_DOC("hw_setting: PMID2OUT UVP 100 mV, OVP 500 mV") },
	{ 0x33, 1, VW_HW,			0xff, 0xd1, VL_DOC("hw_setting: VOOC timing") },
};
#define VL3B_NW (sizeof(vl3b_wl) / sizeof(vl3b_wl[0]))

/* the secondary's write check: its own table, only with pumps=2. Nothing of the primary's tables is consulted */
static inline int vl3b_write_phase(u8 reg, u8 w, u16 val, u8 mode)
{
	if (!(mode & VM_S3B))
		return 0;
	return vl3_tab_phase(vl3b_wl, VL3B_NW, reg, w, val) & ~VW_ENAB;
}

static inline int vl3b_write_ok(u8 reg, u8 w, u16 val, u8 mode)
{
	return vl3b_write_phase(reg, w, val, mode) != 0;
}

static inline int vl3b_enable_ok(u8 reg, u8 w, u16 val, u8 mode)
{
	return (mode & VM_S3B) && reg == 0x07 && w == 1 && val == 0x81;
}

/* stock's secondary sequence in stock's order, RMW reads left out */
static const struct vl_row vl3b_init_rows[] = {		/* after the primary's init, before the handshake */
	{ 0x07, 1, 0x41 }, { 0x11, 1, 0x00 }, { 0x02, 1, 0x01 }, { 0x04, 1, 0x5a }, { 0x00, 1, 0x36 }, { 0x05, 1, 0x2e },
	{ 0x01, 1, 0xbf }, { 0x2b, 1, 0x00 }, { 0x34, 1, 0x60 }, { 0x09, 1, 0x10 }, { 0x10, 1, 0x02 },
};
static const struct vl_row vl3b_hw_rows[] = {		/* after the primary's hw rows and start pattern */
	{ 0x02, 1, 0x01 }, { 0x04, 1, 0x5a }, { 0x05, 1, 0x2e }, { 0x09, 1, 0x13 }, { 0x11, 1, 0x80 }, { 0x0d, 1, 0x70 },
	{ 0x33, 1, 0xd1 }, { 0x34, 1, 0x60 },
};
static const struct vl_row vl3b_adc1_rows[] = { { 0x11, 1, 0x20 }, { 0x11, 1, 0x00 } };	/* round 1, around the 0x13 read */
static const struct vl_row vl3b_adc_rows[] = { { 0x11, 1, 0xa0 }, { 0x11, 1, 0x80 } };	/* from the allow frame on */
/*
 * stock's exit: 07=01, 09 read-modify-write (0x33 -> 0x30: bit 5 is the UCP-rise flag), then stock's reset rows. This
 * table writes 0x10 for that row (same effect, a value stock writes on this chip). Stock repeats the whole set once more 120 ms later;
 * not repeated here.
 */
static const struct vl_row vl3b_exit_rows[] = {
	{ 0x07, 1, 0x01 }, { 0x09, 1, 0x10 }, { 0x11, 1, 0x00 }, { 0x02, 1, 0x01 }, { 0x04, 1, 0x5a }, { 0x00, 1, 0x36 },
	{ 0x05, 1, 0x2e }, { 0x01, 1, 0xbf }, { 0x2b, 1, 0x00 }, { 0x34, 1, 0x60 }, { 0x09, 1, 0x10 }, { 0x10, 1, 0x02 },
};
#define VL3B_NINIT	(sizeof(vl3b_init_rows) / sizeof(vl3b_init_rows[0]))
#define VL3B_NHW	(sizeof(vl3b_hw_rows) / sizeof(vl3b_hw_rows[0]))
#define VL3B_NEXIT	(sizeof(vl3b_exit_rows) / sizeof(vl3b_exit_rows[0]))

/* writes the rows in order through the bus's own checked write; stops at the first failure (-1) or when an end reason is set (-2) */
static inline int vl3_rows(const struct vl3_io *io, const struct vl_row *rw, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (io->stop && io->stop(io->c))
			return -2;
		if (io->wr(io->c, rw[i].reg, rw[i].w, rw[i].val) < 0)
			return -1;
	}
	return 0;
}

static inline int vl3b_init(const struct vl3_io *io)
{
	return vl3_rows(io, vl3b_init_rows, VL3B_NINIT);
}

static inline int vl3b_hw(const struct vl3_io *io, struct vl3 *r)
{
	int rc = vl3_rows(io, vl3b_hw_rows, VL3B_NHW);

	if (!rc)
		r->s.hw_done = 1;
	return rc;
}

/* the secondary's register image, read back right before its enable and in every frame after the allow frame (0x08 is never written or read by stock: not judged) */
static inline int vl3b_image(const u8 *blk)
{
	if (blk[0x00] != 0x36)
		return 0x00 + 1;
	if (blk[0x01] != 0xbf)
		return 0x01 + 1;
	if ((blk[0x02] & 0xef) != 0x01)
		return 0x02 + 1;
	if (blk[0x04] != 0x5a)
		return 0x04 + 1;
	if (blk[0x05] != 0x2e)
		return 0x05 + 1;
	if ((blk[0x09] & 0x97) != 0x13)
		return 0x09 + 1;
	if ((blk[0x0d] & 0xf0) != 0x70)
		return 0x0d + 1;
	if (blk[0x10] != 0x02)
		return 0x10 + 1;
	return 0;
}

/* the secondary's flags, image (after the allow frame), idle / CHG_EN / switching. blk = registers 0x00..0x10 as the primary's */
static inline int vl3b_pre(struct vl3 *r, const u8 *blk, int now)
{
	struct vl3s *s = &r->s;
	int img;

	s->prot_ok = 0;
	s->fl_or[0] |= blk[0x06];
	s->fl_or[1] |= blk[0x09];
	s->fl_or[2] |= blk[0x0d] | blk[0x0e];
	s->fl_or[3] |= blk[0x0f];
	if (vl3_flags(blk))
		return V3E_SFLAGS;
	if (r->ph != V3_ASK) {
		img = vl3b_image(blk);
		s->prot_ok = img == 0;
		if (img)
			return V3E_SIMAGE;
	}
	s->sw_now = (blk[0x06] & 0x04) && (blk[0x07] & 0x80);		/* stock's slave get_cp_status (D.c:2356) */
	if (!s->enabled)
		return vl_pump_idle(blk[0x06], blk[0x07]) ? 0 : V3E_SIDLE;
	if (now - s->t_en >= V3_ENABLE_GRACE_MS) {
		if (!(blk[0x07] & 0x80))
			return V3E_SCHGEN;
		if (blk[0x06] & 0x30)
			return V3E_SVERR;
	}
	if (blk[0x06] & 0x04) {
		s->sw_seen = 1;
		s->n_lost = 0;
	} else if (s->sw_seen && ++s->n_lost >= 3) {
		return V3E_SLOST;
	}
	return 0;
}

/* the 1 s tick of stock's curr event, from the primary's enable on; 1 = this frame is a tick */
static inline int vl3b_tick(struct vl3 *r, int now)
{
	struct vl3s *s = &r->s;

	s->tick_now = 0;
	if (!r->enabled)
		return 0;
	if (!s->tick_init) {
		s->tick_init = 1;
		s->next_tick = now;
	}
	if (now < s->next_tick)
		return 0;
	s->next_tick = now + V3B_TICK_MS;
	s->tick_now = 1;
	return 1;
}

/*
 * The secondary's enable, stock's condition (V.c:5430-5435) with this module's added ones: at a tick, the window NORMAL, total IBUS above
 * 1400 mA (stock's get_ichg of a pump that is not enabled is 0: the primary alone), the secondary not enabled, and never twice (stock's
 * slave_trouble_count <= 1 retry is not taken: one enable per session); plus pumps=2, the primary enabled, switching, its image ok, the
 * input suspend confirmed, the secondary's hw rows written and its own image read back in this frame. "No end reason set" is the C glue's.
 */
static inline int vl3b_due(struct vl3 *r, int now)
{
	struct vl3s *s = &r->s;

	vl3b_tick(r, now);
	if (!s->tick_now || r->pumps != 2 || s->enabled || s->done)
		return 0;
	return r->sw_seen && !r->n_lost && r->prot_ok && s->prot_ok && s->hw_done && r->suspend_ok && r->vbus_st == VB_NORMAL && r->adc_ok &&
	       r->adc.ibus > V3B_ENABLE_MA;
}

/* the secondary's ADC block (14 bytes as the primary's; its VBAT pin is not wired, so VBAT / VOUT / VBUS are not judged); r->adc is this frame's primary block */
static inline int vl3b_adc(struct vl3 *r, const struct vl_adc *a, int now)
{
	struct vl3s *s = &r->s;
	unsigned int i;
	int tot, same = 1, d;

	if (a->tdie > V3_LIM_TDIE_DC)
		return V3E_STDIE;
	if (a->ibus > V3B_PUMP_MAX_MA)
		return V3E_SIBUS_MAX;
	if (!s->enabled)
		return a->ibus > V3B_IDLE_MA ? V3E_SCURR : 0;

	tot = r->adc.ibus + a->ibus;
	if (a->ibus > s->ibus_max)
		s->ibus_max = a->ibus;
	if (a->tdie > s->tdie_max)
		s->tdie_max = a->tdie;
	if (tot > s->tot_max)
		s->tot_max = tot;
	s->nrun++;
	if (tot > vl3_tot_lim(r)) {
		if (++s->n_tot >= 3)
			return V3E_TOTAL;
	} else {
		s->n_tot = 0;
	}
	if (!s->checked && now - s->t_en >= V3B_STATUS_DELAY_MS) {
		d = r->adc.ibus > a->ibus ? r->adc.ibus - a->ibus : a->ibus - r->adc.ibus;
		if (a->ibus >= V3B_MIN_MA && s->sw_now && d <= V3B_DEV_MA)
			s->checked = 1;
		else if (now - s->t_en >= V3B_STATUS_DELAY_MS + V3B_STATUS_MS)
			return V3E_STROUBLE;
	}
	if (s->checked && s->tick_now) {
		if (tot < V3B_LOW_MA) {
			if (++s->n_low >= V3B_LOW_TICKS)
				return V3E_SLOW;
		} else {
			s->n_low = 0;
		}
	}
	for (i = 0; i < 14; i++)
		if (a->raw[i] != s->adc.raw[i])
			same = 0;
	if (s->adc_ok && same) {
		if (++s->n_same >= V3_STALE_N - 1)
			return V3E_SSTALE;
	} else {
		s->n_same = 0;
	}
	return 0;
}

/*
 * What the IRQ thread does for the secondary after each frame's reply, after the primary's own post. Returns 0, a V3E_* limit, or a
 * negative -V3S_*. Round 1 (before the allow reply): stock's freeze rows with the ADC off, flags and idle. After it: the register
 * image, the enable decision (before this frame's ADC read, as stock's tick acts between frames), stock's freeze rows, the ADC block.
 */
static inline int vl3b_run(const struct vl3_io *io, struct vl3 *r, int now)
{
	struct vl3s *s = &r->s;
	u8 blk[17], b[14];
	struct vl_adc a;
	int rc, n, i;

	for (i = 0; i < 17; i++)
		blk[i] = 0;
	if (r->ph == V3_ASK) {
		if (io->rd(io->c, 0x06, 10, blk + 6) != 10)
			return -V3S_RD;
		rc = vl3b_pre(r, blk, now);
		if (vl3_rows(io, vl3b_adc1_rows, 1) < 0)
			return -V3S_WR;
		n = io->rd(io->c, 0x13, 2, b);
		if (vl3_rows(io, vl3b_adc1_rows + 1, 1) < 0)
			return -V3S_WR;
		if (n != 2)
			return -V3S_RD;
		/* the ADC is off in round 1 (stock's 11=00 / 11=20): 0x13 holds the last conversion from whenever it last ran (seen here: android's
		 * last fast charge, 901 mA at 9.3 V). stock reads it and ignores it; idle is judged from 0x06 / 0x07 */
		return rc;
	}
	if (io->rd(io->c, 0x06, 10, blk + 6) != 10 || io->rd(io->c, 0x04, 2, blk + 4) != 2 || io->rd(io->c, 0x00, 3, blk) != 3 ||
	    io->rd(io->c, 0x10, 1, blk + 16) != 1)
		return -V3S_RD;
	for (i = 0; i < 17; i++)
		s->blk[i] = blk[i];
	rc = vl3b_pre(r, blk, now);
	if (rc)
		return rc;
	if (vl3b_due(r, now)) {
		s->done = 1;
		if (!io->en || io->en(io->c) < 0)
			return -V3S_EN;
		s->enabled = 1;
		s->t_en = now;
	}
	if (vl3_rows(io, vl3b_adc_rows, 1) < 0)
		return -V3S_WR;
	n = io->rd(io->c, 0x13, 14, b);
	if (vl3_rows(io, vl3b_adc_rows + 1, 1) < 0)
		return -V3S_WR;
	if (n != 14)
		return -V3S_RD;
	vl_adc_decode(&a, b);
	/* the allow frame's read comes ~2 ms after the hw rows switched the ADC on; the chip needs ~10 ms to replace the old conversion */
	rc = s->fresh ? vl3b_adc(r, &a, now) : 0;
	s->fresh = 1;
	s->adc = a;
	s->adc_ok = 1;
	return rc;
}

#endif /* VOOC_LOGIC_H */
