#include <stdint.h>
#include "ext_include/stm32h7xx.h"
#include "stm32_cmsis_extension.h"
#include "misc.h"
#include "own_std.h"

static char printbuf[128];

#define IMU0_PRESENT
#define IMU1_PRESENT
#define IMU2_PRESENT
#define IMU3_PRESENT
#define IMU4_PRESENT
#define IMU5_PRESENT

#define IMU_SPI_6M25 // 6.25MHz SPI clock. Datasheet maximum: 10MHz

#ifdef IMU_SPI_6M25
#define SPI2_CLKDIV_REGVAL (0b011UL)
#define SPI4_CLKDIV_REGVAL (0b011UL)
#define SPI6_CLKDIV_REGVAL (0b011UL)
#endif


#define IMU024_G_NCS_PORT GPIOA
#define IMU024_G_NCS_PIN  14
#define IMU024_A_NCS_PORT GPIOC
#define IMU024_A_NCS_PIN  10
#define IMU024_M_NCS_PORT GPIOD
#define IMU024_M_NCS_PIN  0

#define IMU135_G_NCS_PORT GPIOD
#define IMU135_G_NCS_PIN  1
#define IMU135_A_NCS_PORT GPIOH
#define IMU135_A_NCS_PIN  15
#define IMU135_M_NCS_PORT GPIOI
#define IMU135_M_NCS_PIN  0

#define SEL_G024()   do{LO(IMU024_G_NCS_PORT,IMU024_G_NCS_PIN); __DSB();}while(0)
#define DESEL_G024() do{HI(IMU024_G_NCS_PORT,IMU024_G_NCS_PIN); __DSB();}while(0)
#define SEL_A024()   do{LO(IMU024_A_NCS_PORT,IMU024_A_NCS_PIN); __DSB();}while(0)
#define DESEL_A024() do{HI(IMU024_A_NCS_PORT,IMU024_A_NCS_PIN); __DSB();}while(0)
#define SEL_M024()   do{LO(IMU024_M_NCS_PORT,IMU024_M_NCS_PIN); __DSB();}while(0)
#define DESEL_M024() do{HI(IMU024_M_NCS_PORT,IMU024_M_NCS_PIN); __DSB();}while(0)

#define SEL_G135()   do{LO(IMU135_G_NCS_PORT,IMU135_G_NCS_PIN); __DSB();}while(0)
#define DESEL_G135() do{HI(IMU135_G_NCS_PORT,IMU135_G_NCS_PIN); __DSB();}while(0)
#define SEL_A135()   do{LO(IMU135_A_NCS_PORT,IMU135_A_NCS_PIN); __DSB();}while(0)
#define DESEL_A135() do{HI(IMU135_A_NCS_PORT,IMU135_A_NCS_PIN); __DSB();}while(0)
#define SEL_M135()   do{LO(IMU135_M_NCS_PORT,IMU135_M_NCS_PIN); __DSB();}while(0)
#define DESEL_M135() do{HI(IMU135_M_NCS_PORT,IMU135_M_NCS_PIN); __DSB();}while(0)


// To convert to two's complement value:
// mask bits 3,2,1 (will contain random values) and 0 (new_data) away.
typedef struct __attribute__((packed))
{
	int16_t raw_x;
	int16_t raw_y;
	int16_t raw_z;
} xcel_fifo_access_rx_t;


/*
	Analysis for required bandwidth and latency:

	Maximum sustained rotational speed 90 deg/s
	Let's assume a meaningful short time maximum (more than a <0.5 degree jerk) angular rate is twice that, 180 deg/s.
	In 5 ms, robot turns 0.9 degrees. When looking at subjects 10 meters away, they shift 
	tan(0.9 degrees)*10000 = 157mm. Seems acceptable given that this is a fairly extreme angular rate and not something
	happening all the time during mapping.

	-> 200Hz is OK.


	Data flow synchronization:

	6 gyros and accelerometers would require 12 interrupt lines to serve in real time without polling. Such number of IOs
	were not available.

	Sensors run on their internal clocks, exact frequency being unspecified and subject to drift.

	Because we want data with low latency, we only want to keep the sensor FIFO fill level to max 1.

	Unfortunately, the register ordering makes it impossible to read the FIFO fill level on the same burst read with the data.

	The sensor is documented to give out all zeroes when reading empty FIFO.

	ISR runs at 10kHz (0.1ms)

	Rate for A: 250Hz (4ms)
	Rate for G: 200Hz (5ms)

*/

#define A_FIFO_READ_ADDR (0x80UL|0x3fUL)
#define G_FIFO_READ_ADDR (0x80UL|0x3fUL)

#if 0

static void start_a024_read()
{
	SEL_IMU024_A();
	__DSB();

	// Make a short critical section, so that the writes start closely matched, in
	// order to get closely matched end-of-transfer interrupts, so that they are likely to tail-chain
	DIS_IRQ();
	*(uint32_t*)&SPI4->TXDR = A_FIFO_READ_ADDR<<24;
	*(uint32_t*)&SPI6->TXDR = A_FIFO_READ_ADDR<<24;
	*(uint32_t*)&SPI2->TXDR = A_FIFO_READ_ADDR<<24;
	ENA_IRQ();
	// add __DMB() here if DIS_IRQ() ENA_IRQ() is removed
	*(uint16_t*)&SPI4->TXDR = 0;
	*(uint16_t*)&SPI6->TXDR = 0;
	*(uint16_t*)&SPI2->TXDR = 0;
}
#endif

