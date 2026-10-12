// SPDX-License-Identifier: GPL-2.0-only
/*
 * Siflower SF19A2890 GMAC hardware NAT offload
 *
 * The HNAT engine sits between the MAC and the DMA engine of the GMAC. It
 * handles IPv4 TCP/UDP NAPT for flows between ports of a switch attached
 * to this GMAC through DSA with the gsw1xx-8021q tagger, optionally with
 * PPPoE on the WAN side, and is driven by netfilter flowtable hardware
 * offload. It has no IPv6 support.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/dsa/8021q.h>
#include <linux/etherdevice.h>
#include <linux/hashtable.h>
#include <linux/if_bridge.h>
#include <linux/inetdevice.h>
#include <linux/iopoll.h>
#include <linux/ip.h>
#include <linux/jhash.h>
#include <linux/module.h>
#include <linux/ppp_defs.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <net/dsa.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "dwmac-sf19a2890-hnat.h"

#define HNAT_CTRL		0x4004
#define  HNAT_CTRL_INIT		0x0040024f
#define  HNAT_CTRL_SYMMETRIC	BIT(25)
#define HNAT_CFG		0x4008
#define  HNAT_CFG_LOAD		BIT(0)
#define  HNAT_CFG_SNAPSHOT	BIT(4)
#define HNAT_VLAN_MODE		0x400c
#define HNAT_REG4		0x4010
#define HNAT_LAN_NET(n)		(0x4040 + 4 * (n))
#define HNAT_LAN_MASK(n)	(0x4080 + 4 * ((n) / 4))
#define HNAT_CLK(n)		(0x408c + 4 * (n))
#define HNAT_CNT_RX		0x4100
#define HNAT_CNT_TO_HOST	0x4114
#define HNAT_CNT_FWD		0x4124
#define HNAT_CNT_RX_ERR		0x4178
#define HNAT_CNT_FWD_DROP	0x4194

#define HNAT_TB_STATUS		0x6000
#define  HNAT_TB_BUSY		BIT(0)
#define HNAT_TB_CFG		0x6004
#define  HNAT_TB_CFG_RD_CLR	BIT(2)
#define  HNAT_TB_CFG_BE		BIT(4)
#define  HNAT_TB_CFG_CRC3_8005	BIT(10)
#define  HNAT_TB_CFG_EN		GENMASK(27, 16)
#define HNAT_TB_ADDR		0x6008
#define HNAT_TB_OP		0x600c
#define  HNAT_TB_OP_RD		0
#define  HNAT_TB_OP_WR		1
#define HNAT_TB_WDATA(n)	(0x6010 + 4 * (n))
#define HNAT_TB_RDATA(n)	(0x6030 + 4 * (n))
#define HNAT_TB_BUF_THRESH	0x6050
#define HNAT_TB_FC_CFG		0x6060
#define HNAT_TB_RFC_TIMER	0x6064
#define HNAT_TB_TFC_TIMER	0x606c

enum {
	TB_IN_HASH	= 0,
	TB_OUT_HASH	= 3,
	TB_NAPT		= 6,
	TB_NAPT_VLD	= 7,
	TB_PUB_NET	= 8,
	TB_DIP_HASH	= 16,
	TB_DIP		= 18,
	TB_DIP_VLD	= 19,
	TB_DMAC		= 20,
	TB_PPPOE	= 21,
	TB_RMAC		= 22,
	TB_VLAN		= 23,
	TB_NUM		= 25,
};

static const u16 hnat_tb_depth[TB_NUM] = {
	[0] = 512, [1] = 256, [2] = 32,
	[3] = 512, [4] = 256, [5] = 32,
	[6] = 1024, [7] = 32, [8] = 16,
	[16] = 256, [17] = 64, [18] = 512, [19] = 16,
	[20] = 128, [21] = 8, [22] = 8, [23] = 128, [24] = 128,
};

#define HNAT_NAPT_NUM		1024
#define HNAT_DIP_NUM		512
#define HNAT_DMAC_NUM		128
#define HNAT_RMAC_NUM		8
#define HNAT_PPPOE_NUM		8
#define HNAT_VLAN_NUM		127
#define HNAT_PUB_NUM		16
#define HNAT_LAN_NUM		8

/*
 * The engine has no per-entry hit counters, so idle flows can't be told
 * apart from busy ones. Entries are removed after this time and the next
 * packet of a busy flow, now going through the software path, makes the
 * flowtable offload it again.
 */
#define HNAT_HW_TIMEOUT		(15 * HZ)

struct hnat_vlan {
	u16 vid;
	u16 refs;
};

struct hnat_rmac {
	u8 mac[ETH_ALEN];
	u16 refs;
};

struct hnat_dmac {
	u8 mac[ETH_ALEN];
	u8 rmac;
	u16 refs;
};

struct hnat_pppoe {
	u16 sid;
	u16 refs;
};

struct hnat_hash_pos {
	u16 bucket;
	u8 table;
	u8 slot;
};

struct hnat_dip {
	__be32 ip;
	u8 vlan;
	u8 dmac;
	u16 refs;
	struct hnat_hash_pos pos;
};

struct hnat_pub {
	__be32 ip;
	u8 vlan;
	u8 plen;
	u16 refs;
};

struct hnat_lan {
	u32 net;
	u8 plen;
	u16 refs;
};

struct hnat_napt {
	u8 lan_vlan;
	u8 wan_vlan;
	u8 pub;
	u8 lan;
	s8 pppoe;
	u16 lan_dip;
	u16 wan_dip;
	struct hnat_hash_pos in;
	struct hnat_hash_pos out;
};

struct hnat_tuple {
	__be32 lan_ip;
	__be32 remote_ip;
	__be32 pub_ip;
	__be16 lan_port;
	__be16 remote_port;
	__be16 pub_port;
	u8 proto;
};

enum { DIR_OUT, DIR_IN };

struct hnat_flow;

struct hnat_cookie {
	struct hlist_node node;
	unsigned long cookie;
	struct hnat_flow *flow;
	int dir;
};

struct hnat_flow {
	struct hlist_node node;
	struct hnat_tuple t;
	struct hnat_cookie cookie[2];
	bool have[2];
	u8 lan_mac[ETH_ALEN];
	u8 lan_smac[ETH_ALEN];
	u8 remote_mac[ETH_ALEN];
	u8 wan_smac[ETH_ALEN];
	u16 lan_vid;
	u16 wan_vid;
	u32 lan_net;
	u8 lan_plen;
	u8 wan_plen;
	bool pppoe;
	u16 pppoe_sid;
	bool in_hw;
	u16 napt;
	unsigned long hw_since;
};

