// SPDX-License-Identifier: GPL-2.0-only
/* OPD2513 SC8547D SuperVOOC adapter and bounded pump trials. */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/consumer.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_qos.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>
#include <linux/power_supply.h>
#include <linux/random.h>
#include <linux/sysfs.h>
#include <linux/usb/wcd939x-usbss.h>
#include <linux/workqueue.h>

#include "vooc_logic.h"
#include "opd2513_policy.h"
#include "ufcs_query.h"

#define VOOC_CTRL	0x07
#define VOOC_STAT	0x06
#define VOOC_CHIP_ID	0x36
#define VOOC_RX		0x2e
#define VOOC_TX		0x2c
#define OPD_TRIAL_MS	1800000
#define OPD_DUAL_TRIAL_MS	180000
/* 100 mA units. Leave room above the 1.4 A secondary-enable threshold. */
#define OPD_DUAL_CAP	18
/* OPD2513 stock DT: oplus_spec,voocphy_current_default = <0x3d>. */
#define OPD_STOCK_ASK	61
#define OPD_HEALTH_MAX_MS	1500
#define OPD_REARM_MS	3000
/* OEM AP VOOC PHY uses a 0 us CPU idle constraint on Linux >= 5.10. */
#define OPD_OEM_CPU_LATENCY_US	0
#define OPD_TRIAL_CPU_LATENCY_US	400

enum { OPD_USB_TEMP, OPD_BATT_CONNECTOR_TEMP, OPD_MOS_TEMP };

static const char *const opd_sensor_names[] = {
	[OPD_USB_TEMP] = "usb-connector",
	[OPD_BATT_CONNECTOR_TEMP] = "battery-connector",
	[OPD_MOS_TEMP] = "vooc-mos",
};

extern int qcom_battmgr_oplus_battery_authentic(struct power_supply *psy);

static bool powered_trial;
module_param(powered_trial, bool, 0400);
MODULE_PARM_DESC(powered_trial, "Enable bounded charging (1.3 A single, 1.8 A dual)");

static bool oem_policy;
module_param(oem_policy, bool, 0400);
MODULE_PARM_DESC(oem_policy, "Use stock SOC/temperature current bands with a 30 minute session limit");

static bool oem_normal_high;
module_param(oem_normal_high, bool, 0400);
MODULE_PARM_DESC(oem_normal_high, "Opt in to guarded stock normal-high temperature bands for dual pumps");

static bool oem_first_3a;
module_param(oem_first_3a, bool, 0400);
MODULE_PARM_DESC(oem_first_3a, "Use the guarded stock first CURR reply (6.1 A requires the high-band option)");

static bool svooc_skip_soc_step;
module_param(svooc_skip_soc_step, bool, 0400);
MODULE_PARM_DESC(svooc_skip_soc_step, "Allow the normal-low 6.1 A ramp above the stock plug-in SOC step");

static bool oem_high_band_first_6a1;
module_param(oem_high_band_first_6a1, bool, 0400);
MODULE_PARM_DESC(oem_high_band_first_6a1, "Use the stock 6.1 A first CURR reply in the high-current band");

static bool oem_ufcs_probe;
module_param(oem_ufcs_probe, bool, 0400);
MODULE_PARM_DESC(oem_ufcs_probe, "Query the wall adapter UFCS capabilities before SuperVOOC");
#ifdef OPD2513_OEM_UFCS
static bool oem_ufcs_authenticated_probe;
module_param(oem_ufcs_authenticated_probe, bool, 0400);
MODULE_PARM_DESC(oem_ufcs_authenticated_probe, "Query UFCS capabilities following OEM adapter authentication");
static bool oem_ufcs_startup_trial;
module_param(oem_ufcs_startup_trial, bool, 0400);
static bool oem_ufcs_powered_trial;
module_param(oem_ufcs_powered_trial, bool, 0400);
MODULE_PARM_DESC(oem_ufcs_powered_trial, "Run the bounded authenticated UFCS dual-pump trial in the high-current SOC band");
MODULE_PARM_DESC(oem_ufcs_startup_trial, "Try the authenticated OEM 5–5.5 V startup PDO with the pumps idle");
#endif

static bool dual_trial;
module_param(dual_trial, bool, 0400);
MODULE_PARM_DESC(dual_trial, "Probe the secondary pump; exercise it only with powered_trial");

extern int qcom_battmgr_oplus_vooc_hold(struct power_supply *psy, bool hold);

struct opd_vooc {
	struct i2c_client *pump;
	struct i2c_client *secondary;
	struct device *mux;
	struct iio_channel *sensors[ARRAY_SIZE(opd_sensor_names)];
	struct notifier_block notify;
	struct delayed_work work;
	struct wakeup_source *wake;
	struct completion end;
	struct vl link;
	struct vl3 run;
	ktime_t deadline;
	ktime_t last_frame;
	ktime_t last_health;
	ktime_t enabled_at;
	ktime_t rearm_at;
	unsigned long long next_raise;
	unsigned long long ask_deadline;
	u8 oem_target;
	u8 peak_ask;
	u8 first_current;
	unsigned int current_replies;
	unsigned int change_notices;
	unsigned int current_tx_retries;
	unsigned int vbus_replies[4];
	unsigned int voltage_replies;
	u16 last_current_tx;
	u16 last_voltage_tx;
	int last_window_delta;
	struct {
		bool recorded;
		int elapsed_ms, primary_ma, secondary_ma;
		u8 target, vbus_state, lost, secondary_enabled, secondary_checked;
		bool guards_ok, primary_switching, bootstrap_allowed;
	} first_downshift;
	bool enable_attempted;
	bool secondary_touched;
	bool stopping;
	bool fault_latched;
	bool removing;
	u64 input_uw_sum;
	unsigned int input_samples;
	bool primary_switching;
	int result;
	unsigned int slow_replies;
	unsigned int max_reply_us;
	unsigned int max_current_reply_us;
	bool attempted;
};

static int opd_write(struct opd_vooc *vooc, const struct vl_row *row, int phase)
{
	/* Enabling the pump is handled separately after protection readback. */
	if (!(vl_write_phase_m(row->reg, row->w, row->val,
			      powered_trial ? VM_S3 : VM_S2) & phase) ||
	    (row->reg == VOOC_CTRL && (row->val & BIT(7))))
		return -EPERM;
	if (row->w == 2)
		return i2c_smbus_write_word_data(vooc->pump, row->reg, row->val);
	return i2c_smbus_write_byte_data(vooc->pump, row->reg, row->val);
}

static int opd_read_block(struct i2c_client *pump, u8 reg, u8 size, u8 *data)
{
	int ret = i2c_smbus_read_i2c_block_data(pump, reg, size, data);

	return ret == size ? 0 : ret < 0 ? ret : -EIO;
}

static int opd_secondary_write(void *data, u8 reg, u8 size, u16 val)
{
	struct opd_vooc *vooc = data;

	if (!vl3b_write_ok(reg, size, val, VM_S3B))
		return -EPERM;
	return i2c_smbus_write_byte_data(vooc->secondary, reg, val);
}

static int opd_secondary_read(void *data, u8 reg, u8 size, u8 *buf)
{
	struct opd_vooc *vooc = data;

	return i2c_smbus_read_i2c_block_data(vooc->secondary, reg, size, buf);
}

static int opd_secondary_enable(void *data)
{
	struct opd_vooc *vooc = data;

	if (READ_ONCE(vooc->stopping) || READ_ONCE(vooc->removing) ||
	    ktime_after(ktime_get(), vooc->deadline) ||
	    ktime_ms_delta(ktime_get(), READ_ONCE(vooc->last_health)) > OPD_HEALTH_MAX_MS ||
	    !vooc->enable_attempted || vooc->link.model != 0x4b)
		return -EPERM;
	return i2c_smbus_write_byte_data(vooc->secondary, VOOC_CTRL, 0x81);
}

