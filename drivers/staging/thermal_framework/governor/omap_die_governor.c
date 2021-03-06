/*
 * drivers/staging/thermal_framework/governor/omap_die_governor.c
 *
 * Copyright (C) 2011-2012 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Dan Murphy <DMurphy@ti.com>
 *
 * Contributors:
 *	Sebastien Sabatier <s-sabatier1@ti.com>
 *	Margarita Olaya Cabrera <magi.olaya@ti.com>
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

#include <linux/err.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/suspend.h>
#include <linux/debugfs.h>

#include <linux/thermal_framework.h>

/* Zone information */
#define FATAL_ZONE	5
#define PANIC_ZONE	4
#define ALERT_ZONE	3
#define MONITOR_ZONE	2
#define SAFE_ZONE	1
#define NO_ACTION	0
#define MAX_NO_MON_ZONES PANIC_ZONE
#define OMAP_FATAL_TEMP 125000
#define OMAP_PANIC_TEMP 110000
#define OMAP_ALERT_TEMP 100000
#define OMAP_MONITOR_TEMP 85000
#define OMAP_SAFE_TEMP  25000

/* TODO: Define this via a configurable file */
#define HYSTERESIS_VALUE 5000
#define NORMAL_TEMP_MONITORING_RATE 1000
#define FAST_TEMP_MONITORING_RATE 250
#define AVERAGE_NUMBER 20


enum governor_instances {
	OMAP_GOV_CPU_INSTANCE,
	OMAP_GOV_GPU_INSTANCE,
	OMAP_GOV_MAX_INSTANCE,
};

#define OMAP_THERMAL_ZONE_NAME_SZ	10
struct omap_thermal_zone {
	char name[OMAP_THERMAL_ZONE_NAME_SZ];
	unsigned int cooling_increment;
	int temp_lower;
	int temp_upper;
	int update_rate;
	int average_rate;
};
#define OMAP_THERMAL_ZONE(n, i, l, u, r, a)		\
{							\
	.name				= n,		\
	.cooling_increment		= (i),		\
	.temp_lower			= (l),		\
	.temp_upper			= (u),		\
	.update_rate			= (r),		\
	.average_rate			= (a),		\
}

struct omap_governor {
	struct thermal_dev *temp_sensor;
	struct thermal_dev thermal_fw;
	struct omap_thermal_zone omap_thermal_zones[MAX_NO_MON_ZONES];
	void (*update_temp_thresh) (struct thermal_dev *, int min, int max);
	int report_rate;
	int panic_zone_reached;
	int cooling_level;
	int hotspot_temp_upper;
	int hotspot_temp_lower;
	int hotspot_temp;
	int pcb_temp;
	bool pcb_sensor_available;
	int sensor_temp;
	int absolute_delta;
	int average_period;
	int avg_gov_sensor_temp;
	int avg_is_valid;
	int omap_gradient_slope;
	int omap_gradient_const;
	int alert_threshold;
	int panic_threshold;
	int prev_zone;
	bool enable_debug_print;
	int sensor_temp_table[AVERAGE_NUMBER];
	struct delayed_work average_gov_sensor_work;
	struct notifier_block pm_notifier;
};

/* Initial set of thersholds for different thermal zones */
static struct omap_thermal_zone omap_thermal_init_zones[] __initdata = {
	OMAP_THERMAL_ZONE("safe", 0, OMAP_SAFE_TEMP, OMAP_MONITOR_TEMP,
			FAST_TEMP_MONITORING_RATE, NORMAL_TEMP_MONITORING_RATE),
	OMAP_THERMAL_ZONE("monitor", 0,
			OMAP_MONITOR_TEMP - HYSTERESIS_VALUE, OMAP_ALERT_TEMP,
			FAST_TEMP_MONITORING_RATE, FAST_TEMP_MONITORING_RATE),
	OMAP_THERMAL_ZONE("alert", 0,
			OMAP_ALERT_TEMP - HYSTERESIS_VALUE, OMAP_PANIC_TEMP,
			FAST_TEMP_MONITORING_RATE, FAST_TEMP_MONITORING_RATE),
	OMAP_THERMAL_ZONE("panic", 1,
			OMAP_PANIC_TEMP - HYSTERESIS_VALUE, OMAP_FATAL_TEMP,
			FAST_TEMP_MONITORING_RATE, FAST_TEMP_MONITORING_RATE),
};

