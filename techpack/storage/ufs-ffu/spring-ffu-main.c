// SPDX-License-Identifier: GPL-2.0-only
#include <linux/blkdev.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

#include "spring-ffu-finish.h"
#include "spring-ffu-match.h"
#include "spring-ffu-partition.h"
#include "spring-ffu-policy.h"

static bool apply_builtin;
module_param(apply_builtin, bool, 0400);
MODULE_PARM_DESC(apply_builtin, "Enable writes for verified built-in firmware rules");

static void spring_ffu_apply_builtin(struct scsi_device *sdev,
				    const struct spring_ffu_update *update)
{
	const struct firmware *firmware;
	bool attempted;
	int ret;

	ret = spring_ffu_check_boot();
	if (ret)
		goto report;

	ret = request_firmware_direct(&firmware, update->firmware, &sdev->sdev_gendev);
	if (ret)
		goto report;

	ret = spring_ffu_write_payload(sdev, update, firmware, &attempted);
	release_firmware(firmware);
	if (attempted) {
		if (ret)
			pr_err("ufs_ffu: firmware transfer failed (%d)\n", ret);
		ret = spring_ffu_finish_update(ret, NULL, NULL);
	}

report:
	if (ret)
		pr_err("ufs_ffu: built-in update stopped (%d)\n", ret);
}

static int spring_ffu_match_disk(struct device *dev, void *data)
{
	struct scsi_device *sdev = data;

	return dev->class && !strcmp(dev->class->name, "block") &&
	       dev->type && dev->type->name && !strcmp(dev->type->name, "disk") &&
	       dev_to_disk(dev)->queue == sdev->request_queue;
}

static void spring_ffu_report_candidate(const struct spring_ffu_partition *partition,
					const u8 *header)
{
	struct scsi_device *sdev = partition->sdev;
	struct spring_ffu_partition_entry entry;
	const struct spring_ffu_update *update;
	char vendor[9], model[17], revision[5];
	char *vendor_id, *model_id, *current_rev;
	size_t read_size;
	void *payload;
	int ret;

	snprintf(vendor, sizeof(vendor), "%.8s", sdev->vendor);
	snprintf(model, sizeof(model), "%.16s", sdev->model);
	snprintf(revision, sizeof(revision), "%.4s", sdev->rev);
	vendor_id = strim(vendor);
	model_id = strim(model);
	current_rev = strim(revision);

	pr_info("ufs_ffu: dry run: vendor=%s model=%s revision=%s\n",
		vendor_id, model_id, current_rev);
	if (header) {
		ret = spring_ffu_find_partition_entry(header, SPRING_FFU_BLOCK_SIZE,
				partition->block_count * SPRING_FFU_BLOCK_SIZE,
				vendor_id, model_id, current_rev, &entry);
		if (ret == -ENOENT)
			goto builtin;
		if (ret) {
			pr_info("ufs_ffu: dry run: rescue table rejected (%d)\n", ret);
			return;
		}

		read_size = ALIGN(entry.size, SPRING_FFU_BLOCK_SIZE);
		payload = kvmalloc(read_size, GFP_KERNEL);
		if (!payload) {
			pr_info("ufs_ffu: dry run: rescue payload allocation failed\n");
			return;
		}
		ret = spring_ffu_read_partition(partition, entry.offset, payload, read_size);
		if (!ret)
			ret = spring_ffu_check_partition_payload(&entry, payload, entry.size);
		kvfree(payload);
		if (ret)
			pr_info("ufs_ffu: dry run: rescue payload rejected (%d)\n", ret);
		else
			pr_info("ufs_ffu: dry run: rescue candidate passed CRC (%u bytes)\n",
				entry.size);
		return;
	}

builtin:
	update = spring_ffu_find_update(vendor_id, model_id, current_rev);
	if (update) {
		pr_info("ufs_ffu: dry run: built-in candidate %s -> %s (%s)\n",
			current_rev, update->to_revision, update->firmware);
		if (apply_builtin)
			spring_ffu_apply_builtin(sdev, update);
	} else {
		pr_info("ufs_ffu: dry run: no built-in update matches\n");
	}
}

static int spring_ffu_probe_rescue(struct scsi_device *sdev)
{
	struct spring_ffu_partition partition;
	struct block_device *bdev, *part;
	struct device *disk_dev;
	dev_t devt = 0;
	u8 *header;
	int ret;

	disk_dev = device_find_child(&sdev->sdev_gendev, sdev, spring_ffu_match_disk);
	if (!disk_dev)
		return -ENODEV;

	rcu_read_lock();
	part = xa_load(&dev_to_disk(disk_dev)->part_tbl, 16);
	if (part)
		devt = part->bd_dev;
	rcu_read_unlock();
	if (!devt) {
		ret = -ENODEV;
		goto put_disk;
	}

	bdev = blkdev_get_by_dev(devt, FMODE_READ, NULL);
	if (IS_ERR(bdev)) {
		ret = PTR_ERR(bdev);
		goto put_disk;
	}

	ret = spring_ffu_rescue_extent(&partition, sdev, bdev);
	if (ret)
		goto put_bdev;

	header = kmalloc(SPRING_FFU_BLOCK_SIZE, GFP_KERNEL);
	if (!header) {
		ret = -ENOMEM;
		goto put_bdev;
	}

	ret = spring_ffu_read_header(&partition, header);
	if (!ret || ret == -ENODATA)
		spring_ffu_report_candidate(&partition, ret ? NULL : header);
	kfree(header);

put_bdev:
	blkdev_put(bdev, FMODE_READ);
put_disk:
	put_device(disk_dev);
	return ret;
}

static int spring_ffu_probe(void)
{
	struct Scsi_Host *host;
	struct scsi_device *sdev;
	int ret;

	host = scsi_host_lookup(0);
	if (!host)
		return -ENODEV;
	sdev = scsi_device_lookup(host, 0, 0, 0);
	scsi_host_put(host);
	if (!sdev)
		return -ENODEV;

	ret = spring_ffu_probe_rescue(sdev);
	scsi_device_put(sdev);
	return ret;
}

static unsigned int spring_ffu_attempts;
static void spring_ffu_probe_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(spring_ffu_work, spring_ffu_probe_work);

static void spring_ffu_probe_work(struct work_struct *work)
{
	int ret = spring_ffu_probe();

	if ((ret == -ENODEV || ret == -ENXIO) && ++spring_ffu_attempts < 10) {
		schedule_delayed_work(&spring_ffu_work, HZ);
		return;
	}

	if (ret == -ENODATA)
		pr_info("ufs_ffu: rescue has no active FFU header\n");
	else if (!ret)
		pr_info("ufs_ffu: rescue FFU header detected\n");
	else
		pr_info("ufs_ffu: rescue probe failed (%d)\n", ret);
}

static int __init spring_ffu_init(void)
{
	int ret = spring_ffu_check_boot();

	if (ret)
		pr_info("ufs_ffu: boot guard skipped probe (%d)\n", ret);
	else
		schedule_delayed_work(&spring_ffu_work, 0);
	return 0;
}

static void __exit spring_ffu_exit(void)
{
	cancel_delayed_work_sync(&spring_ffu_work);
}

module_init(spring_ffu_init);
module_exit(spring_ffu_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Spring UFS firmware update header probe");
