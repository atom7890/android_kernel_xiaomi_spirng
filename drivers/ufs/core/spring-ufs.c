// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "spring-ufs.h"
#include "ufshcd-priv.h"

DECLARE_COMPLETION(ufs_xiaomi_comp);
EXPORT_SYMBOL(ufs_xiaomi_comp);

int mi_ufshcd_query_descriptor_retry(struct ufs_hba *hba,
				    enum query_opcode opcode,
				    enum desc_idn idn, u8 index,
				    u8 selector, u8 *desc_buf, int *buf_len)
{
	return ufshcd_query_descriptor_retry(hba, opcode, idn, index,
					     selector, desc_buf, buf_len);
}
EXPORT_SYMBOL_GPL(mi_ufshcd_query_descriptor_retry);

int mi_ufshcd_read_desc_param(struct ufs_hba *hba, enum desc_idn desc_id,
			      int desc_index, u8 param_offset,
			      u8 *param_read_buf, u8 param_size)
{
	return ufshcd_read_desc_param(hba, desc_id, desc_index, param_offset,
				      param_read_buf, param_size);
}
EXPORT_SYMBOL_GPL(mi_ufshcd_read_desc_param);

static DEFINE_SPINLOCK(ufs_data_lock);
static struct UFS_DATA ufs_data;

struct UFS_DATA *get_ufs_data(void)
{
	return &ufs_data;
}
EXPORT_SYMBOL(get_ufs_data);

void spring_ufs_record_error(const char *reason)
{
	char (*history)[32] = ufs_data.ufs_err_state.err_reason;
	unsigned long flags;

	spin_lock_irqsave(&ufs_data_lock, flags);
	ufs_data.ufs_err_state.err_occurred++;
	memmove(history + 1, history, sizeof(ufs_data.ufs_err_state.err_reason) -
		sizeof(*history));
	strscpy(history[0], reason, sizeof(*history));
	spin_unlock_irqrestore(&ufs_data_lock, flags);
}

static struct spring_ufs_uic_stats uic_stats;

void spring_ufs_count_uic_error(u32 event, u32 status)
{
	unsigned long flags;
	unsigned int bit;

	if (event != UFS_EVT_PA_ERR && event != UFS_EVT_DL_ERR &&
	    event != UFS_EVT_DME_ERR)
		return;

	spin_lock_irqsave(&ufs_data_lock, flags);
	switch (event) {
	case UFS_EVT_PA_ERR:
		uic_stats.pa_err_cnt_total++;
		for (bit = 0; bit < ARRAY_SIZE(uic_stats.pa_err_cnt); bit++)
			if (status & BIT(bit))
				uic_stats.pa_err_cnt[bit]++;
		break;
	case UFS_EVT_DL_ERR:
		uic_stats.dl_err_cnt_total++;
		for (bit = 0; bit < ARRAY_SIZE(uic_stats.dl_err_cnt); bit++)
			if (status & BIT(bit))
				uic_stats.dl_err_cnt[bit]++;
		break;
	case UFS_EVT_DME_ERR:
		uic_stats.dme_err_cnt++;
		break;
	}
	spin_unlock_irqrestore(&ufs_data_lock, flags);
}

void spring_ufs_get_uic_stats(struct spring_ufs_uic_stats *stats)
{
	unsigned long flags;

	spin_lock_irqsave(&ufs_data_lock, flags);
	*stats = uic_stats;
	spin_unlock_irqrestore(&ufs_data_lock, flags);
}
EXPORT_SYMBOL_GPL(spring_ufs_get_uic_stats);

MODULE_LICENSE("GPL");
