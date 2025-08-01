/*
 * B53 common definitions
 *
 * Copyright (C) 2011-2013 Jonas Gorski <jogo@openwrt.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef __B53_PRIV_H
#define __B53_PRIV_H

#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/phylink.h>
#include <linux/etherdevice.h>
#include <net/dsa.h>

#include "b53_regs.h"

struct b53_device;
struct net_device;

struct b53_io_ops {
	int (*read8)(struct b53_device *dev, u8 page, u8 reg, u8 *value);
	int (*read16)(struct b53_device *dev, u8 page, u8 reg, u16 *value);
	int (*read32)(struct b53_device *dev, u8 page, u8 reg, u32 *value);
	int (*read48)(struct b53_device *dev, u8 page, u8 reg, u64 *value);
	int (*read64)(struct b53_device *dev, u8 page, u8 reg, u64 *value);
	int (*write8)(struct b53_device *dev, u8 page, u8 reg, u8 value);
	int (*write16)(struct b53_device *dev, u8 page, u8 reg, u16 value);
	int (*write32)(struct b53_device *dev, u8 page, u8 reg, u32 value);
	int (*write48)(struct b53_device *dev, u8 page, u8 reg, u64 value);
	int (*write64)(struct b53_device *dev, u8 page, u8 reg, u64 value);
	int (*phy_read16)(struct b53_device *dev, int addr, int reg, u16 *value);
	int (*phy_write16)(struct b53_device *dev, int addr, int reg, u16 value);
	int (*irq_enable)(struct b53_device *dev, int port);
	void (*irq_disable)(struct b53_device *dev, int port);
	void (*phy_enable)(struct b53_device *dev, int port);
	void (*phy_disable)(struct b53_device *dev, int port);
	void (*phylink_get_caps)(struct b53_device *dev, int port,
				 struct phylink_config *config);
	struct phylink_pcs *(*phylink_mac_select_pcs)(struct b53_device *dev,
						      int port,
						      phy_interface_t interface);
	u8 (*serdes_map_lane)(struct b53_device *dev, int port);
	void (*serdes_link_set)(struct b53_device *dev, int port,
				unsigned int mode, phy_interface_t interface,
				bool link_up);
};

#define B53_INVALID_LANE	0xff

enum {
	BCM4908_DEVICE_ID = 0x4908,
	BCM5325_DEVICE_ID = 0x25,
	BCM5365_DEVICE_ID = 0x65,
	BCM5389_DEVICE_ID = 0x89,
	BCM5395_DEVICE_ID = 0x95,
	BCM5397_DEVICE_ID = 0x97,
	BCM5398_DEVICE_ID = 0x98,
	BCM53101_DEVICE_ID = 0x53101,
	BCM53115_DEVICE_ID = 0x53115,
	BCM53125_DEVICE_ID = 0x53125,
	BCM53128_DEVICE_ID = 0x53128,
	BCM63XX_DEVICE_ID = 0x6300,
	BCM6318_DEVICE_ID = 0x6318,
	BCM6328_DEVICE_ID = 0x6328,
	BCM6362_DEVICE_ID = 0x6362,
	BCM6368_DEVICE_ID = 0x6368,
	BCM63268_DEVICE_ID = 0x63268,
	BCM53010_DEVICE_ID = 0x53010,
	BCM53011_DEVICE_ID = 0x53011,
	BCM53012_DEVICE_ID = 0x53012,
	BCM53018_DEVICE_ID = 0x53018,
	BCM53019_DEVICE_ID = 0x53019,
	BCM58XX_DEVICE_ID = 0x5800,
	BCM583XX_DEVICE_ID = 0x58300,
	BCM7445_DEVICE_ID = 0x7445,
	BCM7278_DEVICE_ID = 0x7278,
	BCM53134_DEVICE_ID = 0x5075,
};

enum b53_variant_id {
	B53_VARIANT_NONE = 0,
	B53_VARIANT_5325E,
	B53_VARIANT_5325M,
};

struct b53_pcs {
	struct phylink_pcs pcs;
	struct b53_device *dev;
	u8 lane;
};

#define B53_N_PORTS	9
#define B53_N_PORTS_25	6
#define B53_N_PCS	2

struct b53_port {
	u16		vlan_ctl_mask;
	u16		pvid;
	struct ethtool_keee eee;
};

struct b53_vlan {
	u16 members;
	u16 untag;
	bool valid;
};

struct b53_device {
	struct dsa_switch *ds;
	struct b53_platform_data *pdata;
	const char *name;

	struct mutex reg_mutex;
	struct mutex stats_mutex;
	struct mutex arl_mutex;
	const struct b53_io_ops *ops;

	/* chip specific data */
	u32 chip_id;
	enum b53_variant_id variant_id;
	u8 core_rev;
	u8 vta_regs[3];
	u8 duplex_reg;
	u8 jumbo_pm_reg;
	u8 jumbo_size_reg;
	int reset_gpio;
	u8 num_arl_bins;
	u16 num_arl_buckets;
	enum dsa_tag_protocol tag_protocol;

	/* used ports mask */
	u16 enabled_ports;
	unsigned int imp_port;

	/* connect specific data */
	u8 current_page;
	struct device *dev;
	u8 serdes_lane;

	/* Master MDIO bus we got probed from */
	struct mii_bus *bus;

	void *priv;

	/* run time configuration */
	bool enable_jumbo;

	unsigned int num_vlans;
	struct b53_vlan *vlans;
	bool vlan_enabled;
	bool vlan_filtering;
	unsigned int num_ports;
	struct b53_port *ports;

	struct b53_pcs pcs[B53_N_PCS];
};

