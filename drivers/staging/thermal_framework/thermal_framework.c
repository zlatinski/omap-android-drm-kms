/*
 * drivers/thermal/thermal_framework.c
 *
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Dan Murphy <DMurphy@ti.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
*/

#include <linux/module.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>

#include <linux/thermal_framework.h>

static LIST_HEAD(thermal_domain_list);
static DEFINE_MUTEX(thermal_domain_list_lock);

/**
 * DOC: Introduction
 * =================
 * The Thermal Framework is designed to be a central location to link
 * temperature sensor drivers, governors and cooling agents together.
 * The principle is to have one temperature sensor to one governor to many
 * cooling agents.  This model allows the governors to impart cooling policies
 * based on the available cooling agents for a specific domain.
 *
 * The temperature sensor device should register to the framework and report
 * the temperature of the current domain for which it reports a
 * temperature measurement.
 *
 * The governor is responsible for imparting the cooling policy for the specific
 * domain.  The governor will be given a list of cooling agents that it can call
 * to cool the domain.
 *
 * The cooling agent's primary responsibility is to perform an operation on the
 * device to cool the domain it is responsible for.
 *
 * The sensor, governor and the cooling agents are linked in the framework
 * via the domain_name in the thermal_dev structure.
*/
/**
 * struct thermal_domain  - Structure that contains the pointers to the
 *		sensor, governor and a list of cooling agents.
 * @domain_name: The domain that the thermal structure represents
 * @node: The list node of the thermal domain
 * @temp_sensor: The domains temperature sensor thermal device pointer.
 * @governor: The domain governor thermal device pointer.
 * @cooling_agents: The domains list of available cooling agents
 *
 */
#define MAX_DOMAIN_NAME_SZ	32
struct thermal_domain {
	char domain_name[MAX_DOMAIN_NAME_SZ];
	struct list_head node;
	struct thermal_dev *temp_sensor;
	struct thermal_dev *governor;
	struct list_head cooling_agents;
};

#ifdef CONFIG_THERMAL_FRAMEWORK_DEBUG
static struct dentry *thermal_dbg;
static struct dentry *thermal_domains_dbg;
static struct dentry *thermal_devices_dbg;

static int thermal_debug_show_domain(struct seq_file *s, void *data)
{
	struct thermal_domain *domain = (struct thermal_domain *)s->private;
	struct thermal_dev *tdev;

	seq_printf(s, "Domain name: %s\n", domain->domain_name);
	seq_printf(s, "Temperature sensor:\n");
	if (domain->temp_sensor) {
		seq_printf(s, "\tName: %s\n", domain->temp_sensor->name);
		seq_printf(s, "\tCurrent temperature: %d\n",
			thermal_device_call(domain->temp_sensor, report_temp));
		mutex_lock(&thermal_domain_list_lock);
		thermal_device_call(domain->temp_sensor, debug_report, s);
		mutex_unlock(&thermal_domain_list_lock);
	}
	seq_printf(s, "Governor:\n");
	if (domain->governor) {
		seq_printf(s, "\tName: %s\n", domain->governor->name);
		mutex_lock(&thermal_domain_list_lock);
		thermal_device_call(domain->temp_sensor, debug_report, s);
		mutex_unlock(&thermal_domain_list_lock);
	}
	seq_printf(s, "Cooling agents:\n");
	list_for_each_entry(tdev, &domain->cooling_agents, node) {
		seq_printf(s, "\tName: %s\n", tdev->name);
		mutex_lock(&thermal_domain_list_lock);
		thermal_device_call(domain->temp_sensor, debug_report, s);
		mutex_unlock(&thermal_domain_list_lock);
	}

	return 0;
}

static int thermal_debug_domain_open(struct inode *inode, struct file *file)
{
	return single_open(file, thermal_debug_show_domain, inode->i_private);
}

static const struct file_operations thermal_debug_fops = {
	.open           = thermal_debug_domain_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = single_release,
};

static int thermal_debug_domain(struct thermal_domain *domain)
{
	return PTR_ERR(debugfs_create_file(domain->domain_name, S_IRUGO,
		thermal_domains_dbg, (void *)domain, &thermal_debug_fops));
}