struct hnat_rule {
	struct hnat_tuple t;
	int dir;
	u8 eth[2 * ETH_ALEN];
	u16 in_vid;
	u16 out_vid;
	u32 lan_net;
	u8 lan_plen;
	u8 wan_plen;
	bool pppoe;
	u16 pppoe_sid;
};

struct sf19a2890_hnat {
	struct device *dev;
	void __iomem *base;
	struct net_device *ndev;
	struct mutex lock;
	u32 ctrl;
	bool running;

	DECLARE_BITMAP(napt_used, HNAT_NAPT_NUM);
	struct hnat_napt napt[HNAT_NAPT_NUM];
	struct hnat_dip dip[HNAT_DIP_NUM];
	struct hnat_dmac dmac[HNAT_DMAC_NUM];
	struct hnat_rmac rmac[HNAT_RMAC_NUM];
	struct hnat_pppoe pppoe[HNAT_PPPOE_NUM];
	struct hnat_vlan vlan[HNAT_VLAN_NUM];
	struct hnat_pub pub[HNAT_PUB_NUM];
	struct hnat_lan lan[HNAT_LAN_NUM];

	DECLARE_HASHTABLE(flows, 8);
	DECLARE_HASHTABLE(cookies, 9);
	struct list_head block_list;
	struct dentry *debugfs;

	unsigned int nflows;
	unsigned int nhw;
	unsigned long added;
	unsigned long failed;
	unsigned long expired;
	unsigned long resets;
};

static int hnat_tb_wait(struct sf19a2890_hnat *h)
{
	u32 val;

	return readl_poll_timeout(h->base + HNAT_TB_STATUS, val,
				  !(val & HNAT_TB_BUSY), 0, 1000);
}

static int hnat_tb_write(struct sf19a2890_hnat *h, int tb, int idx,
			 const u32 *data)
{
	int i, ret;

	ret = hnat_tb_wait(h);
	if (ret)
		return ret;

	writel(tb << 16 | idx, h->base + HNAT_TB_ADDR);
	for (i = 0; i < 5; i++)
		writel(data[i], h->base + HNAT_TB_WDATA(i));
	writel(HNAT_TB_OP_WR, h->base + HNAT_TB_OP);

	return hnat_tb_wait(h);
}

static int hnat_tb_read(struct sf19a2890_hnat *h, int tb, int idx, u32 *data)
{
	int i, ret;

	ret = hnat_tb_wait(h);
	if (ret)
		return ret;

	writel(tb << 16 | idx, h->base + HNAT_TB_ADDR);
	writel(HNAT_TB_OP_RD, h->base + HNAT_TB_OP);

	ret = hnat_tb_wait(h);
	if (ret)
		return ret;

	for (i = 0; i < 5; i++)
		data[i] = readl(h->base + HNAT_TB_RDATA(i));

	return 0;
}

static int hnat_tb_clear(struct sf19a2890_hnat *h, int tb, int idx)
{
	static const u32 zero[5];

	return hnat_tb_write(h, tb, idx, zero);
}

static int hnat_tb_set_bit(struct sf19a2890_hnat *h, int tb, int idx, bool on)
{
	u32 d[5];
	int ret;

	ret = hnat_tb_read(h, tb, idx / 32, d);
	if (ret)
		return ret;

	if (on)
		d[0] |= BIT(idx % 32);
	else
		d[0] &= ~BIT(idx % 32);

	return hnat_tb_write(h, tb, idx / 32, d);
}

static void hnat_load_cfg(struct sf19a2890_hnat *h)
{
	u32 val = readl(h->base + HNAT_CFG);

	writel(val & ~HNAT_CFG_LOAD, h->base + HNAT_CFG);
	writel(val | HNAT_CFG_LOAD, h->base + HNAT_CFG);
}

static u16 hnat_crc16(const u8 *buf, int len, u16 poly)
{
	u16 crc = 0;
	int i;

	while (len--) {
		crc ^= *buf++ << 8;
		for (i = 0; i < 8; i++)
			crc = crc & 0x8000 ? (crc << 1) ^ poly : crc << 1;
	}

	return crc;
}

/*
 * NAPT hash tables 1 and 3 hold two 10-bit entry pointers in word 0 with
 * their valid bits at 21:20. Table 2 holds four, the last one split into
 * word 0 bits 31:30 and word 1 bits 7:0, with valid bits at word 1 11:8.
 */
static int hnat_napt_hash_add(struct sf19a2890_hnat *h, int tb, int which,
			      u16 bucket, u16 idx)
{
	u32 d[5];
	int i, ret;

	ret = hnat_tb_read(h, tb, bucket, d);
	if (ret)
		return ret;

	if (which == 1) {
		for (i = 0; i < 4; i++)
			if (!(d[1] & BIT(8 + i)))
				break;
		if (i == 4)
			return -ENOSPC;

		if (i < 3) {
			d[0] &= ~(0x3ff << (i * 10));
			d[0] |= idx << (i * 10);
		} else {
			d[0] &= ~GENMASK(31, 30);
			d[0] |= (idx & 0x3) << 30;
			d[1] &= ~GENMASK(7, 0);
			d[1] |= idx >> 2;
		}
		d[1] |= BIT(8 + i);
	} else {
		for (i = 0; i < 2; i++)
			if (!(d[0] & BIT(20 + i)))
				break;
		if (i == 2)
			return -ENOSPC;

		d[0] &= ~(0x3ff << (i * 10));
		d[0] |= idx << (i * 10) | BIT(20 + i);
	}

	ret = hnat_tb_write(h, tb, bucket, d);

	return ret ?: i;
}

static int hnat_napt_hash_insert(struct sf19a2890_hnat *h, int base,
				 const u8 *key, u16 idx,
				 struct hnat_hash_pos *pos)
{
	u16 c1 = hnat_crc16(key, 13, 0x1021);
	u16 c2 = hnat_crc16(key, 13, 0x8005);
	u16 bucket[3] = { (c1 >> 7) & 0x1ff, c1 & 0xff, (c2 >> 4) & 0x1f };
	int i, ret;

