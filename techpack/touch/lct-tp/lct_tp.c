// SPDX-License-Identifier: GPL-2.0
/* Spring LCT touch interface reconstruction from the retained module's
 * DWARF declarations, disassembly and proc interface. No controller code.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>
#include <linux/string.h>
#include "lct_tp.h"

enum lct_id {
 LCT_SELFTEST, LCT_INFO, LCT_LOCKDOWN, LCT_WORK,
 LCT_PROX_DATA, LCT_PROX_SWITCH, LCT_GRIP, LCT_GESTURE, LCT_COUNT
};
union lct_callback {
 tp_selftest_callback_t selftest;
 int (*info)(const char *);
 int (*lockdown)(void);
 int (*toggle)(bool);
 struct {
  set_screen_angle_callback set;
  get_screen_angle_callback get;
 } grip;
};
struct lct_node {
 const char *name;
 umode_t mode;
 enum lct_id id;
 struct mutex registration_lock;
 struct mutex operation_lock;
 spinlock_t buffer_lock;
 struct proc_dir_entry *pde;
 bool active;
 int value;
 char text[128];
 union lct_callback callback;
 bool selftest_pending;
 unsigned char selftest_command;
 char selftest_result[8];
};
static struct lct_node nodes[LCT_COUNT] = {
 [LCT_SELFTEST] = { .name = "tp_selftest", .mode = 0666 },
 [LCT_INFO] = { .name = "tp_info", .mode = 0444 },
 [LCT_LOCKDOWN] = { .name = "tp_lockdown_info", .mode = 0444 },
 [LCT_WORK] = { .name = "tp_work", .mode = 0444 },
 [LCT_PROX_DATA] = { .name = "tp_proximity_data", .mode = 0666 },
 [LCT_PROX_SWITCH] = { .name = "tp_proximity_switch", .mode = 0666 },
 [LCT_GRIP] = { .name = "tp_grip_area", .mode = 0666 },
 [LCT_GESTURE] = { .name = "tp_gesture", .mode = 0444 },
};
DEFINE_STATIC_SRCU(lct_callback_readers);
static const struct proc_ops lct_proc_ops;

static void lct_update_text(struct lct_node *node, const char *text)
{
 unsigned long flags;
 if (!text || !READ_ONCE(node->active))
  return;
 spin_lock_irqsave(&node->buffer_lock, flags);
 strscpy(node->text, text, sizeof(node->text));
 spin_unlock_irqrestore(&node->buffer_lock, flags);
}

static int lct_register(enum lct_id id, union lct_callback callback,
   int initial_value, const char *initial_text)
{
 struct lct_node *node = &nodes[id];
 int ret = 0;
 mutex_lock(&node->registration_lock);
 if (node->pde) {
  ret = -EBUSY;
  goto out;
 }
 /* No old proc callback remains after lct_unregister() has returned. */
 node->callback = callback;
 node->value = initial_value;
 memset(node->text, 0, sizeof(node->text));
 if (initial_text)
  strscpy(node->text, initial_text, sizeof(node->text));
 node->selftest_pending = false;
 node->selftest_command = 0;
 node->selftest_result[0] = '\0';
 WRITE_ONCE(node->active, true);
 node->pde = proc_create_data(node->name, node->mode, NULL,
         &lct_proc_ops, node);
 if (!node->pde) {
  WRITE_ONCE(node->active, false);
  memset(&node->callback, 0, sizeof(node->callback));
  ret = -ENOMEM;
 }
out:
 mutex_unlock(&node->registration_lock);
 return ret;
}

static void lct_unregister(enum lct_id id)
{
 struct lct_node *node = &nodes[id];
 mutex_lock(&node->registration_lock);
 WRITE_ONCE(node->active, false);
 /* No buffer/operation lock held: the controller callback may publish state. */
 if (node->pde) {
  proc_remove(node->pde);
  node->pde = NULL;
 }
 synchronize_srcu(&lct_callback_readers);
 memset(&node->callback, 0, sizeof(node->callback));
 node->selftest_pending = false;
 mutex_unlock(&node->registration_lock);
}