static int thermal_remove_cooling_action(struct thermal_dev *tdev,
						unsigned int priority)
{
	struct thermal_cooling_action *action, *tmp;

	list_for_each_entry_safe(action, tmp, &tdev->cooling_actions, node) {
		if (action->priority == priority) {
			list_del(&action->node);
			debugfs_remove(action->d);
			kfree(action);
			break;
		}
	}

	return 0;
}

static int thermal_add_action_debug(struct thermal_cooling_action *action,
					struct dentry *d)
{
	char buf[32];

	sprintf(buf, "action_%d", action->priority);

	action->d = debugfs_create_u32(buf, S_IRUGO, d,
					(u32 *)&action->reduction);

	return PTR_ERR(action->d);
}

static int thermal_insert_cooling_action(struct thermal_dev *tdev,
						unsigned int priority,
						unsigned int reduction,
						struct dentry *d)
{
	struct list_head *head = &tdev->cooling_actions;
	struct thermal_cooling_action *action, *new;

	list_for_each_entry(action, &tdev->cooling_actions, node) {
		if (action->priority > priority)
			break;
		else
			head = &action->node;
	}

	new = kzalloc(sizeof(struct thermal_cooling_action), GFP_KERNEL);
	if (!new)
		return -ENOMEM;
	new->priority = priority;
	new->reduction = reduction;
	list_add(&new->node, head);

	return thermal_add_action_debug(new, d);
}

static ssize_t thermal_debug_inject_action_write(struct file *file,
				const char __user *user_buf,
				size_t count, loff_t *ppos)
{
	struct thermal_dev *tdev = file->private_data;
	unsigned int priority;
	int reduction;
	char buf[32];
	ssize_t len;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, user_buf, len))
		return -EFAULT;

	buf[len] = '\0';
	if (sscanf(buf, "%u %d", &priority, &reduction) != 2)
		return -EINVAL;

	/* I know, there is a better way to lock this stuff */
	mutex_lock(&thermal_domain_list_lock);
	if (reduction < 0)
		thermal_remove_cooling_action(tdev, priority);
	else
		thermal_insert_cooling_action(tdev, priority, reduction,
						file->f_path.dentry->d_parent);
	mutex_unlock(&thermal_domain_list_lock);

	return count;
}

static int thermal_debug_inject_action_open(struct inode *inode,
							struct file *file)
{
	file->private_data = inode->i_private;

	return 0;
}

static const struct file_operations inject_action_fops = {
	.write = thermal_debug_inject_action_write,
	.open = thermal_debug_inject_action_open,
	.owner = THIS_MODULE,
	.llseek = default_llseek,
};

