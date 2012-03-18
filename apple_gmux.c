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

#include <linux/delay.h>

static struct apple_gmux_data {
	unsigned long iostart;
	unsigned long iolen;
	acpi_handle dhandle;
	enum vga_switcheroo_client_id resume_client_id;

	struct backlight_device *bdev;
} gmux_data;

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

static inline u8 gmux_read8(int port)
{
	return inb(gmux_data.iostart + port);
}

static inline void gmux_write8(int port, u8 val)
{
	outb(val, gmux_data.iostart + port);
}

static inline u32 gmux_read32(int port)
{
	return inl(gmux_data.iostart + port);
}

static int gmux_get_brightness(struct backlight_device *bd)
{
	return gmux_read32(GMUX_PORT_BRIGHTNESS) &
	       GMUX_BRIGHTNESS_MASK;
}

static int gmux_update_status(struct backlight_device *bd)
{
	u32 brightness = bd->props.brightness;

	/*
	 * Older gmux versions require writing out lower bytes first then
	 * setting the upper byte to 0 to flush the values. Newer versions
	 * accept a single u32 write, but the old method also works, so we
	 * just use the old method for all gmux versions.
	 */
	gmux_write8(GMUX_PORT_BRIGHTNESS, brightness);
	gmux_write8(GMUX_PORT_BRIGHTNESS + 1, brightness >> 8);
	gmux_write8(GMUX_PORT_BRIGHTNESS + 2, brightness >> 16);
	gmux_write8(GMUX_PORT_BRIGHTNESS + 3, 0);

	return 0;
}

static int gmux_switchto(enum vga_switcheroo_client_id id)
{
	if (id == VGA_SWITCHEROO_IGD) {
		gmux_write8(GMUX_PORT_SWITCH_DDC, 1);
		gmux_write8(GMUX_PORT_SWITCH_DISPLAY, 2);
		gmux_write8(GMUX_PORT_SWITCH_EXTERNAL, 2);
	} else {
		gmux_write8(GMUX_PORT_SWITCH_DDC, 2);
		gmux_write8(GMUX_PORT_SWITCH_DISPLAY, 3);
		gmux_write8(GMUX_PORT_SWITCH_EXTERNAL, 3);
	}

	return 0;
}

static int gmux_switchddc(enum vga_switcheroo_client_id id)
{
	if (id == VGA_SWITCHEROO_IGD) {
		pr_info("switch ddc to IGD\n");
		gmux_write8(GMUX_PORT_SWITCH_DDC, 1);
	} else {
		pr_info("switch ddc to DIS\n");
		gmux_write8(GMUX_PORT_SWITCH_DDC, 2);
	}

	return 0;
}

static int gmux_call_acpi_pwrd(int arg)
{
	// TODO: don't hardcode this
	const char *method = "\\_SB_.PCI0.P0P2.GFX0.PWRD";
	acpi_handle pwrd_handle = NULL;
	acpi_status status = AE_OK;
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object arg0 = { ACPI_TYPE_INTEGER };
	struct acpi_object_list arg_list = { 1, &arg0 };

	status = acpi_get_handle(NULL, (acpi_string) method, &pwrd_handle);
	if (ACPI_FAILURE(status)) {
		pr_err("Cannot get PWRD handle: %s\n", acpi_format_exception(status));
		return -ENODEV;
	}

	arg0.integer.value = arg;

	status = acpi_evaluate_object(pwrd_handle, NULL, &arg_list, &buffer);
	if (ACPI_FAILURE(status)) {
		pr_err("PWRD call failed: %s\n", acpi_format_exception(status));
		return -ENODEV;
	}

	//acpi_result_to_string(buffer.pointer);
	kfree(buffer.pointer);

	pr_info("PWRD call successful\n");
	return 0;
}