static ssize_t lct_proc_read(struct file *file, char __user *buf,
       size_t count, loff_t *pos)
{
 struct lct_node *node = pde_data(file_inode(file));
 char result[128];
 unsigned long flags;
 int idx, ret = 0, length = 0, value, attempt;
 tp_selftest_callback_t selftest;
 int (*info)(const char *);
 int (*lockdown)(void);
 get_screen_angle_callback get_angle;

 if (!node)
  return -ENODEV;
 if (*pos < 0)
  return -EINVAL;
 if (!count)
  return 0;
 /* Each record ends at EOF; readers rewind before polling again. */
 if (*pos)
  return 0;
 mutex_lock(&node->operation_lock);
 idx = srcu_read_lock(&lct_callback_readers);
 if (!READ_ONCE(node->active)) {
  ret = -ENODEV;
  goto out;
 }
 switch (node->id) {
 case LCT_SELFTEST:
  if (node->selftest_pending) {
   selftest = READ_ONCE(node->callback.selftest);
   if (selftest) {
    value = 0;
    for (attempt = 0; attempt < 3; attempt++) {
     value = selftest(node->selftest_command);
     if (value != 1)
      break;
    }
    scnprintf(node->selftest_result, sizeof(node->selftest_result),
        "%d\n", value == 2 ? 2 : value == 1 ? 1 : 0);
   }
   node->selftest_pending = false;
  }
  length = scnprintf(result, sizeof(result), "%s", node->selftest_result);
  break;
 case LCT_INFO:
 case LCT_LOCKDOWN:
  if (node->id == LCT_INFO) {
   info = READ_ONCE(node->callback.info);
   if (info)
    info("CMD_INFO");
  } else {
   lockdown = READ_ONCE(node->callback.lockdown);
   if (lockdown)
    lockdown();
  }
  spin_lock_irqsave(&node->buffer_lock, flags);
  strscpy(result, node->text[0] ? node->text : "Unknown touchpad",
    sizeof(result));
  spin_unlock_irqrestore(&node->buffer_lock, flags);
  length = strnlen(result, sizeof(result));
  break;
 case LCT_GRIP:
  get_angle = READ_ONCE(node->callback.grip.get);
  value = get_angle ? get_angle() : READ_ONCE(node->value);
  if (value < 0) {
   ret = -EIO;
   goto out;
  }
  WRITE_ONCE(node->value, value);
  length = scnprintf(result, sizeof(result), "%d\n", value);
  break;
 case LCT_PROX_DATA:
  length = scnprintf(result, sizeof(result), "%d\n", READ_ONCE(node->value));
  break;
 default:
  length = scnprintf(result, sizeof(result), "%d\n", !!READ_ONCE(node->value));
  break;
 }
 ret = simple_read_from_buffer(buf, count, pos, result, length);
out:
 srcu_read_unlock(&lct_callback_readers, idx);
 mutex_unlock(&node->operation_lock);
 return ret;
}

static ssize_t lct_proc_write(struct file *file, const char __user *buf,
        size_t count, loff_t *pos)
{
 struct lct_node *node = pde_data(file_inode(file));
 char command[128];
 unsigned int number;
 int idx, ret = 0, angle;
 bool enable;
 int (*toggle)(bool);
 set_screen_angle_callback set_angle;

 if (!node)
  return -ENODEV;
 if (!count)
  return 0;
 if (count >= sizeof(command) || (node->id == LCT_SELFTEST && count > 64))
  return -E2BIG;
 if (*pos < 0)
  return -EINVAL;
 if (copy_from_user(command, buf, count))
  return -EFAULT;
 command[count] = '\0';
 mutex_lock(&node->operation_lock);
 idx = srcu_read_lock(&lct_callback_readers);
 if (!READ_ONCE(node->active)) {
  ret = -ENODEV;
  goto out;
 }
 if (node->id == LCT_SELFTEST) {
  if (node->selftest_pending) {
   ret = -EBUSY;
   goto out;
  }
  if (!strncmp(command, "mmi", 3))
   node->selftest_command = 0;
  else if (!strncmp(command, "i2c", 3))
   node->selftest_command = 1;
  else if (!strncmp(command, "open", 4))
   node->selftest_command = 2;
  else if (!strncmp(command, "short", 5))
   node->selftest_command = 3;
  else
   goto out; /* Reference consumes an unknown command without running a test. */
  node->selftest_pending = true;
  goto out;
 }
 if (node->id == LCT_GRIP) {
  if (kstrtoint(strim(command), 10, &angle) || angle < 0 || angle > 360) {
   ret = -EINVAL;
   goto out;
  }
  set_angle = READ_ONCE(node->callback.grip.set);
  if (!set_angle || set_angle(angle) < 0) {
   ret = -EIO;
   goto out;
  }
  WRITE_ONCE(node->value, angle);
  goto out;
 }
 if (kstrtouint(strim(command), 10, &number)) {
  ret = -EINVAL;
  goto out;
 }
 if (node->id == LCT_PROX_DATA) {
  /* Binary verified: a valid numeric proc write acknowledges/reset data to 0. */
  WRITE_ONCE(node->value, 0);
  goto out;
 }
 if (node->id != LCT_WORK && node->id != LCT_PROX_SWITCH && node->id != LCT_GESTURE) {
  ret = -EACCES;
  goto out;
 }
 enable = !!number;
 if (enable != !!READ_ONCE(node->value)) {
  toggle = READ_ONCE(node->callback.toggle);
  if (!toggle || toggle(enable) < 0) {
   ret = -EIO;
   goto out;
  }
  WRITE_ONCE(node->value, enable);
 }
out:
 srcu_read_unlock(&lct_callback_readers, idx);
 mutex_unlock(&node->operation_lock);
 if (ret)
  return ret;
 if (node->id != LCT_SELFTEST)
  *pos += count;
 return count;
}

