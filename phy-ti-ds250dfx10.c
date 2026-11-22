// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the TI DS250DF410 / DS250DF810 / DS250DF230 Retimers
 *
 * Copyright (C) 2022-2023 Josua Mayer <josua@solid-run.com>
 */

#include <linux/i2c.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/version.h>

#define DS250DF410_REG_CHAN_CONFIG_ID 0xEF
#define DS250DF410_MASK_CHAN_CONFIG_ID GENMASK(3, 0)
#define DS250DF410_REG_VERSION 0xF0
#define DS250DF410_REG_DEVICE_ID 0xF1
#define DS250DF410_REG_CHAN_VERSION 0xF3
#define DS250DF410_MASK_CHAN_VERSION GENMASK(7, 4)
#define DS250DF410_MASK_SHARE_VERSION GENMASK(3, 0)

#define DS250DF230_DEVICE_ID 0x15

/* default CAL_CLK frequency if not given in DT (all SolidRun boards use 25 MHz) */
#define DS250DFX10_CAL_CLK_DEFAULT_HZ 25000000

struct ds250dfx10_phy_priv {
	struct i2c_client *client;
	uint8_t channel;
	bool crosspoint;
	bool is_df230;
	u32 cal_clk_hz;
};

struct ds250dfx10_priv {
	struct phy *phy[8];
	struct phy_provider *provider;
};

static int ds250dfx10_read_register(struct i2c_client *client, uint8_t address, uint8_t *value,
									uint8_t mask)
{
	s32 res;

	res = i2c_smbus_read_byte_data(client, address);
	if (res < 0) {
		dev_err(&client->dev, "failed to read register %#04x: %d\n", address,
				res);
		return -EIO;
	}

	*value = res & mask;
	return 0;
}

static int ds250dfx10_write_register(struct i2c_client *client, uint8_t address, uint8_t value,
									 uint8_t mask)
{
	int ret;
	uint8_t tmp;
	s32 res;

	// combine with current value according to mask
	if (mask != 0xFF) {
		ret = ds250dfx10_read_register(client, address, &tmp, ~mask);
		if (ret)
			return ret;

		value = (value & mask) | tmp;
	}

	// write new value
	res = i2c_smbus_write_byte_data(client, address, value);
	if (res < 0) {
		dev_err(&client->dev, "failed to write register %#04x=%#04x: %d\n",
				address, value, res);
		return -EIO;
	}

	return 0;
}

static void ds250dfx10_config_1g(struct i2c_client *client, uint8_t channel, bool crosspoint)
{
	int ret = 0;

	// enable smbus access to single channel
	ret |= ds250dfx10_write_register(client, 0xFF, 0x01, 0x03);

	// select channel
	ret |= ds250dfx10_write_register(client, 0xFC, 1 << channel, 0xFF);

	// reset channel registers
	ret |= ds250dfx10_write_register(client, 0x00, 0x04, 0x04);

	// assert cdr
	ret |= ds250dfx10_write_register(client, 0x0A, 0x0C, 0x0C);

	// configure 2x2 crosspoint switch if enabled
	if (crosspoint)
		ret |= ds250dfx10_write_register(client, 0x96, 0x06, 0x0F);

	// set manual data rate override to 1.25Gbps
	ret |= ds250dfx10_write_register(client, 0x60, 0x00, 0xFF);
	ret |= ds250dfx10_write_register(client, 0x61, 0xb2, 0xFF);
	ret |= ds250dfx10_write_register(client, 0x62, 0x00, 0xFF);
	ret |= ds250dfx10_write_register(client, 0x63, 0xb2, 0xFF);

	// set maximum ppm delta tolerance
	ret |= ds250dfx10_write_register(client, 0x64, 0xFF, 0xFF);

	// enable manual divider override
	ret |= ds250dfx10_write_register(client, 0x09, 0x04, 0x04);

	// set divider to 16
	ret |= ds250dfx10_write_register(client, 0x18, 0x40, 0x70);

	// enable pre- and post-fir
	ret |= ds250dfx10_write_register(client, 0x3D, 0x80, 0x80);

	// set main cursor magnitude +15
	ret |= ds250dfx10_write_register(client, 0x3D, 0x00, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3D, 0x0F, 0x1F);

	// set pre cursor magnitude -4
	ret |= ds250dfx10_write_register(client, 0x3E, 0x40, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3E, 0x04, 0x0F);

	// set post cursor magnitude -4
	ret |= ds250dfx10_write_register(client, 0x3F, 0x40, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3F, 0x04, 0x0F);

	// deassert cdr
	ret |= ds250dfx10_write_register(client, 0x0A, 0x00, 0x0C);

	if (!ret)
		dev_info(&client->dev, "configured channel %u for 1G\n", channel);
}

