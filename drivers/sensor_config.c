#include <linux/fs.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/semaphore.h>
#include <linux/uaccess.h>
#include "cam_os_wrapper.h"

#define DEVICE_NAME "srcfg"
#define DRV_VERSION "1.0"

static struct semaphore sem;
static unsigned char gsrcfg[64] = {0};

#ifdef SIGMASTAR_MARUKO
extern s32 DrvVifInit(void);
extern s32 DrvVifSetIoPad(u32 nSNRPadID, u32 eBusType, u32 nPara);
extern s32 DrvVifSetMclk(u32 nSNRPadID, u32 eBusType, u8 nOnOff, u32 nMclk);
extern s32 DrvVifSensorPdwn(u32 nSNRPadID, u32 eBusType, u32 ePOL);
extern s32 DrvVifSensorReset(u32 nSNRPadID, u32 eBusType, u32 ePOL);
#else
extern s32 DrvVif_Init(void);
extern s32 DrvVif_SetIOPad(u32 nSNRPadID, u32 eBusType, u32 nPara);
extern s32 DrvVif_SetMclk(u32 nSNRPadID, u8 nOnOff, u32 nMclk);
extern s32 DrvVif_SensorPdwn(u32 nSNRPadID, u32 ePOL);
extern s32 DrvVif_SensorReset(u32 nSNRPadID, u32 ePOL);
#endif

static int do_sensor_cfg(int SnrIdx, int BusType, int LaneNum, int MclkIdx, int Pdwn, int Rst) {
	switch (MclkIdx) {
		case 0:
			MclkIdx = 27000;
			break;
		case 1:
			MclkIdx = 72000;
			break;
		case 2:
			MclkIdx = 61700;
			break;
		case 3:
			MclkIdx = 54000;
			break;
		case 4:
			MclkIdx = 48000;
			break;
		case 5:
			MclkIdx = 43200;
			break;
		case 6:
			MclkIdx = 36000;
			break;
		case 7:
			MclkIdx = 24000;
			break;
		case 8:
			MclkIdx = 21600;
			break;
		case 9:
			MclkIdx = 12000;
			break;
		case 10:
			MclkIdx = 5400;
			break;
		default:
			MclkIdx = 0;
			break;
	}

#ifdef SIGMASTAR_MARUKO
	DrvVifInit();
	DrvVifSetIoPad(SnrIdx, BusType, LaneNum);
	DrvVifSetMclk(SnrIdx, BusType, MclkIdx ? 1 : 0, MclkIdx);
	DrvVifSensorPdwn(SnrIdx, BusType, Pdwn ? 1 : 0);
	DrvVifSensorReset(SnrIdx, BusType, Rst ? 1 : 0);
#else
	DrvVif_Init();
	DrvVif_SetIOPad(SnrIdx, BusType, 0);
	DrvVif_SetMclk(SnrIdx, MclkIdx ? 1 : 0, MclkIdx);
	DrvVif_SensorPdwn(SnrIdx, Pdwn ? 1 : 0);
	DrvVif_SensorReset(SnrIdx, Rst ? 1 : 0);
#endif

	printk("Sensor configuration: ID=%u, Bus=%u, Lane=%u, MCLK=%u, PWD_PIN=%u, RST_PIN=%u\n",
		SnrIdx, BusType, LaneNum, MclkIdx, Pdwn, Rst);

	return 0;
}

static int srcfg_open(struct inode *inode, struct file *file) {
	return 0;
}

static int srcfg_close(struct inode *inode, struct file *file) {
	return 0;
}

static long srcfg_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
	return 0;
}

static ssize_t srcfg_read(struct file *filp, char __user *buff, size_t count, loff_t *offp) {
	int ret = 0;

	if (down_interruptible(&sem)) {
		return -ERESTARTSYS;
	}

	if (strlen(gsrcfg)) {
		printk("%s", gsrcfg);
	}

	up(&sem);
	return ret;
}

static ssize_t srcfg_write(struct file *filp, const char __user *buff, size_t count, loff_t *ppos) {
	int ret = 0;
	void __user *argp = NULL;
	int help = 0;
	int SnrIdx = -1;
	int BusType = -1;
	int LaneNum = -1;
	int MclkIdx = -1;
	int Pdwn = -1;
	int Rst = -1;
	char srcfg[64] = {0};

	if (down_interruptible(&sem)) {
		return -ERESTARTSYS;
	}

	argp = (void __user *)buff;

	memset(gsrcfg, 0, sizeof(gsrcfg));
	if (copy_from_user(gsrcfg, argp, count)) {
		ret = -EFAULT;
	} else {
		*ppos += count;
		ret = count;
	}

	if (memcmp(gsrcfg, "help", 4) == 0) {
		help = 1;
	} else {
		memset(srcfg, 0, sizeof(srcfg));
		sscanf(gsrcfg, "%s %d %d %d %d %d %d",
			srcfg, &SnrIdx, &BusType, &LaneNum, &MclkIdx, &Pdwn, &Rst);
	}
	up(&sem);

	if (help) {
		printk("SensorId BusType MclkIdx PwdPin RstPin\n"
			" -SensorId Select sensor ID 0~8\n"
			" -BusType Select sensor bus type 0:Parallel, 1:MIPI, 2:BT601, 3:BT656, 4:BT1120, 5:LVDS\n"
			" -LaneNum Lane number\n"
			" -MclkIdx 0:27MHz, 1:72MHz, 2:61.7MHz, 3:54MHz, 4:48MHz, 5:43.2MHz, 6:36MHz, 7:24MHz, 8:21.6MHz, 9:12MHz, 10:5.4MHz, 999:MCLK off\n"
			" -PwdPin sensor power down pin 0:low 1:high\n"
			" -RstPin sensor reset pin 0:low 1:high\n");
	} else {
		do_sensor_cfg(SnrIdx, BusType, LaneNum, MclkIdx, Pdwn, Rst);
	}

	return ret;
}

static struct file_operations srcfg_fops = {
	.owner = THIS_MODULE,
	.read = srcfg_read,
	.write = srcfg_write,
	.open = srcfg_open,
	.release = srcfg_close,
	.unlocked_ioctl = srcfg_ioctl,
};

static struct miscdevice srcfg_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_NAME,
	.fops = &srcfg_fops,
};

static int __init srcfg_init(void) {
	int ret = 0;

	sema_init(&sem, 1);
	ret = misc_register(&srcfg_miscdev);
	if (ret < 0)  {
		printk("Cannot register driver %s\n", DEVICE_NAME);
		return ret; 
	}

	return 0;
}

static void __exit srcfg_exit(void) {
	misc_deregister(&srcfg_miscdev);
}

module_init(srcfg_init);
module_exit(srcfg_exit);

MODULE_DESCRIPTION("srcfg driver");
MODULE_AUTHOR("SigmaStar");
MODULE_LICENSE("GPL");
