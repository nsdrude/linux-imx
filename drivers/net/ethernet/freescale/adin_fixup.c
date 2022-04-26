// SPDX-License-Identifier: GPL-2.0+
/**
 *  Simple fixup / driver for Analog Devices Industrial Ethernet PHYs
 *
 * Copyright 2019 Analog Devices Inc.
 * Copyright 2022 Variscite LTD
 */
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/phy.h>
#include <linux/property.h>
#include <linux/of.h>
#include <linux/version.h>

#define PHY_ID_ADIN1200				0x0283bc20
#define PHY_ID_ADIN1300				0x0283bc30

#define ADIN1300_MII_EXT_REG_PTR		0x0010
#define ADIN1300_MII_EXT_REG_DATA		0x0011

#define ADIN1300_GE_CLK_CFG			0xff1f
#define   ADIN1300_GE_CLK_RCVR_125_EN		BIT(5)

#define ADIN1300_GE_RGMII_CFG_REG		0xff23
#define   ADIN1300_GE_RGMII_RXID_EN		BIT(2)
#define   ADIN1300_GE_RGMII_TXID_EN		BIT(1)
#define   ADIN1300_GE_RGMII_EN			BIT(0)

/**
 * adin_phy_interface_is_rgmii - Convenience function for testing if a PHY interface
 * is RGMII (all variants)
 *
 * Backported from phy_interface_is_rgmii in 5.4-2.1.x-imx_var01 kernel
 *
 * @phydev: the phy_device struct
 */
static inline bool adin_phy_interface_is_rgmii(struct phy_device *phydev)
{
        return phydev->interface >= PHY_INTERFACE_MODE_RGMII &&
                phydev->interface <= PHY_INTERFACE_MODE_RGMII_TXID;
};

static u16 adin_ext_read(struct phy_device *phydev, const u32 regnum)
{
	u16 val;

	phy_write(phydev, ADIN1300_MII_EXT_REG_PTR, regnum);
	val = phy_read(phydev, ADIN1300_MII_EXT_REG_DATA);

	return val;
}

static int adin_ext_write(struct phy_device *phydev, const u32 regnum, const u16 val)
{
	phy_write(phydev, ADIN1300_MII_EXT_REG_PTR, regnum);

	return phy_write(phydev, ADIN1300_MII_EXT_REG_DATA, val);
}

/**
 * adin_get_phy_mode_override - Get phy-mode override for adin PHY
 *
 * The function gets phy-mode string from property 'adi,phy-mode-override'
 * and return its index in phy_modes table, or errno in error case.
 */
static int adin_get_phy_mode_override(struct phy_device *phydev)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
	struct device *dev = &phydev->mdio.dev;
#else
	struct device *dev = &phydev->dev;
#endif
	struct device_node *of_node = dev->of_node;
	const char *phy_mode_override;
	const char *prop_phy_mode_override = "adi,phy-mode-override";
	int err, i;

	err = of_property_read_string(of_node, prop_phy_mode_override,
				      &phy_mode_override);
	if (err < 0)
		return err;

	for (i = 0; i < PHY_INTERFACE_MODE_MAX; i++)
		if (!strcasecmp(phy_mode_override, phy_modes(i)))
			return i;

	printk("%s: Error %s = '%s' is not valid\n", __func__,
	       prop_phy_mode_override, phy_mode_override);

	return -ENODEV;
}

static int adin_config_rgmii_mode(struct phy_device *phydev)
{
	int reg;
	int phy_mode_override = adin_get_phy_mode_override(phydev);

	if (phy_mode_override >= 0) {
		phydev->interface = (phy_interface_t) phy_mode_override;
	}

	reg = adin_ext_read(phydev, ADIN1300_GE_RGMII_CFG_REG);
	if (reg < 0)
		return reg;

	if (!adin_phy_interface_is_rgmii(phydev)) {
		reg &= ~ADIN1300_GE_RGMII_EN;
		return adin_ext_write(phydev, ADIN1300_GE_RGMII_CFG_REG, reg);
	}

	reg |= ADIN1300_GE_RGMII_EN;

	if (phydev->interface == PHY_INTERFACE_MODE_RGMII_ID ||
	    phydev->interface == PHY_INTERFACE_MODE_RGMII_RXID) {
		reg |= ADIN1300_GE_RGMII_RXID_EN;
	} else {
		reg &= ~ADIN1300_GE_RGMII_RXID_EN;
	}

	if (phydev->interface == PHY_INTERFACE_MODE_RGMII_ID ||
	    phydev->interface == PHY_INTERFACE_MODE_RGMII_TXID) {
		reg |= ADIN1300_GE_RGMII_TXID_EN;
	} else {
		reg &= ~ADIN1300_GE_RGMII_TXID_EN;
	}

	return adin_ext_write(phydev, ADIN1300_GE_RGMII_CFG_REG, reg);
}

static int adin_set_clock_config(struct phy_device *phydev)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
	struct device *dev = &phydev->mdio.dev;
#else
	struct device *dev = &phydev->dev;
#endif
	struct device_node *of_node = dev->of_node;
	int reg = 0;

	if (of_property_read_bool(of_node, "adi,clk_rcvr_125_en")) {
		printk("%s: Enabling 125 MHz clock out\n", __func__);

		reg = adin_ext_read(phydev, ADIN1300_GE_CLK_CFG);
		reg |= ADIN1300_GE_CLK_RCVR_125_EN;
		reg = adin_ext_write(phydev, ADIN1300_GE_CLK_CFG, reg);
	}

	return reg;
}

static int adin1300_phy_fixup(struct phy_device *phydev)
{
	int rc;

	rc = adin_config_rgmii_mode(phydev);
	if (rc < 0)
		return rc;

	rc = adin_set_clock_config(phydev);
	if (rc < 0)
		return rc;


	printk("%s: PHY is using mode '%s'\n",
		__func__, phy_modes(phydev->interface));

	return 0;
}

void adin_register_fixup(struct net_device *ndev) {
	int err;

	if (!IS_BUILTIN(CONFIG_PHYLIB))
		return;

	err = phy_register_fixup_for_uid(PHY_ID_ADIN1300, 0xffffffff,
					 adin1300_phy_fixup);
	if (err)
		printk("%s: Error: Cannot register PHY board fixup\n", __func__);
}