typedef union __attribute__((packed))
{
	struct __attribute__((packed))
	{
		uint8_t dummy1;
		int16_t x;
		int16_t y;
		int16_t z;
		uint8_t dummy2;
	} coords;

	struct __attribute__((packed))
	{
		uint32_t first;
		uint32_t second;
	} blocks;
} xyz_in_fifo_t;

#define A_REMOVE_STATUS_BITS(_x_) do{((xyz_in_fifo_t*)&(_x_))->blocks.first &= 0xf0fff000; ((xyz_in_fifo_t*)&(_x_))->blocks.second &= 0x00fff0ff; }while(0)

// Gyro data has no status bits to remove

typedef union __attribute__((packed))
{
	struct __attribute__((packed))
	{
		int16_t x;
		int16_t y;
		int16_t z;
		uint16_t rhall;
	} coords;

	struct __attribute__((packed))
	{
		uint32_t first;
		uint32_t second;
	} blocks;
} m_dataframe_in_fifo_t;

#define M_REMOVE_STATUS_BITS(_x_) do{((m_dataframe_in_fifo_t*)&(_x_))->blocks.first &= 0xfff8fff8; ((xyz_in_fifo_t*)&(_x_))->blocks.second &= 0xfffcfffe; }while(0)


volatile xyz_in_fifo_t latest_a[6];

#if 0
volatile int a024_spis_remaining;

/*
	SPIs are started almost at the same time, and run with synchronous clocks. They should finish at almost the same time.
	It's expected that these inthandlers run tail-chained most of the time.
*/
void a024_any_spi_finished_inthandler()
{
	a024_spis_remaining--;
	if(a024_spis_remaining == 0)
	{
		// All reads finished
	}
}

void a024_read_finished_inthandler()
{
	DESEL_IMU024_A();
	*(uint32_t*)&latest_a[0] = *(uint32_t*)&SPI4->RXDR;
	*(uint16_t*)((void*)&latest_a[0]+4) = *(uint16_t*)&SPI4->RXDR;
	*(uint32_t*)&latest_a[2] = *(uint32_t*)&SPI6->RXDR;
	*(uint16_t*)((void*)&latest_a[2]+4) = *(uint16_t*)&SPI6->RXDR;
	*(uint32_t*)&latest_a[4] = *(uint32_t*)&SPI2->RXDR;
	*(uint16_t*)((void*)&latest_a[4]+4) = *(uint16_t*)&SPI2->RXDR;

}
#endif

/*
	SPI is master. Timing is totally deterministic, minus APB bus uncertainty.
	APB bus will never be heavily loaded. Uncertainty is in range of tens of nanoseconds - hundreds worst.

	SPI clock = 6.25 MHz
	16 bits = 2.56 us
	5 us is surely long enough to read the FIFO fill level

	48 bits = 7.68 us
	10 us is surely long enough to read FIFO content
	
	- Timer int: Issue A0 A2 A4 status read. Set next timer int 5 us from now
	- Timer int: Read A0 A2 A4 status. Issue relevant FIFO reads (where data exists). Set next timer int 10 us from now.
	- Timer int: Read the data. Issue A1 A3 A4 status read, and so on...


	Magnetometer is used in "FORCED" mode, meaning that it doesn't free run, conversion is triggered by us.
	Given the deterministic conversion time, we can just read out the data when we know it's ready.
*/

// Missing numbers will go into default:, which will disable chip select signals and wait additional 1 us.
enum
{
	IDLE = 0,
	START_A024_STATUS_READ = 1,
	// deassert chip selects
	READ_A024_STATUS__START_DATA_READ = 3,
	// deassert chip selects
	READ_A024_DATA__START_A135_STATUS_READ = 5,
	// deassert chip selects
	READ_A135_STATUS__START_DATA_READ = 7,
	// deassert chip selects
	READ_A135_DATA__START_G024_STATUS_READ = 9,
	// deassert chip selects
	READ_G024_STATUS__START_DATA_READ = 11,
	// deassert chip selects
	READ_G024_DATA__START_G135_STATUS_READ = 13,
	// deassert chip selects
	READ_G135_DATA__START_M024_DATA_READ__OR__START_WAIT = 15,
	// deassert chip selects
	READ_M024_DATA__START_M135_DATA_READ = 17,
	// deassert chip selects
	READ_M135_DATA__START_WAIT = 19 // this deasserts chip selects manually (without help of default:)
} cur_state;

#define AGM01_WR32 (*(volatile uint32_t*)&SPI4->TXDR)
#define AGM23_WR32 (*(volatile uint32_t*)&SPI6->TXDR)
#define AGM45_WR32 (*(volatile uint32_t*)&SPI2->TXDR)

