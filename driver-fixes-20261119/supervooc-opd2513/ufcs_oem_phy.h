/* SPDX-License-Identifier: GPL-2.0-only */
/* Included after the SC8547D register definitions in opd2513_vooc.c. */
#include "ufcs_class.h"

int opd2513_ufcs_verify(struct ufcs_dev *ufcs);

#define OPD_UFCS_STOCK_MAX_MA 6100
#define UFCS_SEND_SOURCE_HARD_RESET BIT(0)
#define OPD_UFCS_START_MIN_MV 5000
#define OPD_UFCS_START_MAX_MV 5500
#define OPD_UFCS_START_MA 800

struct opd_ufcs_phy {
	struct i2c_client *pump;
	int request_mv;
	int request_ma;
};

static int opd_phy_noop(struct ufcs_dev *ufcs)
{
	return 0;
}

static int opd_phy_disable(struct ufcs_dev *ufcs)
{
	struct opd_ufcs_phy *phy = ufcs->drv_data;

	return i2c_smbus_write_byte_data(phy->pump, UFCS_CTRL1, 0);
}

static int opd_phy_unsupported(struct ufcs_dev *ufcs)
{
	return -EOPNOTSUPP;
}

static int opd_phy_update(struct ufcs_dev *ufcs, u8 mask, u8 val)
{
	struct opd_ufcs_phy *phy = ufcs->drv_data;
	struct i2c_client *pump = phy->pump;
	int old = i2c_smbus_read_byte_data(pump, UFCS_CTRL1);

	if (old < 0)
		return old;
	return i2c_smbus_write_byte_data(pump, UFCS_CTRL1, (old & ~mask) | val);
}

static int opd_phy_source_hard_reset(struct ufcs_dev *ufcs)
{
	int retry, ret = -EIO;

	for (retry = 0; retry < 3; retry++) {
		ret = opd_phy_update(ufcs, UFCS_SEND_SOURCE_HARD_RESET,
				     UFCS_SEND_SOURCE_HARD_RESET);
		if (!ret) {
			msleep(100);
			break;
		}
	}
	return ret;
}

static int opd_phy_handshake(struct ufcs_dev *ufcs)
{
	return opd_phy_update(ufcs, UFCS_HANDSHAKE, UFCS_HANDSHAKE);
}

static int opd_phy_baud(struct ufcs_dev *ufcs, enum ufcs_baud_rate rate)
{
	if (rate >= UFCS_BAUD_COUNT)
		return -EINVAL;
	return opd_phy_update(ufcs, GENMASK(4, 3), rate << UFCS_BAUD_SHIFT);
}

static int opd_phy_read(struct ufcs_dev *ufcs, unsigned char *buf, int len)
{
	struct opd_ufcs_phy *phy = ufcs->drv_data;

	return opd_ufcs_receive(phy->pump, buf, len);
}

static int opd_phy_write(struct ufcs_dev *ufcs, unsigned char *buf, int len)
{
	struct opd_ufcs_phy *phy = ufcs->drv_data;
	struct i2c_client *pump = phy->pump;
	u8 frame[65] = { UFCS_TX_DATA };
	int ret;

	if (len < 3 || len > sizeof(frame) - 1)
		return -EMSGSIZE;
	/* Permit exactly the contract armed by the charging worker on the wire. */
	if ((buf[1] & 7) == UFCS_DATA_MSG && buf[2] == DATA_MSG_REQUEST) {
		int request_mv = READ_ONCE(phy->request_mv);
		int mv, ma;

		if (len != 12 || buf[3] != 8 || request_mv == 0)
			return -EPERM;
		mv = ((buf[8] << 8) | buf[9]) * 10;
		ma = ((buf[10] << 8) | buf[11]) * 10;
		if (mv != request_mv || ma != READ_ONCE(phy->request_ma))
			return -EPERM;
	}
	ret = i2c_smbus_write_byte_data(pump, UFCS_TX_LEN, len);
	if (ret)
		return ret;
	memcpy(frame + 1, buf, len);
	ret = i2c_master_send(pump, frame, len + 1);
	if (ret != len + 1)
		return ret < 0 ? ret : -EIO;
	return opd_phy_update(ufcs, UFCS_SEND, UFCS_SEND);
}

