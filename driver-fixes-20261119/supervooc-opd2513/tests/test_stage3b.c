/*
 * Host test for the stage 3b part of vooc_logic.h (both pumps). cc -I.. test_stage3b.c -o test_stage3b && ./test_stage3b
 * (./test_stage3b --tables prints the write tables of both buses). test_logic.c (stage 2) and test_stage3.c (3a) are separate.
 * Data: capture_round1.h and capture_dual.h, generated from the stock capture: both buses of the
 * pumps from the first frame of round 1 to 18913.7, the init and the end. What the capture does not contain is synthesised here and
 * marked "synthetic": the secondary's 0x06..0x0f block (its 0x0f and 0x07 are real), the registers 0x00..0x05 and 0x10 of both pumps,
 * TDIE (0x1f / 0x20) and the secondary's ADC bytes beyond IBUS (taken from the primary's block of the same frame, the pumps share VBUS).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vooc_logic.h>
#include "capture_round1.h"
#include "capture_dual.h"

static int fails, checks;

#define CHECK(c, ...) do { \
	checks++; \
	if (!(c)) { \
		fails++; \
		printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #c); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

#define M3B (VM_S2 | VM_S3 | VM_S3S | VM_S3B)
/* the replay ends before frame 72 (18913.2422): there stock changes its own ask (battery-voltage reply 0x7f02, then ask-current 0x2802 = 4.0 A),
 * which this module never does. Frames 0..71 are stock's session with the ask at 7.3 A */
#define D_LAST 72

/* ---- two simulated buses: every write is checked against that bus's own table, as pw_raw() / ps_raw() do ---- */

struct bus {
	int n, refused, rd_fail, stuck, fail_reg, fail_left, fail_skip;
	int seq[1024];
	struct d_w w[1024];
	u8 enable[1024];
	u8 r06, r07;
};
static struct bus pb, sb;
static int g_seq;

static int bus_wr(struct bus *b, int sec, u8 reg, u8 w, u16 val, int enable)
{
	int ok = sec ? (enable ? vl3b_enable_ok(reg, w, val, M3B) : vl3b_write_ok(reg, w, val, M3B)) :
		       (enable ? vl3_enable_ok(reg, w, val, M3B) : vl_write_ok_m(reg, w, val, M3B));

	if (!ok) {
		b->refused++;
		return -1;
	}
	if (b->n < 1024) {
		b->seq[b->n] = g_seq++;
		b->w[b->n].reg = reg;
		b->w[b->n].w = w;
		b->w[b->n].val = val;
		b->enable[b->n++] = enable;
	}
	if (reg == b->fail_reg) {
		if (b->fail_skip > 0) {
			b->fail_skip--;
		} else if (b->fail_left > 0) {
			b->fail_left--;
			return -5;
		}
	}
	if (reg == 0x07 && !enable && !b->stuck && (val == 0x00 || val == 0x01)) {
		b->r07 &= 0x7f;
		b->r06 &= ~0x04;
	}
	return 0;
}

static void bus_reset(struct bus *b)
{
	memset(b, 0, sizeof(*b));
	b->fail_reg = -1;
	b->r06 = 0x06;
	b->r07 = 0x81;
}

/* what the secondary's reads return; the tests set it */
static struct {
	u8 blk[17];
	u8 adc[14];
	int fail_rd;		/* register whose read fails, -1 none */
	int en_calls, en_fail, stop;
	int en_primary_ok;	/* the primary state at the enable call */
	const struct vl3 *r;
} syn;

static int pio_wr(void *c, u8 reg, u8 w, u16 val) { (void)c; return bus_wr(&pb, 0, reg, w, val, 0); }
static int sio_wr(void *c, u8 reg, u8 w, u16 val) { (void)c; return bus_wr(&sb, 1, reg, w, val, 0); }

static int pio_rd(void *c, u8 reg, u8 n, u8 *b)
{
	(void)c;
	if (pb.rd_fail)
		return -5;
	CHECK(reg == 0x06 && n == 2, "primary ladder read %02x/%d", reg, n);
	b[0] = pb.r06;
	b[1] = pb.r07;
	return 2;
}

static int sio_rd(void *c, u8 reg, u8 n, u8 *b)
{
	(void)c;
	if (sb.rd_fail || syn.fail_rd == reg)
		return -5;
	if (reg == 0x06 && n == 2) {			/* the ladder's confirmation */
		b[0] = sb.r06;
		b[1] = sb.r07;
		return 2;
	}
	if (reg == 0x06 && n == 10)
		memcpy(b, syn.blk + 6, 10);
	else if (reg == 0x04 && n == 2)
		memcpy(b, syn.blk + 4, 2);
	else if (reg == 0x00 && n == 3)
		memcpy(b, syn.blk, 3);
	else if (reg == 0x10 && n == 1)
		b[0] = syn.blk[16];
	else if (reg == 0x13 && (n == 14 || n == 2))
		memcpy(b, syn.adc, n);
	else {
		CHECK(0, "unexpected secondary read %02x/%d", reg, n);
		return -5;
	}
	return n;
}

static int sio_en(void *c)
{
	const struct vl3 *r = syn.r;

	(void)c;
	syn.en_calls++;
	if (r)
		syn.en_primary_ok = r->pumps == 2 && r->enabled && r->sw_seen && !r->n_lost && r->prot_ok && r->suspend_ok && r->vbus_st == VB_NORMAL && r->s.prot_ok &&
				    r->s.hw_done && r->adc.ibus > 1400;
	if (syn.en_fail)
		return -1;
	return bus_wr(&sb, 1, 0x07, 1, 0x81, 1);
}

static int sio_stop(void *c) { (void)c; return syn.stop; }
static void nodly(void *c, int ms) { (void)c; (void)ms; }

static const struct vl3_io pio = { 0, pio_wr, pio_rd, nodly, 0, 0 };
static const struct vl3_io sio = { 0, sio_wr, sio_rd, nodly, sio_en, sio_stop };

/* ---- helpers shared by the tests ---- */

