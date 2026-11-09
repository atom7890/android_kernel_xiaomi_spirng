#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/reboot.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/efi.h>
#include <linux/rcupdate.h>
#include <linux/of.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_host.h>
#include <linux/completion.h>
#include "sd.h"
#include "charger_partition.h"

// extern struct completion ufs_xiaomi_comp;

#define DEFAULT_SDA_PART_NO (5)
#define CN_SDA_PART_NO (5)
#define IN_SDA_PART_NO (5)
#define GL_SDA_PART_NO (5)
#define OT_SDA_PART_NO (5)

struct ChargerPartition {
	struct scsi_device *sdev;
	struct delayed_work charger_partition_work;

	int part_info_part_number;
	char *part_info_part_name;

	//bool is_charger_partition_rdy;
};
static struct ChargerPartition *charger_partition;
static void *rw_buf;
static DEFINE_MUTEX(charger_transaction_lock);
static struct task_struct *charger_transaction_owner;
static bool chg_parti_is_rdy = false;

typedef struct part {
	sector_t part_start;
	sector_t part_size;
} partinfo;
static partinfo part_info = { 0 };

static int charger_scsi_read_partition(struct scsi_device *sdev, void *buf, uint32_t lba, uint32_t blocks)
{
	uint8_t cdb[16] = {0};
	int ret = 0;
	struct scsi_sense_hdr sshdr = {};
	int residual = 0;
	const struct scsi_exec_args exec_args = {
		.sshdr = &sshdr,
		.resid = &residual,
	};
	unsigned long flags = 0;
	bool noresume;

	spin_lock_irqsave(sdev->host->host_lock, flags);
	ret = scsi_device_get(sdev);
	if (!ret && !scsi_device_online(sdev)) {
		ret = -ENODEV;
		scsi_device_put(sdev);
		pr_err("[charger] %s get device fail\n", __func__);
	}
	spin_unlock_irqrestore(sdev->host->host_lock, flags);
	if (ret) {
                pr_err("[charger] %s 2\n", __func__);
		return ret;
        }
	spin_lock_irqsave(sdev->host->host_lock, flags);
	noresume = sdev->host->eh_noresume;
	sdev->host->eh_noresume = 1;
	spin_unlock_irqrestore(sdev->host->host_lock, flags);

	// Fill in the CDB with SCSI command structure
	cdb[0] = READ_10;				// Command
	cdb[1] = 0;
	cdb[2] = (lba >> 24) & 0xff;	// LBA
 	cdb[3] = (lba >> 16) & 0xff;
 	cdb[4] = (lba >> 8) & 0xff;
	cdb[5] = (lba) & 0xff;
	cdb[6] = 0;						// Group Number
	cdb[7] = (blocks >> 8) & 0xff;	// Transfer Len
	cdb[8] = (blocks) & 0xff;
	cdb[9] = 0;						// Control
	ret = scsi_execute_cmd(sdev, cdb, REQ_OP_DRV_IN, buf, (blocks * PART_BLOCK_SIZE), HZ, 2, &exec_args);
	spin_lock_irqsave(sdev->host->host_lock, flags);
	sdev->host->eh_noresume = noresume;
	spin_unlock_irqrestore(sdev->host->host_lock, flags);
	scsi_device_put(sdev);
	if (ret > 0 || (!ret && residual))
		ret = -EIO;
	if (ret)
		pr_err("[charger] %s read error %d\n", __func__, ret);
	return ret;
}

