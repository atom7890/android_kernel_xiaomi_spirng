
/*************************************************************************
 @Author: liuqi
 @Created Time : Fri 12 Apr 2024 03:19:00 PM CST
 @File Name: wifi_ant_check.c
 @Description:
 ************************************************************************/
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <net/sock.h>
#include <net/netlink.h>
#include <linux/pm_wakeirq.h>
#include <linux/pm_wakeup.h>
#include <net/genetlink.h>
#include <linux/nl80211.h>
#include <linux/netdevice.h>
#define DEVICE_NAME "wifi_ant_check"
#define NETLINK_WIFI_SAR_LIMIT 26
#define MAX_MSGSIZE 1024
#define LC_WIFI_SAR_LIMIT_NETLINK_PORTID 100
#define DSI_HEAD "1"
#define DSI_BODY "1"
#define DSI_ANT "2"
static struct class *wifi_ant_check_class;
static struct device *class_dev;
static int gpio_pin;
static bool gpio_state = false;
static bool enable_state;
static int irq_no;
struct pm_data
{
	struct device *dev;
	int debounce_time;
	struct delayed_work debounce_work;
	struct mutex wake_lock;
	bool awake;
};
struct sock *nl_sk = NULL;
static int send_sar_enable_vendor_cmd(int pid, char *choice)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	size_t len;
	int ret;

	if (!choice || !nl_sk)
		return -ENOTCONN;

	len = strlen(choice);
	skb = nlmsg_new(len, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, len, 0);
	if (!nlh) {
		kfree_skb(skb);
		return -EMSGSIZE;
	}

	NETLINK_CB(skb).portid = 0;
	NETLINK_CB(skb).dst_group = 0;
	memcpy(nlmsg_data(nlh), choice, len);

	/* netlink_unicast consumes skb, including on failure. */
	ret = netlink_unicast(nl_sk, skb, pid, MSG_DONTWAIT);
	return ret < 0 ? ret : 0;
}

static void recv_msg_from_userspace(struct sk_buff *skb)
{
	if (!skb || !nlmsg_ok(nlmsg_hdr(skb), skb->len))
		return;

	pr_info("[WIFI_ANT_CHECK] Client port=%u connected\n",
		NETLINK_CB(skb).portid);
}


static void wifi_pm_relax(struct pm_data *data)
{
	mutex_lock(&data->wake_lock);
	/* An IRQ may have queued another debounce pass while this ran. */
	if (!delayed_work_pending(&data->debounce_work) && data->awake) {
		pm_relax(data->dev);
		data->awake = false;
	}
	mutex_unlock(&data->wake_lock);
}

static void gpio_debounce_work(struct work_struct *work)
{
	int ret = -1;
	struct pm_data *pm_data =
	    container_of(work, struct pm_data, debounce_work.work);
	gpio_state = gpio_get_value(gpio_pin);
	pr_info("GPIO interrupt occurred. New state: %d\n", gpio_state);
	// irq continue if enable_state == true
	if (READ_ONCE(enable_state))
	{
		// send msg to wifi driver.
		pr_info("[WIFI_ANT_CHECK] %s-%d: The wifi ant check function has been enabled.\n", __func__, __LINE__);
		if (1 == gpio_state)
		{
			ret = send_sar_enable_vendor_cmd(LC_WIFI_SAR_LIMIT_NETLINK_PORTID, DSI_ANT);
		}
		else
		{
			ret = send_sar_enable_vendor_cmd(LC_WIFI_SAR_LIMIT_NETLINK_PORTID, DSI_HEAD);
		}
	}
	else
	{
		pr_err("[WIFI_ANT_CHECK] %s-%d: The wifi ant check function has been disabled.\n", __func__, __LINE__);
	}
	pr_info("[WIFI_ANT_CHECK] %s-%d: Irq handler func return: %d\n", __func__, __LINE__, ret);
	wifi_pm_relax(pm_data);
}

static irqreturn_t irq_handler_func(int irq, void *irq_data)
{
	struct pm_data *data = irq_data;

	/* This is the threaded IRQ callback, so a mutex is permitted. */
	mutex_lock(&data->wake_lock);
	if (!data->awake) {
		pm_stay_awake(data->dev);
		data->awake = true;
	}
	mod_delayed_work(system_wq, &data->debounce_work,
			 msecs_to_jiffies(data->debounce_time));
	mutex_unlock(&data->wake_lock);
	return IRQ_HANDLED;
}

// read GPIO status
static ssize_t gpio_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", gpio_state);
}
// read enable status
static ssize_t enable_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", enable_state);
}
// write enable status
static ssize_t enable_state_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int val;
	int ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	WRITE_ONCE(enable_state, val != 0);
	return count;
}

