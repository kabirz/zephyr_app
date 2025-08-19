#include <stdio.h>
#include <zephyr/kernel.h>

extern void slave_proccess(void);

int main(void)
{
	printf("Hello World! %s\n", CONFIG_BOARD_TARGET);
	while (true) {
		slave_proccess();
	}

	return 0;
}