static int charger_scsi_write_partition(struct scsi_device *sdev, void *buf, uint32_t lba, uint32_t blocks)
{
	uint8_t cdb[16] = {0};
	int ret = 0;
	struct scsi_sense_hdr sshdr = {};
	int residual = 0;
	const struct scsi_exec_args exec_args = {
		.sshdr = &sshdr,
		.resid = &residual,
	};
	unsigned long flags = 0;
	bool noresume;

	spin_lock_irqsave(sdev->host->host_lock, flags);
	ret = scsi_device_get(sdev);
	if (!ret && !scsi_device_online(sdev)) {
		ret = -ENODEV;
		scsi_device_put(sdev);
		pr_err("[charger] %s get device fail\n", __func__);
	}
	spin_unlock_irqrestore(sdev->host->host_lock, flags);

	if (ret) {
                pr_err("[charger] %s get scsi fail\n", __func__);
		return ret;
        }
	spin_lock_irqsave(sdev->host->host_lock, flags);
	noresume = sdev->host->eh_noresume;
	sdev->host->eh_noresume = 1;
	spin_unlock_irqrestore(sdev->host->host_lock, flags);

	// Fill in the CDB with SCSI command structure
	cdb[0] = WRITE_10;				// Command
	cdb[1] = 0;
	cdb[2] = (lba >> 24) & 0xff;	// LBA
	cdb[3] = (lba >> 16) & 0xff;
	cdb[4] = (lba >> 8) & 0xff;
	cdb[5] = (lba) & 0xff;
	cdb[6] = 0;						// Group Number
	cdb[7] = (blocks >> 8) & 0xff;	// Transfer Len
	cdb[8] = (blocks) & 0xff;
	cdb[9] = 0;					// Control
	ret = scsi_execute_cmd(sdev, cdb, REQ_OP_DRV_OUT, buf, (blocks * PART_BLOCK_SIZE), HZ, 2, &exec_args);
	spin_lock_irqsave(sdev->host->host_lock, flags);
	sdev->host->eh_noresume = noresume;
	spin_unlock_irqrestore(sdev->host->host_lock, flags);
	scsi_device_put(sdev);
	if (ret > 0 || (!ret && residual))
		ret = -EIO;
	if (ret)
		pr_err("[charger] %s write error %d\n", __func__, ret);
	return ret;
}

int charger_partition_alloc(u8 charger_partition_host_type, u8 charger_partition_info_type, uint32_t size)
{
	int ret = 0;

	if(!chg_parti_is_rdy) {
		pr_err("[charger] charger partition not rdy, can't do rw!");
		return -1;
	}
	if(charger_partition_host_type >= CHARGER_PARTITION_HOST_LAST) {
		pr_err("[charger] charger_partition_host_type not support!");
		return -1;
	}
	if(charger_partition_info_type >= CHARGER_PARTITION_INFO_LAST) {
		pr_err("[charger] charger_partition_info_type not support!");
		return -1;
	}
	if(size >= CHARGER_PARTITION_RWSIZE) {
		pr_err("[charger] read %u size not support!", size);
		return -1;
	}

	mutex_lock(&charger_transaction_lock);
	if (rw_buf) {
		ret = -EBUSY;
		goto unlock;
	}
	rw_buf = kzalloc(CHARGER_PARTITION_RWSIZE, GFP_KERNEL);
	if(!rw_buf){
		pr_err("[charger] malloc buf error!");
		ret = -ENOMEM;
		goto unlock;
	}

	/* check if avaliable */
	ret = charger_scsi_read_partition(charger_partition->sdev, rw_buf, (part_info.part_start + CHARGER_PARTITION_HEADER), CHARGER_PARTITION_RWSIZE / PART_BLOCK_SIZE);
	if (ret) {
		pr_err("[charger] %s charger read error %d\n", __func__, ret);
		goto free_buffer;
	}
	pr_debug("[charger] %s avaliable:%u\n", __func__, ((charger_partition_header *)rw_buf)->avaliable);
	if(0 == ((charger_partition_header *)rw_buf)->avaliable) {
		pr_err("[charger] %s not avaliable, can't do rw now!\n", __func__);
		ret = -EBUSY;
		goto free_buffer;
	}
	((charger_partition_header *)rw_buf)->avaliable = 0;
	pr_debug("[charger] %s set avaliable:%u\n", __func__, ((charger_partition_header *)rw_buf)->avaliable);
	ret = charger_scsi_write_partition(charger_partition->sdev, (void *)rw_buf, (part_info.part_start + CHARGER_PARTITION_HEADER), CHARGER_PARTITION_RWSIZE / PART_BLOCK_SIZE);
	if (ret) {
		pr_err("[charger] %s charger write error %d\n", __func__, ret);
		goto free_buffer;
	}

	WRITE_ONCE(charger_transaction_owner, current);
	goto unlock;

free_buffer:
	kfree(rw_buf);
	rw_buf = NULL;
unlock:
	mutex_unlock(&charger_transaction_lock);
	return ret;
}
EXPORT_SYMBOL(charger_partition_alloc);