static void ds250dfx10_config_10g(struct i2c_client *client, uint8_t channel, bool crosspoint)
{
	int ret = 0;

	// enable smbus access to single channel
	ret |= ds250dfx10_write_register(client, 0xFF, 0x01, 0x03);

	// select channel
	ret |= ds250dfx10_write_register(client, 0xFC, 1 << channel, 0xFF);

	// reset channel registers
	ret |= ds250dfx10_write_register(client, 0x00, 0x04, 0x04);

	// assert cdr
	ret |= ds250dfx10_write_register(client, 0x0A, 0x0C, 0x0C);

	// configure 2x2 crosspoint switch if enabled
	if (crosspoint)
		ret |= ds250dfx10_write_register(client, 0x96, 0x06, 0x0F);

	// disable manual data rate override
	ret |= ds250dfx10_write_register(client, 0x60, 0x00, 0xFF);
	ret |= ds250dfx10_write_register(client, 0x61, 0x00, 0xFF);
	ret |= ds250dfx10_write_register(client, 0x62, 0x00, 0xFF);
	ret |= ds250dfx10_write_register(client, 0x63, 0x00, 0xFF);

	// set minimum ppm delta tolerance (reset-default)
	ret |= ds250dfx10_write_register(client, 0x64, 0x00, 0xFF);

	// disable manual divider override
	ret |= ds250dfx10_write_register(client, 0x09, 0x00, 0x04);

	// select 10.3125 rate
	ret |= ds250dfx10_write_register(client, 0x2F, 0x00, 0xF0);

	// enable pre- and post-fir
	ret |= ds250dfx10_write_register(client, 0x3D, 0x80, 0x80);

	// set main cursor magnitude +15
	ret |= ds250dfx10_write_register(client, 0x3D, 0x00, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3D, 0x0F, 0x1F);

	// set pre cursor magnitude -4
	ret |= ds250dfx10_write_register(client, 0x3E, 0x40, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3E, 0x04, 0x0F);

	// set post cursor magnitude -4
	ret |= ds250dfx10_write_register(client, 0x3F, 0x40, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3F, 0x04, 0x0F);

	// deassert cdr
	ret |= ds250dfx10_write_register(client, 0x0A, 0x00, 0x0C);

	if (!ret)
		dev_info(&client->dev, "configured channel %u for 10G\n", channel);
}