	for (i = 0; i < 3; i++) {
		ret = hnat_napt_hash_add(h, base + i, i, bucket[i], idx);
		if (ret == -ENOSPC)
			continue;
		if (ret < 0)
			return ret;

		pos->table = base + i;
		pos->bucket = bucket[i];
		pos->slot = ret;
		return 0;
	}

	return -ENOSPC;
}

static void hnat_napt_hash_remove(struct sf19a2890_hnat *h,
				  const struct hnat_hash_pos *pos)
{
	u32 d[5];

	if (hnat_tb_read(h, pos->table, pos->bucket, d))
		return;

	if ((pos->table - TB_IN_HASH) % 3 == 1)
		d[1] &= ~BIT(8 + pos->slot);
	else
		d[0] &= ~BIT(20 + pos->slot);

	hnat_tb_write(h, pos->table, pos->bucket, d);
}

/*
 * DIP hash buckets hold four 9-bit pointers in word 0, the last one split
 * into word 0 bits 31:27 and word 1 bits 3:0, with valid bits at word 1
 * 7:4. Slot 2 of the first DIP hash table doesn't work.
 */
static int hnat_dip_hash_add(struct sf19a2890_hnat *h, int which, u16 bucket,
			     u16 idx)
{
	int tb = TB_DIP_HASH + which;
	u32 d[5];
	int i, ret;

	ret = hnat_tb_read(h, tb, bucket, d);
	if (ret)
		return ret;

	for (i = 0; i < 4; i++) {
		if (which == 0 && i == 2)
			continue;
		if (!(d[1] & BIT(4 + i)))
			break;
	}
	if (i == 4)
		return -ENOSPC;

	if (i < 3) {
		d[0] &= ~(0x1ff << (i * 9));
		d[0] |= idx << (i * 9);
	} else {
		d[0] &= ~GENMASK(31, 27);
		d[0] |= (idx & 0x1f) << 27;
		d[1] &= ~GENMASK(3, 0);
		d[1] |= idx >> 5;
	}
	d[1] |= BIT(4 + i);

	ret = hnat_tb_write(h, tb, bucket, d);

	return ret ?: i;
}

static void hnat_dip_hash_remove(struct sf19a2890_hnat *h,
				 const struct hnat_hash_pos *pos)
{
	u32 d[5];

	if (hnat_tb_read(h, pos->table, pos->bucket, d))
		return;

	d[1] &= ~BIT(4 + pos->slot);
	hnat_tb_write(h, pos->table, pos->bucket, d);
}

static int hnat_vlan_get(struct sf19a2890_hnat *h, u16 vid)
{
	u32 d[5] = { vid };
	int i, free = -1;

	for (i = 0; i < HNAT_VLAN_NUM; i++) {
		if (h->vlan[i].refs && h->vlan[i].vid == vid) {
			h->vlan[i].refs++;
			return i;
		}
		if (!h->vlan[i].refs && free < 0)
			free = i;
	}
	if (free < 0)
		return -ENOSPC;

	if (hnat_tb_write(h, TB_VLAN, free, d))
		return -EIO;

	h->vlan[free].vid = vid;
	h->vlan[free].refs = 1;

	return free;
}

static void hnat_vlan_put(struct sf19a2890_hnat *h, int i)
{
	if (--h->vlan[i].refs)
		return;

	hnat_tb_clear(h, TB_VLAN, i);
}

static int hnat_rmac_get(struct sf19a2890_hnat *h, const u8 *mac)
{
	u32 d[5] = {};
	int i, free = -1;

	for (i = 0; i < HNAT_RMAC_NUM; i++) {
		if (h->rmac[i].refs && ether_addr_equal(h->rmac[i].mac, mac)) {
			h->rmac[i].refs++;
			return i;
		}
		if (!h->rmac[i].refs && free < 0)
			free = i;
	}
	if (free < 0)
		return -ENOSPC;

	d[0] = get_unaligned_le32(mac);
	d[1] = get_unaligned_le16(mac + 4);
	if (hnat_tb_write(h, TB_RMAC, free, d))
		return -EIO;

	ether_addr_copy(h->rmac[free].mac, mac);
	h->rmac[free].refs = 1;

	return free;
}

static void hnat_rmac_put(struct sf19a2890_hnat *h, int i)
{
	if (--h->rmac[i].refs)
		return;

	hnat_tb_clear(h, TB_RMAC, i);
}

static int hnat_dmac_get(struct sf19a2890_hnat *h, const u8 *mac,
			 const u8 *smac)
{
	u32 d[5] = {};
	int i, rmac, free = -1;

	for (i = 0; i < HNAT_DMAC_NUM; i++) {
		struct hnat_dmac *e = &h->dmac[i];

		if (e->refs && ether_addr_equal(e->mac, mac) &&
		    ether_addr_equal(h->rmac[e->rmac].mac, smac)) {
			e->refs++;
			return i;
		}
		if (!e->refs && free < 0)
			free = i;
	}
	if (free < 0)
		return -ENOSPC;

	rmac = hnat_rmac_get(h, smac);
	if (rmac < 0)
		return rmac;

	d[0] = get_unaligned_le32(mac);
	d[1] = get_unaligned_le16(mac + 4) | rmac << 16;
	if (hnat_tb_write(h, TB_DMAC, free, d)) {
		hnat_rmac_put(h, rmac);
		return -EIO;
	}

	ether_addr_copy(h->dmac[free].mac, mac);
	h->dmac[free].rmac = rmac;
	h->dmac[free].refs = 1;

	return free;
}

static void hnat_dmac_put(struct sf19a2890_hnat *h, int i)
{
	if (--h->dmac[i].refs)
		return;

	hnat_tb_clear(h, TB_DMAC, i);
	hnat_rmac_put(h, h->dmac[i].rmac);
}

static int hnat_dip_write(struct sf19a2890_hnat *h, int i)
{
	u32 d[5] = {};

	d[0] = (__force u32)h->dip[i].ip;
	d[1] = h->dip[i].vlan << 7 | h->dip[i].dmac;

	return hnat_tb_write(h, TB_DIP, i, d);
}

