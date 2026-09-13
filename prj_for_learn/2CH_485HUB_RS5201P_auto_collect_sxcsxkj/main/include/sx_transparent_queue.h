#ifndef _TRANSPARENT_QUEUE_H_
#define _TRANSPARENT_QUEUE_H_

#include <stdlib.h>

void sx_transparent_queue_init(void);
void sx_transparent_queue_stop(void);
void transparent_task(void *pvParameter);

#endif
