/*
 * ef44_ramlog: mirror every kernel message into a fixed, otherwise unused
 * DRAM window (0xA3810000: the gap between the ION MFC heap and the third RAM
 * bank, which neither this kernel nor TWRP uses and which survives warm resets),
 * so the log of a boot that hangs or reboots can be read back afterwards with
 * /dev/mem (from recovery, or from the next boot). Registered as a console with
 * CON_PRINTBUFFER, so it also gets the messages printed before it was
 * registered.
 *
 * Layout: struct ramlog_ctrl, then two slots of struct ramlog_hdr + ring
 * buffer. Each boot writes the slot the previous boot did not, so the log of
 * the boot before survives even if the phone boots straight back into Android
 * (e.g. after a watchdog reset).
 *
 * Bring-up helper: booting with ef44_ramlog.autorecovery_secs=N makes every
 * reset that goes through msm_restart land in recovery, and reboots into
 * recovery by itself after N seconds unless the parameter is set to 0 through
 * /sys/module/ef44_ramlog/parameters/autorecovery_secs first.
 */

#include <linux/console.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/moduleparam.h>
#include <linux/reboot.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define RAMLOG_PHYS	0xA3810000
#define RAMLOG_SIZE	0x000E0000
#define RAMLOG_MAGIC	0x34344645	/* "EF44" */
#define RECOVERY_REASON	0x77665502

struct ramlog_ctrl {
	u32 magic;
	u32 cur;	/* slot written by the running boot */
	u32 boots;	/* boots seen since the window was last cleared */
	u32 reserved;
};

struct ramlog_hdr {
	u32 boot;	/* value of ctrl.boots when this slot was written */
	u32 pos;	/* next write offset in the slot's data */
	u32 wrapped;	/* data has been filled at least once */
	u32 reserved;
};

#define SLOT_SIZE	((RAMLOG_SIZE - sizeof(struct ramlog_ctrl)) / 2)
#define DATA_SIZE	(SLOT_SIZE - sizeof(struct ramlog_hdr))

static struct ramlog_ctrl __iomem *ctrl;
static struct ramlog_hdr __iomem *hdr;
static char __iomem *data;
static DEFINE_SPINLOCK(ramlog_lock);

/* read by arch/arm/mach-msm/restart.c */
int ef44_force_recovery;
extern void *restart_reason;

static int autorecovery_secs;

/* writing 0 (e.g. from init on sys.boot_completed) disarms it right away */
static int autorecovery_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (!ret && autorecovery_secs <= 0)
		ef44_force_recovery = 0;
	return ret;
}

static struct kernel_param_ops autorecovery_ops = {
	.set	= autorecovery_set,
	.get	= param_get_int,
};
module_param_cb(autorecovery_secs, &autorecovery_ops, &autorecovery_secs, 0644);

static void ramlog_write(struct console *con, const char *s, unsigned int n)
{
	unsigned long flags;
	u32 pos;

	spin_lock_irqsave(&ramlog_lock, flags);
	pos = readl(&hdr->pos);
	while (n--) {
		writeb(*s++, data + pos);
		if (++pos == DATA_SIZE) {
			pos = 0;
			writel(1, &hdr->wrapped);
		}
	}
	writel(pos, &hdr->pos);
	spin_unlock_irqrestore(&ramlog_lock, flags);
}

static struct console ramlog_console = {
	.name	= "ef44ramlog",
	.write	= ramlog_write,
	.flags	= CON_PRINTBUFFER | CON_ENABLED | CON_ANYTIME,
	.index	= -1,
};

/* called from the board's .reserve hook; a no-op unless the window is RAM */
void __init ef44_ramlog_reserve(void)
{
	memblock_remove(RAMLOG_PHYS, RAMLOG_SIZE);
}