static int gmux_set_discrete_state(enum vga_switcheroo_state state)
{
	/* TODO: locking for completions needed? */
	init_completion(&powerchange_done);

	if (state == VGA_SWITCHEROO_ON) {
		gmux_call_acpi_pwrd(0);
		gmux_write8(GMUX_PORT_DISCRETE_POWER, 1);
		gmux_write8(GMUX_PORT_DISCRETE_POWER, 3);
		pr_info("discrete card powered up\n");
	} else {
		gmux_write8(GMUX_PORT_DISCRETE_POWER, 1);
		gmux_write8(GMUX_PORT_DISCRETE_POWER, 0);
		gmux_call_acpi_pwrd(1);
		pr_info("discrete card powered down\n");
	}

	if (wait_for_completion_interruptible_timeout(&powerchange_done, msecs_to_jiffies(200)))
		pr_info("completion timeout\n");

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
	/* early mbps with switchable graphics use nvidia integrated graphics,
	 * hardcode that the 9400M is integrated */
	if (pdev->vendor == PCI_VENDOR_ID_INTEL)
		return VGA_SWITCHEROO_IGD;
	else if (pdev->vendor == PCI_VENDOR_ID_NVIDIA && pdev->device == 0x0863)
		return VGA_SWITCHEROO_IGD;
	else 
		return VGA_SWITCHEROO_DIS;
}

static struct vga_switcheroo_handler gmux_handler = {
	.switchto = gmux_switchto,
	.switchddc = gmux_switchddc,
	.power_state = gmux_set_power_state,
	.init = gmux_init,
	.get_client_id = gmux_get_client_id,
};

static void gmux_disable_interrupts(void)
{
	gmux_write8(GMUX_PORT_INTERRUPT_ENABLE, GMUX_INTERRUPT_DISABLE);
}

static void gmux_enable_interrupts(void)
{
	gmux_write8(GMUX_PORT_INTERRUPT_ENABLE, GMUX_INTERRUPT_ENABLE);
}

static int gmux_interrupt_get_status(void)
{
	return gmux_read8(GMUX_PORT_INTERRUPT_STATUS);
}

static void gmux_interrupt_activate_status(void)
{
	int old_status;
	int new_status;
	
	/* to reactivate interrupts write back current status */
	old_status = gmux_read8(GMUX_PORT_INTERRUPT_STATUS);
	gmux_write8(GMUX_PORT_INTERRUPT_STATUS, old_status);
	new_status = gmux_read8(GMUX_PORT_INTERRUPT_STATUS);
	
	/* status = 0 indicates active interrupts */
	if (new_status)
		pr_info("gmux: error: activate_status, old_status %d new_status %d\n", old_status, new_status);
}

static void gmux_notify_handler(acpi_handle device, u32 value, void *context)
{
	int status;

	status = gmux_interrupt_get_status();
	gmux_disable_interrupts();
	pr_info("gmux: gpe handler called: status %d\n", status);

	gmux_interrupt_activate_status();
	gmux_enable_interrupts();
	
	if (status & GMUX_INTERRUPT_STATUS_POWER)
		complete(&powerchange_done);
}

static const struct backlight_ops gmux_bl_ops = {
	.get_brightness = gmux_get_brightness,
	.update_status = gmux_update_status,
};

static int gmux_suspend(struct pnp_dev *dev, pm_message_t state)
{
	gmux_data.resume_client_id = gmux_read8(GMUX_PORT_SWITCH_DISPLAY) == 2 ?
		VGA_SWITCHEROO_IGD : VGA_SWITCHEROO_DIS;
	return 0;
}

static int gmux_resume(struct pnp_dev *dev)
{
	gmux_switchto(gmux_data.resume_client_id);
	return 0;
}

