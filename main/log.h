#ifndef LIGHTCTL_LOG_H
#define LIGHTCTL_LOG_H

#include <esp_log.h>

#define err(fmt, ...)   ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#define warn(fmt, ...)  ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#define info(fmt, ...)  ESP_LOGI(TAG, fmt, ## __VA_ARGS__)
#define debug(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)

#endif /* LIGHTCTL_LOG_H */
