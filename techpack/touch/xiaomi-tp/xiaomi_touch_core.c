#include <linux/kref.h>
#include <linux/mm.h>
/*
 * Copyright (c) 2023 Xiaomi, Inc.
 * All Rights Reserved.
 * Confidential and Proprietary - Xiaomi, Inc.
 */
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/stdarg.h>
#include <linux/power_supply.h>
#include <net/sock.h>
#include <net/netlink.h>

#include "xiaomi_touch.h"
DEFINE_SRCU(spring_touch_readers);
static DEFINE_MUTEX(spring_panel_lifecycle_lock);
static bool spring_panel_closing[MAX_TOUCH_PANEL_COUNT];
static bool spring_panel_retired[MAX_TOUCH_PANEL_COUNT];
static xiaomi_touch_t xiaomi_touch;

xiaomi_touch_data_t *spring_touch_client_data(s8 touch_id)
{
	SPRING_TOUCH_READER_GUARD;
 if (IS_TOUCH_ID_INVALID(touch_id))
  return NULL;
 return &xiaomi_touch.xiaomi_touch_data[touch_id];
}

static struct task_struct *xiaomi_touch_temp_thread[MAX_TOUCH_PANEL_COUNT];

#define NETLINK_TEST 24
#define MAX_MSGSIZE 16
int stringlength(char *s);
static u32 pid;
struct sock *nl_sk;

void sendnlmsg(char message)
{
	SPRING_TOUCH_READER_GUARD;
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	u32 portid = READ_ONCE(pid);

	if (!nl_sk || !portid)
		return;

	skb = nlmsg_new(MAX_MSGSIZE, GFP_KERNEL);
	if (!skb)
		return;

	nlh = nlmsg_put(skb, 0, 0, 0, MAX_MSGSIZE, 0);
	if (!nlh) {
		kfree_skb(skb);
		return;
	}

	memset(nlmsg_data(nlh), 0, MAX_MSGSIZE);
	((char *)nlmsg_data(nlh))[0] = message;
	NETLINK_CB(skb).portid = 0;
	NETLINK_CB(skb).dst_group = 0;
	netlink_unicast(nl_sk, skb, portid, MSG_DONTWAIT);
}

void nl_data_ready(struct sk_buff *skb)
{
	SPRING_TOUCH_READER_GUARD;
	struct nlmsghdr *nlh;

	if (!skb || skb->len < NLMSG_HDRLEN)
		return;

	nlh = nlmsg_hdr(skb);
	if (!nlmsg_ok(nlh, skb->len))
		return;

	WRITE_ONCE(pid, NETLINK_CB(skb).portid);
}

int netlink_init(void)
{
	SPRING_TOUCH_READER_GUARD;
	struct netlink_kernel_cfg netlink_cfg;
	memset(&netlink_cfg, 0, sizeof(struct netlink_kernel_cfg));
	netlink_cfg.groups = 0;
	netlink_cfg.flags = 0;
	netlink_cfg.input = nl_data_ready;
	netlink_cfg.cb_mutex = NULL;
	nl_sk = netlink_kernel_create(&init_net, NETLINK_TEST, &netlink_cfg);

	if (!nl_sk) {
		LOG_ERROR("create netlink socket error\n");
		return 1;
	}

	return 0;
}

void netlink_exit(void)
{
	SPRING_TOUCH_READER_GUARD;
	if (nl_sk != NULL) {
		netlink_kernel_release(nl_sk);
		nl_sk = NULL;
	}

	LOG_INFO("self module exited\n");
}


/* Owner reference plus one reference per userspace VMA. */
struct spring_touch_allocation {
	struct list_head node;
	struct kref refs;
	struct rcu_head retire;
	void *base;
	size_t size;
};

static LIST_HEAD(spring_touch_allocations);
static DEFINE_MUTEX(spring_touch_allocation_lock);

static void spring_touch_allocation_release(struct kref *ref)
{
	SPRING_TOUCH_READER_GUARD;
	struct spring_touch_allocation *allocation =
		container_of(ref, struct spring_touch_allocation, refs);

	kfree(allocation->base);
	kfree(allocation);
}

void *kzalloc_retry(size_t size, int retry)
{
	SPRING_TOUCH_READER_GUARD;
	struct spring_touch_allocation *allocation;
	void *base = NULL;

	if (!size || retry <= 0)
		return NULL;

	allocation = kzalloc(sizeof(*allocation), GFP_KERNEL);
	if (!allocation)
		return NULL;

	while (retry-- > 0) {
		base = kzalloc(size, GFP_KERNEL);
		if (base)
			break;
	}
	if (!base) {
		kfree(allocation);
		return NULL;
	}

	allocation->base = base;
	allocation->size = size;
	kref_init(&allocation->refs);
	mutex_lock(&spring_touch_allocation_lock);
	list_add_tail(&allocation->node, &spring_touch_allocations);
	mutex_unlock(&spring_touch_allocation_lock);
	return base;
}

static void spring_touch_retire_allocation(struct rcu_head *head)
{
 struct spring_touch_allocation *allocation =
  container_of(head, struct spring_touch_allocation, retire);
 kref_put(&allocation->refs, spring_touch_allocation_release);
}

void kzalloc_free(void *base)
{
	SPRING_TOUCH_READER_GUARD;
	struct spring_touch_allocation *allocation;

	if (!base)
		return;

	mutex_lock(&spring_touch_allocation_lock);
	list_for_each_entry(allocation, &spring_touch_allocations, node) {
		if (allocation->base != base)
			continue;
		list_del(&allocation->node);
		mutex_unlock(&spring_touch_allocation_lock);
		call_srcu(&spring_touch_readers, &allocation->retire,
			  spring_touch_retire_allocation);
		return;
	}
	mutex_unlock(&spring_touch_allocation_lock);

	/* An unknown pointer must not bypass the reference accounting. */
	WARN_ON_ONCE(1);
}

static void spring_touch_vma_open(struct vm_area_struct *vma)
{
	SPRING_TOUCH_READER_GUARD;
	struct spring_touch_allocation *allocation = vma->vm_private_data;

	kref_get(&allocation->refs);
}

static void spring_touch_vma_close(struct vm_area_struct *vma)
{
	SPRING_TOUCH_READER_GUARD;
	struct spring_touch_allocation *allocation = vma->vm_private_data;

	kref_put(&allocation->refs, spring_touch_allocation_release);
}

static const struct vm_operations_struct spring_touch_vm_ops = {
	.open = spring_touch_vma_open,
	.close = spring_touch_vma_close,
};