static struct ufcs_dev_ops opd_phy_ops = {
	.init = opd_phy_noop,
	.enable = opd_phy_noop,
	.disable = opd_phy_disable,
	.handshake = opd_phy_handshake,
	.set_baud_rate = opd_phy_baud,
	.read_msg = opd_phy_read,
	.write_msg = opd_phy_write,
	.source_hard_reset = opd_phy_source_hard_reset,
	.cable_hard_reset = opd_phy_unsupported,
};

static irqreturn_t opd_phy_irq(int irq, void *arg)
{
	static const u8 errors[] = {
		UFCS_RECV_ERR_ACK_TIMEOUT, UFCS_RECV_ERR_TRANS_FAIL,
		UFCS_RECV_ERR_BUFF_BUSY, UFCS_COMM_ERR_RX_OVERFLOW,
		UFCS_RECV_ERR_DATA_READY, UFCS_RECV_ERR_SENT_CMP,
	};
	static const u8 communication[] = {
		UFCS_HW_ERR_HARD_RESET, UFCS_COMM_ERR_CRC_ERR,
		UFCS_COMM_ERR_STOP_ERR, UFCS_COMM_ERR_START_FAIL,
		UFCS_COMM_ERR_RX_LEN_ERR, UFCS_COMM_ERR_BYTE_TIMEOUT,
		UFCS_COMM_ERR_TRAINING_ERR, UFCS_COMM_ERR_BAUD_RATE_ERR,
	};
	struct ufcs_dev *ufcs = arg;
	struct opd_ufcs_phy *phy = ufcs->drv_data;
	unsigned int status = 0, i;
	u8 flags[3];

	if (opd_read_block(phy->pump, UFCS_FLAGS, sizeof(flags), flags))
		return IRQ_HANDLED;
	for (i = 0; i < ARRAY_SIZE(errors); i++)
		if (flags[0] & BIT(i))
			status |= BIT(errors[i]);
	for (i = 0; i < ARRAY_SIZE(communication); i++)
		if (flags[1] & BIT(i))
			status |= BIT(communication[i]);
	if (flags[2] & BIT(6))
		status |= BIT(UFCS_COMM_ERR_BAUD_RATE_CHANGE);
	if (flags[2] & BIT(7))
		status |= BIT(UFCS_COMM_ERR_BUS_CONFLICT);
	if (READ_ONCE(ufcs->handshake_state) == UFCS_HS_WAIT) {
		if (flags[0] & UFCS_FLAG_HS_FAIL)
			WRITE_ONCE(ufcs->handshake_state, UFCS_HS_FAIL);
		else if (flags[0] & UFCS_HS_OK)
			WRITE_ONCE(ufcs->handshake_state, UFCS_HS_SUCCESS);
	}
	if (flags[0] || status) {
		WRITE_ONCE(ufcs->err_flag_save, status);
		ufcs_set_error_flag(ufcs, status);
		ufcs_msg_handler(ufcs);
	}
	return IRQ_HANDLED;
}

#include "ufcs_oem_power.h"