static struct omap_governor *omap_gov_instance[OMAP_GOV_MAX_INSTANCE];

/**
 * DOC: Introduction
 * =================
 * The OMAP On-Die Temperature governor maintains the policy for the
 * on die temperature sensor.  The main goal of the governor is to receive
 * a temperature report from a thermal sensor and calculate the current
 * thermal zone.  Each zone will sort through a list of cooling agents
 * passed in to determine the correct cooling strategy that will cool the
 * device appropriately for that zone.
 *
 * The temperature that is reported by the temperature sensor is first
 * calculated to an OMAP hot spot temperature.
 * This takes care of the existing temperature gradient between
 * the OMAP hot spot and the on-die temp sensor.
 * Note: The "slope" parameter is multiplied by 1000 in the configuration
 * file to avoid using floating values.
 * Note: The "offset" is defined in milli-celsius degrees.
 *
 * Next the hot spot temperature is then compared to thresholds to determine
 * the proper zone.
 *
 * There are 5 zones identified:
 *
 * FATAL_ZONE: This zone indicates that the on-die temperature has reached
 * a point where the device needs to be rebooted and allow ROM or the bootloader
 * to run to allow the device to cool.
 *
 * PANIC_ZONE: This zone indicates a near fatal temperature is approaching
 * and should impart all neccessary cooling agent to bring the temperature
 * down to an acceptable level.
 *
 * ALERT_ZONE: This zone indicates that die is at a level that may need more
 * agressive cooling agents to keep or lower the temperature.
 *
 * MONITOR_ZONE: This zone is used as a monitoring zone and may or may not use
 * cooling agents to hold the current temperature.
 *
 * SAFE_ZONE: This zone is optimal thermal zone.  It allows the device to
 * run at max levels without imparting any cooling agent strategies.
 *
 * NO_ACTION: Means just that.  There was no action taken based on the current
 * temperature sent in.
*/

/**
 * omap_update_report_rate() - Updates the temperature sensor monitoring rate
 *
 * @new_rate: New measurement rate for the temp sensor
 *
 * No return
 */
static void omap_update_report_rate(struct omap_governor *omap_gov,
					struct thermal_dev *temp_sensor,
					int new_rate)
{
	if (omap_gov->report_rate == -EOPNOTSUPP) {
		pr_err("%s:Updating the report rate is not supported\n",
			__func__);
		return;
	}

	if (omap_gov->report_rate != new_rate)
		omap_gov->report_rate = thermal_device_call(temp_sensor,
						set_temp_report_rate, new_rate);
}

/*
 * convert_omap_sensor_temp_to_hotspot_temp() -Convert the temperature from the
 *		OMAP on-die temp sensor into OMAP hot spot temperature.
 *		This takes care of the existing temperature gradient between
 *		the OMAP hot spot and the on-die temp sensor.
 *
 * @sensor_temp: Raw temperature reported by the OMAP die temp sensor
 *
 * Returns the calculated hot spot temperature for the zone calculation
 */
static signed int convert_omap_sensor_temp_to_hotspot_temp(
				struct omap_governor *omap_gov, int sensor_temp)
{
	int absolute_delta;