#define b53_for_each_port(dev, i) \
	for (i = 0; i < B53_N_PORTS; i++) \
		if (dev->enabled_ports & BIT(i))


static inline int is5325(struct b53_device *dev)
{
	return dev->chip_id == BCM5325_DEVICE_ID;
}

static inline int is5325e(struct b53_device *dev)
{
	return is5325(dev) &&
		dev->variant_id == B53_VARIANT_5325E;
}

static inline int is5325m(struct b53_device *dev)
{
	return is5325(dev) &&
		dev->variant_id == B53_VARIANT_5325M;
}

static inline int is5365(struct b53_device *dev)
{
#ifdef CONFIG_BCM47XX
	return dev->chip_id == BCM5365_DEVICE_ID;
#else
	return 0;
#endif
}

static inline int is5397_98(struct b53_device *dev)
{
	return dev->chip_id == BCM5397_DEVICE_ID ||
		dev->chip_id == BCM5398_DEVICE_ID;
}

static inline int is539x(struct b53_device *dev)
{
	return dev->chip_id == BCM5395_DEVICE_ID ||
		dev->chip_id == BCM5397_DEVICE_ID ||
		dev->chip_id == BCM5398_DEVICE_ID;
}

static inline int is531x5(struct b53_device *dev)
{
	return dev->chip_id == BCM53115_DEVICE_ID ||
		dev->chip_id == BCM53125_DEVICE_ID ||
		dev->chip_id == BCM53101_DEVICE_ID ||
		dev->chip_id == BCM53128_DEVICE_ID ||
		dev->chip_id == BCM53134_DEVICE_ID;
}

static inline int is63xx(struct b53_device *dev)
{
	return dev->chip_id == BCM63XX_DEVICE_ID ||
		dev->chip_id == BCM6318_DEVICE_ID ||
		dev->chip_id == BCM6328_DEVICE_ID ||
		dev->chip_id == BCM6362_DEVICE_ID ||
		dev->chip_id == BCM6368_DEVICE_ID ||
		dev->chip_id == BCM63268_DEVICE_ID;
}

static inline int is6318_268(struct b53_device *dev)
{
	return dev->chip_id == BCM6318_DEVICE_ID ||
		dev->chip_id == BCM63268_DEVICE_ID;
}

static inline int is5301x(struct b53_device *dev)
{
	return dev->chip_id == BCM53010_DEVICE_ID ||
		dev->chip_id == BCM53011_DEVICE_ID ||
		dev->chip_id == BCM53012_DEVICE_ID ||
		dev->chip_id == BCM53018_DEVICE_ID ||
		dev->chip_id == BCM53019_DEVICE_ID;
}

static inline int is58xx(struct b53_device *dev)
{
	return dev->chip_id == BCM58XX_DEVICE_ID ||
		dev->chip_id == BCM583XX_DEVICE_ID ||
		dev->chip_id == BCM7445_DEVICE_ID ||
		dev->chip_id == BCM7278_DEVICE_ID ||
		dev->chip_id == BCM53134_DEVICE_ID;
}

#define B53_63XX_RGMII0	4
#define B53_CPU_PORT_25	5
#define B53_CPU_PORT	8