static void ds250dfx10_config_25g(struct i2c_client *client, uint8_t channel, bool crosspoint)
{
	int ret = 0;

	// enable smbus access to single channel
	ret |= ds250dfx10_write_register(client, 0xFF, 0x01, 0x03);

	// select channel
	ret |= ds250dfx10_write_register(client, 0xFC, 1 << channel, 0xFF);

	// reset channel registers
	ret |= ds250dfx10_write_register(client, 0x00, 0x04, 0x04);

	// assert cdr
	ret |= ds250dfx10_write_register(client, 0x0A, 0x0C, 0x0C);

	// configure 2x2 crosspoint switch if enabled
	if (crosspoint)
		ret |= ds250dfx10_write_register(client, 0x96, 0x06, 0x0F);

	// disable manual data rate override
	ret |= ds250dfx10_write_register(client, 0x60, 0x00, 0xFF);
	ret |= ds250dfx10_write_register(client, 0x61, 0x00, 0xFF);
	ret |= ds250dfx10_write_register(client, 0x62, 0x00, 0xFF);
	ret |= ds250dfx10_write_register(client, 0x63, 0x00, 0xFF);

	// set minimum ppm delta tolerance (reset-default)
	ret |= ds250dfx10_write_register(client, 0x64, 0x00, 0xFF);

	// disable manual divider override
	ret |= ds250dfx10_write_register(client, 0x09, 0x00, 0x04);

	// select 25.78125 rate
	ret |= ds250dfx10_write_register(client, 0x2F, 0x50, 0xF0);

	// enable pre- and post-fir
	ret |= ds250dfx10_write_register(client, 0x3D, 0x80, 0x80);

	// set main cursor magnitude +15
	ret |= ds250dfx10_write_register(client, 0x3D, 0x00, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3D, 0x0F, 0x1F);

	// set pre cursor magnitude -4
	ret |= ds250dfx10_write_register(client, 0x3E, 0x40, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3E, 0x04, 0x0F);

	// set post cursor magnitude -4
	ret |= ds250dfx10_write_register(client, 0x3F, 0x40, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3F, 0x04, 0x0F);

	// deassert cdr
	ret |= ds250dfx10_write_register(client, 0x0A, 0x00, 0x0C);

	if (!ret)
		dev_info(&client->dev, "configured channel %u for 25G\n", channel);
}

/*
 * DS250DF230 configuration.
 *
 * The rate table selected through Reg_0x2F[6:4] on the DS250DF230 is only
 * valid with a 30.72 MHz CAL_CLK (DS250DF230 datasheet, Table 8-6), while the
 * DS250DF410 tables assume 25 MHz. To be independent of the CAL_CLK frequency
 * the DS250DF230 is always configured with a manual data rate override:
 *
 *   VCO   = bitrate * divider            (19.6 - 25.8 GHz)
 *   count = VCO / CAL_CLK * 16           (Reg_0x60/0x61 group 0, 0x62/0x63 group 1)
 *
 * e.g. 25 MHz CAL_CLK: 1.25G/div16 -> 12800, 10.3125G/div2 -> 13200,
 *      25.78125G/div1 -> 16500.
 * Bit 7 of Reg_0x61/0x63 enables the override, Reg_0x09[2] together with
 * Reg_0x18[6:4] forces the divider.
 */
static int ds250df230_config(struct ds250dfx10_phy_priv *priv, u64 bitrate,
			     u8 div_sel, const char *name)
{
	struct i2c_client *client = priv->client;
	u64 vco = bitrate << div_sel;
	u32 count;
	int ret = 0;

	count = (u32)div_u64(vco * 16 + priv->cal_clk_hz / 2, priv->cal_clk_hz);
	if (count > 0x7FFF) {
		dev_err(&client->dev, "ppm count %u out of range\n", count);
		return -EINVAL;
	}

	// enable smbus access to single channel
	ret |= ds250dfx10_write_register(client, 0xFF, 0x01, 0x03);

	// select channel
	ret |= ds250dfx10_write_register(client, 0xFC, 1 << priv->channel, 0xFF);

	// reset channel registers
	ret |= ds250dfx10_write_register(client, 0x00, 0x04, 0x04);

	// assert cdr
	ret |= ds250dfx10_write_register(client, 0x0A, 0x0C, 0x0C);

	// configure 2x2 crosspoint switch if enabled
	if (priv->crosspoint)
		ret |= ds250dfx10_write_register(client, 0x96, 0x06, 0x0F);

	// manual data rate override (both groups)
	ret |= ds250dfx10_write_register(client, 0x60, count & 0xFF, 0xFF);
	ret |= ds250dfx10_write_register(client, 0x61, 0x80 | (count >> 8), 0xFF);
	ret |= ds250dfx10_write_register(client, 0x62, count & 0xFF, 0xFF);
	ret |= ds250dfx10_write_register(client, 0x63, 0x80 | (count >> 8), 0xFF);

	// set maximum ppm delta tolerance
	ret |= ds250dfx10_write_register(client, 0x64, 0xFF, 0xFF);

	// manual divider override
	ret |= ds250dfx10_write_register(client, 0x09, 0x04, 0x04);
	ret |= ds250dfx10_write_register(client, 0x18, div_sel << 4, 0x70);

	/*
	 * Everything else (PPM check as lock qualifier, adaptive CTLE, output
	 * muted while unlocked) stays at the reset defaults.
	 */

	// enable pre- and post-fir
	ret |= ds250dfx10_write_register(client, 0x3D, 0x80, 0x80);

	// set main cursor magnitude +15
	ret |= ds250dfx10_write_register(client, 0x3D, 0x00, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3D, 0x0F, 0x1F);

	// set pre cursor magnitude -4
	ret |= ds250dfx10_write_register(client, 0x3E, 0x40, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3E, 0x04, 0x0F);

	// set post cursor magnitude -4
	ret |= ds250dfx10_write_register(client, 0x3F, 0x40, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3F, 0x04, 0x0F);

	// deassert cdr
	ret |= ds250dfx10_write_register(client, 0x0A, 0x00, 0x0C);

	if (ret)
		return -EIO;

	dev_info(&client->dev, "configured channel %u for %s (ppm count %u)\n",
		 priv->channel, name, count);
	return 0;
}

