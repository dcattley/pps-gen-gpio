/*
 * pps_gen_gpio.c -- kernel GPIO PPS signal generator
 *
 * Copyright (C)  2009   Alexander Gordeev <lasaine@lvk.cs.msu.su>
 *                2018   Juan Solano <jsm@jsolano.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define DRV_NAME	"pps-gen-gpio"
#define pr_fmt(fmt) DRV_NAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/time.h>
#include <linux/hrtimer.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/version.h>

#define DRVDESC "GPIO PPS signal generator"
MODULE_AUTHOR("Juan Solano <jsm@jsolano.com>");
MODULE_DESCRIPTION(DRVDESC);
MODULE_LICENSE("GPL");

#define PULSE_PERIOD_MIN_NS    (NSEC_PER_SEC / 1000)    /* 1000 PPS */
#define PULSE_PERIOD_MAX_NS    (2 * NSEC_PER_SEC)       /* 0.5 PPS */

#define PULSE_WIDTH_MIN_NS 		(5  * NSEC_PER_USEC)    /* 5us */
#define PULSE_WIDTH_MAX_NS		(PULSE_PERIOD_MAX_NS - PULSE_WIDTH_MIN_NS)
#define PULSE_WIDTH_MAX_SPIN_NS	(100 * NSEC_PER_USEC)   /* 100us */

#define SAFETY_INTERVAL_DEF_NS  (10 * NSEC_PER_USEC)    /* 10us */
#define SAFETY_INTERVAL_MAX_NS  (50 * NSEC_PER_USEC)    /* 50us */

enum pps_gen_gpio_level {
	GPIO_LOW = 0,
	GPIO_HIGH
};

/* Module parameters. */
static unsigned long safety_interval_ns = SAFETY_INTERVAL_DEF_NS;
MODULE_PARM_DESC(safeint, "Safety interval added to timer latency (ns)");
module_param_named(safeint, safety_interval_ns, ulong, 0644);

/* Device private data structure. */
struct pps_gen_gpio_devdata {
	struct device *dev;             /* backref to device */
	struct gpio_desc *gpio;         /* GPIO port descriptor */
	struct hrtimer timer;

	long gpio_latency_ns;           /* measured port write time (ns) */
	long hrtimer_avg_latency_ns;    /* Average of hrtimer interrupt latency. */

	bool pulse_enable;

	u32 pulse_period_ns;
	u32 pulse_width_ns;

	struct timespec64 ts_pulse_time;
	enum pps_gen_gpio_level gpio_level;

	atomic_long_t next_pulse_adjust_ns;

	unsigned long long tmr_count[2];
	unsigned long long tmr_missed[2];
};


/* hrtimer event callback */
static enum hrtimer_restart hrtimer_callback(struct hrtimer *timer)
{
	unsigned long irq_flags;
	long hrtimer_latency_ns;
	struct pps_gen_gpio_devdata *devdata =
		container_of(timer, struct pps_gen_gpio_devdata, timer);

	const enum pps_gen_gpio_level entry_gpio_level = devdata->gpio_level;

	/* Timeline:
	 *    ts_expire_req
	 *      <... ts_hrtimer_latency ...>
	 *    ts_expire_real
	 *      <... spin until time_gpio_assert_ns ...>
	 *    time_gpio_assert_ns: [assert]
	 *      <... spin until time_gpio_deassert_ns ...>
	 *    time_gpio_deassert_ns: [deassert]
	 */
	struct timespec64 ts_pulse_time = devdata->ts_pulse_time;

	const struct timespec64 ts_gpio_assert = timespec64_sub(ts_pulse_time,
		ns_to_timespec64(devdata->gpio_latency_ns));

	const struct timespec64 ts_gpio_deassert = timespec64_add(ts_pulse_time,
		ns_to_timespec64(devdata->pulse_width_ns - devdata->gpio_latency_ns));

	const struct timespec64 ts_gpio_action =
		(GPIO_LOW == entry_gpio_level) ? ts_gpio_assert : ts_gpio_deassert;