int spring_touch_map_allocation(struct vm_area_struct *vma, dma_addr_t phys)
{
	SPRING_TOUCH_READER_GUARD;
	struct spring_touch_allocation *allocation;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset;
	int ret = -ENODEV;

	mutex_lock(&spring_touch_allocation_lock);
	list_for_each_entry(allocation, &spring_touch_allocations, node) {
		if (virt_to_phys(allocation->base) != phys)
			continue;
		if (!IS_ALIGNED((unsigned long)allocation->base, PAGE_SIZE) ||
		    !IS_ALIGNED(allocation->size, PAGE_SIZE)) {
			ret = -EINVAL;
			break;
		}
		if (vma->vm_pgoff >= (allocation->size >> PAGE_SHIFT)) {
			ret = -EINVAL;
			break;
		}
		offset = vma->vm_pgoff << PAGE_SHIFT;
		if (!size || size > allocation->size - offset) {
			ret = -EINVAL;
			break;
		}

		ret = remap_pfn_range(vma, vma->vm_start,
				     (phys >> PAGE_SHIFT) + vma->vm_pgoff,
				     size, vma->vm_page_prot);
		if (!ret) {
			kref_get(&allocation->refs);
			vma->vm_private_data = allocation;
			vma->vm_ops = &spring_touch_vm_ops;
		}
		break;
	}
	mutex_unlock(&spring_touch_allocation_lock);
	return ret;
}


void *kvzalloc_retry(size_t size, int retry)
{
	SPRING_TOUCH_READER_GUARD;
	void *p = NULL;

	while (retry > 0) {
		retry--;
		p = kvzalloc(size, GFP_KERNEL);
		if (p)
			return p;
	}
	return NULL;
}

void kvzalloc_free(void *p)
{
	SPRING_TOUCH_READER_GUARD;
	kvfree(p);
}

xiaomi_touch_data_t *get_xiaomi_touch_data(s8 touch_id)
{
	SPRING_TOUCH_READER_GUARD;
	if (IS_TOUCH_ID_INVALID(touch_id) || READ_ONCE(spring_panel_closing[touch_id])) {
		LOG_ERROR("touch id %d hasn't select, return!", touch_id);
		return NULL;
	} else if (!(xiaomi_touch.panel_register_mask & (1 << touch_id))) {
		LOG_ERROR("panel in touch id %d hasn't register, return!", touch_id);
		return NULL;
	}
	return &xiaomi_touch.xiaomi_touch_data[touch_id];
}

xiaomi_touch_driver_param_t *get_xiaomi_touch_driver_param(s8 touch_id)
{
	SPRING_TOUCH_READER_GUARD;
	if (IS_TOUCH_ID_INVALID(touch_id) || READ_ONCE(spring_panel_closing[touch_id])) {
		LOG_ERROR("touch id %d hasn't select, return!", touch_id);
		return NULL;
	} else if (!(xiaomi_touch.panel_register_mask & (1 << touch_id))) {
		LOG_ERROR("panel in touch id %d hasn't register, return!", touch_id);
		return NULL;
	}
	return &xiaomi_touch.xiaomi_touch_driver_param[touch_id];
}

void notify_xiaomi_touch(xiaomi_touch_data_t *xiaomi_touch_data, enum poll_notify_type type)
{
	SPRING_TOUCH_READER_GUARD;
	private_data_t *client_private_data = NULL;
	if (!xiaomi_touch_data)
		return;
	spin_lock(&xiaomi_touch_data->private_data_lock);
	list_for_each_entry_rcu(client_private_data, &xiaomi_touch_data->private_data_list, node) {
		LOG_VERBOSE("notify xiaomi-touch data update, client private data is %p", client_private_data);
		wake_up_all(&client_private_data->poll_wait_queue_head);
		if (type == COMMON_DATA_NOTIFY) {
			wake_up_all(&client_private_data->poll_wait_queue_head_for_cmd);
		} else if (type == FRAME_DATA_NOTIFY) {
			wake_up_all(&client_private_data->poll_wait_queue_head_for_frame);
		} else if (type == RAW_DATA_NOTIFY) {
			wake_up_all(&client_private_data->poll_wait_queue_head_for_raw);
		}
	}
	spin_unlock(&xiaomi_touch_data->private_data_lock);
}

void add_common_data_to_buf(s8 touch_id, enum common_data_cmd cmd, enum common_data_mode mode, int length, int *data)
{
	SPRING_TOUCH_READER_GUARD;
	xiaomi_touch_data_t *touch = get_xiaomi_touch_data(touch_id);
	common_data_t *entry;
	int index;

	if (!touch || length < 0 ||
	    length > ARRAY_SIZE(((common_data_t *)0)->data_buf) ||
	    (length && !data))
		return;

	mutex_lock(&touch->common_data_buf_lock);
	index = atomic_read(&touch->common_data_buf_index);
	if (index < 0 || index >= COMMON_DATA_BUF_SIZE)
		index = 0;

	entry = &touch->common_data_buf[index];
	memset(entry, 0, sizeof(*entry));
	entry->touch_id = touch_id;
	entry->cmd = cmd;
	entry->mode = mode;
	entry->data_len = length;
	if (length)
		memcpy(entry->data_buf, data, length * sizeof(entry->data_buf[0]));

	atomic_set(&touch->common_data_buf_index,
		   (index + 1) % COMMON_DATA_BUF_SIZE);
	mutex_unlock(&touch->common_data_buf_lock);
	notify_xiaomi_touch(touch, COMMON_DATA_NOTIFY);
}
EXPORT_SYMBOL_GPL(add_common_data_to_buf);


void *get_raw_data_base(s8 touch_id)
{
	SPRING_TOUCH_READER_GUARD;
	xiaomi_touch_data_t *xiaomi_touch_data = get_xiaomi_touch_data(touch_id);
	void *base = NULL;

	if (!xiaomi_touch_data)
		return NULL;

	if (!xiaomi_touch_data->frame_data_mmap_base) {
		LOG_ERROR("touch id %d copy data failed, xiaomi_touch_data %p, base %p",
			touch_id, xiaomi_touch_data, xiaomi_touch_data->frame_data_mmap_base);
		return NULL;
	}

	base = xiaomi_touch_data->frame_data_mmap_base +
		atomic_read(&xiaomi_touch_data->frame_data_buf_index) * xiaomi_touch_data->frame_data_size;
	return base;
}
EXPORT_SYMBOL_GPL(get_raw_data_base);

void notify_raw_data_update(s8 touch_id)
{
	SPRING_TOUCH_READER_GUARD;
	xiaomi_touch_data_t *xiaomi_touch_data = get_xiaomi_touch_data(touch_id);

	if (!xiaomi_touch_data)
		return;

	atomic_inc(&xiaomi_touch_data->frame_data_buf_index);
	if (atomic_read(&xiaomi_touch_data->frame_data_buf_index) >= xiaomi_touch_data->frame_data_buf_size)
		atomic_set(&xiaomi_touch_data->frame_data_buf_index, 0);


	notify_xiaomi_touch(xiaomi_touch_data, FRAME_DATA_NOTIFY);
}
EXPORT_SYMBOL_GPL(notify_raw_data_update);
int get_bms_temp_common(void)
{
	SPRING_TOUCH_READER_GUARD;
	struct power_supply *battery;
	union power_supply_propval prop;
	int ret;

	battery = power_supply_get_by_name("battery");
	if (!battery) {
		battery = power_supply_get_by_name("bms");
		if (!battery) {
			LOG_INFO("can't find bms and battery");
			return -INVAILD_TEMPERATURE;
		}
	}

	ret = power_supply_get_property(battery, POWER_SUPPLY_PROP_TEMP, &prop);
	if (ret) {
		LOG_INFO("can't read battery temp");
		return -INVAILD_TEMPERATURE;
	}

	return prop.intval;
}
EXPORT_SYMBOL_GPL(get_bms_temp_common);

