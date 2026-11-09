#include <linux/module.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/kdev_t.h>
#include <linux/idr.h>
#include <linux/thermal.h>
#include <linux/reboot.h>
#include <linux/string.h>
#include <linux/of.h>
#include <net/netlink.h>
#include <net/genetlink.h>
#include <linux/suspend.h>
#include <linux/cpu_cooling.h>
#include <linux/soc/qcom/panel_event_notifier.h>
#include <linux/pm_qos.h>
#include <linux/cpufreq.h>
#include <linux/kobject.h>
#include <linux/kernfs.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>
#include <drivers/base/base.h>

#include <linux/notifier.h>
#include <linux/srcu.h>

enum mi_thermal_notifier_events {
	THERMAL_BOARD_TEMP = 0,
};

SRCU_NOTIFIER_HEAD(mi_thermal_notifier);
EXPORT_SYMBOL_GPL(mi_thermal_notifier);

int mi_thermal_reg_notifier(struct notifier_block *nb)
{
	return srcu_notifier_chain_register(&mi_thermal_notifier, nb);
}
EXPORT_SYMBOL_GPL(mi_thermal_reg_notifier);

int mi_thermal_unreg_notifier(struct notifier_block *nb)
{
	return srcu_notifier_chain_unregister(&mi_thermal_notifier, nb);
}
EXPORT_SYMBOL_GPL(mi_thermal_unreg_notifier);

int mi_thermal_notifier_call_chain(unsigned long event, int value)
{
	return srcu_notifier_call_chain(&mi_thermal_notifier, event, &value);
}
EXPORT_SYMBOL_GPL(mi_thermal_notifier_call_chain);


#if defined(CONFIG_DRM_PANEL)
static struct drm_panel *prim_panel;
#endif
#define BOOST_BUFFER_SIZE 128
#define BOARD__BUFFER_SIZE 128
struct mi_thermal_device {
	struct device *dev;
	struct class *class;
	struct attribute_group attrs;
};

struct freq_table {
	u32 frequency;
};

struct cpufreq_device {
	int id;
	unsigned int cpufreq_state;
	unsigned int max_level;
	struct freq_table *freq_table; /* In descending order */
	struct cpufreq_policy *policy;
	struct list_head node;
	struct freq_qos_request *qos_req;
};

#ifdef CONFIG_MI_THERMAL_MULTI_CHARGE
struct usb_monitor {
	struct notifier_block psy_nb;
	struct work_struct usb_state_work;
	int usb_online;
};
static struct usb_monitor usb_state;
static atomic_t charger_mode = ATOMIC_INIT(-1);
#endif

static struct mi_thermal_device mi_thermal_dev;
static int screen_state = 0;
static int screen_light = 0;
#if IS_ENABLED(CONFIG_HAVE_MULTI_SCREEN)
static void *cookie_sec = NULL;
static struct drm_panel *sec_panel;
static int retry_count_sec = 10;
#endif
static int screen_last_status = 0;
static void *cookie = NULL;

static atomic_t temp_state = ATOMIC_INIT(0);
static atomic_t switch_mode = ATOMIC_INIT(-1);
static atomic_t balance_mode = ATOMIC_INIT(0);
static atomic_t modem_limit = ATOMIC_INIT(0);
static atomic_t market_download_limit = ATOMIC_INIT(0);
static atomic_t board_sensor_temp_comp_default = ATOMIC_INIT(0);
static atomic_t cpu_nolimit_temp_default = ATOMIC_INIT(0);
static atomic_t poor_modem_limit = ATOMIC_INIT(0);
static atomic_t wifi_limit = ATOMIC_INIT(0);
static atomic_t flash_state = ATOMIC_INIT(0);
static atomic_t thermal_max_brightness = ATOMIC_INIT(0);
static atomic_t modem_rate = ATOMIC_INIT(0);
static atomic_t modem_level = ATOMIC_INIT(0);
static char boost_buf[128];
const char *board_sensor;
static char board_sensor_temp[128];
static char board_sensor_second_temp[128];
static char board_sensor_other_temp[128];
static char ambient_sensor_temp[128];
const char *ambient_sensor;
#ifdef CONFIG_HAVE_CHARGE_TEMP
static char board_sensor_charge_temp[128];
#endif
static LIST_HEAD(cpufreq_dev_list);
static DEFINE_MUTEX(cpufreq_list_lock);
static DEFINE_PER_CPU(struct freq_qos_request, qos_req);

