/*
 * Host test for vooc_logic.h: replays round 1 of the stock capture through the frame logic and checks the abort,
 * stop and write-whitelist rules. cc -I.. test_logic.c -o test_logic && ./test_logic     (./test_logic --table prints the write table)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vooc_logic.h>
#include "capture_round1.h"

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

static int feed(struct vl *s, u8 rx, u8 fl, u16 *tx)
{
	*tx = 0;
	return vl_irq(s, rx, fl, tx);
}

static void replay(void)
{
	struct vl s;
	u16 tx;
	int i, r;

	vl_init(&s, CAP_RL, CAP_RH);
	CHECK(s.local == 0x9cf0, "local code 0x%04x, stock logged code_id_local 0x9cf0", s.local);
	CHECK(feed(&s, 0x00, 0x20, &tx) == VL_NONE && s.state == VL_WAIT, "TXSEQ_DONE after the handshake: no reply");
	for (i = 0; i < CAP_N; i++) {
		r = feed(&s, cap_rx[i], cap_flags[i], &tx);
		CHECK(r == (i == CAP_N - 1 ? VL_LAST : VL_TX), "frame %d returned %d", i + 1, r);
		CHECK(tx == cap_tx[i], "frame %d: tx 0x%04x, capture 0x%04x", i + 1, tx, cap_tx[i]);
	}
	CHECK(s.state == VL_DONE, "state %d after the last row", s.state);
	CHECK(s.model == CAP_ID && vl_id_ok(s.model), "adapter id 0x%02x", s.model);
	CHECK(s.seen & 4, "adapter check not marked ok");
	r = feed(&s, CAP_SECOND_RX, CAP_SECOND_FLAGS, &tx);
	CHECK(r == VL_NONE && tx == 0 && s.state == VL_DONE, "second 0x04 after the last row: reply %d tx 0x%x", r, tx);
}

static void second_ask(void)
{
	struct vl s;
	u16 tx;
	int i, n;

	for (n = 1; n <= CAP_N; n++) {		/* a second 0x04 after n frames, wherever it lands */
		vl_init(&s, CAP_RL, CAP_RH);
		for (i = 0; i < n; i++)
			feed(&s, cap_rx[i], cap_flags[i], &tx);
		if (s.pend)			/* inside a follow-up an 0x88 is data, not a command */
			continue;
		CHECK(feed(&s, 0x88, 0x07, &tx) == VL_NONE && tx == 0, "second 0x04 after frame %d answered", n);
		CHECK(s.state == (n == CAP_N ? VL_DONE : VL_STOP), "state %d after the second 0x04 at frame %d", s.state, n);
		CHECK(feed(&s, cap_rx[(n < CAP_N) ? n : 0], 0x07, &tx) == VL_NONE && tx == 0, "reply after STOP at frame %d", n);
	}
}

