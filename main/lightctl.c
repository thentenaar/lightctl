
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_err.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <esp_sntp.h>
#include <driver/gpio.h>

#if CONFIG_PM_ENABLE
#include <esp_pm.h>
#include <esp_sleep.h>
#endif

/* components/mdns */
#include <mdns.h>

#include "log.h"
#include "event.h"
#include "dallas.h"
#include "settings.h"
#include "wifi.h"
#include "http.h"

/**
 * Microsecond conversion macros
 */
#define SECONDS 1000000
#define MINUTES (uint64_t)(60 * SECONDS)
#define HOURS   (uint64_t)(60 * MINUTES)

ESP_EVENT_DEFINE_BASE(LIGHTCTL_EVENT);

esp_event_loop_handle_t lightctl_ev;
static esp_timer_handle_t timer;
static const char *TAG = "lightctl";

static gpio_config_t ls_conf = {
	.mode         = GPIO_MODE_OUTPUT,
	.intr_type    = GPIO_PIN_INTR_DISABLE,
	.pull_down_en = GPIO_PULLDOWN_DISABLE,
	.pull_up_en   = GPIO_PULLUP_DISABLE,
	.pin_bit_mask = (1ULL << CONFIG_GPIO_LIGHTS) |
	                (1ULL << CONFIG_GPIO_STATUS_LED),
};

static gpio_config_t sw_conf = {
	.mode         = GPIO_MODE_INPUT,
	.intr_type    = GPIO_PIN_INTR_ANYEDGE,
	.pull_down_en = GPIO_PULLDOWN_ENABLE,
	.pull_up_en   = GPIO_PULLUP_DISABLE,
	.pin_bit_mask = (1ULL << CONFIG_GPIO_SWON) |
	                (1ULL << CONFIG_GPIO_SWOFF)
};

static esp_event_loop_args_t ev_args = {
	.queue_size      = CONFIG_ESP_SYSTEM_EVENT_QUEUE_SIZE,
	.task_name       = "lightctl_ev",
	.task_priority   = 0,
	.task_stack_size = CONFIG_LIGHTCTL_EVLOOP_STACK_SIZE,
	.task_core_id    = tskNO_AFFINITY
};

#if CONFIG_PM_ENABLE
static esp_pm_config_esp32_t pm_config = {
	.min_freq_mhz = CONFIG_ESP32_XTAL_FREQ,
	.max_freq_mhz = CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
	.light_sleep_enable = 1,
#endif
};
#endif /* PM_ENABLE */

static void IRAM_ATTR switch_isr(void *arg)
{
	(void)arg;
	esp_event_isr_post_to(lightctl_ev, LIGHTCTL_EVENT, SWITCH,
	                      NULL, 0, NULL);
}

static void lights_on(void)
{
	if (gpio_get_level(CONFIG_GPIO_SWOFF))
		return;

	gpio_set_level(CONFIG_GPIO_LIGHTS, 1);
	settings_lock();
	settings.lights_status = 1;
	settings_unlock();
}

static void lights_off(void)
{
	if (gpio_get_level(CONFIG_GPIO_SWON))
		return;

	gpio_set_level(CONFIG_GPIO_LIGHTS, 0);
	settings_lock();
	settings.lights_status = 0;
	settings_unlock();
}

static void schedule(void *arg)
{
	unsigned int shr, smn, ehr, emn;
	time_t now    = time(NULL);
	struct tm *tm = gmtime(&now);
	(void)arg;

	settings_lock();
	shr = settings.shr;
	smn = settings.smn;
	ehr = settings.ehr;
	emn = settings.emn;
	settings_unlock();

	/**
	 * If the ending time is in the same hour and preceeds the
	 * starting minute, handle the ending first.
	 */
	if (tm->tm_hour == ehr && ehr == shr && emn < smn) {
		if (tm->tm_min < emn) {
			esp_timer_start_once(
				timer,
				(emn - tm->tm_min) * MINUTES -
				tm->tm_sec * SECONDS
			);
			return;
		}

		if (tm->tm_min < smn)
			lights_off();
	}

	/**
	 * We're in the starting hour, but haven't gotten to the
	 * minute yet.
	 */
	if (tm->tm_hour == shr && tm->tm_min < smn) {
		esp_timer_start_once(
			timer,
			(smn - tm->tm_min) * MINUTES -
			tm->tm_sec * SECONDS
		);

		return;
	} else if (tm->tm_hour == shr && tm->tm_min >= smn)
		lights_on();

	/**
	 * We're in the ending hour, but haven't gotten to the
	 * minute yet.
	 */
	if (tm->tm_hour == ehr && tm->tm_min < emn) {
		esp_timer_start_once(
			timer,
			(emn - tm->tm_min) * MINUTES -
			tm->tm_sec * SECONDS
		);

		return;
	} else if (tm->tm_hour == ehr && tm->tm_min >= emn)
		lights_off();

	/* Check again at the top of the next hour */
	esp_timer_start_once(timer, HOURS -
		            tm->tm_min * MINUTES -
		            tm->tm_sec * SECONDS);

}