static struct workqueue_struct *screen_state_wq;
static struct delayed_work screen_state_dw;

static int cpufreq_set_level(struct cpufreq_device *cdev, unsigned long state)
{
	/* Request state should be less than max_level */
	if (WARN_ON(state > cdev->max_level))
		return -EINVAL;

	/* Check if the old cooling action is same as new cooling action */
	if (cdev->cpufreq_state == state)
		return 0;

	{
		int ret = freq_qos_update_request(cdev->qos_req,
					cdev->freq_table[state].frequency);
		if (ret >= 0)
			cdev->cpufreq_state = state;
		return ret;
	}
}

void cpu_limits_set_level(unsigned int cpu, unsigned int max_freq)
{
	struct cpufreq_device *cpufreq_dev;
	unsigned int level = 0;

	mutex_lock(&cpufreq_list_lock);
	list_for_each_entry (cpufreq_dev, &cpufreq_dev_list, node) {
		if (cpufreq_dev->id == cpu) {
			for (level = 0; level <= cpufreq_dev->max_level;
			     level++) {
				int target_freq =
					cpufreq_dev->freq_table[level].frequency;
				if (max_freq >= target_freq) {
					cpufreq_set_level(cpufreq_dev, level);
					break;
				}
			}
			break;
		}
	}
	mutex_unlock(&cpufreq_list_lock);
}

static unsigned int find_next_max(struct cpufreq_frequency_table *table,
				  unsigned int prev_max)
{
	struct cpufreq_frequency_table *pos;
	unsigned int max = 0;
	cpufreq_for_each_valid_entry (pos, table) {
		if (pos->frequency > max && pos->frequency < prev_max)
			max = pos->frequency;
	}
	return max;
}

static void destory_thermal_cpu(void)
{
	struct cpufreq_device *priv, *tmp;

	mutex_lock(&cpufreq_list_lock);
	list_for_each_entry_safe(priv, tmp, &cpufreq_dev_list, node) {
		freq_qos_remove_request(priv->qos_req);
		list_del(&priv->node);
		cpufreq_cpu_put(priv->policy);
		kfree(priv->freq_table);
		kfree(priv);
	}
	mutex_unlock(&cpufreq_list_lock);
}

static int cpu_thermal_init(void)
{
	int cpu, ret = 0;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy;
		struct cpufreq_device *cdev;
		unsigned int count, i, freq;

		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			pr_err("%s: cpufreq policy not found for cpu%d\n",
			       __func__, cpu);
			ret = -ESRCH;
			goto unwind;
		}

		if (!policy->freq_table) {
			ret = -ENODEV;
			goto put_policy;
		}
		count = cpufreq_table_count_valid_entries(policy);
		if (!count) {
			ret = -ENODEV;
			goto put_policy;
		}

		cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
		if (!cdev) {
			ret = -ENOMEM;
			goto put_policy;
		}

		cdev->policy = policy;
		cdev->qos_req = &per_cpu(qos_req, cpu);
		cdev->max_level = count - 1;
		cdev->id = policy->cpu;
		cdev->freq_table = kmalloc_array(
			count, sizeof(*cdev->freq_table), GFP_KERNEL);
		if (!cdev->freq_table) {
			ret = -ENOMEM;
			goto free_device;
		}

		for (i = 0, freq = UINT_MAX; i < count; i++) {
			freq = find_next_max(policy->freq_table, freq);
			cdev->freq_table[i].frequency = freq;
			if (!freq)
				pr_warn("%s: table has duplicate entries\n",
					__func__);
		}

		ret = freq_qos_add_request(&policy->constraints, cdev->qos_req,
					  FREQ_QOS_MAX,
					  cdev->freq_table[0].frequency);
		if (ret < 0)
			goto free_table;

		mutex_lock(&cpufreq_list_lock);
		list_add(&cdev->node, &cpufreq_dev_list);
		mutex_unlock(&cpufreq_list_lock);
		continue;

free_table:
		kfree(cdev->freq_table);
free_device:
		kfree(cdev);
put_policy:
		cpufreq_cpu_put(policy);
unwind:
		destory_thermal_cpu();
		return ret;
	}
	return 0;
}