/*
 * Temperature distribution strategy:
 *   a. Only launch when screen is on
 *   b. Update temperature for firmware at the moment the screen lights up,
 *     and then, update temperature for firmware every 2 degrees of subsequent temperature change
 *
 * Notes:
 *   VIRTUAL-SENSOR, Case temperature fitting, is a parameter that can well reflect the overall
 * temperature change of the mobile phone in the range of 25℃ ~ 53℃. It can be obtained through
 * monitoring node /sys/class/thermal/thermal_message/board_sensor_temp or through program
 * usb_get_property(USB_PROP_BOARD_TEMP, &board_temp).
 *   However, temperature changes in low-temperature scenarios require more attention for goodix.
 * And VIRTUAL-SENSOR cannot guarantee the detection accuracy below 25℃.
 *   Therefore, the monitoring object is changed to battery temperature.
 */
static int xiaomi_touch_temp_thread_func(void *data)
{
	s8 touch_id = (long)data;
	int temp_n = 0;
	int last_temp = 1000;
	int cur_temp0, cur_temp = 0;
	xiaomi_touch_data_t *xiaomi_touch_data = get_xiaomi_touch_data(touch_id);
	xiaomi_touch_driver_param_t *xiaomi_touch_driver_param = get_xiaomi_touch_driver_param(touch_id);

	LOG_INFO("enter");
	if (!xiaomi_touch_data || !xiaomi_touch_driver_param ||
			!xiaomi_touch_driver_param->hardware_operation.set_thermal_temp)
		return -1;

	while (!kthread_should_stop()) {
		wait_event_interruptible(xiaomi_touch_data->temp_detect_wait_queue,
				atomic_read(&xiaomi_touch_data->temp_detect_ready) ||
				kthread_should_stop());
		if (kthread_should_stop())
			break;
		if(atomic_read(&xiaomi_touch_data->temp_detect_ready)) {
			cur_temp0 = get_bms_temp_common();
			cur_temp = (cur_temp0 + 5) / 10; // Rounding, in degrees Celsius
			if (abs(cur_temp0) < INVAILD_TEMPERATURE &&
					xiaomi_touch_driver_param->hardware_operation.set_thermal_temp &&
					abs(cur_temp - last_temp) >= xiaomi_touch_driver_param->hardware_param.temp_change_value) {
				xiaomi_touch_driver_param->hardware_operation.set_thermal_temp(cur_temp, false);
				add_common_data_to_buf(touch_id, SET_CUR_VALUE, DATA_MODE_145, 1, &cur_temp);
				last_temp = cur_temp;
			}

			/*
			 * The update rules for battery temperature are as follows:
			 * a. updates every 5s when charging; updates every 30s when not charging
			 * b. if the system enters sleep mode when screen-off, the temperature will not be updated until the system is awakened.
			 */
			if (xiaomi_touch.charging_status)
				temp_n = 5;
			else
				temp_n = 10; // 30;

			LOG_DEBUG("cur_temp0:%d, last_temp:%d, sleep %dms", cur_temp0, last_temp, temp_n * 1000);
			msleep(temp_n * 1000);
		}
	}
	LOG_INFO("exit");

	return 0;
}

void enable_temperature_detection_func(bool is_resume)
{
	SPRING_TOUCH_READER_GUARD;
	xiaomi_touch_data_t *xiaomi_touch_data = get_xiaomi_touch_data(0);
	xiaomi_touch_driver_param_t *xiaomi_touch_driver_param = get_xiaomi_touch_driver_param(0);

	LOG_VERBOSE("enter");
	if (!xiaomi_touch_data || !xiaomi_touch_driver_param ||
			!xiaomi_touch_driver_param->hardware_operation.set_thermal_temp)
		return;

	if (is_resume) {
		atomic_set(&xiaomi_touch_data->temp_detect_ready, 1);
		LOG_DEBUG("start detect temperature");
	} else {
		atomic_set(&xiaomi_touch_data->temp_detect_ready, 0);
		LOG_DEBUG("stop detect temperature");
	}

	wake_up_interruptible(&xiaomi_touch_data->temp_detect_wait_queue);
}
EXPORT_SYMBOL_GPL(enable_temperature_detection_func);

#if defined(TOUCH_PLATFORM_XRING)
#ifdef TOUCH_SENSORHUB_SUPPORT
struct delayed_work xiaomi_touch_ipc_work;
static void xiaomi_touch_ipc_handler(struct work_struct *work)
{
	SPRING_TOUCH_READER_GUARD;
	xiaomi_notify_sensorhub_enable(0, false, true);
	return;
}

static int xiaomi_touch_ipc_callback(struct ipc_pkt *pkt, u16 tag, u8 cmd, void *arg)
{
	SPRING_TOUCH_READER_GUARD;
	if (cmd == SYNA_IPC_SOS) {
		LOG_WARNING("Sensorhub status abnormal, needind rescue!");
		schedule_delayed_work(&xiaomi_touch_ipc_work, msecs_to_jiffies(0));
	} else
		LOG_ERROR("Unsupported ipc cmd %x\n", cmd);

	return 0;
}

static void xiaomi_touch_register_ipc_notifier(void)
{
	SPRING_TOUCH_READER_GUARD;
	struct ipc_notifier_info info;
	info.callback = xiaomi_touch_ipc_callback;
	info.arg = NULL;

	if (sh_register_recv_notifier(IPC_VC_AO_NS_SH_AP_TP, TAG_SH_CONNECTIVITY, &info))
		LOG_ERROR("register ipc notifier failed!\n");

	INIT_DELAYED_WORK(&xiaomi_touch_ipc_work, xiaomi_touch_ipc_handler);

	return;
}
#endif
#endif

