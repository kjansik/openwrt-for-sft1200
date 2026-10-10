/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __DWMAC_SF19A2890_HNAT_H__
#define __DWMAC_SF19A2890_HNAT_H__

#include <linux/device.h>
#include <linux/netdevice.h>

struct sf19a2890_hnat;

struct sf19a2890_hnat *sf19a2890_hnat_init(struct device *dev,
					   void __iomem *base);
void sf19a2890_hnat_deinit(struct sf19a2890_hnat *hnat);
int sf19a2890_hnat_setup_tc(struct sf19a2890_hnat *hnat,
			    struct net_device *ndev,
			    enum tc_setup_type type, void *type_data);

#endif
