// SPDX-License-Identifier: GPL-2.0-only
#include <asm/unaligned.h>
#include <linux/blkdev.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <scsi/scsi.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

#include "spring-ffu-partition.h"
#include "spring-ffu-policy.h"

#define SPRING_FFU_READ_BLOCKS 128

/* The caller holds the SCSI device reference and keeps bdev open. */
int spring_ffu_rescue_extent(struct spring_ffu_partition *partition,
			     struct scsi_device *sdev,
			     struct block_device *bdev)
{
	const unsigned int sectors_per_block = SPRING_FFU_BLOCK_SIZE >> SECTOR_SHIFT;
	sector_t capacity, start, size;

	if (sdev->channel || sdev->id || sdev->lun ||
	    strncmp(sdev->host->hostt->proc_name ?: "", "ufshcd", 6) ||
	    sdev->sector_size != SPRING_FFU_BLOCK_SIZE ||
	    bdev_get_queue(bdev) != sdev->request_queue ||
	    bdev->bd_partno != 16 || !bdev->bd_meta_info ||
	    strcmp(bdev->bd_meta_info->volname, "rescue"))
		return -ENODEV;

	capacity = get_capacity(bdev->bd_disk);
	start = bdev->bd_start_sect;
	size = bdev_nr_sectors(bdev);
	if (!start || !size || start >= capacity || size > capacity - start ||
	    !IS_ALIGNED(start, sectors_per_block) ||
	    !IS_ALIGNED(size, sectors_per_block) ||
	    (start + size - 1) / sectors_per_block > U32_MAX)
		return -EINVAL;

	*partition = (struct spring_ffu_partition) {
		.sdev = sdev,
		.first_block = start / sectors_per_block,
		.block_count = size / sectors_per_block,
	};
	return 0;
}

/* The caller owns a device reference for the entire transaction. */
static int spring_ffu_partition_io(const struct spring_ffu_partition *partition,
				   u64 offset, void *buffer, unsigned int blocks,
				   bool write)
{
	struct scsi_device *sdev = partition->sdev;
	struct scsi_sense_hdr sense = {};
	int residual = 0;
	struct scsi_exec_args args = { .sshdr = &sense, .resid = &residual };
	u8 cdb[10] = { write ? WRITE_10 : READ_10 };
	unsigned long flags;
	bool noresume;
	int ret;

	if (!sdev || sdev->sector_size != SPRING_FFU_BLOCK_SIZE ||
	    !scsi_device_online(sdev))
		return -ENODEV;
	if (!blocks || blocks > SPRING_FFU_READ_BLOCKS ||
	    offset >= partition->block_count ||
	    blocks > partition->block_count - offset ||
	    partition->first_block > U32_MAX ||
	    offset > U32_MAX - partition->first_block ||
	    blocks - 1 > U32_MAX - partition->first_block - offset)
		return -EINVAL;

	put_unaligned_be32(partition->first_block + offset, cdb + 2);
	put_unaligned_be16(blocks, cdb + 7);

	spin_lock_irqsave(sdev->host->host_lock, flags);
	noresume = sdev->host->eh_noresume;
	sdev->host->eh_noresume = 1;
	spin_unlock_irqrestore(sdev->host->host_lock, flags);
	ret = scsi_execute_cmd(sdev, cdb, write ? REQ_OP_DRV_OUT : REQ_OP_DRV_IN,
			       buffer, blocks * SPRING_FFU_BLOCK_SIZE,
			       15 * HZ, 3, &args);
	spin_lock_irqsave(sdev->host->host_lock, flags);
	sdev->host->eh_noresume = noresume;
	spin_unlock_irqrestore(sdev->host->host_lock, flags);

	return ret > 0 || (!ret && residual) ? -EIO : ret;
}

int spring_ffu_read_partition(const struct spring_ffu_partition *partition,
			      u64 offset, void *data, size_t size)
{
	size_t block_count;
	size_t remaining;
	u64 block;
	void *buffer;
	int ret = 0;

	if (!size || !IS_ALIGNED(offset, SPRING_FFU_BLOCK_SIZE) ||
	    !IS_ALIGNED(size, SPRING_FFU_BLOCK_SIZE))
		return -EINVAL;
	block = offset / SPRING_FFU_BLOCK_SIZE;
	block_count = size / SPRING_FFU_BLOCK_SIZE;
	if (block >= partition->block_count ||
	    block_count > partition->block_count - block)
		return -EINVAL;

	buffer = kmalloc(min_t(size_t, size, SPRING_FFU_READ_BLOCKS *
					      SPRING_FFU_BLOCK_SIZE), GFP_NOIO);
	if (!buffer)
		return -ENOMEM;

	for (remaining = block_count; remaining;) {
		unsigned int count = min_t(size_t, remaining, SPRING_FFU_READ_BLOCKS);
		size_t bytes = count * SPRING_FFU_BLOCK_SIZE;

		ret = spring_ffu_partition_io(partition, block, buffer, count, false);
		if (ret)
			break;
		memcpy(data, buffer, bytes);
		data += bytes;
		block += count;
		remaining -= count;
	}
	kfree(buffer);
	return ret;
}

/* The caller supplies a full SPRING_FFU_BLOCK_SIZE buffer. */
int spring_ffu_read_header(const struct spring_ffu_partition *partition,
			   u8 *header)
{
	int ret;

	ret = spring_ffu_read_partition(partition, 0, header, SPRING_FFU_BLOCK_SIZE);
	if (ret)
		return ret;

	return memcmp(header, "ffu", sizeof("ffu")) ? -ENODATA : 0;
}

int spring_ffu_invalidate_partition(const struct spring_ffu_partition *partition,
				    const u8 *expected_header)
{
	u8 *header;
	int ret;

	if (memcmp(expected_header, "ffu", sizeof("ffu")))
		return -EINVAL;

	header = kmalloc(SPRING_FFU_BLOCK_SIZE, GFP_NOIO);
	if (!header)
		return -ENOMEM;
	ret = spring_ffu_partition_io(partition, 0, header, 1, false);
	if (ret)
		goto out;
	if (memcmp(header, expected_header, SPRING_FFU_BLOCK_SIZE)) {
		ret = -ESTALE;
		goto out;
	}

	memset(header, 0, SPRING_FFU_BLOCK_SIZE);
	ret = spring_ffu_partition_io(partition, 0, header, 1, true);
out:
	kfree(header);
	return ret;
}
