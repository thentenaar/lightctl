
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_err.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <driver/gpio.h>

#include "log.h"
#include "event.h"
#include "wifi.h"

#define RETRY_DELAY pdMS_TO_TICKS(CONFIG_WIFI_RETRY_MS)

static const char *TAG = "wifi";

static unsigned int retries = CONFIG_WIFI_MAX_RETRIES;
static TaskHandle_t blinker = NULL;

static wifi_config_t wifi_config = {
	.sta = {
		.ssid     = CONFIG_WIFI_SSID,
		.password = CONFIG_WIFI_PSK
	}
};

static wifi_country_t wifi_country = {
	.cc           = CONFIG_WIFI_COUNTRY,
	.schan        = CONFIG_WIFI_SCHAN,
	.nchan        = CONFIG_WIFI_NCHAN,
	.max_tx_power = CONFIG_ESP32_PHY_MAX_WIFI_TX_POWER,
	.policy       = WIFI_COUNTRY_POLICY_AUTO
};

/**
 * Blink the LED with the configured period, 50% duty cycle while
 * we're connecting to the WiFi.
 */
static void led_blinker(void *params)
{
	int state = 0;

	do {
		gpio_set_level(CONFIG_GPIO_STATUS_LED, (state ^= 1) & 1);
		vTaskDelay(pdMS_TO_TICKS(CONFIG_WIFI_BLINK_MS >> 1));
	} while (1);
	(void)params;
}

static void got_ip(void *arg, esp_event_base_t event_base,
                   int32_t event_id, void *event_data)
{
	esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID,
	                                      got_ip);

	if (blinker) {
		vTaskDelete(blinker);
		blinker = NULL;
	}

	gpio_set_level(CONFIG_GPIO_STATUS_LED, 1);
	esp_event_post_to(lightctl_ev, LIGHTCTL_EVENT, CONNECTED,
	                  NULL, 0, 10);
}

static void wifi_event(void *arg, esp_event_base_t event_base,
                       int32_t event_id, void *event_data)
{
	switch (event_id) {
	case WIFI_EVENT_STA_START:
		retries = CONFIG_WIFI_MAX_RETRIES;
		if (!blinker) {
			xTaskCreate(led_blinker, "wifi_led",
				    1024, NULL, 1, &blinker);
		}

		goto connect;
	case WIFI_EVENT_STA_STOP:
		if (blinker) {
			vTaskDelete(blinker);
			blinker = NULL;
		}

		gpio_set_level(CONFIG_GPIO_STATUS_LED, 0);
		esp_event_post_to(lightctl_ev, LIGHTCTL_EVENT, LOSTCONN,
		                  NULL, 0, 10);
		break;
	case WIFI_EVENT_STA_CONNECTED:
		info("station connected");
		retries = CONFIG_WIFI_MAX_RETRIES;
		break;
	case WIFI_EVENT_STA_DISCONNECTED:
		info("station disconnected");
		if (retries) {
			info("retrying...");
			--retries;
			goto connect;
		}

		info("max retries exceeded");
		if (blinker) {
			vTaskDelete(blinker);
			blinker = NULL;
		}

		gpio_set_level(CONFIG_GPIO_STATUS_LED, 0);
		esp_event_post_to(lightctl_ev, LIGHTCTL_EVENT, LOSTCONN,
		                  NULL, 0, 10);
		vTaskDelay(RETRY_DELAY);
		esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_START,
			       NULL, 0, 10);
		break;
	}

	return;

connect:
	info("connecting...");
	esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
	                                    got_ip, NULL, NULL);
	esp_event_handler_instance_register(IP_EVENT, IP_EVENT_GOT_IP6,
	                                    got_ip, NULL, NULL);
	esp_wifi_connect();
}

void wifi_init(void)
{
#if CONFIG_ESP32_WIFI_NVS_ENABLED
	esp_err_t ret;
	info("Initializing wifi-nvs...");

	ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
	    ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}

	ESP_ERROR_CHECK(ret);
#endif

	/* Initialize the network stack */
	info("Initializing netif...");
	ESP_ERROR_CHECK(esp_netif_init());
	assert(esp_netif_create_default_wifi_sta());

	/* Init and start WiFi in STA mode */
	info("initializing...");
	esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
	                                    wifi_event, NULL, NULL);

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

#if !CONFIG_ESP32_WIFI_NVS_ENABLED
	esp_wifi_set_storage(WIFI_STORAGE_RAM);
#endif

	esp_wifi_set_mode(WIFI_MODE_STA);
	esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
	esp_wifi_set_country(&wifi_country);

#if CONFIG_PM_ENABLE
	/* Wake around the DTIM interval */
	esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
#endif

	esp_wifi_start();
}