static int hnat_dip_get(struct sf19a2890_hnat *h, __be32 ip, int vlan,
			const u8 *mac, const u8 *smac)
{
	struct hnat_dip *e;
	int i, dmac, ret, free = -1;
	u16 c1, bucket[2];
	u8 key[5];

	for (i = 0; i < HNAT_DIP_NUM; i++) {
		e = &h->dip[i];
		if (!e->refs) {
			if (free < 0)
				free = i;
			continue;
		}
		if (e->ip != ip || e->vlan != vlan)
			continue;

		if (!ether_addr_equal(h->dmac[e->dmac].mac, mac) ||
		    !ether_addr_equal(h->rmac[h->dmac[e->dmac].rmac].mac, smac)) {
			dmac = hnat_dmac_get(h, mac, smac);
			if (dmac < 0)
				return dmac;
			hnat_dmac_put(h, e->dmac);
			e->dmac = dmac;
			hnat_dip_write(h, i);
		}
		e->refs++;
		return i;
	}
	if (free < 0)
		return -ENOSPC;

	e = &h->dip[free];
	dmac = hnat_dmac_get(h, mac, smac);
	if (dmac < 0)
		return dmac;

	e->ip = ip;
	e->vlan = vlan;
	e->dmac = dmac;

	/* 32-bit address followed by the 7-bit VLAN index, right aligned */
	memcpy(key, &ip, 4);
	key[4] = vlan;
	for (i = 3; i >= 0; i--) {
		key[i + 1] = (key[i + 1] & 0x7f) | (key[i] & 1) << 7;
		key[i] >>= 1;
	}
	c1 = hnat_crc16(key, sizeof(key), 0x1021);
	bucket[0] = c1 & 0xff;
	bucket[1] = (c1 >> 10) & 0x3f;

	for (i = 0; i < 2; i++) {
		ret = hnat_dip_hash_add(h, i, bucket[i], free);
		if (ret >= 0)
			break;
	}
	if (ret < 0)
		goto err_dmac;

	e->pos.table = TB_DIP_HASH + i;
	e->pos.bucket = bucket[i];
	e->pos.slot = ret;

	ret = hnat_dip_write(h, free);
	if (!ret)
		ret = hnat_tb_set_bit(h, TB_DIP_VLD, free, true);
	if (ret)
		goto err_hash;

	e->refs = 1;

	return free;

err_hash:
	hnat_dip_hash_remove(h, &e->pos);
err_dmac:
	hnat_dmac_put(h, dmac);
	return ret;
}

static void hnat_dip_put(struct sf19a2890_hnat *h, int i)
{
	struct hnat_dip *e = &h->dip[i];

	if (--e->refs)
		return;

	hnat_tb_set_bit(h, TB_DIP_VLD, i, false);
	hnat_dip_hash_remove(h, &e->pos);
	hnat_tb_clear(h, TB_DIP, i);
	hnat_dmac_put(h, e->dmac);
}

static int hnat_pub_get(struct sf19a2890_hnat *h, __be32 ip, int vlan, u8 plen)
{
	u32 d[5] = {};
	int i, free = -1;

	for (i = 0; i < HNAT_PUB_NUM; i++) {
		struct hnat_pub *e = &h->pub[i];

		if (e->refs && e->ip == ip && e->vlan == vlan && e->plen == plen) {
			e->refs++;
			return i;
		}
		if (!e->refs && free < 0)
			free = i;
	}
	if (free < 0)
		return -ENOSPC;

	d[0] = (__force u32)ip;
	d[1] = min_t(u8, plen, 31) | vlan << 5;
	if (hnat_tb_write(h, TB_PUB_NET, free, d))
		return -EIO;

	h->pub[free].ip = ip;
	h->pub[free].vlan = vlan;
	h->pub[free].plen = plen;
	h->pub[free].refs = 1;

	return free;
}

static void hnat_pub_put(struct sf19a2890_hnat *h, int i)
{
	if (--h->pub[i].refs)
		return;

	hnat_tb_clear(h, TB_PUB_NET, i);
}

static int hnat_pppoe_get(struct sf19a2890_hnat *h, u16 sid)
{
	u32 d[5] = {};
	int i, free = -1;

	for (i = 0; i < HNAT_PPPOE_NUM; i++) {
		struct hnat_pppoe *e = &h->pppoe[i];

		if (e->refs && e->sid == sid) {
			e->refs++;
			return i;
		}
		if (!e->refs && free < 0)
			free = i;
	}
	if (free < 0)
		return -ENOSPC;

	/* header template: PPP protocol IPv4, then ver/type 0x11, code 0, sid */
	d[0] = PPP_IP;
	d[1] = 0x11000000 | sid;
	if (hnat_tb_write(h, TB_PPPOE, free, d))
		return -EIO;

	h->pppoe[free].sid = sid;
	h->pppoe[free].refs = 1;

	return free;
}

static void hnat_pppoe_put(struct sf19a2890_hnat *h, int i)
{
	if (--h->pppoe[i].refs)
		return;

	hnat_tb_clear(h, TB_PPPOE, i);
}

static void hnat_lan_write(struct sf19a2890_hnat *h, int i, u32 net, u8 plen)
{
	int shift = (i % 4) * 8;
	u32 mask;

	mask = readl(h->base + HNAT_LAN_MASK(i)) & ~(0xff << shift);
	if (plen)
		mask |= ((32 - plen) | 0x20) << shift;

	writel(net, h->base + HNAT_LAN_NET(i));
	writel(mask, h->base + HNAT_LAN_MASK(i));
	hnat_load_cfg(h);
}

static int hnat_lan_get(struct sf19a2890_hnat *h, u32 net, u8 plen)
{
	int i, free = -1;

	for (i = 0; i < HNAT_LAN_NUM; i++) {
		struct hnat_lan *e = &h->lan[i];

		if (e->refs && e->net == net && e->plen == plen) {
			e->refs++;
			return i;
		}
		if (!e->refs && free < 0)
			free = i;
	}
	if (free < 0)
		return -ENOSPC;

	hnat_lan_write(h, free, net, plen);
	h->lan[free].net = net;
	h->lan[free].plen = plen;
	h->lan[free].refs = 1;

	return free;
}

static void hnat_lan_put(struct sf19a2890_hnat *h, int i)
{
	if (--h->lan[i].refs)
		return;

	hnat_lan_write(h, i, 0, 0);
}

static void hnat_in_key(u8 *key, const struct hnat_tuple *t)
{
	key[0] = t->proto;
	memcpy(key + 1, &t->remote_port, 2);
	memcpy(key + 3, &t->remote_ip, 4);
	memcpy(key + 7, &t->pub_port, 2);
	memcpy(key + 9, &t->pub_ip, 4);
}