int charger_partition_dealloc(u8 charger_partition_host_type, u8 charger_partition_info_type, uint32_t size)
{
	int ret = 0;

	if(!chg_parti_is_rdy) {
		pr_err("[charger] charger partition not rdy, can't do rw!");
		return -1;
	}
	if(charger_partition_host_type >= CHARGER_PARTITION_HOST_LAST) {
		pr_err("[charger] charger_partition_host_type not support!");
		return -1;
	}
	if(charger_partition_info_type >= CHARGER_PARTITION_INFO_LAST) {
		pr_err("[charger] charger_partition_info_type not support!");
		return -1;
	}
	if(size >= CHARGER_PARTITION_RWSIZE) {
		pr_err("[charger] read %u size not support!", size);
		return -1;
	}

	mutex_lock(&charger_transaction_lock);
	if (charger_transaction_owner != current) {
		mutex_unlock(&charger_transaction_lock);
		return -EPERM;
	}
	memset(rw_buf, 0, CHARGER_PARTITION_RWSIZE);
	ret = charger_scsi_read_partition(charger_partition->sdev, rw_buf, (part_info.part_start + CHARGER_PARTITION_HEADER), CHARGER_PARTITION_RWSIZE / PART_BLOCK_SIZE);
	if (ret) {
		pr_err("[charger] %s charger read error %d\n", __func__, ret);
		goto free_buffer;
	}
	((charger_partition_header *)rw_buf)->avaliable = 1;
	pr_debug("[charger] %s set avaliable:%u\n", __func__, ((charger_partition_header *)rw_buf)->avaliable);
	ret = charger_scsi_write_partition(charger_partition->sdev, (void *)rw_buf, (part_info.part_start + CHARGER_PARTITION_HEADER), CHARGER_PARTITION_RWSIZE / PART_BLOCK_SIZE);
	if (ret)
		pr_err("[charger] %s charger write error %d\n", __func__, ret);

free_buffer:
	WRITE_ONCE(charger_transaction_owner, NULL);
	kfree(rw_buf);
	rw_buf = NULL;
	mutex_unlock(&charger_transaction_lock);
	return ret;
}
EXPORT_SYMBOL(charger_partition_dealloc);

void *charger_partition_read(u8 charger_partition_host_type, u8 charger_partition_info_type, uint32_t size)
{
	int ret = 0;

	if(!chg_parti_is_rdy) {
		pr_err("[charger] charger partition not rdy, can't do read!");
		return NULL;
	}
	if (READ_ONCE(charger_transaction_owner) != current) {
		pr_err("[charger] caller does not own the charger transaction!");
		return NULL;
	}
	if(charger_partition_host_type >= CHARGER_PARTITION_HOST_LAST) {
		pr_err("[charger] charger_partition_host_type not support!");
		return NULL;
	}
	if(charger_partition_info_type >= CHARGER_PARTITION_INFO_LAST) {
		pr_err("[charger] charger_partition_info_type not support!");
		return NULL;
	}
	if(size >= CHARGER_PARTITION_RWSIZE) {
		pr_err("[charger] read %u size not support!", size);
		return NULL;
	}

	memset(rw_buf, 0, CHARGER_PARTITION_RWSIZE);
	ret = charger_scsi_read_partition(charger_partition->sdev, rw_buf, (part_info.part_start + charger_partition_info_type), CHARGER_PARTITION_RWSIZE / PART_BLOCK_SIZE);
	if (ret) {
		pr_err("[charger] %s charger read error %d\n", __func__, ret);
		return NULL;
	}
	return rw_buf;
}
EXPORT_SYMBOL(charger_partition_read);

