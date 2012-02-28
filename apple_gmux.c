/*
 *  Gmux driver for Apple laptops
 *
 *  Copyright (C) Canonical Ltd. <seth.forshee@canonical.com>
 *  Copyright (C) 2010-2012 Andreas Heider <andreas@meetr.de>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/backlight.h>
#include <linux/acpi.h>
#include <linux/pnp.h>
#include <linux/pci.h>
#include <linux/vga_switcheroo.h>

struct apple_gmux_data {
	unsigned long iostart;
	unsigned long iolen;

	struct backlight_device *bdev;
};

/* TODO: remove hack */
static struct apple_gmux_data *gmux_sdata;

DECLARE_COMPLETION(powerchange_done);

/*
 * gmux port offsets. Many of these are not yet used, but may be in the
 * future, and it's useful to have them documented here anyhow.
 */
#define GMUX_PORT_VERSION_MAJOR		0x04
#define GMUX_PORT_VERSION_MINOR		0x05
#define GMUX_PORT_VERSION_RELEASE	0x06
#define GMUX_PORT_SWITCH_DISPLAY	0x10
#define GMUX_PORT_SWITCH_GET_DISPLAY	0x11
#define GMUX_PORT_INTERRUPT_ENABLE	0x14
#define GMUX_PORT_INTERRUPT_STATUS	0x16
#define GMUX_PORT_SWITCH_DDC		0x28
#define GMUX_PORT_SWITCH_EXTERNAL	0x40
#define GMUX_PORT_SWITCH_GET_EXTERNAL	0x41
#define GMUX_PORT_DISCRETE_POWER	0x50
#define GMUX_PORT_MAX_BRIGHTNESS	0x70
#define GMUX_PORT_BRIGHTNESS		0x74

#define GMUX_MIN_IO_LEN			(GMUX_PORT_BRIGHTNESS + 4)

#define GMUX_INTERRUPT_ENABLE		0xff
#define GMUX_INTERRUPT_DISABLE		0x00

#define GMUX_INTERRUPT_STATUS_ACTIVE	0
#define GMUX_INTERRUPT_STATUS_DISPLAY	(1 << 0)
#define GMUX_INTERRUPT_STATUS_POWER	(1 << 2)
#define GMUX_INTERRUPT_STATUS_HOTPLUG	(1 << 3)

#define GMUX_BRIGHTNESS_MASK		0x00ffffff
#define GMUX_MAX_BRIGHTNESS		GMUX_BRIGHTNESS_MASK

static inline u8 gmux_read8(struct apple_gmux_data *gmux_data, int port)
{
	return inb(gmux_data->iostart + port);
}

static inline void gmux_write8(struct apple_gmux_data *gmux_data, int port,
			       u8 val)
{
	outb(val, gmux_data->iostart + port);
}

static inline u32 gmux_read32(struct apple_gmux_data *gmux_data, int port)
{
	return inl(gmux_data->iostart + port);
}

static int gmux_get_brightness(struct backlight_device *bd)
{
	struct apple_gmux_data *gmux_data = bl_get_data(bd);
	return gmux_read32(gmux_data, GMUX_PORT_BRIGHTNESS) &
	       GMUX_BRIGHTNESS_MASK;
}

static int gmux_update_status(struct backlight_device *bd)
{
	struct apple_gmux_data *gmux_data = bl_get_data(bd);
	u32 brightness = bd->props.brightness;

	/*
	 * Older gmux versions require writing out lower bytes first then
	 * setting the upper byte to 0 to flush the values. Newer versions
	 * accept a single u32 write, but the old method also works, so we
	 * just use the old method for all gmux versions.
	 */
	gmux_write8(gmux_data, GMUX_PORT_BRIGHTNESS, brightness);
	gmux_write8(gmux_data, GMUX_PORT_BRIGHTNESS + 1, brightness >> 8);
	gmux_write8(gmux_data, GMUX_PORT_BRIGHTNESS + 2, brightness >> 16);
	gmux_write8(gmux_data, GMUX_PORT_BRIGHTNESS + 3, 0);

	return 0;
}

static int gmux_switchto(enum vga_switcheroo_client_id id)
{
	if (id == VGA_SWITCHEROO_IGD) {
		gmux_write8(gmux_sdata, GMUX_PORT_SWITCH_DDC, 1);
		gmux_write8(gmux_sdata, GMUX_PORT_SWITCH_DISPLAY, 2);
		gmux_write8(gmux_sdata, GMUX_PORT_SWITCH_EXTERNAL, 2);
	} else {
		gmux_write8(gmux_sdata, GMUX_PORT_SWITCH_DDC, 2);
		gmux_write8(gmux_sdata, GMUX_PORT_SWITCH_DISPLAY, 3);
		gmux_write8(gmux_sdata, GMUX_PORT_SWITCH_EXTERNAL, 3);
	}

	return 0;
}

