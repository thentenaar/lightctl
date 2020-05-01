#ifndef LIGHTCTL_EVENT_H
#define LIGHTCTL_EVENT_H

#include <esp_event.h>

/**
 * Application-specific event IDs
 */
enum {
	CONNECTED, /**< Connected to WiFi */
	LOSTCONN,  /**< Lost the WiFi conn. */
	SWITCH,    /**< Switch state changed */
	ON,        /**< "On" state requested */
	OFF,       /**< "Off" state requested */
	SCHED_ON,  /**< Enable schedule */
	SCHED_OFF, /**< Disable schedule */
};

ESP_EVENT_DECLARE_BASE(LIGHTCTL_EVENT);
extern esp_event_loop_handle_t lightctl_ev;

#endif /* LIGHTCTL_EVENT_H */
