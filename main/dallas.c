
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_sntp.h>
#include <driver/gpio.h>

#ifdef CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif

#include "log.h"
#include "settings.h"
#include "dallas.h"

/**
 * The calendar info is bcd.
 */
#define bcd2i(B) (((((B) & 0xf0) >> 4) * 10) + ((B) & 0x0f))
#define i2bcd(I) ((((I) / 10) << 4) + ((I) % 10))

static const char *TAG = "dallas";

static gpio_config_t ce_conf = {
	.mode         = GPIO_MODE_OUTPUT,
	.intr_type    = GPIO_PIN_INTR_DISABLE,
	.pull_down_en = GPIO_PULLDOWN_DISABLE,
	.pull_up_en   = GPIO_PULLUP_DISABLE,
	.pin_bit_mask = 1ULL << CONFIG_DALLAS_GPIO_CE
};

static gpio_config_t sd_conf = {
	.mode         = GPIO_MODE_OUTPUT,
	.intr_type    = GPIO_PIN_INTR_DISABLE,
	.pull_down_en = GPIO_PULLDOWN_DISABLE,
	.pull_up_en   = GPIO_PULLUP_DISABLE,
	.pin_bit_mask = (1ULL << CONFIG_DALLAS_GPIO_SCL) |
	                (1ULL << CONFIG_DALLAS_GPIO_SDA)
};

/**
 * We'll need this pm lock to ensure the esp32 doesn't go to sleep
 * while we're busy bit-banging the dallas
 */
#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t pm_lock;
#endif

/**
 * Setup the GPIO pins for a transfer
 *
 * vTaskDelay() is used here to ensure tCC (CE-to-CLK setup) > 1 us.
 */
static void dallas_xfer_start(void)
{
#if CONFIG_PM_ENABLE
	esp_pm_lock_acquire(pm_lock);
#endif

	gpio_set_direction(CONFIG_DALLAS_GPIO_SDA, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_DALLAS_GPIO_SDA, 0);
	gpio_set_level(CONFIG_DALLAS_GPIO_SCL, 0);
	gpio_set_level(CONFIG_DALLAS_GPIO_CE, 1);
	vTaskDelay(1);
}

/**
 * Reset the GPIO pins post-xfer
 */
static void dallas_xfer_stop(void)
{
	gpio_set_level(CONFIG_DALLAS_GPIO_SCL, 0);
	gpio_set_direction(CONFIG_DALLAS_GPIO_SDA, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_DALLAS_GPIO_SDA, 0);
	gpio_set_level(CONFIG_DALLAS_GPIO_CE, 0);
	vTaskDelay(1);

#if CONFIG_PM_ENABLE
	esp_pm_lock_release(pm_lock);
#endif
}

/**
 * Bits should be present on SDA on the rising edge of SCL.
 *
 * NOTE: On read, the DS1302 samples the last bit of the address on the
 * rising edge of SCL, and outputs the first bit of the value on the
 * falling edge of SCL. So we have to change the pin state in between to
 * avoid a conflict.
 */
static void _dallas_tx(uint8_t b)
{
	int i;

	gpio_set_direction(CONFIG_DALLAS_GPIO_SDA, GPIO_MODE_OUTPUT);
	for (i = 8; i; i--, b >>= 1) {
		gpio_set_level(CONFIG_DALLAS_GPIO_SCL, 0);
		gpio_set_level(CONFIG_DALLAS_GPIO_SDA, b & 1);
		gpio_set_level(CONFIG_DALLAS_GPIO_SCL, 1);
	}

	gpio_set_direction(CONFIG_DALLAS_GPIO_SDA, GPIO_MODE_INPUT);
}

/**
 * Bits will be present on SDA on the falling edge of SCL
 */
static uint8_t _dallas_rx(void)
{
	int i;
	uint8_t b = 0;

	for (i = 0; i < 8; i++) {
		gpio_set_level(CONFIG_DALLAS_GPIO_SCL, 0);
		b |= gpio_get_level(CONFIG_DALLAS_GPIO_SDA) << i;
		gpio_set_level(CONFIG_DALLAS_GPIO_SCL, 1);
	}

	return b;
}

/**
 * Set the write protect bit
 */
static void dallas_set_wp(unsigned int wp)
{
	dallas_xfer_start();
	_dallas_tx(0x8e);
	_dallas_tx(wp ? 0x80 : 0);
	dallas_xfer_stop();
}

/**
 * Read a byte from the dallas
 */
uint8_t dallas_read(uint8_t addr)
{
	uint8_t b = 0;

	dallas_xfer_start();
	_dallas_tx(addr | 1);
	b = _dallas_rx();
	dallas_xfer_stop();
	return b;
}

/**
 * Write a byte to the dallas
 */
void dallas_write(uint8_t addr, uint8_t b)
{
	dallas_set_wp(0);
	dallas_xfer_start();
	_dallas_tx(addr & ~1);
	_dallas_tx(b);
	dallas_xfer_stop();
	dallas_set_wp(1);
}