static int gmux_set_discrete_state(enum vga_switcheroo_state state)
{
	/* TODO: locking for completions needed? */
	init_completion(&powerchange_done);

	if (state == VGA_SWITCHEROO_ON) {
		gmux_write8(gmux_sdata, GMUX_PORT_DISCRETE_POWER, 1);
		gmux_write8(gmux_sdata, GMUX_PORT_DISCRETE_POWER, 3);
		pr_info("discrete powered up\n");
	} else {
		gmux_write8(gmux_sdata, GMUX_PORT_DISCRETE_POWER, 0);
		pr_info("discrete powered down\n");
	}

	/* TODO: add timeout */
	pr_info("before completion\n");
    wait_for_completion(&powerchange_done);
	pr_info("after completion\n");

	return 0;
}

static int gmux_set_power_state(enum vga_switcheroo_client_id id,
				enum vga_switcheroo_state state)
{
	if (id == VGA_SWITCHEROO_IGD)
		return 0;

	return gmux_set_discrete_state(state);
}

static int gmux_init(void)
{
	return 0;
}

static int gmux_get_client_id(struct pci_dev *pdev)
{
	if (pdev->vendor == 0x8086) /* TODO: better detection, see bbswitch */
		return VGA_SWITCHEROO_IGD;
	else
		return VGA_SWITCHEROO_DIS;
}

static struct vga_switcheroo_handler gmux_handler = {
	.switchto = gmux_switchto,
	.power_state = gmux_set_power_state,
	.init = gmux_init,
	.get_client_id = gmux_get_client_id,
};

static void gmux_disable_interrupts(void)
{
	gmux_write8(gmux_sdata, GMUX_PORT_INTERRUPT_ENABLE, GMUX_INTERRUPT_DISABLE);
}

static void gmux_enable_interrupts(void)
{
	gmux_write8(gmux_sdata, GMUX_PORT_INTERRUPT_ENABLE, GMUX_INTERRUPT_ENABLE);
}

static int gmux_interrupt_get_status(void)
{
	return gmux_read8(gmux_sdata, GMUX_PORT_INTERRUPT_STATUS);
}

static void gmux_interrupt_activate_status(void)
{
	int old_status;
	int new_status;
	
	/* to reactivate interrupts write back current status */
	old_status = gmux_interrupt_get_status();
	gmux_write8(gmux_sdata, GMUX_PORT_INTERRUPT_STATUS, old_status);
	new_status = gmux_interrupt_get_status();
	
	/* status = 0 indicates active interrupts */
	if (new_status)
		pr_info("gmux: error: activate_status, old_status %d new_status %d\n", old_status, new_status);
}

static u32 gmux_gpe_handler(acpi_handle gpe_device, u32 gpe_number, void *context)
{
	int status;

	status = gmux_interrupt_get_status();
	gmux_disable_interrupts();
	pr_info("gmux: gpe handler called: status %d\n", status);

	gmux_interrupt_activate_status();
	gmux_enable_interrupts();
	
	/* TODO: & */
	if (status == GMUX_INTERRUPT_STATUS_POWER)
		complete(&powerchange_done);

	return 0;
}

static const struct backlight_ops gmux_bl_ops = {
	.get_brightness = gmux_get_brightness,
	.update_status = gmux_update_status,
};

