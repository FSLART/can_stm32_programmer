#ifndef BOOT_PORT_H
#define BOOT_PORT_H
#include "stm32f4xx_hal.h"
int boot_flash_init(void);
void boot_run(CAN_HandleTypeDef *can);
#endif