static int spring_register_touch_panel_impl(struct device *dev, s8 touch_id, hardware_param_t *hardware_param, hardware_operation_t *hardware_operation)
{
	xiaomi_touch_driver_param_t *xiaomi_touch_driver_param = NULL;
	xiaomi_touch_data_t *xiaomi_touch_data = NULL;
	u32 alloc_size = 0;
	int ret = -ENOMEM;

	if (!dev || !hardware_param)
		return -EINVAL;

	LOG_INFO("enter");
	if (IS_TOUCH_ID_INVALID(touch_id)) {
		LOG_ERROR("touch id error, please check it!");
		return -1;
	}

	if (xiaomi_touch.panel_register_mask & (1 << touch_id)) {
		LOG_ERROR("touch id %d has register, stop register again!", touch_id);
		return -1;
	}

	if (spring_panel_retired[touch_id])
		return -EBUSY;

	/* init panel driver param */
	if (hardware_param) {
		xiaomi_touch_driver_param = &xiaomi_touch.xiaomi_touch_driver_param[touch_id];
		xiaomi_touch_driver_param->touch_id = touch_id;
		memcpy(&xiaomi_touch_driver_param->hardware_param, hardware_param, sizeof(hardware_param_t));
	} else {
		LOG_ERROR("param is null");
		return -1;
	}
	if (hardware_operation)
		memcpy(&xiaomi_touch_driver_param->hardware_operation, hardware_operation, sizeof(hardware_operation_t));


	/* init xiaomi touch driver param */
	xiaomi_touch_data = &xiaomi_touch.xiaomi_touch_data[touch_id];
	memset(xiaomi_touch_data, 0, sizeof(xiaomi_touch_data_t));
	xiaomi_touch_data->touch_id = touch_id;
	spin_lock_init(&xiaomi_touch_data->private_data_lock);
	mutex_init(&xiaomi_touch_data->common_data_buf_lock);
	xiaomi_touch_data->frame_data_size = hardware_param->frame_data_page_size * PAGE_SIZE;
	xiaomi_touch_data->frame_data_buf_size = hardware_param->frame_data_buf_size;
	xiaomi_touch_data->raw_data_size = hardware_param->raw_data_page_size * PAGE_SIZE;
	xiaomi_touch_data->raw_data_buf_size = hardware_param->raw_data_buf_size;

	/* The temperature detection function is enabled by default when machine startup */
	init_waitqueue_head(&xiaomi_touch_data->temp_detect_wait_queue);
	atomic_set(&xiaomi_touch_data->temp_detect_ready, 1);

	/* alloc mmap memory */
	xiaomi_touch_data->frame_data_mmap_phy_base = 0;
	alloc_size = xiaomi_touch_data->frame_data_size * xiaomi_touch_data->frame_data_buf_size;
	LOG_DEBUG("alloc size = %d, frame data size %d, frame data page size %d, frame data buf size %d",
		alloc_size, xiaomi_touch_data->frame_data_size,
		hardware_param->frame_data_page_size, hardware_param->frame_data_buf_size);
	if (alloc_size) {
		xiaomi_touch_data->frame_data_mmap_base = kzalloc_retry(alloc_size, 3);
		if (!xiaomi_touch_data->frame_data_mmap_base) {
			LOG_ERROR("touch id %d alloc frame data memory failed!", touch_id);
			goto spring_alloc_fail;
		}
		LOG_DEBUG("frame data base %p, size %u", xiaomi_touch_data->frame_data_mmap_base, alloc_size);
		xiaomi_touch_data->frame_data_mmap_phy_base = virt_to_phys(xiaomi_touch_data->frame_data_mmap_base);
	}

	xiaomi_touch_data->raw_data_mmap_phy_base = 0;
	alloc_size = xiaomi_touch_data->raw_data_size * xiaomi_touch_data->raw_data_buf_size;

	LOG_DEBUG("alloc size = %d, raw data size %d, raw data page size %d, raw data buf size %d",
		alloc_size, xiaomi_touch_data->raw_data_size,
		hardware_param->raw_data_page_size, hardware_param->raw_data_buf_size);
	if (alloc_size) {
		xiaomi_touch_data->raw_data_mmap_base = kzalloc_retry(alloc_size, 3);
		if (!xiaomi_touch_data->raw_data_mmap_base) {
			LOG_ERROR("touch id %d alloc raw data memory failed!", touch_id);
			goto spring_alloc_fail;
		}
		LOG_DEBUG("raw data base %p, size %u", xiaomi_touch_data->raw_data_mmap_base, alloc_size);
		xiaomi_touch_data->raw_data_mmap_phy_base = virt_to_phys(xiaomi_touch_data->raw_data_mmap_base);
	}

	xiaomi_touch_data->event_wq = alloc_workqueue("xiaomi-touch-event-queue",
		WQ_UNBOUND | WQ_HIGHPRI | WQ_CPU_INTENSIVE, 1);
	if (!xiaomi_touch_data->event_wq) {
		LOG_ERROR("ERROR: Cannot create work thread");
		goto spring_alloc_fail;
	}

	/* alloc poll data memory */
	xiaomi_touch_data->poll_data = kzalloc_retry(sizeof(htc_ic_polldata_t), 3);
	if (!xiaomi_touch_data->poll_data) {
		LOG_ERROR("alloc poll data memory is failed!");
		goto spring_alloc_fail;
	}

	/* init list */
	INIT_LIST_HEAD(&xiaomi_touch_data->private_data_list);

	/* print hardware param*/
	if (hardware_operation && hardware_param) {

		if (hardware_operation->ic_get_lockdown_info) {
			hardware_operation->ic_get_lockdown_info(hardware_param->lockdown_info);
		}
		LOG_INFO("x_resolution=%d, y_resolution=%d, rx_num=%d, tx_num=%d, "
			"super_resolution_factor=%d, "
			"frame_data_page_size=%d, frame_data_buf_size=%d, "
			"raw_data_page_size=%d, raw_data_buf_size=%d, "
			"config_file_name=%.64s, driver_version=%.64s, fw_version=%.64s, "
			"lockdown={0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}",
			hardware_param->x_resolution, hardware_param->y_resolution, hardware_param->rx_num, hardware_param->tx_num,
			hardware_param->super_resolution_factor,
			hardware_param->frame_data_page_size, hardware_param->frame_data_buf_size,
			hardware_param->raw_data_page_size, hardware_param->raw_data_buf_size,
			hardware_param->config_file_name, hardware_param->driver_version, hardware_param->fw_version,
			hardware_param->lockdown_info[0], hardware_param->lockdown_info[1], hardware_param->lockdown_info[2], hardware_param->lockdown_info[3],
			hardware_param->lockdown_info[4], hardware_param->lockdown_info[5], hardware_param->lockdown_info[6], hardware_param->lockdown_info[7]);
	}
	/* init touch mode */
	ret = xiaomi_touch_init_touch_mode(touch_id, dev);
	if (ret)
		goto spring_alloc_fail;

	xiaomi_touch_create_proc(xiaomi_touch_driver_param);

	/* complete register */
	xiaomi_touch.panel_register_mask |= (1 << touch_id);
	LOG_INFO("current panel_register_mask is %d", xiaomi_touch.panel_register_mask);

	/* create a thread for temp detect */
	if (hardware_operation && hardware_operation->set_thermal_temp) {
		LOG_INFO("startup temperature detect thread");
		xiaomi_touch_temp_thread[touch_id] = kthread_create(xiaomi_touch_temp_thread_func, (void *)(long)touch_id, "xiaomi_touch_temp_thread");
		if (IS_ERR(xiaomi_touch_temp_thread[touch_id])) {
			LOG_ERROR("Failed to create xiaomi_touch_temp_thread[touch_id]");
			goto err_out;
		}
		wake_up_process(xiaomi_touch_temp_thread[touch_id]);
	}

#if defined(TOUCH_PLATFORM_XRING)
#ifdef TOUCH_SENSORHUB_SUPPORT
/* NOTE:
 * This process is used for disabling sensorhub when it running abnormally.
 * For example, receiving normal touch frames.
 */
	xiaomi_touch_register_ipc_notifier();
#endif
#endif
	return 0;

err_out:
	/* Temperature monitoring failure is nonfatal to touch registration. */
	xiaomi_touch_temp_thread[touch_id] = NULL;

	return 0;

spring_alloc_fail:
	spring_touch_cancel_mode_work(touch_id);
	if (xiaomi_touch_data->event_wq) {
		destroy_workqueue(xiaomi_touch_data->event_wq);
		xiaomi_touch_data->event_wq = NULL;
	}
	kzalloc_free(xiaomi_touch_data->poll_data);
	kzalloc_free(xiaomi_touch_data->raw_data_mmap_base);
	kzalloc_free(xiaomi_touch_data->frame_data_mmap_base);
	memset(xiaomi_touch_data, 0, sizeof(*xiaomi_touch_data));
	memset(xiaomi_touch_driver_param, 0, sizeof(*xiaomi_touch_driver_param));
	return ret;

}
int register_touch_panel(struct device *dev, s8 touch_id, hardware_param_t *hardware_param, hardware_operation_t *hardware_operation)
{
 int ret;
 mutex_lock(&spring_panel_lifecycle_lock);
 ret = spring_register_touch_panel_impl(dev, touch_id, hardware_param, hardware_operation);
 mutex_unlock(&spring_panel_lifecycle_lock);
 return ret;
}
EXPORT_SYMBOL_GPL(register_touch_panel);