static ssize_t thermal_temp_state_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&temp_state));
}

static ssize_t thermal_temp_state_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&temp_state, val);

	return len;
}

static DEVICE_ATTR(temp_state, 0664, thermal_temp_state_show,
		   thermal_temp_state_store);

static ssize_t cpu_limits_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t cpu_limits_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t len)
{
	unsigned int cpu;
	unsigned int max;

	if (sscanf(buf, "cpu%u %u", &cpu, &max) != 2) {
		pr_err("input param error, can not prase param\n");
		return -EINVAL;
	}
	cpu_limits_set_level(cpu, max);

	return len;
}

static DEVICE_ATTR(cpu_limits, 0664, cpu_limits_show, cpu_limits_store);

static ssize_t thermal_screen_state_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", screen_state);
}

static DEVICE_ATTR(screen_state, 0664, thermal_screen_state_show, NULL);

static ssize_t thermal_sconfig_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&switch_mode));
}

static ssize_t thermal_sconfig_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&switch_mode, val);

	return len;
}

static ssize_t thermal_boost_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s", boost_buf);
}

static ssize_t thermal_boost_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t len)
{
	int ret;
	ret = snprintf(boost_buf, BOOST_BUFFER_SIZE, "%s", buf);
	return len;
}

static ssize_t thermal_balance_mode_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&balance_mode));
}

static ssize_t thermal_balance_mode_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&balance_mode, val);

	return len;
}

static ssize_t thermal_market_download_limit_show(struct device *dev,
						  struct device_attribute *attr,
						  char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n",
			atomic_read(&market_download_limit));
}

static ssize_t
thermal_market_download_limit_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&market_download_limit, val);
	return len;
}

static DEVICE_ATTR(boost, 0644, thermal_boost_show, thermal_boost_store);

static DEVICE_ATTR(sconfig, 0664, thermal_sconfig_show, thermal_sconfig_store);

static DEVICE_ATTR(balance_mode, 0664, thermal_balance_mode_show,
		   thermal_balance_mode_store);

static DEVICE_ATTR(market_download_limit, 0664,
		   thermal_market_download_limit_show,
		   thermal_market_download_limit_store);

static ssize_t thermal_board_sensor_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	if (!board_sensor)
		board_sensor = "invalid";

	return snprintf(buf, PAGE_SIZE, "%s", board_sensor);
}

static DEVICE_ATTR(board_sensor, 0664, thermal_board_sensor_show, NULL);

static ssize_t thermal_board_sensor_temp_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", board_sensor_temp);
}

static ssize_t thermal_board_sensor_temp_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int value = (int)simple_strtol(buf, NULL, 10);
	int ret;

	snprintf(board_sensor_temp, sizeof(board_sensor_temp), "%s", buf);
	ret = mi_thermal_notifier_call_chain(THERMAL_BOARD_TEMP, value / 100);
	if (ret)
		pr_err("%s: mi_thermal_notifier_call_chain error:%d\n",
		       __func__, ret);
	return len;
}

static DEVICE_ATTR(board_sensor_temp, 0664, thermal_board_sensor_temp_show,
		   thermal_board_sensor_temp_store);
static ssize_t thermal_modem_rate_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&modem_rate));
}
static ssize_t thermal_modem_rate_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&modem_rate, val);
	return len;
}

static DEVICE_ATTR(modem_rate, 0664, thermal_modem_rate_show,
		   thermal_modem_rate_store);
static ssize_t thermal_modem_level_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&modem_level));
}
static ssize_t thermal_modem_level_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&modem_level, val);
	return len;
}

static DEVICE_ATTR(modem_level, 0664, thermal_modem_level_show,
		   thermal_modem_level_store);
static ssize_t
thermal_board_sensor_second_temp_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s", board_sensor_second_temp);
}

static ssize_t
thermal_board_sensor_second_temp_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t len)
{
	snprintf(board_sensor_second_temp, BOARD__BUFFER_SIZE, "%s", buf);

	return len;
}

static DEVICE_ATTR(board_sensor_second_temp, 0664,
		   thermal_board_sensor_second_temp_show,
		   thermal_board_sensor_second_temp_store);

static ssize_t
thermal_board_sensor_other_temp_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s", board_sensor_other_temp);
}

static ssize_t
thermal_board_sensor_other_temp_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t len)
{
	snprintf(board_sensor_other_temp, BOARD__BUFFER_SIZE, "%s", buf);

	return len;
}

