// SPDX-License-Identifier: GPL-2.0
/*
 * Provisioned Wi-Fi / Bluetooth addresses from the Google bootloader
 *
 * The Pixel bootloader (ABL) publishes the factory-provisioned addresses as
 * strings under /chosen/cdt/cdb2 ("wlan_mac1", "bt_addr", as
 * "AA:BB:CC:DD:EE:FF" or "AABBCCDDEEFF"). Downstream handed them to the
 * vendor WLAN platform driver; on mainline, copy them into the standard
 * properties of the wifi0 / bluetooth0 alias nodes so ath10k and hci_qca
 * pick them up instead of a random MAC and the QCA default BD address.
 *
 * Copyright 2019 Google Inc.
 */
#include <linux/etherdevice.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>

#define CDB_PATH "/chosen/cdt/cdb2"

static bool __init cdt_parse_addr(struct device_node *cdb, const char *name,
				  u8 addr[ETH_ALEN])
{
	const char *str;
	int len;

	str = of_get_property(cdb, name, &len);
	if (!str || len < 12 || !memchr(str, '\0', len))
		return false;

	if (sscanf(str, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
		   &addr[0], &addr[1], &addr[2],
		   &addr[3], &addr[4], &addr[5]) == ETH_ALEN)
		return true;

	return sscanf(str, "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx",
		      &addr[0], &addr[1], &addr[2],
		      &addr[3], &addr[4], &addr[5]) == ETH_ALEN;
}

static void __init cdt_add_prop(const char *alias, const char *prop_name,
				const u8 val[ETH_ALEN])
{
	struct device_node *np;
	struct property *prop;

	np = of_find_node_by_path(alias);
	if (!np)
		return;

	if (of_property_present(np, prop_name))
		goto out;

	prop = kzalloc(sizeof(*prop) + ETH_ALEN, GFP_KERNEL);
	if (!prop)
		goto out;

	prop->name = kstrdup(prop_name, GFP_KERNEL);
	if (!prop->name) {
		kfree(prop);
		goto out;
	}
	prop->value = prop + 1;
	prop->length = ETH_ALEN;
	memcpy(prop->value, val, ETH_ALEN);

	if (of_add_property(np, prop)) {
		kfree(prop->name);
		kfree(prop);
		goto out;
	}

	pr_info("%pOF: %s from the bootloader\n", np, prop_name);
out:
	of_node_put(np);
}

static int __init google_cdt_macs_init(void)
{
	struct device_node *cdb;
	u8 addr[ETH_ALEN], bd[ETH_ALEN];
	int i;

	cdb = of_find_node_by_path(CDB_PATH);
	if (!cdb)
		return 0;

	/* Only accept a globally administered unicast Wi-Fi MAC */
	if (cdt_parse_addr(cdb, "wlan_mac1", addr) &&
	    is_valid_ether_addr(addr) && !is_local_ether_addr(addr))
		cdt_add_prop("wifi0", "local-mac-address", addr);

	/* local-bd-address is little-endian, the string is big-endian */
	if (cdt_parse_addr(cdb, "bt_addr", addr) && !is_zero_ether_addr(addr)) {
		for (i = 0; i < ETH_ALEN; i++)
			bd[i] = addr[ETH_ALEN - 1 - i];
		cdt_add_prop("bluetooth0", "local-bd-address", bd);
	}

	of_node_put(cdb);

	return 0;
}
/* Before the WLAN/BT drivers probe; they are modules or later initcalls */
core_initcall(google_cdt_macs_init);
