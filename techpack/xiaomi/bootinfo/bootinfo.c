// SPDX-License-Identifier: GPL-2.0-only
/*
 * Spring bootinfo reconstruction from the retained module's behavior.
 * This is not the original Xiaomi source.
 */
#include <linux/bitops.h>
#include <linux/ctype.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#define REASON_LEN 64

static char powerup_reason[REASON_LEN];
static char powerdown_reason[REASON_LEN];
static char current_pu_reason_str[REASON_LEN];
static unsigned int pu_reason;
static unsigned int pd_reason;
static struct kobject *bootinfo_kobj;
static DEFINE_MUTEX(reason_lock);

module_param_string(pureason, powerup_reason, REASON_LEN, 0644);
module_param_string(pdreason, powerdown_reason, REASON_LEN, 0644);

static const char * const reset_reasons[] = {
	"wdog", "kpanic", "reboot", "other",
	"dm_verity_enforcing", "dm_verity_logging",
	"dm_verity_keysclear", "fastboot_reboot",
};

static const char * const powerup_reasons[] = {
	"hw_reset", "smpl", "rtc", "dc_chg",
	"usb_chg", "pon1", "cable", "keypad",
	NULL, "long_power_key",
};

static const char * const powerdown_reasons[] = {
	"soft", "ps_hold", "pmic_wd", "gp2",
	"gp1", "kpd_and_resin", "resin", "long_power_key",
};

/* Called with reason_lock held. Serialize with module parameter writes. */
static void refresh_reasons(bool up, bool down)
{
	unsigned int value;

	kernel_param_lock(THIS_MODULE);
	if (up && powerup_reason[0] &&
	    !kstrtouint(powerup_reason, 16, &value))
		pu_reason = value;
	if (down && powerdown_reason[0] &&
	    !kstrtouint(powerdown_reason, 16, &value))
		pd_reason = value;
	kernel_param_unlock(THIS_MODULE);
}

static const char *select_powerup_reason(unsigned int pu, unsigned int pd)
{
	static const unsigned int order[] = { 0, 1, 2, 4, 3, 7, 5 };
	unsigned int reset = pu >> 16;
	unsigned int bit;
	size_t i;

	if ((pu & BIT(8)) || ((pu & BIT(0)) && (pd & BIT(1)))) {
		if (reset) {
			bit = __ffs((unsigned long)reset);
			if (bit < ARRAY_SIZE(reset_reasons)) {
				/* Retained binary maps reset bit 7 to reboot. */
				if (bit == 7)
					bit = 2;
				return reset_reasons[bit];
			}
		}
	}

	if (pd & BIT(7))
		return powerup_reasons[9];

	for (i = 0; i < ARRAY_SIZE(order); i++)
		if (pu & BIT(order[i]))
			return powerup_reasons[order[i]];

	return "unknown reboot";
}

static ssize_t powerup_reason_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	ssize_t ret;

	mutex_lock(&reason_lock);
	if (!current_pu_reason_str[0]) {
		refresh_reasons(true, true);
		strscpy(current_pu_reason_str,
			select_powerup_reason(pu_reason, pd_reason),
			sizeof(current_pu_reason_str));
	}
	ret = sysfs_emit(buf, "%s\n", current_pu_reason_str);
	mutex_unlock(&reason_lock);
	return ret;
}

static ssize_t powerup_reason_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	char token[REASON_LEN];
	size_t start = 0, end, length;

	/* Preserve the original first-whitespace-delimited-token behavior. */
	while (start < count && isspace((unsigned char)buf[start]))
		start++;
	end = start;
	while (end < count && !isspace((unsigned char)buf[end]) && buf[end])
		end++;

	length = end - start;
	if (!length)
		return count;
	if (length >= sizeof(token))
		return -E2BIG;

	memcpy(token, buf + start, length);
	token[length] = '\0';

	mutex_lock(&reason_lock);
	strscpy(current_pu_reason_str, token, sizeof(current_pu_reason_str));
	mutex_unlock(&reason_lock);
	return count;
}

static ssize_t powerup_reason_details_show(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  char *buf)
{
	ssize_t ret;

	mutex_lock(&reason_lock);
	refresh_reasons(true, false);
	ret = sysfs_emit(buf, "0x%x\n", pu_reason);
	mutex_unlock(&reason_lock);
	return ret;
}

static ssize_t poweroff_reason_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	const char *reason = "unknown reboot";
	ssize_t ret;

	mutex_lock(&reason_lock);
	refresh_reasons(false, true);
	if (pd_reason & 0xff)
		reason = powerdown_reasons[__ffs((unsigned long)(pd_reason & 0xff))];
	ret = sysfs_emit(buf, "%s\n", reason);
	mutex_unlock(&reason_lock);
	return ret;
}

static ssize_t poweroff_reason_details_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	ssize_t ret;

	mutex_lock(&reason_lock);
	refresh_reasons(false, true);
	ret = sysfs_emit(buf, "0x%x\n", pd_reason);
	mutex_unlock(&reason_lock);
	return ret;
}

static struct kobj_attribute powerup_reason_attr =
	__ATTR(powerup_reason, 0644, powerup_reason_show, powerup_reason_store);
static struct kobj_attribute powerup_reason_details_attr =
	__ATTR(powerup_reason_details, 0644, powerup_reason_details_show, NULL);
static struct kobj_attribute poweroff_reason_attr =
	__ATTR(poweroff_reason, 0644, poweroff_reason_show, NULL);
static struct kobj_attribute poweroff_reason_details_attr =
	__ATTR(poweroff_reason_details, 0644, poweroff_reason_details_show, NULL);

static struct attribute *bootinfo_attrs[] = {
	&powerup_reason_attr.attr,
	&powerup_reason_details_attr.attr,
	&poweroff_reason_attr.attr,
	&poweroff_reason_details_attr.attr,
	NULL,
};

static const struct attribute_group bootinfo_group = {
	.attrs = bootinfo_attrs,
};

static int __init bootinfo_init(void)
{
	int ret;

	bootinfo_kobj = kobject_create_and_add("bootinfo", NULL);
	if (!bootinfo_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(bootinfo_kobj, &bootinfo_group);
	if (ret) {
		kobject_put(bootinfo_kobj);
		bootinfo_kobj = NULL;
	}
	return ret;
}

static void __exit bootinfo_exit(void)
{
	sysfs_remove_group(bootinfo_kobj, &bootinfo_group);
	kobject_put(bootinfo_kobj);
}

module_init(bootinfo_init);
module_exit(bootinfo_exit);
MODULE_DESCRIPTION("Spring boot reason interface reconstruction");
MODULE_LICENSE("GPL");