	/* PCB sensor inputs are required only for CPU domain */
	if ((!strcmp(omap_gov->thermal_fw.domain_name, "cpu")) &&
		omap_gov->pcb_sensor_available &&
		(omap_gov->avg_is_valid == 1)) {
		omap_gov->pcb_temp  = thermal_lookup_temp("pcb");
		if (omap_gov->pcb_temp < 0)
			return sensor_temp + omap_gov->absolute_delta;

		absolute_delta = (
			((omap_gov->avg_gov_sensor_temp - omap_gov->pcb_temp) *
				thermal_lookup_slope("pcb",
				omap_gov->thermal_fw.domain_name) / 1000) +
				thermal_lookup_offset("pcb",
				omap_gov->thermal_fw.domain_name));
		/* Ensure that this formula never returns negative value */
		if (absolute_delta < 0)
			absolute_delta = 0;
	} else {
		absolute_delta = ((sensor_temp *
					omap_gov->omap_gradient_slope / 1000) +
					omap_gov->omap_gradient_const);
	}

	omap_gov->absolute_delta = absolute_delta;

	pr_debug("%s: sensor.temp -> hotspot.temp: sensor %d avg_sensor %d pcb %d, delta %d hotspot %d\n",
			omap_gov->thermal_fw.domain_name,
			sensor_temp, omap_gov->avg_gov_sensor_temp,
			omap_gov->pcb_temp, omap_gov->absolute_delta,
			sensor_temp + absolute_delta);

	omap_gov->hotspot_temp = sensor_temp + absolute_delta;
	return sensor_temp + absolute_delta;
}

/*
 * hotspot_temp_to_omap_sensor_temp() - Convert the temperature from
 *		the OMAP hot spot temperature into the OMAP on-die temp sensor.
 *		This is useful to configure the thresholds at OMAP on-die
 *		sensor level. This takes care of the existing temperature
 *		gradient between the OMAP hot spot and the on-die temp sensor.
 *
 * @hot_spot_temp: Hot spot temperature to the be calculated to CPU on-die
 *		temperature value.
 *
 * Returns the calculated on-die temperature.
 */

static signed hotspot_temp_to_sensor_temp(struct omap_governor *omap_gov,
							int hot_spot_temp)
{
	/* PCB sensor inputs are required only for CPU domain */
	if ((!strcmp(omap_gov->thermal_fw.domain_name, "cpu")) &&
		omap_gov->pcb_sensor_available && (omap_gov->avg_is_valid == 1))
		return hot_spot_temp - omap_gov->absolute_delta;
	else
		return ((hot_spot_temp - omap_gov->omap_gradient_const) *
				1000) / (1000 + omap_gov->omap_gradient_slope);
}

static int omap_enter_zone(struct omap_governor *omap_gov,
				struct omap_thermal_zone *zone,
				bool set_cooling_level,
				struct list_head *cooling_list, int cpu_temp)
{
	int temp_upper;
	int temp_lower;

	if (list_empty(cooling_list)) {
		pr_err("%s: No Cooling devices registered\n",
			__func__);
		return -ENODEV;
	}

	if (set_cooling_level) {
		if (zone->cooling_increment)
			omap_gov->cooling_level += zone->cooling_increment;
		else
			omap_gov->cooling_level = 0;
		thermal_device_call_all(cooling_list, cool_device,
						omap_gov->cooling_level);
	}
	temp_lower = hotspot_temp_to_sensor_temp(omap_gov, zone->temp_lower);
	temp_upper = hotspot_temp_to_sensor_temp(omap_gov, zone->temp_upper);
	thermal_device_call(omap_gov->temp_sensor, set_temp_thresh, temp_lower,
								temp_upper);
	omap_update_report_rate(omap_gov, omap_gov->temp_sensor,
							zone->update_rate);

	omap_gov->hotspot_temp_lower = temp_lower;
	omap_gov->hotspot_temp_upper = temp_upper;

	/* PCB sensor inputs are required only for CPU domain */
	if ((!strcmp(omap_gov->thermal_fw.domain_name, "cpu")) &&
		omap_gov->pcb_sensor_available)
		omap_gov->average_period = zone->average_rate;

	return 0;
}