int charger_partition_write(u8 charger_partition_host_type, u8 charger_partition_info_type, void *buf, uint32_t size)
{
	int ret = 0;
	uint32_t offset;

	if(!chg_parti_is_rdy) {
		pr_err("[charger] charger partition not rdy, can't do read!");
		return -1;
	}
	if (READ_ONCE(charger_transaction_owner) != current) {
		pr_err("[charger] caller does not own the charger transaction!");
		return -1;
	}
	if(charger_partition_host_type >= CHARGER_PARTITION_HOST_LAST) {
		pr_err("[charger] charger_partition_host_type not support!");
		return -1;
	}
	if(charger_partition_info_type >= CHARGER_PARTITION_INFO_LAST) {
		pr_err("[charger] charger_partition_info_type not support!");
		return -1;
	}
	if(size >= CHARGER_PARTITION_RWSIZE) {
		pr_err("[charger] read %u size not support!", size);
		return -1;
	}

	memset(rw_buf, 0, CHARGER_PARTITION_RWSIZE);
	ret = charger_scsi_read_partition(charger_partition->sdev, rw_buf, (part_info.part_start + charger_partition_info_type), CHARGER_PARTITION_RWSIZE / PART_BLOCK_SIZE);
	if (ret) {
		pr_err("[charger] %s charger read error %d\n", __func__, ret);
		return -1;
	}
	memcpy(rw_buf, buf, size);

	ret = charger_scsi_write_partition(charger_partition->sdev, rw_buf, (part_info.part_start + charger_partition_info_type), CHARGER_PARTITION_RWSIZE / PART_BLOCK_SIZE);
	if (ret) {
		pr_err("[charger] %s charger write error %d\n", __func__, ret);
		return -1;
	}

	offset = part_info.part_start + charger_partition_info_type;
	pr_err("[charger] %s: start: %llu, info_type:%d, offset:%llu \n", __func__, part_info.part_start, charger_partition_info_type, offset);
	return ret;
}
EXPORT_SYMBOL(charger_partition_write);

static bool look_up_scsi_device(int lun)
{
	struct Scsi_Host *shost;

	shost = scsi_host_lookup(0);
	if (!shost)
		return false;

	charger_partition->sdev = scsi_device_lookup(shost, 0, 0, lun);
	scsi_host_put(shost);
	return charger_partition->sdev != NULL;
}

static bool check_device_is_correct(void)
{
	pr_err("[charger] %s proc name is %s", __func__, charger_partition->sdev->host->hostt->proc_name);
	if (strncmp(charger_partition->sdev->host->hostt->proc_name, UFSHCD, strlen(UFSHCD))) {
		/*check if the device is ufs. If not, just return directly.*/
                pr_err("[charger] %s proc name is not ufshcd, name: %s", __func__, charger_partition->sdev->host->hostt->proc_name);
		return false;
	}

	return true;
}

static bool get_charger_partition_info(void)
{
	struct scsi_device *sdev = charger_partition->sdev;
	const unsigned int sectors_per_block = PART_BLOCK_SIZE / PART_SECTOR_SIZE;
	struct scsi_disk *sdkp;
	struct block_device *part;
	sector_t start, size, capacity;
	bool valid = false;

	if (sdev->lun || sdev->sector_size != PART_BLOCK_SIZE)
		return false;

	device_lock(&sdev->sdev_gendev);
	sdkp = dev_get_drvdata(&sdev->sdev_gendev);
	if (!sdkp || !sdkp->disk)
		goto unlock_device;

	rcu_read_lock();
	part = xa_load(&sdkp->disk->part_tbl, charger_partition->part_info_part_number);
	if (!part || !part->bd_meta_info ||
	    strcmp(part->bd_meta_info->volname, "charger"))
		goto unlock_rcu;

	start = part->bd_start_sect;
	size = bdev_nr_sectors(part);
	capacity = get_capacity(sdkp->disk);
	if (!start || start >= capacity || size > capacity - start ||
	    !IS_ALIGNED(start, sectors_per_block) ||
	    !IS_ALIGNED(size, sectors_per_block) ||
	    size / sectors_per_block < CHARGER_PARTITION_INFO_LAST ||
	    (start + size - 1) / sectors_per_block > U32_MAX)
		goto unlock_rcu;

	part_info.part_start = start / sectors_per_block;
	part_info.part_size = size / sectors_per_block;
	valid = true;

unlock_rcu:
	rcu_read_unlock();
unlock_device:
	device_unlock(&sdev->sdev_gendev);
	return valid;
}

