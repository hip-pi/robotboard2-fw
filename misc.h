/*
	
	Definitions and stuff needed from everywhere
*/

#pragma once

void error(int code);
void delay_us(uint32_t i) __attribute__((section(".text_itcm")));
void delay_ms(uint32_t i) __attribute__((section(".text_itcm")));

// Priority 0 is the highest quick-safety-shutdown level which won't be disabled for atomic operations.
#define DIS_IRQ() do{__set_BASEPRI(1UL << (8 - __NVIC_PRIO_BITS))}while(0)
#define ENA_IRQ() do{__set_BASEPRI(0UL)}while(0)

#define LED_ON()  HI(GPIOC, 13);
#define LED_OFF() LO(GPIOC, 13);