/* Caller owns the mux and primary pump with both charge pumps inactive. */
static int opd_ufcs_authenticated_probe(struct opd_vooc *vooc, struct power_supply *usb)
{
	static struct ufcs_config config = { .ic_vendor_id = 0x0006 };
	static const u8 regs[] = { UFCS_DPDM, UFCS_CTRL2, UFCS_MASK1, UFCS_MASK2, 0xcc };
	static const u8 vals[] = { 0x01, 0x30, 0x01, 0x40, 0x80 };
	struct i2c_client *pump = vooc->pump;
	struct opd_ufcs_phy phy = { .pump = pump };
	struct ufcs_dev *ufcs;
	u64 info = 0, emark = 0, power[UFCS_OPLUS_VND_POWER_INFO_MAX];
	u64 modes[UFCS_OUTPUT_MODE_MAX];
	u8 saved[ARRAY_SIZE(regs)], flags[3];
	bool clean = true;
	struct power_supply *battery = NULL;
	int i, ret, auth = -ENODATA, cable = -ENODATA;
	int load_ret = -ENODATA;
	int advertised = 0, cable_ma = 0, adapter_ma = 0, ceiling_ma = 0;
	int power_count = -ENODATA, modes_count = -ENODATA, exit_ret = -ENODATA;

	ret = i2c_smbus_read_byte_data(pump, UFCS_CTRL1);
	if (ret != 0)
		return ret < 0 ? ret : -EBUSY;
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = i2c_smbus_read_byte_data(pump, regs[i]);
		if (ret < 0)
			return ret;
		saved[i] = ret;
	}
	ufcs = ufcs_device_register(&pump->dev, &opd_phy_ops, &phy, &config);
	if (IS_ERR(ufcs))
		return PTR_ERR(ufcs);
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = i2c_smbus_write_byte_data(pump, regs[i], vals[i]);
		if (ret)
			goto restore;
	}
	ret = opd_read_block(pump, UFCS_FLAGS, sizeof(flags), flags);
	if (ret)
		goto restore;
	ret = request_threaded_irq(pump->irq, NULL, opd_phy_irq,
				   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				   "opd2513-ufcs", ufcs);
	if (ret)
		goto restore;
	ret = i2c_smbus_write_byte_data(pump, UFCS_CTRL1, UFCS_ENABLE);
	if (!ret)
		ret = ufcs_handshake(ufcs);
	if (!ret)
		ret = ufcs_intf_get_device_info(ufcs, &info);
	if (!ret && (info >> 48) == UFCS_OPLUS_DEV_ID && !ufcs_is_test_mode(ufcs)) {
		ret = ufcs_intf_config_watchdog(ufcs, 3000);
		if (!ret)
			auth = opd2513_ufcs_verify(ufcs);
		if (auth == 1) {
			cable = ufcs_intf_get_emark_info(ufcs, &emark);
			power_count = ufcs_intf_get_power_info_ext(ufcs, power, ARRAY_SIZE(power));
			modes_count = ufcs_intf_get_pdo_info(ufcs, modes, ARRAY_SIZE(modes));
			/* The captured OEM type-AA cable reports its own current limit. */
			if (!cable && UFCS_OPLUS_VND_EMARK_INFO(emark) == UFCS_OPLUS_EMARK_INFO_OPLUS_CABLE &&
			    UFCS_OPLUS_VND_EMARK_HW_VER(emark) == 0xaa &&
			    UFCS_OPLUS_VND_EMARK_VOL_MAX(emark) >= OPD_UFCS_TARGET_MV)
				cable_ma = UFCS_OPLUS_VND_EMARK_CURR_MAX(emark);
			for (i = 0; i < power_count; i++) {
				if (UFCS_OPLUS_VND_POWER_INFO_TYPE(power[i]) == UFCS_OPLUS_POWER_INFO_UFCS &&
				    UFCS_OPLUS_VND_POWER_INFO_VOL_MAX(power[i]) >= OPD_UFCS_TARGET_MV)
					adapter_ma = max_t(int, adapter_ma, UFCS_OPLUS_VND_POWER_INFO_CURR_MAX(power[i]));
			}
			for (i = 0; i < modes_count; i++) {
				dev_info(&pump->dev, "UFCS PDO %llu: %llu..%llu mV, %llu..%llu mA\n",
					 UFCS_OUTPUT_MODE_INDEX(modes[i]), UFCS_OUTPUT_MODE_VOL_MIN(modes[i]),
					 UFCS_OUTPUT_MODE_VOL_MAX(modes[i]), UFCS_OUTPUT_MODE_CURR_MIN(modes[i]),
					 UFCS_OUTPUT_MODE_CURR_MAX(modes[i]));
				if (UFCS_OUTPUT_MODE_VOL_MIN(modes[i]) <= OPD_UFCS_TARGET_MV &&
				    UFCS_OUTPUT_MODE_VOL_MAX(modes[i]) >= OPD_UFCS_TARGET_MV)
					advertised = max_t(int, advertised, UFCS_OUTPUT_MODE_CURR_MAX(modes[i]));
			}
			ceiling_ma = min(min(cable_ma, adapter_ma),
					 min(advertised, OPD_UFCS_STOCK_MAX_MA));
			dev_info(&pump->dev, "UFCS 11 V ceiling: cable=%d mA adapter=%d mA PDO=%d mA device=%d mA combined=%d mA\n",
				 cable_ma, adapter_ma, advertised, OPD_UFCS_STOCK_MAX_MA,
				 ceiling_ma);
			if ((oem_ufcs_startup_trial || oem_ufcs_powered_trial) &&
			    ceiling_ma >= OPD_UFCS_START_MA &&
			    modes_count > 0 && !READ_ONCE(vooc->removing)) {
				int mv = max_t(int, OPD_UFCS_START_MIN_MV,
					       UFCS_OUTPUT_MODE_VOL_MIN(modes[0]));

				if (mv <= OPD_UFCS_START_MAX_MV) {
					if (oem_ufcs_powered_trial && powered_trial)
						load_ret = opd_ufcs_run_pumps(vooc, ufcs, usb, mv, ceiling_ma, &battery);
					else
						opd_ufcs_startup_request(ufcs, pump, mv);
				}
			}
		}
	}
	if (ufcs_handshake_success(ufcs))
		exit_ret = ufcs_force_exit(ufcs);
	else if (battery)
		exit_ret = opd_phy_source_hard_reset(ufcs);
	/* Stop new PHY events before tearing down the protocol workers. */
	opd_phy_disable(ufcs);
	free_irq(pump->irq, ufcs);