static void adc_bytes(u8 *b, int ibus_raw, int vbus_raw, int vbat_raw, int tdie_raw, int seed)
{
	memset(b, 0, 14);
	b[0] = ibus_raw >> 8; b[1] = ibus_raw;
	b[2] = vbus_raw >> 8; b[3] = vbus_raw;
	b[8] = vbat_raw >> 8; b[9] = vbat_raw;
	b[10] = seed;			/* the 4 bytes of IBAT / unused: a moving byte so that blocks differ */
	b[12] = tdie_raw >> 8; b[13] = tdie_raw;
}

/* the secondary's blocks as they read when everything is fine (synthetic: 0x00..0x05, 0x10 and the 0x06..0x0f block) */
static void good_sblk(u8 *blk, int enabled)
{
	memset(blk, 0, 17);
	blk[0x00] = 0x36; blk[0x01] = 0xbf; blk[0x02] = 0x01; blk[0x04] = 0x5a; blk[0x05] = 0x2e;
	blk[0x06] = enabled ? 0x06 : 0x02;
	blk[0x07] = enabled ? 0x81 : 0x07;
	blk[0x08] = 0xe0; blk[0x09] = 0x13; blk[0x0d] = 0x70; blk[0x0e] = 0x03; blk[0x10] = 0x02;
}

/* a dual running state: the primary enabled and switching, the secondary enabled at t = 0 and past its status check */
static void run2(struct vl3 *r, int cap)
{
	vl3_init(r, cap, 0);
	r->pumps = 2;
	r->suspend_ok = 1;
	r->ph = V3_RUN;
	r->enabled = 1;
	r->vbus_st = VB_NORMAL;
	r->adc_ok = 1;
	r->sw_seen = 1;
	r->prot_ok = 1;
	vl_adc_decode(&r->adc, (u8[14]){ 0 });
	r->adc.ibus = 700;
	r->s.hw_done = 1;
	r->s.enabled = 1;
	r->s.done = 1;
	r->s.t_en = 0;
	r->s.sw_seen = 1;
	r->s.checked = 1;
	r->s.tick_init = 1;
	r->s.next_tick = 1000000;
	r->s.prot_ok = 1;
	r->s.fresh = 1;
	good_sblk(syn.blk, 1);
	adc_bytes(syn.adc, 373, 2090, 3040, 64, 1);	/* 700 mA, 7.8 V, 32.0 C */
	r->s.adc_ok = 0;
}

static int frame2(struct vl3 *r, int now, int seed)
{
	syn.adc[10] = seed;
	syn.r = r;
	return vl3b_run(&sio, r, now);
}

/* ---- 1: the capture, both buses, round 1 to 18913.7 ---- */

static void filt_p(const struct d_frame *f, int k, int ss, int cap, struct d_w *out, int *n)
{
	int i, m = 0, cmd = k > 17 ? ((f->rx & 0x1e) >> 1) : 0;		/* ask-current exists after the allow frame (index 17) only */

	for (i = 0; i < f->npw; i++) {
		struct d_w w = f->pw[i];

		if (!ss && ((w.reg == 0x05 && w.val == 0xaf) || (w.reg == 0x08 && w.val == 0x00)))
			continue;
		if (!ss && i >= 2 && i == f->npw - 2 && w.reg == 0x05 && w.val == 0x2f && f->pw[i + 1].reg == 0x08 && f->pw[i + 1].val == 0xe0 &&
		    f->pw[i - 1].reg == 0x11 && f->pw[i - 1].val == 0x80)
			break;
		if (cap != V3_STOCK_ASK && w.reg == 0x2c && cmd == 3)
			w.val = (cap << 8) | 0x0002;
		out[m++] = w;
	}
	*n = m;
}

