/*
 * Amlogic GxTV
 * HDMI RX
 * Copyright (C) 2010 Amlogic, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the named License,
 * or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */


#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/major.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/amlogic/tvin/tvin.h>

/* Local include */
#include "hdmirx_drv.h"
#include "hdmi_rx_reg.h"

/*------------------------marco define------------------------------*/
#define SCRAMBLE_SEL 1
#define HYST_HDMI_TO_DVI 5
/* must = 0, other agilent source fail */
#define HYST_DVI_TO_HDMI 0
#define GCP_GLOBAVMUTE_EN 1 /* ag506 must clear this bit */
#define EDID_CLK_DIV 9 /* sys clk/(9+1) = 20M */
#define HDCP_KEY_WR_TRIES		(5)
#define __asmeq(x, y)  ".ifnc " x "," y " ; .err ; .endif\n\t"

/*------------------------marco define------------------------------*/

/*------------------------variable define------------------------------*/
static DEFINE_SPINLOCK(reg_rw_lock);
/*should enable fast switching, since some devices in non-current port
will suspend because of RxSense = 0, such as xiaomi-mtk box*/
static bool phy_fast_switching = true;
static bool phy_fsm_enhancement = true;
unsigned int last_clk_rate;

/* SNPS suggest to use the previous setting 0x3f when handle eq issues to
make clk_stable bit more stable(=1),but 0x3f may misjudge 46.25~92.5
TMDSCLK as 25~46.25M TMDSCLK,pll_rate&REQUESTCLK will become
not correct.
so revert the setting to the default value 0x6 according to the PHY spec */
static uint8_t phy_lock_thres = 0x6;
static uint32_t phy_cfg_clk = 24000;
static uint32_t modet_clk = 24000;

/* top_irq_en bit[16:13] hdcp_sts */
int top_intr_maskn_value = 1;
bool hdcp_enable = 1;

static int acr_mode;
MODULE_PARM_DESC(acr_mode, "\n acr_mode\n");
module_param(acr_mode, int, 0664);

static int auto_aclk_mute = 2;
MODULE_PARM_DESC(auto_aclk_mute, "\n auto_aclk_mute\n");
module_param(auto_aclk_mute, int, 0664);

static int aud_avmute_en = 1;
MODULE_PARM_DESC(aud_avmute_en, "\n aud_avmute_en\n");
module_param(aud_avmute_en, int, 0664);

int aud_mute_sel;
MODULE_PARM_DESC(aud_mute_sel, "\n aud_mute_sel\n");
module_param(aud_mute_sel, int, 0664);

static int force_clk_rate;
MODULE_PARM_DESC(force_clk_rate, "\n force_clk_rate\n");
module_param(force_clk_rate, int, 0664);

int md_ists_en = VIDEO_MODE;
MODULE_PARM_DESC(md_ists_en, "\n rx_md_ists_en\n");
module_param(md_ists_en, int, 0664);

int pdec_ists_en;/* = AVI_CKS_CHG | DVIDET | DRM_CKS_CHG | DRM_RCV_EN;*/
MODULE_PARM_DESC(pdec_ists_en, "\n pdec_ists_en\n");
module_param(pdec_ists_en, int, 0664);

/* bit5 pll_lck_chg_en */
/* bit6 clk_change_en */
int hdmi_ists_en = AKSV_RCV;
MODULE_PARM_DESC(hdmi_ists_en, "\n hdmi_ists_en\n");
module_param(hdmi_ists_en, int, 0664);

uint32_t packet_fifo_cfg;
module_param(packet_fifo_cfg, int, 0664);
MODULE_PARM_DESC(packet_fifo_cfg, "packet_fifo_cfg");

int pd_fifo_start_cnt = K_PKT_FIFO_START;
module_param(pd_fifo_start_cnt, int, 0664);
MODULE_PARM_DESC(pd_fifo_start_cnt, "pd_fifo_start_cnt");

/* Controls equalizer reference voltage. */
static int eq_ref_voltage = 0x1ea;
module_param(eq_ref_voltage, int, 0664);
MODULE_PARM_DESC(eq_ref_voltage, "eq_ref_voltage");

#ifdef HDCP22_ENABLE
int hdcp22_on;
MODULE_PARM_DESC(hdcp22_on, "\n hdcp22_on\n");
module_param(hdcp22_on, int, 0664);

int force_hdcp14_en;
MODULE_PARM_DESC(force_hdcp14_en, "\n force_hdcp14_en\n");
module_param(force_hdcp14_en, int, 0664);
#endif

/*------------------------variable define end------------------------------*/

/**
 * Read data from HDMI RX CTRL
 * @param[in] addr register address
 * @return data read value
 */
uint32_t hdmirx_rd_dwc(uint16_t addr)
{
	ulong flags;
	int data;
	unsigned long dev_offset = 0x10;
	spin_lock_irqsave(&reg_rw_lock, flags);
	wr_reg(MAP_ADDR_MODULE_TOP, hdmirx_addr_port | dev_offset, addr);
	data = rd_reg(MAP_ADDR_MODULE_TOP, hdmirx_data_port | dev_offset);
	spin_unlock_irqrestore(&reg_rw_lock, flags);
	return data;
}

uint32_t hdmirx_rd_bits_dwc(uint16_t addr, uint32_t mask)
{
	return get(hdmirx_rd_dwc(addr), mask);
}

/**
 * Write data to HDMI RX CTRL
 * @param[in] addr register address
 * @param[in] data new register value
 */
void hdmirx_wr_dwc(uint16_t addr, uint32_t data)
{
	ulong flags;
	unsigned long dev_offset = 0x10;
	spin_lock_irqsave(&reg_rw_lock, flags);
	wr_reg(MAP_ADDR_MODULE_TOP, hdmirx_addr_port | dev_offset, addr);
	wr_reg(MAP_ADDR_MODULE_TOP, hdmirx_data_port | dev_offset, data);
	spin_unlock_irqrestore(&reg_rw_lock, flags);
}

void hdmirx_wr_bits_dwc(uint16_t addr, uint32_t mask, uint32_t value)
{
	hdmirx_wr_dwc(addr, set(hdmirx_rd_dwc(addr), mask, value));
}

/**
 * Read data from HDMI RX phy
 * @param[in] addr register address
 * @return data read value
 */
uint16_t hdmirx_rd_phy(uint8_t reg_address)
{
	int cnt = 0;
	/* hdmirx_wr_dwc(DWC_I2CM_PHYG3_SLAVE, 0x39); */
	hdmirx_wr_dwc(DWC_I2CM_PHYG3_ADDRESS, reg_address);
	hdmirx_wr_dwc(DWC_I2CM_PHYG3_OPERATION, 0x02);
	do {
		if ((cnt % 10) == 0) {
			/* wait i2cmpdone */
			if (hdmirx_rd_dwc(DWC_HDMI_ISTS)&(1<<28)) {
				hdmirx_wr_dwc(DWC_HDMI_ICLR, 1<<28);
				break;
			}
		}
		cnt++;
		if (cnt > 50000) {
			rx_pr("[HDMIRX err]: %s(%x,%x) timeout\n",
				__func__, 0x39, reg_address);
			break;
		}
	} while (1);

	return (uint16_t)(hdmirx_rd_dwc(DWC_I2CM_PHYG3_DATAI));
}

uint16_t hdmirx_rd_bits_phy(uint16_t addr, uint32_t mask)
{
	return get(hdmirx_rd_phy(addr), mask);
}

/**
 * Write data to HDMI RX phy
 * @param[in] addr register address
 * @param[in] data new register value
 */
int hdmirx_wr_phy(uint8_t reg_address, uint16_t data)
{
	int error = 0;
	int cnt = 0;
	/* hdmirx_wr_dwc(DWC_I2CM_PHYG3_SLAVE, 0x39); */
	hdmirx_wr_dwc(DWC_I2CM_PHYG3_ADDRESS, reg_address);
	hdmirx_wr_dwc(DWC_I2CM_PHYG3_DATAO, data);
	hdmirx_wr_dwc(DWC_I2CM_PHYG3_OPERATION, 0x01);

	do {
		/* wait i2cmpdone */
		if ((cnt % 10) == 0) {
			if (hdmirx_rd_dwc(DWC_HDMI_ISTS)&(1<<28)) {
				hdmirx_wr_dwc(DWC_HDMI_ICLR, 1<<28);
				break;
			}
		}
		cnt++;
		if (cnt > 50000) {
			error = -1;
			if (log_level & ERR_LOG) {
				rx_pr("[error]:(%x,%x,%x)timeout\n",
					__func__, 0x39, reg_address, data);
			}
			break;
		}
	} while (1);
	return error;
}

int hdmirx_wr_bits_phy(uint16_t addr, uint32_t mask, uint32_t value)
{
	return hdmirx_wr_phy(addr, set(hdmirx_rd_phy(addr), mask, value));
}

unsigned long hdmirx_rd_top(unsigned long addr)
{
	ulong flags;
	int data;
	unsigned long dev_offset = 0;
	spin_lock_irqsave(&reg_rw_lock, flags);
	wr_reg(MAP_ADDR_MODULE_TOP, hdmirx_addr_port | dev_offset, addr);
	wr_reg(MAP_ADDR_MODULE_TOP, hdmirx_addr_port | dev_offset, addr);
	data = rd_reg(MAP_ADDR_MODULE_TOP, hdmirx_data_port | dev_offset);
	spin_unlock_irqrestore(&reg_rw_lock, flags);
	return data;
} /* hdmirx_rd_TOP */

