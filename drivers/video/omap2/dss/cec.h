/*
* linux/drivers/video/omap2/dss/cec_util.h
*
* CEC local utility header file
*
* Copyright (C) 2012 Texas Instruments, Inc
* Author: Muralidhar Dixit <murali.dixit@ti.com>
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License version 2 as published by
* the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License along with
* this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef _CEC_PRIV_H_
#define _CEC_PRIV_H_

int cec_claim_module(
		void* data,
		int (*hdmi_runtime_get_cb)(void),
		void (*hdmi_runtime_put_cb)(void),
		void (**hdmi_cec_enable_cb)(int status),
		void (**hdmi_cec_irq_cb)(void),
		void (**hdmi_cec_hpd)(int phy_addr, int status));
void cec_release_module(void);

#endif //_CEC_PRIV_H_
