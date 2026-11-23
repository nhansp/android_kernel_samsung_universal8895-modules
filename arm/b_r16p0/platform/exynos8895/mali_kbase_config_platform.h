/* SPDX-License-Identifier: GPL-2.0 */
/* Exynos 8895 kbase platform configuration; protected mode is not supported. */

/**
 * Power management configuration
 *
 * Attached value: pointer to @ref kbase_pm_callback_conf
 * Default value: See @ref kbase_pm_callback_conf
 */
#define POWER_MANAGEMENT_CALLBACKS (&exynos8895_pm_callbacks)

/**
 * Platform specific configuration functions
 *
 * Attached value: pointer to @ref kbase_platform_funcs_conf
 * Default value: See @ref kbase_platform_funcs_conf
 */
#define PLATFORM_FUNCS (&exynos8895_platform_funcs)

extern struct kbase_pm_callback_conf exynos8895_pm_callbacks;
extern struct kbase_platform_funcs_conf exynos8895_platform_funcs;

/**
 * Autosuspend delay
 *
 * The delay time (in milliseconds) to be used for autosuspend, Samsung's
 * gpu_runtime_pm_delay_time. The samsung,autosuspend-delay-ms DT property
 * overrides it.
 */
#define AUTO_SUSPEND_DELAY (50)