static int ds250df230_set_mode(struct ds250dfx10_phy_priv *priv, int submode)
{
	switch (submode) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
		/*
		 * FIXME: untested. The DS250DF230 datasheet (Table 8-5) lists
		 * rates below 4.9 Gbps as "CDR bypassed" only, so 1G may need
		 * raw mode (see Programmer's Guide SNLU182) instead of the
		 * divide-by-16 CDR setting inherited from the DS250DF410 code.
		 */
		return ds250df230_config(priv, 1250000000ULL, 4, "1G");
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_USXGMII:
	case PHY_INTERFACE_MODE_XGMII:
		return ds250df230_config(priv, 10312500000ULL, 1, "10G");
	case PHY_INTERFACE_MODE_25GBASER:
		return ds250df230_config(priv, 25781250000ULL, 0, "25G");
	default:
		dev_err(&priv->client->dev, "unsupported interface submode %i\n",
			submode);
		return -EOPNOTSUPP;
	}
}

static int ds250dfx10_phy_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct ds250dfx10_phy_priv *priv = phy_get_drvdata(phy);

	if (mode != PHY_MODE_ETHERNET)
		return -EOPNOTSUPP;

	if (priv->is_df230)
		return ds250df230_set_mode(priv, submode);

	switch (submode) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
		ds250dfx10_config_1g(priv->client, priv->channel, priv->crosspoint);
		break;
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_USXGMII:
	case PHY_INTERFACE_MODE_XGMII:
		ds250dfx10_config_10g(priv->client, priv->channel, priv->crosspoint);
		break;
	case PHY_INTERFACE_MODE_25GBASER:
		ds250dfx10_config_25g(priv->client, priv->channel, priv->crosspoint);
		break;
	default:
		dev_err(&priv->client->dev, "unsupported interface submode %i\n",
				  submode);
		return -EOPNOTSUPP;
	}

	return 0;
}

static const struct phy_ops ds250dfx10_phy_ops = {
	.set_mode	= ds250dfx10_phy_set_mode,
	.owner		= THIS_MODULE,
};