static int opd_check_pump(struct opd_vooc *vooc)
{
	const struct vl_row freeze = { 0x11, 1, 0xa0 }, unfreeze = { 0x11, 1, 0x80 };
	struct i2c_client *pump = vooc->pump;
	u8 regs[17] = {}, raw[14];
	struct vl_adc adc;
	int ret, thaw, elapsed = 0;

	ret = opd_read_block(pump, 0x06, 10, regs + 6) ?:
	      opd_read_block(pump, 0x04, 2, regs + 4) ?:
	      opd_read_block(pump, 0x00, 3, regs) ?:
	      opd_read_block(pump, 0x10, 1, regs + 16);
	if (ret)
		return ret;
	ret = opd_write(vooc, &freeze, VW_SESS);
	if (ret)
		return ret;
	ret = opd_read_block(pump, 0x13, sizeof(raw), raw);
	thaw = opd_write(vooc, &unfreeze, VW_SESS);
	if (ret || thaw)
		return ret ?: thaw;
	vl_adc_decode(&adc, raw);
	if (vooc->enable_attempted)
		elapsed = ktime_ms_delta(ktime_get(), vooc->enabled_at);
	ret = vl3_post(&vooc->run, regs, &adc, elapsed);
	if (ret) {
		dev_warn(&pump->dev, "SuperVOOC pump stopped: %s\n", vl3_why(ret));
		return -ERANGE;
	}
	vooc->primary_switching = vooc->enable_attempted && (regs[VOOC_STAT] & BIT(2));
	vooc->run.adc = adc;
	vooc->run.adc_ok = 1;
	return 0;
}

/* Run only in the threaded IRQ: serialize target changes with PHY replies. */
static int opd_update_ask(struct opd_vooc *vooc, ktime_t now, bool current_query)
{
	struct vl3 *run = &vooc->run;
	u8 target = READ_ONCE(vooc->oem_target);
	bool secondary_ready = run->s.checked && run->s.sw_now;
	u8 bootstrap = opd2513_first_current_target(target, oem_high_band_first_6a1);
	bool initial_allowed = false;
	bool guards_ok = run->suspend_ok && run->prot_ok && run->s.hw_done &&
		run->s.prot_ok && run->adc_ok && run->vbus_st == VB_NORMAL;
	u8 old_ask = run->ask4;
	int elapsed_ms = vooc->enable_attempted ?
		ktime_ms_delta(now, vooc->enabled_at) : 0;
	int ret;

	if (!oem_policy || !run->stage4)
		return 0;
	if (oem_first_3a && run->ph == V3_RUN && vooc->enable_attempted) {
		initial_allowed = opd2513_first_current_allowed(target, bootstrap,
			run->sent, run->ask4,
			guards_ok,
			run->s.checked, elapsed_ms,
			vooc->primary_switching || elapsed_ms < V3_ENABLE_GRACE_MS,
			run->sw_seen && run->n_lost > 0 && run->n_lost < 3,
			!run->sw_seen && run->enabled);
		if (current_query && opd2513_first_current_dual_ready(target,
				bootstrap, run->sent, guards_ok, vooc->primary_switching,
				secondary_ready, elapsed_ms))
			initial_allowed = true;
	}
	/* Stock selects the curve in ASK_CURRENT_LEVEL itself. A preceding
	 * BVOL notice may already have armed the timeout for the startup ask.
	 * It must not lock in that placeholder for the first real reply.
	 */
	if (initial_allowed) {
		target = bootstrap;
		if (current_query && !run->sent)
			run->ask4 = target;
	}
	ret = opd2513_ask_step(&run->ask4, run->sent, target,
			secondary_ready || initial_allowed, ktime_to_ms(now),
			&vooc->next_raise, &vooc->ask_deadline);
	/* Capture the decision before the voltage reply or pump readback changes
	 * the inputs. Print it after the IRQ is released, outside reply timing.
	 */
	if (old_ask == vooc->first_current && old_ask >= 30 &&
	    run->sent == old_ask && run->ask4 < old_ask &&
	    !vooc->first_downshift.recorded) {
		vooc->first_downshift.recorded = true;
		vooc->first_downshift.elapsed_ms = elapsed_ms;
		vooc->first_downshift.target = target;
		vooc->first_downshift.vbus_state = run->vbus_st;
		vooc->first_downshift.guards_ok = guards_ok;
		vooc->first_downshift.primary_switching = vooc->primary_switching;
		vooc->first_downshift.lost = run->n_lost;
		vooc->first_downshift.secondary_enabled = run->s.enabled;
		vooc->first_downshift.secondary_checked = run->s.checked;
		vooc->first_downshift.primary_ma = run->adc.ibus;
		vooc->first_downshift.secondary_ma = run->s.adc.ibus;
		vooc->first_downshift.bootstrap_allowed = initial_allowed;
	}
	/* Allow the previously agreed current during an adapter transition. */
	run->lim4 = max(run->sent, run->ask4);
	if (ret)
		return -ETIMEDOUT;
	return 0;
}