#define AGM01_WR16 (*(volatile uint16_t*)&SPI4->TXDR)
#define AGM23_WR16 (*(volatile uint16_t*)&SPI6->TXDR)
#define AGM45_WR16 (*(volatile uint16_t*)&SPI2->TXDR)

#define AGM01_WR8  (*(volatile uint8_t*)&SPI4->TXDR)
#define AGM23_WR8  (*(volatile uint8_t*)&SPI6->TXDR)
#define AGM45_WR8  (*(volatile uint8_t*)&SPI2->TXDR)

#define AGM01_RD32 (*(volatile uint32_t*)&SPI4->RXDR)
#define AGM23_RD32 (*(volatile uint32_t*)&SPI6->RXDR)
#define AGM45_RD32 (*(volatile uint32_t*)&SPI2->RXDR)

#define AGM01_RD16 (*(volatile uint16_t*)&SPI4->RXDR)
#define AGM23_RD16 (*(volatile uint16_t*)&SPI6->RXDR)
#define AGM45_RD16 (*(volatile uint16_t*)&SPI2->RXDR)

#define AGM01_RD8 (*(volatile uint8_t*)&SPI4->RXDR)
#define AGM23_RD8 (*(volatile uint8_t*)&SPI6->RXDR)
#define AGM45_RD8 (*(volatile uint8_t*)&SPI2->RXDR)

#define AG01_READ_CMD() do{AGM01_WR32 = 0x000000bf; __DMB(); AGM01_WR16 = 0x0000; __DMB(); AGM01_WR8 = 0x00; __DMB();}while(0)
#define AG23_READ_CMD() do{AGM23_WR32 = 0x000000bf; __DMB(); AGM23_WR16 = 0x0000; __DMB(); AGM23_WR8 = 0x00; __DMB();}while(0)
#define AG45_READ_CMD() do{AGM45_WR32 = 0x000000bf; __DMB(); AGM45_WR16 = 0x0000; __DMB(); AGM45_WR8 = 0x00; __DMB();}while(0)

#define M01_READ_CMD_PART1() do{AGM01_WR32 = 0x000000c2; __DMB(); AGM01_WR32 = 0x0000; __DMB();}while(0)
#define M23_READ_CMD_PART1() do{AGM23_WR32 = 0x000000c2; __DMB(); AGM23_WR32 = 0x0000; __DMB();}while(0)
#define M45_READ_CMD_PART1() do{AGM45_WR32 = 0x000000c2; __DMB(); AGM45_WR32 = 0x0000; __DMB();}while(0)

#define M01_READ_CMD_PART2() do{AGM01_WR8 = 0x00; __DMB();}while(0)
#define M23_READ_CMD_PART2() do{AGM23_WR8 = 0x00; __DMB();}while(0)
#define M45_READ_CMD_PART2() do{AGM45_WR8 = 0x00; __DMB();}while(0)

#define STATUS_FILL_LEVEL_MASK (0x7f00)

static inline void set_timer(uint16_t tenth_us)
{
	TIM3->ARR = tenth_us-1;
	TIM3->CR1 = 1UL<<3 /* one pulse mode */ | 1UL<<2 /*only over/underflow generates update int*/ | 1UL /* enable*/;
}

#define STATUS_READ_WAIT_TIME 50
#define DATA_READ_WAIT_TIME 100
#define FINAL_WAIT_TIME 50000
#define DESEL_WAIT_TIME 10

volatile int a_status_cnt[6];
volatile int a_nonstatus_cnt[6];

volatile int dbg;


void imu_fsm_inthandler()
{
	TIM3->SR = 0UL;

	static int reading0, reading1, reading2; // was reading started in the previous state? Numbers do not point to sensor indeces
	switch(cur_state)
	{
		case START_A024_STATUS_READ:
		{
			SEL_A024();
			__DSB();
			AGM01_WR16 = 0x008e;
			AGM23_WR16 = 0x008e;
			AGM45_WR16 = 0x008e;
			set_timer(STATUS_READ_WAIT_TIME);
			cur_state++;
		} break;

		case READ_A024_STATUS__START_DATA_READ:
		{
			uint16_t status0 = AGM01_RD16;
			uint16_t status2 = AGM23_RD16;
			uint16_t status4 = AGM45_RD16;

			SEL_A024();
			__DSB();
			reading0 = reading1 = reading2 = 0;
			if(status0 & STATUS_FILL_LEVEL_MASK)
			{
				a_status_cnt[0]++;
				reading0 = 1;
				AG01_READ_CMD();
			}
			else
				a_nonstatus_cnt[0]++;

			if(status2 & STATUS_FILL_LEVEL_MASK)
			{
				a_status_cnt[2]++;
				reading1 = 1;
				AG23_READ_CMD();
			}
			else
				a_nonstatus_cnt[2]++;

			if(status4 & STATUS_FILL_LEVEL_MASK)
			{
				a_status_cnt[4]++;
				reading2 = 1;
				AG45_READ_CMD();
			}
			else
				a_nonstatus_cnt[4]++;

			set_timer(DATA_READ_WAIT_TIME);
			cur_state++;
		} break;

		case READ_A024_DATA__START_A135_STATUS_READ:
		{
			if(reading0)
			{
				latest_a[0].blocks.first  = AGM01_RD32; __DMB();
				latest_a[0].blocks.second = AGM01_RD32;
				A_REMOVE_STATUS_BITS(latest_a[0]);
			}
			if(reading1)
			{
				latest_a[2].blocks.first  = AGM23_RD32; __DMB();
				latest_a[2].blocks.second = AGM23_RD32;
				A_REMOVE_STATUS_BITS(latest_a[2]);
			}
			if(reading2)
			{
				latest_a[4].blocks.first  = AGM45_RD32; __DMB();
				latest_a[4].blocks.second = AGM45_RD32;
				A_REMOVE_STATUS_BITS(latest_a[4]);
			}

			set_timer(FINAL_WAIT_TIME);
			cur_state = START_A024_STATUS_READ;

		} break;

		default:
		{
			DESEL_A024();
			DESEL_G024();
			DESEL_M024();
			DESEL_A135();
			DESEL_G135();
			DESEL_M135();
			set_timer(DESEL_WAIT_TIME);
			cur_state++;
		} break;

	}

	__DSB();

}