static inline unsigned int b53_max_arl_entries(struct b53_device *dev)
{
	return dev->num_arl_buckets * dev->num_arl_bins;
}

struct b53_device *b53_switch_alloc(struct device *base,
				    const struct b53_io_ops *ops,
				    void *priv);

int b53_switch_detect(struct b53_device *dev);

int b53_switch_register(struct b53_device *dev);

static inline void b53_switch_remove(struct b53_device *dev)
{
	dsa_unregister_switch(dev->ds);
}

static inline void b53_switch_shutdown(struct b53_device *dev)
{
	dsa_switch_shutdown(dev->ds);
}

#define b53_build_op(type_op_size, val_type)				\
static inline int b53_##type_op_size(struct b53_device *dev, u8 page,	\
				     u8 reg, val_type val)		\
{									\
	int ret;							\
									\
	mutex_lock(&dev->reg_mutex);					\
	ret = dev->ops->type_op_size(dev, page, reg, val);		\
	mutex_unlock(&dev->reg_mutex);					\
									\
	return ret;							\
}

b53_build_op(read8, u8 *);
b53_build_op(read16, u16 *);
b53_build_op(read32, u32 *);
b53_build_op(read48, u64 *);
b53_build_op(read64, u64 *);

b53_build_op(write8, u8);
b53_build_op(write16, u16);
b53_build_op(write32, u32);
b53_build_op(write48, u64);
b53_build_op(write64, u64);

struct b53_arl_entry {
	u16 port;
	u8 mac[ETH_ALEN];
	u16 vid;
	u8 is_valid:1;
	u8 is_age:1;
	u8 is_static:1;
};

static inline void b53_arl_to_entry(struct b53_arl_entry *ent,
				    u64 mac_vid, u32 fwd_entry)
{
	memset(ent, 0, sizeof(*ent));
	ent->port = fwd_entry & ARLTBL_DATA_PORT_ID_MASK;
	ent->is_valid = !!(fwd_entry & ARLTBL_VALID);
	ent->is_age = !!(fwd_entry & ARLTBL_AGE);
	ent->is_static = !!(fwd_entry & ARLTBL_STATIC);
	u64_to_ether_addr(mac_vid, ent->mac);
	ent->vid = mac_vid >> ARLTBL_VID_S;
}

static inline void b53_arl_to_entry_25(struct b53_arl_entry *ent,
				       u64 mac_vid)
{
	memset(ent, 0, sizeof(*ent));
	ent->port = (mac_vid >> ARLTBL_DATA_PORT_ID_S_25) &
		     ARLTBL_DATA_PORT_ID_MASK_25;
	ent->is_valid = !!(mac_vid & ARLTBL_VALID_25);
	ent->is_age = !!(mac_vid & ARLTBL_AGE_25);
	ent->is_static = !!(mac_vid & ARLTBL_STATIC_25);
	u64_to_ether_addr(mac_vid, ent->mac);
	ent->vid = mac_vid >> ARLTBL_VID_S_65;
}

static inline void b53_arl_from_entry(u64 *mac_vid, u32 *fwd_entry,
				      const struct b53_arl_entry *ent)
{
	*mac_vid = ether_addr_to_u64(ent->mac);
	*mac_vid |= (u64)(ent->vid & ARLTBL_VID_MASK) << ARLTBL_VID_S;
	*fwd_entry = ent->port & ARLTBL_DATA_PORT_ID_MASK;
	if (ent->is_valid)
		*fwd_entry |= ARLTBL_VALID;
	if (ent->is_static)
		*fwd_entry |= ARLTBL_STATIC;
	if (ent->is_age)
		*fwd_entry |= ARLTBL_AGE;
}

static inline void b53_arl_from_entry_25(u64 *mac_vid,
					 const struct b53_arl_entry *ent)
{
	*mac_vid = ether_addr_to_u64(ent->mac);
	*mac_vid |= (u64)(ent->port & ARLTBL_DATA_PORT_ID_MASK_25) <<
			  ARLTBL_DATA_PORT_ID_S_25;
	*mac_vid |= (u64)(ent->vid & ARLTBL_VID_MASK_25) <<
			  ARLTBL_VID_S_65;
	if (ent->is_valid)
		*mac_vid |= ARLTBL_VALID_25;
	if (ent->is_static)
		*mac_vid |= ARLTBL_STATIC_25;
	if (ent->is_age)
		*mac_vid |= ARLTBL_AGE_25;
}