static irqreturn_t opd_frame(int irq, void *data)
{
	struct opd_vooc *vooc = data;
	const struct vl3_io secondary_io = {
		.c = vooc, .wr = opd_secondary_write, .rd = opd_secondary_read,
		.en = opd_secondary_enable,
	};
	struct vl_row reply = { .reg = VOOC_TX, .w = 2 };
	ktime_t start = ktime_get();
	struct vl_row predata = { .reg = 0x31, .w = 2 };
	int frame, ret, exchange, i;
	u8 flags, previous_current;
	bool current_reply = false;
	s64 elapsed;

	if (completion_done(&vooc->end) || READ_ONCE(vooc->stopping))
		return IRQ_HANDLED;

	if (powered_trial && ktime_after(start, vooc->deadline)) {
		ret = -ETIME;
		goto finish;
	}
	if (powered_trial &&
	    ktime_ms_delta(start, READ_ONCE(vooc->last_health)) > OPD_HEALTH_MAX_MS) {
		ret = -ESTALE;
		goto finish;
	}

	frame = i2c_smbus_read_word_data(vooc->pump, VOOC_RX);
	if (frame < 0) {
		ret = frame;
		goto finish;
	}
	flags = frame >> 8;
	if (vooc->run.stage4 && vl3_current_tx_status(&vooc->run, &flags))
		vooc->current_tx_retries++;
	if (!vooc->run.tx_pending) {
		ret = opd_update_ask(vooc, start,
			(flags & VF_RXDONE) && ((frame & 0x1e) >> 1) == VC3_CURR);
		if (ret)
			goto finish;
	}

	previous_current = vooc->run.sent;
	if (flags & VF_RXDONE)
		WRITE_ONCE(vooc->last_frame, start);
	if (powered_trial)
		exchange = vl3_irq(&vooc->link, &vooc->run, frame & 0xff,
				   flags, &reply.val);
	else
		exchange = vl_irq(&vooc->link, frame & 0xff, flags, &reply.val);
	if (oem_policy && vooc->run.stage4)
		vooc->run.lim4 = max(vooc->run.sent, vooc->run.ask4);
	if (exchange < 0) {
		dev_info(&vooc->pump->dev, "adapter frame %02x stopped: %s, adapter ID %02x\n",
			 frame & 0xff, vl_why(exchange), vooc->link.model);
		ret = -EPROTO;
		goto finish;
	}
	if (!exchange) {
		if (vooc->link.state != VL_STOP)
			return IRQ_HANDLED;
		ret = -EPROTO;
		goto finish;
	}

	if (powered_trial && (vooc->run.act & V3A_ENABLE)) {
		ret = -EPERM;
		if (READ_ONCE(vooc->stopping) || READ_ONCE(vooc->removing) ||
		    !vooc->run.suspend_ok ||
		    !vooc->run.prot_ok || vooc->link.model != 0x4b ||
		    vooc->run.adc.ibus > 100 ||
		    vooc->run.adc.vbat < 3450)
			goto finish;
		vooc->enabled_at = ktime_get();
		vooc->enable_attempted = true;
		ret = i2c_smbus_write_byte_data(vooc->pump, VOOC_CTRL, 0x80);
		if (ret)
			goto finish;
	}
	ret = opd_write(vooc, &reply, VW_SESS);
	if (ret < 0)
		goto finish;
	if (powered_trial && vooc->run.ph != V3_ASK &&
	    (frame & (VF_RXDONE << 8))) {
		u8 cmd = (frame & 0x1e) >> 1;

		if (cmd == VC3_CURR) {
			current_reply = true;
			if (vooc->run.stage4 && vooc->run.sent != previous_current) {
				vooc->run.tx_previous = previous_current;
				vooc->run.tx_pending = 1;
				opd2513_ask_notified(vooc->run.sent, previous_current,
					ktime_to_ms(start), &vooc->ask_deadline);
			}
			if (!vooc->current_replies)
				vooc->first_current = vooc->run.sent;
			vooc->current_replies++;
			vooc->peak_ask = max(vooc->peak_ask, vooc->run.sent);
			vooc->last_current_tx = reply.val;
		}
		if (cmd == VC3_VBUS && vooc->run.vbus_st >= VB_LOW &&
		    vooc->run.vbus_st <= VB_NORMAL) {
			vooc->vbus_replies[vooc->run.vbus_st]++;
			vooc->last_window_delta = vooc->run.adc.vbus -
				2 * vooc->run.adc.vbat;
		}
		if (cmd == VC3_BVOL && reply.val != vl3_data_word(V3_CHG_CODE)) {
			vooc->voltage_replies++;
			vooc->last_voltage_tx = reply.val;
		}
		if (cmd == VC3_BVOL && reply.val == vl3_data_word(V3_CHG_CODE)) {
			vooc->change_notices++;
			if (oem_policy && vooc->run.stage4)
				opd2513_ask_notified(vooc->run.ask4, vooc->run.sent,
					ktime_to_ms(start), &vooc->ask_deadline);
		}
	}
	elapsed = ktime_us_delta(ktime_get(), start);
	vooc->max_reply_us = max_t(unsigned int, vooc->max_reply_us, elapsed);
	if (current_reply)
		vooc->max_current_reply_us = max_t(unsigned int,
					vooc->max_current_reply_us, elapsed);
	if (elapsed > 1900 || (elapsed > 1500 && ++vooc->slow_replies == 2)) {
		ret = -ETIMEDOUT;
		goto finish;
	}
	if (powered_trial) {
		if (exchange == VL_LAST) {
			dev_info(&vooc->pump->dev, "SuperVOOC adapter %02x identified\n", vooc->link.model);
			if (vooc->link.model != 0x4b) {
				ret = -EPROTONOSUPPORT;
				goto finish;
			}
		}
		if (vooc->run.act & V3A_PREDATA) {
			predata.val = vooc->run.predata;
			ret = opd_write(vooc, &predata, VW_HW | VW_RUN);
			if (ret)
				goto finish;
		}
		if (vooc->run.act & V3A_HW) {
			for (i = 0; i < ARRAY_SIZE(vl3_hw_rows); i++) {
				ret = opd_write(vooc, &vl3_hw_rows[i], VW_HW);
				if (ret)
					goto finish;
			}
			if (vooc->run.pumps == 2 && vl3b_hw(&secondary_io, &vooc->run)) {
				ret = -EIO;
				goto finish;
			}
		}
		if (vooc->run.ph != V3_ASK) {
			ret = opd_check_pump(vooc);
			if (ret)
				goto finish;
		}
		if (vooc->run.pumps == 2) {
			ret = vl3b_run(&secondary_io, &vooc->run,
				       vooc->enable_attempted ?
				       ktime_ms_delta(ktime_get(), vooc->enabled_at) : 0);
			if (ret) {
				dev_warn(&vooc->pump->dev, "secondary pump stopped: %s (%d)\n",
					 ret > 0 ? vl3_why(ret) : "I/O failure", ret);
				ret = ret > 0 ? -ERANGE : -EIO;
				goto finish;
			}
		}
		if (vooc->primary_switching) {
			unsigned int ibus = vooc->run.adc.ibus;

			if (vooc->run.pumps == 2 && vooc->run.s.sw_now)
				ibus += vooc->run.s.adc.ibus;
			vooc->input_uw_sum += (u64)vooc->run.adc.vbus * ibus;
			vooc->input_samples++;
		}
		return IRQ_HANDLED;
	}
	if (exchange != VL_LAST)
		return IRQ_HANDLED;
	ret = 0;
finish:
	vooc->result = ret;
	complete_all(&vooc->end);
	return IRQ_HANDLED;
}

static int opd_read_supply(struct power_supply *psy, enum power_supply_property prop, int *v)
{
	union power_supply_propval value;
	int ret = power_supply_get_property(psy, prop, &value);

	if (!ret)
		*v = value.intval;
	return ret;
}

static bool opd_adapter_candidate(struct power_supply *usb)
{
	int online, type, voltage;

	return !opd_read_supply(usb, POWER_SUPPLY_PROP_ONLINE, &online) && online &&
	       !opd_read_supply(usb, POWER_SUPPLY_PROP_USB_TYPE, &type) &&
	       type == POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID &&
	       !opd_read_supply(usb, POWER_SUPPLY_PROP_VOLTAGE_NOW, &voltage) &&
	       voltage >= 4750000 && voltage <= 5500000;
}

static int opd_battery_check(struct device *dev, bool normal_high,
		int connector_mc, int *plug_soc, bool skip_soc_step)
{
	struct power_supply *bat = power_supply_get_by_name("qcom-battmgr-bat");
	int voltage, temp, soc, health, ret;

	if (!bat)
		return -ENODEV;
	ret = opd_read_supply(bat, POWER_SUPPLY_PROP_VOLTAGE_NOW, &voltage) ?:
	      opd_read_supply(bat, POWER_SUPPLY_PROP_TEMP, &temp) ?:
	      opd_read_supply(bat, POWER_SUPPLY_PROP_CAPACITY, &soc);
	if (ret)
		goto out;
	ret = -ERANGE;
	if (voltage < 3450000 || voltage > 4450000 || temp < 210 ||
	    soc < 0 || soc > 80)
		goto out;
	if (temp > OPD_SVOOC_NORMAL_HIGH_DC &&
	    (!opd2513_normal_high_allowed(temp, normal_high, connector_mc) ||
	     opd_read_supply(bat, POWER_SUPPLY_PROP_HEALTH, &health) ||
	     health != POWER_SUPPLY_HEALTH_GOOD))
		goto out;
	ret = qcom_battmgr_oplus_battery_authentic(bat);
	if (ret != 1) {
		dev_warn_ratelimited(dev, "battery authentication returned %d\n", ret);
		ret = ret < 0 ? ret : -EPERM;
		goto out;
	}
	ret = 0;
	if (plug_soc) {
		if (*plug_soc < 0)
			*plug_soc = soc;
		ret = opd2513_supervooc_session_ask(*plug_soc, soc, temp, voltage / 1000,
						   normal_high, connector_mc, skip_soc_step);
		if (skip_soc_step && ret > 0 && ret < 61)
			dev_info_ratelimited(dev,
				"SuperVOOC policy derate: target=%d mA plug_SOC=%d live_SOC=%d battery_temp=%d dC connector=%d mC battery_voltage=%d uV normal_high=%u\n",
				ret * 100, *plug_soc, soc, temp, connector_mc,
				voltage, normal_high);
		if (!ret)
			ret = -ERANGE;
	}
out:
	power_supply_put(bat);
	return ret;
}

