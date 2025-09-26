/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2016-2025 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef MEDIA_INTEL_IPU_ACPI_H
#define MEDIA_INTEL_IPU_ACPI_H

#include "ipu7-isys.h"

#define MAX_ACPI_SENSOR_NUM	4
#define MAX_ACPI_I2C_NUM	12
#define MAX_ACPI_GPIO_NUM	12

#define GPIO_RESET		0x0

#define IPU7_SPDATA_GPIO_NUM 	4
#define IPU7_SPDATA_IRQ_PIN_NAME_LEN 16

enum connection_type {
	TYPE_DIRECT,
	TYPE_SERDES
};

/* Data representation as it is in ACPI SSDB buffer */
struct sensor_bios_data_packed {
	u8 version;
	u8 sku;
	u8 guid_csi2[16];
	u8 devfunction;
	u8 bus;
	u32 dphylinkenfuses;
	u32 clockdiv;
	u8 link;
	u8 lanes;
	u32 csiparams[10];
	u32 maxlanespeed;
	u8 sensorcalibfileidx;
	u8 sensorcalibfileidxInMBZ[3];
	u8 romtype;
	u8 vcmtype;
	u8 platforminfo;
	u8 platformsubinfo;
	u8 flash;
	u8 privacyled;
	u8 degree;
	u8 mipilinkdefined;
	u32 mclkspeed;
	u8 controllogicid;
	u8 mipidataformat;
	u8 siliconversion;
	u8 customerid;
	u8 mclkport;
	u8 pmicpos;
	u8 voltagerail;
	u8 pprval;
	u8 pprunit;
	u8 flashid;
	u8 phyconfig;
	u8 reserved2[7];
} __attribute__((__packed__));

struct ipu_i2c_info {
	unsigned short bus;
	unsigned short addr;
	char bdf[32];
};

/* Fields needed by ipu driver */
/* Each I2C client can have 12 device */
struct sensor_bios_data {
	struct device *dev;
	u8 link;
	u8 lanes;
	u8 vcmtype;
	u8 flash;
	u8 degree;
	u8 mclkport;
	u32 mclkspeed;
	u16 xshutdown;
	u8 controllogicid;
	u8 pprval;
	u8 pprunit;
	struct ipu_i2c_info i2c[MAX_ACPI_I2C_NUM];
	u64 i2c_num;
	u8 bus_type;
};

struct control_logic_data_packed {
	u8 version;
	u8 controllogictype;
	u8 controllogicid;
	u8 sensorcardsku;
	u8 inputclk;
	u8 platformid;
	u8 subplatformid;
	u8 customerid;
	u8 wled1_maxflashcurrent;
	u8 wled1_maxtorchcurrent;
	u8 wled2_maxflashcurrent;
	u8 wled2_maxtorchcurrent;
	u8 wled1_type;
	u8 wled2_type;
	u8 pch_clk_src;
	u8 reserved2[17];
} __attribute__((__packed__));

struct ipu_gpio_info {
	unsigned short init_state;
	unsigned short pin;
	unsigned short func;
	bool valid;
};

/* Each I2C client linked to 1 set of CTL Logic */
struct control_logic_data {
	struct device *dev;
	u8 id;
	u8 type;
	u8 sku;
	u64 gpio_num;
	struct ipu_gpio_info gpio[MAX_ACPI_GPIO_NUM];
	bool completed;
};

int ipu_get_acpi_devices(void **spdata);

struct ipu7_isys_subdev_pdata *get_built_in_pdata(void);

int ipu_acpi_get_cam_data(struct device *dev,
				struct sensor_bios_data *sensor);

int ipu_acpi_get_dep_data(struct device *dev,
				struct control_logic_data *ctl_data);

int ipu_acpi_get_control_logic_data(struct device *dev,
				struct control_logic_data **ctl_data);

struct ipu_i2c_helper {
	int (*fn)(struct device *dev, void *priv,
		struct ipu7_isys_csi2_config *csi2,
		bool reprobe);
	void *driver_data;
};

struct ipu_camera_module_data {
	struct list_head list;
	struct ipu7_isys_subdev_info sd;
	struct ipu7_isys_csi2_config csi2;
	unsigned int ext_clk;
	void *pdata; /* Ptr to generated platform data*/
	void *priv; /* Private for specific subdevice */
};

struct ipu_acpi_devices {
	const char *hid_name;
	const char *real_driver;
	int (*get_platform_data)(struct device *dev,
				 struct ipu_camera_module_data *data,
				 void *priv,
				 size_t size,
				 enum connection_type type,
				 const char *sensor_name,
				 const char *serdes_name,
				 const char *hid_name,
				 int sensor_physical_addr,
				 int link_freq);
	void *priv_data;
	size_t priv_size;
	enum connection_type connect;
	const char *serdes_name;
	int sensor_physical_addr;
	int link_freq; /* in mbps */
};

#endif