static int __devinit gmux_probe(struct pnp_dev *pnp,
				const struct pnp_device_id *id)
{
	struct apple_gmux_data *gmux_data;
	struct resource *res;
	struct backlight_properties props;
	struct backlight_device *bdev;
	u8 ver_major, ver_minor, ver_release;
	acpi_handle dhandle;
	acpi_status status;
	int ret = -ENXIO;

	gmux_data = kzalloc(sizeof(*gmux_data), GFP_KERNEL);
	if (!gmux_data)
		return -ENOMEM;
	pnp_set_drvdata(pnp, gmux_data);

	res = pnp_get_resource(pnp, IORESOURCE_IO, 0);
	if (!res) {
		pr_err("Failed to find gmux I/O resource\n");
		goto err_free;
	}

	gmux_data->iostart = res->start;
	gmux_data->iolen = res->end - res->start;

	if (gmux_data->iolen < GMUX_MIN_IO_LEN) {
		pr_err("gmux I/O region too small (%lu < %u)\n",
		       gmux_data->iolen, GMUX_MIN_IO_LEN);
		goto err_free;
	}

	if (!request_region(gmux_data->iostart, gmux_data->iolen,
			    "Apple gmux")) {
		pr_err("gmux I/O already in use\n");
		goto err_free;
	}

	/*
	 * On some machines the gmux is in ACPI even thought the machine
	 * doesn't really have a gmux. Check for invalid version information
	 * to detect this.
	 */
	ver_major = gmux_read8(gmux_data, GMUX_PORT_VERSION_MAJOR);
	ver_minor = gmux_read8(gmux_data, GMUX_PORT_VERSION_MINOR);
	ver_release = gmux_read8(gmux_data, GMUX_PORT_VERSION_RELEASE);
	if (ver_major == 0xff && ver_minor == 0xff && ver_release == 0xff) {
		pr_info("gmux device not present\n");
		ret = -ENODEV;
		goto err_release;
	}

	pr_info("Found gmux version %d.%d.%d\n", ver_major, ver_minor,
		ver_release);

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_PLATFORM;
	props.max_brightness = gmux_read32(gmux_data, GMUX_PORT_MAX_BRIGHTNESS);

	/*
	 * Currently it's assumed that the maximum brightness is less than
	 * 2^24 for compatibility with old gmux versions. Cap the max
	 * brightness at this value, but print a warning if the hardware
	 * reports something higher so that it can be fixed.
	 */
	if (WARN_ON(props.max_brightness > GMUX_MAX_BRIGHTNESS))
		props.max_brightness = GMUX_MAX_BRIGHTNESS;

	bdev = backlight_device_register("gmux_backlight", &pnp->dev,
					 gmux_data, &gmux_bl_ops, &props);
	if (IS_ERR(bdev)) {
		ret = PTR_ERR(bdev);
		goto err_release;
	}

	gmux_data->bdev = bdev;
	bdev->props.brightness = gmux_get_brightness(bdev);
	backlight_update_status(bdev);

	dhandle = pnp_acpi_device(pnp);
	if (!dhandle) {
		pr_err("Cannot find acpi device for pnp device %s\n", dev_name(&pnp->dev));
		goto err_release;
	} else {
		struct acpi_buffer buf = { ACPI_ALLOCATE_BUFFER, NULL };
		acpi_get_name(dhandle, ACPI_FULL_PATHNAME, &buf);
		pr_info("Found acpi handle for pnp device %s: %s\n", 
			dev_name(&pnp->dev), (char *)buf.pointer);
		kfree(buf.pointer);
	}

    /* TODO: use dhandle? */
	status = acpi_install_gpe_handler(NULL, 0x16, ACPI_GPE_LEVEL_TRIGGERED, &gmux_gpe_handler, dhandle);
	if (ACPI_FAILURE(status)) {
		printk("Install gpe handler failed: %s\n", acpi_format_exception(status));
		goto err_release;
	}

	status = acpi_enable_gpe(NULL, 0x16);
	if (ACPI_FAILURE(status)) {
		pr_err("Enable gpe failed: %s\n", acpi_format_exception(status));
		goto err_enable_gpe;
	}

	/* HACK */
	gmux_sdata = gmux_data;
	
	if (vga_switcheroo_register_handler(&gmux_handler))
		goto err_switcheroo;

	gmux_enable_interrupts();

	return 0;

err_switcheroo:
	acpi_remove_gpe_handler(NULL, 0x16, &gmux_gpe_handler);
err_enable_gpe:
	backlight_device_unregister(bdev);
err_release:
	release_region(gmux_data->iostart, gmux_data->iolen);
err_free:
	kfree(gmux_data);
	return ret;
}

static void __devexit gmux_remove(struct pnp_dev *pnp)
{
	struct apple_gmux_data *gmux_data = pnp_get_drvdata(pnp);

	vga_switcheroo_unregister_handler();
	backlight_device_unregister(gmux_data->bdev);
	gmux_disable_interrupts();
	acpi_remove_gpe_handler(NULL, 0x16, &gmux_gpe_handler);
	acpi_disable_gpe(NULL, 0x16);
	release_region(gmux_data->iostart, gmux_data->iolen);
	kfree(gmux_data);
}

static const struct pnp_device_id gmux_device_ids[] = {
	{"APP000B", 0},
	{"", 0}
};

static struct pnp_driver gmux_pnp_driver = {
	.name		= "apple-gmux",
	.probe		= gmux_probe,
	.remove		= __devexit_p(gmux_remove),
	.id_table	= gmux_device_ids,
};

static int __init apple_gmux_init(void)
{
	return pnp_register_driver(&gmux_pnp_driver);
}

static void __exit apple_gmux_exit(void)
{
	pnp_unregister_driver(&gmux_pnp_driver);
}

module_init(apple_gmux_init);
module_exit(apple_gmux_exit);

MODULE_AUTHOR("Seth Forshee <seth.forshee@canonical.com>");
MODULE_DESCRIPTION("Apple Gmux Driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pnp, gmux_device_ids);