static const struct proc_ops lct_proc_ops = {
 .proc_lseek = default_llseek,
 .proc_read = lct_proc_read,
 .proc_write = lct_proc_write,
};

int init_lct_tp_selftest(tp_selftest_callback_t callback)
{
 union lct_callback cb = { .selftest = callback };
 return lct_register(LCT_SELFTEST, cb, 0, NULL);
}
EXPORT_SYMBOL(init_lct_tp_selftest);
void uninit_lct_tp_selftest(void) { lct_unregister(LCT_SELFTEST); }
EXPORT_SYMBOL(uninit_lct_tp_selftest);

void set_lct_tp_info_callback(int (*pfun)(const char *))
{
 if (READ_ONCE(nodes[LCT_INFO].active))
  WRITE_ONCE(nodes[LCT_INFO].callback.info, pfun);
}
EXPORT_SYMBOL(set_lct_tp_info_callback);
void set_lct_tp_lockdown_info_callback(int (*pfun)(void))
{
 if (READ_ONCE(nodes[LCT_LOCKDOWN].active))
  WRITE_ONCE(nodes[LCT_LOCKDOWN].callback.lockdown, pfun);
}
EXPORT_SYMBOL(set_lct_tp_lockdown_info_callback);
void update_lct_tp_info(char *tp_info_buf) { lct_update_text(&nodes[LCT_INFO], tp_info_buf); }
EXPORT_SYMBOL(update_lct_tp_info);
void update_lct_tp_lockdown_info(char *tp_lockdown_info_buf)
{
 lct_update_text(&nodes[LCT_LOCKDOWN], tp_lockdown_info_buf);
}
EXPORT_SYMBOL(update_lct_tp_lockdown_info);
int init_lct_tp_info(char *tp_info_buf)
{
 union lct_callback cb = { .info = NULL };
 return lct_register(LCT_INFO, cb, 0, tp_info_buf);
}
EXPORT_SYMBOL(init_lct_tp_info);
int init_lct_tp_lockdown_info(void)
{
 union lct_callback cb = { .lockdown = NULL };
 return lct_register(LCT_LOCKDOWN, cb, 0, NULL);
}
EXPORT_SYMBOL(init_lct_tp_lockdown_info);
void uninit_lct_tp_info(void) { lct_unregister(LCT_INFO); }
EXPORT_SYMBOL(uninit_lct_tp_info);
void uninit_lct_tp_lockdown_info(void) { lct_unregister(LCT_LOCKDOWN); }
EXPORT_SYMBOL(uninit_lct_tp_lockdown_info);

int init_lct_tp_work(tp_work_cb_t callback)
{
 union lct_callback cb = { .toggle = callback };
 return callback ? lct_register(LCT_WORK, cb, 1, NULL) : -EINVAL;
}
EXPORT_SYMBOL(init_lct_tp_work);
void uninit_lct_tp_work(void) { lct_unregister(LCT_WORK); }
EXPORT_SYMBOL(uninit_lct_tp_work);
void set_lct_tp_work_status(bool en)
{
 if (READ_ONCE(nodes[LCT_WORK].active)) WRITE_ONCE(nodes[LCT_WORK].value, en);
}
EXPORT_SYMBOL(set_lct_tp_work_status);
bool get_lct_tp_work_status(void)
{
 return READ_ONCE(nodes[LCT_WORK].active) && !!READ_ONCE(nodes[LCT_WORK].value);
}
EXPORT_SYMBOL(get_lct_tp_work_status);