void unregister_touch_panel(s8 touch_id)
{
 xiaomi_touch_data_t *data;
 xiaomi_touch_driver_param_t *param;
 private_data_t *client, *next;

 if (IS_TOUCH_ID_INVALID(touch_id))
  return;
 mutex_lock(&spring_panel_lifecycle_lock);
 if (!(xiaomi_touch.panel_register_mask & BIT(touch_id)))
  goto out;
 data = &xiaomi_touch.xiaomi_touch_data[touch_id];
 param = &xiaomi_touch.xiaomi_touch_driver_param[touch_id];
 WRITE_ONCE(spring_panel_closing[touch_id], true);

 if (!IS_ERR_OR_NULL(xiaomi_touch_temp_thread[touch_id])) {
  kthread_stop(xiaomi_touch_temp_thread[touch_id]);
  xiaomi_touch_temp_thread[touch_id] = NULL;
 }
/* Let registration/API calls which started before closing finish first. */
 synchronize_srcu(&spring_touch_readers);
#if defined(CONFIG_DRM)
 if (data->dev)
  xiaomi_unregister_panel_notifier(data->dev, touch_id);
#endif
 xiaomi_touch_remove_proc(touch_id);
 /* All operations which obtained the old callback pointers have returned. */
 synchronize_srcu(&spring_touch_readers);
 spring_touch_cancel_mode_work(touch_id);
 if (data->event_wq) {
  destroy_workqueue(data->event_wq);
  data->event_wq = NULL;
 }

 spin_lock(&data->private_data_lock);
 list_for_each_entry_safe(client, next, &data->private_data_list, node) {
  list_del_init(&client->node);
  WRITE_ONCE(client->touch_id, -1);
  wake_up_all(&client->poll_wait_queue_head);
  wake_up_all(&client->poll_wait_queue_head_for_cmd);
  wake_up_all(&client->poll_wait_queue_head_for_frame);
  wake_up_all(&client->poll_wait_queue_head_for_raw);
 }
 spin_unlock(&data->private_data_lock);

 kzalloc_free(data->frame_data_mmap_base);
 kzalloc_free(data->raw_data_mmap_base);
 kzalloc_free(data->poll_data);
 data->frame_data_mmap_base = NULL;
 data->raw_data_mmap_base = NULL;
 data->poll_data = NULL;
 data->frame_data_mmap_phy_base = 0;
 data->raw_data_mmap_phy_base = 0;
 memset(&param->hardware_operation, 0, sizeof(param->hardware_operation));
 memset(&param->hardware_param, 0, sizeof(param->hardware_param));
 xiaomi_touch.panel_register_mask &= ~BIT(touch_id);
 spring_panel_retired[touch_id] = true;
out:
 mutex_unlock(&spring_panel_lifecycle_lock);
}

EXPORT_SYMBOL_GPL(unregister_touch_panel);

#if defined(CONFIG_DRM)
static void xiaomi_touch_resume_work(struct work_struct *work)
{
	SPRING_TOUCH_READER_GUARD;
	xiaomi_touch_data_t *xiaomi_touch_data = container_of(work, xiaomi_touch_data_t, resume_work);
	s8 touch_id = xiaomi_touch_data->touch_id;
	xiaomi_touch_driver_param_t *xiaomi_touch_driver_param = get_xiaomi_touch_driver_param(touch_id);
#ifdef TOUCH_THP_SUPPORT
	int value = 0;
#endif

	LOG_INFO("touch id %d enter", touch_id);
	if (!xiaomi_touch_data || !xiaomi_touch_driver_param)
		return;
	if (!xiaomi_touch_data->is_suspend) {
		LOG_ERROR("touch id %d is resume, stop resume", touch_id);
		return;
	}
	XIAOMI_TOUCH_UTC_PRINT("");
#ifdef TOUCH_SENSORHUB_SUPPORT
	xiaomi_notify_sensorhub_enable(touch_id, false, false);
#endif
	if (xiaomi_touch_driver_param->hardware_operation.ic_resume_suspend) {
		xiaomi_touch_driver_param->hardware_operation.ic_resume_suspend(true, xiaomi_get_gesture_type(touch_id));
	}
#ifdef TOUCH_THP_SUPPORT
	add_common_data_to_buf(touch_id, SET_CUR_VALUE, DATA_MODE_27, 1, &value);
	add_common_data_to_buf(touch_id, SET_CUR_VALUE, DATA_MODE_61, 1, &xiaomi_touch.charging_status);
#endif
	/*reset charge state*/
	if (xiaomi_touch_driver_param->hardware_operation.ic_set_charge_state) {
		xiaomi_touch_driver_param->hardware_operation.ic_set_charge_state(xiaomi_touch.charging_status);
	}
	xiaomi_touch_data->is_suspend = false;
	/* other resume to do */

}

static void xiaomi_touch_suspend_work(struct work_struct *work)
{
	SPRING_TOUCH_READER_GUARD;
	xiaomi_touch_data_t *xiaomi_touch_data = container_of(work, xiaomi_touch_data_t, suspend_work);
	s8 touch_id = xiaomi_touch_data->touch_id;
	xiaomi_touch_driver_param_t *xiaomi_touch_driver_param = get_xiaomi_touch_driver_param(touch_id);
#ifdef TOUCH_THP_SUPPORT
	int value = 1;
#endif
#ifdef TOUCH_SENSORHUB_SUPPORT
#ifndef CONFIG_TOUCH_FACTORY_BUILD
	int ssh_value = XIAOMI_TOUCH_SENSORHUB_NONUIENABLE;
#endif
#endif

	LOG_INFO("touch id %d enter", touch_id);
	if (!xiaomi_touch_data || !xiaomi_touch_driver_param)
		return;
	if (xiaomi_touch_data->is_suspend) {
		LOG_ERROR("touch id %d is suspend, stop suspend", touch_id);
		return;
	}
	XIAOMI_TOUCH_UTC_PRINT("");
	if (xiaomi_touch_driver_param->hardware_operation.ic_resume_suspend) {
		xiaomi_touch_driver_param->hardware_operation.ic_resume_suspend(false, xiaomi_get_gesture_type(touch_id));
	}
#ifdef TOUCH_THP_SUPPORT
	add_common_data_to_buf(touch_id, SET_CUR_VALUE, DATA_MODE_27, 1, &value);
#ifdef TOUCH_SENSORHUB_SUPPORT
	xiaomi_notify_sensorhub_enable(touch_id, true, false);
#ifndef CONFIG_TOUCH_FACTORY_BUILD
	if (!xiaomi_touch_data->ssh_status && (driver_get_touch_mode(touch_id, DATA_MODE_10) != 0) && (driver_get_touch_mode(touch_id, DATA_MODE_17) == 2)) {
		LOG_INFO("sleep by nonui mode,need hold wakeup nonui sensor to notify ap reenter gesture");
		add_common_data_to_buf(touch_id, SET_CUR_VALUE, DATA_MODE_27, 1, &ssh_value);
	}
#endif
#endif
#endif
	xiaomi_touch_data->is_suspend = true;
	/* other suspend to do */

}

