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
#include <stdint.h>

// correctly list the interrupts
#include "sys/alt_irq.h"
// static void name_of_ISR (void* context, alt_u32 id) {}
// declare global but also volatile in C code body

//There are four key elements required in code to use interrupts:
//1) An include statement,
//2) an ISR (which does basic processing and also clears the interrupt at the end of the ISR),
//3) A call to register the interrupt in the main c-code,
//4) Enabling/Disabling the interrupt by the “main” c-code inside the peripheral core (see core
//datasheet) at appropriate times for the application. This usually involves an Interrupt Mask.

#include "system.h"
#include "altera_avalon_pio_regs.h"

int main()
{
	printf("Hello from Nios II!\n");

	uint8_t button;
	uint8_t test_switch;

	while(1) {
		//button = IORD(BUTTON_PIO_BASE, 0);
		test_switch = IORD(SWITCH_PIO_BASE, 0);

		//printf("Button Value: %d\n", button);
		printf("Switch Value: %d\n", test_switch);
	}

	return 0;
}
