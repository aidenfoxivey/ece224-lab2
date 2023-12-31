#include <stdint.h>
#include <stdio.h>

#include "altera_avalon_pio_regs.h"
#include "sys/alt_irq.h"
#include "system.h"

#define ENABLE_REG_OFFSET 0
#define BUSY_REG_OFFSET 1
#define PERIOD_REG_OFFSET 2
#define PULSE_WIDTH_REG_OFFSET 3

void enable_egm(int period);
void disable_egm();
void print_statistics(int period, int tasks_run);
static void response_interrupt(void *context, alt_u32 id);
int background();

int main() {
	disable_egm();

	/* Run background function to warm it up */
	background();

	/* Poll method to use:  0 -> interrupts, 1 -> tight polling */
	int interrupts_or_polling = IORD(SWITCH_PIO_BASE, 0);
	if (interrupts_or_polling == 0) {
		printf("Interrupt mode selected.\n");
	} else {
		printf("Tight polling mode selected.\n");
	}

	printf("Please, press PB0 to continue.\n");
	int button = IORD(BUTTON_PIO_BASE,0);
	while(button != 14) {
		button = IORD(BUTTON_PIO_BASE,0);
	}

	/* print CSV header to stdout */
	printf("period,pulse_width,tasks_run,latency,missed,multiple\n");

	if (interrupts_or_polling == 0) {
		/* =============== INTERRUPTS BEGIN =============== */
		alt_irq_register(STIMULUS_IN_IRQ, (void*)0, response_interrupt);

		/* enable interrupt */
		IOWR(STIMULUS_IN_BASE, 2, 0xFFFF);

		for (int period = 2; period <= 5000; period += 2) {
			IOWR(LED_PIO_BASE, 0, 0x2);
			IOWR(LED_PIO_BASE, 0, 0x0);
			enable_egm(period);

			int background_count = 0;

			int isBusy = IORD(EGM_BASE, BUSY_REG_OFFSET);
			while (isBusy == 1) {
				IOWR(LED_PIO_BASE, 0, 0x1);
				background();
				IOWR(LED_PIO_BASE, 0, 0x0);
				background_count++;
				isBusy = IORD(EGM_BASE, BUSY_REG_OFFSET);
			}

			print_statistics(period, background_count);
			disable_egm();
		}
		/* =============== INTERRUPTS END =============== */
	} else {
		/* =============== TIGHT POLLING BEGIN =============== */
		int period;

		for (period = 2; period <= 5000; period += 2) {
			IOWR(LED_PIO_BASE, 0, 0x2);
			IOWR(LED_PIO_BASE, 0, 0x0);
			int background_count = 0;
			int characterization_count = 0;
			int allowed_count = 0;
			/* characterize the cycle */
			/* While there is no stimulus in from the EGM,
			 * run background task and count the number we
			 * can fit in a pulse width (when stimulus is 0).
			 * */

			enable_egm(period);

			if (IORD(EGM_BASE, BUSY_REG_OFFSET) == 1) {
				while(IORD(STIMULUS_IN_BASE, 0) == 0) {}
				IOWR(RESPONSE_OUT_BASE, 0, 1);
				IOWR(RESPONSE_OUT_BASE, 0, 0);
				while(IORD(STIMULUS_IN_BASE, 0) == 1) {
					IOWR(LED_PIO_BASE, 0, 0x1);
					background();
					IOWR(LED_PIO_BASE, 0, 0x0);
					characterization_count++;
				}
				while(IORD(STIMULUS_IN_BASE, 0) == 0) {
					IOWR(LED_PIO_BASE, 0, 0x1);
					background();
					IOWR(LED_PIO_BASE, 0, 0x0);
					characterization_count++;
				}
			}

			 /* Allowed count is characterization count subtracted by 1
			  * because we want to count how many background tasks we can
			  * run before the stimulus turns to a 1.
			  * */
			allowed_count = characterization_count - 1;

			/* while busy bit is running */
			while (IORD(EGM_BASE, BUSY_REG_OFFSET) == 1) {
				/* Write response to EGM. */
				IOWR(RESPONSE_OUT_BASE, 0, 1);
				IOWR(RESPONSE_OUT_BASE, 0, 0);

				/* Run the background task for the specified allowed times. */
				for (int i = 0; i < allowed_count; i++) {
					background();
					background_count++;
					if (IORD(EGM_BASE, BUSY_REG_OFFSET) == 0) {
						break;
					}
				}

				while (IORD(STIMULUS_IN_BASE, 0) == 0) {
					if (IORD(EGM_BASE, BUSY_REG_OFFSET) == 0) {
						break;
					}
				}
			}

			print_statistics(period, background_count);
			disable_egm();
		}
		/* =============== TIGHT POLLING END =============== */
	}

	return 0;
}

void print_statistics(int period, int tasks_run) {
	int pulse_width = period / 2;
	int latency = IORD(EGM_BASE, 4);
	int missed = IORD(EGM_BASE, 5);
	int multiple = IORD(EGM_BASE, 6);
	printf("%d,%d,%d,%d,%d,%d\n", period, pulse_width, tasks_run, latency, missed, multiple);
}

void enable_egm(int period) {
  int pulse_width = period / 2;

  // Configure EGM period and pulse width
  IOWR(EGM_BASE, PERIOD_REG_OFFSET, period);
  IOWR(EGM_BASE, PULSE_WIDTH_REG_OFFSET, pulse_width);

  // Enable EGM
  IOWR(EGM_BASE, ENABLE_REG_OFFSET, 1);
}

void disable_egm() { IOWR(EGM_BASE, ENABLE_REG_OFFSET, 0); }

static void response_interrupt(void *context, alt_u32 id) {
	IOWR(LED_PIO_BASE, 0, 0x4);
	IOWR(RESPONSE_OUT_BASE, 0, 0x1);
	IOWR(RESPONSE_OUT_BASE, 0, 0x0);
	IOWR(STIMULUS_IN_BASE, 3, 0x0);
	IOWR(LED_PIO_BASE, 0, 0x0);
}

int background() {
	int j;
	int x = 0;
	int grainsize = 4;
	int g_taskProcessed = 0;

	for (j=0; j < grainsize; j++) {
		g_taskProcessed++;
	}

	return x;
}