static DEVICE_ATTR(board_sensor_other_temp, 0664,
		   thermal_board_sensor_other_temp_show,
		   thermal_board_sensor_other_temp_store);

static ssize_t thermal_ambient_sensor_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s", "ABT-SENSOR");
}

static DEVICE_ATTR(ambient_sensor, 0664, thermal_ambient_sensor_show, NULL);
static ssize_t thermal_ambient_sensor_temp_show(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s", ambient_sensor_temp);
}

static ssize_t thermal_ambient_sensor_temp_store(struct device *dev,
						 struct device_attribute *attr,
						 const char *buf, size_t len)
{
	snprintf(ambient_sensor_temp, BOARD__BUFFER_SIZE, "%s", buf);

	return len;
}

static DEVICE_ATTR(ambient_sensor_temp, 0664, thermal_ambient_sensor_temp_show,
		   thermal_ambient_sensor_temp_store);
#ifdef CONFIG_HAVE_CHARGE_TEMP
static ssize_t
thermal_board_sensor_charge_temp_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s", board_sensor_charge_temp);
}

static ssize_t
thermal_board_sensor_charge_temp_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t len)
{
	snprintf(board_sensor_charge_temp, BOARD__BUFFER_SIZE, "%s", buf);

	return len;
}

static DEVICE_ATTR(board_sensor_charge_temp, 0664,
		   thermal_board_sensor_charge_temp_show,
		   thermal_board_sensor_charge_temp_store);
#endif

static ssize_t thermal_modem_limit_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&modem_limit));
}
static ssize_t thermal_modem_limit_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&modem_limit, val);
	return len;
}

static DEVICE_ATTR(modem_limit, 0664, thermal_modem_limit_show,
		   thermal_modem_limit_store);

static ssize_t thermal_wifi_limit_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&wifi_limit));
}
static ssize_t thermal_wifi_limit_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&wifi_limit, val);
	return len;
}

static DEVICE_ATTR(wifi_limit, 0664, thermal_wifi_limit_show,
		   thermal_wifi_limit_store);

static ssize_t thermal_flash_state_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&flash_state));
}
static ssize_t thermal_flash_state_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&flash_state, val);
	return len;
}

static DEVICE_ATTR(flash_state, 0664, thermal_flash_state_show,
		   thermal_flash_state_store);

static ssize_t thermal_max_brightness_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n",
			atomic_read(&thermal_max_brightness));
}
static ssize_t thermal_max_brightness_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&thermal_max_brightness, val);
	return len;
}

static DEVICE_ATTR(thermal_max_brightness, 0664, thermal_max_brightness_show,
		   thermal_max_brightness_store);

static ssize_t
thermal_board_sensor_temp_comp_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n",
			atomic_read(&board_sensor_temp_comp_default));
}

static ssize_t
thermal_board_sensor_temp_comp_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&board_sensor_temp_comp_default, val);

	return len;
}

static DEVICE_ATTR(board_sensor_temp_comp, 0664,
		   thermal_board_sensor_temp_comp_show,
		   thermal_board_sensor_temp_comp_store);

static ssize_t thermal_poor_modem_limit_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&poor_modem_limit));
}
static ssize_t thermal_poor_modem_limit_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&poor_modem_limit, val);
	return len;
}

static DEVICE_ATTR(poor_modem_limit, 0664, thermal_poor_modem_limit_show,
		   thermal_poor_modem_limit_store);

static ssize_t thermal_cpu_nolimit_temp_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n",
			atomic_read(&cpu_nolimit_temp_default));
}

static ssize_t thermal_cpu_nolimit_temp_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&cpu_nolimit_temp_default, val);

	return len;
}

static DEVICE_ATTR(cpu_nolimit_temp, 0664, thermal_cpu_nolimit_temp_show,
		   thermal_cpu_nolimit_temp_store);

#ifdef CONFIG_MI_THERMAL_MULTI_CHARGE
static ssize_t thermal_charger_temp_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&charger_mode));
}

static ssize_t thermal_charger_temp_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&charger_mode, val);

	return len;
}

static DEVICE_ATTR(charger_temp, 0664, thermal_charger_temp_show,
		   thermal_charger_temp_store);

static ssize_t thermal_usb_online_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", usb_state.usb_online);
}