uint32_t hdmirx_rd_bits_top(uint16_t addr, uint32_t mask)
{
	return get(hdmirx_rd_top(addr), mask);
}

void hdmirx_wr_top(unsigned long addr, unsigned long data)
{
	ulong flags;
	unsigned long dev_offset = 0;
	spin_lock_irqsave(&reg_rw_lock, flags);
	wr_reg(MAP_ADDR_MODULE_TOP, hdmirx_addr_port | dev_offset, addr);
	wr_reg(MAP_ADDR_MODULE_TOP, hdmirx_data_port | dev_offset, data);
	spin_unlock_irqrestore(&reg_rw_lock, flags);
}

void hdmirx_wr_bits_top(uint16_t addr, uint32_t mask, uint32_t value)
{
	hdmirx_wr_top(addr, set(hdmirx_rd_top(addr), mask, value));
}

#ifdef HDCP22_ENABLE
void rx_hdcp22_wr_only(uint32_t addr, uint32_t data)
{
	ulong flags;
	spin_lock_irqsave(&reg_rw_lock, flags);
	wr_reg(MAP_ADDR_MODULE_HDMIRX_CAPB3,
	reg_maps[rx.chip_id][MAP_ADDR_MODULE_HDMIRX_CAPB3].phy_addr | addr,
	data);
	spin_unlock_irqrestore(&reg_rw_lock, flags);
}

uint32_t rx_hdcp22_rd(uint32_t addr)
{
	uint32_t data;
	ulong flags;
	spin_lock_irqsave(&reg_rw_lock, flags);
	data = rd_reg(MAP_ADDR_MODULE_HDMIRX_CAPB3,
	reg_maps[rx.chip_id][MAP_ADDR_MODULE_HDMIRX_CAPB3].phy_addr | addr);
	spin_unlock_irqrestore(&reg_rw_lock, flags);
	return data;
}

void rx_hdcp22_rd_check(uint32_t addr, uint32_t exp_data, uint32_t mask)
{
	uint32_t rd_data;
	rd_data = rx_hdcp22_rd(addr);
	if ((rd_data | mask) != (exp_data | mask))
		rx_pr("addr=0x%02x rd_data=0x%08x\n", addr, rd_data);
}

void rx_hdcp22_wr(uint32_t addr, uint32_t data)
{
	rx_hdcp22_wr_only(addr, data);
	rx_hdcp22_rd_check(addr, data, 0);
}

void rx_hdcp22_wr_reg(uint32_t addr, uint32_t data)
{
	rx_sec_reg_write((unsigned *)(unsigned long)
	(reg_maps[rx.chip_id][MAP_ADDR_MODULE_HDMIRX_CAPB3].phy_addr + addr),
	data);
}

uint32_t rx_hdcp22_rd_reg(uint32_t addr)
{
	return (uint32_t)rx_sec_reg_read((unsigned *)(unsigned long)
	(reg_maps[rx.chip_id][MAP_ADDR_MODULE_HDMIRX_CAPB3].phy_addr + addr));
}

uint32_t rx_hdcp22_rd_reg_bits(uint16_t addr, uint32_t mask)
{
	return get(rx_hdcp22_rd_reg(addr), mask);
}

void rx_hdcp22_wr_reg_bits(uint16_t addr, uint32_t mask, uint32_t value)
{
	rx_hdcp22_wr_reg(addr, set(rx_hdcp22_rd_reg(addr), mask, value));
}

void hdcp22_wr_top(uint32_t addr, uint32_t data)
{
	sec_top_write((unsigned *)(unsigned long)addr, data);
}

uint32_t hdcp22_rd_top(uint32_t addr)
{
	return (uint32_t)sec_top_read((unsigned *)(unsigned long)addr);
}

void sec_top_write(unsigned *addr, unsigned value)
{
	register long x0 asm("x0") = 0x8200001e;
	register long x1 asm("x1") = (unsigned long)addr;
	register long x2 asm("x2") = value;
	asm volatile(
		__asmeq("%0", "x0")
		__asmeq("%1", "x1")
		__asmeq("%2", "x2")
		"smc #0\n"
		: : "r"(x0), "r"(x1), "r"(x2)
	);
}

unsigned sec_top_read(unsigned *addr)
{
	register long x0 asm("x0") = 0x8200001d;
	register long x1 asm("x1") = (unsigned long)addr;
	asm volatile(
		__asmeq("%0", "x0")
		__asmeq("%1", "x1")
		"smc #0\n"
		: "+r"(x0) : "r"(x1)
	);
	return (unsigned)(x0&0xffffffff);
}

void rx_sec_reg_write(unsigned *addr, unsigned value)
{
	register long x0 asm("x0") = 0x8200002f;
	register long x1 asm("x1") = (unsigned long)addr;
	register long x2 asm("x2") = value;
	asm volatile(
		__asmeq("%0", "x0")
		__asmeq("%1", "x1")
		__asmeq("%2", "x2")
		"smc #0\n"
		: : "r"(x0), "r"(x1), "r"(x2)
	);
}

unsigned rx_sec_reg_read(unsigned *addr)
{
	register long x0 asm("x0") = 0x8200001f;
	register long x1 asm("x1") = (unsigned long)addr;
	asm volatile(
		__asmeq("%0", "x0")
		__asmeq("%1", "x1")
		"smc #0\n"
		: "+r"(x0) : "r"(x1)
	);
	return (unsigned)(x0&0xffffffff);
}

unsigned rx_sec_set_duk(void)
{
	register long x0 asm("x0") = 0x8200002e;
	asm volatile(
		__asmeq("%0", "x0")
		"smc #0\n"
		: "+r"(x0)
	);
	return (unsigned)(x0&0xffffffff);
}

#endif

void hdmirx_phy_pddq(int enable)
{
	hdmirx_wr_bits_dwc(DWC_SNPS_PHYG3_CTRL,
		MSK(1, 1), enable);
}

/**************************
    hw functions
***************************/

void hdmirx_wr_ctl_port(unsigned int offset, unsigned long data)
{
	ulong flags;
	spin_lock_irqsave(&reg_rw_lock, flags);
	wr_reg(MAP_ADDR_MODULE_TOP, hdmirx_ctrl_port+offset, data);
	spin_unlock_irqrestore(&reg_rw_lock, flags);
}