	struct timespec64 ts_expire_req, ts_expire_real, ts_gpio_instr_time,
			ts_hrtimer_latency, ts1, ts2;

	devdata->tmr_count[entry_gpio_level] += 1;

	/* We have to disable interrupts here. The idea is to prevent
	 * other interrupts on the same processor to introduce random
	 * lags while polling the clock; ktime_get_real_ts64() takes <1us on
	 * most machines while other interrupt handlers can take much
	 * more potentially.
	 *
	 * Note: approximate time with blocked interrupts =
	 * pulse_width_ns + safety_interval_ns + average hrtimer latency
	 */
	local_irq_save(irq_flags);

	/* Get current timestamp and requested time to check if we are late. */
	ktime_get_real_ts64(&ts_expire_real);
	ts_expire_req = ktime_to_timespec64(hrtimer_get_softexpires(timer));
	if (timespec64_compare(&ts_expire_real, &ts_gpio_action) > 0) {

		devdata->tmr_missed[entry_gpio_level] += 1;

		/* Force deassert PPS GPIO. */
		if (GPIO_LOW != entry_gpio_level)
			gpiod_set_value(devdata->gpio, (devdata->gpio_level = GPIO_LOW));

		local_irq_restore(irq_flags);
		ts_hrtimer_latency = timespec64_sub(ts_expire_real, ts_gpio_action);
		dev_err_ratelimited(devdata->dev, "late %lldns for %s [%lld.%09ld] > [%lld.%09ld] for pulse [%lld.%09ld]\n",
			timespec64_to_ns(&ts_hrtimer_latency),
			(GPIO_LOW == entry_gpio_level ? "assert" : "deassert"),
		 	ts_expire_real.tv_sec, ts_expire_real.tv_nsec,
			ts_gpio_action.tv_sec, ts_gpio_action.tv_nsec,
			devdata->ts_pulse_time.tv_sec, devdata->ts_pulse_time.tv_nsec);
		goto done;
	}

	/* Re-assert current level on GPIO to 'warm the cache' and eliminate assymetry of leading edge uncertainty */
	gpiod_set_value(devdata->gpio, devdata->gpio_level);

	if (GPIO_LOW == entry_gpio_level) {
		/* Busy loop until the time is right for a GPIO assert. */
		do
			ktime_get_real_ts64(&ts1);
		while (timespec64_compare(&ts1, &ts_gpio_assert) < 0);

		/* Assert PPS GPIO. */
		gpiod_set_value(devdata->gpio, (devdata->gpio_level = GPIO_HIGH));

		ktime_get_real_ts64(&ts2);

		if (devdata->pulse_width_ns <= PULSE_WIDTH_MAX_SPIN_NS) {
			/* Busy loop until the time is right for a GPIO deassert. */
			do
				ktime_get_real_ts64(&ts1);
			while (timespec64_compare(&ts1, &ts_gpio_deassert) < 0);

			/* Deassert PPS GPIO. */
			gpiod_set_value(devdata->gpio, (devdata->gpio_level = GPIO_LOW));

			ktime_get_real_ts64(&ts2);
		}
	}
	else {
		/* Busy loop until the time is right for a GPIO deassert. */
		do
			ktime_get_real_ts64(&ts1);
		while (timespec64_compare(&ts1, &ts_gpio_deassert) < 0);

		/* Deassert PPS GPIO. */
		gpiod_set_value(devdata->gpio, (devdata->gpio_level = GPIO_LOW));

		ktime_get_real_ts64(&ts2);
	}

	local_irq_restore(irq_flags);

	/* Update the calibrated GPIO set instruction time. */
	ts_gpio_instr_time = timespec64_sub(ts2, ts1);
	devdata->gpio_latency_ns = (devdata->gpio_latency_ns
		+ timespec64_to_ns(&ts_gpio_instr_time)) / 2;

done:
	/* Update the average hrtimer latency. */
	ts_hrtimer_latency = timespec64_sub(ts_expire_real, ts_expire_req);
	hrtimer_latency_ns = timespec64_to_ns(&ts_hrtimer_latency);