#if defined(TOUCH_IS_TDDI)
static void xiaomi_touch_suspend_tddi(s8 touch_id)
{
	SPRING_TOUCH_READER_GUARD;
	xiaomi_touch_data_t *xiaomi_touch_data = get_xiaomi_touch_data(touch_id);
	xiaomi_touch_driver_param_t *xiaomi_touch_driver_param = get_xiaomi_touch_driver_param(touch_id);
	int value = 1;
	LOG_INFO("touch id %d enter", touch_id);
	if (!xiaomi_touch_data || !xiaomi_touch_driver_param)
		return;
	if (xiaomi_touch_data->is_suspend) {
		LOG_ERROR("touch id %d is suspend, stop suspend", touch_id);
		return;
	}
	XIAOMI_TOUCH_UTC_PRINT("");
	if (xiaomi_touch_driver_param->hardware_operation.ic_resume_suspend) {
		xiaomi_touch_driver_param->hardware_operation.ic_resume_suspend(false, xiaomi_get_gesture_type(touch_id));
	}
#ifdef TOUCH_THP_SUPPORT
		LOG_INFO("suspend to thp");
		add_common_data_to_buf(touch_id, SET_CUR_VALUE, DATA_MODE_27, 1, &value);
#endif
	xiaomi_touch_data->is_suspend = true;
	/* other suspend to do */
}
#endif

#if defined(TOUCH_PLATFORM_XRING)
static void xiaomi_drm_panel_notifier_callback(enum xring_panel_event_tag tag,
		struct xring_panel_event_notification *notification, void *client_data)
{
	SPRING_TOUCH_READER_GUARD;
	long touch_id = (long)client_data;
	if (!notification || IS_TOUCH_ID_INVALID(touch_id)) {
		LOG_ERROR("Invalid data. notification %p, touch_id %ld", notification, touch_id);
		return;
	}
	if (notification->type <= DRM_PANEL_EVENT_MAX)
		LOG_INFO("Notification type:%d, early_trigger:%d", notification->type, notification->data.early_trigger);
	switch (notification->type) {
	case DRM_PANEL_EVENT_UNBLANK:
	/*NOTE: uncomment after display adapted
		if (notification->data.early_trigger == 1) {
			LOG_INFO("FB_BLANK_UNBLAN");
			schedule_resume_suspend_work((s8)touch_id, true);
		}
	*/
			LOG_INFO("PANEL_EVENT_UNBLANK");
			schedule_resume_suspend_work((s8)touch_id, true);
		break;
	case DRM_PANEL_EVENT_BLANK:
	case DRM_PANEL_EVENT_BLANK_LP:
	/*NOTE: uncomment after display adapted
		if (notification->data.early_trigger == 0) {
			LOG_INFO("FB_BLANK LP");
			schedule_resume_suspend_work((s8)touch_id, false);
		}
	*/
			LOG_INFO("%s", notification->type == DRM_PANEL_EVENT_BLANK_LP ? \
				"PANEL_EVENT_BLANK_LP" : "PANEL_EVENT_BLANK");
			schedule_resume_suspend_work((s8)touch_id, false);
		break;
	case DRM_PANEL_EVENT_FPS_CHANGE:
		break;
	default:
		LOG_ERROR("notification serviced :%d", notification->type);
		break;
	}
	if (notification->type < 4)
		LOG_INFO("notification complete");
}
#else
static void xiaomi_drm_panel_notifier_callback(enum panel_event_notifier_tag tag,
		struct panel_event_notification *notification, void *client_data)
{
	SPRING_TOUCH_READER_GUARD;
	long touch_id = (long)client_data;
#ifdef TOUCH_IS_TDDI
	xiaomi_touch_data_t *xiaomi_touch_data = get_xiaomi_touch_data(touch_id);
#endif

	if (!notification || IS_TOUCH_ID_INVALID(touch_id)) {
		LOG_ERROR("Invalid data. notification %p, touch_id %ld", notification, touch_id);
		return;
	}
	if (notification->notif_type < 4)
		LOG_INFO("Notification type:%d, early_trigger:%d", notification->notif_type, notification->notif_data.early_trigger);

	switch (notification->notif_type) {
	case DRM_PANEL_EVENT_UNBLANK:
#if defined(TOUCH_IS_TDDI)
		if (notification->notif_data.early_trigger == 0) {
			LOG_INFO("TDDI_FB_BLANK_UNBLAN");
			schedule_resume_suspend_work((s8)touch_id, true);
		}
#else
		if (notification->notif_data.early_trigger == 1) {
			LOG_INFO("FB_BLANK_UNBLAN");
			schedule_resume_suspend_work((s8)touch_id, true);
		}
#endif
		break;
	case DRM_PANEL_EVENT_BLANK:
	case DRM_PANEL_EVENT_BLANK_LP:
#if defined(TOUCH_IS_TDDI)
		if (notification->notif_data.early_trigger == 1) {
			flush_workqueue(xiaomi_touch_data->event_wq);
			LOG_INFO("TDDI_FB_BLANK %s", notification->notif_type == DRM_PANEL_EVENT_BLANK_LP ? "POWER DOWN" : "LP");
			xiaomi_touch_suspend_tddi((s8)touch_id);
		}
#else
		if (notification->notif_data.early_trigger == 0) {
			LOG_INFO("FB_BLANK %s", notification->notif_type == DRM_PANEL_EVENT_BLANK_LP ? "POWER DOWN" : "LP");
			schedule_resume_suspend_work((s8)touch_id, false);
		}
#endif
		break;
	case DRM_PANEL_EVENT_FPS_CHANGE:
		break;
	default:
		LOG_ERROR("notification serviced :%d", notification->notif_type);
		break;
	}
	if (notification->notif_type < 4)
		LOG_INFO("notification complete");
}
#endif