void hdmirx_irq_enable(bool enable)
{
	int data32 = 0;

	if (enable) {
		if (is_meson_txlx_cpu()) {
			data32 |= 1 << 31; /* DRC_CKS_CHG */
			data32 |= 1 << 30; /* DRC_RCV */
			data32 |= 0 << 29; /* AUD_TYPE_CHG */
			data32 |= 0 << 28; /* DVI_DET */
			data32 |= 1 << 27; /* VSI_CKS_CHG */
			data32 |= 0 << 26; /* GMD_CKS_CHG */
			data32 |= 0 << 25; /* AIF_CKS_CHG */
			data32 |= 1 << 24; /* AVI_CKS_CHG */
			data32 |= 0 << 23; /* ACR_N_CHG */
			data32 |= 0 << 22; /* ACR_CTS_CHG */
			data32 |= 1 << 21; /* GCP_AV_MUTE_CHG */
			data32 |= 0 << 20; /* GMD_RCV */
			data32 |= 0 << 19; /* AIF_RCV */
			data32 |= 0 << 18; /* AVI_RCV */
			data32 |= 0 << 17; /* ACR_RCV */
			data32 |= 0 << 16; /* GCP_RCV */
			data32 |= 1 << 15; /* VSI_RCV */
			data32 |= 0 << 14; /* AMP_RCV */
			data32 |= 0 << 13; /* AMP_CHG */
			data32 |= 0 << 8; /* PD_FIFO_NEW_ENTRY */
			data32 |= 0 << 4; /* PD_FIFO_OVERFL */
			data32 |= 0 << 3; /* PD_FIFO_UNDERFL */
			data32 |= 0 << 2; /* PD_FIFO_TH_START_PASS */
			data32 |= 0 << 1; /* PD_FIFO_TH_MAX_PASS */
			data32 |= 0 << 0; /* PD_FIFO_TH_MIN_PASS */
			data32 |= pdec_ists_en;
		} else if (is_meson_txhd_cpu()) {
			/* data32 |= 1 << 31;  DRC_CKS_CHG */
			/* data32 |= 1 << 30; DRC_RCV */
			data32 |= 0 << 29; /* AUD_TYPE_CHG */
			data32 |= 0 << 28; /* DVI_DET */
			data32 |= 1 << 27; /* VSI_CKS_CHG */
			data32 |= 0 << 26; /* GMD_CKS_CHG */
			data32 |= 0 << 25; /* AIF_CKS_CHG */
			data32 |= 1 << 24; /* AVI_CKS_CHG */
			data32 |= 0 << 23; /* ACR_N_CHG */
			data32 |= 0 << 22; /* ACR_CTS_CHG */
			data32 |= 1 << 21; /* GCP_AV_MUTE_CHG */
			data32 |= 0 << 20; /* GMD_RCV */
			data32 |= 0 << 19; /* AIF_RCV */
			data32 |= 0 << 18; /* AVI_RCV */
			data32 |= 0 << 17; /* ACR_RCV */
			data32 |= 0 << 16; /* GCP_RCV */
			data32 |= 1 << 15; /* VSI_RCV */
			/* data32 |= 0 << 14;  AMP_RCV */
			/* data32 |= 0 << 13;  AMP_CHG */
			data32 |= 0 << 8; /* PD_FIFO_NEW_ENTRY */
			data32 |= 0 << 4; /* PD_FIFO_OVERFL */
			data32 |= 0 << 3; /* PD_FIFO_UNDERFL */
			data32 |= 0 << 2; /* PD_FIFO_TH_START_PASS */
			data32 |= 0 << 1; /* PD_FIFO_TH_MAX_PASS */
			data32 |= 0 << 0; /* PD_FIFO_TH_MIN_PASS */
			data32 |= pdec_ists_en;
		} else { /* TXL and previous Chip */
			data32 = 0;
			data32 |= 0 << 29; /* AUD_TYPE_CHG */
			data32 |= 0 << 28; /* DVI_DET */
			data32 |= 1 << 27; /* VSI_CKS_CHG */
			data32 |= 0 << 26; /* GMD_CKS_CHG */
			data32 |= 0 << 25; /* AIF_CKS_CHG */
			data32 |= 1 << 24; /* AVI_CKS_CHG */
			data32 |= 0 << 23; /* ACR_N_CHG */
			data32 |= 0 << 22; /* ACR_CTS_CHG */
			data32 |= 1 << 21; /* GCP_AV_MUTE_CHG */
			data32 |= 0 << 20; /* GMD_RCV */
			data32 |= 0 << 19; /* AIF_RCV */
			data32 |= 0 << 18; /* AVI_RCV */
			data32 |= 0 << 17; /* ACR_RCV */
			data32 |= 0 << 16; /* GCP_RCV */
			data32 |= 0 << 15; /* VSI_RCV */
			data32 |= 0 << 14; /* AMP_RCV */
			data32 |= 0 << 13; /* AMP_CHG */
			/* diff */
			data32 |= 1 << 10; /* DRC_CKS_CHG */
			data32 |= 1 << 9; /* DRC_RCV */
			/* diff */
			data32 |= 0 << 8; /* PD_FIFO_NEW_ENTRY */
			data32 |= 0 << 4; /* PD_FIFO_OVERFL */
			data32 |= 0 << 3; /* PD_FIFO_UNDERFL */
			data32 |= 0 << 2; /* PD_FIFO_TH_START_PASS */
			data32 |= 0 << 1; /* PD_FIFO_TH_MAX_PASS */
			data32 |= 0 << 0; /* PD_FIFO_TH_MIN_PASS */
			data32 |= pdec_ists_en;
		}
		hdmirx_wr_dwc(DWC_PDEC_IEN_SET, data32);
		hdmirx_wr_dwc(DWC_AUD_FIFO_IEN_SET, OVERFL|UNDERFL);
		/*hdmirx_wr_dwc(DWC_MD_IEN_SET, rx_md_ists_en);*/
		/*data32 = 0;
		data32 |= AKSV_RCV;
		data32 |= SCDC_TMDS_CFG_CHG;
		/* if (!is_meson_txhd_cpu())
			data32 |= SCDC_TMDS_CFG_CHG;
		data32 |= _BIT(6);
		data32 |= _BIT(5);
		hdmirx_wr_dwc(DWC_HDMI_IEN_SET, data32);*/
		/* hdcp2.2*/
		/*if (hdcp22_on)
			hdmirx_wr_dwc(DWC_HDMI2_IEN_SET, 0x3f);*/
	} else {
		/* clear enable */
		hdmirx_wr_dwc(DWC_PDEC_IEN_CLR, ~0);
		hdmirx_wr_dwc(DWC_AUD_CEC_IEN_CLR, ~0);
		hdmirx_wr_dwc(DWC_AUD_FIFO_IEN_CLR, ~0);
		hdmirx_wr_dwc(DWC_MD_IEN_CLR, ~0);
		/* clear status */
		hdmirx_wr_dwc(DWC_PDEC_ICLR, ~0);
		hdmirx_wr_dwc(DWC_AUD_CEC_ICLR, ~0);
		hdmirx_wr_dwc(DWC_AUD_FIFO_ICLR, ~0);
		hdmirx_wr_dwc(DWC_MD_ICLR, ~0);
	}
}

void hdmirx_irq_hdcp_enable(bool enable)
{

	if (enable) {
		/* hdcp2.2 */
		if (hdcp22_on)
			hdmirx_wr_dwc(DWC_HDMI2_IEN_SET, 0x1f);

		/* hdcp1.4 */
		hdmirx_wr_dwc(DWC_HDMI_IEN_SET, AKSV_RCV);
	} else {
		/* hdcp2.2 */
		if (hdcp22_on) {
			/* clear enable */
			hdmirx_wr_dwc(DWC_HDMI2_IEN_CLR, ~0);
			/* clear status */
			hdmirx_wr_dwc(DWC_HDMI2_ICLR, ~0);
		}
		/* hdcp1.4 */
		/* clear enable */
		hdmirx_wr_dwc(DWC_HDMI_IEN_CLR, ~0);
		/* clear status */
		hdmirx_wr_dwc(DWC_HDMI_ICLR, ~0);
	}
}

bool hdmirx_tmds_pll_lock(void)
{
	if ((hdmirx_rd_dwc(DWC_HDMI_PLL_LCK_STS) & 1) == 1)
		return true;
	else {
		if (log_level & VIDEO_LOG)
			rx_pr("pll_unlock\n");
		return false;
	}

}

bool hdmirx_audio_pll_lock(void)
{
	/* if ((hdmirx_rd_dwc(DWC_AUD_PLL_CTRL) & (1 << 31)) == 0) */
	if ((rd_reg_hhi(HHI_AUD_PLL_CNTL_I) & (1 << 31)) == 0)
		return false;
	else
		return true;
}

bool is_clk_stable(void)
{
	int clk;

	clk = hdmirx_rd_phy(PHY_MAINFSM_STATUS1);
	clk = (clk >> 8) & 1;
	if (1 == clk)
		return true;
	else
		return false;
}

int hdmirx_audio_fifo_rst(void)
{
	int error = 0;
	hdmirx_wr_bits_dwc(DWC_AUD_FIFO_CTRL, AFIF_INIT, 1);
	udelay(20);
	hdmirx_wr_bits_dwc(DWC_AUD_FIFO_CTRL, AFIF_INIT, 0);
	hdmirx_wr_dwc(DWC_DMI_SW_RST, 0x10);
	if (log_level & AUDIO_LOG)
		rx_pr("%s\n", __func__);
	return error;
}

int hdmirx_control_clk_range(unsigned long min, unsigned long max)
{
	int error = 0;
	unsigned evaltime = 0;
	unsigned long ref_clk;

	ref_clk = modet_clk;
	evaltime = (ref_clk * 4095) / 158000;
	min = (min * evaltime) / ref_clk;
	max = (max * evaltime) / ref_clk;
	hdmirx_wr_bits_dwc(DWC_HDMI_CKM_F, MINFREQ, min);
	hdmirx_wr_bits_dwc(DWC_HDMI_CKM_F, CKM_MAXFREQ, max);
	return error;
}

int packet_init(void)
{
	int error = 0;
	int data32 = 0;

	data32 |= 1 << 9; /* amp_err_filter */
	data32 |= 1 << 8; /* isrc_err_filter */
	data32 |= 1 << 7; /* gmd_err_filter */
	data32 |= 1 << 6; /* aif_err_filter */
	data32 |= 1 << 5; /* avi_err_filter */
	data32 |= 1 << 4; /* vsi_err_filter */
	data32 |= 1 << 3; /* gcp_err_filter */
	data32 |= 1 << 2; /* acrp_err_filter */
	data32 |= 1 << 1; /* ph_err_filter */
	data32 |= 0 << 0; /* checksum_err_filter */
	hdmirx_wr_dwc(DWC_PDEC_ERR_FILTER, data32);

	data32 = hdmirx_rd_dwc(DWC_PDEC_CTRL);
	data32 |= 1 << 31;	/* PFIFO_STORE_FILTER_EN */
	data32 |= 1 << 4;	/* PD_FIFO_WE */
	data32 |= 1 << 0;	/* PDEC_BCH_EN */
	data32 &= (~GCP_GLOBAVMUTE);
	data32 |= GCP_GLOBAVMUTE_EN << 15;
	data32 |= packet_fifo_cfg;
	hdmirx_wr_dwc(DWC_PDEC_CTRL, data32);

	data32 = 0;
	data32 |= pd_fifo_start_cnt << 20;	/* PD_start */
	data32 |= 640 << 10;	/* PD_max */
	data32 |= 8 << 0;		/* PD_min */
	hdmirx_wr_dwc(DWC_PDEC_FIFO_CFG, data32);

	return error;
}

int hdmirx_packet_fifo_rst(void)
{
	int error = 0;

	hdmirx_wr_bits_dwc(DWC_PDEC_CTRL,
		PD_FIFO_FILL_INFO_CLR|PD_FIFO_CLR, ~0);
	hdmirx_wr_bits_dwc(DWC_PDEC_CTRL,
		PD_FIFO_FILL_INFO_CLR|PD_FIFO_CLR,  0);
	return error;
}