	/* If the new latency value is bigger then the old, use the new
	 * value, if not then slowly move towards the new value. This
	 * way it should be safe in bad conditions and efficient in
	 * good conditions.
	 */
	if (hrtimer_latency_ns > devdata->hrtimer_avg_latency_ns)
		devdata->hrtimer_avg_latency_ns = hrtimer_latency_ns;
	else
		devdata->hrtimer_avg_latency_ns =
			(3 * devdata->hrtimer_avg_latency_ns + hrtimer_latency_ns) / 4;

	/* Update the hrtimer expire time for the next pulse edge. */
	if (GPIO_HIGH == devdata->gpio_level) {
		/* next pulse edge is deassert. phase changes do not apply just width. */

		/* Calculate the deassert edge timeout */
		timespec64_add_ns(&ts_pulse_time,
			devdata->pulse_width_ns - (devdata->gpio_latency_ns +
			(2 * devdata->hrtimer_avg_latency_ns) + safety_interval_ns));
	}
	else {
		/* next pulse edge is assert. adjust for any change in phase.  */
		long next_pulse_adjust_ns = atomic_long_xchg(&devdata->next_pulse_adjust_ns, 0);

		u32 next_pulse_period_ns = devdata->pulse_period_ns;

		if (next_pulse_adjust_ns < 0)
			next_pulse_period_ns -= (u32)-next_pulse_adjust_ns;
		else
			next_pulse_period_ns += (u32)next_pulse_adjust_ns;

		/* Calculate the assert edge timeout */
		timespec64_add_ns(&ts_pulse_time,
			next_pulse_period_ns - (devdata->gpio_latency_ns +
			(2 * devdata->hrtimer_avg_latency_ns) + safety_interval_ns));

		/* Advance the pulse time */
		timespec64_add_ns(&devdata->ts_pulse_time, next_pulse_period_ns);

		if (next_pulse_adjust_ns != 0) {
			dev_info(devdata->dev, "adjusted phase %ldns for pulse [%lld.%09ld]\n",
				next_pulse_adjust_ns,
				devdata->ts_pulse_time.tv_sec, devdata->ts_pulse_time.tv_nsec);
		}
	}

	hrtimer_set_expires(timer,
		timespec64_to_ktime(ts_pulse_time));

	return HRTIMER_RESTART;
}

/* Initial calibration of GPIO set instruction time. */
#define GPIO_NTESTS_SHIFT 7
static void pps_gen_calibrate(struct pps_gen_gpio_devdata *devdata)
{
	struct gpio_desc *gpio = devdata->gpio;
	int i;
	long acc = 0;

	for (i = 0; i < (1 << GPIO_NTESTS_SHIFT); i++) {
		struct timespec64 a, b;
		unsigned long irq_flags;

		local_irq_save(irq_flags);
		ktime_get_real_ts64(&a);
		gpiod_set_value(gpio, (devdata->gpio_level = GPIO_LOW));
		ktime_get_real_ts64(&b);
		local_irq_restore(irq_flags);

		b = timespec64_sub(b, a);
		acc += timespec64_to_ns(&b);
	}
	devdata->gpio_latency_ns = acc >> GPIO_NTESTS_SHIFT;
	dev_info(devdata->dev, "PPS GPIO set takes %ldns\n", devdata->gpio_latency_ns);
}

/* attributes */
static ssize_t pulse_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pps_gen_gpio_devdata *devdata = dev_get_drvdata(dev);
	struct timespec64 ts_pulse_time = devdata->ts_pulse_time;

	return sysfs_emit(buf, "%lld %ld %lu %lu\n", ts_pulse_time.tv_sec, ts_pulse_time.tv_nsec, (unsigned long)devdata->pulse_period_ns, (unsigned long)devdata->pulse_width_ns);
}

