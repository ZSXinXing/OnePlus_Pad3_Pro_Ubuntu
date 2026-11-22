/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OPD2513_POLICY_H
#define OPD2513_POLICY_H

#define OPD_SVOOC_NORMAL_HIGH_DC 350
#define OPD_SVOOC_WARM_DC 440
/* Same ceiling as the existing all-channel charging sensor gate. */
#define OPD_CONNECTOR_GUARD_MC 55000
#define OPD_HIGH_CURRENT_MAX_SOC 55

static inline int opd2513_high_current_soc(int soc)
{
	return soc >= 0 && soc <= OPD_HIGH_CURRENT_MAX_SOC;
}

#define OPD_UFCS_SECONDARY_OPEN_MA 1900
#define OPD_UFCS_SECONDARY_CLOSE_MA 1600
#define OPD_UFCS_SECONDARY_MONITOR_MS 500

/* ColorOS .402 virtual CP thresholds and mismatch debounce. Call at 500 ms. */
static inline int opd2513_ufcs_secondary_wanted(int current_ma, int active)
{
	return active ? current_ma > OPD_UFCS_SECONDARY_CLOSE_MA :
			current_ma >= OPD_UFCS_SECONDARY_OPEN_MA;
}

static inline int opd2513_ufcs_secondary_step(int measured_ma, int target_ma,
		int active, unsigned int *mismatch_count)
{
	int measured = opd2513_ufcs_secondary_wanted(measured_ma, active);
	int target = opd2513_ufcs_secondary_wanted(target_ma, active);

	if (measured == active || measured == target) {
		*mismatch_count = 0;
		return measured;
	}
	if (*mismatch_count < 3) {
		++*mismatch_count;
		return active;
	}
	*mismatch_count = 0;
	return measured;
}

/* The stock driver can fall back to battery temperature without shell_back.
 * For the optional normal-high band, also require a valid, cool battery
 * connector reading. The connector is an additional guard, not a substitute
 * for the stock shell or battery sensor.
 */
static inline int opd2513_normal_high_allowed(int temp_deci_c,
		int enabled, int connector_mc)
{
	return enabled && temp_deci_c > OPD_SVOOC_NORMAL_HIGH_DC &&
	       temp_deci_c < OPD_SVOOC_WARM_DC && connector_mc >= 0 &&
	       connector_mc < OPD_SVOOC_WARM_DC * 100;
}

/*
 * OPD2513 stock DT svooc_charge_strategy, soc_range = 0/20/35/55/75/85/90.
 * The existing guarded voltage range (3450..4450 mV) is below the first
 * voltage breakpoint in these normal-temperature curves (4480 or 4540 mV).
 * The 0..20, 20..35 and 35..55 bands all start at 61 x 100 mA. The
 * remaining supported bands start at 30, or 35/32 in normal-high.
 * Return zero outside the implemented guarded envelope; do not extrapolate
 * the stock warm, cold or taper curves into those ranges.
 */
static inline int opd2513_stock_ask(int soc, int temp_deci_c, int voltage_mv,
		int normal_high_enabled, int connector_mc)
{
	if (soc < 0 || soc > 80 || temp_deci_c < 210 ||
	    (temp_deci_c > OPD_SVOOC_NORMAL_HIGH_DC &&
	     !opd2513_normal_high_allowed(temp_deci_c, normal_high_enabled, connector_mc)) ||
	    voltage_mv < 3450 || voltage_mv > 4450)
		return 0;
	if (opd2513_high_current_soc(soc))
		return 61;
	if (temp_deci_c < OPD_SVOOC_NORMAL_HIGH_DC)
		return 30;
	return soc <= 75 ? 35 : 32;
}

/* OEM chooses the SOC band at plug-in. Keep the live SOC, voltage and
 * temperature validity gates active for the entire session.
 */
static inline int opd2513_stock_session_ask(int plug_soc, int live_soc,
		int temp_deci_c, int voltage_mv, int normal_high_enabled, int connector_mc)
{
	if (live_soc < 0 || live_soc > 80)
		return 0;
	return opd2513_stock_ask(plug_soc, temp_deci_c, voltage_mv,
				 normal_high_enabled, connector_mc);
}