#ifdef CONFIG_BCM47XX

#include <linux/bcm47xx_nvram.h>
#include <bcm47xx_board.h>
static inline int b53_switch_get_reset_gpio(struct b53_device *dev)
{
	enum bcm47xx_board board = bcm47xx_board_get();

	switch (board) {
	case BCM47XX_BOARD_LINKSYS_WRT300NV11:
	case BCM47XX_BOARD_LINKSYS_WRT310NV1:
		return 8;
	default:
		return bcm47xx_nvram_gpio_pin("robo_reset");
	}
}
#else
static inline int b53_switch_get_reset_gpio(struct b53_device *dev)
{
	return -ENOENT;
}
#endif

/* Exported functions towards other drivers */
void b53_imp_vlan_setup(struct dsa_switch *ds, int cpu_port);
int b53_configure_vlan(struct dsa_switch *ds);
void b53_get_strings(struct dsa_switch *ds, int port, u32 stringset,
		     uint8_t *data);
void b53_get_ethtool_stats(struct dsa_switch *ds, int port, uint64_t *data);
int b53_get_sset_count(struct dsa_switch *ds, int port, int sset);
void b53_get_ethtool_phy_stats(struct dsa_switch *ds, int port, uint64_t *data);
int b53_set_ageing_time(struct dsa_switch *ds, unsigned int msecs);
int b53_br_join(struct dsa_switch *ds, int port, struct dsa_bridge bridge,
		bool *tx_fwd_offload, struct netlink_ext_ack *extack);
void b53_br_leave(struct dsa_switch *ds, int port, struct dsa_bridge bridge);
void b53_br_set_stp_state(struct dsa_switch *ds, int port, u8 state);
void b53_br_fast_age(struct dsa_switch *ds, int port);
int b53_br_flags_pre(struct dsa_switch *ds, int port,
		     struct switchdev_brport_flags flags,
		     struct netlink_ext_ack *extack);
int b53_br_flags(struct dsa_switch *ds, int port,
		 struct switchdev_brport_flags flags,
		 struct netlink_ext_ack *extack);
int b53_setup_devlink_resources(struct dsa_switch *ds);
void b53_port_event(struct dsa_switch *ds, int port);
int b53_vlan_filtering(struct dsa_switch *ds, int port, bool vlan_filtering,
		       struct netlink_ext_ack *extack);
int b53_vlan_add(struct dsa_switch *ds, int port,
		 const struct switchdev_obj_port_vlan *vlan,
		 struct netlink_ext_ack *extack);
int b53_vlan_del(struct dsa_switch *ds, int port,
		 const struct switchdev_obj_port_vlan *vlan);
int b53_fdb_add(struct dsa_switch *ds, int port,
		const unsigned char *addr, u16 vid,
		struct dsa_db db);
int b53_fdb_del(struct dsa_switch *ds, int port,
		const unsigned char *addr, u16 vid,
		struct dsa_db db);
int b53_fdb_dump(struct dsa_switch *ds, int port,
		 dsa_fdb_dump_cb_t *cb, void *data);
int b53_mdb_add(struct dsa_switch *ds, int port,
		const struct switchdev_obj_port_mdb *mdb,
		struct dsa_db db);
int b53_mdb_del(struct dsa_switch *ds, int port,
		const struct switchdev_obj_port_mdb *mdb,
		struct dsa_db db);
int b53_mirror_add(struct dsa_switch *ds, int port,
		   struct dsa_mall_mirror_tc_entry *mirror, bool ingress,
		   struct netlink_ext_ack *extack);
enum dsa_tag_protocol b53_get_tag_protocol(struct dsa_switch *ds, int port,
					   enum dsa_tag_protocol mprot);
void b53_mirror_del(struct dsa_switch *ds, int port,
		    struct dsa_mall_mirror_tc_entry *mirror);
int b53_setup_port(struct dsa_switch *ds, int port);
int b53_enable_port(struct dsa_switch *ds, int port, struct phy_device *phy);
void b53_disable_port(struct dsa_switch *ds, int port);
void b53_brcm_hdr_setup(struct dsa_switch *ds, int port);
int b53_eee_init(struct dsa_switch *ds, int port, struct phy_device *phy);
bool b53_support_eee(struct dsa_switch *ds, int port);
int b53_set_mac_eee(struct dsa_switch *ds, int port, struct ethtool_keee *e);

#endif