static ssize_t pulse_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct pps_gen_gpio_devdata *devdata = dev_get_drvdata(dev);
	int args;
	struct timespec64 ts_pulse_time;
	unsigned long pulse_period_ns;
	unsigned long pulse_width_ns;

	args = sscanf(buf, "%lld %ld %lu %lu", &ts_pulse_time.tv_sec, &ts_pulse_time.tv_nsec, &pulse_period_ns, &pulse_width_ns);
	if (args != 4)
		return -EINVAL;

	if (pulse_period_ns) {
		if ((pulse_period_ns < PULSE_PERIOD_MIN_NS) || (pulse_period_ns > PULSE_PERIOD_MAX_NS))
			return -EINVAL;

		if ((pulse_width_ns < PULSE_WIDTH_MIN_NS) || (pulse_width_ns > PULSE_WIDTH_MAX_NS))
			return -EINVAL;

		if ((pulse_width_ns + PULSE_WIDTH_MIN_NS) > pulse_period_ns)
			return -EINVAL;
	}

	devdata->pulse_enable = !!pulse_period_ns;

	hrtimer_cancel(&devdata->timer);

	if (devdata->pulse_enable) {
		struct timespec64 ts_safe_start;

		set_normalized_timespec64(&ts_pulse_time, ts_pulse_time.tv_sec, ts_pulse_time.tv_nsec);

		/* ensure start time is sufficiently (> 1 sec) in the future */
		ktime_get_real_ts64(&ts_safe_start);
		ts_safe_start.tv_sec += 2;

		if (ts_pulse_time.tv_sec < ts_safe_start.tv_sec)
			ts_pulse_time.tv_sec = ts_safe_start.tv_sec;

		devdata->ts_pulse_time = ts_pulse_time;
		devdata->pulse_period_ns = pulse_period_ns;
		devdata->pulse_width_ns = pulse_width_ns;

		/* calculate the timer expiry required for the requested trigger */
		set_normalized_timespec64(&ts_pulse_time, ts_pulse_time.tv_sec,
			(s64)ts_pulse_time.tv_nsec - (s64)(devdata->gpio_latency_ns +
			(2 * devdata->hrtimer_avg_latency_ns) + safety_interval_ns));

		hrtimer_start(&devdata->timer, timespec64_to_ktime(ts_pulse_time), HRTIMER_MODE_ABS);
	}
	else {
		/* Deassert PPS GPIO. */
		gpiod_set_value(devdata->gpio, (devdata->gpio_level = GPIO_LOW));
	}

	return count;
}

static ssize_t adjust_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct pps_gen_gpio_devdata *devdata = dev_get_drvdata(dev);
	long val;

	if (kstrtol(buf, 0, &val) != 0)
		return -EINVAL;

	if ((unsigned long)abs(val) >= (devdata->pulse_period_ns >> 1))
		return -EINVAL;

	atomic_long_set(&devdata->next_pulse_adjust_ns, val);

	return count;
}

static ssize_t latency_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pps_gen_gpio_devdata *devdata = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lu %lu\n", (unsigned long)devdata->gpio_latency_ns, (unsigned long)devdata->hrtimer_avg_latency_ns);
}

static ssize_t counts_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pps_gen_gpio_devdata *devdata = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%llu %llu %llu %llu\n",
		devdata->tmr_count[GPIO_LOW], devdata->tmr_missed[GPIO_LOW],
		devdata->tmr_count[GPIO_HIGH], devdata->tmr_missed[GPIO_HIGH]);
}

static DEVICE_ATTR_RW(pulse);
static DEVICE_ATTR_WO(adjust);
static DEVICE_ATTR_RO(latency);
static DEVICE_ATTR_RO(counts);

static struct attribute *pps_gen_gpio_attrs[] = {
	&dev_attr_pulse.attr,
	&dev_attr_adjust.attr,
	&dev_attr_latency.attr,
	&dev_attr_counts.attr,
	NULL,
};

ATTRIBUTE_GROUPS(pps_gen_gpio);