static int TOP_init(void)
{
	int err = 0;
	int data32 = 0;

	data32 |= (0xf	<< 13); /* bit[16:13] */
	data32 |= 0	<< 11;
	data32 |= 0	<< 10;
	data32 |= 0	<< 9;
	data32 |= 0 << 8;
	data32 |= EDID_CLK_DIV << 0;
	hdmirx_wr_top(TOP_EDID_GEN_CNTL,  data32);

	if (is_meson_gxtvbb_cpu()) {
		hdmirx_wr_top(TOP_INFILTER_GXTVBB,
			(0x2001 << 16));
	} else {
		data32 = 0;
		/* SDA filter internal clk div */
		data32 |= 1 << 29;
		/* SDA sampling clk div */
		data32 |= 1 << 16;
		/* SCL filter internal clk div */
		data32 |= 1 << 13;
		/* SCL sampling clk div */
		data32 |= 1 << 0;
		hdmirx_wr_top(TOP_INFILTER_HDCP, data32);
		hdmirx_wr_top(TOP_INFILTER_I2C0, data32);
		hdmirx_wr_top(TOP_INFILTER_I2C1, data32);
		hdmirx_wr_top(TOP_INFILTER_I2C2, data32);
		hdmirx_wr_top(TOP_INFILTER_I2C3, data32);
	}

	data32 = 0;
	/* conversion mode of 422 to 444 */
	data32 |= 0	<< 19;
	/* !!!!dolby vision 422 to 444 ctl bit */
	data32 |= 0	<< 0;
	hdmirx_wr_top(TOP_VID_CNTL,	data32);

	if (!is_meson_txhd_cpu()) {
		data32 = 0;
		data32 |= 0	<< 20;
		data32 |= 0	<< 8;
		data32 |= 0x0a	<< 0;
		hdmirx_wr_top(TOP_VID_CNTL2,  data32);
	}
	data32 = 0;
	/* delay cycles before n/cts update pulse */
	data32 |= 7 << 0;
	hdmirx_wr_top(TOP_ACR_CNTL2, data32);
	return err;
}

static int DWC_init(void)
{
	int err = 0;
	unsigned long   data32;
	unsigned evaltime = 0;

	evaltime = (modet_clk * 4095) / 158000;
	/* enable all */
	hdmirx_wr_dwc(DWC_HDMI_OVR_CTRL, ~0);
	/* recover to default value.
	remain code for some time.
	if no side effect then remove it */
	/*
	hdmirx_wr_bits_dwc(DWC_HDMI_SYNC_CTRL,
		VS_POL_ADJ_MODE, VS_POL_ADJ_AUTO);
	hdmirx_wr_bits_dwc(DWC_HDMI_SYNC_CTRL,
		HS_POL_ADJ_MODE, HS_POL_ADJ_AUTO);
	*/
	hdmirx_wr_bits_dwc(DWC_HDMI_CKM_EVLTM,
		EVAL_TIME, evaltime);
	hdmirx_control_clk_range(TMDS_CLK_MIN,
		TMDS_CLK_MAX);

	/* hdmirx_wr_bits_dwc(DWC_SNPS_PHYG3_CTRL,
		((1 << 2) - 1) << 2, port); */

	data32 = 0;
	data32 |= 0     << 20;
	data32 |= 1     << 19;
	data32 |= 5     << 16;  /* [18:16]  valid_mode */
	data32 |= 0     << 12;  /* [13:12]  ctrl_filt_sens */
	data32 |= 3     << 10;  /* [11:10]  vs_filt_sens */
	data32 |= 0     << 8;   /* [9:8]    hs_filt_sens */
	data32 |= 2     << 6;   /* [7:6]    de_measure_mode */
	data32 |= 0     << 5;   /* [5]      de_regen */
	data32 |= 3     << 3;   /* [4:3]    de_filter_sens */
	hdmirx_wr_dwc(DWC_HDMI_ERROR_PROTECT, data32);

	data32 = 0;
	data32 |= 0     << 8;   /* [10:8]   hact_pix_ith */
	data32 |= 0     << 5;   /* [5]      hact_pix_src */
	data32 |= 1     << 4;   /* [4]      htot_pix_src */
	hdmirx_wr_dwc(DWC_MD_HCTRL1, data32);

	data32 = 0;
	data32 |= 1     << 12;  /* [14:12]  hs_clk_ith */
	data32 |= 7     << 8;   /* [10:8]   htot32_clk_ith */
	data32 |= 1     << 5;   /* [5]      vs_act_time */
	data32 |= 3     << 3;   /* [4:3]    hs_act_time */
	data32 |= 0     << 0;   /* [1:0]    h_start_pos */
	hdmirx_wr_dwc(DWC_MD_HCTRL2, data32);

	data32 = 0;
	data32 |= 1	<< 4;   /* [4]      v_offs_lin_mode */
	data32 |= 1	<< 1;   /* [1]      v_edge */
	data32 |= 0	<< 0;   /* [0]      v_mode */
	hdmirx_wr_dwc(DWC_MD_VCTRL, data32);

	data32  = 0;
	data32 |= 1 << 10;  /* [11:10]  vofs_lin_ith */
	data32 |= 3 << 8;   /* [9:8]    vact_lin_ith */
	data32 |= 0 << 6;   /* [7:6]    vtot_lin_ith */
	data32 |= 7 << 3;   /* [5:3]    vs_clk_ith */
	data32 |= 2 << 0;   /* [2:0]    vtot_clk_ith */
	hdmirx_wr_dwc(DWC_MD_VTH, data32);

	data32  = 0;
	data32 |= 1 << 2;   /* [2]      fafielddet_en */
	data32 |= 0 << 0;   /* [1:0]    field_pol_mode */
	hdmirx_wr_dwc(DWC_MD_IL_POL, data32);

	data32  = 0;
	data32 |= 0	<< 1;
	data32 |= 1	<< 0;
	hdmirx_wr_dwc(DWC_HDMI_RESMPL_CTRL, data32);

	data32	= 0;
	data32 |= (hdmirx_rd_dwc(DWC_HDMI_MODE_RECOVER) & 0xf8000000);
	data32 |= (0	<< 24);
	data32 |= (0	<< 18);
	data32 |= (HYST_HDMI_TO_DVI	<< 13);
	data32 |= (HYST_DVI_TO_HDMI	<< 8);
	data32 |= (0	<< 6);
	data32 |= (0	<< 4);
	data32 |= (0	<< 2);
	data32 |= (0	<< 0);
	hdmirx_wr_dwc(DWC_HDMI_MODE_RECOVER, data32);

	return err;
}

bool hdmirx_is_key_write(void)
{
	if (hdmirx_rd_dwc(DWC_HDCP_BKSV0) != 0)
		return 1;
	else
		return 0;
}

void hdmi_rx_ctrl_hdcp_config(const struct hdmi_rx_ctrl_hdcp *hdcp)
{
	int error = 0;
	unsigned i = 0;
	unsigned k = 0;

	hdmirx_wr_bits_dwc(DWC_HDCP_SETTINGS, HDCP_FAST_MODE, 0);
	hdmirx_wr_bits_dwc(DWC_HDCP_CTRL, ENCRIPTION_ENABLE, 0);
	/* hdmirx_wr_bits_dwc(ctx, DWC_HDCP_CTRL, KEY_DECRYPT_ENABLE, 1); */
	hdmirx_wr_bits_dwc(DWC_HDCP_CTRL, KEY_DECRYPT_ENABLE, 0);
	hdmirx_wr_dwc(DWC_HDCP_SEED, hdcp->seed);
	for (i = 0; i < HDCP_KEYS_SIZE; i += 2) {

		for (k = 0; k < HDCP_KEY_WR_TRIES; k++) {
			if (hdmirx_rd_bits_dwc(DWC_HDCP_STS,
				HDCP_KEY_WR_OK_STS) != 0) {
				break;
			}
		}
		if (k < HDCP_KEY_WR_TRIES) {
			hdmirx_wr_dwc(DWC_HDCP_KEY1, hdcp->keys[i + 0]);
			hdmirx_wr_dwc(DWC_HDCP_KEY0, hdcp->keys[i + 1]);
		} else {
			error = -EAGAIN;
			break;
		}
	}
	hdmirx_wr_dwc(DWC_HDCP_BKSV1, hdcp->bksv[0]);
	hdmirx_wr_dwc(DWC_HDCP_BKSV0, hdcp->bksv[1]);
	if (!is_meson_txhd_cpu()) {
		hdmirx_wr_bits_dwc(DWC_HDCP_RPT_CTRL,
			REPEATER, hdcp->repeat ? 1 : 0);
		/* nothing attached downstream */
		hdmirx_wr_dwc(DWC_HDCP_RPT_BSTATUS, 0);
	}
	hdmirx_wr_bits_dwc(DWC_HDCP_CTRL, ENCRIPTION_ENABLE, 1);
}

void rx_set_hpd(uint8_t val)
{
	if (is_meson_gxtvbb_cpu()) {
		if (!val) {
			hdmirx_wr_top(TOP_HPD_PWR5V,
				hdmirx_rd_top(TOP_HPD_PWR5V)&(~(1<<rx.port)));
		} else {
			hdmirx_wr_top(TOP_HPD_PWR5V,
				hdmirx_rd_top(TOP_HPD_PWR5V)|(1<<rx.port));
		}
	} else {
		if (val) {
			hdmirx_wr_top(TOP_HPD_PWR5V,
				hdmirx_rd_top(TOP_HPD_PWR5V)&(~(1<<rx.port)));
		} else {
			hdmirx_wr_top(TOP_HPD_PWR5V,
				hdmirx_rd_top(TOP_HPD_PWR5V)|(1<<rx.port));
		}
	}

	if (log_level & LOG_EN)
		rx_pr("%s, port:%d, val:%d\n", __func__,
						rx.port, val);
}