static void hnat_out_key(u8 *key, const struct hnat_tuple *t)
{
	key[0] = t->proto;
	memcpy(key + 1, &t->lan_port, 2);
	memcpy(key + 3, &t->lan_ip, 4);
	memcpy(key + 7, &t->remote_port, 2);
	memcpy(key + 9, &t->remote_ip, 4);
}

static bool hnat_same_subnet(__be32 a, __be32 b, u8 plen)
{
	return !((a ^ b) & inet_make_mask(plen));
}

static int hnat_napt_add(struct sf19a2890_hnat *h, const struct hnat_flow *fl)
{
	const struct hnat_tuple *t = &fl->t;
	u32 lan_ip = (__force u32)t->lan_ip;
	struct hnat_napt *n;
	u32 d[5] = {};
	__be32 next_hop;
	u8 key[13];
	int idx, ret;

	idx = find_first_zero_bit(h->napt_used, HNAT_NAPT_NUM);
	if (idx == HNAT_NAPT_NUM)
		return -ENOSPC;
	n = &h->napt[idx];

	ret = hnat_vlan_get(h, fl->lan_vid);
	if (ret < 0)
		return ret;
	n->lan_vlan = ret;

	ret = hnat_vlan_get(h, fl->wan_vid);
	if (ret < 0)
		goto err_lan_vlan;
	n->wan_vlan = ret;

	ret = hnat_pub_get(h, t->pub_ip, n->wan_vlan, fl->wan_plen);
	if (ret < 0)
		goto err_wan_vlan;
	n->pub = ret;

	ret = hnat_lan_get(h, fl->lan_net, fl->lan_plen);
	if (ret < 0)
		goto err_pub;
	n->lan = ret;

	ret = hnat_dip_get(h, t->lan_ip, n->lan_vlan, fl->lan_mac,
			   fl->lan_smac);
	if (ret < 0)
		goto err_lan;
	n->lan_dip = ret;

	/* off-link destinations are looked up by the router's own address */
	next_hop = hnat_same_subnet(t->remote_ip, t->pub_ip, fl->wan_plen) ?
		   t->remote_ip : t->pub_ip;
	ret = hnat_dip_get(h, next_hop, n->wan_vlan, fl->remote_mac,
			   fl->wan_smac);
	if (ret < 0)
		goto err_lan_dip;
	n->wan_dip = ret;

	n->pppoe = -1;
	if (fl->pppoe) {
		ret = hnat_pppoe_get(h, fl->pppoe_sid);
		if (ret < 0)
			goto err_wan_dip;
		n->pppoe = ret;
	}

	/* the table interface byte swaps these fields in big endian mode */
	d[0] = (__force u32)t->remote_ip;
	d[1] = (lan_ip & 0xffff) << 16 | (__force u16)t->remote_port;
	d[2] = (__force u16)t->lan_port << 16 | lan_ip >> 16;
	d[3] = t->proto << 27 | n->lan_vlan << 20 |
	       (__force u16)t->pub_port << 4 | n->pub;
	if (n->pppoe >= 0) {
		d[3] |= (n->pppoe & 0x3) << 30 | BIT(29);
		d[4] = n->pppoe >> 2;
	}
	ret = hnat_tb_write(h, TB_NAPT, idx, d);
	if (ret)
		goto err_pppoe;

	hnat_in_key(key, t);
	ret = hnat_napt_hash_insert(h, TB_IN_HASH, key, idx, &n->in);
	if (ret)
		goto err_entry;

	hnat_out_key(key, t);
	ret = hnat_napt_hash_insert(h, TB_OUT_HASH, key, idx, &n->out);
	if (ret)
		goto err_in_hash;

	ret = hnat_tb_set_bit(h, TB_NAPT_VLD, idx, true);
	if (ret)
		goto err_out_hash;

	set_bit(idx, h->napt_used);

	return idx;

err_out_hash:
	hnat_napt_hash_remove(h, &n->out);
err_in_hash:
	hnat_napt_hash_remove(h, &n->in);
err_entry:
	hnat_tb_clear(h, TB_NAPT, idx);
err_pppoe:
	if (n->pppoe >= 0)
		hnat_pppoe_put(h, n->pppoe);
err_wan_dip:
	hnat_dip_put(h, n->wan_dip);
err_lan_dip:
	hnat_dip_put(h, n->lan_dip);
err_lan:
	hnat_lan_put(h, n->lan);
err_pub:
	hnat_pub_put(h, n->pub);
err_wan_vlan:
	hnat_vlan_put(h, n->wan_vlan);
err_lan_vlan:
	hnat_vlan_put(h, n->lan_vlan);
	return ret;
}

static void hnat_napt_del(struct sf19a2890_hnat *h, int idx)
{
	struct hnat_napt *n = &h->napt[idx];

	hnat_tb_set_bit(h, TB_NAPT_VLD, idx, false);
	hnat_napt_hash_remove(h, &n->out);
	hnat_napt_hash_remove(h, &n->in);
	hnat_tb_clear(h, TB_NAPT, idx);
	if (n->pppoe >= 0)
		hnat_pppoe_put(h, n->pppoe);
	hnat_dip_put(h, n->wan_dip);
	hnat_dip_put(h, n->lan_dip);
	hnat_lan_put(h, n->lan);
	hnat_pub_put(h, n->pub);
	hnat_vlan_put(h, n->wan_vlan);
	hnat_vlan_put(h, n->lan_vlan);
	clear_bit(idx, h->napt_used);
}

