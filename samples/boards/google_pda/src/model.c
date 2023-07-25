/*
 * Copyright (c) 2022 The Chromium OS Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

#include <stm32g0xx_ll_bus.h>
#include <stm32g0xx_ll_system.h>
#include <stm32g0xx_ll_dma.h>
#include <stm32g0xx_ll_rcc.h>
#include <stm32g0xx_ll_gpio.h>
#include <stm32g0xx_ll_ucpd.h>

#include "crc32.h"
#include "meas.h"
#include "controls.h"
#include "view.h"
#include "model.h"

#include "LL_UCPD_PATCH.h"

/* STM32 interrupt registers */
#define UCPD_IRQ		8
#define DMA1_CHANNEL1_IRQ	9
#define DMA1_CHANNEL1_PRIO	2
#define UCPD_PRIO		2

/* snooper model thread parameters */
#define MODEL_THREAD_STACK_SIZE 500
#define MODEL_THREAD_PRIORITY 5

/* STM32 UCPD parameters */
static LL_UCPD_InitTypeDef ucpd_params;

/* Byte size of various portions of the packet */
#define MOD_BUFFERS 40
#define PACKET_HEADER_LEN 20
#define PD_SAMPLES 488
#define CRC_LEN 4
#define PACKET_BYTE_SIZE (PACKET_HEADER_LEN + PD_SAMPLES + CRC_LEN)
#define MAX_PACKET_XFER_SIZE 64

/* container for information of the type of packet to be sent */
struct packet_type_t {
	uint16_t unused1: 4;
	uint16_t polarity: 2;
	uint16_t lost: 1;
	uint16_t partial: 1;
	uint16_t version: 4;
	uint16_t type: 4;
};

/* container for header of the packet */
struct header_t {
	uint32_t sequence;
	uint16_t cc1_voltage;
	uint16_t cc2_voltage;
	uint16_t vcon_current;
	uint16_t vbus_voltage;
	uint16_t vbus_current;
	struct packet_type_t packet_type;
	uint16_t data_len;
	uint16_t unused;
};

/* container for the entire packet */
struct packet_t {
	struct header_t header;
	uint8_t data[PD_SAMPLES];
	uint32_t crc;
};

/* storage for all the information of the current state of the snooper */
static struct model_t {
	const struct device *dev;
	struct packet_t packet;
	uint8_t dma_buffer[PD_SAMPLES];
	k_tid_t tid;
	bool start;
	bool empty_print;
	bool slow_print;
	bool auto_stop;

	uint8_t mod_buff[MOD_BUFFERS][PD_SAMPLES];
	uint16_t mod_size[MOD_BUFFERS];
	struct packet_type_t sop[MOD_BUFFERS];
	uint8_t mw;
	uint8_t mr;
	uint32_t sleep_time;
} model;

K_THREAD_STACK_DEFINE(model_stack_area, MODEL_THREAD_STACK_SIZE);
static struct k_thread model_thread_data;
static int pd_line;

void start_snooper(bool s)
{
	model.start = s;
	if (s) {
		model.packet.header.sequence = 0;
		view_set_snoop(CC1_CHANNEL_BIT | CC2_CHANNEL_BIT);
	} else {
		view_set_snoop(0);
	}
}

void reset_snooper() {
	memset(&model.mod_buff, 0, MOD_BUFFERS * PD_SAMPLES);
	memset(&model.mod_size, 0, MOD_BUFFERS * sizeof(uint16_t));
	memset(&model.sop, 0, MOD_BUFFERS * sizeof(uint16_t));
	model.mr = 0;
	model.mw = 0;
	model.packet.header.sequence = 0;
}

void set_role(snooper_mask_t role_mask) {
	if (role_mask & SINK_BIT) {
		LL_UCPD_SetccEnable(UCPD1, LL_UCPD_CCENABLE_CC1CC2);
		LL_UCPD_SetSNKRole(UCPD1);
		return;
	}

	if (role_mask & CC1_CHANNEL_BIT) {
		LL_UCPD_SetccEnable(UCPD1, LL_UCPD_CCENABLE_CC1);
	}
	else if (role_mask & CC2_CHANNEL_BIT) {
		LL_UCPD_SetccEnable(UCPD1, LL_UCPD_CCENABLE_CC2);
	}
	LL_UCPD_SetSRCRole(UCPD1);

	uint8_t Rp = role_mask & PULL_RESISTOR_BITS;

	switch (Rp) {
	case 0:
		LL_UCPD_SetRpResistor(UCPD1, LL_UCPD_RESISTOR_NONE);
		break;
	case 1:
		LL_UCPD_SetRpResistor(UCPD1, LL_UCPD_RESISTOR_DEFAULT);
		break;
	case 2:
		LL_UCPD_SetRpResistor(UCPD1, LL_UCPD_RESISTOR_1_5A);
		break;
	case 3:
		LL_UCPD_SetRpResistor(UCPD1, LL_UCPD_RESISTOR_3_0A);
	}

}