/**
 * omap_fatal_zone() - Shut-down the system to ensure OMAP Junction
 *			temperature decreases enough
 *
 * @cpu_temp:	The current adjusted CPU temperature
 *
 * No return forces a restart of the system
 */
static void omap_fatal_zone(int cpu_temp)
{
	pr_emerg("%s:FATAL ZONE (hot spot temp: %i)\n", __func__, cpu_temp);

	kernel_restart(NULL);
}

static int omap_thermal_manager(struct omap_governor *omap_gov,
				struct list_head *cooling_list, int temp)
{
	int cpu_temp, zone = NO_ACTION;
	bool set_cooling_level = true;

	cpu_temp = convert_omap_sensor_temp_to_hotspot_temp(omap_gov, temp);
	if (cpu_temp >= OMAP_FATAL_TEMP) {
		omap_fatal_zone(cpu_temp);
		return FATAL_ZONE;
	} else if (cpu_temp >= omap_gov->panic_threshold) {
		int temp_upper;

		omap_gov->panic_zone_reached++;
		temp_upper = (((OMAP_FATAL_TEMP -
				omap_gov->panic_threshold) / 4) *
					omap_gov->panic_zone_reached) +
				omap_gov->panic_threshold;
		if (temp_upper >= OMAP_FATAL_TEMP)
			temp_upper = OMAP_FATAL_TEMP;
		omap_gov->omap_thermal_zones[PANIC_ZONE - 1].temp_upper =
								temp_upper;
		zone = PANIC_ZONE;
	} else if (cpu_temp < (omap_gov->panic_threshold - HYSTERESIS_VALUE)) {
		if (cpu_temp >= omap_gov->alert_threshold) {
			set_cooling_level = omap_gov->panic_zone_reached == 0;
			zone = ALERT_ZONE;
		} else if (cpu_temp < (omap_gov->alert_threshold -
						HYSTERESIS_VALUE)) {
			if (cpu_temp >= OMAP_MONITOR_TEMP) {
				omap_gov->panic_zone_reached = 0;
				zone = MONITOR_ZONE;
			} else {
				/*
				 * this includes the case where :
				 * (OMAP_MONITOR_TEMP - HYSTERESIS_VALUE) <= T
				 * && T < OMAP_MONITOR_TEMP
				 */
				omap_gov->panic_zone_reached = 0;
				zone = SAFE_ZONE;
			}
		} else {
			/*
			 * this includes the case where :
			 * (OMAP_ALERT_TEMP - HYSTERESIS_VALUE) <=
			 * T < OMAP_ALERT_TEMP
			 */
			omap_gov->panic_zone_reached = 0;
			zone = MONITOR_ZONE;
		}
	} else {
		/*
		 * this includes the case where :
		 * (OMAP_PANIC_TEMP - HYSTERESIS_VALUE) <= T < OMAP_PANIC_TEMP
		 */
		set_cooling_level = omap_gov->panic_zone_reached == 0;
		zone = ALERT_ZONE;
	}

	if (zone != NO_ACTION) {
		struct omap_thermal_zone *therm_zone;

		therm_zone = &omap_gov->omap_thermal_zones[zone - 1];
		if ((omap_gov->enable_debug_print) &&
		((omap_gov->prev_zone != zone) || (zone == PANIC_ZONE))) {
			pr_info("%s:sensor %d avg sensor %d pcb ",
				 __func__, temp,
				 omap_gov->avg_gov_sensor_temp);
			pr_info("%d, delta %d hot spot %d\n",
				 omap_gov->pcb_temp, omap_gov->absolute_delta,
				 cpu_temp);
			pr_info("%s: hot spot temp %d - going into %s zone\n",
				__func__, cpu_temp, therm_zone->name);
			omap_gov->prev_zone = zone;
		}

		omap_enter_zone(omap_gov, therm_zone,
				set_cooling_level, cooling_list, cpu_temp);
	}