// define device attributes
static DEVICE_ATTR(gpio_state, S_IRUGO, gpio_state_show, NULL);
static DEVICE_ATTR(enable_state, S_IRUGO|S_IWUSR, enable_state_show, enable_state_store);
struct netlink_kernel_cfg cfg =
{
	.input = recv_msg_from_userspace,
};
static bool spring_wifi_ant_enabled(void)
{
	struct device_node *chosen;
	const char *bootargs;
	bool enabled = false;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return false;

	if (!of_property_read_string(chosen, "bootargs", &bootargs))
		enabled = strstr(bootargs, "wlancmdline.hwc=CN") != NULL;

	of_node_put(chosen);
	return enabled;
}


static int wifi_ant_check_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct pm_data *data;
	u32 debounce;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	mutex_init(&data->wake_lock);
	INIT_DELAYED_WORK(&data->debounce_work, gpio_debounce_work);
	WRITE_ONCE(enable_state, spring_wifi_ant_enabled());

	gpio_pin = of_get_named_gpio(np, "gpio", 0);
	if (gpio_pin < 0)
		return gpio_pin;

	if (of_property_read_u32(np, "debounce-time", &debounce))
		debounce = 50;
	if (debounce > 0x7fffffffU)
		return -EINVAL;
	data->debounce_time = debounce;

	ret = devm_gpio_request(dev, gpio_pin, DEVICE_NAME);
	if (ret)
		return ret;

	gpio_state = !!gpio_get_value(gpio_pin);
	irq_no = gpio_to_irq(gpio_pin);
	if (irq_no < 0)
		return irq_no;

	nl_sk = netlink_kernel_create(&init_net, NETLINK_WIFI_SAR_LIMIT, &cfg);
	if (!nl_sk)
		return -ENOMEM;

	wifi_ant_check_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(wifi_ant_check_class)) {
		ret = PTR_ERR(wifi_ant_check_class);
		goto err_netlink;
	}

	class_dev = device_create(wifi_ant_check_class, NULL, 0, NULL,
				  DEVICE_NAME);
	if (IS_ERR(class_dev)) {
		ret = PTR_ERR(class_dev);
		goto err_class;
	}

	ret = device_create_file(class_dev, &dev_attr_gpio_state);
	if (ret)
		goto err_device;
	ret = device_create_file(class_dev, &dev_attr_enable_state);
	if (ret)
		goto err_gpio_attr;

	ret = device_init_wakeup(dev, true);
	if (ret)
		goto err_enable_attr;
	ret = dev_pm_set_wake_irq(dev, irq_no);
	if (ret)
		goto err_wakeup;

	platform_set_drvdata(pdev, data);
	/* Register last: all resources used by the handler are now ready. */
	ret = devm_request_threaded_irq(dev, irq_no, NULL, irq_handler_func,
			IRQF_ONESHOT | IRQF_TRIGGER_RISING |
			IRQF_TRIGGER_FALLING, DEVICE_NAME, data);
	if (ret)
		goto err_wake_irq;

	return 0;

err_wake_irq:
	platform_set_drvdata(pdev, NULL);
	dev_pm_clear_wake_irq(dev);
err_wakeup:
	device_init_wakeup(dev, false);
err_enable_attr:
	device_remove_file(class_dev, &dev_attr_enable_state);
err_gpio_attr:
	device_remove_file(class_dev, &dev_attr_gpio_state);
err_device:
	device_unregister(class_dev);
err_class:
	class_destroy(wifi_ant_check_class);
err_netlink:
	netlink_kernel_release(nl_sk);
	nl_sk = NULL;
	return ret;
}


static int wifi_ant_check_remove(struct platform_device *pdev)
{
	struct pm_data *data = platform_get_drvdata(pdev);

	if (!data)
		return 0;

	/* Stop and synchronize the IRQ before draining its queued work. */
	devm_free_irq(&pdev->dev, irq_no, data);
	cancel_delayed_work_sync(&data->debounce_work);

	mutex_lock(&data->wake_lock);
	if (data->awake) {
		pm_relax(data->dev);
		data->awake = false;
	}
	mutex_unlock(&data->wake_lock);

	dev_pm_clear_wake_irq(&pdev->dev);
	device_init_wakeup(&pdev->dev, false);
	device_remove_file(class_dev, &dev_attr_enable_state);
	device_remove_file(class_dev, &dev_attr_gpio_state);
	device_unregister(class_dev);
	class_destroy(wifi_ant_check_class);
	netlink_kernel_release(nl_sk);
	nl_sk = NULL;
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static const struct of_device_id wifi_ant_check_dt_ids[] =
{
	{ .compatible = "wifi_ant_check", },
	{ /* sentinel */ },
};
static struct platform_driver wifi_ant_check_driver =
{
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = of_match_ptr(wifi_ant_check_dt_ids),
	},
	.probe = wifi_ant_check_probe,
	.remove = wifi_ant_check_remove,
};
module_platform_driver(wifi_ant_check_driver);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("iliuqi");
MODULE_DESCRIPTION("GPIO Interrupt Driver which use by wifi ant check!");