static void printings()
{
	DIS_IRQ();
	int a0s = a_status_cnt[0];
	int a2s = a_status_cnt[2];
	int a4s = a_status_cnt[4];
	int a0n = a_nonstatus_cnt[0];
	int a2n = a_nonstatus_cnt[2];
	int a4n = a_nonstatus_cnt[4];
	int a0x = latest_a[0].coords.x;
	int a0y = latest_a[0].coords.y;
	int a0z = latest_a[0].coords.z;
	int a2x = latest_a[2].coords.x;
	int a2y = latest_a[2].coords.y;
	int a2z = latest_a[2].coords.z;
	int a4x = latest_a[4].coords.x;
	int a4y = latest_a[4].coords.y;
	int a4z = latest_a[4].coords.z;
	ENA_IRQ();

	uart_print_string_blocking("a0: "); 
	o_utoa16_fixed(a0s, printbuf); uart_print_string_blocking(printbuf); 
	uart_print_string_blocking(" / "); 
	o_utoa16_fixed(a0n, printbuf); uart_print_string_blocking(printbuf); 
	uart_print_string_blocking("  x="); 
	o_itoa16_fixed(a0x, printbuf); uart_print_string_blocking(printbuf); 
	uart_print_string_blocking("  y="); 
	o_itoa16_fixed(a0y, printbuf); uart_print_string_blocking(printbuf); 
	uart_print_string_blocking("  z="); 
	o_itoa16_fixed(a0z, printbuf); uart_print_string_blocking(printbuf);
	uart_print_string_blocking("\r\n");

	uart_print_string_blocking("a2: "); 
	o_utoa16_fixed(a2s, printbuf); uart_print_string_blocking(printbuf); 
	uart_print_string_blocking(" / "); 
	o_utoa16_fixed(a2n, printbuf); uart_print_string_blocking(printbuf); 
	uart_print_string_blocking("  x="); 
	o_itoa16_fixed(a2x, printbuf); uart_print_string_blocking(printbuf); 
	uart_print_string_blocking("  y="); 
	o_itoa16_fixed(a2y, printbuf); uart_print_string_blocking(printbuf); 
	uart_print_string_blocking("  z="); 
	o_itoa16_fixed(a2z, printbuf); uart_print_string_blocking(printbuf);
	uart_print_string_blocking("\r\n");

	uart_print_string_blocking("a4: "); 
	o_utoa16_fixed(a4s, printbuf); uart_print_string_blocking(printbuf); 
	uart_print_string_blocking(" / "); 
	o_utoa16_fixed(a4n, printbuf); uart_print_string_blocking(printbuf); 
	uart_print_string_blocking("  x="); 
	o_itoa16_fixed(a4x, printbuf); uart_print_string_blocking(printbuf); 
	uart_print_string_blocking("  y="); 
	o_itoa16_fixed(a4y, printbuf); uart_print_string_blocking(printbuf); 
	uart_print_string_blocking("  z="); 
	o_itoa16_fixed(a4z, printbuf); uart_print_string_blocking(printbuf);
	uart_print_string_blocking("\r\n");

	uart_print_string_blocking("dbg = "); 
	o_utoa32_hex(dbg, printbuf); uart_print_string_blocking(printbuf); 

	uart_print_string_blocking("\r\n");
	uart_print_string_blocking("\r\n");
	
}

#define XCEL_RANGE_2G  0b0011
#define XCEL_RANGE_4G  0b0101
#define XCEL_RANGE_8G  0b1000
#define XCEL_RANGE_16G 0b1100
#define XCEL_T16MS_BW31HZ  0b01010
#define XCEL_T8MS_BW63HZ   0b01011
#define XCEL_T4MS_BW125HZ  0b01100
#define XCEL_T2MS_BW250HZ  0b01101
#define XCEL_T1MS_BW500HZ  0b01110

