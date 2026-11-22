// SPDX-License-Identifier: GPL-2.0-only
/* ColorOS OPD2513 ufcsd mailbox, following the OEM oplus_chg_ufcs ABI. */
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>

#include "ufcs_class.h"

#define GET_AUTH_DATA 1
#define NOTIFY_AUTH_DATA _IOW(0x66, 1, char)
#define AUTH_DATA_SIZE 16

struct oem_command {
	__le32 cmd;
	__le32 size;
	u8 data[128];
};

static DEFINE_MUTEX(auth_lock);
static DEFINE_MUTEX(verify_lock);
static u8 auth_data[AUTH_DATA_SIZE];
static bool auth_ready;
static bool daemon_open;
static enum { REQUEST, RESPONSE, EXIT_SENT } daemon_state;

int opd2513_ufcs_verify(struct ufcs_dev *ufcs);

/* The protocol driver calls the OEM verifier without exporting credentials. */
int opd2513_ufcs_verify(struct ufcs_dev *ufcs)
{
	u8 data[AUTH_DATA_SIZE];
	int ret;

	mutex_lock(&auth_lock);
	if (!auth_ready) {
		mutex_unlock(&auth_lock);
		return -ENODATA;
	}
	memcpy(data, auth_data, sizeof(data));
	mutex_unlock(&auth_lock);
	mutex_lock(&verify_lock);
	ret = ufcs_intf_verify_adapter(ufcs, 1, data, sizeof(data));
	mutex_unlock(&verify_lock);
	memzero_explicit(data, sizeof(data));
	return ret;
}
EXPORT_SYMBOL_GPL(opd2513_ufcs_verify);

static int auth_open(struct inode *inode, struct file *file)
{
	int ret = 0;

	mutex_lock(&auth_lock);
	if (daemon_open || auth_ready) {
		ret = -EBUSY;
	} else {
		ret = nonseekable_open(inode, file);
		if (!ret) {
			daemon_open = true;
			daemon_state = REQUEST;
		}
	}
	mutex_unlock(&auth_lock);
	return ret;
}

static int auth_release(struct inode *inode, struct file *file)
{
	mutex_lock(&auth_lock);
	daemon_open = false;
	mutex_unlock(&auth_lock);
	return 0;
}

static ssize_t auth_read(struct file *file, char __user *buf, size_t count,
			 loff_t *pos)
{
	struct oem_command command = {};
	ssize_t ret;

	if (count < sizeof(command))
		return -EINVAL;
	mutex_lock(&auth_lock);
	if (daemon_state == REQUEST) {
		command.cmd = cpu_to_le32(GET_AUTH_DATA);
		command.size = cpu_to_le32(sizeof(__le32));
	} else if (daemon_state == EXIT_SENT || !auth_ready) {
		ret = daemon_state == EXIT_SENT ? 0 : -EAGAIN;
		goto out;
	}
	if (copy_to_user(buf, &command, sizeof(command))) {
		ret = -EFAULT;
		goto out;
	}
	daemon_state = daemon_state == REQUEST ? RESPONSE : EXIT_SENT;
	ret = sizeof(command);
out:
	mutex_unlock(&auth_lock);
	return ret;
}

static long auth_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	u8 data[AUTH_DATA_SIZE] = {};
	int ret = 0;

	if (cmd != NOTIFY_AUTH_DATA)
		return -ENOTTY;
	mutex_lock(&auth_lock);
	if (daemon_state != RESPONSE) {
		ret = -EPROTO;
	} else if (auth_ready) {
		ret = -EALREADY;
	} else if (copy_from_user(data, (void __user *)arg, sizeof(data))) {
		ret = -EFAULT;
	} else {
		memcpy(auth_data, data, sizeof(data));
		WRITE_ONCE(auth_ready, true);
	}
	mutex_unlock(&auth_lock);
	memzero_explicit(data, sizeof(data));
	return ret;
}

static const struct file_operations auth_fops = {
	.owner = THIS_MODULE,
	.open = auth_open,
	.release = auth_release,
	.read = auth_read,
	.unlocked_ioctl = auth_ioctl,
};

static struct miscdevice auth_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ufcs_dev",
	.fops = &auth_fops,
	.mode = 0600,
};

static ssize_t ready_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(auth_ready));
}
static DEVICE_ATTR_RO(ready);
static struct attribute *auth_attrs[] = { &dev_attr_ready.attr, NULL };
ATTRIBUTE_GROUPS(auth);

static int __init auth_init(void)
{
	auth_device.groups = auth_groups;
	return misc_register(&auth_device);
}
module_init(auth_init);

static void __exit auth_exit(void)
{
	misc_deregister(&auth_device);
	memzero_explicit(auth_data, sizeof(auth_data));
}
module_exit(auth_exit);

MODULE_DESCRIPTION("OPD2513 ColorOS UFCS authentication daemon interface");
MODULE_LICENSE("GPL");