static DEVICE_ATTR(usb_online, 0664, thermal_usb_online_show, NULL);

static int usb_online_callback(struct notifier_block *nb, unsigned long val,
			       void *data)
{
	struct power_supply *psy = data;

	if (strcmp(psy->desc->name, "usb"))
		return NOTIFY_OK;
	schedule_work(&usb_state.usb_state_work);

	return NOTIFY_OK;
}

static void usb_online_work(struct work_struct *work)
{
	static struct power_supply *usb_psy;
	union power_supply_propval ret = {
		0,
	};
	int err = 0;

	if (!usb_psy)
		usb_psy = power_supply_get_by_name("usb");

	if (usb_psy) {
		err = power_supply_get_property(usb_psy,
						POWER_SUPPLY_PROP_ONLINE, &ret);
		if (err) {
			pr_err("usb online read error:%d\n", err);
			return;
		}
		usb_state.usb_online = ret.intval;
		if (mi_thermal_dev.dev)
			sysfs_notify(&mi_thermal_dev.dev->kobj, NULL,
				     "usb_online");
	}
}
#endif

static int of_parse_thermal_message(void)
{
	struct device_node *np;

	np = of_find_node_by_name(NULL, "thermal-message");
	if (!np)
		return -EINVAL;

	if (of_property_read_string(np, "board-sensor", &board_sensor))
		return -EINVAL;

	pr_info("%s board sensor: %s\n", __func__, board_sensor);

	return 0;
}

static struct attribute *mi_thermal_dev_attr_group[] = {
	&dev_attr_temp_state.attr,
	&dev_attr_cpu_limits.attr,
	&dev_attr_sconfig.attr,
	&dev_attr_screen_state.attr,
	&dev_attr_boost.attr,
	&dev_attr_board_sensor.attr,
	&dev_attr_board_sensor_temp.attr,
	&dev_attr_board_sensor_second_temp.attr,
	&dev_attr_board_sensor_other_temp.attr,
	&dev_attr_balance_mode.attr,
	&dev_attr_modem_limit.attr,
	&dev_attr_wifi_limit.attr,
	&dev_attr_flash_state.attr,
	&dev_attr_thermal_max_brightness.attr,
	&dev_attr_market_download_limit.attr,
	&dev_attr_board_sensor_temp_comp.attr,
	&dev_attr_poor_modem_limit.attr,
	&dev_attr_cpu_nolimit_temp.attr,
	&dev_attr_modem_rate.attr,
	&dev_attr_modem_level.attr,
	&dev_attr_ambient_sensor.attr,
	&dev_attr_ambient_sensor_temp.attr,
#ifdef CONFIG_HAVE_CHARGE_TEMP
	&dev_attr_board_sensor_charge_temp.attr,
#endif
#ifdef CONFIG_MI_THERMAL_MULTI_CHARGE
	&dev_attr_charger_temp.attr,
	&dev_attr_usb_online.attr,
#endif
	NULL,
};

static const char *get_screen_state_name(int mode)
{
	switch (mode) {
	case DRM_PANEL_EVENT_UNBLANK:
		return "On";
	case DRM_PANEL_EVENT_BLANK_LP:
		return "Doze";
	case DRM_PANEL_EVENT_BLANK:
		return "Off";
	default:
		return "Unknown";
	}
}