static int opd_read_temperatures(struct opd_vooc *vooc, int *temperatures)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(opd_sensor_names); i++) {
		ret = iio_read_channel_processed(vooc->sensors[i], &temperatures[i]);
		if (ret < 0) {
			dev_warn_ratelimited(&vooc->pump->dev,
				"SuperVOOC %s temperature read failed: %d\n",
				opd_sensor_names[i], ret);
			return ret;
		}
		/* The ADC uses millidegrees C. Reject rail or over-temperature readings. */
		if (temperatures[i] < 0 || temperatures[i] >= OPD_CONNECTOR_GUARD_MC) {
			dev_warn_ratelimited(&vooc->pump->dev,
				"SuperVOOC %s temperature outside range: %d mC\n",
				opd_sensor_names[i], temperatures[i]);
			return -ERANGE;
		}
	}
	return 0;
}

static bool opd_pump_idle(struct i2c_client *pump)
{
	int ctrl = i2c_smbus_read_byte_data(pump, VOOC_CTRL);
	int status = i2c_smbus_read_byte_data(pump, VOOC_STAT);

	return ctrl >= 0 && status >= 0 && vl_pump_idle(status, ctrl);
}

static bool opd_stop_pumps(struct opd_vooc *vooc)
{
	bool primary_off = !vooc->enable_attempted;
	bool secondary_off = !vooc->secondary_touched;
	int i;

	for (i = 0; i < 3; i++) {
		/* Send both stop requests before waiting for either readback. */
		if (!secondary_off)
			opd_secondary_write(vooc, VOOC_CTRL, 1, vl3b_exit_rows[0].val);
		if (!primary_off)
			opd_write(vooc, &vl_exit_rows[0], VW_EXIT);
		if (!secondary_off)
			secondary_off = opd_pump_idle(vooc->secondary);
		if (!primary_off)
			primary_off = opd_pump_idle(vooc->pump);
		if (secondary_off && primary_off)
			return true;
		msleep(20);
	}
	/* Stop I2C accesses so the armed hardware watchdogs can expire. */
	msleep(1100);
	if (!secondary_off)
		secondary_off = opd_pump_idle(vooc->secondary);
	if (!primary_off)
		primary_off = opd_pump_idle(vooc->pump);
	return secondary_off && primary_off;
}

/* SC8547D UFCS PHY registers, as used by the OEM UFCS HAL. */
#define UFCS_DPDM 0x21
#define UFCS_CTRL1 0x40
#define UFCS_CTRL2 0x41
#define UFCS_FLAGS 0x43
#define UFCS_MASK1 0x46
#define UFCS_MASK2 0x47
#define UFCS_TX_LEN 0x49
#define UFCS_TX_DATA 0x4a
#define UFCS_RX_LEN 0x8a
#define UFCS_RX_DATA 0x8b
#define UFCS_ENABLE BIT(7)
#define UFCS_HANDSHAKE BIT(5)
#define UFCS_SEND BIT(2)
#define UFCS_BAUD_SHIFT 3
#define UFCS_BAUD_COUNT 4
#define UFCS_ACK_TIMEOUT BIT(0)
#define UFCS_RX_READY BIT(4)
#define UFCS_SENT BIT(5)
#define UFCS_HS_OK BIT(6)
#define UFCS_FLAG_HS_FAIL BIT(7)
#define UFCS_BAD_FLAGS (BIT(1) | BIT(3) | UFCS_FLAG_HS_FAIL)

enum opd_ufcs_command {
	OPD_UFCS_PING = 0x00,
	OPD_UFCS_GET_CAPS = 0x06,
	OPD_UFCS_GET_DEVICE_INFO = 0x0a,
	OPD_UFCS_EXIT = 0x0f,
};

static int opd_ufcs_wait(struct opd_vooc *vooc, unsigned int wanted, u8 flags[3])
{
	ktime_t end = ktime_add_ms(ktime_get(), 300);
	int ret;

	do {
		if (READ_ONCE(vooc->removing))
			return -ESHUTDOWN;
		ret = opd_read_block(vooc->pump, UFCS_FLAGS, 3, flags);
		if (ret)
			return ret;
		if ((flags[0] & UFCS_BAD_FLAGS) || flags[1] || (flags[2] & BIT(7)))
			return -EPROTO;
		if (flags[0] & UFCS_ACK_TIMEOUT)
			return -EBUSY;
		if (flags[0] & wanted)
			return 0;
		usleep_range(1000, 2000);
	} while (ktime_before(ktime_get(), end));
	return -ETIMEDOUT;
}

static int opd_ufcs_send(struct i2c_client *pump, enum opd_ufcs_command cmd, u8 seq, u8 baud)
{
	const u8 msg[] = { 0x20 | (seq << 1), 0x08, cmd };
	int ret;

	ret = i2c_smbus_write_byte_data(pump, UFCS_TX_LEN, sizeof(msg));
	if (ret)
		return ret;
	ret = i2c_smbus_write_i2c_block_data(pump, UFCS_TX_DATA, sizeof(msg), msg);
	if (ret)
		return ret;
	return i2c_smbus_write_byte_data(pump, UFCS_CTRL1, UFCS_ENABLE | UFCS_SEND | (baud << UFCS_BAUD_SHIFT));
}

static int opd_ufcs_receive(struct i2c_client *pump, u8 *buf, unsigned int size)
{
	u8 rx_register = UFCS_RX_DATA;
	int len, ret;

	len = i2c_smbus_read_byte_data(pump, UFCS_RX_LEN);
	if (len < 0)
		return len;
	if (len < 3 || len > size)
		return -EBADMSG;
	if (len <= I2C_SMBUS_BLOCK_MAX) {
		ret = opd_read_block(pump, UFCS_RX_DATA, len, buf);
		return ret ? ret : len;
	}
	ret = i2c_master_send(pump, &rx_register, 1);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;
	ret = i2c_master_recv(pump, buf, len);
	return ret == len ? len : ret < 0 ? ret : -EIO;
}

/* Called with runtime PM held and the USB mux owned by this work item.
 * Query device identity and capabilities without requesting a power contract.
 */