static void dual_replay(int cap, int ss, int *end_k, int *end_code, int *en_k)
{
	struct vl s;
	struct vl3 r;
	struct vl_adc a;
	static struct d_w exp_s[D_LAST * 20];
	struct d_w exp_p[20];
	int k, i, n_exp_s = 0, rc, t_en = -1, ndiff = 0, sen_cap = -1, nexp;
	u8 blk[17], b14[14];
	u8 r05 = 0x2f;

	bus_reset(&pb);
	bus_reset(&sb);
	g_seq = 0;
	memset(&syn, 0, sizeof(syn));
	syn.fail_rd = -1;
	*end_k = *end_code = *en_k = -1;
	vl_init(&s, CAP_RL, CAP_RH);
	vl3_init(&r, cap, ss);
	r.pumps = 2;
	r.suspend_ok = 1;

	for (k = 0; k < D_LAST; k++)
		for (i = 0; i < df[k].nsw; i++) {
			if (df[k].sw[i].reg == 0x07 && df[k].sw[i].val == 0x81)
				sen_cap = k;
			exp_s[n_exp_s++] = df[k].sw[i];
		}

	for (k = 0; k < D_LAST; k++) {
		const struct d_frame *f = &df[k];
		int now = f->t_us / 1000 + 5, p0 = pb.n;
		u16 tx = 0;
		int allow;

		rc = vl3_irq(&s, &r, f->rx, f->fl, &tx);
		CHECK(rc == VL_TX || rc == VL_LAST, "frame %d (rx 0x%02x) returned %d", k, f->rx, rc);
		if (rc < 0)
			break;
		allow = !!(r.act & V3A_HW);

		/* the primary's side, as the C file does it (s3_tx, s3_after, check_pump / s3_post) */
		if (r.act & V3A_ENABLE) {
			bus_wr(&pb, 0, 0x07, 1, 0x80, 1);
			t_en = f->t_us;
		}
		bus_wr(&pb, 0, 0x2c, 2, tx, 0);
		if (r.act & V3A_PREDATA)
			bus_wr(&pb, 0, 0x31, 2, r.predata, 0);
		if (allow) {
			for (i = 0; i < (int)VL3_NHW; i++)
				bus_wr(&pb, 0, vl3_hw_rows[i].reg, 1, vl3_hw_rows[i].val, 0);
			for (i = 0; ss && i < VL3_NSTART; i++)
				bus_wr(&pb, 0, vl3_start_rows[i].reg, 1, vl3_start_rows[i].val, 0);
			CHECK(vl3b_hw(&sio, &r) == 0, "secondary hw rows");
		}
		bus_wr(&pb, 0, 0x11, 1, 0xa0, 0);
		bus_wr(&pb, 0, 0x11, 1, 0x80, 0);
		if (r.ph != V3_ASK) {
			memset(blk, 0, 17);
			blk[0x00] = 0x36; blk[0x01] = 0xbf; blk[0x02] = 0x01; blk[0x04] = 0x5a; blk[0x10] = 0x02;	/* synthetic */
			memcpy(blk + 6, f->pblk, 10);
			if (f->r05)
				r05 = f->r05;
			blk[0x05] = ss ? r05 : 0x2f;
			if (!ss && blk[0x08] == 0x00)
				blk[0x08] = 0xe0;
			memcpy(b14, f->padc, 10);
			memset(b14 + 10, 0, 4);					/* TDIE: synthetic 0 C */
			vl_adc_decode(&a, b14);
			rc = vl3_post(&r, blk, &a, t_en < 0 ? 0 : (f->t_us - t_en) / 1000);
			CHECK(rc == 0 || cap != V3_STOCK_ASK, "frame %d: the primary's checks ended stock's own run: %s", k, vl3_why(rc));
			r.adc = a;
			r.adc_ok = 1;
			if (rc && *end_k < 0) {
				*end_k = k;
				*end_code = rc;
			}
			if (vl3_restore_due(&r)) {
				bus_wr(&pb, 0, 0x05, 1, 0x2f, 0);
				bus_wr(&pb, 0, 0x08, 1, 0xe0, 0);
				r.restored = 1;
			}
		}

		/* the secondary: its reads from the capture where the capture has them */
		{
			u8 sib[14];

			good_sblk(syn.blk, 0);
			memcpy(syn.blk + 6, f->pblk, 10);					/* synthetic: the primary's block of this frame ... */
			{
				int sen = 0, sib_ma = (((f->s13[0] & 0x0f) << 8) | f->s13[1]) * 1875 / 1000;

				for (i = 0; i < sb.n; i++)
					sen |= sb.enable[i];
				syn.blk[0x06] = sen ? (sib_ma > 100 ? 0x06 : 0x02) : (f->pblk[0] & ~0x04);	/* switching once it carries current */
				syn.blk[0x07] = f->s07 >= 0 ? (u8)f->s07 : sen ? 0x81 : 0x07;
			}
			syn.blk[0x0f] = f->s0f;							/* ... with the secondary's real 0x07 and 0x0f */
			syn.blk[0x04] = 0x5a; syn.blk[0x05] = 0x2e; syn.blk[0x00] = 0x36; syn.blk[0x01] = 0xbf; syn.blk[0x02] = 0x01; syn.blk[0x10] = 0x02;
			if (r.ph != V3_ASK) {
				syn.blk[0x09] = 0x13;
				syn.blk[0x0d] = 0x70;
			}
			memcpy(sib, f->padc, 10);
			sib[0] = f->s13[0];
			sib[1] = f->s13[1];
			memset(sib + 10, 0, 4);
			sib[12] = 0; sib[13] = 64;							/* synthetic TDIE 32.0 C */
			memcpy(syn.adc, sib, 14);
			if (f->s06 >= 0 && (syn.blk[0x07] & 0x80))
				CHECK(syn.blk[0x06] == f->s06, "frame %d: synthetic secondary 0x06 0x%02x, capture 0x%02x", k, syn.blk[0x06], f->s06);
			syn.r = &r;
			rc = vl3b_run(&sio, &r, now);
			CHECK(rc == 0 || cap != V3_STOCK_ASK, "frame %d (uptime 18910.%06u): the secondary's checks ended stock's own run: %s (%d)", k, f->t_us, rc > 0 ? vl3_why(rc) : "i2c", rc);
			if (rc && *end_k < 0) {
				*end_k = k;
				*end_code = rc;
			}
			if (r.s.enabled && *en_k < 0)
				*en_k = k;
		}

		/* the primary's writes of this frame against the capture's, in order */
		filt_p(f, k, ss, cap, exp_p, &nexp);
		CHECK(pb.n - p0 == nexp, "frame %d: %d primary writes, capture %d", k, pb.n - p0, nexp);
		for (i = 0; i < nexp && i < pb.n - p0; i++)
			if (pb.w[p0 + i].reg != exp_p[i].reg || pb.w[p0 + i].w != exp_p[i].w || pb.w[p0 + i].val != exp_p[i].val) {
				CHECK(0, "frame %d primary write %d: ours %02x/%d=%04x, capture %02x/%d=%04x", k, i, pb.w[p0 + i].reg, pb.w[p0 + i].w, pb.w[p0 + i].val,
				      exp_p[i].reg, exp_p[i].w, exp_p[i].val);
				ndiff++;
				break;
			}
		if (*end_k >= 0)
			break;
	}

	if (*end_k < 0) {
		/* the secondary's whole stream against the capture's (the enable falls in the next frame's window: the stream is what is compared) */
		CHECK(sb.n == n_exp_s, "secondary: %d writes, capture %d", sb.n, n_exp_s);
		for (i = 0; i < sb.n && i < n_exp_s; i++)
			if (sb.w[i].reg != exp_s[i].reg || sb.w[i].w != exp_s[i].w || sb.w[i].val != exp_s[i].val) {
				CHECK(0, "secondary write %d: ours %02x=%02x, capture %02x=%02x", i, sb.w[i].reg, sb.w[i].val, exp_s[i].reg, exp_s[i].val);
				break;
			}
		CHECK(sen_cap >= 0 && *en_k == sen_cap + 1, "secondary enable: ours in frame %d, stock's write in the window of frame %d", *en_k, sen_cap);
		CHECK(sb.refused == 0 && pb.refused == 0, "refused writes: primary %d secondary %d", pb.refused, sb.refused);
	}
	(void)ndiff;
}