static int __init ef44_ramlog_init(void)
{
	u32 cur = 0, boots = 0;
	char __iomem *slot;

	ctrl = ioremap_nocache(RAMLOG_PHYS, RAMLOG_SIZE);
	if (!ctrl)
		return -ENOMEM;
	if (readl(&ctrl->magic) == RAMLOG_MAGIC) {
		cur = readl(&ctrl->cur) ? 0 : 1;
		boots = readl(&ctrl->boots);
	}
	slot = (char __iomem *)(ctrl + 1) + cur * SLOT_SIZE;
	hdr = (struct ramlog_hdr __iomem *)slot;
	data = slot + sizeof(*hdr);
	writel(boots + 1, &hdr->boot);
	writel(0, &hdr->pos);
	writel(0, &hdr->wrapped);
	writel(cur, &ctrl->cur);
	writel(boots + 1, &ctrl->boots);
	writel(RAMLOG_MAGIC, &ctrl->magic);
	register_console(&ramlog_console);
	return 0;
}
early_initcall(ef44_ramlog_init);

static void autorecovery_fn(struct work_struct *work)
{
	if (autorecovery_secs <= 0) {
		ef44_force_recovery = 0;
		pr_info("ef44_ramlog: autorecovery cancelled\n");
		return;
	}
	pr_emerg("ef44_ramlog: boot not confirmed after %d s, rebooting to recovery\n",
		 autorecovery_secs);
	kernel_restart("recovery");
}
static DECLARE_DELAYED_WORK(autorecovery_work, autorecovery_fn);

static int __init ef44_autorecovery_init(void)
{
	if (autorecovery_secs <= 0)
		return 0;
	ef44_force_recovery = 1;
	if (restart_reason)
		__raw_writel(RECOVERY_REASON, restart_reason);
	schedule_delayed_work(&autorecovery_work, autorecovery_secs * HZ);
	pr_info("ef44_ramlog: autorecovery armed for %d s\n", autorecovery_secs);
	return 0;
}
late_initcall_sync(ef44_autorecovery_init);

/*
 * Emergency restart: the phone has no hardware reset combination, so holding
 * the power key for 8 seconds restarts into recovery from the kernel,
 * whatever userspace is doing (e.g. screen off and the power key unmapped).
 */
#define LONGPRESS_SECS	8

static void longpress_fn(struct work_struct *work)
{
	pr_emerg("ef44_ramlog: power key held %d s, restarting to recovery\n",
		 LONGPRESS_SECS);
	ef44_force_recovery = 1;
	if (restart_reason)
		__raw_writel(RECOVERY_REASON, restart_reason);
	kernel_restart("recovery");
}
static DECLARE_DELAYED_WORK(longpress_work, longpress_fn);

static void longpress_event(struct input_handle *handle, unsigned int type,
			    unsigned int code, int value)
{
	if (type != EV_KEY || code != KEY_POWER)
		return;
	/*
	 * Input events arrive in the pwrkey interrupt handler: 3.4's
	 * cancel_delayed_work() uses del_timer_sync(), which must not run
	 * there (it warned and could lock up the CPU on a key release, so the
	 * screen never came back on); __cancel_delayed_work() does not wait.
	 */
	if (value == 1)
		schedule_delayed_work(&longpress_work, LONGPRESS_SECS * HZ);
	else if (value == 0)
		__cancel_delayed_work(&longpress_work);
}

static int longpress_connect(struct input_handler *handler,
			     struct input_dev *dev,
			     const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;
	handle->dev = dev;
	handle->handler = handler;
	handle->name = "ef44_longpress";
	ret = input_register_handle(handle);
	if (ret)
		goto err_free;
	ret = input_open_device(handle);
	if (ret)
		goto err_unregister;
	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return ret;
}

static void longpress_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id longpress_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(KEY_POWER)] = BIT_MASK(KEY_POWER) },
	},
	{ },
};

static struct input_handler longpress_handler = {
	.event		= longpress_event,
	.connect	= longpress_connect,
	.disconnect	= longpress_disconnect,
	.name		= "ef44_longpress",
	.id_table	= longpress_ids,
};

static int __init ef44_longpress_init(void)
{
	return input_register_handler(&longpress_handler);
}
late_initcall(ef44_longpress_init);
