#ifndef LIGHTCTL_DALLAS_H
#define LIGHTCTL_DALLAS_H

#include <stdint.h>
#include <time.h>

/**
 * Addresses in the dallas RAM for settings
 */
#define SETTINGS_SW    0xc0 /**< Selected on/off state   */
#define SETTINGS_SSW   0xc2 /**< Schedule: on/off state  */
#define SETTINGS_SHR   0xc4 /**< Schedule: Start hour    */
#define SETTINGS_SMN   0xc6 /**< Schedule: Start minute  */
#define SETTINGS_EHR   0xc8 /**< Schedule: Ending hour   */
#define SETTINGS_EMN   0xca /**< Schedule: Ending minute */
#define SETTINGS_V     0xcc /**< Settings validity       */
#define SETTINGS_VALID 0x5a /**< Validity indicator      */

/**
 * Read a byte from the dallas
 */
uint8_t dallas_read(uint8_t addr);

/**
 * Write a byte to the dallas
 */
void dallas_write(uint8_t addr, uint8_t b);

/**
 * Set the system clock from the dallas
 */
void dallas_set_system_clock(void);

/**
 * Called when we get the time via NTP
 */
void dallas_sync(struct timeval *tv);

void dallas_init(void);

#endif /* LIGHTCTL_DALLAS_H */