static int __devinit gmux_probe(struct pnp_dev *pnp,
				const struct pnp_device_id *id)
{
	struct resource *res;
	struct backlight_properties props;
	struct backlight_device *bdev;
	u8 ver_major, ver_minor, ver_release;
	acpi_status status;
	int ret = -ENXIO;

	res = pnp_get_resource(pnp, IORESOURCE_IO, 0);
	if (!res) {
		pr_err("Failed to find gmux I/O resource\n");
		goto err_begin;
	}

	gmux_data.iostart = res->start;
	gmux_data.iolen = res->end - res->start;

	if (gmux_data.iolen < GMUX_MIN_IO_LEN) {
		pr_err("gmux I/O region too small (%lu < %u)\n",
		       gmux_data.iolen, GMUX_MIN_IO_LEN);
		goto err_begin;
	}

	if (!request_region(gmux_data.iostart, gmux_data.iolen,
			    "Apple gmux")) {
		pr_err("gmux I/O already in use\n");
		goto err_begin;
	}

	/*
	 * On some machines the gmux is in ACPI even thought the machine
	 * doesn't really have a gmux. Check for invalid version information
	 * to detect this.
	 */
	ver_major = gmux_read8(GMUX_PORT_VERSION_MAJOR);
	ver_minor = gmux_read8(GMUX_PORT_VERSION_MINOR);
	ver_release = gmux_read8(GMUX_PORT_VERSION_RELEASE);
	if (ver_major == 0xff && ver_minor == 0xff && ver_release == 0xff) {
		pr_info("gmux device not present\n");
		ret = -ENODEV;
		goto err_release;
	}

	pr_info("Found gmux version %d.%d.%d\n", ver_major, ver_minor,
		ver_release);

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_PLATFORM;
	props.max_brightness = gmux_read32(GMUX_PORT_MAX_BRIGHTNESS);

	/*
	 * Currently it's assumed that the maximum brightness is less than
	 * 2^24 for compatibility with old gmux versions. Cap the max
	 * brightness at this value, but print a warning if the hardware
	 * reports something higher so that it can be fixed.
	 */
	if (WARN_ON(props.max_brightness > GMUX_MAX_BRIGHTNESS))
		props.max_brightness = GMUX_MAX_BRIGHTNESS;

	bdev = backlight_device_register("gmux_backlight", &pnp->dev,
					 NULL, &gmux_bl_ops, &props);
	if (IS_ERR(bdev)) {
		ret = PTR_ERR(bdev);
		goto err_release;
	}

	gmux_data.bdev = bdev;
	bdev->props.brightness = gmux_get_brightness(bdev);
	backlight_update_status(bdev);

	gmux_data.dhandle = DEVICE_ACPI_HANDLE(&pnp->dev);
	if (!gmux_data.dhandle) {
		pr_err("Cannot find acpi device for pnp device %s\n", dev_name(&pnp->dev));
		goto err_release;
	} else {
		struct acpi_buffer buf = { ACPI_ALLOCATE_BUFFER, NULL };
		acpi_get_name(gmux_data.dhandle, ACPI_SINGLE_NAME, &buf);
		pr_info("Found acpi handle for pnp device %s: %s\n", 
			dev_name(&pnp->dev), (char *)buf.pointer);
		kfree(buf.pointer);
	}

	status = acpi_install_notify_handler(gmux_data.dhandle, ACPI_DEVICE_NOTIFY, &gmux_notify_handler, pnp);
	if (ACPI_FAILURE(status)) {
		printk("Install notify handler failed: %s\n", acpi_format_exception(status));
		goto err_notify;
	}

	if (vga_switcheroo_register_handler(&gmux_handler))
		goto err_register;

	init_completion(&powerchange_done);
	gmux_enable_interrupts();

	/* TODO: check this out */
	/*active_card = gmux_read8(GMUX_PORT_SWITCH_GET_DISPLAY);*/
	/*pr_info("active card: %x\n", active_card);*/
	/*pr_info("rom shadow: %x %d", pdev->vendor, pdev->resource[PCI_ROM_RESOURCE].flags & IORESOURCE_ROM_SHADOW);*/

	return 0;

err_register:
	status = acpi_remove_notify_handler(gmux_data.dhandle, ACPI_DEVICE_NOTIFY, &gmux_notify_handler);
	if (ACPI_FAILURE(status)) {
		printk("Install notify handler failed: %s\n", acpi_format_exception(status));
	}
err_notify:
	backlight_device_unregister(bdev);
err_release:
	release_region(gmux_data.iostart, gmux_data.iolen);
err_begin:
	return ret;
}

static void __devexit gmux_remove(struct pnp_dev *pnp)
{
	acpi_status status;

	vga_switcheroo_unregister_handler();
	backlight_device_unregister(gmux_data.bdev);
	gmux_disable_interrupts();
	status = acpi_remove_notify_handler(gmux_data.dhandle, ACPI_DEVICE_NOTIFY, &gmux_notify_handler);
	if (ACPI_FAILURE(status)) {
		printk("Install notify handler failed: %s\n", acpi_format_exception(status));
	}
	release_region(gmux_data.iostart, gmux_data.iolen);
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
	.suspend	= gmux_suspend,
	.resume		= gmux_resume
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