	return zone;
}

/*
 * Make an average of the OMAP on-die temperature
 * this is helpful to handle burst activity of OMAP when extrapolating
 * the OMAP hot spot temperature from on-die sensor and PCB temperature
 * Re-evaluate the temperature gradient between hot spot and on-die sensor
 * (See absolute_delta) and reconfigure the thresholds if needed
 */
static void average_on_die_temperature(struct omap_governor *omap_gov)
{
	int i;

	if (omap_gov->temp_sensor == NULL)
		return;

	/* Read current temperature */
	omap_gov->sensor_temp = thermal_request_temp(omap_gov->temp_sensor);

	/* if on-die sensor does not report a correct value, then return */
	if (omap_gov->sensor_temp == -EINVAL)
		return;

	/* Update historical buffer */
	for (i = 1; i < AVERAGE_NUMBER; i++) {
		omap_gov->sensor_temp_table[AVERAGE_NUMBER - i] =
		omap_gov->sensor_temp_table[AVERAGE_NUMBER - i - 1];
	}
	omap_gov->sensor_temp_table[0] = omap_gov->sensor_temp;

	if (omap_gov->sensor_temp_table[AVERAGE_NUMBER - 1] == 0)
		omap_gov->avg_is_valid = 0;
	else
		omap_gov->avg_is_valid = 1;

	/* Compute the new average value */
	omap_gov->avg_gov_sensor_temp = 0;
	for (i = 0; i < AVERAGE_NUMBER; i++)
		omap_gov->avg_gov_sensor_temp += omap_gov->sensor_temp_table[i];

	omap_gov->avg_gov_sensor_temp =
		(omap_gov->avg_gov_sensor_temp / AVERAGE_NUMBER);

	/*
	 * Reconfigure the current temperature thresholds according
	 * to the current PCB temperature
	 */
	convert_omap_sensor_temp_to_hotspot_temp(omap_gov,
				omap_gov->sensor_temp);
	thermal_device_call(omap_gov->temp_sensor, set_temp_thresh,
				omap_gov->hotspot_temp_lower,
				omap_gov->hotspot_temp_upper);
}

static void average_sensor_delayed_work_fn(struct work_struct *work)
{
	struct omap_governor *omap_gov =
				container_of(work, struct omap_governor,
				average_gov_sensor_work.work);

	average_on_die_temperature(omap_gov);

	schedule_delayed_work(&omap_gov->average_gov_sensor_work,
		msecs_to_jiffies(omap_gov->average_period));
}

static int omap_process_temp(struct thermal_dev *gov,
				struct list_head *cooling_list,
				struct thermal_dev *temp_sensor,
				int temp)
{
	struct omap_governor *omap_gov = container_of(gov, struct
					omap_governor, thermal_fw);

	pr_debug("%s: Received temp %i\n", __func__, temp);
	omap_gov->temp_sensor = temp_sensor;
	if (!omap_gov->pcb_sensor_available &&
		(thermal_check_domain("pcb") == 0))
		omap_gov->pcb_sensor_available = true;

	return omap_thermal_manager(omap_gov, cooling_list, temp);
}

static int omap_die_pm_notifier_cb(struct notifier_block *notifier,
				unsigned long pm_event,  void *unused)
{
	struct omap_governor *omap_gov = container_of(notifier, struct
						omap_governor, pm_notifier);

	switch (pm_event) {
	case PM_SUSPEND_PREPARE:
		cancel_delayed_work_sync(&omap_gov->average_gov_sensor_work);
		break;
	case PM_POST_SUSPEND:
		schedule_work(&omap_gov->average_gov_sensor_work.work);
		break;
	}

	return NOTIFY_DONE;
}

/* debugfs hooks for omap die gov */
static int option_get(void *data, u64 *val)
{
	u32 *option = data;

	*val = *option;

	return 0;
}