static void replays(void)
{
	int k, c, e;

	dual_replay(V3_STOCK_ASK, 1, &k, &c, &e);			/* stock's own ask, stock_start verbatim: both buses equal the capture */
	CHECK(k < 0 && e >= 0, "verbatim replay ended at frame %d code %d", k, c);
	dual_replay(V3_STOCK_ASK, 0, &k, &c, &e);			/* stock_start = 0: minus the start and UCP-back rows */
	CHECK(k < 0 && e >= 0, "stock_start=0 replay ended at frame %d code %d", k, c);
	dual_replay(14, 0, &k, &c, &e);					/* cap 14: only the ask-current word differs; stock's real current (1.76 A with both pumps) is over 1.4 A + 300 mA */
	CHECK(k >= 0 && c == V3E_TOTAL, "cap 14 over stock's current: frame %d code %d", k, c);
	dual_replay(20, 0, &k, &c, &e);					/* cap 20: the current stays below 2.3 A in this window: no limit, streams equal */
	CHECK(k < 0 && e >= 0, "cap 20 ended at frame %d code %d", k, c);
}

/* the total / primary A5 rule on the capture's own current, computed here from the raw bytes (independent of vl3_post / vl3b_adc): the first frame
 * on which three consecutive reads exceed cap + 300 mA ends the run, for the primary alone and for the sum once the secondary is enabled */
static void total_oracle(void)
{
	int cap, k, c, e, n_p, n_t, first, sen = -1, pen = -1, fired = 0, quiet = 0;

	for (k = 0; k < D_LAST; k++)
		for (e = 0; e < df[k].nsw; e++)
			if (df[k].sw[e].reg == 0x07 && df[k].sw[e].val == 0x81)
				sen = k + 1;				/* the module's enable frame, see dual_replay() */
	for (k = 0; k < D_LAST && pen < 0; k++)
		for (e = 0; e < df[k].npw; e++)
			if (df[k].pw[e].reg == 0x07 && df[k].pw[e].val == 0x80)
				pen = k;
	CHECK(sen > 0 && pen > 17, "no secondary enable in the capture");
	for (cap = 14; cap <= 30; cap += 2) {
		int lim = cap * 100 + 300;

		first = -1;
		n_p = n_t = 0;
		for (k = 0; k < D_LAST && first < 0; k++) {
			int p = (((df[k].padc[0] & 0x0f) << 8) | df[k].padc[1]) * 1875 / 1000;
			int s = (((df[k].s13[0] & 0x0f) << 8) | df[k].s13[1]) * 1875 / 1000;

			if (k < pen)
				continue;			/* the pump is not enabled before the frame stock enabled it */
			n_p = p > lim ? n_p + 1 : 0;
			n_t = k >= sen && p + s > lim ? n_t + 1 : 0;
			if (n_p >= 3 || n_t >= 3)
				first = k;
		}
		dual_replay(cap, 0, &k, &c, &e);
		fired += first >= 0;
		quiet += first < 0;
		if (first >= 0)
			CHECK(k == first, "cap %d: oracle frame %d, module frame %d code %d", cap, first, k, c);
		else
			CHECK(k < 0, "cap %d: oracle says no limit, module ended at %d code %d", cap, k, c);
	}
	CHECK(fired >= 1 && quiet >= 1, "oracle: %d caps end, %d do not", fired, quiet);
}

static void due_ready(struct vl3 *r, int cap)
{
	vl3_init(r, cap, 0);
	r->pumps = 2;
	r->suspend_ok = 1;
	r->ph = V3_RUN;
	r->enabled = 1;
	r->vbus_st = VB_NORMAL;
	r->adc_ok = 1;
	r->sw_seen = 1;
	r->prot_ok = 1;
	r->adc.ibus = 1401;
	r->s.hw_done = 1;
	r->s.prot_ok = 1;
	r->s.tick_init = 1;
	r->s.next_tick = 5000;
}

static void enable_conditions(void)
{
	struct vl3 r;
	int i, n;

	due_ready(&r, 30);
	CHECK(vl3b_due(&r, 5000) == 1, "all conditions met");
	CHECK(r.s.next_tick == 6000 && r.s.tick_now, "the tick is consumed and the next one is 1000 ms later");
	CHECK(vl3b_due(&r, 5000) == 0 && vl3b_due(&r, 5999) == 0, "no second evaluation inside the tick");
	r.s.done = 0;
	CHECK(vl3b_due(&r, 6000) == 1, "next tick");

	due_ready(&r, 30);
	CHECK(vl3b_due(&r, 4999) == 0 && vl3b_due(&r, 5000) == 1, "tick time");
	for (n = 1398; n <= 1403; n++) {				/* stock: cp_ichg > 1400 */
		due_ready(&r, 30);
		r.adc.ibus = n;
		CHECK(vl3b_due(&r, 5000) == (n > 1400), "total %d mA", n);
	}
	due_ready(&r, 30);					/* a tick that fails the condition is spent */
	r.adc.ibus = 1400;
	CHECK(vl3b_due(&r, 5000) == 0, "1400 mA");
	r.adc.ibus = 1500;
	CHECK(vl3b_due(&r, 5999) == 0 && vl3b_due(&r, 6000) == 1, "failed tick spent, next at +1000");

	for (i = 0; i < 12; i++) {					/* every other condition, alone */
		due_ready(&r, 30);
		switch (i) {
		case 0: r.pumps = 1; break;
		case 1: r.enabled = 0; break;
		case 2: r.sw_seen = 0; break;
		case 3: r.n_lost = 1; break;
		case 4: r.prot_ok = 0; break;
		case 5: r.s.prot_ok = 0; break;
		case 6: r.s.hw_done = 0; break;
		case 7: r.suspend_ok = 0; break;
		case 8: r.vbus_st = VB_HIGH; break;
		case 9: r.adc_ok = 0; break;
		case 10: r.s.done = 1; break;
		case 11: r.s.enabled = 1; break;
		}
		CHECK(vl3b_due(&r, 5000) == 0, "condition %d missing: still due", i);
		if (i == 8) {
			due_ready(&r, 30);
			r.vbus_st = VB_LOW;
			CHECK(vl3b_due(&r, 5000) == 0, "window LOW");
		}
	}
	due_ready(&r, 30);					/* the first tick is at the primary's enable (stock: the monitor start) */
	r.s.tick_init = 0;
	r.adc.ibus = 3;
	CHECK(vl3b_due(&r, 777) == 0 && r.s.next_tick == 1777 && r.s.tick_init, "first tick at the first post after the primary's enable");
	due_ready(&r, 30);
	r.enabled = 0;
	r.s.tick_init = 0;
	CHECK(vl3b_due(&r, 777) == 0 && !r.s.tick_init, "no tick before the primary is enabled");
}