static void hnat_hw_init(struct sf19a2890_hnat *h)
{
	struct hnat_flow *fl;
	int tb, i, bkt;

	for (tb = 0; tb < TB_NUM; tb++)
		for (i = 0; i < hnat_tb_depth[tb]; i++)
			hnat_tb_clear(h, tb, i);

	bitmap_zero(h->napt_used, HNAT_NAPT_NUM);
	memset(h->dip, 0, sizeof(h->dip));
	memset(h->dmac, 0, sizeof(h->dmac));
	memset(h->rmac, 0, sizeof(h->rmac));
	memset(h->pppoe, 0, sizeof(h->pppoe));
	memset(h->vlan, 0, sizeof(h->vlan));
	memset(h->pub, 0, sizeof(h->pub));
	memset(h->lan, 0, sizeof(h->lan));

	hash_for_each(h->flows, bkt, fl, node)
		fl->in_hw = false;
	h->nhw = 0;

	writel(h->ctrl, h->base + HNAT_CTRL);
	for (i = 0; i < HNAT_LAN_NUM; i++)
		writel(0, h->base + HNAT_LAN_NET(i));
	writel(0, h->base + HNAT_LAN_MASK(0));
	writel(0, h->base + HNAT_LAN_MASK(4));
	hnat_load_cfg(h);

	writel(0x64, h->base + HNAT_REG4);
	writel(0x400000, h->base + HNAT_VLAN_MODE);
	writel(HNAT_TB_CFG_EN | HNAT_TB_CFG_CRC3_8005 | HNAT_TB_CFG_BE |
	       HNAT_TB_CFG_RD_CLR, h->base + HNAT_TB_CFG);
	writel(0x1500, h->base + HNAT_TB_RFC_TIMER);
	writel(0x1500, h->base + HNAT_TB_TFC_TIMER);
	writel(0x3, h->base + HNAT_TB_FC_CFG);
	writel(0x0, h->base + HNAT_TB_FC_CFG);
	writel(0x020f020f, h->base + HNAT_TB_BUF_THRESH);
	writel(375000000, h->base + HNAT_CLK(0));
	writel(6250000, h->base + HNAT_CLK(1));
	writel(0xffffffff, h->base + HNAT_CLK(2));
	writel(75000000, h->base + HNAT_CLK(3));

	h->running = true;
}

/* a GMAC reset also resets the engine and loses all entries */
static void hnat_check_reset(struct sf19a2890_hnat *h)
{
	if (h->running && readl(h->base + HNAT_CTRL) == h->ctrl)
		return;

	if (h->running) {
		dev_warn(h->dev, "engine was reset, reinitialising\n");
		h->resets++;
	}
	hnat_hw_init(h);
}

static int hnat_flow_program(struct sf19a2890_hnat *h, struct hnat_flow *fl)
{
	int idx;

	idx = hnat_napt_add(h, fl);
	if (idx < 0) {
		h->failed++;
		return idx;
	}

	fl->napt = idx;
	fl->in_hw = true;
	fl->hw_since = jiffies;
	h->nhw++;
	h->added++;

	return 0;
}

static void hnat_flow_unprogram(struct sf19a2890_hnat *h, struct hnat_flow *fl)
{
	if (!fl->in_hw)
		return;

	hnat_napt_del(h, fl->napt);
	fl->in_hw = false;
	h->nhw--;
}

static u32 hnat_tuple_hash(const struct hnat_tuple *t)
{
	return jhash(t, sizeof(*t), 0);
}

static struct hnat_flow *hnat_flow_find(struct sf19a2890_hnat *h,
					const struct hnat_tuple *t)
{
	struct hnat_flow *fl;

	hash_for_each_possible(h->flows, fl, node, hnat_tuple_hash(t))
		if (!memcmp(&fl->t, t, sizeof(*t)))
			return fl;

	return NULL;
}

static struct hnat_cookie *hnat_cookie_find(struct sf19a2890_hnat *h,
					    unsigned long cookie)
{
	struct hnat_cookie *c;

	hash_for_each_possible(h->cookies, c, node, cookie)
		if (c->cookie == cookie)
			return c;

	return NULL;
}

static void hnat_flow_free(struct sf19a2890_hnat *h, struct hnat_flow *fl)
{
	int dir;

	hnat_flow_unprogram(h, fl);
	for (dir = 0; dir < 2; dir++)
		if (fl->have[dir])
			hash_del(&fl->cookie[dir].node);
	hash_del(&fl->node);
	kfree(fl);
	h->nflows--;
}

static int hnat_port_vid(struct sf19a2890_hnat *h, struct net_device *dev,
			 u16 *vid)
{
	struct net_device *br;
	struct dsa_port *dp;

	dp = dsa_port_from_netdev(dev);
	if (IS_ERR(dp))
		return -EOPNOTSUPP;

	if (dp->cpu_dp->conduit != h->ndev ||
	    dp->cpu_dp->tag_ops->proto != DSA_TAG_PROTO_MXL_GSW1XX_8021Q)
		return -EOPNOTSUPP;

	br = dsa_port_bridge_dev_get(dp);
	if (br) {
		if (br_vlan_enabled(br))
			return -EOPNOTSUPP;
		*vid = dsa_tag_8021q_bridge_vid(dsa_port_bridge_num_get(dp));
	} else {
		*vid = dsa_tag_8021q_standalone_vid(dp);
	}

	return 0;
}

static int hnat_find_ifa(struct net_device *dev, __be32 addr, u32 *net,
			 u8 *plen)
{
	struct net_device *upper = netdev_master_upper_dev_get_rcu(dev);
	struct in_device *in_dev = __in_dev_get_rcu(upper ?: dev);
	const struct in_ifaddr *ifa;

	if (!in_dev)
		return -ENOENT;

	in_dev_for_each_ifa_rcu(ifa, in_dev) {
		if (inet_ifa_match(addr, ifa)) {
			*net = ntohl(ifa->ifa_address & ifa->ifa_mask);
			*plen = ifa->ifa_prefixlen;
			return 0;
		}
	}

	return -ENOENT;
}

/*
 * The public address can sit on a device stacked on the port, like a PPPoE
 * interface, so look it up by the local address itself.
 */
static int hnat_find_local(struct net *net, __be32 addr, u8 *plen)
{
	struct net_device *dev = __ip_dev_find(net, addr, false);
	const struct in_ifaddr *ifa;
	struct in_device *in_dev;

	in_dev = dev ? __in_dev_get_rcu(dev) : NULL;
	if (!in_dev)
		return -ENOENT;

	in_dev_for_each_ifa_rcu(ifa, in_dev) {
		if (ifa->ifa_local == addr) {
			*plen = ifa->ifa_prefixlen;
			return 0;
		}
	}

	return -ENOENT;
}

static void hnat_mangle_eth(const struct flow_action_entry *act, u8 *eth)
{
	const u8 *src = (const u8 *)&act->mangle.val;
	u8 *dst = eth + act->mangle.offset;

	if (act->mangle.offset > 8)
		return;

	if (act->mangle.mask == 0xffff) {
		src += 2;
		dst += 2;
	}

	memcpy(dst, src, act->mangle.mask ? 2 : 4);
}