static void xiaomi_register_panel_notifier_work(struct work_struct *work)
{
	SPRING_TOUCH_READER_GUARD;
	xiaomi_touch_data_t *xiaomi_touch_data = container_of(work, xiaomi_touch_data_t, panel_notifier_register_work.work);
	static int check_count = 0;
	struct drm_panel *panel = NULL;
	struct device_node *node;
	int count = 0;
	int i = 0;
	long touch_id = -1;
#if defined(TOUCH_PLATFORM_XRING)
	char *property_name = "dsi-panel";
#else
	char *property_name = "panel";
#endif

	if (READ_ONCE(spring_panel_closing[xiaomi_touch_data->touch_id]))
		return;

	LOG_INFO("Start register panel notifier");
	count = of_count_phandle_with_args(xiaomi_touch_data->dev->of_node, property_name, NULL);
	for (i = 0; i < count; i++) {
		node = of_parse_phandle(xiaomi_touch_data->dev->of_node, property_name, i);
		panel = of_drm_find_panel(node);
		if (!IS_ERR(panel)) {
			break;
		} else {
			panel = NULL;
			of_node_put(node);
		}
	}

	if (!panel) {
		LOG_ERROR("Failed to register panel notifier, try again");
		if (check_count++ < 5)
			schedule_delayed_work(&xiaomi_touch_data->panel_notifier_register_work, msecs_to_jiffies(5000));
		else {
			LOG_ERROR("Failed to register panel notifier, not try");
		}
		return;
	}

	for (i = 0; i < MAX_TOUCH_PANEL_COUNT; i++) {
		if (xiaomi_touch_data == &xiaomi_touch.xiaomi_touch_data[i]) {
			touch_id = i;
			break;
		}
	}

	if (touch_id < 0) {
		LOG_ERROR("can't not find this touch id!");
		return;
	}

#if defined(TOUCH_PLATFORM_XRING)
	xiaomi_touch_data->notifier_cookie = xring_panel_event_notifier_register(xiaomi_touch_data->panel_event_notifier_tag,
		xiaomi_touch_data->panel_event_notifier_client, node,
		&xiaomi_drm_panel_notifier_callback, (void *)touch_id);
#else
#if IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
	xiaomi_touch_data->notifier_cookie = panel_event_notifier_register(xiaomi_touch_data->panel_event_notifier_tag,
		xiaomi_touch_data->panel_event_notifier_client, panel,
		&xiaomi_drm_panel_notifier_callback, (void *)touch_id);
#endif
#endif
	of_node_put(node);
	if (IS_ERR(xiaomi_touch_data->notifier_cookie)) {
		LOG_ERROR("Failed to register for panel events");
	}
}

#endif

void schedule_resume_suspend_work(s8 touch_id, bool resume_work)
{
	SPRING_TOUCH_READER_GUARD;
#if defined(CONFIG_DRM)
	xiaomi_touch_data_t *xiaomi_touch_data = get_xiaomi_touch_data(touch_id);
	bool queue_work_result = false;
	if (!xiaomi_touch_data) {
		LOG_ERROR("xiaomi_touch_data is NULL!");
		return;
	}

	flush_workqueue(xiaomi_touch_data->event_wq);
	if (resume_work) {
		queue_work_result = queue_work(xiaomi_touch_data->event_wq, &xiaomi_touch_data->resume_work);
	} else {
		queue_work_result = queue_work(xiaomi_touch_data->event_wq, &xiaomi_touch_data->suspend_work);
	}

	if (!queue_work_result) {
		LOG_ERROR("queue %s work failed, retry", resume_work ? "resume" : "suspend");
	}

#endif
}
EXPORT_SYMBOL_GPL(schedule_resume_suspend_work);

/**
 * this function must be called after register_touch_panel()
*/
void xiaomi_register_panel_notifier(struct device *dev, s8 touch_id,
		int panel_event_notifier_tag, int panel_event_notifier_client)
{
	SPRING_TOUCH_READER_GUARD;
#if defined(CONFIG_DRM)
	xiaomi_touch_data_t *xiaomi_touch_data = get_xiaomi_touch_data(touch_id);
	if (xiaomi_touch_data == NULL)
		return;
	xiaomi_touch_data->dev = dev;
	xiaomi_touch_data->panel_event_notifier_tag = panel_event_notifier_tag;
	xiaomi_touch_data->panel_event_notifier_client = panel_event_notifier_client;
	INIT_DELAYED_WORK(&xiaomi_touch_data->panel_notifier_register_work, xiaomi_register_panel_notifier_work);
	INIT_WORK(&xiaomi_touch_data->suspend_work, xiaomi_touch_suspend_work);
	INIT_WORK(&xiaomi_touch_data->resume_work, xiaomi_touch_resume_work);
	schedule_delayed_work(&xiaomi_touch_data->panel_notifier_register_work, msecs_to_jiffies(0));
#endif
}
EXPORT_SYMBOL_GPL(xiaomi_register_panel_notifier);

void xiaomi_unregister_panel_notifier(struct device *dev, s8 touch_id)
{
	SPRING_TOUCH_READER_GUARD;
#if defined(CONFIG_DRM)
	xiaomi_touch_data_t *xiaomi_touch_data = spring_touch_client_data(touch_id);
	if (xiaomi_touch_data == NULL || !xiaomi_touch_data->dev)
		return;
	/* Finish registration before detaching the notifier. */
	cancel_delayed_work_sync(&xiaomi_touch_data->panel_notifier_register_work);

#if defined(TOUCH_PLATFORM_XRING)
	if (!IS_ERR_OR_NULL(xiaomi_touch_data->notifier_cookie))
		xring_panel_event_notifier_unregister(xiaomi_touch_data->notifier_cookie);
#else
#if IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
	if (!IS_ERR_OR_NULL(xiaomi_touch_data->notifier_cookie))
		panel_event_notifier_unregister(xiaomi_touch_data->notifier_cookie);
#endif
#endif
	xiaomi_touch_data->notifier_cookie = NULL;
	xiaomi_touch_data->dev = NULL;
	/* No notifier can queue new suspend/resume work now. */
	cancel_work_sync(&xiaomi_touch_data->suspend_work);
	cancel_work_sync(&xiaomi_touch_data->resume_work);
#endif
}
EXPORT_SYMBOL_GPL(xiaomi_unregister_panel_notifier);

static int xiaomi_get_charging_status(void)
{
	SPRING_TOUCH_READER_GUARD;
	struct power_supply *usb_psy;
	struct power_supply *dc_psy;
	union power_supply_propval val;
	int rc = 0;

	dc_psy = power_supply_get_by_name("wireless");
	if (dc_psy) {
		rc = power_supply_get_property(dc_psy, POWER_SUPPLY_PROP_ONLINE, &val);
		if (rc < 0)
			LOG_ERROR("Couldn't get DC online status, rc=%d\n", rc);
		else if (val.intval == 1)
			return CHARGING | WIRELESS_CHARGING;
	}

	usb_psy = power_supply_get_by_name("usb");
	if (usb_psy) {
		rc = power_supply_get_property(usb_psy, POWER_SUPPLY_PROP_ONLINE, &val);
		if (rc < 0)
			LOG_ERROR("Couldn't get usb online status, rc=%d\n", rc);
		else if (val.intval == 1)
			return CHARGING | WIRED_CHARGING;
	}
	return NOT_CHARGING;
}

