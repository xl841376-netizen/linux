/* SPDX-License-Identifier: GPL-2.0 */
/*
 * crux_dep - tiny provider/consumer state contract for crux (sm8150)
 *
 * Problem it solves: a consumer driver that depends on another block being in
 * a particular state (powered, firmware up, link in AMSS, ...) has no way to
 * ask that block today, so it silently takes its default path.  When the two
 * sides disagree, both act on the same resources and the board locks up.
 *
 * Providers call crux_dep_set() when their state changes.  Consumers call
 * crux_dep_require() before using the resource; a mismatch returns false and
 * logs a ratelimited warning naming both sides, so the default-path bug shows
 * up in dmesg instead of turning into a hang.
 */
#ifndef _LINUX_CRUX_DEP_H
#define _LINUX_CRUX_DEP_H

#include <linux/types.h>

enum crux_dep_state {
	CRUX_DEP_UNKNOWN = 0,
	CRUX_DEP_DOWN,
	CRUX_DEP_UP,
	CRUX_DEP_ERROR,
	CRUX_DEP_STATE_MAX,
};

void crux_dep_set(const char *name, enum crux_dep_state state);
enum crux_dep_state crux_dep_get(const char *name);
const char *crux_dep_state_name(enum crux_dep_state state);

/* Wait until @name reaches @state; 0 on success, -ETIMEDOUT/-ENOENT otherwise */
int crux_dep_wait(const char *name, enum crux_dep_state state,
		  unsigned long timeout_ms);

/*
 * true  -> provider is in the required state
 * false -> it is not; a ratelimited warning is emitted unless @quiet
 */
bool crux_dep_require(const char *name, enum crux_dep_state state,
		      const char *who, bool quiet);

#endif /* _LINUX_CRUX_DEP_H */