int check_charger_partition_header(void)
{
	int ret = 0;
	charger_partition_header *header = NULL;

	header = kzalloc(CHARGER_PARTITION_RWSIZE, GFP_KERNEL);
	if(!header){
		pr_err("[charger] malloc buf error!");
		return -1;
	}

	ret = charger_scsi_read_partition(charger_partition->sdev, (void *)header, (part_info.part_start + CHARGER_PARTITION_HEADER), CHARGER_PARTITION_RWSIZE / PART_BLOCK_SIZE);
	if (ret) {
		pr_err("[charger] %s charger read error %d\n", __func__, ret);
		kfree(header);
		return -1;
	}

	if(header->magic != CHARGER_PARTITION_MAGIC) {
		pr_err("[charger] %s magic error, set to default!\n", __func__);
		header->magic = CHARGER_PARTITION_MAGIC;
	}
	header->initialized = 1;
	header->avaliable = 1;
	pr_err("[charger] %s initiablized ok\n", __func__);

	ret = charger_scsi_write_partition(charger_partition->sdev, (void *)header, (part_info.part_start + CHARGER_PARTITION_HEADER), CHARGER_PARTITION_RWSIZE / PART_BLOCK_SIZE);
	if (ret) {
		pr_err("[charger] %s charger write error %d\n", __func__, ret);
		kfree(header);
		return -1;
	}

	ret = charger_scsi_read_partition(charger_partition->sdev, (void *)header, (part_info.part_start + CHARGER_PARTITION_HEADER), CHARGER_PARTITION_RWSIZE / PART_BLOCK_SIZE);
	if (ret) {
		pr_err("[charger] %s charger read error %d\n", __func__, ret);
		kfree(header);
		return -1;
	}
	pr_debug("[charger] %s magic:0x%0x, version:%d\n", __func__, header->magic, header->version);

	kfree(header);
	return ret;
}

int get_charger_partition_info_1(void)
{
	int ret = 0;
	charger_partition_info_1 *info_1 = NULL;

	ret = charger_partition_alloc(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_1, sizeof(charger_partition_info_1));
	if(ret < 0) {
		pr_err("[charger] %s failed to alloc\n", __func__);
		return -1;
	}

	info_1 = (charger_partition_info_1 *)charger_partition_read(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_1, sizeof(charger_partition_info_1));
	if(!info_1) {
		pr_err("[charger] %s failed to read\n", __func__);
		ret = charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_1, sizeof(charger_partition_info_1));
		if(ret < 0) {
			pr_err("[charger] %s failed to dealloc\n", __func__);
			return -1;
		}
		return -1;
	}
	pr_err("[charger] %s ret: %d, info_1->test: 0x%0x, zero_speed_mode: %u, power_off_mode: %u\n", __func__, ret, info_1->test, info_1->zero_speed_mode, info_1->power_off_mode);

	ret = charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_1, sizeof(charger_partition_info_1));
	if(ret < 0) {
		pr_err("[charger] %s failed to dealloc\n", __func__);
		return -1;
	}
	return 0;
}