static int hnat_parse_rule(struct sf19a2890_hnat *h, struct flow_cls_offload *f,
			   struct hnat_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct net_device *in_dev, *out_dev = NULL;
	struct flow_match_ipv4_addrs addrs;
	struct flow_match_control control;
	struct flow_match_basic basic;
	struct flow_match_ports ports;
	struct flow_match_meta meta;
	struct flow_action_entry *act;
	__be32 new_saddr = 0, new_daddr = 0;
	__be16 new_sport = 0, new_dport = 0;
	int i, ret;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS) ||
	    flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN))
		return -EOPNOTSUPP;

	flow_rule_match_control(rule, &control);
	if (control.key->addr_type != FLOW_DISSECTOR_KEY_IPV4_ADDRS)
		return -EOPNOTSUPP;

	flow_rule_match_basic(rule, &basic);
	if (basic.key->n_proto != htons(ETH_P_IP))
		return -EOPNOTSUPP;

	switch (basic.key->ip_proto) {
	case IPPROTO_TCP:
		r->t.proto = 0;
		break;
	case IPPROTO_UDP:
		r->t.proto = 1;
		break;
	default:
		return -EOPNOTSUPP;
	}

	flow_rule_match_ipv4_addrs(rule, &addrs);
	flow_rule_match_ports(rule, &ports);
	flow_rule_match_meta(rule, &meta);

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			switch (act->mangle.htype) {
			case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
				hnat_mangle_eth(act, r->eth);
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
				if (act->mangle.offset == offsetof(struct iphdr, saddr))
					new_saddr = (__force __be32)act->mangle.val;
				else if (act->mangle.offset == offsetof(struct iphdr, daddr))
					new_daddr = (__force __be32)act->mangle.val;
				else
					return -EOPNOTSUPP;
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
			case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
				if (act->mangle.mask == ~htonl(0xffff0000))
					new_sport = htons(ntohl(act->mangle.val) >> 16);
				else if (act->mangle.mask == ~htonl(0xffff))
					new_dport = htons(ntohl(act->mangle.val) & 0xffff);
				else
					return -EOPNOTSUPP;
				break;
			default:
				return -EOPNOTSUPP;
			}
			break;
		case FLOW_ACTION_CSUM:
			break;
		case FLOW_ACTION_REDIRECT:
			out_dev = act->dev;
			break;
		case FLOW_ACTION_PPPOE_PUSH:
			if (r->pppoe)
				return -EOPNOTSUPP;
			r->pppoe = true;
			r->pppoe_sid = act->pppoe.sid;
			break;
		default:
			return -EOPNOTSUPP;
		}
	}

	if (new_saddr && !new_daddr) {
		r->dir = DIR_OUT;
		r->t.lan_ip = addrs.key->src;
		r->t.lan_port = ports.key->src;
		r->t.remote_ip = addrs.key->dst;
		r->t.remote_port = ports.key->dst;
		r->t.pub_ip = new_saddr;
		r->t.pub_port = new_sport ?: ports.key->src;
	} else if (new_daddr && !new_saddr) {
		r->dir = DIR_IN;
		r->t.remote_ip = addrs.key->src;
		r->t.remote_port = ports.key->src;
		r->t.pub_ip = addrs.key->dst;
		r->t.pub_port = ports.key->dst;
		r->t.lan_ip = new_daddr;
		r->t.lan_port = new_dport ?: ports.key->dst;
	} else {
		return -EOPNOTSUPP;
	}

	/*
	 * Only the WAN side can be PPPoE. The flowtable doesn't describe the
	 * PPPoE header on the inbound rule, it comes from the outbound one.
	 */
	if (!out_dev || (r->pppoe && r->dir != DIR_OUT))
		return -EOPNOTSUPP;

	/*
	 * The engine adds the PPPoE header without checking the MTU, so full
	 * size packets would get dropped instead of fragmented. TCP is kept
	 * small enough by MSS clamping, UDP stays in software.
	 */
	if (r->pppoe && r->t.proto != 0)
		return -EOPNOTSUPP;

	rcu_read_lock();

	ret = -EOPNOTSUPP;
	in_dev = dev_get_by_index_rcu(dev_net(h->ndev), meta.key->ingress_ifindex);
	if (!in_dev)
		goto out;

	ret = hnat_port_vid(h, in_dev, &r->in_vid);
	if (!ret)
		ret = hnat_port_vid(h, out_dev, &r->out_vid);
	if (ret)
		goto out;

	ret = hnat_find_ifa(r->dir == DIR_OUT ? in_dev : out_dev,
			    r->t.lan_ip, &r->lan_net, &r->lan_plen);
	if (ret)
		goto out;

	ret = hnat_find_local(dev_net(h->ndev), r->t.pub_ip, &r->wan_plen);
out:
	rcu_read_unlock();
	return ret;
}

static int hnat_cls_replace(struct sf19a2890_hnat *h,
			    struct flow_cls_offload *f)
{
	struct hnat_rule r = {};
	struct hnat_cookie *c;
	struct hnat_flow *fl;
	int ret;

	c = hnat_cookie_find(h, f->cookie);
	if (c) {
		fl = c->flow;
		if (!fl->in_hw && fl->have[DIR_OUT] && fl->have[DIR_IN])
			return hnat_flow_program(h, fl);
		return 0;
	}

	ret = hnat_parse_rule(h, f, &r);
	if (ret)
		return ret;

	fl = hnat_flow_find(h, &r.t);
	if (!fl) {
		fl = kzalloc(sizeof(*fl), GFP_KERNEL);
		if (!fl)
			return -ENOMEM;
		fl->t = r.t;
		hash_add(h->flows, &fl->node, hnat_tuple_hash(&fl->t));
		h->nflows++;
	}

	if (fl->have[r.dir]) {
		hash_del(&fl->cookie[r.dir].node);
		fl->have[r.dir] = false;
	}

	fl->lan_net = r.lan_net;
	fl->lan_plen = r.lan_plen;
	fl->wan_plen = r.wan_plen;
	if (r.dir == DIR_OUT) {
		ether_addr_copy(fl->remote_mac, r.eth);
		ether_addr_copy(fl->wan_smac, r.eth + ETH_ALEN);
		fl->lan_vid = r.in_vid;
		fl->wan_vid = r.out_vid;
		fl->pppoe = r.pppoe;
		fl->pppoe_sid = r.pppoe_sid;
	} else {
		ether_addr_copy(fl->lan_mac, r.eth);
		ether_addr_copy(fl->lan_smac, r.eth + ETH_ALEN);
		fl->wan_vid = r.in_vid;
		fl->lan_vid = r.out_vid;
	}

