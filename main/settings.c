
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "log.h"
#include "settings.h"

struct lightctl_settings settings;
static SemaphoreHandle_t sem = NULL;
static const char *TAG = "settings";

void settings_lock(void)
{
	while (!xSemaphoreTake(sem, pdMS_TO_TICKS(10)))
		vTaskDelay(1);
}

void settings_unlock(void)
{
	xSemaphoreGive(sem);
}

void settings_init(void)
{
	if ((sem = xSemaphoreCreateBinary()))
		xSemaphoreGive(sem);
	else err("failed to create semaphore");
}