#define WR_REG(_a_, _d_) ( (_a_) | ((_d_)<<8) )
static const uint16_t a_init_seq[] =
{
	WR_REG(0x0f, XCEL_RANGE_2G),
	WR_REG(0x10, XCEL_T16MS_BW31HZ),
	WR_REG(0x3e, 0b01<<6 /*FIFO mode*/ | 0b00 /*X,Y and Z stored*/)
};

#define NUM_ELEM(_tbl_) (sizeof(_tbl_)/sizeof(_tbl_[0]))



#define GYRO_RANGE_2000DPS  0b000
#define GYRO_RANGE_1000DPS  0b001
#define GYRO_RANGE_500DPS   0b010
#define GYRO_RANGE_250DPS   0b011
#define GYRO_RANGE_125DPS   0b100

#define GYRO_ODR100HZ_BW32HZ  0b0111
#define GYRO_ODR200HZ_BW64HZ  0b0110
#define GYRO_ODR100HZ_BW12HZ  0b0101
#define GYRO_ODR200HZ_BW23HZ  0b0100
#define GYRO_ODR400HZ_BW47HZ  0b0011
#define GYRO_ODR1000HZ_BW116HZ 0b0010
#define GYRO_ODR2000HZ_BW230HZ 0b0001
#define GYRO_ODR2000HZ_BW523HZ 0b0000

static const uint16_t g_init_seq[] =
{
	WR_REG(0x0f, GYRO_RANGE_250DPS),
	WR_REG(0x10, GYRO_ODR100HZ_BW32HZ),
	WR_REG(0x3e, 0b01<<6 /*FIFO mode*/ | 0b00 /*X,Y and Z stored*/)
};

#define M_ODR_2HZ  (0b001<<3)
#define M_ODR_6HZ  (0b010<<3)
#define M_ODR_8HZ  (0b011<<3)
#define M_ODR_10HZ (0b000<<3)
#define M_ODR_15HZ (0b100<<3)
#define M_ODR_20HZ (0b101<<3)
#define M_ODR_25HZ (0b110<<3)
#define M_ODR_30HZ (0b111<<3)

#define M_OP_NORMAL (0b00<<1)
#define M_OP_FORCED (0b01<<1)
#define M_OP_SLEEP  (0b11<<1)

// 0 = BOSCH regular preset: 0.5 mA
// 1 = BOSCH enhanced regular preset: 0.8 mA
// 2 = midway between 1 and 3: around 2.5 mA
// 3 = BOSCH high accuracy preset: 4.9 mA
#define M_ACCURACY 1

#if M_ACCURACY == 0
#define M_NUM_XY_REPETITIONS 9
#define M_NUM_Z_REPETITIONS  15
#define M_INTERVAL_MS 100
#endif

#if M_ACCURACY == 1
#define M_NUM_XY_REPETITIONS 15
#define M_NUM_Z_REPETITIONS  27
#define M_INTERVAL_MS 100
#endif

#if M_ACCURACY == 2
#define M_NUM_XY_REPETITIONS 31
#define M_NUM_Z_REPETITIONS  55
#define M_INTERVAL_MS 75
#endif

#if M_ACCURACY == 3
#define M_NUM_XY_REPETITIONS 47
#define M_NUM_Z_REPETITIONS  83
#define M_INTERVAL_MS 50
#endif


#define M_CONV_TIME_US (145*M_NUM_XY_REPETITIONS + 500*M_NUM_Z_REPETITIONS + 980 + 100 /*a bit extra on our own*/)

#define M_NUM_XY_REPETITIONS_REGVAL ((M_NUM_XY_REPETITIONS-1)/2)
#define M_NUM_Z_REPETITIONS_REGVAL  (M_NUM_Z_REPETITIONS-1)

static const uint16_t m_init_seq[] =
{
	WR_REG(0x4b, 1 /*Enable power*/),
	WR_REG(0x4c, M_ODR_10HZ | M_OP_NORMAL),
	WR_REG(0x51, M_NUM_XY_REPETITIONS_REGVAL),
	WR_REG(0x52, M_NUM_Z_REPETITIONS_REGVAL)
};


#include "imu_m_compensation.c"