static void echo_and_id(void)
{
	struct vl s;
	u16 tx;
	int i, r, k, m1, m2;

	/*
	 * Flip each bit of each echoed nibble (frames 4..7). Frame 7 decides. Stock masks the code with 0xfefe, so a flip of
	 * far bit 0 (frame 7, nibble bit 0) or far bit 8 (frame 5, nibble bit 0) is not seen: those two are expected to pass.
	 */
	for (k = 3; k <= 6; k++)
		for (m1 = 0; m1 < 4; m1++) {
			static const int far_lsb[4] = { 12, 8, 4, 0 };
			int far_bit = far_lsb[k - 3] + m1, masked = far_bit == 0 || far_bit == 8;
			u8 rx[CAP_N];

			memcpy(rx, cap_rx, sizeof(rx));
			rx[k] ^= 2 << m1;
			vl_init(&s, CAP_RL, CAP_RH);
			for (i = 0; i < 6; i++)
				CHECK(feed(&s, rx[i], cap_flags[i], &tx) == VL_TX, "frame %d", i + 1);
			r = feed(&s, rx[6], 0x07, &tx);
			CHECK(r == (masked ? VL_TX : VL_E_ECHO) && (r != VL_E_ECHO || tx == 0),
			      "echo flip frame %d nibble bit %d: %d", k + 1, m1, r);
		}
	vl_init(&s, CAP_RL + 1, CAP_RH);	/* a different local code against the capture's echo */
	for (i = 0; i < 6; i++)
		feed(&s, cap_rx[i], cap_flags[i], &tx);
	CHECK(feed(&s, cap_rx[6], 0x07, &tx) == VL_E_ECHO && tx == 0, "wrong local code accepted");

	for (m1 = 0; m1 < 16; m1++)		/* the model id is two nibbles, bit 7 dropped; only 4b 4c 4d 4e 65 pass */
		for (m2 = 0; m2 < 16; m2++) {
			int id = ((m1 << 4) | m2) & 0x7f;

			vl_init(&s, CAP_RL, CAP_RH);
			for (i = 0; i < 8; i++)
				feed(&s, cap_rx[i], cap_flags[i], &tx);
			feed(&s, 0x80 | (m1 << 1), 0x07, &tx);
			r = feed(&s, 0x80 | (m2 << 1), 0x07, &tx);
			CHECK(r == (vl_id_ok(id) ? VL_TX : VL_E_ID), "id 0x%02x: %d", id, r);
		}
	for (m1 = 0; m1 < 256; m1++)
		CHECK(vl_id_ok(m1) == (m1 == 0x4b || m1 == 0x4c || m1 == 0x4d || m1 == 0x4e || m1 == 0x65), "vl_id_ok(0x%02x)", m1);
}

static void flags_and_start(void)
{
	struct vl s;
	u16 tx;
	int fl, st, i, r;

	for (fl = 0; fl < 256; fl++)		/* every abort flag ends the session in every state */
		for (st = 0; st < 3; st++) {
			vl_init(&s, CAP_RL, CAP_RH);
			for (i = 0; i < (st == 0 ? 0 : st == 1 ? 3 : CAP_N); i++)
				feed(&s, cap_rx[i], cap_flags[i], &tx);
			r = feed(&s, 0x98, fl, &tx);
			CHECK((r == VL_E_FLAG) == !!(fl & 0xd8), "flags 0x%02x state %d: %d", fl, st, r);
			if (fl & 0xd8)
				CHECK(tx == 0, "flags 0x%02x: reply sent with an abort flag", fl);
		}
	CHECK(VF_ABORT == (0x80 | 0x40 | 0x10 | 0x08), "abort mask 0x%02x", VF_ABORT);

	vl_init(&s, CAP_RL, CAP_RH);		/* 5 frames that are not 0xa8 are ignored, the 6th ends the session */
	for (i = 1; i <= 5; i++)
		CHECK(feed(&s, 0x88, 0x07, &tx) == VL_NONE && tx == 0 && s.state == VL_WAIT, "ignored frame %d", i);
	CHECK(feed(&s, VL_FIRST, 0x06, &tx) == VL_TX && tx == 0x3002, "0xa8 after 5 junk frames");
	vl_init(&s, CAP_RL, CAP_RH);
	for (i = 1; i <= 5; i++)
		feed(&s, 0x88, 0x07, &tx);
	CHECK(feed(&s, 0x88, 0x07, &tx) == VL_E_START, "6 frames without 0xa8");

	for (fl = 0; fl < 256; fl++) {		/* the only first frame that gets a reply is 0xa8, and the reply is 0x3002 */
		vl_init(&s, CAP_RL, CAP_RH);
		r = feed(&s, fl, 0x06, &tx);
		CHECK((r == VL_TX) == (fl == VL_FIRST) && (r != VL_TX || tx == 0x3002), "first frame 0x%02x -> %d 0x%04x", fl, r, tx);
	}
	vl_init(&s, CAP_RL, CAP_RH);		/* head, order */
	feed(&s, cap_rx[0], 0x06, &tx);
	for (fl = 0; fl < 256; fl++) {
		struct vl c = s;

		r = feed(&c, fl, 0x07, &tx);
		if ((fl & 0xe0) != 0x80)
			CHECK(r == VL_E_HEAD, "head 0x%02x: %d", fl, r);
		else if (((fl & 0x1e) >> 1) == VC_IDENT)
			CHECK(r == VL_TX, "0x0c rejected");
		else if (((fl & 0x1e) >> 1) == VC_ASK)
			CHECK(r == VL_NONE && c.state == VL_STOP, "second 0x04 %d", r);
		else
			CHECK(r == VL_E_CMD, "command 0x%02x out of order: %d", (fl & 0x1e) >> 1, r);
	}
}