/* through vl3b_run: the enable call, once, and only with the primary running; fuzz */
static void enable_once(void)
{
	struct vl3 r;
	int n, k, bad_en = 0, twice = 0;

	srand(11);
	for (n = 0; n < 3000; n++) {
		int now = 0, calls = 0;

		bus_reset(&sb);
		memset(&syn, 0, sizeof(syn));
		syn.fail_rd = -1;
		due_ready(&r, 1 + rand() % 30);
		r.ph = V3_PRE;
		r.s.tick_init = 0;
		for (k = 0; k < 80; k++) {
			int ib;

			now += 20 + rand() % 60;
			r.enabled = rand() % 8 != 0;
			r.sw_seen = rand() % 8 != 0;
			r.n_lost = rand() % 8 == 0;
			r.prot_ok = rand() % 8 != 0;
			r.suspend_ok = rand() % 8 != 0;
			r.vbus_st = rand() % 8 == 0 ? VB_HIGH : VB_NORMAL;
			r.adc.ibus = 1300 + rand() % 200;
			r.s.hw_done = rand() % 8 != 0;
			good_sblk(syn.blk, r.s.enabled);
			if (rand() % 10 == 0)
				syn.blk[0x05] = 0x2f;
			ib = r.s.enabled ? 700 : 1;
			adc_bytes(syn.adc, ib * 1000 / 1875, 2090, 3040, 64, rand());
			syn.r = &r;
			syn.en_fail = rand() % 20 == 0;
			vl3b_run(&sio, &r, now);
			if (r.s.enabled && !calls && syn.en_calls)
				calls = syn.en_calls;
			if (syn.en_calls > 1)
				twice++;
			if (syn.en_calls && !syn.en_primary_ok)
				bad_en++;
			if (r.s.done)
				r.s.enabled = r.s.enabled;
			if (sb.n) {		/* the secondary can only be enabled by a write of 0x07 = 0x81 that came through the enable path */
				int i, e = 0;

				for (i = 0; i < sb.n; i++)
					e += sb.enable[i];
				if (e > 1)
					twice++;
			}
		}
	}
	CHECK(!twice, "the secondary was enabled twice in %d sessions", twice);
	CHECK(!bad_en, "the enable was called %d times without every condition", bad_en);
	/* a refused enable, or a failed one, still counts: never again */
	bus_reset(&sb);
	memset(&syn, 0, sizeof(syn));
	syn.fail_rd = -1;
	due_ready(&r, 30);
	r.ph = V3_PRE;
	good_sblk(syn.blk, 0);
	adc_bytes(syn.adc, 1, 2090, 3040, 64, 1);
	syn.en_fail = 1;
	n = vl3b_run(&sio, &r, 5000);
	CHECK(n == -V3S_EN && r.s.done && !r.s.enabled && syn.en_calls == 1, "enable failed: rc %d", n);
	syn.en_fail = 0;
	r.s.next_tick = 5000;
	good_sblk(syn.blk, 0);
	n = vl3b_run(&sio, &r, 5000);
	CHECK(syn.en_calls == 1 && !r.s.enabled, "no second try after a failed enable");
	/* pumps=1 never reaches the secondary's enable even with every condition true */
	bus_reset(&sb);
	due_ready(&r, 30);
	r.pumps = 1;
	r.ph = V3_PRE;
	memset(&syn, 0, sizeof(syn));
	syn.fail_rd = -1;
	good_sblk(syn.blk, 0);
	adc_bytes(syn.adc, 1, 2090, 3040, 64, 1);
	vl3b_run(&sio, &r, 5000);
	CHECK(!syn.en_calls && !sb.enable[0], "pumps=1: enable called");
	/* the enable is refused by the write table in any other mode and by io_wr in every mode */
	CHECK(!vl3b_enable_ok(0x07, 1, 0x81, VM_S2 | VM_S3 | VM_S3S) && vl3b_enable_ok(0x07, 1, 0x81, M3B) && !vl3b_write_ok(0x07, 1, 0x81, M3B), "enable table");
}

/* ---- 3: the secondary's limits, each crossed ---- */