/* no operations: the PHY core treats missing ops as successful no-ops */
static const struct phy_ops ds250dfx10_passthrough_phy_ops = {
	.owner		= THIS_MODULE,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
static struct phy *ds250dfx10_of_xlate(struct device *dev, const struct of_phandle_args *args)
#else
static struct phy *ds250dfx10_of_xlate(struct device *dev, struct of_phandle_args *args)
#endif
{
	struct ds250dfx10_priv *phy_priv = dev_get_drvdata(dev);
	int channel;

	if (args->args_count != 1) {
		dev_err(phy_priv->provider->dev, "DT did not pass correct no of args\n");
		return ERR_PTR(-ENODEV);
	}

	channel = args->args[0];
	if (WARN_ON(channel >= ARRAY_SIZE(phy_priv->phy))
		|| !phy_priv->phy[channel])
		return ERR_PTR(-ENODEV);

	return phy_priv->phy[channel];
}

/*
 * "ti,optional": the retimer is only fitted on some variants of a board. If it
 * does not answer on I2C, register pass-through PHYs (no operations) instead of
 * failing, so that consumers listing its channels in "phys" still probe and the
 * lane simply runs without a retimer.
 */
static int ds250dfx10_probe_absent(struct i2c_client *client, struct ds250dfx10_priv *priv)
{
	struct device_node *np = client->dev.of_node;
	int channels, i;

	if (of_device_is_compatible(np, "ti,ds250df810"))
		channels = 8;
	else if (of_device_is_compatible(np, "ti,ds250df410"))
		channels = 4;
	else
		channels = 2;

	for (i = 0; i < channels; i++) {
		priv->phy[i] = devm_phy_create(&client->dev, NULL,
					       &ds250dfx10_passthrough_phy_ops);
		if (IS_ERR(priv->phy[i])) {
			int ret = PTR_ERR(priv->phy[i]);

			priv->phy[i] = NULL;
			return ret;
		}
	}

	priv->provider = devm_of_phy_provider_register(&client->dev, ds250dfx10_of_xlate);
	if (IS_ERR(priv->provider))
		return PTR_ERR(priv->provider);

	dev_info(&client->dev, "not present (optional), %d pass-through channels\n", channels);
	return 0;
}

static int ds250dfx10_probe(struct i2c_client *client)
{
	struct ds250dfx10_priv *priv;
	struct ds250dfx10_phy_priv *phy_priv;
	struct device_node *child = NULL;
	uint8_t chan_config_id, device_id, version, chan_version, share_version, channels;
	uint8_t reg;
	bool crosspoint;
	u32 cal_clk_hz = DS250DFX10_CAL_CLK_DEFAULT_HZ;
	int ret, i;

	crosspoint = of_property_read_bool(client->dev.of_node, "ti,crosspoint-enable");
	of_property_read_u32(client->dev.of_node, "ti,cal-clock-frequency", &cal_clk_hz);
	if (!cal_clk_hz)
		cal_clk_hz = DS250DFX10_CAL_CLK_DEFAULT_HZ;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto no_phys;
	}
	i2c_set_clientdata(client, priv);

	if (of_property_read_bool(client->dev.of_node, "ti,optional") &&
	    i2c_smbus_read_byte_data(client, DS250DF410_REG_DEVICE_ID) < 0)
		return ds250dfx10_probe_absent(client, priv);

	/* read device identification */
	ret = ds250dfx10_read_register(client, DS250DF410_REG_DEVICE_ID, &reg, 0xFF);
	if (ret)
		goto no_phys;
	device_id = reg;

	/* read device version */
	ret = ds250dfx10_read_register(client, DS250DF410_REG_VERSION, &reg, 0xFF);
	if (ret)
		goto no_phys;
	version = reg;

    // report device id
	dev_info(&client->dev, "device id %#04x version %#04x\n", device_id, version);

	switch (device_id) {
	case 0x10:
	case DS250DF230_DEVICE_ID:
		break;
	default:
		dev_warn(&client->dev, "unknown device id, expect problems!\n");
	}

	// read channel config id
	ret = ds250dfx10_read_register(client, DS250DF410_REG_CHAN_CONFIG_ID, &reg,
								   DS250DF410_MASK_CHAN_CONFIG_ID);
	if (ret)
		goto no_phys;
	chan_config_id = reg;

	switch (chan_config_id) {
	case 0xC:
		channels = 8;
		break;
	case 0xE:
		channels = (device_id == DS250DF230_DEVICE_ID) ? 2 : 4;
		break;
	case 0x5:
		channels = 2;
		break;
	default:
		dev_err(&client->dev, "unknown channel configuration id %#03x\n", chan_config_id);
		ret = -EINVAL;
		goto no_phys;
	}
	dev_info(&client->dev, "%u channels\n", channels);

	// read channel version
	ret = ds250dfx10_read_register(client, DS250DF410_REG_CHAN_VERSION, &reg, 0xFF);
	if (ret)
		goto no_phys;
	chan_version = (reg & DS250DF410_MASK_CHAN_VERSION) >> 4;
	share_version = reg & DS250DF410_MASK_SHARE_VERSION;

	dev_info(&client->dev, "channel version %#03x share version %#03x\n",
			 chan_version, share_version);

	// create PHY objects for all channels
	for (i = 0; i < channels; i++) {
		priv->phy[i] = devm_phy_create(&client->dev, child, &ds250dfx10_phy_ops);
		if (IS_ERR(priv->phy[i])) {
			ret = PTR_ERR(priv->phy[i]);
			priv->phy[i] = NULL;
			of_node_put(child);
			goto no_provider;
		}

		phy_priv = devm_kzalloc(&client->dev, sizeof(*phy_priv), GFP_KERNEL);
		if (!phy_priv) {
			ret = -ENOMEM;
			goto no_provider;
		}
		phy_set_drvdata(priv->phy[i], phy_priv);

		phy_priv->client = client;
		phy_priv->channel = i;
		phy_priv->crosspoint = crosspoint;
		phy_priv->is_df230 = (device_id == DS250DF230_DEVICE_ID);
		phy_priv->cal_clk_hz = cal_clk_hz;

		/*
		 * Bring the channel into a defined 10G state, phylink will
		 * reconfigure it through set_mode() once the interface is up.
		 */
		if (phy_priv->is_df230)
			ds250df230_set_mode(phy_priv, PHY_INTERFACE_MODE_10GBASER);
		else if (crosspoint)
			ds250dfx10_config_10g(client, i, crosspoint);

		dev_info(&client->dev, "created phy for channel %u\n", i);
	}
	of_node_put(child);

	// register self as phy provider with generic lookup function
	priv->provider = devm_of_phy_provider_register(&client->dev, ds250dfx10_of_xlate);

	return 0;

	devm_of_phy_provider_unregister(&client->dev, priv->provider);
no_provider:
	for (i = 0; i < 8; i++) {
		if (priv->phy[i])
			devm_phy_destroy(&client->dev, priv->phy[i]);
	}
no_phys:
	return ret;
}