static int opd_ufcs_probe(struct opd_vooc *vooc)
{
	static const u8 registers[] = { UFCS_DPDM, UFCS_CTRL2, UFCS_MASK1, UFCS_MASK2 };
	static const u8 values[] = { 0x01, 0x30, 0x01, 0x40 };
	struct i2c_client *pump = vooc->pump;
	u8 cap[OPD_UFCS_CAP_BYTES], saved[ARRAY_SIZE(registers)], flags[3] = {};
	u8 baud = 0, seq = 0;
	const char *phase = "setup";
	bool linked = false, clean = true;
	int ret, len, i, vendor = -1, current_ma = -1, exit_ret = 0;

	ret = i2c_smbus_read_byte_data(pump, UFCS_CTRL1);
	if (ret != 0)
		return ret < 0 ? ret : -EBUSY;
	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		ret = i2c_smbus_read_byte_data(pump, registers[i]);
		if (ret < 0)
			return ret;
		saved[i] = ret;
	}
	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		ret = i2c_smbus_write_byte_data(pump, registers[i], values[i]);
		if (ret)
			goto restore_ufcs;
	}
	ret = opd_read_block(pump, UFCS_FLAGS, sizeof(flags), flags);
	if (ret)
		goto restore_ufcs;
	ret = i2c_smbus_write_byte_data(pump, UFCS_CTRL1, UFCS_ENABLE | UFCS_HANDSHAKE);
	if (ret)
		goto restore_ufcs;
	phase = "handshake";
	ret = opd_ufcs_wait(vooc, UFCS_HS_OK, flags);
	if (ret)
		goto restore_ufcs;
	linked = true;
	phase = "ping";
	/* The OEM PHY tries 115200, 57600, 38400 and 19200 baud. */
	for (baud = 0; baud < UFCS_BAUD_COUNT; baud++) {
		ret = opd_ufcs_send(pump, OPD_UFCS_PING, seq++, baud);
		if (ret)
			goto restore_ufcs;
		ret = opd_ufcs_wait(vooc, UFCS_SENT, flags);
		if (ret != -EBUSY && ret != -ETIMEDOUT)
			break;
		/* Leave the OEM inter-message interval before the next ping. */
		msleep(10);
	}
	if (ret)
		goto restore_ufcs;
	/* The OEM policy checks the vendor before selecting its authentication
	 * and cable capability paths. Record the identity before reading PDOs.
	 */
	msleep(10);
	phase = "device-info";
	ret = opd_ufcs_send(pump, OPD_UFCS_GET_DEVICE_INFO, seq++, baud);
	if (ret)
		goto restore_ufcs;
	ret = opd_ufcs_wait(vooc, UFCS_RX_READY, flags);
	if (ret)
		goto restore_ufcs;
	len = opd_ufcs_receive(pump, cap, sizeof(cap));
	if (len < 0) {
		ret = len;
		goto restore_ufcs;
	}
	vendor = opd_ufcs_device_vendor(cap, len);
	if (vendor < 0) {
		ret = -EBADMSG;
		goto restore_ufcs;
	}
	msleep(10);
	phase = "capabilities";
	ret = opd_ufcs_send(pump, OPD_UFCS_GET_CAPS, seq++, baud);
	if (ret)
		goto restore_ufcs;
	ret = opd_ufcs_wait(vooc, UFCS_RX_READY, flags);
	if (ret)
		goto restore_ufcs;
	len = opd_ufcs_receive(pump, cap, sizeof(cap));
	if (len < 0) {
		ret = len;
		goto restore_ufcs;
	}
	current_ma = opd_ufcs_cap_current(cap, len, OPD_UFCS_TARGET_MV);
	ret = current_ma < 0 ? -EBADMSG : 0;

restore_ufcs:
	dev_info(&pump->dev,
		 "UFCS capability response: phase=%s baud_index=%u flags=%02x/%02x/%02x\n",
		 phase, baud, flags[0], flags[1], flags[2]);
	if (linked && baud < UFCS_BAUD_COUNT && !READ_ONCE(vooc->removing)) {
		msleep(10);
		exit_ret = opd_ufcs_send(pump, OPD_UFCS_EXIT, seq, baud);
		if (!exit_ret)
			exit_ret = opd_ufcs_wait(vooc, UFCS_SENT, flags);
	}
	if (i2c_smbus_write_byte_data(pump, UFCS_CTRL1, 0) ||
	    i2c_smbus_read_byte_data(pump, UFCS_CTRL1) != 0)
		clean = false;
	for (i = ARRAY_SIZE(registers); i-- > 0;)
		if (i2c_smbus_write_byte_data(pump, registers[i], saved[i]) ||
		    i2c_smbus_read_byte_data(pump, registers[i]) != saved[i])
			clean = false;
	if (vendor >= 0)
		dev_info(&pump->dev, "UFCS source device vendor=0x%04x\n", vendor);
	dev_info(&pump->dev, "UFCS capability probe: phase=%s handshake=%d result=%d advertised_11V_mA=%d restored=%d exit=%d\n",
		 phase, linked, ret, current_ma, clean, exit_ret);
	return clean ? 0 : -EIO;
}

#ifdef OPD2513_OEM_UFCS
#include "ufcs_oem_phy.h"
#endif