static void xiaomi_power_supply_work(struct work_struct *work)
{
	SPRING_TOUCH_READER_GUARD;
	xiaomi_touch_t *xiaomi_touch = container_of(work, xiaomi_touch_t, power_supply_work);
	int charging_status;
	int i = 0;
	xiaomi_touch_driver_param_t *xiaomi_touch_driver_param;

	charging_status = xiaomi_get_charging_status();
	if (charging_status != xiaomi_touch->charging_status || xiaomi_touch->charging_status < 0) {
		xiaomi_touch->charging_status = charging_status;
	for (i = 0; i < MAX_TOUCH_PANEL_COUNT; i++) {
		xiaomi_touch_driver_param = get_xiaomi_touch_driver_param(i);
		if (!xiaomi_touch_driver_param)
			break;
#ifdef TOUCH_THP_SUPPORT
		add_common_data_to_buf(i, SET_CUR_VALUE, DATA_MODE_61, 1, &charging_status);
#endif
		if (xiaomi_touch_driver_param->hardware_operation.ic_set_charge_state) {
			xiaomi_touch_driver_param->hardware_operation.ic_set_charge_state(charging_status);
		}
	}
	}
}

static int xiaomi_power_supply_notifier_callback(struct notifier_block *nb, unsigned long event, void *ptr)
{
	SPRING_TOUCH_READER_GUARD;
	schedule_work(&xiaomi_touch.power_supply_work);
	return 0;
}

static void xiaomi_register_power_supply_event(xiaomi_touch_t *xiaomi_touch)
{
	SPRING_TOUCH_READER_GUARD;
	int retval = 0;

	if (xiaomi_touch == NULL)
		return;
	INIT_WORK(&xiaomi_touch->power_supply_work, xiaomi_power_supply_work);
	xiaomi_touch->charging_status = -1;
	xiaomi_touch->power_supply_notifier.notifier_call = xiaomi_power_supply_notifier_callback;
	retval = power_supply_reg_notifier(&xiaomi_touch->power_supply_notifier);
	if (retval < 0) {
		LOG_ERROR("error:%d\n", retval);
		return;
	}
}

static void xiaomi_unregister_power_supply_event(void)
{
	SPRING_TOUCH_READER_GUARD;
	power_supply_unreg_notifier(&xiaomi_touch.power_supply_notifier);
	cancel_work_sync(&xiaomi_touch.power_supply_work);
}

#if IS_ENABLED(CONFIG_MIEV)
void xiaomi_touch_mievent_report_int(unsigned int code, int panel_id, const char *fault_name, const char *vendor_name, long error_code)
{
	SPRING_TOUCH_READER_GUARD;
	struct misight_mievent *event;

#ifdef CONFIG_TOUCH_FACTORY_BUILD
	LOG_INFO("factory build,return\n");
	return;
#endif
	LOG_INFO("code:%d,fault_name:%s,panel_id:%d,vendor_name:%s,error_code:%ld\n", code, fault_name, panel_id, vendor_name, error_code);
	event = cdev_tevent_alloc(code);
	if (!event) {
		LOG_ERROR("misight event is error\n");
		return;
	}
	cdev_tevent_add_int(event, "panel_id", panel_id);
	cdev_tevent_add_str(event, "fault_name", fault_name);
	cdev_tevent_add_str(event, "vendor_name", vendor_name);
	cdev_tevent_add_int(event, "error_code", error_code);
	cdev_tevent_write(event);
	cdev_tevent_destroy(event);
}
EXPORT_SYMBOL_GPL(xiaomi_touch_mievent_report_int);

void xiaomi_touch_mievent_report_str(unsigned int code, int panel_id, const char *fault_name, const char *vendor_name)
{
	SPRING_TOUCH_READER_GUARD;
	struct misight_mievent *event;

#ifdef CONFIG_TOUCH_FACTORY_BUILD
	LOG_INFO("factory build,return\n");
	return;
#endif
	LOG_INFO("code:%d,fault_name:%s,panel_id:%d,vendor_name:%s\n", code, fault_name, panel_id, vendor_name);
	event = cdev_tevent_alloc(code);
	if (!event) {
		LOG_ERROR("misight event is error\n");
		return;
	}
	cdev_tevent_add_int(event, "panel_id", panel_id);
	cdev_tevent_add_str(event, "fault_name", fault_name);
	cdev_tevent_add_str(event, "vendor_name", vendor_name);
	cdev_tevent_write(event);
	cdev_tevent_destroy(event);
}
EXPORT_SYMBOL_GPL(xiaomi_touch_mievent_report_str);
#endif

void nfc_to_touch_event(s8 touch_id,u8 val){
	SPRING_TOUCH_READER_GUARD;
	xiaomi_touch_driver_param_t *xiaomi_touch_driver_param = get_xiaomi_touch_driver_param(touch_id);
	LOG_INFO("enter touch_id =%d val = %d",touch_id, val);
	if(!xiaomi_touch_driver_param) {
		LOG_INFO("Touch panel  id %d hasn't register, return!",touch_id);
		return;
	}
	if(xiaomi_touch_driver_param->hardware_operation.set_nfc_to_touch_event)
		xiaomi_touch_driver_param->hardware_operation.set_nfc_to_touch_event(val);
	else
		LOG_INFO("Non-filp models do not support this function");
}
EXPORT_SYMBOL_GPL(nfc_to_touch_event);

static int xiaomi_touch_probe(struct platform_device *pdev)
{
	LOG_ALWAYS("xiaomi_touch ver: %s", XIAOMI_TOUCH_VERSION);
#ifdef TOUCH_KNOCK_SUPPORT
	knock_node_init();
#endif
	memset(&xiaomi_touch, 0, sizeof(xiaomi_touch_t));
	xiaomi_touch_sys_init();
	xiaomi_touch_operation_init(&xiaomi_touch);
	xiaomi_register_power_supply_event(&xiaomi_touch);
	netlink_init();
	xiaomi_touch_evdev_init(&xiaomi_touch);
	LOG_INFO("over");

	return 0;
}

static int xiaomi_touch_remove(struct platform_device *pdev)
{
	LOG_INFO("enter");
	xiaomi_touch_evdev_remove();
    netlink_exit();
	xiaomi_unregister_power_supply_event();
	xiaomi_touch_operation_remove();
	xiaomi_touch_sys_remove();
	memset(&xiaomi_touch, 0, sizeof(xiaomi_touch_t));
#ifdef TOUCH_KNOCK_SUPPORT
	knock_node_release();
#endif
	LOG_INFO("over");
	return 0;
}

#if defined(TOUCH_PLATFORM_XRING)
#include "touch_of_match_table.h"
#else
static const struct of_device_id xiaomi_touch_of_match[] = {
	{ .compatible = "xiaomi-touch", },
	{ },
};
#endif
static struct platform_driver xiaomi_touch_device_driver = {
	.probe		= xiaomi_touch_probe,
	.remove		= xiaomi_touch_remove,
	.driver		= {
		.name	= "xiaomi-touch",
		.suppress_bind_attrs = true,
		.of_match_table = of_match_ptr(xiaomi_touch_of_match),
	}
};

static int __init xiaomi_touch_init(void)
{
	return platform_driver_register(&xiaomi_touch_device_driver);
}

static void __exit xiaomi_touch_exit(void)
{
	platform_driver_unregister(&xiaomi_touch_device_driver);
	srcu_barrier(&spring_touch_readers);
}

MODULE_LICENSE("GPL");

module_init(xiaomi_touch_init);
module_exit(xiaomi_touch_exit);