static void ds250dfx10_remove(struct i2c_client *client)
{
	struct ds250dfx10_priv *priv = i2c_get_clientdata(client);
	int i;

	for (i = 0; i < 8; i++)
		if (priv->phy[i])
			devm_phy_destroy(&client->dev, priv->phy[i]);
}

#ifdef CONFIG_OF
static const struct of_device_id ds250dfx10_dt_ids[] = {
	{ .compatible = "ti,ds250df230", },
	{ .compatible = "ti,ds250df410", },
	{ .compatible = "ti,ds250df810", },
	{ }
};
MODULE_DEVICE_TABLE(of, ds250dfx10_dt_ids);
#endif

static struct i2c_device_id ds250dfx10_idtable[] = {
	{ "ds250df410", 0 },
	{ "ds250df810", 1 },
	{ "ds250df230", 2 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, ds250dfx10_idtable);

static struct i2c_driver ds250dfx10_driver = {
	.driver = {
		.name   = "ds250dfx10",
		.of_match_table = of_match_ptr(ds250dfx10_dt_ids),
	},

	.id_table       = ds250dfx10_idtable,
	.probe      = ds250dfx10_probe,
	.remove         = ds250dfx10_remove,
};

module_i2c_driver(ds250dfx10_driver);

MODULE_AUTHOR("Josua Mayer <josua@solid-run.com>");
MODULE_DESCRIPTION("TI DS250DFX10 Retimer Driver");
MODULE_LICENSE("GPL");