restore:
	ufcs_device_unregister(ufcs);
	if (i2c_smbus_write_byte_data(pump, UFCS_CTRL1, 0) ||
	    i2c_smbus_read_byte_data(pump, UFCS_CTRL1) != 0)
		clean = false;
	for (i = ARRAY_SIZE(regs); i-- > 0;)
		if (i2c_smbus_write_byte_data(pump, regs[i], saved[i]) ||
		    i2c_smbus_read_byte_data(pump, regs[i]) != saved[i])
			clean = false;
	if (load_ret == -EUCLEAN)
		clean = false;
	if (battery) {
		int online, uv, rc = -EIO;

		/* Keep buck suspended until the source has returned to its default. */
		for (i = 0; i < 15; i++) {
			if (!opd_read_supply(usb, POWER_SUPPLY_PROP_ONLINE, &online) &&
			    (!online || (!opd_read_supply(usb, POWER_SUPPLY_PROP_VOLTAGE_NOW, &uv) &&
					 uv >= 4750000 && uv <= 5500000))) {
				rc = 0;
				break;
			}
			msleep(50);
		}
		if (!clean || rc ||
		    qcom_battmgr_oplus_vooc_hold(battery, false)) {
			dev_crit(&pump->dev, "UFCS cleanup unconfirmed, retaining buck hold\n");
			clean = false;
		}
		power_supply_put(battery);
	}
	if (oem_ufcs_powered_trial)
		dev_info(&pump->dev, "UFCS powered trial returned %d\n", load_ret);
	dev_info(&pump->dev,
		 "UFCS authenticated probe: status=%d vendor=%04llx auth=%d cable=%d emark=%016llx power_modes=%d pdos=%d advertised_11V_mA=%d restored=%d exit=%d\n",
		 ret, info >> 48, auth, cable, emark, power_count, modes_count,
		 advertised, clean, exit_ret);
	return clean ? (exit_ret == -ENODATA ? 0 : exit_ret) : -EIO;
}
