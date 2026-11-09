// SPDX-License-Identifier: GPL-2.0-only
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <scsi/scsi.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

#include "spring-ffu-match.h"

#define SPRING_FFU_CHUNK_SIZE 0x80000
#define SPRING_FFU_DOWNLOAD_MODE 0x0e
#define SPRING_FFU_MAX_SIZE 0x1000000

static int spring_ffu_write_chunk(struct scsi_device *sdev, void *buffer,
				  unsigned int offset, unsigned int length)
{
	struct scsi_sense_hdr sense;
	int residual = 0;
	struct scsi_exec_args args = { .sshdr = &sense, .resid = &residual };
	u8 cdb[10] = {
		WRITE_BUFFER, SPRING_FFU_DOWNLOAD_MODE, 0,
		offset >> 16, offset >> 8, offset,
		length >> 16, length >> 8, length, 0,
	};
	unsigned long flags;
	bool noresume;
	int ret;

	spin_lock_irqsave(sdev->host->host_lock, flags);
	noresume = sdev->host->eh_noresume;
	sdev->host->eh_noresume = 1;
	spin_unlock_irqrestore(sdev->host->host_lock, flags);

	ret = scsi_execute_cmd(sdev, cdb, REQ_OP_DRV_OUT, buffer, length,
			       15 * HZ, 0, &args);

	spin_lock_irqsave(sdev->host->host_lock, flags);
	sdev->host->eh_noresume = noresume;
	spin_unlock_irqrestore(sdev->host->host_lock, flags);

	if (ret > 0 || (!ret && residual))
		return -EIO;
	return ret;
}

/* The caller owns a device reference and serializes the FFU transaction. */
int spring_ffu_write_payload(struct scsi_device *sdev,
			     const struct spring_ffu_update *update,
			     const struct firmware *firmware, bool *attempted)
{
	u8 digest[SHA256_DIGEST_SIZE];
	unsigned int offset;
	unsigned int length;
	void *buffer;
	int ret = 0;

	*attempted = false;
	if (!update || !firmware || !firmware->size ||
	    firmware->size != update->size || firmware->size > SPRING_FFU_MAX_SIZE)
		return -EINVAL;
	if (!scsi_device_online(sdev))
		return -ENODEV;

	sha256(firmware->data, firmware->size, digest);
	if (memcmp(digest, update->digest, sizeof(digest)))
		return -EBADMSG;

	buffer = kmalloc(min_t(size_t, firmware->size, SPRING_FFU_CHUNK_SIZE),
			 GFP_NOIO);
	if (!buffer)
		return -ENOMEM;

	for (offset = 0; offset < firmware->size; offset += length) {
		length = min_t(size_t, firmware->size - offset, SPRING_FFU_CHUNK_SIZE);
		memcpy(buffer, firmware->data + offset, length);
		*attempted = true;
		ret = spring_ffu_write_chunk(sdev, buffer, offset, length);
		if (ret)
			break;
	}
	kfree(buffer);
	return ret;
}