/* In the opt-in SVOOC mode, skip the plug-in SOC current step while
 * the normal-low operating envelope is valid. The stock admission,
 * live high-SOC cutoff and voltage bounds still apply.
 */
static inline int opd2513_supervooc_session_ask(int plug_soc, int live_soc,
		int temp_deci_c, int voltage_mv, int normal_high_enabled,
		int connector_mc, int skip_soc_step)
{
	int stock = opd2513_stock_session_ask(plug_soc, live_soc, temp_deci_c,
					     voltage_mv, normal_high_enabled, connector_mc);

	/* Battery warm-band temperatures and connector temperatures are
	 * different inputs. The connector keeps its existing 55 C gate.
	 */
	if (stock && skip_soc_step && temp_deci_c < OPD_SVOOC_NORMAL_HIGH_DC &&
	    connector_mc >= 0 && connector_mc < OPD_CONNECTOR_GUARD_MC)
		return 61;
	return stock;
}

/* Stock replies with the selected curve's current on the first request. */
static inline unsigned char opd2513_first_current_target(unsigned char target,
		int high_band_enabled)
{
	if (target == 32 || target == 35 || (target == 61 && high_band_enabled))
		return target;
	return 30;
}

/* If both pumps are already switching on the first request, skip the
 * legacy ramp. Keep this inside the same bounded startup window.
 */
static inline int opd2513_first_current_dual_ready(unsigned char target,
		unsigned char bootstrap, unsigned char sent, int guards_ok,
		int primary_switching, int secondary_switching,
		unsigned int elapsed_ms)
{
	return !sent && target >= bootstrap && target <= 61 && guards_ok &&
	       primary_switching && secondary_switching && elapsed_ms < 2000;
}

/* A bounded bootstrap for supported stock targets. A failed or
 * completed slave start returns to the ordinary dual-pump policy.
 */
static inline int opd2513_first_current_allowed(unsigned char target,
		unsigned char bootstrap,
		unsigned char sent, unsigned char ask, int pumps_ready,
		int secondary_checked, unsigned int elapsed_ms,
		int switching_or_enable_grace, int primary_loss_pending,
		int primary_start_pending)
{
	if (target < bootstrap || target > 61 || !pumps_ready || secondary_checked ||
	    elapsed_ms >= 2000)
		return 0;
	if (!sent)
		return switching_or_enable_grace;
	/* Keep the transmitted grant while the pump first starts or during
	 * a transient switching-status drop. The deadline and pump fault
	 * checks still apply, including the three-frame switching-loss check.
	 */
	return sent == bootstrap && ask == bootstrap &&
	       (switching_or_enable_grace || primary_loss_pending ||
		primary_start_pending);
}

/* Called only by the serialized PHY thread. Times are monotonic milliseconds. */
static inline int opd2513_ask_step(unsigned char *ask, unsigned char sent,
		unsigned char target, int secondary_ready, unsigned long long now,
		unsigned long long *next_raise, unsigned long long *ack_deadline)
{
	unsigned char previous = *ask;

	if (!secondary_ready && target > 18)
		target = 18;
	if (target < previous)
		*ask = target;
	else if (target > previous && sent == previous && now >= *next_raise) {
		*ask = target < previous + 5 ? target : previous + 5;
		*next_raise = now + 1000;
	}
	if ((sent || *ack_deadline) && sent != *ask) {
		if (previous != *ask || !*ack_deadline)
			*ack_deadline = now + 2000;
		if (now > *ack_deadline)
			return -1;
	} else {
		*ack_deadline = 0;
	}
	return 0;
}

/* Arm the first-request timeout only after the PHY notice was transmitted. */
static inline void opd2513_ask_notified(unsigned char ask, unsigned char sent,
		unsigned long long now, unsigned long long *ack_deadline)
{
	if (ask != sent && !*ack_deadline)
		*ack_deadline = now + 2000;
}

#endif