static void
screen_state_for_thermal_callback(enum panel_event_notifier_tag tag,
				  struct panel_event_notification *notification,
				  void *client_data)
{
	if (!notification) {
		pr_err("%s:Invalid notification\n", __func__);
		return;
	}

	if (notification->notif_data.early_trigger) {
		return;
	}
	if (tag == PANEL_EVENT_NOTIFICATION_PRIMARY) {
		switch (notification->notif_type) {
		case DRM_PANEL_EVENT_UNBLANK:
			screen_light = screen_light | 0x1;
			break;
		case DRM_PANEL_EVENT_BLANK:
		case DRM_PANEL_EVENT_BLANK_LP:
			screen_light = screen_light & 0x2;
			break;
		case DRM_PANEL_EVENT_FPS_CHANGE:
			return;
		default:
			return;
		}
		pr_err("%s: %s, screen_light = %d, %s\n", __func__,
		       get_screen_state_name(notification->notif_type),
		       screen_light, board_sensor_temp);
	}
#if IS_ENABLED(CONFIG_HAVE_MULTI_SCREEN)
	else if (tag == PANEL_EVENT_NOTIFICATION_SECONDARY) {
		switch (notification->notif_type) {
		case DRM_PANEL_EVENT_UNBLANK:
			screen_light = screen_light | 0x2;
			break;
		case DRM_PANEL_EVENT_BLANK:
		case DRM_PANEL_EVENT_BLANK_LP:
			screen_light = screen_light & 0x1;
			break;
		case DRM_PANEL_EVENT_FPS_CHANGE:
			return;
		default:
			return;
		}
		pr_err("%s: %s, sencondery_screen_light = %d, %s\n", __func__,
		       get_screen_state_name(notification->notif_type),
		       screen_light, board_sensor_temp);
	}
#endif
	if (screen_light) {
		screen_state = 1;
		pr_err("%s: screen_light = %d, so screen_state = %d, %s\n",
		       __func__, screen_light, screen_state, board_sensor_temp);
	} else {
		screen_state = 0;
		pr_err("%s: screen_light = %d, so screen_state = %d, %s\n",
		       __func__, screen_light, screen_state, board_sensor_temp);
	}
	if (screen_last_status != screen_state) {
		sysfs_notify(&mi_thermal_dev.dev->kobj, NULL, "screen_state");
		screen_last_status = screen_state;
	}
}


static int thermal_check_panel(struct device_node *np)
{
	int i, count;
	struct device_node *node;
	struct drm_panel *panel;

	prim_panel = NULL;
	count = of_count_phandle_with_args(np, "panel", NULL);
	if (count <= 0)
		return count ? count : -ENODEV;

	for (i = 0; i < count; i++) {
		node = of_parse_phandle(np, "panel", i);
		if (!node)
			continue;
		panel = of_drm_find_panel(node);
		of_node_put(node);
		if (!IS_ERR_OR_NULL(panel)) {
			prim_panel = panel;
			return 0;
		}
	}
	return -EPROBE_DEFER;
}



static int create_thermal_message_node(void)
{
	struct kernfs_node *class_sd = NULL, *thermal_sd = NULL;
	struct kobject *class_kobj;
	struct subsys_private *cp;
	struct class *cls;
	int ret = -ENODEV;

	if (!kernel_kobj || !kernel_kobj->sd || !kernel_kobj->sd->parent)
		return -ENODEV;

	class_sd = kernfs_find_and_get(kernel_kobj->sd->parent, "class");
	if (!class_sd)
		return -ENODEV;
	thermal_sd = kernfs_find_and_get(class_sd, "thermal");
	if (!thermal_sd)
		goto put_nodes;

	class_kobj = thermal_sd->priv;
	if (!class_kobj)
		goto put_nodes;
	cp = to_subsys_private(class_kobj);
	cls = cp->class;
	if (!cls)
		goto put_nodes;

	mi_thermal_dev.class = cls;
	mi_thermal_dev.dev = device_create(cls, NULL, 'H', NULL, "thermal_message");
	if (IS_ERR_OR_NULL(mi_thermal_dev.dev)) {
		ret = mi_thermal_dev.dev ? PTR_ERR(mi_thermal_dev.dev) : -ENOMEM;
		mi_thermal_dev.dev = NULL;
		mi_thermal_dev.class = NULL;
		goto put_nodes;
	}

	mi_thermal_dev.attrs.attrs = mi_thermal_dev_attr_group;
	ret = sysfs_create_group(&mi_thermal_dev.dev->kobj, &mi_thermal_dev.attrs);
	if (ret) {
		device_destroy(cls, 'H');
		mi_thermal_dev.dev = NULL;
		mi_thermal_dev.class = NULL;
	}

put_nodes:
	if (thermal_sd)
		kernfs_put(thermal_sd);
	kernfs_put(class_sd);
	return ret;
}



static void destroy_thermal_message_node(void)
{
	if (!mi_thermal_dev.dev)
		return;
	sysfs_remove_group(&mi_thermal_dev.dev->kobj, &mi_thermal_dev.attrs);
	device_destroy(mi_thermal_dev.class, 'H');
	mi_thermal_dev.dev = NULL;
	mi_thermal_dev.class = NULL;
}



