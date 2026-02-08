// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * acer_brightness.c
 *
 * Acer Predator/Nitro keyboard backlight brightness-only module:
 * - Uses Acer gaming WMI interface (WMID_GUID4) method 20 with 16-byte payload
 * - Exposes LED class device: /sys/class/leds/acer::kbd_backlight/brightness (0-100)
 *
 * Typing behavior (optimized):
 * - On any keypress: only turns on if currently off (avoids redundant WMI calls)
 * - Auto-off timer is restarted via mod_delayed_work() (less churn / fewer races)
 * - Uses dedicated WQ_UNBOUND workqueue to avoid hogging per-CPU worker threads
 *
 * Notes:
 * - Controls keyboard backlight brightness embedded in gaming payload byte 2 (0-100)
 * - No firmware readback; uses cached/applied state in driver
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/leds.h>
#include <linux/mutex.h>
#include <linux/acpi.h>
#include <linux/wmi.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/errno.h>
#include <linux/keyboard.h>
#include <linux/notifier.h>
#include <linux/atomic.h>

MODULE_AUTHOR("Modified by Lautaro Lucas C. (lau-bin)");
MODULE_DESCRIPTION("Acer keyboard backlight brightness-only with keypress auto-off (optimized workqueue usage)");
MODULE_LICENSE("GPL");

/* Acer gaming WMI GUID */
#define WMID_GUID4 "7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56"

/* Gaming keyboard backlight method (takes a 16-byte payload) */
#define ACER_WMID_SET_GAMINGKBBL_METHODID 20
#define GAMING_KBBL_CONFIG_LEN 16

/* payload[2] = brightness (0-100)
 * payload[9] = 0/1 toggle (your previous userspace used 1 often)
 */
static int payload9_value = 1;
module_param(payload9_value, int, 0644);
MODULE_PARM_DESC(payload9_value, "Value for payload[9] (0=dynamic, 1=static-ish).");

/* Auto-off delay after last keypress, in milliseconds */
static int auto_off_ms = 2000;
module_param(auto_off_ms, int, 0644);
MODULE_PARM_DESC(auto_off_ms, "Milliseconds after last keypress to turn keyboard brightness to 0");

/* Optional: apply brightness immediately when module loads */
static bool apply_on_load = false;
module_param(apply_on_load, bool, 0644);
MODULE_PARM_DESC(apply_on_load, "Apply current cached brightness on module load");

/* Default cached brightness at load (used for sysfs default) */
static int initial_brightness = 100;
module_param(initial_brightness, int, 0644);
MODULE_PARM_DESC(initial_brightness, "Initial cached brightness (0-100) exposed via sysfs on load");

/*
 * Optional debounce for "turn on" when off (ms).
 * 0 disables. Useful if your firmware is extremely slow and keypress storms happen.
 */
static int on_debounce_ms = 0;
module_param(on_debounce_ms, int, 0644);
MODULE_PARM_DESC(on_debounce_ms, "Minimum ms between off->on applies (0 disables)");

static DEFINE_MUTEX(kbb_mutex);
static u8 cached_brightness;

/* Track last applied state to avoid redundant firmware writes */
static atomic_t is_lit = ATOMIC_INIT(0);          /* 0=off, 1=on */
static atomic_t applied_brightness = ATOMIC_INIT(-1); /* -1=unknown, else 0..100 */

/* Debounce bookkeeping */
static unsigned long last_on_apply_jiffies;

static struct delayed_work turn_on_work;
static struct delayed_work turn_off_work;

static struct notifier_block kbd_nb;

/* Dedicated workqueue (unbound) to avoid per-CPU worker contention */
static struct workqueue_struct *acer_wq;

/* ---- WMI write helpers ---- */

static int acer_wmid_gaming_set_payload(const u8 payload[GAMING_KBBL_CONFIG_LEN])
{
	struct acpi_buffer input = { (acpi_size)GAMING_KBBL_CONFIG_LEN, (void *)payload };
	struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status;

	status = wmi_evaluate_method(WMID_GUID4, 0, ACER_WMID_SET_GAMINGKBBL_METHODID,
				     &input, &result);
	if (ACPI_FAILURE(status)) {
		kfree(result.pointer);
		return -EIO;
	}

	kfree(result.pointer);
	return 0;
}

