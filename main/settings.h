#ifndef LIGHTCTL_SETTINGS_H
#define LIGHTCTL_SETTINGS_H

#include <stdint.h>

extern struct lightctl_settings {
	uint8_t lights_status; /**< Current lights status      */
	uint8_t light_sw;      /**< Selected on/off state      */
	uint8_t override_sw;   /**< Override on/auto/off state */
	uint8_t sched_sw;      /**< Schedule on/off state      */
	uint8_t shr;           /**< Schedule: Starting hour    */
	uint8_t smn;           /**< Schedule: Starting minute  */
	uint8_t ehr;           /**< Schedule: Ending hour      */
	uint8_t emn;           /**< Schedule: Ending minute    */
} settings;

void settings_lock(void);
void settings_unlock(void);
void settings_init(void);

#endif /* LIGHTCTL_SETTINGS_H */