static void send_sensor_init()
{
	for(int a=0; a < NUM_ELEM(a_init_seq); a++)
	{
		SEL_A024();
		__DSB();
		AGM01_WR16 = a_init_seq[a];
		AGM23_WR16 = a_init_seq[a];
		AGM45_WR16 = a_init_seq[a];
		__DSB();
		delay_us(6);
		DESEL_A024();
		// Clean up the RX fifo of the dummy data:
		AGM01_RD16;
		AGM23_RD16;
		AGM45_RD16;
		__DSB();
		delay_us(4);
	}

	for(int g=0; g < NUM_ELEM(g_init_seq); g++)
	{
		SEL_G024();
		__DSB();
		AGM01_WR16 = g_init_seq[g];
		AGM23_WR16 = g_init_seq[g];
		AGM45_WR16 = g_init_seq[g];
		__DSB();
		delay_us(6);
		DESEL_G024();
		// Clean up the RX fifo of the dummy data:
		AGM01_RD16;
		AGM23_RD16;
		AGM45_RD16;
		__DSB();
		delay_us(4);
	}

	for(int m=0; m < NUM_ELEM(m_init_seq); m++)
	{
		SEL_M024();
		__DSB();
		AGM01_WR16 = m_init_seq[m];
		AGM23_WR16 = m_init_seq[m];
		AGM45_WR16 = m_init_seq[m];
		__DSB();
		delay_us(6);
		DESEL_M024();
		// Clean up the RX fifo of the dummy data:
		AGM01_RD16;
		AGM23_RD16;
		AGM45_RD16;
		__DSB();
		delay_us(4);
		if(m==0)
			delay_ms(5); // Extra delay after the turn-on command (startup time in datasheet: 3ms)
	}

	// Fetch calibration data from the magnetometer:

	SEL_M024();
	AGM01_WR8 = 0x80 | M_CALIB_START_ADDR;
	AGM23_WR8 = 0x80 | M_CALIB_START_ADDR;
	AGM45_WR8 = 0x80 | M_CALIB_START_ADDR;
	__DSB();
	while(!(SPI4->SR & 1)) ;
	while(!(SPI6->SR & 1)) ;
	while(!(SPI2->SR & 1)) ;
	__DSB();

	AGM01_RD8;
	AGM23_RD8;
	AGM45_RD8;
	__DSB();

	for(int m=0; m < sizeof(m_calib[0]); m++)
	{
		AGM01_WR8 = 0;
		AGM23_WR8 = 0;
		AGM45_WR8 = 0;
		__DSB();

		while(!(SPI4->SR & 1)) ;
		while(!(SPI6->SR & 1)) ;
		while(!(SPI2->SR & 1)) ;
		__DSB();

		*(((uint8_t*)&m_calib[0])+m) = AGM01_RD8; __DSB();
		*(((uint8_t*)&m_calib[2])+m) = AGM23_RD8; __DSB();
		*(((uint8_t*)&m_calib[4])+m) = AGM45_RD8; __DSB();
		__DSB();
	}
	DESEL_M024();

	for(int i=0; i<6; i+=2)
	{
		uart_print_string_blocking("\r\nsensor "); o_itoa16(i, printbuf); uart_print_string_blocking(printbuf); uart_print_string_blocking("\r\n");
		uart_print_string_blocking("x1="); o_itoa16(m_calib[i].x1, printbuf); uart_print_string_blocking(printbuf); uart_print_string_blocking("\r\n");
		uart_print_string_blocking("y1="); o_itoa16(m_calib[i].y1, printbuf); uart_print_string_blocking(printbuf); uart_print_string_blocking("\r\n");
		uart_print_string_blocking("z4="); o_itoa16(m_calib[i].z4, printbuf); uart_print_string_blocking(printbuf); uart_print_string_blocking("\r\n");
		uart_print_string_blocking("x2="); o_itoa16(m_calib[i].x2, printbuf); uart_print_string_blocking(printbuf); uart_print_string_blocking("\r\n");
		uart_print_string_blocking("y2="); o_itoa16(m_calib[i].y2, printbuf); uart_print_string_blocking(printbuf); uart_print_string_blocking("\r\n");
		uart_print_string_blocking("z2="); o_itoa16(m_calib[i].z2, printbuf); uart_print_string_blocking(printbuf); uart_print_string_blocking("\r\n");
		uart_print_string_blocking("z1="); o_utoa16(m_calib[i].z1, printbuf); uart_print_string_blocking(printbuf); uart_print_string_blocking("\r\n");
		uart_print_string_blocking("xyz1="); o_utoa16(m_calib[i].xyz1, printbuf); uart_print_string_blocking(printbuf); uart_print_string_blocking("\r\n");
		uart_print_string_blocking("z3="); o_itoa16(m_calib[i].z3, printbuf); uart_print_string_blocking(printbuf); uart_print_string_blocking("\r\n");
		uart_print_string_blocking("xy2="); o_itoa16(m_calib[i].xy2, printbuf); uart_print_string_blocking(printbuf); uart_print_string_blocking("\r\n");
		uart_print_string_blocking("xy1="); o_utoa16(m_calib[i].xy1, printbuf); uart_print_string_blocking(printbuf); uart_print_string_blocking("\r\n");
		uart_print_string_blocking("\r\n");
	}

	

}


/* 

	Three sensors are connected to shared nCS signals, each on separate SPI bus.
	Thus, three sensors (0,2,4 or 1,3,5) are all read simultaneously.

	While the sampling rates are the same, due to free-running oscillators, the FIFO
	fill level on the sensors won't always be the same. This means, sometimes we read
	data from a sensor which has nothing in FIFO. This is acceptable per datasheet, and
	zeroes will be read out. Note that this case is only allowed when "read or burst
	read access time [will not] exceed the sampling time". This won't become an issue
	with our fast SPI comms.




	Idle time between write accesses at least 2 us

	Write operation: 16 bit access
	R/W bit 0 = write
	7 bits  reg addr
	8 bits  data
	MISO is tri-stated by BMX055

	Single-byte read operation: 16 bit access

	TX data:
	R/W bit  1 = read
	7 bits   reg addr
	8 bits   do not care

	RX data:
	8 bits   do not care
	8 bits   data


	Multi-byte read operation: (n+1)*8 bit access

	TX data:
	R/W bit  1 = read
	7 bits   reg addr
	n*8 bits do not care

	RX data:
	8 bits    do not care
	n*8 bits  data from reg_addr, reg_addr+1, and so on

	With 6.25MHz SPI clock, max inter-data idleness setting of 15 clock cycles equals 2.4us idle time
	between write accesses.

*/