static int acer_kbb_brightness_apply(u8 brightness)
{
	u8 payload[GAMING_KBBL_CONFIG_LEN] = { 0 };

	payload[2] = brightness;                    /* 0-100 */
	payload[9] = (u8)(payload9_value ? 1 : 0);  /* 0/1 */

	return acer_wmid_gaming_set_payload(payload);
}

/* ---- Work functions ---- */

static void acer_turn_on_workfn(struct work_struct *work)
{
	u8 b;
	int ret;
	unsigned long now = jiffies;

	/* If already on, skip */
	if (atomic_read(&is_lit))
		return;

	/* Optional debounce */
	if (on_debounce_ms > 0 &&
	    time_before(now, last_on_apply_jiffies + msecs_to_jiffies(on_debounce_ms)))
		return;

	mutex_lock(&kbb_mutex);
	b = cached_brightness;

	/*
	 * If cached brightness is 0, turning "on" does nothing useful.
	 * Keep is_lit = 0, and skip firmware call.
	 */
	if (b == 0) {
		mutex_unlock(&kbb_mutex);
		return;
	}

	/* If firmware already has this brightness (as far as we know), skip */
	if (atomic_read(&applied_brightness) == b) {
		atomic_set(&is_lit, 1);
		mutex_unlock(&kbb_mutex);
		return;
	}

	ret = acer_kbb_brightness_apply(b);
	if (!ret) {
		atomic_set(&is_lit, 1);
		atomic_set(&applied_brightness, b);
		last_on_apply_jiffies = now;
	}
	mutex_unlock(&kbb_mutex);

	if (ret)
		pr_debug("turn_on apply failed: %d\n", ret);
}

static void acer_turn_off_workfn(struct work_struct *work)
{
	int ret;

	/* If already off, skip */
	if (!atomic_read(&is_lit) && atomic_read(&applied_brightness) == 0)
		return;

	mutex_lock(&kbb_mutex);

	/* If we already believe firmware is at 0, skip */
	if (atomic_read(&applied_brightness) == 0) {
		atomic_set(&is_lit, 0);
		mutex_unlock(&kbb_mutex);
		return;
	}

	ret = acer_kbb_brightness_apply(0);
	if (!ret) {
		atomic_set(&is_lit, 0);
		atomic_set(&applied_brightness, 0);
	}
	mutex_unlock(&kbb_mutex);

	if (ret)
		pr_debug("turn_off apply failed: %d\n", ret);
}

/* ---- Keyboard notifier: reacts to real keypresses ---- */

static int acer_kbb_keyboard_notify(struct notifier_block *nb,
				    unsigned long action, void *data)
{
	struct keyboard_notifier_param *param = data;

	if (action != KBD_KEYCODE)
		return NOTIFY_OK;

	/* Only key press, not release */
	if (!param->down)
		return NOTIFY_OK;

	/*
	 * Turn on only if currently off.
	 * This removes the expensive "WMI write on every keypress" behavior.
	 */
	if (!atomic_read(&is_lit))
		queue_delayed_work(acer_wq, &turn_on_work, 0);

	/*
	 * Restart auto-off timer with mod_delayed_work().
	 * This is preferable to cancel_delayed_work()+schedule_delayed_work().
	 */
	if (auto_off_ms > 0)
		mod_delayed_work(acer_wq, &turn_off_work, msecs_to_jiffies(auto_off_ms));

	return NOTIFY_OK;
}

/* ---- LED class device ---- */

static int acer_kbb_led_set(struct led_classdev *cdev, enum led_brightness value)
{
	u8 b;
	int ret;

	/* led_brightness is 0..255; our sysfs max is 100 */
	b = (value > 100) ? 100 : (u8)value;

	/*
	 * If we're currently "on" and already applied this brightness, skip.
	 * If b==0 and already off, skip.
	 */
	if (atomic_read(&applied_brightness) == b) {
		/* Still update cached_brightness so keypress uses latest intent */
		mutex_lock(&kbb_mutex);
		cached_brightness = b;
		mutex_unlock(&kbb_mutex);

		if (b == 0)
			atomic_set(&is_lit, 0);
		else
			atomic_set(&is_lit, 1);

		return 0;
	}

	mutex_lock(&kbb_mutex);
	ret = acer_kbb_brightness_apply(b);
	if (!ret) {
		cached_brightness = b;              /* keypress uses this */
		atomic_set(&applied_brightness, b); /* what we believe firmware has */
		atomic_set(&is_lit, b ? 1 : 0);
	}
	mutex_unlock(&kbb_mutex);

	return ret;
}