void control_reset(void)
{
	unsigned long data32;

	/* Enable functional modules */
	data32  = 0;
	data32 |= 1 << 5;   /* [5]      cec_enable */
	data32 |= 1 << 4;   /* [4]      aud_enable */
	data32 |= 1 << 3;   /* [3]      bus_enable */
	data32 |= 1 << 2;   /* [2]      hdmi_enable */
	data32 |= 1 << 1;   /* [1]      modet_enable */
	data32 |= 1 << 0;   /* [0]      cfg_enable */
	hdmirx_wr_dwc(DWC_DMI_DISABLE_IF, data32);

	mdelay(1);

	/* Reset functional modules */
	/* data32 = hdmirx_rd_dwc(DWC_SNPS_PHYG3_CTRL); */
	hdmirx_wr_dwc(DWC_DMI_SW_RST,	0x0000001F);
	/* hdmirx_wr_dwc(DWC_SNPS_PHYG3_CTRL, data32); */
	if (is_meson_txl_cpu() || is_meson_gxtvbb_cpu())
		cecrx_hw_init();
}

void clk_off(void)
{
	/* wr_reg(HHI_HDMIRX_CLK_CNTL, 0); */
	/* wr_reg(HHI_HDMIRX_AUD_CLK_CNTL, 0); */
}

#ifdef HDCP22_ENABLE
void rx_esm_tmdsclk_en(bool en)
{
	hdmirx_wr_bits_top(TOP_CLK_CNTL, HDCP22_TMDSCLK_EN, en);

	if (log_level & HDCP_LOG)
		rx_pr("%s:%d\n", __func__, en);
}

void hdcp22_clk_init(void)
{
	unsigned int data32;

	/* Enable clk81_hdcp22_pclk */
	wr_reg_hhi(HHI_GCLK_MPEG2, (rd_reg_hhi(HHI_GCLK_MPEG2)|1<<3));

	/* Enable hdcp22_esmclk */
	/* .clk0               ( fclk_div7  ), */
	/* .clk1               ( fclk_div4  ), */
	/* .clk2               ( fclk_div3  ), */
	/* .clk3               ( fclk_div5  ), */
	wr_reg_hhi(HHI_HDCP22_CLK_CNTL,
	(rd_reg_hhi(HHI_HDCP22_CLK_CNTL) & 0xffff0000) |
	 /* [10: 9] clk_sel. select fclk_div7=2000/7=285.71 MHz */
	((0 << 9)   |
	 /* [    8] clk_en. Enable gated clock */
	 (1 << 8)   |
	 /* [ 6: 0] clk_div. Divide by 1. = 285.71/1 = 285.71 MHz */
	 (0 << 0)));

	wr_reg_hhi(HHI_HDCP22_CLK_CNTL,
	(rd_reg_hhi(HHI_HDCP22_CLK_CNTL) & 0x0000ffff) |
	/* [26:25] clk_sel. select cts_oscin_clk=24 MHz */
	((0 << 25)  |
	 (1 << 24)  |   /* [   24] clk_en. Enable gated clock */
	 (0 << 16)));

	data32 = hdmirx_rd_top(TOP_CLK_CNTL);
	data32 |= (hdcp22_on << 5);
	data32 |= (hdcp22_on << 4);
	data32 |= (hdcp22_on << 3);
	hdmirx_wr_top(TOP_CLK_CNTL, data32);    /* DEFAULT: {32'h0} */
}

void hdcp22_clk_en(uint8_t en)
{
	hdmirx_wr_bits_top(TOP_CLK_CNTL, MSK(3, 3), en);
}

void hdcp22_suspend(void)
{
	wr_reg_hhi(HHI_HDCP22_CLK_CNTL, 0);
	hdmirx_wr_bits_top(TOP_CLK_CNTL, MSK(3, 3), 0);
	/* note: can't pull down hpd before enter suspend */
	/* it will stop cec wake up func if EE domain still working */
	/* rx_set_hpd(0); */
	hpd_to_esm = 0;
	do_esm_rst_flag = 1;
	hdmirx_wr_dwc(DWC_HDCP22_CONTROL,
				0x0);
	if (hdcp22_kill_esm == 0) {
		rx_pr("kill = 1\n");
		hdmirx_hdcp22_esm_rst();
		msleep(20);
	}
	rx_pr("hdcp22 off\n");
}

void hdcp22_resume(void)
{
	hdcp22_kill_esm = 0;
	switch_set_state(&rx.hpd_sdev, 0x0);
	hdcp22_clk_init();
	hdmirx_wr_dwc(DWC_HDCP22_CONTROL,
				0x1000);
	hdcp22_wr_top(TOP_SKP_CNTL_STAT, 0x1);
	hdmirx_hw_config();
	switch_set_state(&rx.hpd_sdev, 0x01);
	hpd_to_esm = 1;
	/* dont need to delay 900ms to wait sysctl start hdcp_rx22,
	sysctl is userspace it wakes up later than driver */
	/* mdelay(900); */
	/* rx_set_hpd(1); */
	rx_pr("hdcp22 on\n");
}

#endif

void clk_init(void)
{
	unsigned int data32;

	/* DWC clock enable */
	/* Turn on clk_hdmirx_pclk, also = sysclk */
	wr_reg_hhi(HHI_GCLK_MPEG0,
	rd_reg_hhi(HHI_GCLK_MPEG0) | (1 << 21));

	/* Enable APB3 fail on error */
	/* APB3 to HDMIRX-TOP err_en */
	/* default 0x3ff, | bit15 = 1 */

	/* hdmirx_wr_ctl_port(0, 0x83ff); */
	/* hdmirx_wr_ctl_port(0x10, 0x83ff); */

	/* turn on clocks: md, cfg... */
	/* G9 clk tree */
	/* fclk_div5 400M ----- mux sel = 3 */
	/* fclk_div3 850M ----- mux sel = 2 */
	/* fclk_div4 637M ----- mux sel = 1 */
	/* XTAL		24M  ----- mux sel = 0 */
	/* [26:25] HDMIRX mode detection clock mux select: osc_clk */
	/* [24]    HDMIRX mode detection clock enable */
	/* [22:16] HDMIRX mode detection clock divider */
	/* [10: 9] HDMIRX config clock mux select: */
	/* [    8] HDMIRX config clock enable */
	/* [ 6: 0] HDMIRX config clock divider: */
	data32  = 0;
	data32 |= 0 << 25;
	data32 |= 1 << 24;
	data32 |= 0 << 16;
	data32 |= 3 << 9;
	data32 |= 1 << 8;
	data32 |= 2 << 0;
	wr_reg_hhi(HHI_HDMIRX_CLK_CNTL, data32);

	data32 = 0;
	data32 |= 2	<< 25;
	data32 |= acr_mode << 24;
	data32 |= 0	<< 16;
	data32 |= 2	<< 9;
	data32 |= 1	<< 8;
	data32 |= 2	<< 0;
	wr_reg_hhi(HHI_HDMIRX_AUD_CLK_CNTL, data32);

	if (is_meson_txlx_cpu() || is_meson_txhd_cpu())  {
		/* [15] hdmirx_aud_pll4x_en override enable */
		/* [14] hdmirx_aud_pll4x_en override value */
		/* [6:5] clk_sel for cts_hdmirx_aud_pll_clk:
			0=hdmirx_aud_pll_clk */
		/* [4] clk_en for cts_hdmirx_aud_pll_clk */
		/* [2:0] clk_div for cts_hdmirx_aud_pll_clk */
		data32  = 0;
		data32 |= (0 << 15);
		data32 |= (1 << 14);
		data32 |= (0 << 5);
		data32 |= (0 << 4);
		data32 |= (0 << 0);
		wr_reg_hhi(HHI_AUDPLL_CLK_OUT_CNTL, data32);
		data32 |= (1 << 4);
		wr_reg_hhi(HHI_AUDPLL_CLK_OUT_CNTL, data32);
	}



#if 0 /* def HDCP22_ENABLE */
	if (hdcp22_on) {
		/* Enable clk81_hdcp22_pclk */
		wr_reg_hhi(HHI_GCLK_MPEG2, (rd_reg_hhi(HHI_GCLK_MPEG2)|1<<3));

		/* Enable hdcp22_esmclk */
		/* .clk0               ( fclk_div7  ), */
		/* .clk1               ( fclk_div4  ), */
		/* .clk2               ( fclk_div3  ), */
		/* .clk3               ( fclk_div5  ), */
		wr_reg_hhi(HHI_HDCP22_CLK_CNTL,
		(rd_reg_hhi(HHI_HDCP22_CLK_CNTL) & 0xffff0000) |
		 /* [10: 9] clk_sel. select fclk_div7=2000/7=285.71 MHz */
		((0 << 9)   |
		 /* [    8] clk_en. Enable gated clock */
		 (1 << 8)   |
		 /* [ 6: 0] clk_div. Divide by 1. = 285.71/1 = 285.71 MHz */
		 (0 << 0)));

		wr_reg_hhi(HHI_HDCP22_CLK_CNTL,
		(rd_reg_hhi(HHI_HDCP22_CLK_CNTL) & 0x0000ffff) |
		/* [26:25] clk_sel. select cts_oscin_clk=24 MHz */
		((0 << 25)  |
		 (1 << 24)  |   /* [   24] clk_en. Enable gated clock */
		 (0 << 16)));
	}
#endif
	data32 = 0;
	data32 |= 0 << 31;  /* [31]     disable clkgating */
	data32 |= 1 << 17;  /* [17]     audfifo_rd_en */
	data32 |= 1 << 16;  /* [16]     pktfifo_rd_en */
	data32 |= 1 << 2;   /* [2]      hdmirx_cecclk_en */
	data32 |= 0 << 1;   /* [1]      bus_clk_inv */
	data32 |= 0 << 0;   /* [0]      hdmi_clk_inv */
	hdmirx_wr_top(TOP_CLK_CNTL, data32);    /* DEFAULT: {32'h0} */
}