static void opd_identify(struct opd_vooc *vooc, struct power_supply *usb)
{
	const struct vl3_io secondary_io = { .c = vooc, .wr = opd_secondary_write };
	struct i2c_client *pump = vooc->pump;
	struct device *bus = pump->adapter->dev.parent;
	struct device *secondary_bus = NULL;
	struct pm_qos_request qos = {};
	struct power_supply *battery = NULL;
	ktime_t started, next_sample, last;
	bool main_touched = false, off = true, charging_notified = false;
	bool normal_high = oem_normal_high && powered_trial && oem_policy && vooc->secondary;
	unsigned int trial_ms = OPD_TRIAL_MS;
	int online, cpu_latency_us;
	u16 random;
	int temperatures[ARRAY_SIZE(opd_sensor_names)];
	int ret, cleanup, i, status, ctrl, stock_ask;
	int plug_soc = -1;
	int *ufcs_admission_soc = NULL;
	bool authenticate = false;

#ifdef OPD2513_OEM_UFCS
	int ufcs_plug_soc = -1;

	authenticate = oem_ufcs_authenticated_probe;
	if (authenticate && oem_ufcs_powered_trial)
		ufcs_admission_soc = &ufcs_plug_soc;
#endif

	ret = opd_read_temperatures(vooc, temperatures);
	if (ret) {
		dev_warn(&pump->dev, "charging temperature gate failed: %d\n", ret);
		return;
	}

	ret = opd_battery_check(&pump->dev, normal_high,
				temperatures[OPD_BATT_CONNECTOR_TEMP], ufcs_admission_soc, false);
	if (ret < 0)
		return;

	if (ufcs_admission_soc && !opd2513_high_current_soc(*ufcs_admission_soc)) {
		dev_info(&pump->dev, "UFCS powered trial skipped: plug SOC=%d\n",
			 *ufcs_admission_soc);
		authenticate = false;
	}

	vooc->attempted = true;
	__pm_stay_awake(vooc->wake);
	ret = pm_runtime_resume_and_get(bus);
	if (ret < 0)
		goto relax;

	status = i2c_smbus_read_byte_data(pump, VOOC_STAT);
	ctrl = i2c_smbus_read_byte_data(pump, VOOC_CTRL);
	if (i2c_smbus_read_byte_data(pump, VOOC_CHIP_ID) != 0x49 ||
	    status < 0 || ctrl != 0 || !vl_pump_idle(status, ctrl)) {
		ret = -EBUSY;
		goto put_bus;
	}
	if (READ_ONCE(vooc->removing) || !opd_adapter_candidate(usb)) {
		ret = -ENOLINK;
		goto put_bus;
	}
	ret = wcd939x_usbss_claim_handshake(vooc->mux, &pump->dev);
	if (ret)
		goto put_bus;
	cpu_latency_us = powered_trial && oem_policy ? OPD_OEM_CPU_LATENCY_US :
			 OPD_TRIAL_CPU_LATENCY_US;
	cpu_latency_qos_add_request(&qos, cpu_latency_us);

	vooc->enable_attempted = false;
	vooc->secondary_touched = false;
	if (oem_ufcs_probe || authenticate) {
#ifdef OPD2513_OEM_UFCS
		if (authenticate)
			ret = opd_ufcs_authenticated_probe(vooc, usb);
		else
#endif
			ret = opd_ufcs_probe(vooc);
		if (ret) {
			vooc->fault_latched = (ret == -EIO);
			goto restore;
		}
		if (READ_ONCE(vooc->removing) ||
		    opd_read_supply(usb, POWER_SUPPLY_PROP_ONLINE, &online) || !online ||
		    opd_read_supply(usb, POWER_SUPPLY_PROP_VOLTAGE_NOW, &status) ||
		    status < 4750000 || status > 5500000) {
			ret = -ENOLINK;
			goto restore;
		}
	}
	vooc->primary_switching = false;
	vooc->input_uw_sum = 0;
	vooc->input_samples = 0;
	vooc->peak_ask = 0;
	vooc->current_replies = 0;
	vooc->first_current = 0;
	vooc->change_notices = 0;
	vooc->current_tx_retries = 0;
	vooc->first_downshift.recorded = false;
	memset(vooc->vbus_replies, 0, sizeof(vooc->vbus_replies));
	vooc->voltage_replies = 0;
	vooc->last_current_tx = 0;
	vooc->last_voltage_tx = 0;
	vooc->last_window_delta = 0;
	WRITE_ONCE(vooc->stopping, false);
	vl3_init(&vooc->run, V3_CAP_MAX, false);
	vooc->run.stock_ask = OPD_STOCK_ASK;
	if (powered_trial && vooc->secondary) {
		struct i2c_client *secondary = vooc->secondary;
		struct device *dev = secondary->adapter->dev.parent;

		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0)
			goto restore;
		secondary_bus = dev;
		status = i2c_smbus_read_byte_data(secondary, VOOC_STAT);
		ctrl = i2c_smbus_read_byte_data(secondary, VOOC_CTRL);
		if (status < 0 || ctrl < 0 || !vl_pump_idle(status, ctrl)) {
			ret = -EBUSY;
			goto restore;
		}
		vooc->run.pumps = 2;
		vooc->run.cap = OPD_DUAL_CAP;
		trial_ms = OPD_DUAL_TRIAL_MS;
		if (oem_policy) {
			stock_ask = opd_battery_check(&pump->dev, normal_high,
					      temperatures[OPD_BATT_CONNECTOR_TEMP], &plug_soc,
					      svooc_skip_soc_step);
			if (stock_ask <= 0) {
				ret = stock_ask ?: -ERANGE;
				goto restore;
			}
			vooc->run.cap = stock_ask;
			vooc->run.stage4 = 1;
			vooc->run.ask4 = min_t(u8, stock_ask, OPD_DUAL_CAP);
			vooc->run.lim4 = vooc->run.ask4;
			WRITE_ONCE(vooc->oem_target, stock_ask);
			vooc->ask_deadline = 0;
			vooc->next_raise = ktime_to_ms(ktime_get()) + 1000;
			trial_ms = OPD_TRIAL_MS;
			dev_info(&pump->dev, "SuperVOOC policy: target %d mA, start %u mA, session %u ms, plug SOC %d%%, skip SOC step %u\n",
				 stock_ask * 100, vooc->run.ask4 * 100, trial_ms, plug_soc,
				 svooc_skip_soc_step);
		}
	}
	if (powered_trial) {
		/* D+/D- routing can change USB type after we claim the mux. */
		if (READ_ONCE(vooc->removing) ||
		    opd_read_supply(usb, POWER_SUPPLY_PROP_ONLINE, &online) || !online) {
			ret = -ENOLINK;
			goto restore;
		}
		battery = power_supply_get_by_name("qcom-battmgr-bat");
		if (!battery) {
			ret = -ENODEV;
			goto restore;
		}
		ret = qcom_battmgr_oplus_vooc_hold(battery, true);
		if (ret) {
			dev_warn(&pump->dev, "SuperVOOC buck handoff failed: %d\n", ret);
			goto restore;
		}
		vooc->run.suspend_ok = 1;
	}
	msleep(10);
	main_touched = true;
	for (i = 0; i < ARRAY_SIZE(vl_init_rows); i++) {
		ret = opd_write(vooc, &vl_init_rows[i], VW_INIT);
		if (ret)
			goto restore;
	}
	if (vooc->run.pumps == 2) {
		vooc->secondary_touched = true;
		ret = vl3b_init(&secondary_io);
		if (ret) {
			ret = -EIO;
			goto restore;
		}
	}

	get_random_bytes(&random, sizeof(random));
	vl_init(&vooc->link, random, random >> 8);
	vooc->slow_replies = 0;
	vooc->max_reply_us = 0;
	vooc->max_current_reply_us = 0;
	reinit_completion(&vooc->end);
	started = ktime_get();
	next_sample = ktime_add_ms(started, 500);
	WRITE_ONCE(vooc->last_frame, 0);
	WRITE_ONCE(vooc->last_health, started);
	vooc->deadline = ktime_add_ms(started, trial_ms);
	ret = request_threaded_irq(pump->irq, NULL, opd_frame,
				   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				   KBUILD_MODNAME, vooc);
	if (ret)
		goto restore;

	ret = opd_write(vooc, &vl_handshake, VW_INIT);
	if (!ret && powered_trial) {
		while (!wait_for_completion_timeout(&vooc->end, msecs_to_jiffies(20))) {
			ktime_t now = ktime_get();

			if (READ_ONCE(vooc->removing)) {
				ret = -ESHUTDOWN;
				break;
			}
			last = READ_ONCE(vooc->last_frame);
			if (ktime_after(now, vooc->deadline)) {
				ret = -ETIME;
				break;
			}
			if ((last && ktime_ms_delta(now, last) > 200) ||
			    (!last && ktime_ms_delta(now, started) > 2000)) {
				ret = -ETIMEDOUT;
				break;
			}
			if (ktime_before(now, next_sample))
				continue;
			next_sample = ktime_add_ms(now, 500);
			ret = opd_read_temperatures(vooc, temperatures);
			if (ret)
				break;
			ret = opd_read_supply(usb, POWER_SUPPLY_PROP_ONLINE, &online);
			if (ret) {
				dev_warn(&pump->dev, "SuperVOOC USB status read failed: %d\n", ret);
				break;
			}
			if (!online) {
				dev_info(&pump->dev, "SuperVOOC USB source went offline\n");
				ret = -ENOLINK;
				break;
			}
			stock_ask = opd_battery_check(&pump->dev, normal_high,
					      temperatures[OPD_BATT_CONNECTOR_TEMP],
					      vooc->run.stage4 ? &plug_soc : NULL,
					      svooc_skip_soc_step);
			if (stock_ask < 0) {
				dev_warn(&pump->dev, "SuperVOOC battery gate closed: %d, connector %d mC\n",
					 stock_ask, temperatures[OPD_BATT_CONNECTOR_TEMP]);
				ret = stock_ask;
				break;
			}
			if (vooc->run.stage4)
				WRITE_ONCE(vooc->oem_target, min_t(u8, stock_ask, vooc->run.cap));
			if (!charging_notified && READ_ONCE(vooc->primary_switching)) {
				power_supply_changed(battery);
				charging_notified = true;
			}
			WRITE_ONCE(vooc->last_health, now);
		}
		if (!ret)
			ret = vooc->result;
	} else if (!ret)
		ret = wait_for_completion_timeout(&vooc->end, msecs_to_jiffies(2500)) ?
			vooc->result : -ETIMEDOUT;
	WRITE_ONCE(vooc->stopping, true);
	free_irq(pump->irq, vooc);
	if (vooc->first_downshift.recorded)
		dev_info(&pump->dev,
			 "first current downshift: enable_ms=%d target=%u vbus_state=%u guards=%u primary_switching=%u primary_lost=%u secondary_enabled=%u secondary_checked=%u primary_mA=%d secondary_mA=%d bootstrap=%u\n",
			 vooc->first_downshift.elapsed_ms, vooc->first_downshift.target,
			 vooc->first_downshift.vbus_state, vooc->first_downshift.guards_ok,
			 vooc->first_downshift.primary_switching, vooc->first_downshift.lost,
			 vooc->first_downshift.secondary_enabled, vooc->first_downshift.secondary_checked,
			 vooc->first_downshift.primary_ma, vooc->first_downshift.secondary_ma,
			 vooc->first_downshift.bootstrap_allowed);
	if (powered_trial)
		dev_info(&pump->dev,
			 "SuperVOOC trial: frames=%d peak primary IBUS=%d mA max primary die=%d dC mean total pump input=%u uW over %u samples\n",
			 vooc->run.nrun, vooc->run.ibus_max, vooc->run.tdie_max,
			 vooc->input_samples ? (u32)div_u64(vooc->input_uw_sum, vooc->input_samples) : 0,
			 vooc->input_samples);
	else if (!ret)
		dev_info(&pump->dev, "SuperVOOC adapter %02x identified\n", vooc->link.model);
	if (oem_policy)
		dev_info(&pump->dev, "current negotiation: target=%u ask=%u replied=%u peak=%u first=%u (100 mA), current-replies=%u change-notices=%u current-tx-retries=%u slave-switching=%u\n",
			 READ_ONCE(vooc->oem_target), vooc->run.ask4, vooc->run.sent,
			 vooc->peak_ask, vooc->first_current, vooc->current_replies, vooc->change_notices,
			 vooc->current_tx_retries, vooc->run.s.sw_now);
	if (powered_trial)
		dev_info(&pump->dev, "protocol replies: VBUS low=%u high=%u normal=%u last-delta=%d mV, voltage=%u last-current-tx=%04x last-voltage-tx=%04x (local writes)\n",
			 vooc->vbus_replies[VB_LOW], vooc->vbus_replies[VB_HIGH],
			 vooc->vbus_replies[VB_NORMAL], vooc->last_window_delta,
			 vooc->voltage_replies, vooc->last_current_tx, vooc->last_voltage_tx);
	if (powered_trial && oem_policy)
		dev_info(&pump->dev,
			 "PHY timing: CPU latency request=%d us, max reply=%u us, max current reply=%u us\n",
			 cpu_latency_us, vooc->max_reply_us, vooc->max_current_reply_us);
	if (vooc->run.pumps == 2)
		dev_info(&pump->dev, "secondary pump: enabled=%u status-confirmed=%u frames=%d peak IBUS=%d mA peak total IBUS=%d mA\n",
			 vooc->run.s.enabled, vooc->run.s.checked, vooc->run.s.nrun,
			 vooc->run.s.ibus_max, vooc->run.s.tot_max);
