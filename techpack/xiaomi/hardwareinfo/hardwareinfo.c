// SPDX-License-Identifier: GPL-2.0
/*
 * Adapted from Xiaomi hardwareinfo source for Spring.
 * Minidump name and buffer sizes verified against Spring's module.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <asm/io.h>
#include <soc/qcom/minidump.h>

#define MAX_HARDWAREINFO_LEN 4096
#define DATABUF_LEN 256

static DEFINE_MUTEX(hwinfo_mutex);
static char *hardwareinfo_buff;
static struct kobject *hardware_info_to_dump_kobj;

static ssize_t read_xiaomi_hardw_info(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	ssize_t ret;

	mutex_lock(&hwinfo_mutex);
	ret = sysfs_emit(buf, "%s\n", hardwareinfo_buff);
	mutex_unlock(&hwinfo_mutex);
	return ret;
}

static ssize_t write_xiaomi_hardw_info(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	size_t incoming = strnlen(buf, min_t(size_t, count, DATABUF_LEN - 1));
	size_t used;
	ssize_t ret = count;

	if (!incoming)
		return -EINVAL;

	mutex_lock(&hwinfo_mutex);
	used = strnlen(hardwareinfo_buff, MAX_HARDWAREINFO_LEN);
	if (used >= MAX_HARDWAREINFO_LEN ||
	    incoming >= MAX_HARDWAREINFO_LEN - used) {
		ret = -ENOSPC;
		goto unlock;
	}

	memcpy(hardwareinfo_buff + used, buf, incoming);
	hardwareinfo_buff[used + incoming] = '\0';
unlock:
	mutex_unlock(&hwinfo_mutex);
	return ret;
}

static struct kobj_attribute hardw_info_attr =
	__ATTR(hardw_info, 0600, read_xiaomi_hardw_info, write_xiaomi_hardw_info);

static struct attribute *xiaomi_hardwareinfo_attrs[] = {
	&hardw_info_attr.attr,
	NULL,
};

static const struct attribute_group attr_hardwareinfo_group = {
	.attrs = xiaomi_hardwareinfo_attrs,
};

static int __init hardware_info_to_dump_init(void)
{
	struct md_region region = {0};
	int ret;

	hardwareinfo_buff = kzalloc(MAX_HARDWAREINFO_LEN, GFP_KERNEL);
	if (!hardwareinfo_buff)
		return -ENOMEM;

	hardware_info_to_dump_kobj =
		kobject_create_and_add("hardware_info_to_dump", kernel_kobj);
	if (!hardware_info_to_dump_kobj) {
		ret = -ENOMEM;
		goto free_buffer;
	}

	ret = sysfs_create_group(hardware_info_to_dump_kobj,
				 &attr_hardwareinfo_group);
	if (ret)
		goto put_kobject;

	strscpy(region.name, "HARDW_INFO", sizeof(region.name));
	region.virt_addr = (uintptr_t)hardwareinfo_buff;
	region.phys_addr = virt_to_phys(hardwareinfo_buff);
	region.size = MAX_HARDWAREINFO_LEN;

	ret = msm_minidump_add_region(&region);
	if (ret < 0) {
		pr_err("Failed to add hardw_info data in Minidump: %d\n", ret);
		goto remove_group;
	}

	return 0;

remove_group:
	sysfs_remove_group(hardware_info_to_dump_kobj, &attr_hardwareinfo_group);
put_kobject:
	kobject_put(hardware_info_to_dump_kobj);
	hardware_info_to_dump_kobj = NULL;
free_buffer:
	kfree(hardwareinfo_buff);
	hardwareinfo_buff = NULL;
	return ret;
}

/* No module_exit: registered minidump storage remains valid until reboot. */
module_init(hardware_info_to_dump_init);
MODULE_LICENSE("GPL");