int init_lct_tp_proximity_data(void)
{
 union lct_callback cb = { .toggle = NULL };
 return lct_register(LCT_PROX_DATA, cb, 0, NULL);
}
EXPORT_SYMBOL(init_lct_tp_proximity_data);
void uninit_lct_tp_proximity_data(void) { lct_unregister(LCT_PROX_DATA); }
EXPORT_SYMBOL(uninit_lct_tp_proximity_data);
void set_lct_tp_proximity_data(int value)
{
 if (READ_ONCE(nodes[LCT_PROX_DATA].active)) WRITE_ONCE(nodes[LCT_PROX_DATA].value, value);
}
EXPORT_SYMBOL(set_lct_tp_proximity_data);
bool get_lct_tp_proximity_data_data(void)
{
 return READ_ONCE(nodes[LCT_PROX_DATA].active) && !!READ_ONCE(nodes[LCT_PROX_DATA].value);
}
EXPORT_SYMBOL(get_lct_tp_proximity_data_data);

int init_lct_tp_proximity_switch(tp_proximity_switch_cb_t callback)
{
 union lct_callback cb = { .toggle = callback };
 return callback ? lct_register(LCT_PROX_SWITCH, cb, 0, NULL) : -EINVAL;
}
EXPORT_SYMBOL(init_lct_tp_proximity_switch);
void uninit_lct_tp_proximity_switch(void) { lct_unregister(LCT_PROX_SWITCH); }
EXPORT_SYMBOL(uninit_lct_tp_proximity_switch);
void set_lct_tp_proximity_switch_status(bool en)
{
 if (READ_ONCE(nodes[LCT_PROX_SWITCH].active)) WRITE_ONCE(nodes[LCT_PROX_SWITCH].value, en);
}
EXPORT_SYMBOL(set_lct_tp_proximity_switch_status);
bool get_lct_tp_proximity_switch_status(void)
{
 return READ_ONCE(nodes[LCT_PROX_SWITCH].active) && !!READ_ONCE(nodes[LCT_PROX_SWITCH].value);
}
EXPORT_SYMBOL(get_lct_tp_proximity_switch_status);

int init_lct_tp_grip_area(set_screen_angle_callback set_fun, get_screen_angle_callback get_fun)
{
 union lct_callback cb = { .grip = { .set = set_fun, .get = get_fun } };
 return set_fun ? lct_register(LCT_GRIP, cb, 0, NULL) : -1;
}
EXPORT_SYMBOL(init_lct_tp_grip_area);
void uninit_lct_tp_grip_area(void) { lct_unregister(LCT_GRIP); }
EXPORT_SYMBOL(uninit_lct_tp_grip_area);
int set_tp_grip_area_angle(int screen_angle)
{
 if (!READ_ONCE(nodes[LCT_GRIP].active)) return -ENODEV;
 WRITE_ONCE(nodes[LCT_GRIP].value, screen_angle);
 return 0;
}
EXPORT_SYMBOL(set_tp_grip_area_angle);
int get_tp_grip_area_angle(void) { return READ_ONCE(nodes[LCT_GRIP].value); }
EXPORT_SYMBOL(get_tp_grip_area_angle);

int init_lct_tp_gesture(tp_gesture_cb_t callback)
{
 union lct_callback cb = { .toggle = callback };
 return callback ? lct_register(LCT_GESTURE, cb, 0, NULL) : -EINVAL;
}
EXPORT_SYMBOL(init_lct_tp_gesture);
void uninit_lct_tp_gesture(void) { lct_unregister(LCT_GESTURE); }
EXPORT_SYMBOL(uninit_lct_tp_gesture);
void set_lct_tp_gesture_status(bool en)
{
 if (READ_ONCE(nodes[LCT_GESTURE].active)) WRITE_ONCE(nodes[LCT_GESTURE].value, en);
}
EXPORT_SYMBOL(set_lct_tp_gesture_status);
bool get_lct_tp_gesture_status(void)
{
 return READ_ONCE(nodes[LCT_GESTURE].active) && !!READ_ONCE(nodes[LCT_GESTURE].value);
}
EXPORT_SYMBOL(get_lct_tp_gesture_status);

static int __init spring_lct_init(void)
{
 int i;
 for (i = 0; i < LCT_COUNT; i++) {
  nodes[i].id = i;
  mutex_init(&nodes[i].registration_lock);
  mutex_init(&nodes[i].operation_lock);
  spin_lock_init(&nodes[i].buffer_lock);
 }
 pr_info("lct_tp: Spring LCT interface reconstruction initialized\n");
 return 0;
}
static void __exit spring_lct_exit(void)
{
 int i;
 for (i = 0; i < LCT_COUNT; i++) lct_unregister(i);
}
module_init(spring_lct_init);
module_exit(spring_lct_exit);
MODULE_DESCRIPTION("Spring LCT touch interface reconstruction");
MODULE_LICENSE("GPL");
