/*
 * pps_gen_gpio.c -- kernel GPIO PPS signal generator
 *
 *
 * Copyright (C) 2014   Juan Solano <jsm@jsolano.com>
 *               2009   Alexander Gordeev <lasaine@lvk.cs.msu.su>
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

/*
 * TODO:
 * fix issues when realtime clock is adjusted in a leap
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/time.h>
#include <linux/hrtimer.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>

#define DRVDESC "GPIO PPS signal generator"
#define SEND_DELAY_MAX		100000
#define SAFETY_INTERVAL	3000	/* set the hrtimer earlier for safety (ns) */

/* module parameters */
static unsigned int send_delay = 30000;
MODULE_PARM_DESC(delay,	"Delay between setting and dropping the signal (ns)");
module_param_named(delay, send_delay, uint, 0);

/* device specific private data structure */
struct pps_gen_gpio_devdata {
	struct gpio_desc *pps_gpio;	/* GPIO port descriptor */
	struct hrtimer timer;
	long port_write_time;		/* calibrated port write time (ns) */
};

/* calibrated time between a hrtimer event and the reaction */
static long hrtimer_error = SAFETY_INTERVAL;

/* the kernel hrtimer event */
static enum hrtimer_restart hrtimer_event(struct hrtimer *timer)
{
	struct timespec expire_time, ts1, ts2, ts3, dts;
	struct pps_gen_gpio_devdata *devdata;
	long lim, delta;
	unsigned long flags;

	/* We have to disable interrupts here. The idea is to prevent
	 * other interrupts on the same processor to introduce random
	 * lags while polling the clock. getnstimeofday() takes <1us on
	 * most machines while other interrupt handlers can take much
	 * more potentially.
	 *
	 * NB: approx time with blocked interrupts =
	 * send_delay + 3 * SAFETY_INTERVAL
	 */
	local_irq_save(flags);

	/* first of all we get the time stamp... */
	getnstimeofday(&ts1);
	expire_time = ktime_to_timespec(hrtimer_get_softexpires(timer));
	devdata = container_of(timer, struct pps_gen_gpio_devdata, timer);
	lim = NSEC_PER_SEC - send_delay - devdata->port_write_time;

	/* check if we are late */
	if (expire_time.tv_sec != ts1.tv_sec || ts1.tv_nsec > lim) {
		local_irq_restore(flags);
		pr_err("we are late this time %ld.%09ld\n",
		       ts1.tv_sec, ts1.tv_nsec);
		goto done;
	}

	/* busy loop until the time is right for an assert edge */
	do {
		getnstimeofday(&ts2);
	} while (expire_time.tv_sec == ts2.tv_sec && ts2.tv_nsec < lim);

	/* set the signal */
	gpiod_set_value(devdata->pps_gpio, 1);

	/* busy loop until the time is right for a clear edge */
	lim = NSEC_PER_SEC - devdata->port_write_time;
	do {
		getnstimeofday(&ts2);
	} while (expire_time.tv_sec == ts2.tv_sec && ts2.tv_nsec < lim);

	/* unset the signal */
	gpiod_set_value(devdata->pps_gpio, 0);

	getnstimeofday(&ts3);

	local_irq_restore(flags);

	/* update calibrated port write time */
	dts = timespec_sub(ts3, ts2);
	devdata->port_write_time =
		(devdata->port_write_time + timespec_to_ns(&dts)) >> 1;

done:
	/* update calibrated hrtimer error */
	dts = timespec_sub(ts1, expire_time);
	delta = timespec_to_ns(&dts);
	/* If the new error value is bigger then the old, use the new
	 * value, if not then slowly move towards the new value. This
	 * way it should be safe in bad conditions and efficient in
	 * good conditions.
	 */
	if (delta >= hrtimer_error)
		hrtimer_error = delta;
	else
		hrtimer_error = (3 * hrtimer_error + delta) >> 2;

	/* update the hrtimer expire time */
	hrtimer_set_expires(timer,
			    ktime_set(expire_time.tv_sec + 1,
				      NSEC_PER_SEC - (send_delay +
				      devdata->port_write_time +
				      SAFETY_INTERVAL +
				      2 * hrtimer_error)));

	return HRTIMER_RESTART;
}