void init_imu()
{

	#if defined(IMU0_PRESENT) || defined(IMU1_PRESENT)

		RCC->APB2ENR |= 1UL<<13;

		SPI4->CFG1 = SPI4_CLKDIV_REGVAL<<28 | 1UL<<15 /*TX DMA*/ | 1UL<<14 /*RX DMA*/ |
		             (1/*FIFO threshold*/   -1)<<5 | (8/*bits per frame*/   -1);

		SPI4->CFG2 = 1UL<<29 /*SSOE - another new incorrectly documented trap bit*/ | 1UL<<26 /*Software slave management*/ | 1UL<<22 /*Master*/ |
		             1UL<<25 /*CPOL*/ | 1UL<<24 /*CPHA*/;
		
		SPI4->CR1 =  1UL<<12 /*SSI bit must always be high in software nSS managed master mode*/;

		IO_ALTFUNC(GPIOE,13, 5); // MISO

		IO_ALTFUNC(GPIOE,14, 5); // MOSI
		IO_SPEED(GPIOE,14, 2);

		IO_ALTFUNC(GPIOE,12, 5); // SCK
		IO_SPEED(GPIOE,12, 2);

		SPI4->CR1 |= 1UL;
		SPI4->CR1 |= 1UL<<9;
	#endif

	#if defined(IMU2_PRESENT) || defined(IMU3_PRESENT)

		RCC->APB4ENR |= 1UL<<5;

		SPI6->CFG1 = SPI4_CLKDIV_REGVAL<<28 | 1UL<<15 /*TX DMA*/ | 1UL<<14 /*RX DMA*/ |
		             (1/*FIFO threshold*/   -1)<<5 | (8/*bits per frame*/   -1);

		SPI6->CFG2 = 1UL<<29 /*SSOE - another new incorrectly documented trap bit*/ | 1UL<<26 /*Software slave management*/ | 1UL<<22 /*Master*/ |
		             1UL<<25 /*CPOL*/ | 1UL<<24 /*CPHA*/;


		SPI6->CR1 =  1UL<<12 /*SSI bit must always be high in software nSS managed master mode*/;

		IO_ALTFUNC(GPIOG,12, 5); // MISO

		IO_ALTFUNC(GPIOG,14, 5); // MOSI
		IO_SPEED(GPIOG,14, 2);

		IO_ALTFUNC(GPIOG,13, 5); // SCK
		IO_SPEED(GPIOG,13, 2);

		SPI6->CR1 |= 1UL;
		SPI6->CR1 |= 1UL<<9;

	#endif

	#if defined(IMU4_PRESENT) || defined(IMU5_PRESENT)

		RCC->APB1LENR |= 1UL<<14;

		SPI2->CFG1 = SPI4_CLKDIV_REGVAL<<28 | 1UL<<15 /*TX DMA*/ | 1UL<<14 /*RX DMA*/ |
		             (1/*FIFO threshold*/   -1)<<5 | (8/*bits per frame*/   -1);

		SPI2->CFG2 = 1UL<<29 /*SSOE - another new incorrectly documented trap bit*/ | 1UL<<26 /*Software slave management*/ | 1UL<<22 /*Master*/ |
		             1UL<<25 /*CPOL*/ | 1UL<<24 /*CPHA*/;


		SPI2->CR1 =  1UL<<12 /*SSI bit must always be high in software nSS managed master mode*/;

		IO_ALTFUNC(GPIOI, 2, 5); // MISO

		IO_ALTFUNC(GPIOI, 3, 5); // MOSI
		IO_SPEED(GPIOI, 3, 2);

		IO_ALTFUNC(GPIOI, 1, 5); // SCK
		IO_SPEED(GPIOI, 1, 2);

		SPI2->CR1 |= 1UL;
		SPI2->CR1 |= 1UL<<9;

	#endif

	#if defined(IMU0_PRESENT) || defined(IMU2_PRESENT) || defined(IMU4_PRESENT)
		DESEL_A024();
		DESEL_G024();
		DESEL_M024();
		IO_TO_GPO(IMU024_G_NCS_PORT,IMU024_G_NCS_PIN);
		IO_SPEED (IMU024_G_NCS_PORT,IMU024_G_NCS_PIN, 1);
		IO_TO_GPO(IMU024_A_NCS_PORT,IMU024_A_NCS_PIN);
		IO_SPEED (IMU024_A_NCS_PORT,IMU024_A_NCS_PIN, 1);
		IO_TO_GPO(IMU024_M_NCS_PORT,IMU024_M_NCS_PIN);
		IO_SPEED (IMU024_M_NCS_PORT,IMU024_M_NCS_PIN, 1);
	#endif

	#if defined(IMU1_PRESENT) || defined(IMU3_PRESENT) || defined(IMU5_PRESENT)
		DESEL_A135();
		DESEL_G135();
		DESEL_M135();
		IO_TO_GPO(IMU135_G_NCS_PORT,IMU135_G_NCS_PIN);
		IO_SPEED (IMU135_G_NCS_PORT,IMU135_G_NCS_PIN, 1);
		IO_TO_GPO(IMU135_A_NCS_PORT,IMU135_A_NCS_PIN);
		IO_SPEED (IMU135_A_NCS_PORT,IMU135_A_NCS_PIN, 1);
		IO_TO_GPO(IMU135_M_NCS_PORT,IMU135_M_NCS_PIN);
		IO_SPEED (IMU135_M_NCS_PORT,IMU135_M_NCS_PIN, 1);
	#endif
	

	/*
		TIM3 works as a counter for the interrupt-driven state machine.

		APB1 runs at 100MHz, so timer runs at 200MHz. Even if the APB1 prescaler is changed to 1
		so it runs at 200MHz, the timer will still just run at 200MHz.

		With prescaler = 20, the counter runs at 10MHz - each unit is 0.1 us.
	*/
	RCC->APB1LENR |= 1UL<<1;

	TIM3->CR1 = 1UL<<3 /* one pulse mode */ | 1UL<<2 /*only over/underflow generates update int*/;
	TIM3->DIER |= 1UL; // Update interrupt
	TIM3->PSC = 20;

	send_sensor_init();

	delay_ms(1);

	NVIC_SetPriority(TIM3_IRQn, 5);
	NVIC_EnableIRQ(TIM3_IRQn);

}