void hdmirx_hdcp22_hpd(bool value)
{
	unsigned long data32 = hdmirx_rd_dwc(DWC_HDCP22_CONTROL);
	if (value)
		data32 |= 0x1000;
	else
		data32 &= (~0x1000);
	hdmirx_wr_dwc(DWC_HDCP22_CONTROL, data32);
}

void hdmirx_20_init(void)
{
	unsigned long data32;
	data32 = 0;
	data32 |= 1	<< 12; /* [12]     vid_data_checken */
	data32 |= 1	<< 11; /* [11]     data_island_checken */
	data32 |= 1	<< 10; /* [10]     gb_checken */
	data32 |= 1	<< 9;  /* [9]      preamb_checken */
	data32 |= 1	<< 8;  /* [8]      ctrl_checken */
	data32 |= 1	<< 4;  /* [4]      scdc_enable */
	data32 |= SCRAMBLE_SEL	<< 0;  /* [1:0]    scramble_sel */
	hdmirx_wr_dwc(DWC_HDMI20_CONTROL,    data32);

	data32  = 0;
	data32 |= 1	<< 24; /* [25:24]  i2c_spike_suppr */
	data32 |= 0	<< 20; /* [20]     i2c_timeout_en */
	data32 |= 0	<< 0;  /* [19:0]   i2c_timeout_cnt */
	hdmirx_wr_dwc(DWC_SCDC_I2CCONFIG,    data32);

	data32  = 0;
	data32 |= 0    << 1;  /* [1]      hpd_low */
	data32 |= 1    << 0;  /* [0]      power_provided */
	hdmirx_wr_dwc(DWC_SCDC_CONFIG,   data32);

	data32  = 0;
	data32 |= 0xabcdef << 8;  /* [31:8]   manufacture_oui */
	data32 |= 1	<< 0;  /* [7:0]    sink_version */
	hdmirx_wr_dwc(DWC_SCDC_WRDATA0,	data32);

	data32  = 0;
	data32 |= 10	<< 20; /* [29:20]  chlock_max_err */
	data32 |= 24000	<< 0;  /* [15:0]   milisec_timer_limit */
	hdmirx_wr_dwc(DWC_CHLOCK_CONFIG, data32);

	/* hdcp2.2 ctl */
#ifdef HDCP22_ENABLE
	if (hdcp22_on && hdcp22_firmware_ok_flag)
		hdmirx_wr_dwc(DWC_HDCP22_CONTROL, 0x1000);
	else
#endif
	hdmirx_wr_dwc(DWC_HDCP22_CONTROL, 2);
}

#ifdef HDCP22_ENABLE
void hdmirx_hdcp22_esm_rst(void)
{
	hdmirx_wr_top(TOP_SW_RESET, 0x100);
	mdelay(1);
	hdmirx_wr_top(TOP_SW_RESET, 0x0);
	rx_pr("esm rst\n");
}

void hdmirx_hdcp22_init(void)
{
	int ret = 0;
	ret = rx_sec_set_duk();
	rx_pr("hdcp22 == %d\n", ret);
	if (ret == 1) {
		hdcp22_wr_top(TOP_SKP_CNTL_STAT, 7);
		hdcp22_on = 1;
		hdcp22_clk_init();
		hpd_to_esm = 1;
	} else
		hdcp22_on = 0;
}
#endif

int hdmirx_audio_init(void)
{
	#define AUD_CLK_DELTA   2000
	/* 0=I2S 2-channel; 1=I2S 4 x 2-channel. */
	#define RX_8_CHANNEL        1
	int err = 0;
	unsigned long data32 = 0;

	data32 |= 7	<< 13;
	data32 |= 0	<< 12;
	data32 |= 1	<< 11;
	data32 |= 0	<< 10;

	data32 |= 0	<< 9;
	data32 |= 1	<< 8;
	data32 |= 1	<< 6;
	data32 |= 3	<< 4;
	data32 |= 0	<< 3;
	data32 |= acr_mode  << 2;
	data32 |= acr_mode  << 1;
	data32 |= acr_mode  << 0;
	hdmirx_wr_top(TOP_ACR_CNTL_STAT, data32);

	/*
	recover to default value, bit[27:24]
	set aud_pll_lock filter */
	/*
	data32  = 0;
	data32 |= 0 << 28;
	data32 |= 0 << 24;
	hdmirx_wr_dwc(DWC_AUD_PLL_CTRL, data32);
	*/

	/* AFIFO depth 1536word.
	increase start threshold to middle position */
	data32  = 0;
	data32 |= 160 << 18; /* start */
	data32 |= 200	<< 9; /* max */
	data32 |= 8	<< 0; /* min */
	hdmirx_wr_dwc(DWC_AUD_FIFO_TH, data32);

	/* recover to default value.
	remain code for some time.
	if no side effect then remove it */
	/*
	data32  = 0;
	data32 |= 1	<< 16;
	data32 |= 0	<< 0;
	hdmirx_wr_dwc(DWC_AUD_FIFO_CTRL, data32);
	*/

	data32  = 0;
	data32 |= 0	<< 8;
	data32 |= 1	<< 7;
	data32 |= (RX_8_CHANNEL ? 0x13:0x00) << 2;
	data32 |= 1	<< 0;
	hdmirx_wr_dwc(DWC_AUD_CHEXTR_CTRL, data32);

	data32 = 0;
	/* [22:21]	aport_shdw_ctrl */
	data32 |= 3	<< 21;
	/* [20:19]  auto_aclk_mute */
	data32 |= auto_aclk_mute	<< 19;
	/* [16:10]  aud_mute_speed */
	data32 |= 1	<< 10;
	/* [7]      aud_avmute_en */
	data32 |= aud_avmute_en	<< 7;
	/* [6:5]    aud_mute_sel */
	data32 |= aud_mute_sel	<< 5;
	/* [4:3]    aud_mute_mode */
	data32 |= 1	<< 3;
	/* [2:1]    aud_ttone_fs_sel */
	data32 |= 0	<< 1;
	/* [0]      testtone_en */
	data32 |= 0	<< 0;
	hdmirx_wr_dwc(DWC_AUD_MUTE_CTRL, data32);

	/* recover to default value.
	remain code for some time.
	if no side effect then remove it */
	/*
	data32 = 0;
	data32 |= 0	<< 16;
	data32 |= 0	<< 12;
	data32 |= 0	<< 4;
	data32 |= 0	<< 1;
	data32 |= 0	<< 0;
	hdmirx_wr_dwc(DWC_AUD_PAO_CTRL,   data32);
	*/

	/* recover to default value.
	remain code for some time.
	if no side effect then remove it */
	/*
	data32  = 0;
	data32 |= 0	<< 8;
	hdmirx_wr_dwc(DWC_PDEC_AIF_CTRL,  data32);
	*/

	data32  = 0;
	/* [4:2]    deltacts_irqtrig */
	data32 |= 0 << 2;
	/* [1:0]    cts_n_meas_mode */
	data32 |= 0 << 0;
	/* DEFAULT: {27'd0, 3'd0, 2'd1} */
	hdmirx_wr_dwc(DWC_PDEC_ACRM_CTRL, data32);

	hdmirx_wr_bits_dwc(DWC_AUD_CTRL, DWC_AUD_HBR_ENABLE, 1);

	/* SAO cfg, disable I2S output, no use */
	data32 = 0;
	data32 |= 1	<< 10;
	data32 |= 0	<< 9;
	data32 |= 0x0f	<< 5;
	data32 |= 0	<< 1;
	data32 |= 1	<< 0;
	hdmirx_wr_dwc(DWC_AUD_SAO_CTRL, data32);

	data32  = 0;
	data32 |= 1	<< 6;
	data32 |= 0xf	<< 2;
	hdmirx_wr_dwc(DWC_PDEC_ASP_CTRL, data32);

	return err;
}