/* calibrate port write time */
#define PORT_NTESTS_SHIFT	5
static void calibrate_port(struct pps_gen_gpio_devdata *devdata)
{
	int i;
	long acc = 0;

	for (i = 0; i < (1 << PORT_NTESTS_SHIFT); i++) {
		struct timespec a, b;
		unsigned long irq_flags;

		local_irq_save(irq_flags);
		getnstimeofday(&a);
		gpiod_set_value(devdata->pps_gpio, 0);
		getnstimeofday(&b);
		local_irq_restore(irq_flags);

		b = timespec_sub(b, a);
		acc += timespec_to_ns(&b);
	}

	devdata->port_write_time = acc >> PORT_NTESTS_SHIFT;
	pr_info("port write takes %ldns\n", devdata->port_write_time);
}

static inline ktime_t next_intr_time(struct pps_gen_gpio_devdata *devdata)
{
	struct timespec ts;

	getnstimeofday(&ts);
	return ktime_set(ts.tv_sec +
			((ts.tv_nsec > 990 * NSEC_PER_MSEC) ? 1 : 0),
			NSEC_PER_SEC - (send_delay +
			devdata->port_write_time + 3 * SAFETY_INTERVAL));
}

static int pps_gen_gpio_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct pps_gen_gpio_devdata *devdata;
	int num_gpios;

	num_gpios = of_gpio_named_count(np, "pps-gen-gpios");
	if (num_gpios < 1) {
		dev_err(dev,
			"cannot find a corresponding GPIO defined in DT [%d]\n",
			num_gpios);
		return -EINVAL;
	} else {
		pr_info("found %d GPIOS defined in DT\n", num_gpios);
	}

	/* allocate space for device info */
	devdata = devm_kzalloc(dev, sizeof(struct pps_gen_gpio_devdata),
			       GFP_KERNEL);
	if (!devdata)
		return -ENOMEM;

	/* gpio defined in device tree */
	devdata->pps_gpio = devm_gpiod_get(dev, "pps-gen");
	if (IS_ERR(devdata->pps_gpio)) {
		dev_err(dev, "cannot get PPS GPIO %ld\n",
			PTR_ERR(devdata->pps_gpio));
		return PTR_ERR(devdata->pps_gpio);
	}

	platform_set_drvdata(pdev, devdata);

	ret = gpiod_direction_output(devdata->pps_gpio, 1);
	if (ret < 0) {
		dev_err(dev, "cannot configure PPS GPIO\n");
		return ret;
	}

	calibrate_port(devdata);

	hrtimer_init(&devdata->timer, CLOCK_REALTIME, HRTIMER_MODE_ABS);
	devdata->timer.function = hrtimer_event;
	hrtimer_start(&devdata->timer, next_intr_time(devdata),
		      HRTIMER_MODE_ABS);
	return 0;
}

static int pps_gen_gpio_remove(struct platform_device *pdev)
{
	struct pps_gen_gpio_devdata *devdata = platform_get_drvdata(pdev);
	hrtimer_cancel(&devdata->timer);
	return 0;
}

static void pps_gen_gpio_release(struct device *pdev)
{
}

static const struct of_device_id pps_gen_gpio_dt_ids[] = {
	{ .compatible = "pps-generator-gpios", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, pps_gen_gpio_dt_ids);

static struct platform_driver pps_gen_gpio_driver = {
	.driver		= {
		.name	= "pps_gen_gpio",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(pps_gen_gpio_dt_ids),
	},
	.probe		= pps_gen_gpio_probe,
	.remove		= pps_gen_gpio_remove,
};

static struct platform_device pps_gen_gpio_device = {
	.name = "pps_gen_gpio",
	.id = 0,
	.dev = {
		.release = pps_gen_gpio_release,
	},
};

static int __init pps_gen_gpio_init(void)
{
	pr_info(DRVDESC "\n");
	if (send_delay > SEND_DELAY_MAX) {
		pr_err("delay value should be not greater than %d\n",
		       SEND_DELAY_MAX);
		return -EINVAL;
	}
	platform_device_register(&pps_gen_gpio_device);
	platform_driver_register(&pps_gen_gpio_driver);
	return 0;
}

static void __exit pps_gen_gpio_exit(void)
{
	pr_info("hrtimer avg error is %ldns\n", hrtimer_error);
	platform_driver_unregister(&pps_gen_gpio_driver);
	platform_device_unregister(&pps_gen_gpio_device);
}

module_init(pps_gen_gpio_init);
module_exit(pps_gen_gpio_exit);

MODULE_AUTHOR("Juan Solano <jsm@jsolano.com>");
MODULE_DESCRIPTION(DRVDESC);
MODULE_LICENSE("GPL");