static enum led_brightness acer_kbb_led_get(struct led_classdev *cdev)
{
	/* No firmware readback; report last cached value */
	return cached_brightness;
}

static struct led_classdev acer_kbb_led = {
	.name = "acer::kbd_backlight",
	.brightness_set_blocking = acer_kbb_led_set,
	.brightness_get = acer_kbb_led_get,
	.max_brightness = 100,
};

/* ---- Init/Exit ---- */

static int __init acer_kbb_init(void)
{
	int ret;

	if (!wmi_has_guid(WMID_GUID4)) {
		pr_err("WMID_GUID4 not present; Acer gaming WMI interface unavailable\n");
		return -ENODEV;
	}

	/* Sanitize params */
	if (payload9_value != 0 && payload9_value != 1)
		payload9_value = 1;

	if (auto_off_ms < 0)
		auto_off_ms = 0;

	if (initial_brightness < 0)
		initial_brightness = 0;
	if (initial_brightness > 100)
		initial_brightness = 100;

	if (on_debounce_ms < 0)
		on_debounce_ms = 0;

	cached_brightness = (u8)initial_brightness;
	atomic_set(&is_lit, 0);
	atomic_set(&applied_brightness, -1);
	last_on_apply_jiffies = 0;

	/*
	 * Create dedicated unbound workqueue.
	 * This follows the kernel warning suggestion and avoids hogging per-CPU workers.
	 */
	acer_wq = alloc_workqueue("acer_brightness", WQ_UNBOUND | WQ_FREEZABLE, 1);
	if (!acer_wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&turn_on_work, acer_turn_on_workfn);
	INIT_DELAYED_WORK(&turn_off_work, acer_turn_off_workfn);

	ret = led_classdev_register(NULL, &acer_kbb_led);
	if (ret) {
		pr_err("Failed to register LED class device: %d\n", ret);
		destroy_workqueue(acer_wq);
		acer_wq = NULL;
		return ret;
	}

	/* Register keyboard notifier for real keypress events */
	kbd_nb.notifier_call = acer_kbb_keyboard_notify;
	ret = register_keyboard_notifier(&kbd_nb);
	if (ret) {
		pr_warn("register_keyboard_notifier failed: %d (keypress auto-off disabled)\n", ret);
		/* Keep driver usable via sysfs even without notifier */
	}

	if (apply_on_load) {
		ret = acer_kbb_brightness_apply(cached_brightness);
		if (ret) {
			pr_warn("Initial brightness apply failed: %d\n", ret);
		} else {
			atomic_set(&applied_brightness, cached_brightness);
			atomic_set(&is_lit, cached_brightness ? 1 : 0);
		}
	} else {
		/*
		 * We don't know actual firmware state. Assume "off" to prevent needless writes.
		 * Keypress will turn on once.
		 */
		atomic_set(&applied_brightness, 0);
		atomic_set(&is_lit, 0);
	}

	pr_info("Loaded. Set brightness via /sys/class/leds/%s/brightness (0-100).\n",
		acer_kbb_led.name);
	pr_info("Keypress turns on if off; auto-off after %dms. Workqueue=WQ_UNBOUND.\n",
		auto_off_ms);
	pr_info("Params: initial_brightness=%d apply_on_load=%d payload9_value=%d auto_off_ms=%d on_debounce_ms=%d\n",
		initial_brightness, apply_on_load, payload9_value, auto_off_ms, on_debounce_ms);

	return 0;
}

static void __exit acer_kbb_exit(void)
{
	/* Stop any pending work */
	if (acer_wq) {
		cancel_delayed_work_sync(&turn_on_work);
		cancel_delayed_work_sync(&turn_off_work);
	}

	unregister_keyboard_notifier(&kbd_nb);

	led_classdev_unregister(&acer_kbb_led);

	if (acer_wq) {
		destroy_workqueue(acer_wq);
		acer_wq = NULL;
	}

	pr_info("Unloaded\n");
}

module_init(acer_kbb_init);
module_exit(acer_kbb_exit);

