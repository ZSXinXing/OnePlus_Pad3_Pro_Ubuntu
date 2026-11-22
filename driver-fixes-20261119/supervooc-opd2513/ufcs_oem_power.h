/* SPDX-License-Identifier: GPL-2.0-only */
/* SC8547D 2:1 charge-pump phase of the authenticated UFCS session. */
#define OPD_UFCS_TRIAL_VBUS_MV 9800
#define OPD_UFCS_TRIAL_SECONDS 30
#define OPD_UFCS_START_CHECK_MS 500
#define OPD_UFCS_START_POLL_MS 50
#define OPD_UFCS_START_STEP_MV 100
#define OPD_UFCS_SINGLE_PUMP_MAX_MA 2200

static int opd_ufcs_request(struct ufcs_dev *ufcs, int mv, int ma)
{
	struct opd_ufcs_phy *phy = ufcs->drv_data;
	int ret;

	WRITE_ONCE(phy->request_ma, ma);
	WRITE_ONCE(phy->request_mv, mv);
	ret = ufcs_intf_pdo_set(ufcs, mv, ma);
	WRITE_ONCE(phy->request_mv, 0);
	return ret;
}

static int opd_ufcs_pump_sample(struct i2c_client *pump, bool secondary,
		bool active, bool starting, struct vl_adc *adc, u8 *state)
{
	u8 regs[0x11], raw[14];
	int ret, thaw;
	bool state_ok;

	ret = opd_read_block(pump, 0, sizeof(regs), regs);
	if (ret)
		return ret;
	ret = secondary ? vl3b_image(regs) : vl3_image(regs, 0, 0);
	if (ret || vl3_flags(regs)) {
		dev_warn(&pump->dev, "UFCS pump readback: image_error=%d status=%02x control=%02x flags=%02x/%02x/%02x/%02x\n",
			 ret, regs[0x06], regs[0x07], regs[0x09], regs[0x0d], regs[0x0e], regs[0x0f]);
		return -EIO;
	}
	if (state) {
		state[0] = regs[VOOC_STAT];
		state[1] = regs[VOOC_CTRL];
	}
	state_ok = active ? (regs[VOOC_CTRL] & BIT(7)) && (regs[VOOC_STAT] & BIT(2)) :
		vl_pump_idle(regs[VOOC_STAT], regs[VOOC_CTRL]);
	if (!starting && !state_ok) {
		dev_warn(&pump->dev, "UFCS pump state: expected_active=%d status=%02x control=%02x\n",
			 active, regs[VOOC_STAT], regs[VOOC_CTRL]);
		return -EAGAIN;
	}
	ret = i2c_smbus_write_byte_data(pump, 0x11, 0xa0);
	if (ret)
		return ret;
	ret = opd_read_block(pump, 0x13, sizeof(raw), raw);
	thaw = i2c_smbus_write_byte_data(pump, 0x11, 0x80);
	if (ret || thaw)
		return ret ?: thaw;
	vl_adc_decode(adc, raw);
	if (adc->ibus > V3_LIM_IBUS_MAX_MA || adc->tdie >= V3_LIM_TDIE_DC ||
	    (!active && adc->ibus >= V3B_IDLE_MA) ||
	    (!secondary && (adc->vbus > V3_LIM_VBUS_MV || adc->vbat >= V3_LIM_VBAT_MV)))
		return -ERANGE;
	return state_ok ? 0 : -EAGAIN;
}

static int opd_ufcs_check_live(struct opd_vooc *vooc, struct power_supply *usb,
		struct power_supply *battery, int *soc_out)
{
	int temps[ARRAY_SIZE(opd_sensor_names)], online, soc, temp, voltage, health;

	if (READ_ONCE(vooc->removing) || opd_read_supply(usb, POWER_SUPPLY_PROP_ONLINE, &online) || !online)
		return -ENOLINK;
	if (opd_read_temperatures(vooc, temps) || temps[OPD_USB_TEMP] >= 44000 ||
	    temps[OPD_BATT_CONNECTOR_TEMP] >= 44000)
		return -ERANGE;
	if (opd_read_supply(battery, POWER_SUPPLY_PROP_CAPACITY, &soc) ||
	    opd_read_supply(battery, POWER_SUPPLY_PROP_TEMP, &temp) ||
	    opd_read_supply(battery, POWER_SUPPLY_PROP_VOLTAGE_NOW, &voltage) ||
	    opd_read_supply(battery, POWER_SUPPLY_PROP_HEALTH, &health))
		return -EIO;
	/* Bound the trial to the first point of the .402 normal-low SOC curves. */
	if (soc < 0 || soc > 75 || temp < 210 || temp >= 350 ||
	    voltage < 3450000 || voltage >= V3_LIM_VBAT_MV * 1000 || health != POWER_SUPPLY_HEALTH_GOOD)
		return -ERANGE;
	if (soc_out)
		*soc_out = soc;
	return 0;
}