static void screen_state_check(struct work_struct *work)
{
#if defined(CONFIG_OF) && defined(CONFIG_DRM_PANEL)
	struct device_node *node;
	static int retry_count = 10;
	void *registered;
	int ret;

	if (cookie || !mi_thermal_dev.dev)
		return;

	node = of_find_node_by_name(NULL, "thermal-screen");
	if (!node) {
		pr_err("%s: thermal-screen node not found\n", __func__);
		return;
	}
	ret = thermal_check_panel(node);
	of_node_put(node);

	if (!ret && prim_panel) {
		registered = panel_event_notifier_register(
			PANEL_EVENT_NOTIFICATION_PRIMARY,
			PANEL_EVENT_NOTIFIER_CLIENT_THERMAL,
			prim_panel, screen_state_for_thermal_callback, NULL);
		if (!IS_ERR_OR_NULL(registered)) {
			cookie = registered;
			return;
		}
		pr_err("%s: panel notifier registration failed: %ld\n",
		       __func__, registered ? PTR_ERR(registered) : -ENODEV);
	}

	if (retry_count-- > 0)
		queue_delayed_work(screen_state_wq, &screen_state_dw, 5 * HZ);
#endif
}



static unsigned char temp_aware_mode = 10;
static struct class *power_debug_class;

static ssize_t temp_aware_show(struct class *class,
		struct class_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", READ_ONCE(temp_aware_mode));
}

static ssize_t temp_aware_store(struct class *class,
		struct class_attribute *attr, const char *buf, size_t count)
{
	unsigned long long value;

	if (kstrtoull(buf, 10, &value))
		return -EINVAL;
	if (value >= 1 && value <= 9)
		WRITE_ONCE(temp_aware_mode, (unsigned char)value);
	return count;
}

static CLASS_ATTR_RW(temp_aware);

static int power_debug_init(void)
{
	int ret;

	power_debug_class = class_create(THIS_MODULE, "power_debug");
	if (IS_ERR(power_debug_class)) {
		ret = PTR_ERR(power_debug_class);
		power_debug_class = NULL;
		return ret;
	}

	/* Reference mode is 0664. */
	class_attr_temp_aware.attr.mode = 0664;
	ret = class_create_file(power_debug_class, &class_attr_temp_aware);
	if (ret) {
		class_destroy(power_debug_class);
		power_debug_class = NULL;
	}
	return ret;
}

static void power_debug_exit(void)
{
	if (!power_debug_class)
		return;
	class_remove_file(power_debug_class, &class_attr_temp_aware);
	class_destroy(power_debug_class);
	power_debug_class = NULL;
}


static int __init mi_thermal_interface_init(void)
{
	int ret;

	ret = cpu_thermal_init();
	if (ret)
		return ret;

	ret = of_parse_thermal_message();
	if (ret)
		pr_err("%s: thermal-message parsing failed: %d\n", __func__, ret);

	ret = create_thermal_message_node();
	if (ret)
		goto destroy_cpu;

	ret = power_debug_init();
	if (ret)
		goto destroy_node;

#if defined(CONFIG_OF) && defined(CONFIG_DRM_PANEL)
	screen_state_wq = create_singlethread_workqueue("screen_state_wq");
	if (!screen_state_wq) {
		ret = -ENOMEM;
		goto destroy_debug;
	}
	INIT_DELAYED_WORK(&screen_state_dw, screen_state_check);
	queue_delayed_work(screen_state_wq, &screen_state_dw, 5 * HZ);
#endif
	return 0;

#if defined(CONFIG_OF) && defined(CONFIG_DRM_PANEL)
destroy_debug:
	power_debug_exit();
#endif
destroy_node:
	destroy_thermal_message_node();
destroy_cpu:
	destory_thermal_cpu();
	return ret;
}




module_init(mi_thermal_interface_init);
/* No module_exit: panel and charging notifier callbacks remain resident. */

MODULE_AUTHOR("Xiaomi thermal team");
MODULE_DESCRIPTION("Xiaomi thermal control  interface");
MODULE_LICENSE("GPL v2");

#if IS_ENABLED(CONFIG_HAVE_MULTI_SCREEN) || defined(CONFIG_MI_THERMAL_MULTI_CHARGE) || defined(CONFIG_HAVE_CHARGE_TEMP)
#error "Review optional multi-screen/charging support before enabling it for this Spring candidate"
#endif