static int sensor_get_temperature(void *data, u64 *val)
{
	struct thermal_dev *temp_sensor = (struct thermal_dev *)data;
	struct thermal_domain *domain = temp_sensor->domain;
	int ret;

	ret = thermal_device_call(domain->temp_sensor, report_temp);
	if (ret < 0) {
		pr_err("%s: getting temp is not supported for domain %s\n",
			__func__, domain->domain_name);
		return ret;
	}
	*val = ret;

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(sensor_temp_fops, sensor_get_temperature,
			NULL, "%llu\n");

static void thermal_debug_register_device(struct thermal_dev *tdev)
{
	struct dentry *d;

	d = debugfs_create_dir(tdev->name, thermal_devices_dbg);
	if (IS_ERR(d))
		return;

	/* Am I a cooling device ? */
	if (tdev->dev_ops && tdev->dev_ops->cool_device) {
		struct thermal_cooling_action *cact;

		(void) debugfs_create_file("inject_action", S_IWUSR, d,
					(void *)tdev, &inject_action_fops);
		list_for_each_entry(cact, &tdev->cooling_actions, node)
			thermal_add_action_debug(cact, d);
	}

	/* Am I a sensor device ? */
	if (tdev->dev_ops && tdev->dev_ops->report_temp)
		(void) debugfs_create_file("temperature", S_IRUSR, d,
					(void *)tdev, &sensor_temp_fops);

	thermal_device_call(tdev, register_debug_entries, d);
}

static int __init thermal_debug_init(void)
{
	thermal_dbg = debugfs_create_dir("thermal_debug", NULL);
	if (IS_ERR(thermal_dbg))
		return PTR_ERR(thermal_dbg);

	thermal_domains_dbg = debugfs_create_dir("domains", thermal_dbg);
	if (IS_ERR(thermal_domains_dbg))
		return PTR_ERR(thermal_domains_dbg);

	thermal_devices_dbg = debugfs_create_dir("devices", thermal_dbg);
	if (IS_ERR(thermal_domains_dbg))
		return PTR_ERR(thermal_domains_dbg);

	return 0;
}

static void __exit thermal_debug_exit(void)
{
	debugfs_remove_recursive(thermal_dbg);
}
#else
static int __init thermal_debug_init(void)
{
	return 0;
}
static void __exit thermal_debug_exit(void)
{
}
static int thermal_debug_domain(struct thermal_domain *domain)
{
	return 0;
}
static void thermal_debug_register_device(struct thermal_dev *tdev)
{
}
#endif
/**
 * thermal_sensor_set_temp() - External API to allow a sensor driver to set
 *				the current temperature for a domain
 * @tdev: The thermal device setting the temperature
 *
 * Returns 0 for a successfull call to a governor. ENODEV if a governor does not
 * exist.
 */
int thermal_sensor_set_temp(struct thermal_dev *tdev)
{
	struct thermal_domain *thermal_domain;
	int ret = -ENODEV;

	if (unlikely(IS_ERR_OR_NULL(tdev))) {
		pr_err("%s: Invalid thermal device\n", __func__);
		goto out;
	}

	thermal_domain = tdev->domain;
	if (!thermal_domain) {
		pr_err("%s: device not part of a domain\n", __func__);
		goto out;
	}

	if (list_empty(&thermal_domain->cooling_agents)) {
		pr_debug("%s: no cooling agents for domain %s\n",
			__func__, thermal_domain->domain_name);
		goto out;
	}

	ret = thermal_device_call(thermal_domain->governor, process_temp,
					&thermal_domain->cooling_agents,
					tdev, tdev->current_temp);
	if (ret < 0)
		pr_debug("%s: governor does not have callback\n", __func__);
out:
	return ret;
}
EXPORT_SYMBOL_GPL(thermal_sensor_set_temp);

/*
 * thermal_get_slope() - External API for the sensor driver to
 *					get the Slope
 *					for Hotspot temperature computation
 */
int thermal_get_slope(struct thermal_dev *tdev, const char *rel_name)
{
	struct thermal_domain *thermal_domain;
	int ret = -ENODEV;

	if (unlikely(IS_ERR_OR_NULL(tdev))) {
		pr_err("%s: Invalid thermal device\n", __func__);
		return ret;
	}

	thermal_domain = tdev->domain;
	if (!thermal_domain) {
		pr_err("%s: device not part of a domain\n", __func__);
		return ret;
	}

	if (thermal_domain->temp_sensor &&
		thermal_domain->temp_sensor->dev_ops &&
		thermal_domain->temp_sensor->dev_ops->init_slope) {
		return thermal_domain->temp_sensor->dev_ops->init_slope
			(thermal_domain->temp_sensor, rel_name);
		} else {
			pr_err("%s:Getting slope is not supported for domain %s\n",
				__func__, thermal_domain->domain_name);
			return -EINVAL;
		}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(thermal_get_slope);

/*
 * thermal_get_slope() - External API for the sensor driver to
 *                                      get the Slope
 *                                      for Hotspot temperature computation
 */
int thermal_get_offset(struct thermal_dev *tdev, const char *rel_name)
{
	struct thermal_domain *thermal_domain;
	int ret = -ENODEV;

	if (unlikely(IS_ERR_OR_NULL(tdev))) {
		pr_err("%s: Invalid thermal device\n", __func__);
		return ret;
	}

	thermal_domain = tdev->domain;
	if (!thermal_domain) {
		pr_err("%s: device not part of a domain\n", __func__);
		return ret;
	}

	if (thermal_domain->temp_sensor &&
		thermal_domain->temp_sensor->dev_ops &&
		thermal_domain->temp_sensor->dev_ops->init_offset) {
			return thermal_domain->temp_sensor->dev_ops->init_offset
					(thermal_domain->temp_sensor, rel_name);
		} else {
			pr_err("%s:Getting offset is not supported for domain %s\n",
				__func__, thermal_domain->domain_name);
			return -EINVAL;
		}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(thermal_get_offset);

/**
 * thermal_request_temp() - Requests the thermal sensor to report it's current
 *			    temperature to the governor.
 *
 * @tdev: The agent requesting the updated temperature.
 *
 * Returns the current temperature of the current domain.
 * ENODEV if the temperature sensor does not exist.
 * EOPNOTSUPP if the temperature sensor does not support this API.
 */
int thermal_request_temp(struct thermal_dev *tdev)
{
	struct thermal_domain *thermal_domain;
	int ret = -ENODEV;

	if (unlikely(IS_ERR_OR_NULL(tdev))) {
		pr_err("%s: Invalid thermal device\n", __func__);
		return ret;
	}

	thermal_domain = tdev->domain;
	if (!thermal_domain) {
		pr_err("%s: device not part of a domain\n", __func__);
		return ret;
	}

	ret = thermal_device_call(thermal_domain->temp_sensor, report_temp);
	if (ret < 0) {
		pr_err("%s: getting temp is not supported for domain %s\n",
			__func__, thermal_domain->domain_name);
		ret = -EOPNOTSUPP;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(thermal_request_temp);

/**
 * thermal_init_thermal_state() - Initialize the domain thermal state machine.
 *		Once all the thermal devices are available in the domain.
 *		Force the thermal sensor for the domain to report it's current
 *		temperature.
 *
 * @tdev: The thermal device that just registered
 *
 * Returns -ENODEV for empty lists and 0 for success
 */
static int thermal_init_thermal_state(struct thermal_dev *tdev)
{
	struct thermal_domain *domain;

	domain = tdev->domain;
	if (!domain) {
		pr_err("%s: device not part of a domain\n", __func__);
		return -ENODEV;
	}

	if (domain->temp_sensor && domain->governor &&
				!list_empty(&domain->cooling_agents))
		thermal_sensor_set_temp(domain->temp_sensor);
	else
		pr_debug("%s:Not all components registered for %s domain\n",
			__func__, domain->domain_name);

	return 0;
}

static struct thermal_domain *thermal_domain_find(const char *name)
{
	struct thermal_domain *domain = NULL;

	mutex_lock(&thermal_domain_list_lock);
	if (list_empty(&thermal_domain_list))
		goto out;
	list_for_each_entry(domain, &thermal_domain_list, node)
		if (!strcmp(domain->domain_name, name))
			goto out;
	/* Not found */
	domain = NULL;
out:
	mutex_unlock(&thermal_domain_list_lock);

	return domain;
}

static struct thermal_domain *thermal_domain_add(const char *name)
{
	struct thermal_domain *domain;

	domain = kzalloc(sizeof(struct thermal_domain), GFP_KERNEL);
	if (!domain) {
		pr_err("%s: cannot allocate domain memory\n", __func__);
		return NULL;
	}

	INIT_LIST_HEAD(&domain->cooling_agents);
	strlcpy(domain->domain_name, name, sizeof(domain->domain_name));
	mutex_lock(&thermal_domain_list_lock);
	list_add(&domain->node, &thermal_domain_list);
	thermal_debug_domain(domain);
	mutex_unlock(&thermal_domain_list_lock);
	pr_debug("%s: added thermal domain %s\n", __func__, name);

	return domain;
}

/**
 * thermal_check_domain() - Checks to see if a domain exists
 *
 * @domain_name: The name of the domain for validation.
 *
 * Returns 0 if the domain exists.
 * ENODEV if the domain does not exist.
 */
int thermal_check_domain(const char *domain_name)
{
	struct thermal_domain *thermal_domain;
	int ret = 0;

	thermal_domain = thermal_domain_find(domain_name);
	if (!thermal_domain)
		ret = -ENODEV;

	return ret;
}

/**
 * thermal_lookup_temp() - Requests the thermal sensor to report it's current
 *			    temperature to the governor.
 *
 * @tdev: The agent requesting the updated temperature.
 *
 * Returns the current temperature of the current domain.
 * ENODEV if the temperature sensor does not exist.
 */
int thermal_lookup_temp(const char *name)
{
	struct thermal_domain *thermal_domain;
	int ret = -ENODEV;

	thermal_domain = thermal_domain_find(name);
	if (!thermal_domain) {
		pr_err("%s: %s is a non existing domain\n", __func__, name);
		return ret;
	}

	ret = thermal_device_call(thermal_domain->temp_sensor, report_temp);
	if (ret < 0) {
		pr_err("%s: getting temp is not supported for domain %s\n",
			__func__, thermal_domain->domain_name);
		ret = -EOPNOTSUPP;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(thermal_lookup_temp);

/**
 * thermal_lookup_slope() - First, look up a thermal domain and then, if
 *                     available, requests its thermal sensor to report
 *                     slope.
 *
 * @domain_name: a char pointer to the domain name to look up.
 *
 * Returns the slope of the current found domain.
 * ENODEV if the temperature sensor does not exist.
 * EOPNOTSUPP if the temperature sensor does not support this API.
 */
int thermal_lookup_slope(const char *name, const char *rel_name)
{
	struct thermal_domain *thermal_domain;
	int ret = -ENODEV;

	thermal_domain = thermal_domain_find(name);
	if (!thermal_domain) {
		pr_err("%s: %s is a non existing domain\n", __func__, name);
		return ret;
	}

	ret = thermal_device_call(thermal_domain->temp_sensor, init_slope,
							rel_name);
	if (ret < 0) {
		pr_err("%s: getting slope is not supported for domain %s\n",
			__func__, thermal_domain->domain_name);
		ret = -EOPNOTSUPP;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(thermal_lookup_slope);

/**
 * thermal_lookup_offset() - First, look up a thermal domain and then, if
 *                     available, requests its thermal sensor to report
 *                      it's offset.
 *
 * @domain_name: a char pointer to the domain name to look up.
 *
 * Returns the offset of the current found domain.
 * ENODEV if the temperature sensor does not exist.
 * EOPNOTSUPP if the temperature sensor does not support this API.
 */
int thermal_lookup_offset(const char *name, const char *rel_name)
{
	struct thermal_domain *thermal_domain;
	int ret = -ENODEV;

	thermal_domain = thermal_domain_find(name);
	if (!thermal_domain) {
		pr_err("%s: %s is a non existing domain\n", __func__, name);
		return ret;
	}

	ret = thermal_device_call(thermal_domain->temp_sensor, init_offset,
							rel_name);
	/* offset can be a negative value */
	if (ret == -EOPNOTSUPP) {
		pr_err("%s: getting offset is not supported for domain %s\n",
			__func__, thermal_domain->domain_name);
		ret = -EOPNOTSUPP;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(thermal_lookup_offset);

/**
 * thermal_governor_dev_register() - Registration call for thermal domain governors
 *
 * @tdev: The thermal governor device structure.
 *
 * Returns 0 for a successful registration of the governor to a domain
 * ENODEV if the domain could not be created.
 *
 */
int thermal_governor_dev_register(struct thermal_dev *tdev)
{
	struct thermal_domain *domain;

	if (unlikely(IS_ERR_OR_NULL(tdev))) {
		pr_err("%s: Invalid Governor thermal device\n", __func__);
		return -ENODEV;
	}

	domain = thermal_domain_find(tdev->domain_name);
	if (!domain)
		domain = thermal_domain_add(tdev->domain_name);

	if (unlikely(!domain))
		return -ENODEV;

	mutex_lock(&thermal_domain_list_lock);
	domain->governor = tdev;
	tdev->domain = domain;
	thermal_debug_register_device(tdev);
	mutex_unlock(&thermal_domain_list_lock);
	thermal_init_thermal_state(tdev);
	pr_debug("%s: added %s governor\n", __func__, tdev->name);

	return 0;
}
EXPORT_SYMBOL_GPL(thermal_governor_dev_register);

/**
 * thermal_governor_dev_unregister() - Unregistration call for thermal domain
 *	governors.
 *
 * @tdev: The thermal governor device structure.
 *
 */
void thermal_governor_dev_unregister(struct thermal_dev *tdev)
{
	if (unlikely(IS_ERR_OR_NULL(tdev))) {
		pr_err("%s: Invalid Governor thermal device\n", __func__);
		return;
	}

	mutex_lock(&thermal_domain_list_lock);

	if (tdev->domain)
		tdev->domain->governor = NULL;

	mutex_unlock(&thermal_domain_list_lock);
}
EXPORT_SYMBOL_GPL(thermal_governor_dev_unregister);

/**
 * thermal_cooling_dev_register() - Registration call for cooling agents
 *
 * @tdev: The cooling agent device structure.
 *
 * Returns 0 for a successful registration of a cooling agent to a domain
 * ENODEV if the domain could not be created.
 */
int thermal_cooling_dev_register(struct thermal_dev *tdev)
{
	struct thermal_domain *domain;

	if (unlikely(IS_ERR_OR_NULL(tdev))) {
		pr_err("%s: Invalid Cooling thermal device\n", __func__);
		return -ENODEV;
	}

	domain = thermal_domain_find(tdev->domain_name);
	if (!domain)
		domain = thermal_domain_add(tdev->domain_name);

	if (unlikely(!domain))
		return -ENODEV;

	mutex_lock(&thermal_domain_list_lock);
	list_add(&tdev->node, &domain->cooling_agents);
	tdev->domain = domain;
	INIT_LIST_HEAD(&tdev->cooling_actions);
	thermal_debug_register_device(tdev);
	mutex_unlock(&thermal_domain_list_lock);
	thermal_init_thermal_state(tdev);
	pr_debug("%s: added cooling agent %s\n", __func__, tdev->name);

	return 0;
}
EXPORT_SYMBOL_GPL(thermal_cooling_dev_register);

/**
 * thermal_cooling_dev_unregister() - Unregistration call for cooling agents
 *
 * @tdev: The cooling agent device structure.
 *
 */
void thermal_cooling_dev_unregister(struct thermal_dev *tdev)
{

	if (unlikely(IS_ERR_OR_NULL(tdev))) {
		pr_err("%s: Invalid Cooling thermal device\n", __func__);
		return;
	}

	mutex_lock(&thermal_domain_list_lock);

	if (tdev->domain)
		list_del(&tdev->node);

	mutex_unlock(&thermal_domain_list_lock);
}
EXPORT_SYMBOL_GPL(thermal_cooling_dev_unregister);

/**
 * thermal_sensor_dev_register() - Registration call for temperature sensors
 *
 * @tdev: The temperature device structure.
 *
 * Returns 0 for a successful registration of the temp sensor to a domain
 * ENODEV if the domain could not be created.
 */
int thermal_sensor_dev_register(struct thermal_dev *tdev)
{
	struct thermal_domain *domain;

	if (unlikely(IS_ERR_OR_NULL(tdev))) {
		pr_err("%s: Invalid Sensor thermal device\n", __func__);
		return -ENODEV;
	}

	domain = thermal_domain_find(tdev->domain_name);
	if (!domain)
		domain = thermal_domain_add(tdev->domain_name);

	if (unlikely(!domain))
		return -ENODEV;

	mutex_lock(&thermal_domain_list_lock);
	domain->temp_sensor = tdev;
	tdev->domain = domain;
	thermal_debug_register_device(tdev);
	mutex_unlock(&thermal_domain_list_lock);
	thermal_init_thermal_state(tdev);
	pr_debug("%s: added %s sensor\n", __func__, tdev->name);

	return 0;
}
EXPORT_SYMBOL_GPL(thermal_sensor_dev_register);

/**
 * thermal_sensor_dev_unregister() - Unregistration call for  temperature sensors
 *
 * @tdev: The temperature device structure.
 *
 */
void thermal_sensor_dev_unregister(struct thermal_dev *tdev)
{
	if (unlikely(IS_ERR_OR_NULL(tdev))) {
		pr_err("%s: Invalid Sensor thermal device\n", __func__);
		return;
	}

	mutex_lock(&thermal_domain_list_lock);

	if (tdev->domain)
		tdev->domain->temp_sensor = NULL;

	mutex_unlock(&thermal_domain_list_lock);
}
EXPORT_SYMBOL_GPL(thermal_sensor_dev_unregister);

static int __init thermal_framework_init(void)
{
	thermal_debug_init();
	return 0;
}

static void __exit thermal_framework_exit(void)
{
	struct thermal_domain *domain, *tmp;

	mutex_lock(&thermal_domain_list_lock);
	list_for_each_entry_safe(domain, tmp, &thermal_domain_list, node) {
		list_del(&domain->node);
		kfree(domain);
	}
	mutex_unlock(&thermal_domain_list_lock);
}

postcore_initcall(thermal_framework_init);
module_exit(thermal_framework_exit);

MODULE_AUTHOR("Dan Murphy <DMurphy@ti.com>");
MODULE_DESCRIPTION("Thermal Framework driver");
MODULE_LICENSE("GPL");