/**
 * Set the system clock from the dallas
 */
void dallas_set_system_clock(void)
{
	struct timeval tv;
	struct tm tm;

	info("fetching time");
	memset(&tm, 0, sizeof(tm));
	memset(&tv, 0, sizeof(tv));

	dallas_xfer_start();
	tm.tm_sec  = bcd2i(dallas_read(0x80) & 0x7f);
	tm.tm_min  = bcd2i(dallas_read(0x82) & 0x7f);
	tm.tm_hour = bcd2i(dallas_read(0x84) & 0x3f);
	tm.tm_mday = bcd2i(dallas_read(0x86) & 0x3f);
	tm.tm_mon  = bcd2i(dallas_read(0x88) & 0x1f) - 1;
	tm.tm_wday = bcd2i(dallas_read(0x8a) & 7) - 1;
	tm.tm_year = bcd2i(dallas_read(0x8c)) + 100;
	dallas_xfer_stop();

	tv.tv_sec = mktime(&tm);
	settimeofday(&tv, NULL);
	info("got time: %04u-%02u-%02u %02u:%02u:%02u",
	     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	     tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/**
 * Called when we get the time via NTP
 */
void dallas_sync(struct timeval *tv)
{
	struct tm *tm;
	time_t now = time(NULL);
	(void)tv;

	tm = gmtime(&now);
	info("syncing time: %04u-%02u-%02u %02u:%02u:%02u",
	     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
	     tm->tm_hour, tm->tm_min, tm->tm_sec);

	dallas_set_wp(0);
	dallas_xfer_start();
	_dallas_tx(0xbe);
	_dallas_tx(i2bcd(tm->tm_sec));
	_dallas_tx(i2bcd(tm->tm_min));
	_dallas_tx(i2bcd(tm->tm_hour));
	_dallas_tx(i2bcd(tm->tm_mday));
	_dallas_tx(i2bcd(tm->tm_mon + 1));
	_dallas_tx(i2bcd(tm->tm_wday + 1));
	_dallas_tx(i2bcd(tm->tm_year - 100));
	_dallas_tx(0x80); /* Set WP */
	dallas_xfer_stop();
	sntp_set_time_sync_notification_cb(NULL);
}

/**
 * Initialize the dallas RAM by burst-write
 */
static void dallas_init_ram(void)
{
	unsigned int i;

	info("initializng settings ram...");
	dallas_set_wp(0);
	dallas_xfer_start();
	_dallas_tx(0xfe);
	for (i = 0xc0; i <= SETTINGS_V; i += 2) {
		if (i == SETTINGS_V)
			_dallas_tx(SETTINGS_VALID);
		else _dallas_tx(0);
	}
	dallas_xfer_stop();
	dallas_set_wp(1);
}

void dallas_init(void)
{
#if CONFIG_PM_ENABLE
	ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0,
	                "ds1302", &pm_lock));
	esp_pm_lock_acquire(pm_lock);
#endif

	info("setting TZ to UTC");
	setenv("TZ", "UTC", 1);
	tzset();

	info("initializing...");
	gpio_config(&ce_conf);
	gpio_config(&sd_conf);
	gpio_set_drive_capability(CONFIG_DALLAS_GPIO_CE,  GPIO_DRIVE_CAP_1);
	gpio_set_drive_capability(CONFIG_DALLAS_GPIO_SCL, GPIO_DRIVE_CAP_1);
	gpio_set_drive_capability(CONFIG_DALLAS_GPIO_SDA, GPIO_DRIVE_CAP_1);
	vTaskDelay(pdMS_TO_TICKS(1));

	/* Reset the dallas and clear the Clock Halt flag */
	dallas_xfer_stop();
	dallas_set_wp(0);
	dallas_write(0x80, dallas_read(0x80) & ~0x80);
	dallas_set_wp(1);

	/* Ensure we have valid settings */
	settings_lock();
	memset(&settings, 0, sizeof(settings));
	if (dallas_read(SETTINGS_V) == SETTINGS_VALID) {
		info("reading settings");
		settings.override_sw = gpio_get_level(CONFIG_GPIO_SWON);
		if (gpio_get_level(CONFIG_GPIO_SWOFF))
			settings.override_sw |= 2;

		settings.light_sw = dallas_read(SETTINGS_SW);
		settings.sched_sw = dallas_read(SETTINGS_SSW);
		settings.shr      = dallas_read(SETTINGS_SHR);
		settings.smn      = dallas_read(SETTINGS_SMN);
		settings.ehr      = dallas_read(SETTINGS_EHR);
		settings.emn      = dallas_read(SETTINGS_EMN);
	} else dallas_init_ram();
	settings_unlock();

	/* Initialize the system clock */
	info("setting system clock...");
	dallas_set_system_clock();
}

