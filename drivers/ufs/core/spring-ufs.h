/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SPRING_UFS_H
#define _SPRING_UFS_H

#include <linux/completion.h>
#include <ufs/ufshcd.h>

extern struct completion ufs_xiaomi_comp;

struct UFS_DATA {
	struct {
		s64 err_occurred;
		char err_reason[10][32];
	} ufs_err_state;
};

struct spring_ufs_uic_stats {
	u32 pa_err_cnt_total;
	u32 pa_err_cnt[5];
	u32 dl_err_cnt_total;
	u32 dl_err_cnt[15];
	u32 dme_err_cnt;
};

void spring_ufs_count_uic_error(u32 event, u32 status);
void spring_ufs_get_uic_stats(struct spring_ufs_uic_stats *stats);
struct UFS_DATA *get_ufs_data(void);
void spring_ufs_record_error(const char *reason);

int mi_ufshcd_query_descriptor_retry(struct ufs_hba *hba,
				    enum query_opcode opcode,
				    enum desc_idn idn, u8 index,
				    u8 selector, u8 *desc_buf, int *buf_len);
int mi_ufshcd_read_desc_param(struct ufs_hba *hba, enum desc_idn desc_id,
			      int desc_index, u8 param_offset,
			      u8 *param_read_buf, u8 param_size);

#endif