void hdmirx_phy_init(void)
{
	unsigned int data32;

	data32 = 0;
	data32 |= 1 << 6;
	data32 |= 1 << 4;
	data32 |= rx.port << 2;
	data32 |= 1 << 1;
	data32 |= 1 << 0;
	hdmirx_wr_dwc(DWC_SNPS_PHYG3_CTRL, data32);
	mdelay(1);

	data32	= 0;
	data32 |= 1 << 6;
	data32 |= 1 << 4;
	data32 |= rx.port << 2;
	data32 |= 1 << 1;
	data32 |= 0 << 0;
	hdmirx_wr_dwc(DWC_SNPS_PHYG3_CTRL, data32);

	data32 = 0;
	data32 |= phy_lock_thres << 10;
	data32 |= 1 << 9;
	data32 |= ((phy_cfg_clk * 4) / 1000);
	hdmirx_wr_phy(PHY_CMU_CONFIG, data32);

	hdmirx_wr_phy(PHY_VOLTAGE_LEVEL, eq_ref_voltage);

	data32 = 0;
	data32 |= 0	<< 15;
	data32 |= 0	<< 13;
	data32 |= 0	<< 12;
	data32 |= phy_fast_switching << 11;
	data32 |= 0	<< 10;
	data32 |= phy_fsm_enhancement << 9;
	data32 |= 0	<< 8;
	data32 |= 0	<< 7;
	data32 |= 0 << 5;
	data32 |= 0	<< 3;
	data32 |= 0 << 2;
	data32 |= 0 << 0;
	hdmirx_wr_phy(PHY_SYSTEM_CONFIG, data32);

	hdmirx_wr_phy(MPLL_PARAMETERS2,	0x1c94);
	hdmirx_wr_phy(MPLL_PARAMETERS3,	0x3713);
	/*default 0x24da , EQ optimizing for kaiboer box */
	hdmirx_wr_phy(MPLL_PARAMETERS4,	0x24dc);
	hdmirx_wr_phy(MPLL_PARAMETERS5,	0x5492);
	hdmirx_wr_phy(MPLL_PARAMETERS6,	0x4b0d);
	hdmirx_wr_phy(MPLL_PARAMETERS7,	0x4760);
	hdmirx_wr_phy(MPLL_PARAMETERS8,	0x008c);
	hdmirx_wr_phy(MPLL_PARAMETERS9,	0x0010);
	hdmirx_wr_phy(MPLL_PARAMETERS10, 0x2d20);
	hdmirx_wr_phy(MPLL_PARAMETERS11, 0x2e31);
	hdmirx_wr_phy(MPLL_PARAMETERS12, 0x4b64);
	hdmirx_wr_phy(MPLL_PARAMETERS13, 0x2493);
	hdmirx_wr_phy(MPLL_PARAMETERS14, 0x676d);
	hdmirx_wr_phy(MPLL_PARAMETERS15, 0x23e0);
	hdmirx_wr_phy(MPLL_PARAMETERS16, 0x001b);
	hdmirx_wr_phy(MPLL_PARAMETERS17, 0x2218);
	hdmirx_wr_phy(MPLL_PARAMETERS18, 0x1b25);
	hdmirx_wr_phy(MPLL_PARAMETERS19, 0x2492);
	hdmirx_wr_phy(MPLL_PARAMETERS20, 0x48ea);
	hdmirx_wr_phy(MPLL_PARAMETERS21, 0x0011);
	hdmirx_wr_phy(MPLL_PARAMETERS22, 0x04d2);
	hdmirx_wr_phy(MPLL_PARAMETERS23, 0x0414);

	/* Configuring I2C to work in fastmode */
	hdmirx_wr_dwc(DWC_I2CM_PHYG3_MODE,	 0x1);
	/* disable overload protect for Philips DVD */
	/* NOTE!!!!! don't remove below setting */
	hdmirx_wr_phy(OVL_PROT_CTRL, 0xa);

	/* clear clkrate cfg */
	hdmirx_wr_bits_phy(PHY_CDR_CTRL_CNT, CLK_RATE_BIT, 0);

	#if 0
	/* enable all ports's termination*/
	data32 = 0;
	data32 |= 1 << 8;
	data32 |= 0x0f << 4;
	hdmirx_wr_phy(PHY_MAIN_FSM_OVERRIDE1, data32);
	#endif

	data32 = 0;
	data32 |= 1 << 6;
	data32 |= 1 << 4;
	data32 |= rx.port << 2;
	data32 |= 0 << 1;
	data32 |= 0 << 0;
	hdmirx_wr_dwc(DWC_SNPS_PHYG3_CTRL, data32);
	rx_pr("%s Done!\n", __func__);
}

void hdmirx_edid_reset(void)
{
	hdmirx_wr_bits_top(TOP_SW_RESET, _BIT(1), 0);
	hdmirx_wr_bits_top(TOP_SW_RESET, _BIT(1), 1);
	hdmirx_wr_bits_top(TOP_SW_RESET, _BIT(1), 0);
}

bool hdmirx_phy_clk_rate_monitor(void)
{
	unsigned int clk_rate;
	bool changed = false;
	int i;
	int error = 0;

	if (is_meson_txhd_cpu())
		return false;

	if (force_clk_rate & 0x10)
		clk_rate = force_clk_rate & 1;
	else
		clk_rate = (hdmirx_rd_dwc(DWC_SCDC_REGS0) >> 17) & 1;

	if (clk_rate != last_clk_rate) {
		changed = true;
		for (i = 0; i < 3; i++) {
			error = hdmirx_wr_bits_phy(PHY_CDR_CTRL_CNT,
				CLK_RATE_BIT, clk_rate);

			if (error == 0)
				break;
		}
		if (log_level & VIDEO_LOG)
			rx_pr("clk_rate:%d, last_clk_rate: %d\n",
			clk_rate, last_clk_rate);
		last_clk_rate = clk_rate;
	}
	return changed;
}

void rx_hdcp_init(void)
{
	if (hdcp_enable)
		hdmi_rx_ctrl_hdcp_config(&rx.hdcp);
	else
		hdmirx_wr_bits_dwc(DWC_HDCP_CTRL, ENCRIPTION_ENABLE, 0);
}

void hdmirx_hw_config(void)
{
	rx_pr("%s port:%d\n", __func__, rx.port);
	hdmirx_wr_top(TOP_INTR_MASKN, 0);
	control_reset();
	hdmirx_edid_reset();
	/*hdmirx_irq_enable(FALSE);
	hdmirx_irq_hdcp22_enable(FALSE);*/
	hdmi_rx_ctrl_edid_update();
	/* hdmirx_wr_dwc(DWC_HDCP22_CONTROL, 2); */
	rx_hdcp_init();
	hdmirx_audio_init();
	packet_init();
	if (!is_meson_txhd_cpu())
		hdmirx_20_init();
	hdmirx_wr_top(TOP_INTR_MASKN, top_intr_maskn_value);

	hdmirx_phy_init();
	hdmirx_wr_top(TOP_PORT_SEL, 0x10 | ((1<<rx.port)));
	DWC_init();
	hdmirx_irq_hdcp_enable(TRUE);
	rx_pr("%s  %d Done!\n", __func__, rx.port);
}

void hdmirx_hw_probe(void)
{
	hdmirx_wr_top(TOP_MEM_PD, 0);
	hdmirx_wr_top(TOP_INTR_MASKN, 0);
	hdmirx_wr_top(TOP_SW_RESET, 0);
	clk_init();
	if (is_meson_gxtvbb_cpu())
		hdmirx_wr_top(TOP_HPD_PWR5V, 0x10);
	else
		hdmirx_wr_top(TOP_HPD_PWR5V, 0x1f);
	hdmi_rx_ctrl_edid_update();
	TOP_init();
	control_reset();
	DWC_init();
	hdmirx_phy_init();
	/*hdmirx_irq_enable(FALSE);
	hdmirx_irq_hdcp22_enable(FALSE);*/
	hdmirx_hdcp22_init();
	hdmirx_audio_init();
	packet_init();
	if (!is_meson_txhd_cpu())
		hdmirx_20_init();
	hdmirx_wr_top(TOP_PORT_SEL, 0x10);
	hdmirx_wr_top(TOP_INTR_STAT_CLR, ~0);
	hdmirx_wr_top(TOP_INTR_MASKN, top_intr_maskn_value);
	rx_pr("%s Done!\n", __func__);
}

/***************************************************
func: hdmirx_audio_pll_sw_update
	Sent an update pulse to audio pll module.
	Indicate the ACR info is changed.
***************************************************/
void hdmirx_audio_pll_sw_update(void)
{
	hdmirx_wr_bits_top(TOP_ACR_CNTL_STAT, _BIT(11), 1);
}

/***************************************************
func: is_afifo_error
	check if afifo block or not
	bit4: indicate FIFO is overflow
	bit3: indicate FIFO is underflow
	bit2: start threshold pass
	bit1: wr point above max threshold
	bit0: wr point below mix threshold
***************************************************/
bool is_afifo_error(void)
{
	bool ret = false;
	if ((hdmirx_rd_top(DWC_AUD_FIFO_STS) &
		(OVERFL_STS | UNDERFL_STS)) != 0) {
		ret = true;
		if (log_level & AUDIO_LOG)
			rx_pr("afifo err\n");
	}
	return ret;
}

/***************************************************
func: is_aud_pll_error
	noraml mode: aud_pll = aud_sample_rate * 128
	HBR: aud_pll = aud_sample_rate * 128 * 4
***************************************************/
bool is_aud_pll_error(void)
{
	bool ret = true;
	int32_t clk = hdmirx_get_audio_clock();
	int32_t aud_128fs = rx.aud_info.real_sample_rate * 128;
	int32_t aud_512fs = rx.aud_info.real_sample_rate * 512;
	if (rx.aud_info.real_sample_rate == 0)
		return false;
	if	((abs(clk - aud_128fs) < 500000) ||
		(abs(clk - aud_512fs) < 500000)) {
		ret = false;
	}
	if ((ret) && (log_level & AUDIO_LOG))
		rx_pr("%d,%d,%d,\n", clk, aud_128fs, aud_512fs);
	return ret;
}