/* an honest adapter for every identification code: 17 replies, the allow bit never set, every word ends in 0x02 */
static void all_codes(void)
{
	struct vl s;
	u16 tx;
	int rl, rh, i, r, n;

	for (rl = 0; rl < 128; rl++)
		for (rh = 0; rh < 128; rh++) {
			u8 rx[CAP_N];
			unsigned int loc = (rh << 9) | (rl << 1);

			memcpy(rx, cap_rx, sizeof(rx));
			rx[3] = 0x80 | (((loc >> 12) & 15) << 1);
			rx[4] = 0x80 | (((loc >> 8) & 15) << 1);
			rx[5] = 0x80 | (((loc >> 4) & 15) << 1);
			rx[6] = 0x80 | ((loc & 15) << 1);
			vl_init(&s, rl, rh);
			for (i = 0, n = 0; i < CAP_N; i++) {
				r = feed(&s, rx[i], cap_flags[i], &tx);
				if (r > 0) {
					n++;
					CHECK((tx & 0xff) == 0x02, "tx 0x%04x", tx);
					CHECK(s.now != VC_ASK || tx == 0x3002, "0x04 answered with 0x%04x", tx);
				}
				if (i >= 3 && i <= 6)	/* frames 4..7 are the same reply whatever the code */
					CHECK(tx == 0x1002, "echo reply 0x%04x", tx);
			}
			CHECK(n == CAP_N && s.state == VL_DONE, "code %d/%d: %d replies", rl, rh, n);
			if (fails > 20)
				return;
		}
}

/* random garbage in every state: the 0x04 reply is 0x3002 or absent, nothing else touches the allow reply */
static void fuzz(void)
{
	struct vl s;
	u16 tx;
	int run, i, r;

	srand(1);
	for (run = 0; run < 3000; run++) {
		vl_init(&s, rand(), rand());
		for (i = 0; i < 40; i++) {
			u8 rx = rand() % 3 ? cap_rx[rand() % CAP_N] : rand();
			u8 fl = rand() % 4 ? 0x07 : rand();

			r = feed(&s, rx, fl, &tx);
			if (r > 0) {
				CHECK((tx & 0xff) == 0x02, "tx 0x%04x", tx);
				CHECK(s.now != VC_ASK || tx == 0x3002, "0x04 answered with 0x%04x", tx);
			} else {
				CHECK(tx == 0, "tx written without a reply");
			}
			if (r < 0 || s.state >= VL_DONE)
				break;
		}
	}
}

