/* ============================================================
 * NovaOS - feature subsystem: /etc/features.conf
 *
 * A tiny module-kill-switch subsystem. The config lives at
 * /etc/features.conf in NovaFS ("name=enable|disable" per line),
 * parsed once at boot and re-read/written by the shell 'feature'
 * command and the featureui.nxp tool (both via sysop calls).
 *
 * The security rule (requirement 5): every module entry point must
 * call feature_is_enabled() FIRST - the guard macros in feature.h
 * make that a one-liner, and the module hooks registered here return
 * a disabled stub when their feature is off.
 * ============================================================ */
#ifndef FEATURE_H
#define FEATURE_H

#include "stdint.h"

/* maximum tracked features and name length (NovaFS name limit) */
#define FEAT_MAX 16
#define FEAT_NAME_MAX 24

/* built-in module hooks (registered at init; stubs when disabled) */
int feat_holiday_days_left(void);      /* holiday_module: days to Oct 1, 0 = holiday */
void feat_holiday_banner(void);        /* holiday_module: draw the CJK egg (gated) */
int feat_cn_toggle(void);              /* cn_lang_support demo hook     */
const char *feat_cn_greeting(void);    /* cn_lang_support demo hook     */

/* init: load /etc/features.conf (auto-creates /etc + defaults) */
int feature_init(void);

/* THE gate (requirement 1/5): 1 = enabled, 0 = disabled/unknown.
 * Safe to call before feature_init - unknown names read as disabled. */
int feature_is_enabled(const char *name);

/* management (shell + TUI): list into buf "name <tab> state\n" lines,
 * returns count; set state, persists the file, -1 = no such feature */
int feature_list(char *buf, uint32_t max);
int feature_set(const char *name, int enabled);

#endif /* FEATURE_H */