/* probe */
static int pps_gen_gpio_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct pps_gen_gpio_devdata *devdata;

	/* Allocate space for device info. */
	devdata = devm_kzalloc(dev,
			       sizeof(struct pps_gen_gpio_devdata),
			       GFP_KERNEL);
	if (!devdata) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	/* backpointer to device */
	devdata->dev = dev;

	/* load default values from module parameters */
	devdata->hrtimer_avg_latency_ns = safety_interval_ns;

	/* There should be a single PPS generator GPIO pin defined in DT. */
	if (gpiod_count(dev, NULL) != 1) {
		dev_err(dev, "There should be exactly one GPIO defined in DT\n");
		ret = -EINVAL;
		goto err_dt;
	}

	devdata->gpio = devm_gpiod_get(dev, NULL, GPIOD_OUT_LOW);
	if (IS_ERR(devdata->gpio)) {
		ret = PTR_ERR(devdata->gpio);
		dev_err(dev, "Cannot get PPS GPIO [%d]\n", ret);
		goto err_gpio_get;
	}

	platform_set_drvdata(pdev, devdata);

	/* No support for GPIO that sleeps (remote) */
	if (gpiod_cansleep(devdata->gpio)) {
		dev_err(dev, "PPS GPIO can sleep\n");
		ret = -EINVAL;
		goto err_gpio_cansleep;
	}

	ret = gpiod_direction_output(devdata->gpio, GPIO_LOW);
	if (ret < 0) {
		dev_err(dev, "Cannot configure PPS GPIO\n");
		goto err_gpio_dir;
	}

	pps_gen_calibrate(devdata);
	hrtimer_init(&devdata->timer, CLOCK_REALTIME, HRTIMER_MODE_ABS);
	devdata->timer.function = hrtimer_callback;

	return 0;

err_gpio_cansleep:
err_gpio_dir:
	devm_gpiod_put(dev, devdata->gpio);
err_gpio_get:
err_dt:
	devm_kfree(dev, devdata);
err_alloc:
	return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
static int pps_gen_gpio_remove(struct platform_device *pdev)
#else
static void pps_gen_gpio_remove(struct platform_device *pdev)
#endif
{
	struct device *dev = &pdev->dev;
	struct pps_gen_gpio_devdata *devdata = platform_get_drvdata(pdev);

	devm_gpiod_put(dev, devdata->gpio);
	hrtimer_cancel(&devdata->timer);

	/* Deassert PPS GPIO. */
	gpiod_set_value(devdata->gpio, (devdata->gpio_level = GPIO_LOW));

	dev_info(devdata->dev, "hrtimer average latency was %ldns\n",
		devdata->hrtimer_avg_latency_ns);
	dev_info(devdata->dev, "asserts %llu deasserts %llu\n",
		devdata->tmr_count[GPIO_LOW], devdata->tmr_count[GPIO_HIGH]);
	dev_info(devdata->dev, "missed asserts %llu deasserts %llu\n",
		devdata->tmr_missed[GPIO_LOW], devdata->tmr_missed[GPIO_HIGH]);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	return 0;
#endif
}

/* The compatible property here defined is searched for in the DT */
static const struct of_device_id pps_gen_gpio_dt_ids[] = {
	{ .compatible = "pps-gen-gpio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, pps_gen_gpio_dt_ids);

static struct platform_driver pps_gen_gpio_driver = {
	.driver			= {
		.name		= DRV_NAME,
		.owner		= THIS_MODULE,
		.of_match_table = of_match_ptr(pps_gen_gpio_dt_ids),
		.dev_groups = pps_gen_gpio_groups,
	},
	.probe			= pps_gen_gpio_probe,
	.remove			= pps_gen_gpio_remove,
};

static int __init pps_gen_gpio_init(void)
{
	pr_info(DRVDESC "\n");
	if (safety_interval_ns > SAFETY_INTERVAL_MAX_NS) {
		pr_err("safeint value should be not greater than %ldns\n",
		       SAFETY_INTERVAL_MAX_NS);
		return -EINVAL;
	}
	platform_driver_register(&pps_gen_gpio_driver);
	return 0;
}

static void __exit pps_gen_gpio_exit(void)
{
	platform_driver_unregister(&pps_gen_gpio_driver);
}

module_init(pps_gen_gpio_init);
module_exit(pps_gen_gpio_exit);