static void limits2(void)
{
	struct vl3 r;
	int i, b, n, rc, now;
	static const struct { int reg; u8 mask; } fl[] = {
		{ 0x06, 0x80 }, { 0x06, 0x40 }, { 0x06, 0x08 }, { 0x06, 0x01 }, { 0x09, 0x08 }, { 0x0d, 0x08 }, { 0x0d, 0x04 }, { 0x0e, 0x80 }, { 0x0e, 0x40 },
		{ 0x0e, 0x20 }, { 0x0e, 0x10 }, { 0x0e, 0x08 }, { 0x0f, 0x80 }, { 0x0f, 0x40 }, { 0x0f, 0x20 }, { 0x0f, 0x10 }, { 0x0f, 0x08 }, { 0x0f, 0x04 },
	};

	memset(&syn, 0, sizeof(syn));
	syn.fail_rd = -1;
	bus_reset(&sb);
	run2(&r, 20);
	CHECK(frame2(&r, 500, 2) == 0, "quiet state");

	for (i = 0; i < (int)(sizeof(fl) / sizeof(fl[0])); i++) {		/* L1 flags: enabled, not enabled, round 1 */
		int st;

		for (st = 0; st < 3; st++) {
			run2(&r, 20);
			r.s.enabled = st == 0;
			r.ph = st == 2 ? V3_ASK : V3_RUN;
			good_sblk(syn.blk, st == 0);
			syn.blk[fl[i].reg] |= fl[i].mask;
			rc = st == 2 ? vl3b_run(&sio, &r, 500) : frame2(&r, 500, 2);
			CHECK(rc == V3E_SFLAGS, "flag %02x.%02x state %d: %d", fl[i].reg, fl[i].mask, st, rc);
		}
	}
	run2(&r, 20);						/* flags stock's own blocks show are no faults: 0x09 0x33, 0x0f 0x02, 0x06 0x22 */
	syn.blk[0x09] = 0x33;
	syn.blk[0x0f] = 0x02;
	CHECK(frame2(&r, 500, 2) == 0, "UCP-rise flag");
	run2(&r, 20);
	r.s.enabled = 0;
	r.ph = V3_ASK;
	good_sblk(syn.blk, 0);
	syn.blk[0x06] = 0x22;
	memset(syn.adc, 0, 14);
	CHECK(vl3b_run(&sio, &r, 500) == 0, "round 1: 0x06 = 0x22 is idle");

	for (i = 0; i < 17; i++) {					/* the image: exactly these bits are judged; a flipped bit that is no flag must end it as SIMAGE, an unjudged one must not */
		static const u8 judged[17] = { [0x00] = 0xff, [0x01] = 0xff, [0x02] = 0xef, [0x04] = 0xff, [0x05] = 0xff, [0x09] = 0x97, [0x0d] = 0xf0, [0x10] = 0xff };
		static const u8 flagm[17] = { [0x09] = 0x08, [0x0d] = 0x0c, [0x0e] = 0xf8, [0x0f] = 0xfc, [0x06] = 0xc9 };

		if (i == 0x06 || i == 0x07 || i == 0x0e || i == 0x0f)
			continue;
		for (b = 0; b < 8; b++) {
			run2(&r, 20);
			syn.blk[i] ^= 1 << b;
			rc = frame2(&r, 500, 2);
			if (flagm[i] >> b & 1)
				CHECK(rc == V3E_SFLAGS, "reg %02x bit %d is a flag: %d", i, b, rc);
			else if (judged[i] >> b & 1)
				CHECK(rc == V3E_SIMAGE, "reg %02x bit %d is judged: %d", i, b, rc);
			else
				CHECK(rc == 0, "reg %02x bit %d is not judged: %d", i, b, rc);
		}
	}
	run2(&r, 20);
	syn.blk[0x05] = 0x2f;
	CHECK(frame2(&r, 500, 2) == V3E_SIMAGE, "the primary's 0x05 on the secondary");
	run2(&r, 20);
	syn.blk[0x09] = 0x10;
	CHECK(frame2(&r, 500, 2) == V3E_SIMAGE, "0x09 watchdog not armed");
	run2(&r, 20);
	r.ph = V3_ASK;
	syn.blk[0x05] = 0x00;				/* round 1: the image is not judged yet */
	syn.blk[0x07] = 0x07; syn.blk[0x06] = 0x02;
	memset(syn.adc, 0, 14);
	r.s.enabled = 0;
	CHECK(vl3b_run(&sio, &r, 500) == 0, "round 1 without an image check");

	for (n = 0; n <= 400; n += 20) {			/* CHG_EN and the VBUS error, 100 ms of grace */
		run2(&r, 20);
		syn.blk[0x07] = 0x01;
		rc = frame2(&r, n, 2);
		CHECK(rc == (n >= 100 ? V3E_SCHGEN : 0), "CHG_EN clear %d ms after the enable: %d", n, rc);
		run2(&r, 20);
		syn.blk[0x06] |= 0x10;
		rc = frame2(&r, n, 2);
		CHECK(rc == (n >= 100 ? V3E_SVERR : 0), "VBUS error %d ms after the enable: %d", n, rc);
	}
	run2(&r, 20);					/* before the enable: not idle */
	r.s.enabled = 0;
	syn.blk[0x07] = 0x87;
	CHECK(frame2(&r, 500, 2) == V3E_SIDLE, "CHG_EN before the enable");
	run2(&r, 20);
	r.s.enabled = 0;
	syn.blk[0x06] = 0x06; syn.blk[0x07] = 0x07;
	CHECK(frame2(&r, 500, 2) == V3E_SIDLE, "switching before the enable");
	run2(&r, 20);					/* stopped switching: 3 reads, after it was seen */
	syn.blk[0x06] = 0x02;
	CHECK(frame2(&r, 500, 2) == 0 && frame2(&r, 540, 3) == 0, "two reads without switching");
	CHECK(frame2(&r, 580, 4) == V3E_SLOST, "third read without switching");
	run2(&r, 20);
	r.s.sw_seen = 0;
	syn.blk[0x06] = 0x02;
	for (i = 0; i < 20; i++)
		CHECK(frame2(&r, 500 + 40 * i, i) == 0, "never switched yet: no LOST");
	run2(&r, 20);					/* two reads without switching, switching again, two again: the count restarts */
	for (i = 0; i < 12; i++) {
		syn.blk[0x06] = i % 3 == 2 ? 0x06 : 0x02;
		CHECK(frame2(&r, 500 + 40 * i, i) == 0, "lost count restarts (read %d)", i);
	}

	for (n = 2810; n <= 2840; n++) {			/* per pump IBUS 5300 mA (raw * 1.875) */
		run2(&r, 30);
		r.s.checked = 1;
		adc_bytes(syn.adc, n, 2090, 3040, 64, 2);
		rc = frame2(&r, 500, 2);
		{
			int ib = n * 1875 / 1000;

			CHECK((rc == V3E_SIBUS_MAX) == (ib > 5300), "secondary IBUS %d: %d", ib, rc);
		}
	}
	for (n = 130; n <= 150; n++) {				/* TDIE 70.0 C = raw 140 */
		run2(&r, 30);
		adc_bytes(syn.adc, 373, 2090, 3040, n, 2);
		rc = frame2(&r, 500, 2);
		CHECK((rc == V3E_STDIE) == (n * 5 > 700), "secondary TDIE %d.%d: %d", n * 5 / 10, n * 5 % 10, rc);
	}
	for (n = 320; n <= 330; n++) {				/* a pump that is not enabled carries less than 600 mA */
		run2(&r, 30);
		r.s.enabled = 0;
		good_sblk(syn.blk, 0);
		adc_bytes(syn.adc, n, 2090, 3040, 64, 2);
		rc = frame2(&r, 500, 2);
		CHECK((rc == V3E_SCURR) == (n * 1875 / 1000 > 600), "idle secondary at %d mA: %d", n * 1875 / 1000, rc);
		run2(&r, 30);
		r.s.enabled = 0;
		r.ph = V3_ASK;
		good_sblk(syn.blk, 0);
		memset(syn.adc, 0, 14);
		syn.adc[0] = n >> 8; syn.adc[1] = n;
		rc = vl3b_run(&sio, &r, 500);
		CHECK(rc == 0, "round 1: the ADC is off, its latched %d mA is not judged: %d", n * 1875 / 1000, rc);
	}
	/* seen on the tablet: the secondary's ADC still held android's last fast charge (0x13/0x14 = 01 e1, 901 mA; 0x15/0x16 = 09 b1, 9.3 V) */
	run2(&r, 30);
	r.s.enabled = 0;
	r.ph = V3_ASK;
	good_sblk(syn.blk, 0);
	memset(syn.adc, 0, 14);
	syn.adc[0] = 0x01; syn.adc[1] = 0xe1; syn.adc[2] = 0x09; syn.adc[3] = 0xb1;
	CHECK(vl3b_run(&sio, &r, 500) == 0, "round 1: android's latched 901 mA is not judged");
	run2(&r, 30);
	r.s.enabled = 0;
	r.s.fresh = 0;
	good_sblk(syn.blk, 0);
	CHECK(frame2(&r, 500, 2) == 0, "allow frame: the read right after the ADC switch-on (still 901 mA) is not judged");
	CHECK(r.s.fresh == 1, "allow frame: the next read is judged");
	CHECK(frame2(&r, 547, 3) == V3E_SCURR, "next frame: 901 mA on a secondary that is not enabled ends the session");
	/* the secondary's own VBAT / VBUS / VOUT bytes are not judged: its VBAT pin is not wired */
	run2(&r, 30);
	adc_bytes(syn.adc, 373, 4000, 4095, 64, 2);
	syn.adc[6] = 0x0f; syn.adc[7] = 0xff; syn.adc[4] = 0x0f; syn.adc[5] = 0xff;
	CHECK(frame2(&r, 500, 2) == 0, "garbage VBUS / VBAT / VOUT / VAC bytes on the secondary are not a limit");

	for (b = 1; b <= 73; b++) {				/* the total: above cap + 300 mA on 3 reads, any cap; primary and secondary summed */
		int thr = b * 100 + 300, half = thr / 2 + 40;
		u8 raw[2];
		int sraw = half * 1000 / 1875 + 1;

		run2(&r, b);
		r.adc.ibus = thr - half - 2;			/* just at the limit with the secondary at half+... */
		adc_bytes(syn.adc, sraw, 2090, 3040, 64, 2);
		raw[0] = 0;
		r.adc.ibus = thr - (sraw * 1875 / 1000);	/* total == thr exactly */
		CHECK(frame2(&r, 500, 2) == 0 && frame2(&r, 540, 3) == 0 && frame2(&r, 580, 4) == 0, "cap %d: total at the limit", b);
		r.adc.ibus += 1;				/* total == thr + 1 */
		CHECK(frame2(&r, 620, 5) == 0 && frame2(&r, 660, 6) == 0, "cap %d: two reads over", b);
		rc = frame2(&r, 700, 7);
		CHECK(rc == V3E_TOTAL, "cap %d: third read over: %d", b, rc);
		run2(&r, b);					/* reset by a read below */
		adc_bytes(syn.adc, sraw, 2090, 3040, 64, 2);
		for (i = 0; i < 30; i++) {
			r.adc.ibus = thr - (sraw * 1875 / 1000) + (i % 3 == 2 ? -200 : 5);
			CHECK(frame2(&r, 500 + 40 * i, 10 + i) == 0, "cap %d: no reset at read %d", b, i);
		}
		(void)raw;
	}

	/* the status check after the enable: 300 ms, then up to 3 s (stock: 300 ms sleep, 60 x 50 ms) */
	for (n = 290; n <= 310; n += 1) {
		run2(&r, 30);
		r.s.checked = 0;
		adc_bytes(syn.adc, 100, 2090, 3040, 64, 2);	/* 187 mA: below 500 */
		rc = frame2(&r, n, 2);
		CHECK(rc == 0 && !r.s.checked, "status check at %d ms with a low secondary: %d", n, rc);
	}
	for (n = 3290; n <= 3310; n++) {
		run2(&r, 30);
		r.s.checked = 0;
		adc_bytes(syn.adc, 100, 2090, 3040, 64, 2);
		rc = frame2(&r, n, 2);
		CHECK((rc == V3E_STROUBLE) == (n >= 3300), "trouble at %d ms: %d", n, rc);
	}
	for (n = 297; n <= 300; n++) {				/* not checked before 300 ms, even when everything is fine */
		run2(&r, 30);
		r.s.checked = 0;
		rc = frame2(&r, n, 2);
		CHECK(rc == 0 && r.s.checked == (n >= 300), "check at %d ms: checked %d", n, r.s.checked);
	}
	for (n = 497; n <= 503; n++) {				/* 500 mA on the secondary: raw * 1.875 >= 500 */
		run2(&r, 30);
		r.s.checked = 0;
		adc_bytes(syn.adc, n * 1000 / 1875 + (n * 1000 % 1875 ? 1 : 0), 2090, 3040, 64, 2);
		r.adc.ibus = 700;
		frame2(&r, 400, 2);
		CHECK(r.s.checked == (r.s.adc.ibus >= 500), "secondary %d mA: checked %d", r.s.adc.ibus, r.s.checked);
	}
	for (n = 998; n <= 1003; n++) {				/* |primary - secondary| at most 1000 mA */
		run2(&r, 30);
		r.s.checked = 0;
		adc_bytes(syn.adc, 800, 2090, 3040, 64, 2);	/* 1500 mA */
		r.adc.ibus = 1500 - n;
		frame2(&r, 400, 2);
		CHECK(r.s.checked == (n <= 1000), "deviation %d: checked %d", n, r.s.checked);
	}
	run2(&r, 30);					/* not switching (0x06.2 / CHG_EN) */
	r.s.checked = 0;
	syn.blk[0x06] = 0x02;
	frame2(&r, 400, 2);
	CHECK(!r.s.checked, "not switching: no check");

	/* total below 1100 mA on 4 ticks ends it (stock disables the secondary after 4 ticks) */
	for (n = 1098; n <= 1102; n++) {
		int t;

		run2(&r, 30);
		r.adc.ibus = n - 373 * 1875 / 1000 - 0;
		rc = 0;
		for (t = 0; t < 4 && !rc; t++) {
			r.s.next_tick = 10000 + 1000 * t;
			rc = frame2(&r, 10000 + 1000 * t, 2 + t);
			CHECK(rc == 0 || t == 3, "total %d: ended on tick %d", n, t);
		}
		CHECK((rc == V3E_SLOW) == (r.adc.ibus + r.s.adc.ibus < 1100), "total %d mA over 4 ticks: %d", r.adc.ibus + r.s.adc.ibus, rc);
	}
	run2(&r, 30);					/* a tick above resets the count; frames between ticks do not count */
	r.adc.ibus = 100;
	for (i = 0; i < 40; i++) {
		r.s.next_tick = 1000000;
		CHECK(frame2(&r, 10000 + 40 * i, i) == 0, "frames that are not ticks do not count");
	}
	for (i = 0; i < 3; i++) {
		r.s.next_tick = 20000 + i;
		CHECK(frame2(&r, 20000 + i, 50 + i) == 0, "tick %d", i);
	}
	r.adc.ibus = 2000;
	r.s.next_tick = 21000;
	CHECK(frame2(&r, 21000, 60) == 0 && r.s.n_low == 0, "high tick resets");
	r.adc.ibus = 100;
	for (i = 0; i < 3; i++) {
		r.s.next_tick = 22000 + i;
		CHECK(frame2(&r, 22000 + i, 70 + i) == 0, "tick again %d", i);
	}
	r.s.next_tick = 23000;
	CHECK(frame2(&r, 23000, 80) == V3E_SLOW, "fourth low tick");
	run2(&r, 30);					/* not before the status check passed */
	r.s.checked = 0;
	r.adc.ibus = 10;
	r.s.next_tick = 200;
	CHECK(frame2(&r, 200, 2) == 0 && r.s.n_low == 0 && !r.s.checked, "low rule off before the check");

	/* stale: six identical blocks */
	run2(&r, 30);
	for (i = 1; i < 12; i++) {
		now = 500 + 40 * i;
		rc = frame2(&r, now, 9);
		CHECK(rc == (i >= 6 ? V3E_SSTALE : 0), "identical block %d: %d", i, rc);
		if (rc)
			break;
	}
	CHECK(i == 6, "stale ended it at read %d, wanted 6", i);
	run2(&r, 30);
	for (i = 0; i < 40; i++)
		CHECK(frame2(&r, 500 + 40 * i, i / 2) == 0, "runs of two %d", i);
	run2(&r, 30);					/* identical only in the secondary's IBUS bytes, as stock's own run does up to 8 times: not stale */
	for (i = 0; i < 40; i++) {
		syn.adc[2] = (i / 7) & 0x0f;
		syn.adc[3] = i & 0xff;
		CHECK(frame2(&r, 500 + 40 * i, 3) == 0, "IBUS alone repeated, rest moving: %d", i);
	}

	/* read and write failures */
	for (i = 0; i < 6; i++) {
		static const u8 rr[] = { 0x06, 0x04, 0x00, 0x10, 0x13, 0x13 };

		run2(&r, 30);
		syn.fail_rd = rr[i];
		rc = frame2(&r, 500, 2);
		CHECK(rc == -V3S_RD, "read of 0x%02x fails: %d", rr[i], rc);
		syn.fail_rd = -1;
	}
	bus_reset(&sb);
	run2(&r, 30);
	sb.fail_reg = 0x11;
	sb.fail_left = 1;
	CHECK(frame2(&r, 500, 2) == -V3S_WR, "freeze write fails");
	bus_reset(&sb);
	run2(&r, 30);
	sb.fail_reg = 0x11;
	sb.fail_skip = 1;
	sb.fail_left = 1;
	CHECK(frame2(&r, 500, 2) == -V3S_WR && sb.n == 2, "unfreeze write fails after the freeze");
}