restore:
	off = opd_stop_pumps(vooc);
	cpu_latency_qos_remove_request(&qos);
	if (!off) {
		vooc->fault_latched = true;
		dev_crit(&pump->dev, "SuperVOOC pump-off unconfirmed; retaining buck hold and watchdog\n");
	}
	for (i = 1; off && vooc->secondary_touched && i < ARRAY_SIZE(vl3b_exit_rows); i++) {
		const struct vl_row *row = &vl3b_exit_rows[i];

		cleanup = opd_secondary_write(vooc, row->reg, row->w, row->val);
		if (cleanup)
			dev_err(&pump->dev, "restore secondary register %02x: %d\n", row->reg, cleanup);
	}
	for (i = 0; off && main_touched && i < ARRAY_SIZE(vl_exit_rows); i++) {
		cleanup = opd_write(vooc, &vl_exit_rows[i], VW_EXIT);
		if (cleanup)
			dev_err(&pump->dev, "restore register %02x: %d\n", vl_exit_rows[i].reg, cleanup);
	}
	cleanup = wcd939x_usbss_release_handshake(vooc->mux, &pump->dev);
	if (cleanup)
		dev_err(&pump->dev, "restore USB D+/D-: %d\n", cleanup);
	if (battery) {
		if (off) {
			for (i = 0; i < 3; i++) {
				cleanup = qcom_battmgr_oplus_vooc_hold(battery, false);
				if (!cleanup)
					break;
			}
			if (cleanup) {
				vooc->fault_latched = true;
				dev_err(&pump->dev, "restore buck charging: %d\n", cleanup);
			}
		}
		power_supply_put(battery);
	}
	if (secondary_bus)
		pm_runtime_put(secondary_bus);
put_bus:
	pm_runtime_put(bus);
relax:
	__pm_relax(vooc->wake);
	if (ret == -ETIME)
		dev_info(&pump->dev, "SuperVOOC trial completed its %u ms window\n", trial_ms);
	else if (ret)
		dev_info(&pump->dev, "SuperVOOC session ended: %d\n", ret);
}

static void opd_work(struct work_struct *work)
{
	struct opd_vooc *vooc = container_of(to_delayed_work(work), struct opd_vooc, work);
	struct power_supply *usb = power_supply_get_by_name("qcom-battmgr-usb");
	int online, type, uv;

	if (!usb)
		return;
	if (vooc->fault_latched || READ_ONCE(vooc->removing))
		goto out;
	if (opd_read_supply(usb, POWER_SUPPLY_PROP_ONLINE, &online))
		goto out;
	/* Ignore reclassification caused by the adapter dropping out of VOOC. */
	if (opd_read_supply(usb, POWER_SUPPLY_PROP_USB_TYPE, &type))
		goto out;
	if ((!online || type == POWER_SUPPLY_USB_TYPE_UNKNOWN) && vooc->attempted) {
		if (!vooc->rearm_at)
			vooc->rearm_at = ktime_add_ms(ktime_get(), OPD_REARM_MS);
		if (ktime_before(ktime_get(), vooc->rearm_at)) {
			queue_delayed_work(system_long_wq, &vooc->work,
					   msecs_to_jiffies(OPD_REARM_MS));
			goto out;
		}
		vooc->attempted = false;
	}
	vooc->rearm_at = 0;
	if (!online || type != POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID) {
		if (online && type != POWER_SUPPLY_USB_TYPE_UNKNOWN)
			vooc->attempted = false;
		goto out;
	}
	if (vooc->attempted ||
	    opd_read_supply(usb, POWER_SUPPLY_PROP_VOLTAGE_NOW, &uv) ||
	    uv < 4750000 || uv > 5500000)
		goto out;
	opd_identify(vooc, usb);
out:
	power_supply_put(usb);
}

static int opd_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct opd_vooc *vooc = container_of(nb, struct opd_vooc, notify);
	struct power_supply *psy = data;

	if (event == PSY_EVENT_PROP_CHANGED &&
	    !strcmp(psy->desc->name, "qcom-battmgr-usb"))
		queue_delayed_work(system_long_wq, &vooc->work, msecs_to_jiffies(450));
	return NOTIFY_DONE;
}

