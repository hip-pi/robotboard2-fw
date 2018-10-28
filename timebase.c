/*

Timebase: 10kHz handler

*/

#include <stdint.h>
#include "ext_include/stm32h7xx.h"
#include "stm32_cmsis_extension.h"
#include "misc.h"
#include "charger.h"
#include "pwrswitch.h"

volatile uint32_t ms_cnt;

void timebase_inthandler() __attribute__((section(".text_itcm")));
void timebase_inthandler()
{
	static int cnt;
	TIM5->SR = 0;
	__DSB();

	cnt++;

	// For performance, reuse this loop to run the charge pumps as well:

	// See test report in pwrswitch.c - (250us HI, 500us LO) provided the highest Vgs. We'll round that up to 300us HI, 700us LO)

	if(cnt == 7)
	{
		extern int main_power_enabled;
		if(main_power_enabled) 
			PLAT_CP_HI();

		extern int app_power_enabled;
		if(app_power_enabled)
			APP_CP_HI();

		extern int app_precharge_pulsetrain;
		if(app_precharge_pulsetrain > 0)
		{
//			DIS_IRQ();
//			APP_DIS_DESAT_PROT();
//			delay_us(8);
//			APP_EN_DESAT_PROT();
//			ENA_IRQ();
			app_precharge_pulsetrain--;
		}


		pwrswitch_1khz();
	}
	else if(cnt >= 10)
	{
		PLAT_CP_LO();
		APP_CP_LO();
		ms_cnt++;
		cnt = 0;
	}


	// Keep all functions in ITCM!
	charger_10khz();
}

void init_timebase()
{

	/*
		TIM5 @ APB1. D2PPRE1 = 0b100 (/2), and TIMPRE=0, hence
		counter runs at double the APB1 clk = 100*2 = 200 MHz.

		Note that if the bus clock is doubled to 200MHz by reducing
		the D2PPRE1 divider to 1, everything works automatically,
		and the counter still runs at 200MHz.
	*/

	RCC->APB1LENR |= 1UL<<3;

	TIM5->DIER |= 1UL; // Update interrupt
	TIM5->ARR = 20000-1; // 200MHz -> 10 kHz
	TIM5->CR1 |= 1UL; // Enable

	NVIC_SetPriority(TIM5_IRQn, 10);
	NVIC_EnableIRQ(TIM5_IRQn);
}

void deinit_timebase()
{
	TIM5->DIER = 0;
	TIM5->CR1 = 0;
	TIM5->SR = 0;

	NVIC_DisableIRQ(TIM5_IRQn);
	RCC->APB1LENR &= ~(1UL<<3);
}