/* ---- 4: the write tables of both buses, cross-refusal, every register and value ---- */

static int has(const u8 (*l)[2], int n, int reg, int val)
{
	int i;

	for (i = 0; i < n; i++)
		if (l[i][0] == reg && l[i][1] == val)
			return 1;
	return 0;
}

static void reply_total(void)
{
	struct vl3 r;
	struct vl s;
	u16 tx;
	int k, rc;

	for (k = 0; k < 3; k++) {		/* pumps=1; pumps=2, secondary off but reading 700; pumps=2, secondary on */
		vl_init(&s, CAP_RL, CAP_RH);
		s.state = VL_DONE;
		vl3_init(&r, 20, 0);
		r.pumps = k ? 2 : 1;
		r.ph = V3_RUN;
		r.adc_ok = 1;
		r.adc.vbus = 7800; r.adc.vbat = 3900; r.adc.vout = 3900; r.adc.ibus = 800;
		r.s.enabled = k == 2;
		r.s.adc.ibus = 700;
		tx = 0;
		rc = vl3_irq(&s, &r, 0x82, VF_RXDONE, &tx);
		CHECK(rc == VL_TX && tx == vl3_data_word(vl_batt_code2(&r.adc, k == 2 ? 1500 : 800)), "reply case %d: rc %d tx 0x%04x", k, rc, tx);
	}
	CHECK(vl3_data_word(vl_batt_code2(&r.adc, 800)) != vl3_data_word(vl_batt_code2(&r.adc, 1500)), "the reply depends on the current");
}

/* ---- 5: the way out for both pumps, with failures injected on each ---- */

int main(void)
{
    replays();
    total_oracle();
    enable_conditions();
    enable_once();
    limits2();
    reply_total();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