static int opd_ufcs_startup_request(struct ufcs_dev *ufcs, struct i2c_client *pump, int mv)
{
	int ret = opd_ufcs_request(ufcs, mv, OPD_UFCS_START_MA);

	dev_info(&pump->dev, "UFCS OEM startup request: %d mV %d mA, status=%d\n",
		 mv, OPD_UFCS_START_MA, ret);
	return ret;
}

/* ColorOS checks switching at 500 ms. Keep checking protections on both pumps meanwhile. */
static int opd_ufcs_check_start(struct opd_vooc *vooc, struct power_supply *usb,
		struct power_supply *battery, bool secondary, int ma, ktime_t deadline)
{
	struct i2c_client *pump = secondary ? vooc->secondary : vooc->pump;
	struct i2c_client *peer = secondary ? vooc->pump : vooc->secondary;
	ktime_t started = ktime_get();
	struct vl_adc adc, peer_adc;
	s64 elapsed_ms;
	u8 state[2] = {};
	int ret, status;

	for (;;) {
		ret = opd_ufcs_check_live(vooc, usb, battery, NULL);
		if (ret < 0)
			return ret;
		if (!ktime_before(ktime_get(), deadline))
			return -ETIME;
		status = opd_ufcs_pump_sample(pump, secondary, true, true, &adc, state);
		if (status && status != -EAGAIN)
			return status;
		ret = opd_ufcs_pump_sample(peer, !secondary, secondary, false, &peer_adc, NULL);
		if (ret)
			return ret;
		if (adc.ibus + peer_adc.ibus > min(ma + 650, 6800))
			return -ERANGE;
		elapsed_ms = ktime_ms_delta(ktime_get(), started);
		if (elapsed_ms >= OPD_UFCS_START_CHECK_MS)
			break;
		msleep(OPD_UFCS_START_POLL_MS);
	}
	dev_info(&pump->dev, "UFCS pump start: secondary=%d wait=%lld ms status=%02x control=%02x vbus=%d vout=%d vbat=%d mV ibus=%d mA result=%d\n",
		 secondary, elapsed_ms, state[0], state[1], adc.vbus, adc.vout, adc.vbat,
		 adc.ibus, status);
	return status;
}

static int opd_ufcs_run_pumps(struct opd_vooc *vooc, struct ufcs_dev *ufcs,
		struct power_supply *usb, int mv, int ceiling_ma, struct power_supply **held)
{
	const struct vl3_io secondary_io = { .c = vooc, .wr = opd_secondary_write };
	struct i2c_client *pump = vooc->pump, *secondary = vooc->secondary;
	struct power_supply *battery;
	struct device *secondary_bus;
	struct vl_adc main_adc, sub_adc;
	bool main_touched = false, sub_touched = false, main_on = false, sub_on = false;
	bool stopped = true, source_sampled = false, sub_wanted = false;
	unsigned int mismatch_count = 0, secondary_starts = 0;
	int ma = OPD_UFCS_START_MA, i, ret, limit, plug_soc, target_mv, total;
	int peak_uw = 0, vdiff_failures = 0, samples = 0, dual_samples = 0;
	u64 dual_input_uw_sum = 0, source_info;
	const char *exit_stage = "setup";
	ktime_t deadline, next_pump_check;