void rx_aud_pll_ctl(bool en)
{
	int tmp = 0;
	if (en) {
		tmp = hdmirx_rd_phy(PHY_MAINFSM_STATUS1);
		wr_reg_hhi(HHI_AUD_PLL_CNTL, 0x20000000);
		/* audio pll div depends on input freq */
		wr_reg_hhi(HHI_AUD_PLL_CNTL6, (tmp >> 9 & 3) << 28);
		/* audio pll div fixed to N/CTS as below*/
		/* wr_reg_hhi(HHI_AUD_PLL_CNTL6, 0x40000000); */
		wr_reg_hhi(HHI_AUD_PLL_CNTL5, 0x0000002e);
		wr_reg_hhi(HHI_AUD_PLL_CNTL4, 0x30000000);
		wr_reg_hhi(HHI_AUD_PLL_CNTL3, 0x00000000);
		wr_reg_hhi(HHI_AUD_PLL_CNTL, 0x40000000);
		wr_reg_hhi(HHI_ADC_PLL_CNTL4, 0x805);
		hdmirx_audio_pll_sw_update();
		External_Mute(0);
	} else{
		/* disable pll, into reset mode */
		External_Mute(1);
		wr_reg_hhi(HHI_AUD_PLL_CNTL, 0x20000000);
	}
}

void hdmirx_get_video_info(void)
{
	const unsigned factor = 100;
	unsigned divider = 0;
	uint32_t tmp = 0;

	/* DVI mode */
	rx.cur.hw_dvi = hdmirx_rd_bits_dwc(DWC_PDEC_STS, DVIDET) != 0;
	if (hdcp22_on) {
		/* hdcp encrypted state */
		tmp = hdmirx_rd_dwc(DWC_HDCP22_STATUS);
		rx.cur.hdcp_type = (tmp >> 4) & 1;
	} else {
		rx.cur.hdcp_type = 0;
	}

	if (rx.cur.hdcp_type == 0) {
		rx.cur.hdcp14_state = (hdmirx_rd_dwc(DWC_HDCP_STS) >> 8) & 3;
		rx.cur.hdcp22_state = 0xff;
	} else {
		rx.cur.hdcp14_state = 0xff;
		rx.cur.hdcp22_state = tmp & 1;
	}
	/* AVI parameters */
	rx.cur.hw_vic =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_PB, VID_IDENT_CODE);
	rx.cur.repeat =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_HB, PIX_REP_FACTOR);
	rx.cur.colorspace =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_PB, VIDEO_FORMAT);

	rx.cur.it_content =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_PB, IT_CONTENT);
	rx.cur.rgb_quant_range =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_PB, RGB_QUANT_RANGE);
	rx.cur.yuv_quant_range =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_HB, YUV_QUANT_RANGE);
	/*
	rx.cur.active_valid =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_PB, ACT_INFO_PRESENT);
	rx.cur.bar_valid =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_PB, BAR_INFO_VALID);
	rx.cur.scan_info =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_PB, SCAN_INFO);
	rx.cur.colorimetry =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_PB, COLORIMETRY);
	rx.cur.picture_ratio =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_PB, PIC_ASPECT_RATIO);
	rx.cur.active_ratio =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_PB, ACT_ASPECT_RATIO);
	rx.cur.it_content =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_PB, IT_CONTENT);
	rx.cur.ext_colorimetry =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_PB, EXT_COLORIMETRY);
	rx.cur.rgb_quant_range =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_PB, RGB_QUANT_RANGE);
	rx.cur.n_uniform_scale =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_PB, NON_UNIF_SCALE);
	rx.cur.hw_vic =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_PB, VID_IDENT_CODE);
	rx.cur.repeat =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_HB, PIX_REP_FACTOR);
	*/
	/** @note HW does not support AVI YQ1-0, */
	/* YCC quantization range */
	/** @note HW does not support AVI CN1-0, */
	/* IT content type */
	/*
	rx.cur.bar_end_top =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_TBB, LIN_END_TOP_BAR);
	rx.cur.bar_start_bottom =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_TBB, LIN_ST_BOT_BAR);
	rx.cur.bar_end_left =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_LRB, PIX_END_LEF_BAR);
	rx.cur.bar_start_right =
		hdmirx_rd_bits_dwc(DWC_PDEC_AVI_LRB, PIX_ST_RIG_BAR);
	*/

	/* refresh rate */
	tmp = hdmirx_rd_bits_dwc(DWC_MD_VTC, VTOT_CLK);
	/* tmp = (tmp == 0)? 0: (ctx->md_clk * 100000) / tmp; */
	/* if((params->vtotal == 0) || (params->htotal == 0)) */
	if (tmp == 0)
		rx.cur.frame_rate = 0;
	else
		rx.cur.frame_rate = (modet_clk * 100000) / tmp;
	/* else { */
		/* params->frame_rate = (hdmirx_get_pixel_clock() /
			(params->vtotal * params->htotal / 100)); */

	/* } */
	/* deep color mode */
	tmp = hdmirx_rd_bits_dwc(DWC_HDMI_STS, DCM_CURRENT_MODE);

	switch (tmp) {
	case DCM_CURRENT_MODE_48b:
		rx.cur.colordepth = E_COLORDEPTH_16;
		/* divide by 2 */
		divider = 2.00 * factor;
		break;
	case DCM_CURRENT_MODE_36b:
		rx.cur.colordepth = E_COLORDEPTH_12;
		divider = 1.50 * factor;	/* divide by 1.5 */
		break;
	case DCM_CURRENT_MODE_30b:
		rx.cur.colordepth = E_COLORDEPTH_10;
		divider = 1.25 * factor;	/* divide by 1.25 */
		break;
	default:
		rx.cur.colordepth = E_COLORDEPTH_8;
		divider = 1.00 * factor;
		break;
	}
	/* pixel clock */
	rx.cur.pixel_clk = hdmirx_get_pixel_clock() / divider;
	/* image parameters */
	rx.cur.interlaced = hdmirx_rd_bits_dwc(DWC_MD_STS, ILACE) != 0;
	rx.cur.voffset = hdmirx_rd_bits_dwc(DWC_MD_VOL, VOFS_LIN);
	rx.cur.vactive = hdmirx_rd_bits_dwc(DWC_MD_VAL, VACT_LIN);
	rx.cur.vtotal = hdmirx_rd_bits_dwc(DWC_MD_VTL, VTOT_LIN);
	rx.cur.hoffset = hdmirx_rd_bits_dwc(DWC_MD_HT1, HOFS_PIX);
	rx.cur.hactive = hdmirx_rd_bits_dwc(DWC_MD_HACT_PX, HACT_PIX);
	rx.cur.htotal = hdmirx_rd_bits_dwc(DWC_MD_HT1, HTOT_PIX);
	rx.cur.pixel_clk = (rx.cur.pixel_clk * factor) / divider;
	rx.cur.hoffset = (rx.cur.hoffset * factor) / divider;
	rx.cur.hactive	= (rx.cur.hactive * factor) / divider;
	rx.cur.htotal = (rx.cur.htotal  * factor) / divider;
}

void hdmirx_set_video_mute(bool mute)
{
	/* bluescreen cfg */
	if (rx.pre.colorspace == E_COLOR_RGB) {
		hdmirx_wr_bits_dwc(DWC_HDMI_VM_CFG_CH2, MSK(16, 0), 0x00);
		hdmirx_wr_bits_dwc(DWC_HDMI_VM_CFG_CH_0_1, MSK(16, 0), 0x00);
	} else if (rx.pre.colorspace == E_COLOR_YUV420) {
		hdmirx_wr_bits_dwc(DWC_HDMI_VM_CFG_CH2, MSK(16, 0), 0x1000);
		hdmirx_wr_bits_dwc(DWC_HDMI_VM_CFG_CH_0_1, MSK(16, 0), 0x8000);
	} else {
		hdmirx_wr_bits_dwc(DWC_HDMI_VM_CFG_CH2, MSK(16, 0), 0x8000);
		hdmirx_wr_bits_dwc(DWC_HDMI_VM_CFG_CH_0_1, MSK(16, 0), 0x8000);
	}
	hdmirx_wr_bits_dwc(DWC_HDMI_VM_CFG_CH2, _BIT(16), mute);
	if (log_level & VIDEO_LOG)
		rx_pr("%s-mute:%d\n", __func__, mute);
}

void hdmirx_config_video(void)
{
	hdmirx_set_video_mute(0);
}

static unsigned int clk_util_clk_msr(unsigned int clk_mux)
{
	return meson_clk_measure(clk_mux);
}

unsigned int hdmirx_get_clock(int index)
{
	return clk_util_clk_msr(index);
}

unsigned int hdmirx_get_tmds_clock(void)
{
	uint32_t clk = clk_util_clk_msr(25);
	if (0 == clk) {
		clk = hdmirx_rd_dwc(DWC_HDMI_CKM_RESULT) & 0xffff;
		clk = clk * 158000 / 4095 * 1000;
		if (log_level & VIDEO_LOG)
			rx_pr("use DWC internal tmds clk msr\n");
	}
	return clk;
}

unsigned int hdmirx_get_pixel_clock(void)
{
	return clk_util_clk_msr(29);
}

unsigned int hdmirx_get_audio_clock(void)
{
	return clk_util_clk_msr(24);
}

unsigned int hdmirx_get_mpll_div_clk(void)
{
	return clk_util_clk_msr(27);
}

unsigned int hdmirx_get_esm_clock(void)
{
	return clk_util_clk_msr(68);
}