void set_empty_print(bool e) {
	model.empty_print = e;
}

void set_sleep_time(uint32_t st) {
	model.sleep_time = st;
}

void set_auto_stop(bool s) {
	model.auto_stop = s;
}

#define CC_VOLTAGE_LOW   500
#define CC_VOLTAGE_HIGH  2000

static void model_thread(void *arg1, void *arg2, void *arg3)
{
	int32_t vbus_v, vbus_c;
	int32_t vcon_c;
	int32_t cc1_v, cc2_v;
	struct model_t *sm = (struct model_t *)arg1;

	while (1) {
		if (sm->start) {
			sm->packet.header.sequence++;

			if (sm->packet.header.sequence % 10 == 0) {
				meas_vbus_v(&vbus_v);
				meas_vbus_c(&vbus_c);
				meas_cc1_v(&cc1_v);
				meas_cc2_v(&cc2_v);
				meas_vcon_c(&vcon_c);

				/* because the twinkie itself is a port, detecting a
				 * valid connection through the UCPD line will cause
				 * false positives. e.g. if the line being snooped is a
				 * source to source connection and the twinkie is set as
				 * a sink, the twinkie UCPD will incorrectly detect a
				 * valid connection. So the connection has to be
				 * detected using the ADC pins.
				 */
				if ((cc1_v < CC_VOLTAGE_HIGH) && (cc1_v > CC_VOLTAGE_LOW)) {
					/*connect to non-active line if the active line is not set to view*/
					if (get_view_snoop() & CC1_CHANNEL_BIT)
						LL_UCPD_SetCCPin(UCPD1, LL_UCPD_CCPIN_CC1);
					else
						LL_UCPD_SetCCPin(UCPD1, LL_UCPD_CCPIN_CC2);
					view_set_connection(CC1_CHANNEL_BIT);
					pd_line = 1;
				}
				else if ((cc2_v < CC_VOLTAGE_HIGH) && (cc2_v > CC_VOLTAGE_LOW)) {
					/*connect to non-active line if the active line is not set to view*/
					if (get_view_snoop() & CC2_CHANNEL_BIT)
						LL_UCPD_SetCCPin(UCPD1, LL_UCPD_CCPIN_CC2);
					else
						LL_UCPD_SetCCPin(UCPD1, LL_UCPD_CCPIN_CC1);
					view_set_connection(CC2_CHANNEL_BIT);
					pd_line = 2;
				}
				else {
					view_set_connection(0);
				}
			}
			if (sm->packet.header.sequence % 10 < 8){
				sm->packet.header.vbus_voltage = vbus_v;
				sm->packet.header.vbus_current = vbus_c;
				sm->packet.header.cc1_voltage = cc1_v;
				sm->packet.header.cc2_voltage = cc2_v;
				sm->packet.header.vcon_current = vcon_c;

				/* put pd message in the packet if any are stored */
				if (sm->mw != sm->mr) {
					sm->packet.header.packet_type = sm->sop[sm->mr];
					sm->packet.header.data_len = sm->mod_size[sm->mr];
					memcpy(sm->packet.data, sm->mod_buff[sm->mr], sm->mod_size[sm->mr]);
					memset((uint8_t *)&sm->sop[sm->mr], 0, sizeof(uint16_t));
					sm->mod_size[sm->mr] = 0;
					memset(sm->mod_buff[sm->mr], 0, PD_SAMPLES);
					sm->mr++;
					if (sm->mr == MOD_BUFFERS) {
						sm->mr = 0;
					}
				}

				crc32_init();
				crc32_hash((uint8_t *)&sm->packet, 508);
				sm->packet.crc = crc32_result();
				if (sm->empty_print || sm->packet.header.data_len != 0) {
					int stop_timer = 0;
					for (int i = 0, w = 0; i < PACKET_BYTE_SIZE; i += w) {
						w = uart_fifo_fill(sm->dev, (const uint8_t *)&sm->packet + i, PACKET_BYTE_SIZE - i);
						if (w <= 0) {
							stop_timer++;
							k_usleep(500);
	//						LOG_ERR("ERROR: No receiver connected. Twinkie turning off.");
							if (stop_timer > 100 && sm->auto_stop)
							{
								start_snooper(false);
								break;
							}
						}
					}
				}
				memset(sm->packet.data, 0, PD_SAMPLES);
				sm->packet.header.data_len = 0;
			}
		}
		if (sm->mw == sm->mr)
		{
			k_usleep(sm->sleep_time);
		}
	}
}

