/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Timers, alarms and reminders: what is due, and when (local time), kept in /state/alarms.json (LittleFS) so that a
 * restart keeps them. Set by the model's tools (timer_set, alarm_set, reminder_set, alarms_list, alarm_delete) and
 * by a few phrases of the device; checked every second by the main task, which has them rung (alarm_ring.c) once the
 * assistant sleeps.
 *
 * Repetitions: none, every day, weekdays, the weekend, chosen days of the week, every month (the same day, the same
 * weekday of the month such as the 2nd Tuesday, the last such weekday, the last day) and every year. An alarm due
 * while the device was off (or before the clock was set) is not rung late: a missed one-time alarm or reminder of
 * the last 12 hours is said once the clock is set, the repeated ones go on with their next time.
 */

#pragma once

#include <stdbool.h>
#include <time.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_LABEL_SIZE    96
#define ALARM_RADIO_SIZE    64

typedef enum {
    ALARM_KIND_TIMER,       /* a duration: rings, then says its name ("les pâtes") */
    ALARM_KIND_ALARM,       /* a time of day: rings (or plays a radio), then says its name if it has one */
    ALARM_KIND_REMINDER,    /* a time: a short sound, then its text said */
} alarm_kind_t;

typedef enum {
    ALARM_REPEAT_NONE,
    ALARM_REPEAT_DAILY,
    ALARM_REPEAT_WEEKDAYS,      /* Monday to Friday */
    ALARM_REPEAT_WEEKEND,
    ALARM_REPEAT_WEEKLY,        /* the days of `days` */
    ALARM_REPEAT_MONTHLY,       /* the same day of the month (the last day when the month is shorter) */
    ALARM_REPEAT_MONTHLY_NTH,   /* the same weekday of the month, the nth one (the 2nd Tuesday) */
    ALARM_REPEAT_MONTHLY_LAST,  /* the last such weekday of the month (the last Friday) */
    ALARM_REPEAT_MONTHLY_END,   /* the last day of the month */
    ALARM_REPEAT_YEARLY,        /* the same day every year (29 February: the 28th in other years) */
} alarm_repeat_t;

typedef struct {
    int id;                         /* given by the device, 1 and up */
    alarm_kind_t kind;
    char label[ALARM_LABEL_SIZE];   /* the timer's or alarm's name, the reminder's text; "" for none */
    time_t due;                     /* the next time it rings */
    alarm_repeat_t repeat;
    unsigned days;                  /* ALARM_REPEAT_WEEKLY: bit 0 Sunday .. bit 6 Saturday (struct tm's tm_wday) */
    int hour, minute;               /* the time of day of a repeated one */
    int month_day;                  /* ALARM_REPEAT_MONTHLY / _YEARLY: the day it was set for (1..31) */
    int month;                      /* ALARM_REPEAT_YEARLY: 0..11 */
    int week_day, nth;              /* ALARM_REPEAT_MONTHLY_NTH / _LAST: 0..6, 1..5 */
    char radio[ALARM_RADIO_SIZE];   /* an alarm playing a radio: the station's name, "" for the ring */
    int seconds;                    /* a timer: its duration (said when it rings) */
} alarm_t;

/** Read the alarms kept (after state_mount) */
void alarms_init(void);

/** The local clock is set (SNTP): alarms can be checked */
bool alarms_clock_set(void);

/**
 * @brief Add one (its id given here) and save; false when the list is full or the time is past.
 *        A repeated one: due at its first time from `due` on (`due` gives the time of day and the day it refers to).
 */
bool alarms_add(alarm_t *alarm);

/** Remove one by its id, and save; false if there is none */
bool alarms_remove(int id);

/** Remove all of a kind (-1: all), and save; the number removed */
int alarms_remove_all(int kind);

/** Copy of the list, by time (allocated, count in *count; NULL when empty) */
alarm_t *alarms_list(int *count);

/** The next one due of a kind (-1: any), false if none */
bool alarms_next(int kind, alarm_t *out);

/**
 * @brief Called every second by the main task: the first one due (now or before), taken off or moved to its next
 *        time, saved; false when none is due. A missed one (more than ALARM_LATE_S late: the device was off) is
 *        reported through *missed instead of ringing.
 */
bool alarms_due(alarm_t *out, bool *missed);

/** Snooze: the same rung again in `minutes` (a one-time copy) */
bool alarms_snooze(const alarm_t *alarm, int minutes);

/** The model's view of one: {"id","kind","label","when","repeat"...} (allocated JSON object) */
cJSON *alarms_to_json(const alarm_t *alarm);

/** The repetition from its name ("daily", "weekdays", "weekly"...), ALARM_REPEAT_NONE when unknown */
alarm_repeat_t alarms_repeat_from(const char *name);

/** A day's English name ("monday", "mon") to its bit of alarm_t.days, 0 when unknown */
int alarms_day_bit(const char *name);

#ifdef __cplusplus
}
#endif
