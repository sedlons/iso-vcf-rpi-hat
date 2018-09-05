#include <linux/init.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/io.h>


MODULE_AUTHOR("Robert Sedlacek <sedlons@wernherd.cz>");
MODULE_DESCRIPTION("Linux Kernel Module for 5ch iso voltage measuring shield");
MODULE_LICENSE("GPL");

/***********************************************************************************************************************/
/* Definitions */
/***********************************************************************************************************************/
/* Name prefix of device */
#define DEVICE_NAME_PREFIX "isov"

/* GPIO definition*/
#define GPIO_VCF1 2
#define GPIO_VCF2 22
#define GPIO_VCF3 10
#define GPIO_VCF4 5
#define GPIO_VCF5 26

/* BCM2708 1 MHz timer address */
#define TIMER_PAGE_BASE   0x3F003000   //PERI_BASE
#define TIMER_OFFSET      4

/***********************************************************************************************************************/
/* Macros */
/***********************************************************************************************************************/


/***********************************************************************************************************************/
/* Declarations */
/***********************************************************************************************************************/
/* */
static int isov_misc_open(struct inode *inode, struct file *file);

/* */
static int isov_misc_release(struct inode *inode, struct file *file);

/* */
static ssize_t isov_misc_write(struct file *file, const char __user *buf, size_t count, loff_t *offset);

/* Called when user read from miscellaneous device */
static ssize_t isov_misc_read(struct file *file, char __user *buf, size_t count, loff_t *offset);

/***********************************************************************************************************************/
/* Variables */
/***********************************************************************************************************************/
/* GPIO signals array */
static struct gpio vcfGPIOSignals[] = {
	{ GPIO_VCF1, GPIOF_IN, "Voltage to freq 1" },
	{ GPIO_VCF2, GPIOF_IN, "Voltage to freq 2" },
	{ GPIO_VCF3, GPIOF_IN, "Voltage to freq 3" },
	{ GPIO_VCF4, GPIOF_IN, "Voltage to freq 4" },
	{ GPIO_VCF5, GPIOF_IN, "Voltage to freq 5" }
};

/* Assigned IRQ numbers */
static int vcf_irqs[] = { -1, -1, -1, -1, -1 };

/* BCM2708 1 MHz timer address */
#define TIMER_PAGE_BASE   0x3F003000   //PERI_BASE
#define TIMER_OFFSET      4

/* Timer pointer */
volatile unsigned *(timer) = 0;

/* Time of last pulse for each channel */
u32 last_pulse_time[] = {0, 0, 0, 0, 0};

/* Calibration coeficients for each channel */
static int calib_coeficients[] = {7692308, 7692308, 7692308, 7692308, 7692308};

/* Measured voltages */
static int voltages[] = {0, 0, 0, 0, 0};

/* File operations of miscellaneous device */
static struct file_operations isov_fops = {
	.owner = THIS_MODULE,
	.open = isov_misc_open,
	.read = isov_misc_read,
	.write = isov_misc_write,
	.release = isov_misc_release
};

/* Miscellaneous device */
static struct miscdevice isov_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_NAME_PREFIX,
	.fops = &isov_fops
};

/***********************************************************************************************************************/
/* Interrupts */
/***********************************************************************************************************************/
/* Interrupt handler called at falling edge */
static irqreturn_t vcf_isr(int irq, void *data) {
	static u32 time_diff = 0;
	for(int i=0; i<ARRAY_SIZE(vcf_irqs); i++) {
		if(irq == vcf_irqs[i]) {
			/* Get time difference between now and last pulse */
			time_diff = ioread32(timer) - last_pulse_time[i];

			/* Save now */
			last_pulse_time[i] = ioread32(timer);

			/* Accept result only if time between pulses is less than 1s */	
			if(time_diff < 1000000) {
				voltages[i] = calib_coeficients[i] / time_diff;
			}
		}
	}
	return IRQ_HANDLED;
}