#define MY_DEV_IRQ 12
#define MY_DEV_PRIO  2
#define MY_ISR_ARG  NULL
#define MY_IRQ_FLAGS 0

void ucpd_isr(void)
{
	struct model_t *sm = &model;

	/* TypeCEvent flag currently not used */
	if (LL_UCPD_IsActiveFlag_TypeCEventCC1(UCPD1) || LL_UCPD_IsActiveFlag_TypeCEventCC2(UCPD1)) {
		LL_UCPD_ClearFlag_TypeCEventCC1(UCPD1);
		LL_UCPD_ClearFlag_TypeCEventCC2(UCPD1);
	}

	if (LL_UCPD_IsActiveFlag_RxErr(UCPD1)) {
		LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_1);
		LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_1, PD_SAMPLES);
		memcpy(sm->mod_buff[sm->mw], sm->dma_buffer, PD_SAMPLES);
		memset(sm->dma_buffer, 0, PD_SAMPLES);
		sm->mod_size[sm->mw] = LL_UCPD_ReadRxPaySize(UCPD1);
		sm->sop[sm->mr].type = LL_UCPD_ReadRxOrderSet(UCPD1);
		sm->sop[sm->mw].polarity = pd_line;
		sm->sop[sm->mw].partial = true;
		sm->mw++;
		if (sm->mw == MOD_BUFFERS) {
			sm->mw = 0;
		}
		LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);
		k_wakeup(sm->tid);
	}

	if (LL_UCPD_IsActiveFlag_RxMsgEnd(UCPD1)) {
		LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_1);
		LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_1, PD_SAMPLES);
		memcpy(sm->mod_buff[sm->mw], sm->dma_buffer, PD_SAMPLES);
		memset(sm->dma_buffer, 0, PD_SAMPLES);
		sm->mod_size[sm->mw] = LL_UCPD_ReadRxPaySize(UCPD1);
		sm->sop[sm->mr].type = LL_UCPD_ReadRxOrderSet(UCPD1);
		sm->sop[sm->mw].polarity = pd_line;
		sm->sop[sm->mw].partial = false;
		sm->mw++;
		if (sm->mw == MOD_BUFFERS) {
			sm->mw = 0;
		}
		LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);
//		k_wakeup(sm->tid);
		LL_UCPD_ClearFlag_RxMsgEnd(UCPD1);
	}
}

static void update_stm32g0x_cc_line(void)
{
	SYSCFG->CFGR1 |= SYSCFG_CFGR1_UCPD1_STROBE_Msk;
}

enum pd_cc_t {
	PD_OFF,
	PD_BOTH,
	PD_CC1,
	PD_CC2
};

static void pd_on_cc(enum pd_cc_t p)
{
	switch (p) {
	case PD_OFF:
		LL_UCPD_TypeCDetectionCC1Disable(UCPD1);
		LL_UCPD_TypeCDetectionCC2Disable(UCPD1);
		break;
	case PD_BOTH:
		LL_UCPD_TypeCDetectionCC1Enable(UCPD1);
		LL_UCPD_TypeCDetectionCC2Enable(UCPD1);
		break;
	case PD_CC1:
		LL_UCPD_TypeCDetectionCC2Disable(UCPD1);
		LL_UCPD_TypeCDetectionCC1Enable(UCPD1);
		break;
	case PD_CC2:
		LL_UCPD_TypeCDetectionCC1Disable(UCPD1);
		LL_UCPD_TypeCDetectionCC2Enable(UCPD1);
		break;
	}
}