	if (!secondary || ceiling_ma < 2000)
		return -EOPNOTSUPP;
	secondary_bus = secondary->adapter->dev.parent;
	ret = pm_runtime_resume_and_get(secondary_bus);
	if (ret < 0)
		return ret;
	battery = power_supply_get_by_name("qcom-battmgr-bat");
	if (!battery) {
		ret = -ENODEV;
		goto put_bus;
	}
	ret = opd_ufcs_check_live(vooc, usb, battery, &plug_soc);
	if (ret < 0)
		goto put_battery;
	if (!opd2513_high_current_soc(plug_soc)) {
		ret = -ERANGE;
		goto put_battery;
	}
	if (!opd_pump_idle(pump) || !opd_pump_idle(secondary)) {
		ret = -EBUSY;
		goto put_battery;
	}
	/* ColorOS initializes the curve using the SOC recorded at admission. */
	limit = OPD_UFCS_STOCK_MAX_MA;
	dev_info(&pump->dev, "UFCS session curve: plug SOC %d%%, limit=%d mA\n", plug_soc, limit);
	main_touched = true;
	for (i = 0; i < ARRAY_SIZE(vl3_hw_rows); i++) {
		ret = opd_write(vooc, &vl3_hw_rows[i], VW_HW);
		if (ret)
			goto stop;
	}
	sub_touched = true;
	if (vl3b_init(&secondary_io) || vl3_rows(&secondary_io, vl3b_hw_rows, VL3B_NHW)) {
		ret = -EIO;
		goto stop;
	}
	/* Stock configures the pump and ADC before suspending the buck and sending the first PDO. */
	ret = qcom_battmgr_oplus_vooc_hold(battery, true);
	if (ret)
		goto stop;
	*held = battery;
	ret = opd_ufcs_startup_request(ufcs, pump, mv);
	if (ret)
		goto stop;
	msleep(30);
	deadline = ktime_add_ms(ktime_get(), OPD_UFCS_TRIAL_SECONDS * 1000);
	next_pump_check = ktime_get();
	while (ktime_before(ktime_get(), deadline)) {
		exit_stage = "battery-policy";
		ret = opd_ufcs_check_live(vooc, usb, battery, NULL);
		if (ret)
			goto stop;
		exit_stage = "pump-sample";
		ret = opd_ufcs_pump_sample(pump, false, main_on, false, &main_adc, NULL);
		if (ret)
			goto stop;
		ret = opd_ufcs_pump_sample(secondary, true, sub_on, false, &sub_adc, NULL);
		if (ret)
			goto stop;
		exit_stage = "pump-current";
		total = main_adc.ibus + sub_adc.ibus;
		if (total > min(ma + 650, 6800) ||
		    (sub_on && abs(main_adc.ibus - sub_adc.ibus) > V3B_DEV_MA)) {
			ret = -ERANGE;
			goto stop;
		}
		exit_stage = "pump-voltage-delta";
		if (main_on && main_adc.vbus - 2 * main_adc.vbat > vl3_vdiff_max(main_adc.ibus)) {
			if (++vdiff_failures >= 3) {
				ret = -ERANGE;
				goto stop;
			}
		} else {
			vdiff_failures = 0;
		}
		if (main_on)
			peak_uw = max(peak_uw, main_adc.vbus * total);
		if (sub_on) {
			dual_samples++;
			dual_input_uw_sum += main_adc.vbus * total;
		}
		if (!(samples++ % 4))
			dev_info(&pump->dev, "UFCS load sample: vbus=%d vout=%d vbat=%d mV, ibus=%d/%d mA, request=%d mV/%d mA\n",
				 main_adc.vbus, main_adc.vout, main_adc.vbat,
				 main_adc.ibus, sub_adc.ibus, mv, ma);
		if (main_on && !ktime_before(ktime_get(), next_pump_check)) {
			next_pump_check = ktime_add_ms(ktime_get(), OPD_UFCS_SECONDARY_MONITOR_MS);
			sub_wanted = opd2513_ufcs_secondary_step(total, ma, sub_on, &mismatch_count);
		}
		if (sub_on && !source_sampled && mv == OPD_UFCS_TRIAL_VBUS_MV &&
		    ma == min(limit, ceiling_ma)) {
			int adapter_mv, input_mv, input_mohm;

			source_sampled = true;
			exit_stage = "source-info";
			ret = ufcs_intf_get_source_info(ufcs, &source_info);
			if (ret)
				goto stop;
			adapter_mv = UFCS_SOURCE_INFO_VOL(source_info);
			/* ColorOS uses VAC for this measurement, with VIN as fallback. */
			input_mv = main_adc.vac > 0 ? main_adc.vac : main_adc.vbus;
			input_mohm = total > 0 ? max(adapter_mv - input_mv, 0) * 1000 / total : -1;
			dev_info(&pump->dev, "UFCS source: adapter=%d mV/%llu mA, pump=%d mV/%d mA, PDO=%d mV/%d mA, VAC=%d mV, input impedance=%d mOhm\n",
				 adapter_mv, UFCS_SOURCE_INFO_CURR(source_info), main_adc.vbus,
				 total, mv, ma, main_adc.vac, input_mohm);
			continue;
		}
		if (!main_on) {
			exit_stage = "primary-start";
			/* The stock SC8547D CP_GET_VOUT callback reads VBAT ADC (0x1b). */
			if (main_adc.vbat < 3450 || main_adc.vbat >= V3_LIM_VBAT_MV) {
				ret = -ERANGE;
				goto stop;
			}
			target_mv = (main_adc.vbat * 2 / 100) * 100 + 450;
			if (main_adc.vbus >= target_mv && main_adc.vbus <= target_mv + 200) {
				main_on = true;
				ret = i2c_smbus_write_byte_data(pump, VOOC_CTRL, 0x80);
				if (ret)
					goto stop;
				ret = opd_ufcs_check_start(vooc, usb, battery, false, ma, deadline);
				if (ret)
					goto stop;
				continue;
			}
			/* Stock takes a minimum 100 mV step outside the start window. */
			if (main_adc.vbus < target_mv)
				mv += clamp(target_mv - main_adc.vbus, OPD_UFCS_START_STEP_MV, 1000);
			else
				mv -= clamp(main_adc.vbus - target_mv, OPD_UFCS_START_STEP_MV, 500);
			mv = DIV_ROUND_CLOSEST(mv, 20) * 20;
		} else if (!sub_on && sub_wanted) {
			exit_stage = "secondary-start";
			sub_on = true;
			secondary_starts++;
			ret = i2c_smbus_write_byte_data(secondary, VOOC_CTRL, 0x81);
			if (ret)
				goto stop;
			ret = opd_ufcs_check_start(vooc, usb, battery, true, ma, deadline);
			if (ret)
				goto stop;
			continue;
		} else if (sub_on && !sub_wanted) {
			/* Keep both pumps available until the adapter accepts a single-pump contract. */
			exit_stage = "secondary-stop";
			if (ma > OPD_UFCS_SINGLE_PUMP_MAX_MA) {
				ma = OPD_UFCS_SINGLE_PUMP_MAX_MA;
			} else {
				ret = i2c_smbus_write_byte_data(secondary, VOOC_CTRL, 0x01);
				if (ret)
					goto stop;
				msleep(OPD_UFCS_START_POLL_MS);
				ret = opd_ufcs_pump_sample(secondary, true, false, false,
							   &sub_adc, NULL);
				if (ret)
					goto stop;
				sub_on = false;
				continue;
			}
		} else {
			ma = min(ma + 200, min(min(limit, ceiling_ma),
					       sub_on ? 6100 : OPD_UFCS_SINGLE_PUMP_MAX_MA));
			mv = min(OPD_UFCS_TRIAL_VBUS_MV, mv + 100);
		}
		exit_stage = "pdo";
		if (mv < OPD_UFCS_START_MIN_MV || mv > OPD_UFCS_TRIAL_VBUS_MV) {
			ret = -ERANGE;
			goto stop;
		}
		ret = opd_ufcs_request(ufcs, mv, ma);
		if (ret)
			goto stop;
		msleep(80);
	}
	exit_stage = "trial-timeout";
	ret = -ETIME;
stop:
	for (i = 0; i < 3; i++) {
		if (sub_touched)
			opd_secondary_write(vooc, VOOC_CTRL, 1, 0x01);
		if (main_touched)
			opd_write(vooc, &vl_exit_rows[0], VW_EXIT);
		stopped = opd_pump_idle(pump) && opd_pump_idle(secondary);
		if (stopped)
			break;
		msleep(20);
	}
	if (!stopped) {
		msleep(1100);
		stopped = opd_pump_idle(pump) && opd_pump_idle(secondary);
	}
	if (stopped) {
		if (sub_touched)
			for (i = 1; i < VL3B_NEXIT; i++)
				if (opd_secondary_write(vooc, vl3b_exit_rows[i].reg,
							vl3b_exit_rows[i].w, vl3b_exit_rows[i].val))
					stopped = false;
		/* Only restore UFCS-independent rows here. The primary PHY still owns D+/D-. */
		if (main_touched && (i2c_smbus_write_byte_data(pump, 0x11, 0) ||
				     i2c_smbus_write_byte_data(pump, 0x09, 0x10)))
			stopped = false;
	}
	dev_info(&pump->dev, "UFCS load: attempts=%d/%u dual_samples=%d mean_dual_input=%llu uW peak_input=%d uW status=%d stopped=%d stage=%s\n",
		 main_on, secondary_starts, dual_samples,
		 dual_samples ? div_u64(dual_input_uw_sum, dual_samples) : 0,
		 peak_uw, ret, stopped, exit_stage);
	if (!stopped)
		ret = -EUCLEAN;
	if (*held)
		goto put_bus;
put_battery:
	power_supply_put(battery);
put_bus:
	pm_runtime_put(secondary_bus);
	return ret;
}