int set_charger_partition_info_1(void)
{
	int ret = 0;
	charger_partition_info_1 info_1 = {.power_off_mode = 2, .zero_speed_mode = 2, .test = 0x23456789, .reserved = 0};

	ret = charger_partition_alloc(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_1, sizeof(charger_partition_info_1));
	if(ret < 0) {
		pr_err("[charger] %s failed to alloc\n", __func__);
		return -1;
	}

	ret = charger_partition_write(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_1, (void *)&info_1, sizeof(charger_partition_info_1));
	if(ret < 0) {
		pr_err("[charger] %s failed to write\n", __func__);
		ret = charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_1, sizeof(charger_partition_info_1));
		if(ret < 0) {
			pr_err("[charger] %s failed to dealloc\n", __func__);
			return -1;
		}
		return -1;
	}

	ret = charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_1, sizeof(charger_partition_info_1));
	if(ret < 0) {
		pr_err("[charger] %s failed to dealloc\n", __func__);
		return -1;
	}
	return 0;
}

int charger_partition_get_prop(int type, int *val);
static int eu_mode = -EINVAL;
static void charger_partition_work(struct work_struct *work)
{
	charger_partition_header *header;
	charger_partition_info_2 *info;
	void *buffer;
	int ret;

	pr_info("spring_charger: passive startup begin\n");
	charger_partition->part_info_part_number = 5;
	if (!look_up_scsi_device(0) || !check_device_is_correct() ||
	    !get_charger_partition_info()) {
		pr_err("spring_charger: charger partition unavailable\n");
		return;
	}

	buffer = kzalloc(CHARGER_PARTITION_RWSIZE, GFP_KERNEL);
	if (!buffer)
		return;
	header = buffer;
	ret = charger_scsi_read_partition(charger_partition->sdev, buffer,
					  part_info.part_start, 1);
	if (ret || header->magic != CHARGER_PARTITION_MAGIC || !header->initialized) {
		pr_err("spring_charger: header unavailable (%d)\n", ret);
		goto out;
	}

	ret = charger_scsi_read_partition(charger_partition->sdev, buffer,
					  part_info.part_start + CHARGER_PARTITION_INFO_2, 1);
	if (ret) {
		pr_err("spring_charger: EU record read failed (%d)\n", ret);
		goto out;
	}
	info = buffer;
	WRITE_ONCE(eu_mode, (int)info->eu_model);
	WRITE_ONCE(chg_parti_is_rdy, true);
	pr_info("spring_charger: passive startup ready, eu_mode=%d\n", eu_mode);
out:
	kfree(buffer);
}

int charger_partition_set_prop(int type, int val)
{
    int rc = 0;
    charger_partition_info_2 info_2 = {.eu_model = 0, .test = 0, .reserved = 0};

	if(!chg_parti_is_rdy) {
		pr_err("[charger] charger partition not rdy, can't set prop!");
		return -EINVAL;
	}

	rc = charger_partition_alloc(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_2, sizeof(charger_partition_info_2));
	if(rc < 0) {
		pr_err("[charger] %s failed to alloc\n", __func__);
		return -EINVAL;
	}

    pr_err("[charger] %s: set %d to %u \n", __func__, type, val);

    switch (type)
    {
    case CHARGER_PARTITION_PROP_TEST_MODE:
        info_2.test = val;
        break;
    case CHARGER_PARTITION_PROP_EU_MODE:
        info_2.eu_model = val;
        break;
    default:
        pr_err("[charger] %s unsupport handle type!\n", __func__);
        break;
    }

	rc = charger_partition_write(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_2, (void *)&info_2, sizeof(charger_partition_info_2));
	if(rc < 0) {
		pr_err("[charger] %s failed to write\n", __func__);
		rc = charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_2, sizeof(charger_partition_info_2));
		if(rc < 0) {
			pr_err("[charger] %s failed to dealloc\n", __func__);
			return -EINVAL;
		}
		return -EINVAL;
	}

	pr_info("[charger] %s last_eu_mode:%d, current_eu_mode:%d \n", __func__, eu_mode, info_2.eu_model);
	if(eu_mode != info_2.eu_model){
		pr_err("[charger] %s eu_mode is changed, update it to charge driver \n", __func__);
		schedule_delayed_work(&charger_partition->charger_partition_work, msecs_to_jiffies(100));//100ms
		eu_mode = info_2.eu_model;
	}else{
		cancel_delayed_work_sync(&charger_partition->charger_partition_work);
	if (charger_partition->sdev)
		scsi_device_put(charger_partition->sdev);
	}

	rc = charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_2, sizeof(charger_partition_info_2));
	if(rc < 0) {
		pr_err("[charger] %s failed to dealloc\n", __func__);
		return -EINVAL;
	}

    return 0;
}
EXPORT_SYMBOL(charger_partition_set_prop);