/***********************************************************************************************************************/
/* Functions */
/***********************************************************************************************************************/
/*  */
static int isov_misc_open(struct inode *inode, struct file *file) {
	    return nonseekable_open(inode, file);
}

/* */
static int isov_misc_release(struct inode *inode, struct file *file) {
	    return 0;
}

/* */
static ssize_t isov_misc_write(struct file *file, const char __user *buf, size_t count, loff_t *offset) {
		return -EINVAL;
}

/* Called when user read from miscellaneous device */
static ssize_t isov_misc_read(struct file *file, char __user *buf, size_t count, loff_t *offset){
	static char txt_buffer[256];
	size_t txt_len = 0;
	size_t len;

	if(txt_len - *offset <= 0) {
		/* Write all voltages to text buffer */
		txt_len = snprintf(txt_buffer, 256, "V1=%d V2=%d V3=%d V4=%d V5=%d \n", voltages[0], voltages[1], voltages[2], voltages[3], voltages[4]);

		/* Increment because of zero end */
		txt_len++;
	}

	/* Get minimum size */
	len = (txt_len - *offset > count) ? count : txt_len - *offset;


	if(len <= 0) {
		printk(KERN_ERR "ISOV: Sprintf not write");
		return -ENODATA;
	}

	/* Copy text buffer data to user */
	if(copy_to_user(buf, txt_buffer + *offset, len+1) != 0) {
		printk(KERN_ERR "ISOV: Error writing to misc char device");
		return -EFAULT;
	}


	return len;
}


/***********************************************************************************************************************/
/* Module related functions */
/***********************************************************************************************************************/
/* Module initialization */
static int __init isov_init(void){
	/* Register miscellaneous device */
	misc_register(&isov_misc_device);

	/* Register GPIOs*/
	if(gpio_request_array(vcfGPIOSignals, ARRAY_SIZE(vcfGPIOSignals))) {
		gpio_free_array(vcfGPIOSignals, ARRAY_SIZE(vcfGPIOSignals));
		return -ENODEV;
	}

#if 1

	/* Register IRQs */
//	for(int i=0; i<ARRAY_SIZE(vcf_irqs); i++) 
	{ int i  = 0;
		vcf_irqs[i] = gpio_to_irq(vcfGPIOSignals[i].gpio);
		if(vcf_irqs[i] < 0) {
			/* Unregister already registered IRQs */
			for(i=i-1; i>=0; i--) {
				free_irq(vcf_irqs[i], NULL);
			}
			gpio_free_array(vcfGPIOSignals, ARRAY_SIZE(vcfGPIOSignals));
			return -ENODEV;
		}
	}

	/* Map timer */
	timer = ioremap(TIMER_PAGE_BASE+TIMER_OFFSET, 8);

	/* Request IRQ */
	{
		//TODO: Dodelat for
		int i = 0;
		if(request_irq(vcf_irqs[i], vcf_isr, IRQF_TRIGGER_FALLING, "isov#vcf1", NULL)) {
			for(int i = 0; i<ARRAY_SIZE(vcfGPIOSignals); i++) {
				free_irq(vcf_irqs[i], NULL);
			}
			gpio_free_array(vcfGPIOSignals, ARRAY_SIZE(vcfGPIOSignals));
			iounmap(timer);
			return -ENODEV;
		}
	}

#endif

	printk(KERN_INFO "ISOV: GPIO settings ok.");

	return 0;
}

/* Exit */
static void __exit isov_exit(void){
	printk(KERN_INFO "ISOV: GPIO unregistering.");
	
	/* Unmap timer */
	if (timer) {
		iounmap(timer);
	}

	/* Unregister IRQs */
	for(int i=0; i<ARRAY_SIZE(vcf_irqs); i++) {
		free_irq(vcf_irqs[i], NULL);	
	}

	/* Unregister GPIO */
	gpio_free_array(vcfGPIOSignals, ARRAY_SIZE(vcfGPIOSignals));

	/* Unregister miscellaneous device */
	misc_deregister(&isov_misc_device);
}

module_init(isov_init);
module_exit(isov_exit);

