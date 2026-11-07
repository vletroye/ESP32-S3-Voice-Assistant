/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Ringing a timer, an alarm or a reminder that is due (alarms.c), in its own task:
 * - a timer or an alarm: a chime made here (or the alarm's radio), from silence up to ALARM_RING_VOLUME % of the
 *   speaker over ALARM_RAMP_S seconds whatever the volume set, its name said once the full level is reached, until
 *   the screen is touched or the wake word is said, or Alarm_stop_min minutes (CONFIG.INI);
 * - a reminder: a short chime at the volume set, then its text said (and listened to after, as a reply);
 * - a missed one (the device was off): said, without a chime.
 * The radio playing is suspended meanwhile and plays again after (unless the alarm played one).
 */

#pragma once

#include <stdbool.h>
#include "alarms.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The speaker's volume reached by a timer or an alarm (%) */
#define ALARM_RING_VOLUME   80
/* From silence to that level in this time */
#define ALARM_RAMP_S        30
/* A ring stopped can be snoozed ("encore 5 minutes") this long after */
#define ALARM_SNOOZE_WINDOW_S   600

/** Start its task (after the audio and the screen) */
void alarm_ring_init(void);

/** Ring this one (copied); false when one is ringing already */
bool alarm_ring_start(const alarm_t *alarm, bool missed);

/** A ring (or its spoken text) is going on */
bool alarm_ring_active(void);

/** Stop it (a touch, the wake word): returns once the speaker is free; false if nothing was ringing */
bool alarm_ring_stop(void);

/** The last one rung, again in `minutes` (within ALARM_SNOOZE_WINDOW_S of its ring); false if none */
bool alarm_ring_snooze(int minutes);

#ifdef __cplusplus
}
#endif