	c = &fl->cookie[r.dir];
	c->cookie = f->cookie;
	c->flow = fl;
	c->dir = r.dir;
	hash_add(h->cookies, &c->node, f->cookie);
	fl->have[r.dir] = true;

	if (fl->have[DIR_OUT] && fl->have[DIR_IN] && !fl->in_hw)
		return hnat_flow_program(h, fl);

	return 0;
}

static int hnat_cls_destroy(struct sf19a2890_hnat *h,
			    struct flow_cls_offload *f)
{
	struct hnat_cookie *c = hnat_cookie_find(h, f->cookie);
	struct hnat_flow *fl;

	if (!c)
		return 0;

	fl = c->flow;
	hnat_flow_unprogram(h, fl);
	hash_del(&c->node);
	fl->have[c->dir] = false;

	if (!fl->have[DIR_OUT] && !fl->have[DIR_IN]) {
		hash_del(&fl->node);
		kfree(fl);
		h->nflows--;
	}

	return 0;
}

static int hnat_cls_stats(struct sf19a2890_hnat *h,
			  struct flow_cls_offload *f)
{
	struct hnat_cookie *c = hnat_cookie_find(h, f->cookie);
	struct hnat_flow *fl;

	if (!c)
		return 0;

	fl = c->flow;
	if (fl->in_hw && time_after(jiffies, fl->hw_since + HNAT_HW_TIMEOUT)) {
		hnat_flow_unprogram(h, fl);
		h->expired++;
	}

	flow_stats_update(&f->stats, 0, 0, 0, fl->hw_since,
			  FLOW_ACTION_HW_STATS_DELAYED);

	return 0;
}

static int hnat_setup_tc_block_cb(enum tc_setup_type type, void *type_data,
				  void *cb_priv)
{
	struct sf19a2890_hnat *h = cb_priv;
	struct flow_cls_offload *f = type_data;
	int ret;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	mutex_lock(&h->lock);
	hnat_check_reset(h);

	switch (f->command) {
	case FLOW_CLS_REPLACE:
		ret = hnat_cls_replace(h, f);
		break;
	case FLOW_CLS_DESTROY:
		ret = hnat_cls_destroy(h, f);
		break;
	case FLOW_CLS_STATS:
		ret = hnat_cls_stats(h, f);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	mutex_unlock(&h->lock);

	return ret;
}

static int hnat_setup_ft(struct sf19a2890_hnat *h, struct net_device *ndev,
			 struct flow_block_offload *f)
{
	struct flow_block_cb *block_cb;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &h->block_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block, hnat_setup_tc_block_cb, h);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}

		block_cb = flow_block_cb_alloc(hnat_setup_tc_block_cb, h, h, NULL);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);

		mutex_lock(&h->lock);
		h->ndev = ndev;
		hnat_check_reset(h);
		mutex_unlock(&h->lock);

		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &h->block_list);
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, hnat_setup_tc_block_cb, h);
		if (!block_cb)
			return -ENOENT;

		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

int sf19a2890_hnat_setup_tc(struct sf19a2890_hnat *h, struct net_device *ndev,
			    enum tc_setup_type type, void *type_data)
{
	if (type == TC_SETUP_FT)
		return hnat_setup_ft(h, ndev, type_data);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(sf19a2890_hnat_setup_tc);

static int hnat_debugfs_show(struct seq_file *m, void *v)
{
	struct sf19a2890_hnat *h = m->private;
	unsigned int pppoe = 0;
	u32 cfg;
	int i;

	mutex_lock(&h->lock);

	for (i = 0; i < HNAT_PPPOE_NUM; i++)
		pppoe += h->pppoe[i].refs;

	seq_printf(m, "flows:    %u\n", h->nflows);
	seq_printf(m, "in hw:    %u\n", h->nhw);
	seq_printf(m, "pppoe:    %u\n", pppoe);
	seq_printf(m, "added:    %lu\n", h->added);
	seq_printf(m, "failed:   %lu\n", h->failed);
	seq_printf(m, "expired:  %lu\n", h->expired);
	seq_printf(m, "resets:   %lu\n", h->resets);

	if (h->running) {
		cfg = readl(h->base + HNAT_CFG);
		writel(cfg & ~HNAT_CFG_SNAPSHOT, h->base + HNAT_CFG);
		writel(cfg | HNAT_CFG_SNAPSHOT, h->base + HNAT_CFG);

		seq_printf(m, "rx:       %u\n", readl(h->base + HNAT_CNT_RX));
		seq_printf(m, "to host:  %u\n", readl(h->base + HNAT_CNT_TO_HOST));
		seq_printf(m, "forward:  %u\n", readl(h->base + HNAT_CNT_FWD));
		seq_printf(m, "fwd drop: %u\n", readl(h->base + HNAT_CNT_FWD_DROP));
		seq_printf(m, "rx error: %u\n", readl(h->base + HNAT_CNT_RX_ERR));
	}

	mutex_unlock(&h->lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hnat_debugfs);

struct sf19a2890_hnat *sf19a2890_hnat_init(struct device *dev,
					   void __iomem *base)
{
	struct sf19a2890_hnat *h;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return ERR_PTR(-ENOMEM);

	h->dev = dev;
	h->base = base;
	h->ctrl = HNAT_CTRL_INIT | HNAT_CTRL_SYMMETRIC;
	mutex_init(&h->lock);
	hash_init(h->flows);
	hash_init(h->cookies);
	INIT_LIST_HEAD(&h->block_list);

	h->debugfs = debugfs_create_file("sf19a2890-hnat", 0400, NULL, h,
					 &hnat_debugfs_fops);

	return h;
}
EXPORT_SYMBOL_GPL(sf19a2890_hnat_init);

void sf19a2890_hnat_deinit(struct sf19a2890_hnat *h)
{
	struct hlist_node *tmp;
	struct hnat_flow *fl;
	int bkt;

	debugfs_remove(h->debugfs);

	mutex_lock(&h->lock);
	hash_for_each_safe(h->flows, bkt, tmp, fl, node)
		hnat_flow_free(h, fl);
	mutex_unlock(&h->lock);
}
EXPORT_SYMBOL_GPL(sf19a2890_hnat_deinit);

MODULE_DESCRIPTION("Siflower SF19A2890 GMAC hardware NAT offload");
MODULE_LICENSE("GPL");
