#ifndef SX_MASTER_SLAVE_H
#define SX_MASTER_SLAVE_H
#include "sx_async_uart.h"
void sx_master_slave_init(void);
void sx_task_master_slave(void *pvParameter);
#endif /* SX_MASTER_SLAVE_H */