void timer_test()
{
	int cnt = 0;
	set_timer(50000);

	while(1)
	{
		printings();

		delay_ms(100);
	}
}


void deinit_imu()
{
	#if defined(IMU0_PRESENT) || defined(IMU1_PRESENT)

		SPI4->CR1 = 0;
		RCC->APB2ENR &= ~(1UL<<13);

		IO_TO_GPI(GPIOE,13); // MISO

		IO_TO_GPI(GPIOE,14); // MOSI
		IO_SPEED(GPIOE,14, 0);

		IO_TO_GPI(GPIOE,12); // SCK
		IO_SPEED(GPIOE,12, 0);
	#endif

	#if defined(IMU2_PRESENT) || defined(IMU3_PRESENT)

		SPI6->CR1 = 0;
		RCC->APB4ENR &= ~(1UL<<5);

		IO_TO_GPI(GPIOG,12); // MISO

		IO_TO_GPI(GPIOG,14); // MOSI
		IO_SPEED(GPIOG,14, 0);

		IO_TO_GPI(GPIOG,13); // SCK
		IO_SPEED(GPIOG,13, 0);
	#endif

	#if defined(IMU4_PRESENT) || defined(IMU5_PRESENT)

		SPI2->CR1 = 0;
		RCC->APB1LENR &= ~(1UL<<14);

		IO_TO_GPI(GPIOI, 2); // MISO

		IO_TO_GPI(GPIOI, 3); // MOSI
		IO_SPEED(GPIOI, 3, 0);

		IO_TO_GPI(GPIOI, 1); // SCK
		IO_SPEED(GPIOI, 1, 0);
	#endif

	#if defined(IMU0_PRESENT) || defined(IMU2_PRESENT) || defined(IMU4_PRESENT)
		IO_TO_GPI(IMU024_G_NCS_PORT,IMU024_G_NCS_PIN);
		IO_SPEED (IMU024_G_NCS_PORT,IMU024_G_NCS_PIN, 0);
		IO_TO_GPI(IMU024_A_NCS_PORT,IMU024_A_NCS_PIN);
		IO_SPEED (IMU024_A_NCS_PORT,IMU024_A_NCS_PIN, 0);
		IO_TO_GPI(IMU024_M_NCS_PORT,IMU024_M_NCS_PIN);
		IO_SPEED (IMU024_M_NCS_PORT,IMU024_M_NCS_PIN, 0);
	#endif

	#if defined(IMU1_PRESENT) || defined(IMU3_PRESENT) || defined(IMU5_PRESENT)
		IO_TO_GPI(IMU135_G_NCS_PORT,IMU135_G_NCS_PIN);
		IO_SPEED (IMU135_G_NCS_PORT,IMU135_G_NCS_PIN, 0);
		IO_TO_GPI(IMU135_A_NCS_PORT,IMU135_A_NCS_PIN);
		IO_SPEED (IMU135_A_NCS_PORT,IMU135_A_NCS_PIN, 0);
		IO_TO_GPI(IMU135_M_NCS_PORT,IMU135_M_NCS_PIN);
		IO_SPEED (IMU135_M_NCS_PORT,IMU135_M_NCS_PIN, 0);
	#endif


}
