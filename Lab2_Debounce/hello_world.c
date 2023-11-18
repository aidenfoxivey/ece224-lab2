/*
 * "Hello World" example.
 *
 * This example prints 'Hello from Nios II' to the STDOUT stream. It runs on
 * the Nios II 'standard', 'full_featured', 'fast', and 'low_cost' example
 * designs. It runs with or without the MicroC/OS-II RTOS and requires a STDOUT
 * device in your system's hardware.
 * The memory footprint of this hosted application is ~69 kbytes by default
 * using the standard reference design.
 *
 * For a reduced footprint version of this template, and an explanation of how
 * to reduce the memory footprint for a given application, see the
 * "small_hello_world" template.
 *
 */

#include <stdio.h>
#include <io.h>
#include <sys/alt_irq.h>
#include "altera_avalon_timer.h"
#include "altera_avalon_timer_regs.h"
#include "system.h"

static void handle_timer_interrupt(void *context, alt_32 id);
static void btn_interrupt(void *context, alt_32 id);
int button_pressed = 0;
int count_released = 0;
int button = 0;

int main()
{
	//IOWR(BUTTON_PIO_BASE, 3, 0);
	//IOWR(TIMER_0_BASE, 0, 0);


	//IOWR(TIMER_0_BASE, 1, 0x0);
	alt_irq_register(TIMER_0_IRQ, (void*)0, handle_timer_interrupt);
	alt_irq_register(BUTTON_PIO_IRQ, (void*)0, btn_interrupt);
	IOWR(BUTTON_PIO_BASE, 3, 0);
	IOWR(BUTTON_PIO_BASE, 2, 0xF);
	IOWR_ALTERA_AVALON_TIMER_STATUS(TIMER_0_BASE, 0);
	IOWR_ALTERA_AVALON_TIMER_PERIODH(TIMER_0_BASE, 0x2);
	IOWR_ALTERA_AVALON_TIMER_PERIODL(TIMER_0_BASE, 0xFFFF);
	IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x8);

	int x = 0;
	while(1){
		//printf("\n%i", x);
		//x++;

		if (button_pressed == 7){
			printf("pb 3\n");
			button_pressed = 0;
		}
		else if (button_pressed == 11){
			printf("pb 2\n");
			button_pressed = 0;
		}
		else if (button_pressed == 13){
			printf("pb 1\n");
			button_pressed = 0;
		}
		else if (button_pressed == 14){
			printf("pb 0\n");
			button_pressed = 0;
		}
		//else
			//printf("no pressed");

	}

	return 0;
}

static void handle_timer_interrupt(void* context, alt_32 id){
	int current_value = IORD(BUTTON_PIO_BASE, 0);
	IOWR(LED_PIO_BASE, 0, 0x1);
	if (button == 0 && current_value != 0xF){
		button_pressed = current_value;
		button = 1;
	}
	if(current_value == 0xF){
		count_released++;
		if (count_released < 20)
			IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5);
	}
	else{
		count_released = 0;
		IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5);
	}
	if (count_released == 20){

			IOWR(LED_PIO_BASE, 0, 0x0);
			count_released = 0;
			button = 0;
			IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x8);
			IOWR(BUTTON_PIO_BASE, 2, 0xF);
	}
}

static void btn_interrupt(void *context, alt_32 id){
	IOWR(BUTTON_PIO_BASE, 3, 0);
	IOWR(BUTTON_PIO_BASE, 2, 0);
	IOWR_ALTERA_AVALON_TIMER_CONTROL(TIMER_0_BASE, 0x5);
}