/* the table is the whitelist: exactly these byte writes, these word writes, nothing else */
static void whitelist(void)
{
	static const struct { u8 reg, val; } okb[] = {
		{ 0x07, 0x40 }, { 0x07, 0x00 }, { 0x11, 0x00 }, { 0x11, 0x80 }, { 0x11, 0xa0 }, { 0x02, 0x01 },
		{ 0x04, 0x5a }, { 0x00, 0x36 }, { 0x05, 0x2f }, { 0x01, 0xbf }, { 0x2b, 0x00 }, { 0x2b, 0x81 },
		{ 0x34, 0x60 }, { 0x09, 0x10 }, { 0x10, 0x02 }, { 0x21, 0x21 }, { 0x21, 0xc0 }, { 0x22, 0x00 },
		{ 0x33, 0xd1 }, { 0x30, 0x05 }, { 0x30, 0x7f }, { 0x3b, 0x04 },
	};
	int reg, val, w, n, i, ok, want;

	for (reg = 0; reg < 256; reg++)
		for (val = 0; val < 65536; val++)
			for (w = 1; w <= 2; w++) {
				ok = vl_write_ok(reg, w, val);
				want = 0;
				if (w == 1 && val < 256)
					for (i = 0; i < (int)(sizeof(okb) / sizeof(okb[0])); i++)
						want |= okb[i].reg == reg && okb[i].val == val;
				if (w == 2)
					want = (reg == 0x2c && ((val & 0xff) == 0x02 || val == 0)) || (reg == 0x31 && val == 0);
				if (ok != want) {
					CHECK(ok == want, "write 0x%02x w%d 0x%x: whitelist %d, expected %d", reg, w, val, ok, want);
					return;
				}
			}
	for (reg = 0, n = 0; reg < 256; reg++)
		for (val = 0; val < 256; val++)
			n += vl_write_ok(reg, 1, val);
	CHECK(n == (int)(sizeof(okb) / sizeof(okb[0])), "%d byte writes accepted", n);

	CHECK(!vl_write_ok(0x07, 1, 0x80) && !vl_write_ok(0x07, 1, 0x81) && !vl_write_ok(0x07, 1, 0xc0), "0x07 with bit 7");
	for (val = 0; val < 256; val++) {	/* 0x07 with CHG_EN set is refused, whatever the other bits */
		CHECK(!(val & 0x80) || !vl_write_ok(0x07, 1, val), "0x07 = 0x%02x", val);
		CHECK(!vl_write_ok(0x08, 1, val) && !vl_write_ok(0x0d, 1, val) &&
		      !vl_write_ok(0x0c, 1, val) && !vl_write_ok(0x3c, 1, val), "0x08/0x0d/0x0c/0x3c = 0x%02x", val);
		CHECK(!vl_write_ok(0x3b, 1, val) || val == 0x04, "0x3b = 0x%02x", val);
		CHECK(!vl_write_ok(0x05, 1, val) || val == 0x2f, "0x05 = 0x%02x", val);
		CHECK(!vl_write_ok(0x09, 1, val) || val == 0x10, "0x09 = 0x%02x", val);
		for (reg = 0x40; reg <= 0xcc; reg++)
			CHECK(!vl_write_ok(reg, 1, val) && !vl_write_ok(reg, 2, val), "reg 0x%02x", reg);
	}
}