int model_init(const struct device *dev)
{
	int ret;

	model.dev = dev;
	model.mw = 0;
	model.mr = 0;

	update_stm32g0x_cc_line();

	/** Configure CC pins */

	/* Set PIN A8 as analog */
	LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_8, LL_GPIO_MODE_ANALOG);

	/* Set PIN B15 as analog */
	LL_GPIO_SetPinMode(GPIOB, LL_GPIO_PIN_15, LL_GPIO_MODE_ANALOG);

	/** Configure DMA */

	/* DMA CLOCK */
	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

	LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_1);

	LL_DMA_ConfigTransfer(DMA1, LL_DMA_CHANNEL_1, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);

	/* DMA from UCPD RXDR register */
	LL_DMA_ConfigAddresses(DMA1, LL_DMA_CHANNEL_1, (uint32_t)&UCPD1->RXDR, (uint32_t)model.dma_buffer, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);

	LL_DMA_SetMode(DMA1, LL_DMA_CHANNEL_1, LL_DMA_MODE_NORMAL);

	LL_DMA_SetPeriphIncMode(DMA1, LL_DMA_CHANNEL_1, LL_DMA_PERIPH_NOINCREMENT);

	LL_DMA_SetMemoryIncMode(DMA1, LL_DMA_CHANNEL_1, LL_DMA_MEMORY_INCREMENT);

	LL_DMA_SetPeriphSize(DMA1, LL_DMA_CHANNEL_1, LL_DMA_PDATAALIGN_BYTE);

	LL_DMA_SetMemorySize(DMA1, LL_DMA_CHANNEL_1, LL_DMA_MDATAALIGN_BYTE);

	LL_DMA_SetChannelPriorityLevel(DMA1, LL_DMA_CHANNEL_1, LL_DMA_PRIORITY_VERYHIGH);

	LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_1, PD_SAMPLES);

	LL_DMA_SetPeriphRequest(DMA1, LL_DMA_CHANNEL_1, LL_DMAMUX_REQ_UCPD1_RX);

	LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);

	model.packet.header.sequence = 0;

	/** Configure UCPD */

	ucpd_params.psc_ucpdclk = 0;
	ucpd_params.transwin = 7;
	ucpd_params.IfrGap = 16;
	ucpd_params.HbitClockDiv = 26;

	/*
	 * The UCPD port is disabled in the LL_UCPD_Init function
	 *
	 * NOTE: For proper Power Management operation, this function
	 *       should not be used because it circumvents the zephyr
	 *       clock API. Instead, DTS clock settings and the zephyr
	 *       clock API should be used to enable clocks.
	 */
	ret = LL_UCPD_Init(UCPD1, &ucpd_params);
	if (ret == SUCCESS) {
		/* ORDSET */
		LL_UCPD_SetRxOrderSet(UCPD1, LL_UCPD_ORDERSET_SOP |
					     LL_UCPD_ORDERSET_SOP1 |
//					     LL_UCPD_RXORDSET_SOP1 |
					     LL_UCPD_ORDERSET_SOP2 |
//					     LL_UCPD_RXORDSET_SOP2 |
//					     LL_UCPD_ORDERSET_SOP1_DEBUG |
//					     LL_UCPD_ORDERSET_SOP2_DEBUG |
					     LL_UCPD_ORDERSET_HARDRST |
					     LL_UCPD_ORDERSET_CABLERST);

		/* ENABLE DMA */
		LL_UCPD_RxDMAEnable(UCPD1);

		/* Enable UCPD port */
		LL_UCPD_Enable(UCPD1);
		start_snooper(false);

		pd_on_cc(PD_BOTH);
		update_stm32g0x_cc_line();

		LL_UCPD_EnableIT_RxNE(UCPD1);

		LL_UCPD_EnableIT_TypeCEventCC1(UCPD1);
		LL_UCPD_EnableIT_TypeCEventCC2(UCPD1);
		LL_UCPD_ClearFlag_TypeCEventCC1(UCPD1);
		LL_UCPD_ClearFlag_TypeCEventCC2(UCPD1);

		LL_UCPD_SetccEnable(UCPD1, LL_UCPD_CCENABLE_CC1CC2);

		LL_UCPD_EnableIT_RxMsgEnd(UCPD1);
		LL_UCPD_ClearFlag_RxMsgEnd(UCPD1);

		LL_UCPD_RxEnable(UCPD1);
	} else {
		return -EIO;
	}

	LL_UCPD_SetSNKRole(UCPD1);

	en_cc1(true);
	en_cc2(true);

	set_auto_stop(true);
	set_empty_print(true);
	set_sleep_time(500);


	//crc32_init();

	model.tid = k_thread_create(&model_thread_data, model_stack_area,
			K_THREAD_STACK_SIZEOF(model_stack_area),
			model_thread,
			&model, NULL, NULL,
			MODEL_THREAD_PRIORITY, 0, K_NO_WAIT);

	return 0;
}