int charger_partition_get_prop(int type, int *val)
{
	int rc = 0;
	charger_partition_info_2 *info_2 = NULL;

	if(!chg_parti_is_rdy) {
		pr_err("[charger] charger partition not rdy, can't get prop!");
		return -EINVAL;
	}

	rc = charger_partition_alloc(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_2, sizeof(charger_partition_info_2));
	if(rc < 0) {
		pr_err("[charger] %s failed to alloc\n", __func__);
		return -EINVAL;
	}

	info_2 = (charger_partition_info_2 *)charger_partition_read(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_2, sizeof(charger_partition_info_2));
	if(!info_2) {
		pr_err("[charger] %s failed to read\n", __func__);
		rc = charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_2, sizeof(charger_partition_info_2));
		if(rc < 0) {
			pr_err("[charger] %s failed to dealloc\n", __func__);
			return -EINVAL;
		}
		return -EINVAL;
	}

    switch (type)
    {
    case CHARGER_PARTITION_PROP_TEST_MODE:
        *val = (int)info_2->test;
        break;
    case CHARGER_PARTITION_PROP_EU_MODE:
        *val = (int)info_2->eu_model;
        break;
    default:
        pr_err("[charger] %s unsupport handle type!\n", __func__);
        break;
    }

    pr_err("[charger] %s: %d is %d \n", __func__, type, *val);

	rc = charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL, CHARGER_PARTITION_INFO_2, sizeof(charger_partition_info_2));
	if(rc < 0) {
		pr_err("[charger] %s failed to dealloc\n", __func__);
		return -EINVAL;
	}

    return 0;
}
EXPORT_SYMBOL(charger_partition_get_prop);

int get_eu_mode(void)
{
	pr_debug("%s: eu_mode:%d !", __func__, eu_mode);
	return eu_mode;
}
EXPORT_SYMBOL(get_eu_mode);

/* Vendor Japan-mode ABI recovered from the supplied reference module:
 * set_jp_mode stores its int argument; get_jp_mode returns the same global.
 * Neither function performs partition I/O. */
int jp_mode;
void set_jp_mode(int val)
{
    pr_info("%s: jp_mode:%d !", __func__, val);
    jp_mode = val;
}
EXPORT_SYMBOL(set_jp_mode);

int get_jp_mode(void)
{
    pr_info("%s: jp_mode:%d !", __func__, jp_mode);
    return jp_mode;
}
EXPORT_SYMBOL(get_jp_mode);

static int __init charger_partition_init(void)
{
	charger_partition = (struct ChargerPartition *)kzalloc(sizeof(struct ChargerPartition), GFP_KERNEL);
	if (!charger_partition)
		return -ENOMEM;
	pr_err("%s:init!", __func__);
	INIT_DELAYED_WORK(&charger_partition->charger_partition_work, charger_partition_work);
	schedule_delayed_work(&charger_partition->charger_partition_work, msecs_to_jiffies(CHARGER_WORK_DELAY_MS));
	return 0;
}
device_initcall(charger_partition_init);

static void __exit charger_partition_exit(void)
{
	chg_parti_is_rdy = false;
	cancel_delayed_work_sync(&charger_partition->charger_partition_work);
	if (charger_partition->sdev)
		scsi_device_put(charger_partition->sdev);
	kfree(charger_partition);

	pr_err("%s:exit!", __func__);
}
module_exit(charger_partition_exit);

MODULE_LICENSE("GPL");