static void app_event(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
	(void)arg;
	(void)event_base;
	(void)event_data;

	switch (event_id) {
	case SWITCH:
		settings_lock();
		settings.override_sw = gpio_get_level(CONFIG_GPIO_SWON);
		if (gpio_get_level(CONFIG_GPIO_SWOFF))
			settings.override_sw |= 2;

		if (!settings.override_sw) {
			esp_event_post_to(lightctl_ev, LIGHTCTL_EVENT,
			                  settings.light_sw ? ON : OFF,
			                  NULL, 0, 0);
		}
		settings_unlock();

		if (gpio_get_level(CONFIG_GPIO_SWON))       lights_on();
		else if (gpio_get_level(CONFIG_GPIO_SWOFF)) lights_off();
		break;
	case ON:
		settings_lock();
		settings.light_sw = 1;
		dallas_write(SETTINGS_SW, 1);
		settings_unlock();
		lights_on();
		break;
	case OFF:
		settings_lock();
		settings.light_sw = 0;
		dallas_write(SETTINGS_SW, 0);
		settings_unlock();
		lights_off();
		break;
	case SCHED_ON:
		esp_timer_stop(timer);
		settings_lock();
		settings.sched_sw = 1;
		dallas_write(SETTINGS_SSW, 1);
		dallas_write(SETTINGS_SHR, settings.shr);
		dallas_write(SETTINGS_SMN, settings.smn);
		dallas_write(SETTINGS_EHR, settings.ehr);
		dallas_write(SETTINGS_EMN, settings.emn);
		settings_unlock();
		schedule(NULL);
		break;
	case SCHED_OFF:
		esp_timer_stop(timer);
		settings_lock();
		settings.sched_sw = 0;
		if (!settings.light_sw && settings.lights_status) {
			esp_event_post_to(lightctl_ev, LIGHTCTL_EVENT, OFF,
			                  NULL, 0, 0);
		}
		settings_unlock();
		dallas_write(SETTINGS_SSW, 0);
		break;
	case CONNECTED:
		sntp_set_time_sync_notification_cb(dallas_sync);
		if (!sntp_restart()) sntp_init();
		http_start();
		break;
	case LOSTCONN:
		info("connection lost");
		http_stop();
		sntp_stop();
		break;
	}
}

static esp_timer_create_args_t timer_args = {
	.name     = "schedule_timer",
	.callback = schedule,
	.dispatch_method = ESP_TIMER_TASK
};

void app_main(void)
{
#if CONFIG_PM_ENABLE
	ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
#endif

	/* Create our timer */
	esp_timer_init();
	esp_timer_create(&timer_args, &timer);

	/* Create our event loop */
	ev_args.task_priority = uxTaskPriorityGet(NULL);
	esp_event_loop_create_default();
	esp_event_loop_create(&ev_args, &lightctl_ev);
	esp_event_handler_register_with(lightctl_ev, LIGHTCTL_EVENT,
	                                ESP_EVENT_ANY_ID, app_event, NULL);

	info("Initializing gpio...");
	gpio_config(&sw_conf);
	gpio_config(&ls_conf);

	/* Drive 'lights' @ 20 mA, 'status' @ 10 mA */
	gpio_set_drive_capability(CONFIG_GPIO_LIGHTS,     GPIO_DRIVE_CAP_2);
	gpio_set_drive_capability(CONFIG_GPIO_STATUS_LED, GPIO_DRIVE_CAP_1);

	/* Initialize the pin levels */
	gpio_set_level(CONFIG_GPIO_LIGHTS, 0);
	gpio_set_level(CONFIG_GPIO_STATUS_LED, 0);

	/* Configure the ISR service */
	gpio_install_isr_service(0);
	gpio_isr_handler_add(CONFIG_GPIO_SWON, switch_isr, NULL);
	gpio_isr_handler_add(CONFIG_GPIO_SWOFF, switch_isr, NULL);

#if CONFIG_PM_ENABLE
	gpio_wakeup_enable(CONFIG_GPIO_SWON,  GPIO_INTR_HIGH_LEVEL);
	gpio_wakeup_enable(CONFIG_GPIO_SWOFF, GPIO_INTR_HIGH_LEVEL);
	esp_sleep_enable_gpio_wakeup();
#endif

	/* Get the time and settings from the dallas */
	settings_init();
	dallas_init();

	/* Sample the switch state and set the schedule configuration */
	esp_event_post_to(lightctl_ev, LIGHTCTL_EVENT, SWITCH, NULL, 0, 0);
	settings_lock();
	esp_event_post_to(lightctl_ev, LIGHTCTL_EVENT,
	                  settings.sched_sw ? SCHED_ON : SCHED_OFF,
	                  NULL, 0, 0);
	settings_unlock();

	/* Initial SNTP options */
	sntp_setoperatingmode(SNTP_OPMODE_POLL);
	sntp_setservername(0, "pool.ntp.org");

#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
	sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
#endif

	/* mdns itself hooks the wifi / ip events */
	mdns_init();
	mdns_hostname_set(CONFIG_LWIP_LOCAL_HOSTNAME);
	mdns_service_add("lightctl", "_http", "_tcp", 80, NULL, 0);
	wifi_init();
}