static int opd_probe_secondary(struct opd_vooc *vooc, struct i2c_client *pump)
{
	struct device_node *node;
	struct i2c_adapter *adapter;
	int status, ctrl, id;

	/* Use the OPD2513 DT address, independent of Linux's adapter numbering. */
	node = of_find_node_by_path("/soc@0/geniqup@8c0000/i2c@890000");
	if (!node)
		return -ENODEV;
	adapter = of_get_i2c_adapter_by_node(node);
	of_node_put(node);
	if (!adapter)
		return -EPROBE_DEFER;
	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA |
					      I2C_FUNC_SMBUS_READ_I2C_BLOCK)) {
		i2c_put_adapter(adapter);
		return -EOPNOTSUPP;
	}
	vooc->secondary = devm_i2c_new_dummy_device(&pump->dev, adapter, 0x6a);
	i2c_put_adapter(adapter);
	if (IS_ERR(vooc->secondary))
		return PTR_ERR(vooc->secondary);
	id = i2c_smbus_read_byte_data(vooc->secondary, VOOC_CHIP_ID);
	status = i2c_smbus_read_byte_data(vooc->secondary, VOOC_STAT);
	ctrl = i2c_smbus_read_byte_data(vooc->secondary, VOOC_CTRL);
	if (id != 0x49 || status < 0 || ctrl < 0 || !vl_pump_idle(status, ctrl))
		return -ENODEV;
	dev_info(&pump->dev, "secondary pump %s: id=%02x status=%02x control=%02x (idle)\n",
		 dev_name(&vooc->secondary->dev), id, status, ctrl);
	return 0;
}

static int opd_probe(struct i2c_client *pump)
{
	struct device_node *node;
	struct i2c_client *mux;
	struct opd_vooc *vooc;
	int temperatures[ARRAY_SIZE(opd_sensor_names)];
	unsigned int i;
	int ret;

	if (!of_machine_is_compatible("oneplus,opd2513") || !pump->irq)
		return -ENODEV;
	if (!i2c_check_functionality(pump->adapter,
			I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA) ||
	    (powered_trial && !i2c_check_functionality(pump->adapter,
						 I2C_FUNC_SMBUS_READ_I2C_BLOCK)))
		return -EOPNOTSUPP;
	ret = i2c_smbus_read_byte_data(pump, VOOC_CHIP_ID);
	if (ret != 0x49)
		return ret < 0 ? ret : -ENODEV;

	vooc = devm_kzalloc(&pump->dev, sizeof(*vooc), GFP_KERNEL);
	if (!vooc)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(opd_sensor_names); i++) {
		vooc->sensors[i] = devm_iio_channel_get(&pump->dev, opd_sensor_names[i]);
		if (IS_ERR(vooc->sensors[i]))
			return dev_err_probe(&pump->dev, PTR_ERR(vooc->sensors[i]),
					     "%s temperature sensor unavailable\n",
					     opd_sensor_names[i]);
	}
	dev_info(&pump->dev, "battery voltage/temperature/SOC/authentication gate: %s\n",
		 opd_battery_check(&pump->dev, false, -1, NULL, false) ? "closed" : "ready");
	ret = opd_read_temperatures(vooc, temperatures);
	if (!ret)
		dev_info(&pump->dev, "charging sensors: USB %d, battery connector %d, MOS %d mC\n",
			 temperatures[0], temperatures[1], temperatures[2]);
	node = of_find_compatible_node(NULL, NULL, "qcom,wcd9395-usbss");
	if (!node)
		return -ENODEV;
	mux = of_find_i2c_device_by_node(node);
	of_node_put(node);
	if (!mux)
		return -EPROBE_DEFER;
	vooc->mux = &mux->dev;
	/* Resolve deferred providers before creating a child on the other bus. */
	if (dual_trial) {
		ret = opd_probe_secondary(vooc, pump);
		if (ret) {
			dev_err_probe(&pump->dev, ret, "secondary pump unavailable\n");
			goto put_mux;
		}
	}
	vooc->pump = pump;
	vooc->wake = wakeup_source_register(&pump->dev, KBUILD_MODNAME);
	if (!vooc->wake) {
		ret = -ENOMEM;
		goto put_mux;
	}
	init_completion(&vooc->end);
	INIT_DELAYED_WORK(&vooc->work, opd_work);
	vooc->notify.notifier_call = opd_event;
	i2c_set_clientdata(pump, vooc);
	ret = power_supply_reg_notifier(&vooc->notify);
	if (ret) {
		wakeup_source_unregister(vooc->wake);
		goto put_mux;
	}
	queue_delayed_work(system_long_wq, &vooc->work, msecs_to_jiffies(450));
	dev_info(&pump->dev, "OPD2513 SuperVOOC identification armed\n");
	return 0;
put_mux:
	put_device(vooc->mux);
	return ret;
}

static void opd_remove(struct i2c_client *pump)
{
	struct opd_vooc *vooc = i2c_get_clientdata(pump);

	WRITE_ONCE(vooc->removing, true);
	power_supply_unreg_notifier(&vooc->notify);
	cancel_delayed_work_sync(&vooc->work);
	wakeup_source_unregister(vooc->wake);
	put_device(vooc->mux);
}

/* Read-only protocol snapshot. replied counts local writes, not peer acceptance.
 * last_*_tx records successful TX register writes, not a newly computed value.
 */
static ssize_t session_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct opd_vooc *vooc = dev_get_drvdata(dev);

	return sysfs_emit(buf,
		"target=%u ask=%u replied=%u peak=%u first=%u units=100mA current_replies=%u change_notices=%u current_tx_retries=%u slave_checked=%u slave_switching=%u primary_mA=%d secondary_mA=%d vbus_mV=%d vbat_mV=%d vout_mV=%d vac_mV=%d battery_code=%d stopping=%u vbus_low_replies=%u vbus_high_replies=%u vbus_normal_replies=%u window_delta_mV=%d voltage_replies=%u last_current_tx=%u last_voltage_tx=%u\n",
		READ_ONCE(vooc->oem_target), READ_ONCE(vooc->run.ask4),
		READ_ONCE(vooc->run.sent), READ_ONCE(vooc->peak_ask),
		READ_ONCE(vooc->first_current),
		READ_ONCE(vooc->current_replies), READ_ONCE(vooc->change_notices),
		READ_ONCE(vooc->current_tx_retries),
		READ_ONCE(vooc->run.s.checked), READ_ONCE(vooc->run.s.sw_now),
		READ_ONCE(vooc->run.adc.ibus), READ_ONCE(vooc->run.s.adc.ibus),
		READ_ONCE(vooc->run.adc.vbus), READ_ONCE(vooc->run.adc.vbat),
		READ_ONCE(vooc->run.adc.vout), READ_ONCE(vooc->run.adc.vac),
		vl_batt_code2(&vooc->run.adc, READ_ONCE(vooc->run.adc.ibus) +
			(READ_ONCE(vooc->run.s.sw_now) ? READ_ONCE(vooc->run.s.adc.ibus) : 0)),
		READ_ONCE(vooc->stopping),
		READ_ONCE(vooc->vbus_replies[VB_LOW]),
		READ_ONCE(vooc->vbus_replies[VB_HIGH]),
		READ_ONCE(vooc->vbus_replies[VB_NORMAL]),
		READ_ONCE(vooc->last_window_delta),
		READ_ONCE(vooc->voltage_replies),
		READ_ONCE(vooc->last_current_tx), READ_ONCE(vooc->last_voltage_tx));
}
static DEVICE_ATTR_RO(session_state);
static struct attribute *opd_attrs[] = { &dev_attr_session_state.attr, NULL };
ATTRIBUTE_GROUPS(opd);

static const struct of_device_id opd_match[] = {
	{ .compatible = "southchip,sc8547d" },
	{ }
};
MODULE_DEVICE_TABLE(of, opd_match);

static struct i2c_driver opd_driver = {
	.driver = { .name = KBUILD_MODNAME, .of_match_table = opd_match, .dev_groups = opd_groups },
	.probe = opd_probe,
	.remove = opd_remove,
};
module_i2c_driver(opd_driver);
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_CONSUMER");
MODULE_DESCRIPTION("OPD2513 SC8547D SuperVOOC adapter identification");