/* what the module writes (the row lists it runs) against the capture, an independently typed list and the table */
static void rows(void)
{
	/* stock's exit sequence, typed here as an independent oracle */
	static const struct { u8 reg, w; u16 val; } plan_exit[] = {
		{ 0x07, 1, 0x00 }, { 0x09, 1, 0x10 }, { 0x00, 1, 0x36 }, { 0x02, 1, 0x01 }, { 0x04, 1, 0x5a }, { 0x05, 1, 0x2f },
		{ 0x11, 1, 0x00 }, { 0x2c, 2, 0x0000 }, { 0x30, 1, 0x7f }, { 0x21, 1, 0xc0 }, { 0x2b, 1, 0x00 }, { 0x31, 2, 0x0000 },
		{ 0x10, 1, 0x02 },
	};
	unsigned int i, j, n = 0, nin = sizeof(vl_init_rows) / sizeof(vl_init_rows[0]);
	int hit[VL_NW] = { 0 };

	/* capture init sequence = our rows + handshake, write for write */
	for (i = 0; i < sizeof(cap_init) / sizeof(cap_init[0]); i++) {
		const struct vl_row *r;

		r = n < nin ? &vl_init_rows[n] : &vl_handshake;
		CHECK(n <= nin && r->reg == cap_init[i].reg && r->w == cap_init[i].w && r->val == cap_init[i].val,
		      "init write %u: capture 0x%02x/%d/0x%x, module 0x%02x/%d/0x%x", n, cap_init[i].reg, cap_init[i].w,
		      cap_init[i].val, r->reg, r->w, r->val);
		n++;
	}
	CHECK(n == nin + 1, "capture has %u writes, module %u", n, nin + 1);

	CHECK(sizeof(vl_exit_rows) / sizeof(vl_exit_rows[0]) == sizeof(plan_exit) / sizeof(plan_exit[0]), "exit row count");
	for (i = 0; i < sizeof(plan_exit) / sizeof(plan_exit[0]); i++)
		CHECK(vl_exit_rows[i].reg == plan_exit[i].reg && vl_exit_rows[i].w == plan_exit[i].w &&
		      vl_exit_rows[i].val == plan_exit[i].val, "exit row %u", i);

	/* every row the module runs is allowed in its phase; every init/exit table entry is used by a row (no dead entries) */
	for (i = 0; i < nin + 1; i++) {
		const struct vl_row *r = i < nin ? &vl_init_rows[i] : &vl_handshake;

		CHECK(vl_write_phase(r->reg, r->w, r->val) & VW_INIT, "init row %u not allowed in init", i);
		for (j = 0; j < VL_NW; j++)
			if ((vl_wl[j].phase & VW_INIT) && vl_wl[j].reg == r->reg && vl_wl[j].w == r->w &&
			    (r->val & vl_wl[j].mask) == vl_wl[j].val)
				hit[j] |= VW_INIT;
	}
	for (i = 0; i < sizeof(vl_exit_rows) / sizeof(vl_exit_rows[0]); i++) {
		const struct vl_row *r = &vl_exit_rows[i];

		CHECK(vl_write_phase(r->reg, r->w, r->val) & VW_EXIT, "exit row %u not allowed in exit", i);
		for (j = 0; j < VL_NW; j++)
			if ((vl_wl[j].phase & VW_EXIT) && vl_wl[j].reg == r->reg && vl_wl[j].w == r->w &&
			    (r->val & vl_wl[j].mask) == vl_wl[j].val)
				hit[j] |= VW_EXIT;
	}
	for (j = 0; j < VL_NW; j++)
		CHECK((vl_wl[j].phase & (VW_INIT | VW_EXIT) & ~hit[j]) == 0, "table entry %u (%s) is never written", j, vl_wl[j].what);
	CHECK(vl_write_phase(0x11, 1, 0xa0) == VW_SESS && vl_write_phase(0x11, 1, 0x80) == (VW_INIT | VW_SESS) &&
	      vl_write_phase(0x2c, 2, 0x6e02) == VW_SESS, "session phases");
}

static void table(void)
{
	unsigned int i;

	printf("| reg | width | value | phase | what |\n|---|---|---|---|---|\n");
	for (i = 0; i < VL_NW; i++) {
		const struct vl_w *e = &vl_wl[i];
		char v[32], ph[32] = "";

		if (e->mask == 0xff || e->mask == 0xffff)
			snprintf(v, sizeof(v), e->w == 2 ? "0x%04x" : "0x%02x", e->val);
		else
			snprintf(v, sizeof(v), "0x%04x & 0x%04x", e->val, e->mask);
		snprintf(ph, sizeof(ph), "%s%s%s", e->phase & VW_INIT ? "init " : "", e->phase & VW_SESS ? "session " : "",
			 e->phase & VW_EXIT ? "exit" : "");
		printf("| 0x%02x | %s | %s | %s | %s |\n", e->reg, e->w == 2 ? "word" : "byte", v, ph, e->what);
	}
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--table")) {
		table();
		return 0;
	}
	replay();
	second_ask();
	echo_and_id();
	flags_and_start();
	all_codes();
	fuzz();
	whitelist();
	rows();
	/* idle check: the values stock and this tablet read after the soft reset are idle; CHG_EN or switching is not */
	CHECK(vl_pump_idle(0x22, 0x07) && vl_pump_idle(0x22, 0x00) && vl_pump_idle(0x22, 0x01), "idle values refused");
	CHECK(!vl_pump_idle(0x22, 0x80) && !vl_pump_idle(0x22, 0x87) && !vl_pump_idle(0x26, 0x07), "running pump accepted");
	printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
	return fails != 0;
}
