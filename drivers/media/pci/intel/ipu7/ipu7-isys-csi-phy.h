/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 - 2024 Intel Corporation
 */

#ifndef IPU7_ISYS_CSI_PHY_H
#define IPU7_ISYS_CSI_PHY_H

struct ipu7_isys;

#define PHY_MODE_DPHY		0
#define PHY_MODE_CPHY		1

int ipu7_isys_csi_ctrl_dids_config(struct ipu7_isys *isys, u32 id,
				   u8 vc, u8 dt);
int ipu7_isys_csi_phy_powerup(struct ipu7_isys_csi2 *csi2);
void ipu7_isys_csi_phy_powerdown(struct ipu7_isys_csi2 *csi2);
#endif