static int option_set(void *data, u64 val)
{
	u32 *option = data;

	*option = val;

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(omap_die_gov_fops, option_get, NULL, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(omap_die_gov_rw_fops, option_get, option_set, "%llu\n");

/* Update temp_sensor with current values */
static void debug_apply_thresholds(struct omap_governor *omap_gov)
{
	/*
	 * Reconfigure the current temperature thresholds according
	 * to the new user space thresholds.
	 */
	thermal_device_call(omap_gov->temp_sensor, set_temp_thresh,
				omap_gov->hotspot_temp_lower,
				omap_gov->hotspot_temp_upper);
	omap_gov->sensor_temp = thermal_device_call(omap_gov->temp_sensor,
						    report_temp);
	thermal_sensor_set_temp(omap_gov->temp_sensor);
}

static int option_alert_get(void *data, u64 *val)
{
	struct omap_governor *omap_gov = (struct omap_governor *) data;

	*val = omap_gov->alert_threshold;

	return 0;
}

static int option_alert_set(void *data, u64 val)
{
	struct omap_governor *omap_gov = (struct omap_governor *) data;

	if (val <= OMAP_MONITOR_TEMP) {
		pr_err("Invalid threshold: ALERT:%d is <= MONITOR:%d\n",
			(int)val, OMAP_MONITOR_TEMP);
		return -EINVAL;
	} else if (val >= omap_gov->panic_threshold) {
		pr_err("Invalid threshold: ALERT:%d is >= PANIC:%d\n",
			(int)val, omap_gov->panic_threshold);
		return -EINVAL;
	}
	/* Change the ALERT Threshold value */
	omap_gov->alert_threshold = val;
	/* Also update the governor zone array */
	omap_gov->omap_thermal_zones[MONITOR_ZONE - 1].temp_upper =
						omap_gov->alert_threshold;
	omap_gov->omap_thermal_zones[ALERT_ZONE - 1].temp_lower =
				omap_gov->alert_threshold - HYSTERESIS_VALUE;

	/* Skip sensor update if no sensor is present */
	if (!IS_ERR_OR_NULL(omap_gov->temp_sensor))
		debug_apply_thresholds(omap_gov);

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(omap_die_gov_alert_fops, option_alert_get,
			option_alert_set, "%llu\n");

static int option_panic_get(void *data, u64 *val)
{
	struct omap_governor *omap_gov = (struct omap_governor *) data;

	*val = omap_gov->panic_threshold;

	return 0;
}

static int option_panic_set(void *data, u64 val)
{
	struct omap_governor *omap_gov = (struct omap_governor *) data;

	if (val <= omap_gov->alert_threshold) {
		pr_err("Invalid threshold: PANIC:%d is <= ALERT:%d\n",
			(int)val, omap_gov->alert_threshold);
		return -EINVAL;
	} else if (val >= OMAP_FATAL_TEMP) {
		pr_err("Invalid threshold: PANIC:%d is >= FATAL:%d\n",
			(int)val, OMAP_FATAL_TEMP);
		return -EINVAL;
	}
	/* Change the PANIC Threshold value */
	omap_gov->panic_threshold = val;
	/* Also update the governor zone array */
	omap_gov->omap_thermal_zones[ALERT_ZONE - 1].temp_upper =
						omap_gov->panic_threshold;
	omap_gov->omap_thermal_zones[PANIC_ZONE - 1].temp_lower =
				omap_gov->panic_threshold - HYSTERESIS_VALUE;

	/* Skip sensor update if no sensor is present */
	if (!IS_ERR_OR_NULL(omap_gov->temp_sensor))
		debug_apply_thresholds(omap_gov);

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(omap_die_gov_panic_fops, option_panic_get,
			option_panic_set, "%llu\n");

#ifdef CONFIG_THERMAL_FRAMEWORK_DEBUG
static int omap_gov_register_debug_entries(struct thermal_dev *gov,
					struct dentry *d)
{
	struct omap_governor *omap_gov = container_of(gov, struct
						omap_governor, thermal_fw);

	/* Read Only - Debug properties of cpu and gpu gov */
	(void) debugfs_create_file("cooling_level",
			S_IRUGO, d, &(omap_gov->cooling_level),
			&omap_die_gov_fops);
	(void) debugfs_create_file("hotspot_temp_upper",
			S_IRUGO, d, &(omap_gov->hotspot_temp_upper),
			&omap_die_gov_fops);
	(void) debugfs_create_file("hotspot_temp_lower",
			S_IRUGO, d, &(omap_gov->hotspot_temp_lower),
			&omap_die_gov_fops);
	(void) debugfs_create_file("hotspot_temp",
			S_IRUGO, d, &(omap_gov->hotspot_temp),
			&omap_die_gov_fops);
	(void) debugfs_create_file("avg_cpu_sensor_temp",
			S_IRUGO, d, &(omap_gov->avg_gov_sensor_temp),
			&omap_die_gov_fops);

	/* Read  and Write properties of die gov */
	/* ALERT zone threshold */
	(void) debugfs_create_file("alert_threshold",
			S_IRUGO | S_IWUSR, d, omap_gov,
			&omap_die_gov_alert_fops);

	/* PANIC zone threshold */
	(void) debugfs_create_file("panic_threshold",
			S_IRUGO | S_IWUSR, d, omap_gov,
			&omap_die_gov_panic_fops);

	/* Flag to enable the Debug Zone Prints */
	(void) debugfs_create_file("enable_debug_print",
			S_IRUGO | S_IWUSR, d, &(omap_gov->enable_debug_print),
			&omap_die_gov_rw_fops);

	return 0;
}
#endif

static struct thermal_dev_ops omap_gov_ops = {
	.process_temp = omap_process_temp,
#ifdef CONFIG_THERMAL_FRAMEWORK_DEBUG
	.register_debug_entries = omap_gov_register_debug_entries,
#endif
};

static struct notifier_block omap_die_pm_notifier = {
	.notifier_call = omap_die_pm_notifier_cb,
};

static int __init omap_governor_init(void)
{
	int i;

	for (i = 0; i < OMAP_GOV_MAX_INSTANCE; i++) {
		omap_gov_instance[i] = kzalloc(sizeof(struct omap_governor),
								GFP_KERNEL);
		if (!omap_gov_instance[i]) {
			pr_err("%s:Cannot allocate memory\n", __func__);
			goto error;
		}
	}

	for (i = 0; i < OMAP_GOV_MAX_INSTANCE; i++) {
		omap_gov_instance[i]->average_period =
						NORMAL_TEMP_MONITORING_RATE;
		omap_gov_instance[i]->avg_is_valid = 0;
		omap_gov_instance[i]->hotspot_temp = 0;
		omap_gov_instance[i]->panic_zone_reached = 0;
		omap_gov_instance[i]->pcb_sensor_available = false;
		omap_gov_instance[i]->enable_debug_print = false;
		omap_gov_instance[i]->alert_threshold = OMAP_ALERT_TEMP;
		omap_gov_instance[i]->panic_threshold = OMAP_PANIC_TEMP;

		INIT_DELAYED_WORK(&omap_gov_instance[i]->
					average_gov_sensor_work,
					average_sensor_delayed_work_fn);

		omap_gov_instance[i]->pm_notifier = omap_die_pm_notifier;
		if (register_pm_notifier(&omap_gov_instance[i]->pm_notifier))
			pr_err("%s: omap_gov pm registration failed!\n",
								__func__);
		schedule_work(&omap_gov_instance[i]->
					average_gov_sensor_work.work);
	}

	/* Initializing CPU governor */
	memcpy(omap_gov_instance[OMAP_GOV_CPU_INSTANCE]->omap_thermal_zones,
		omap_thermal_init_zones, sizeof(omap_thermal_init_zones));

	omap_gov_instance[OMAP_GOV_CPU_INSTANCE]->thermal_fw.name =
							"omap_cpu_governor";
	omap_gov_instance[OMAP_GOV_CPU_INSTANCE]->thermal_fw.domain_name =
							"cpu";
	omap_gov_instance[OMAP_GOV_CPU_INSTANCE]->thermal_fw.dev_ops =
							&omap_gov_ops;
	thermal_governor_dev_register(
			&omap_gov_instance[OMAP_GOV_CPU_INSTANCE]->thermal_fw);

	omap_gov_instance[OMAP_GOV_CPU_INSTANCE]->omap_gradient_slope =
	thermal_get_slope(&omap_gov_instance[OMAP_GOV_CPU_INSTANCE]->thermal_fw,
									NULL);
	omap_gov_instance[OMAP_GOV_CPU_INSTANCE]->omap_gradient_const =
	thermal_get_offset(&omap_gov_instance[OMAP_GOV_CPU_INSTANCE]->
							thermal_fw, NULL);

	pr_info("%s: domain %s slope %d const %d\n", __func__,
	omap_gov_instance[OMAP_GOV_CPU_INSTANCE]->thermal_fw.domain_name,
	omap_gov_instance[OMAP_GOV_CPU_INSTANCE]->omap_gradient_slope,
	omap_gov_instance[OMAP_GOV_CPU_INSTANCE]->omap_gradient_const);

	/* Initializing GPU governor */
	memcpy(omap_gov_instance[OMAP_GOV_GPU_INSTANCE]->omap_thermal_zones,
		omap_thermal_init_zones, sizeof(omap_thermal_init_zones));

	omap_gov_instance[OMAP_GOV_GPU_INSTANCE]->thermal_fw.name =
							"omap_gpu_governor";
	omap_gov_instance[OMAP_GOV_GPU_INSTANCE]->thermal_fw.domain_name =
							"gpu";
	omap_gov_instance[OMAP_GOV_GPU_INSTANCE]->thermal_fw.dev_ops =
							&omap_gov_ops;
	thermal_governor_dev_register(
			&omap_gov_instance[OMAP_GOV_GPU_INSTANCE]->thermal_fw);

	omap_gov_instance[OMAP_GOV_GPU_INSTANCE]->omap_gradient_slope =
	thermal_get_slope(&omap_gov_instance[OMAP_GOV_GPU_INSTANCE]->thermal_fw,
									NULL);
	omap_gov_instance[OMAP_GOV_GPU_INSTANCE]->omap_gradient_const =
	thermal_get_offset(&omap_gov_instance[OMAP_GOV_GPU_INSTANCE]->
							thermal_fw, NULL);

	pr_info("%s: domain %s slope %d const %d\n", __func__,
	omap_gov_instance[OMAP_GOV_GPU_INSTANCE]->thermal_fw.domain_name,
	omap_gov_instance[OMAP_GOV_GPU_INSTANCE]->omap_gradient_slope,
	omap_gov_instance[OMAP_GOV_GPU_INSTANCE]->omap_gradient_const);

	return 0;

error:
	for (i = 0; i < OMAP_GOV_MAX_INSTANCE; i++)
		kfree(omap_gov_instance[i]);

	return -ENOMEM;
}

static void __exit omap_governor_exit(void)
{
	int i;

	for (i = 0; i < OMAP_GOV_MAX_INSTANCE; i++) {
		cancel_delayed_work_sync(
			&omap_gov_instance[i]->average_gov_sensor_work);
		thermal_governor_dev_unregister(
					&omap_gov_instance[i]->thermal_fw);
		kfree(omap_gov_instance[i]);
	}
}

module_init(omap_governor_init);
module_exit(omap_governor_exit);

MODULE_AUTHOR("Dan Murphy <DMurphy@ti.com>");
MODULE_DESCRIPTION("OMAP on-die thermal governor");
MODULE_LICENSE("GPL");
