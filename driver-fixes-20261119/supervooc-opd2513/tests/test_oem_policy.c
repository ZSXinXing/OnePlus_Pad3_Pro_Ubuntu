/* Stock DT boundaries and the exact request transition helper used by the IRQ. */
#include <assert.h>
#include <stdio.h>
#include <vooc_logic.h>
#include <opd2513_policy.h>
int main(void)
{
 unsigned long long next = 1000, deadline = 0;
 unsigned char ask = 18;
 struct vl3 run;
 struct vl link;
 u16 reply;
 int soc, temp, voltage, checks = 0;
 unsigned int mismatch_count = 0;
 assert(!opd2513_high_current_soc(-1));
 assert(opd2513_high_current_soc(0));
 assert(opd2513_high_current_soc(55));
 assert(!opd2513_high_current_soc(56));
 /* The opt-in mode skips only the mid-SOC current step. */
 assert(opd2513_supervooc_session_ask(57, 57, 310, 4050, 1, 31000, 0) == 30);
 assert(opd2513_supervooc_session_ask(57, 57, 310, 4050, 1, 31000, 1) == 61);
 assert(opd2513_supervooc_session_ask(57, 80, 310, 4449, 1, 31000, 1) == 61);
 assert(opd2513_supervooc_session_ask(57, 81, 310, 4050, 1, 31000, 1) == 0);
 assert(opd2513_supervooc_session_ask(57, 57, 310, 4451, 1, 31000, 1) == 0);
 assert(opd2513_supervooc_session_ask(57, 57, 351, 4050, 1, 31000, 1) == 35);
 assert(opd2513_supervooc_session_ask(57, 57, 440, 4050, 1, 31000, 1) == 0);
 assert(opd2513_supervooc_session_ask(57, 57, 310, 4050, 1, 44000, 1) == 61);
 /* Device trace: cell 27 C, connector 44.236 C must not select 3 A. */
 assert(opd2513_supervooc_session_ask(56, 56, 270, 4254, 1, 44236, 1) == 61);
 assert(opd2513_supervooc_session_ask(56, 56, 270, 4254, 1, 54999, 1) == 61);
 assert(opd2513_supervooc_session_ask(56, 56, 270, 4254, 1, 55000, 1) == 30);
 assert(opd2513_supervooc_session_ask(56, 56, 270, 4254, 1, 44236, 0) == 30);
 assert(!opd2513_normal_high_allowed(351, 1, 44236));
 assert(opd2513_supervooc_session_ask(57, 57, 310, 4050, 1, -1, 1) == 30);
 assert(opd2513_ufcs_secondary_step(1899, 2200, 0, &mismatch_count) == 0);
 assert(opd2513_ufcs_secondary_step(1900, 2200, 0, &mismatch_count) == 1);
 assert(opd2513_ufcs_secondary_step(1601, 1600, 1, &mismatch_count) == 1);
 assert(opd2513_ufcs_secondary_step(1600, 1600, 1, &mismatch_count) == 0);
 for (int i = 0; i < 3; i++)
  assert(opd2513_ufcs_secondary_step(1500, 6100, 1, &mismatch_count) == 1);
 assert(opd2513_ufcs_secondary_step(1500, 6100, 1, &mismatch_count) == 0);
 assert(opd2513_ufcs_secondary_step(1500, 6100, 1, &mismatch_count) == 1);
 assert(opd2513_ufcs_secondary_step(6100, 6100, 1, &mismatch_count) == 1);
 assert(mismatch_count == 0);
 for (soc = -1; soc <= 101; soc++)
  for (temp = 200; temp <= 360; temp++)
   for (voltage = 3449; voltage <= 4451; voltage += 501) {
    int a = opd2513_stock_ask(soc, temp, voltage, 0, -1);
    if (soc < 0 || soc > 80 || temp < 210 || temp > 350 || voltage < 3450 || voltage > 4450)
     assert(a == 0);
    else {
     assert(a > 0 && a <= 61);
     if (soc <= 55) assert(a == 61);
    }
    checks++;
   }
 assert(opd2513_stock_ask(55, 349, 4450, 0, -1) == 61);
 assert(opd2513_stock_ask(56, 349, 4450, 0, -1) == 30);
 assert(opd2513_stock_ask(75, 350, 4450, 0, -1) == 35);
 assert(opd2513_stock_ask(76, 350, 4450, 0, -1) == 32);
 assert(opd2513_stock_ask(71, 310, 4200, 0, -1) == 30);
 assert(opd2513_stock_session_ask(55, 56, 310, 4100, 0, -1) == 61);
 assert(opd2513_stock_session_ask(56, 55, 310, 4100, 0, -1) == 30);
 assert(opd2513_stock_session_ask(55, 81, 310, 4100, 0, -1) == 0);
 assert(opd2513_stock_session_ask(55, 56, 351, 4100, 0, -1) == 0);
 assert(opd2513_stock_session_ask(55, 56, 310, 4451, 0, -1) == 0);
 /* The opt-in extension reaches the original normal-high band, while
  * a missing or hot connector reading preserves the default cutoff.
  */
 assert(opd2513_stock_ask(55, 351, 4200, 1, 40000) == 61);
 assert(opd2513_stock_ask(56, 439, 4200, 1, 43999) == 35);
 assert(opd2513_stock_ask(76, 439, 4200, 1, 43999) == 32);
 assert(opd2513_stock_ask(55, 440, 4200, 1, 40000) == 0);
 assert(opd2513_stock_ask(55, 351, 4200, 1, 44000) == 0);
 assert(opd2513_stock_ask(55, 351, 4200, 1, -1) == 0);
 assert(opd2513_stock_ask(55, 351, 4451, 1, 40000) == 0);
 assert(opd2513_stock_session_ask(55, 81, 351, 4200, 1, 40000) == 0);
 /* The original ROM uses the full curve current on the first request,
  * including the intermediate warm-normal bands. Cover both pump orders.
  */
 assert(opd2513_first_current_target(30, 1) == 30);
 assert(opd2513_first_current_target(32, 1) == 32);
 assert(opd2513_first_current_target(35, 1) == 35);
 assert(opd2513_first_current_target(61, 0) == 30);
 for (soc = 55; soc <= 80; soc++) {
  u8 target = opd2513_stock_ask(soc, 350, 4200, 0, -1);
  u8 first = opd2513_first_current_target(target, 1);

  assert(first == target);
  assert(opd2513_first_current_allowed(target, first, 0, 18, 1, 0, 50, 1, 0, 0));
  assert(opd2513_first_current_dual_ready(target, first, 0, 1, 1, 1, 1999));
  assert(!opd2513_first_current_dual_ready(target, first, 0, 1, 1, 1, 2000));
  assert(!opd2513_first_current_dual_ready(target, first, 0, 1, 0, 1, 50));
  assert(!opd2513_first_current_dual_ready(target, first, 0, 1, 1, 0, 50));
  assert(!opd2513_first_current_dual_ready(target, first, 0, 0, 1, 1, 50));
  assert(!opd2513_first_current_dual_ready(target, first, 18, 1, 1, 1, 50));
 }
 /* Allow the guarded 3 A first reply within the slave startup window. */
 assert(opd2513_first_current_allowed(30, 30, 0, 18, 1, 0, 0, 1, 0, 0));
 assert(opd2513_first_current_allowed(30, 30, 30, 30, 1, 0, 1999, 1, 0, 0));
 assert(!opd2513_first_current_allowed(30, 30, 30, 30, 1, 0, 2000, 1, 0, 0));
 assert(!opd2513_first_current_allowed(30, 30, 18, 18, 1, 0, 500, 1, 0, 0));
 assert(!opd2513_first_current_allowed(30, 30, 0, 18, 0, 0, 500, 1, 0, 0));
 assert(!opd2513_first_current_allowed(30, 30, 30, 30, 1, 1, 500, 1, 0, 0));
 assert(opd2513_first_current_allowed(61, 30, 0, 18, 1, 0, 500, 1, 0, 0));
 assert(opd2513_first_current_allowed(61, 30, 30, 30, 1, 0, 1500, 1, 0, 0));
 assert(!opd2513_first_current_allowed(29, 30, 0, 18, 1, 0, 500, 1, 0, 0));
 assert(!opd2513_first_current_allowed(62, 30, 0, 18, 1, 0, 500, 1, 0, 0));
 /* A transient primary status drop may preserve a grant, but can
  * neither start a new grant nor extend the original startup deadline. */
 assert(opd2513_first_current_allowed(61, 30, 30, 30, 1, 0, 500, 0, 1, 0));
 assert(!opd2513_first_current_allowed(61, 30, 0, 18, 1, 0, 500, 0, 1, 0));
 assert(!opd2513_first_current_allowed(61, 30, 30, 30, 1, 0, 500, 0, 0, 0));
 assert(!opd2513_first_current_allowed(61, 30, 30, 30, 0, 0, 500, 0, 1, 0));
 assert(!opd2513_first_current_allowed(61, 30, 30, 30, 1, 0, 2000, 0, 1, 0));
 /* OPD2513 trace: at 133 ms the enabled primary had yet to switch.
  * Preserve only the grant already sent, with all other gates intact. */
 assert(opd2513_first_current_allowed(61, 30, 30, 30, 1, 0, 133, 0, 0, 1));
 assert(!opd2513_first_current_allowed(61, 30, 0, 18, 1, 0, 133, 0, 0, 1));
 assert(!opd2513_first_current_allowed(61, 30, 30, 30, 0, 0, 133, 0, 0, 1));
 assert(!opd2513_first_current_allowed(61, 30, 30, 30, 1, 0, 2000, 0, 0, 1));
 /* The measured delay may retain a 4 A high-band grant, under the
  * same deadline. Lower stock bands must stay below that grant. */
 assert(opd2513_first_current_allowed(61, 40, 0, 18, 1, 0, 80, 1, 0, 1));
 assert(opd2513_first_current_allowed(61, 40, 40, 40, 1, 0, 133, 0, 0, 1));
 assert(!opd2513_first_current_allowed(30, 40, 40, 40, 1, 0, 133, 0, 0, 1));
 assert(!opd2513_first_current_allowed(61, 40, 40, 40, 1, 0, 2000, 0, 0, 1));
 assert(opd2513_first_current_allowed(61, 50, 0, 18, 1, 0, 80, 1, 0, 1));
 assert(opd2513_first_current_allowed(61, 50, 50, 50, 1, 0, 133, 0, 0, 1));
 assert(!opd2513_first_current_allowed(61, 50, 50, 50, 1, 0, 2000, 0, 0, 1));
 /* The opt-in stock first reply still requires all guard inputs. */
 assert(opd2513_first_current_allowed(61, 61, 0, 18, 1, 0, 80, 1, 0, 1));
 assert(opd2513_first_current_allowed(61, 61, 61, 61, 1, 0, 133, 0, 0, 1));
 assert(!opd2513_first_current_allowed(61, 61, 0, 18, 0, 0, 80, 1, 0, 1));
 assert(!opd2513_first_current_allowed(61, 61, 61, 61, 1, 0, 2000, 0, 0, 1));
 assert(!opd2513_first_current_allowed(30, 61, 0, 18, 1, 0, 80, 1, 0, 1));
 /* No ramp without a confirmed slave or before the next time slot. */
 assert(!opd2513_ask_step(&ask, 18, 30, 0, 1000, &next, &deadline) && ask == 18);
 assert(!opd2513_ask_step(&ask, 18, 30, 1, 999, &next, &deadline) && ask == 18);
 assert(!opd2513_ask_step(&ask, 18, 30, 1, 1000, &next, &deadline) && ask == 23);
 assert(deadline == 3000);
 /* No second increase until the previous current request is accepted. */
 assert(!opd2513_ask_step(&ask, 18, 30, 1, 2000, &next, &deadline) && ask == 23);
 assert(opd2513_ask_step(&ask, 18, 30, 1, 3001, &next, &deadline) == -1);
 assert(!opd2513_ask_step(&ask, 23, 30, 1, 3100, &next, &deadline) && ask == 28);
 assert(!opd2513_ask_step(&ask, 28, 30, 1, 4100, &next, &deadline) && ask == 30);
 assert(!opd2513_ask_step(&ask, 30, 30, 1, 4200, &next, &deadline) && deadline == 0);
 /* A lower temperature/SOC limit, or slave loss, derates immediately. */
 assert(!opd2513_ask_step(&ask, 30, 20, 1, 4201, &next, &deadline) && ask == 20);
 assert(!opd2513_ask_step(&ask, 20, 30, 0, 4202, &next, &deadline) && ask == 18);
 /* The same ask is used by the PHY encoding and total-current protection. */
 vl3_init(&run, 61, 0);
 run.stock_ask = 61; run.stage4 = 1; run.ask4 = 30; run.lim4 = 30;
 assert(vl3_ask(&run) == 30 && vl3_tot_lim(&run) == 3300);
 run.ask4 = 18; /* previous request remains in force while awaiting response */
 assert(vl3_ask(&run) == 18 && vl3_tot_lim(&run) == 3300);
 run.lim4 = 18;
 assert(vl3_tot_lim(&run) == 2100);
 vl_init(&link, 1, 2); link.state = VL_DONE;
 run.ph = V3_RUN; run.adc_ok = 1; run.sent = 30;
 /* Derating is signalled as the change code, then encoded on CURR. */
 assert(vl3_irq(&link, &run, 0x80 | (VC3_BVOL << 1), VF_RXDONE, &reply) == VL_TX);
 assert(reply == vl3_data_word(V3_CHG_CODE));
 assert(vl3_irq(&link, &run, 0x80 | (VC3_CURR << 1), VF_RXDONE, &reply) == VL_TX);
 assert(run.sent == 18 && reply == vl3_data_word(18));
 assert(!opd2513_ask_step(&ask, 18, 18, 1, 4300, &next, &deadline));
 assert(deadline == 0);
 /* A real adapter may send BVOL before its first CURR. Bootstrap it. */
 vl3_init(&run, 30, 0);
 run.stock_ask = 61; run.stage4 = 1; run.ask4 = 18; run.lim4 = 18;
 vl_init(&link, 1, 2); link.state = VL_DONE;
 run.ph = V3_RUN; run.adc_ok = 1;
 assert(vl3_irq(&link, &run, 0x80 | (VC3_BVOL << 1), VF_RXDONE, &reply) == VL_TX);
 assert(reply == vl3_data_word(V3_CHG_CODE));
 assert(run.sent == 0); /* Notice alone must never grant a higher current. */
 assert(vl3_irq(&link, &run, 0x80 | (VC3_CURR << 1), VF_RXDONE, &reply) == VL_TX);
 assert(run.sent == 18 && reply == vl3_data_word(18));
 ask = 18; next = 1000; deadline = 0;
 assert(!opd2513_ask_step(&ask, 0, 30, 1, 1000, &next, &deadline));
 assert(ask == 18 && deadline == 0); /* No invented acknowledgement. */
 opd2513_ask_notified(ask, 0, 1000, &deadline);
 assert(deadline == 3000);
 opd2513_ask_notified(ask, 0, 2000, &deadline);
 assert(deadline == 3000); /* Repeated notices must not extend the timeout. */
 assert(!opd2513_ask_step(&ask, 0, 30, 1, 3000, &next, &deadline));
 assert(opd2513_ask_step(&ask, 0, 30, 1, 3001, &next, &deadline) == -1);
 assert(!opd2513_ask_step(&ask, 18, 18, 1, 3002, &next, &deadline));
 assert(deadline == 0);
 /* BVOL may have armed the timeout for the placeholder ask. The stock
  * first CURR selects the guarded 3 A curve even with that timer armed. */
 run.sent = 0; run.ask4 = 18; run.cap = 30;
 deadline = 3000;
 assert(opd2513_first_current_allowed(30, 30, run.sent, run.ask4, 1, 0, 25, 1, 0, 0));
 run.ask4 = 30;
 assert(!opd2513_ask_step(&run.ask4, run.sent, 30, 1, 1025, &next, &deadline));
 assert(vl3_irq(&link, &run, 0x80 | (VC3_CURR << 1), VF_RXDONE, &reply) == VL_TX);
 assert(run.sent == 30 && reply == 0x1e02);
 /* Coupled PHY/monitor sequence: BVOL first, then 1.8 -> 2.3 -> 2.8 -> 3A.
  * This covers the bootstrap and the following ramp as one dialogue, rather
  * than assuming the initial current reply happened outside the test. */
 {
  unsigned long long t = 1000, raise = 1000, ack = 0;
  int stage;
  const unsigned char expected[] = { 18, 23, 28, 30 };
  vl3_init(&run, 30, 0);
  run.stock_ask = 61; run.stage4 = 1; run.ask4 = 18; run.lim4 = 18;
  run.ph = V3_RUN; run.adc_ok = 1;
  vl_init(&link, 1, 2); link.state = VL_DONE;
  for (stage = 0; stage < 4; stage++, t += 1000) {
   assert(!opd2513_ask_step(&run.ask4, run.sent, 30, stage != 0,
                          t, &raise, &ack));
   assert(run.ask4 == expected[stage]);
   assert(vl3_irq(&link, &run, 0x80 | (VC3_BVOL << 1), VF_RXDONE, &reply) == VL_TX);
   assert(reply == vl3_data_word(V3_CHG_CODE));
   assert(vl_write_phase_m(0x2c, 2, reply, VM_S3) & VW_SESS);
   assert(vl_write_phase_m(0x31, 2, run.predata, VM_S3) & VW_RUN);
   opd2513_ask_notified(run.ask4, run.sent, t, &ack);
   assert(ack != 0);
   assert(vl3_irq(&link, &run, 0x80 | (VC3_CURR << 1), VF_RXDONE, &reply) == VL_TX);
   assert(reply == vl3_data_word(expected[stage]));
   assert(run.sent == expected[stage]);
   run.lim4 = run.sent;
   assert(vl3_tot_lim(&run) == expected[stage] * 100 + 300);
  }
  assert(!opd2513_ask_step(&run.ask4, run.sent, 30, 1, t, &raise, &ack));
  assert(ack == 0 && run.ask4 == 30);
  /* Losing slave confirmation must request the starting cap again. */
  assert(!opd2513_ask_step(&run.ask4, run.sent, 30, 0, t + 1, &raise, &ack));
  assert(run.ask4 == 18 && run.sent == 30);
  assert(vl3_irq(&link, &run, 0x80 | (VC3_BVOL << 1), VF_RXDONE, &reply) == VL_TX);
  assert(reply == vl3_data_word(V3_CHG_CODE));
  assert(vl3_irq(&link, &run, 0x80 | (VC3_CURR << 1), VF_RXDONE, &reply) == VL_TX);
  assert(run.sent == 18);
 }
 /* The 6.1 A band must stay at the verified 3 A bootstrap until the
  * secondary pump is ready. Exercise the real encoding at every step.
  */
 {
  const u8 expected[] = { 35, 40, 45, 50, 55, 60, 61 };
  unsigned long long t = 1500, raise = 0, ack = 0;
  unsigned int stage;

  vl3_init(&run, 61, 0);
  run.stock_ask = 61; run.stage4 = 1; run.ask4 = 30; run.lim4 = 30;
  run.ph = V3_RUN; run.adc_ok = 1; run.sent = 30;
  vl_init(&link, 1, 2); link.state = VL_DONE;
  assert(opd2513_first_current_allowed(61, 30, run.sent, run.ask4, 1, 0, t, 1, 0, 0));
  assert(!opd2513_ask_step(&run.ask4, run.sent, 30, 1, t, &raise, &ack));
  assert(run.ask4 == 30 && vl3_tot_lim(&run) == 3300);
  for (stage = 0; stage < sizeof(expected); stage++, t += 1000) {
   assert(!opd2513_first_current_allowed(61, 30, run.sent, run.ask4, 1, 1, t, 1, 0, 0));
   assert(!opd2513_ask_step(&run.ask4, run.sent, 61, 1, t, &raise, &ack));
   assert(run.ask4 == expected[stage]);
   run.lim4 = run.ask4;
   assert(vl3_irq(&link, &run, 0x80 | (VC3_BVOL << 1), VF_RXDONE, &reply) == VL_TX);
   assert(reply == vl3_data_word(V3_CHG_CODE));
   assert(vl3_irq(&link, &run, 0x80 | (VC3_CURR << 1), VF_RXDONE, &reply) == VL_TX);
   assert(reply == vl3_data_word(expected[stage]));
   assert(vl_write_phase_m(0x2c, 2, reply, VM_S3) & VW_SESS);
   assert(vl3_tot_lim(&run) == expected[stage] * 100 + 300);
  }
  assert(!opd2513_ask_step(&run.ask4, run.sent, 30, 1, t, &raise, &ack));
  assert(run.ask4 == 30);
 }
 /* An I2C write can succeed before the PHY reports that transmission failed. */
 {
  u8 flags;
  int retry;
  vl3_init(&run, 30, 0);
  run.stage4 = 1;
  run.stock_ask = 61;
  run.ph = V3_RUN;
  run.adc_ok = 1;
  run.ask4 = run.want = 30;
  vl_init(&link, 1, 2);
  link.state = VL_DONE;
  for (retry = 0; retry < 3; retry++) {
   run.tx_previous = 18;
   run.sent = 30;
   run.tx_pending = 1;
   flags = VF_RXDONE | VF_TX_WRITE_FAIL;
   assert(vl3_current_tx_status(&run, &flags) == 1);
   assert(flags == VF_RXDONE && run.sent == 18 && run.tx_retries == retry + 1);
   assert(vl3_irq(&link, &run, 0x80 | (VC3_BVOL << 1), flags, &reply) == VL_TX);
   assert(reply == vl3_data_word(V3_CHG_CODE));
  }
  run.sent = 30;
  run.tx_pending = 1;
  flags = VF_RXDONE | VF_TX_WRITE_FAIL;
  assert(vl3_current_tx_status(&run, &flags) == 0 && (flags & VF_ABORT));
  assert(vl3_irq(&link, &run, 0x80 | (VC3_BVOL << 1), flags, &reply) == VL_E_FLAG);
  run.tx_retries = 0;
  flags = VF_RXDONE | VF_TX_WRITE_FAIL | 0x10;
  assert(vl3_current_tx_status(&run, &flags) == 0 && (flags & 0x10));
  flags = VF_RXDONE;
  assert(vl3_current_tx_status(&run, &flags) == 0 && !run.tx_pending && run.sent == 30);
 }
 printf("OEM policy: %d envelope checks and boundary/ramp/timeout/limit assertions passed\n", checks);
 return 0;
}
