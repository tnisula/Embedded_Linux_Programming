#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

/************************************************************************
 * Local defines
 */

#define dprint(msg, ...) printk(KERN_ALERT MODULE_NAME ": " msg, ##__VA_ARGS__)
#define file_to_dev(filp) \
  (container_of((filp)->f_inode->i_cdev, struct this_device, cdev))

/************************************************************************
 * Module
 */

MODULE_LICENSE("Dual BSD/GPL");
#define MODULE_NAME "ocfifo"

static struct class *class;
static dev_t first_devnum;
static int n_devices = 1;
module_param(n_devices, int, 0444);

/************************************************************************
 * Devices
 */

struct this_device {
  struct cdev cdev;
  struct device *dev;
  char fifo;
  bool full;
  wait_queue_head_t readq;
  wait_queue_head_t writeq;
};

static struct this_device *devices;

/************************************************************************
 * Fileops
 */

static int dev_open(struct inode *inode, struct file *filp)
{
  return 0;
}

static loff_t dev_llseek(struct file *filp, loff_t offs, int whence)
{
  return offs;
}

static ssize_t dev_read(
    struct file *filp, char __user *ubuff, size_t len, loff_t *offs)
{
  struct this_device *dev = file_to_dev(filp);
  dprint("read %p\n", dev);

  int ret = wait_event_interruptible_exclusive(dev->readq, dev->full);

  if (ret) {
    return ret;
  }

  int uncopied = copy_to_user(ubuff, &dev->fifo, 1);

  if (uncopied) {
    ret = -EFAULT;
  } else {
    ret = 1;
    dev->full = 0;
    wake_up_interruptible(&dev->writeq);
    ++*offs;
  }

  return ret;
}

static ssize_t dev_write(struct file *filp, const char __user *ubuff, size_t len, loff_t *offs)
{
  struct this_device *dev = file_to_dev(filp);
  dprint("write %p\n", dev);

  int ret = wait_event_interruptible_exclusive(dev->writeq, !dev->full);

  if (ret) {
    return ret;
  }

  int uncopied = copy_from_user(&dev->fifo, ubuff, 1);

  if (uncopied) {
    ret = -EFAULT;
  } else {
    ret = 1;
    dev->full = 1;
    wake_up_interruptible(&dev->readq);
    ++*offs;
  }

  return ret;
}

static int dev_release(struct inode *inode, struct file *filp)
{
  return 0;
}

static struct file_operations fileops = {
  .owner = THIS_MODULE,
  .open = dev_open,
  .llseek = dev_llseek,
  .read = dev_read,
  .write = dev_write,
  .release = dev_release
};

/************************************************************************
 * Init and exit
 */

static int demo_init(void)
{
  int err = 0;

  dprint("init\n");

  // Reserve space for devices
  devices = kmalloc(sizeof(struct this_device) * n_devices, GFP_KERNEL);
  if (!devices) {
    dprint("No memory for devices.");
    return -ENOMEM;
  }
  memset(devices, 0, sizeof(struct this_device) * n_devices);

  // Create device class
  class = class_create(THIS_MODULE, MODULE_NAME);

  // Allocate device numbers
  err = alloc_chrdev_region(&first_devnum, 0, n_devices, MODULE_NAME);
  if (err) {
    dprint("chrdev alloc error %d\n", err);
    goto devnum_fail;
  }

  // Create devices
  int major = MAJOR(first_devnum);
  int minor = MINOR(first_devnum);
  
  int d;

  for (d = 0; d < n_devices; ++d) {
    int devnum = MKDEV(major, minor+d);
    dprint("creating chrdev %d:%d\n", MAJOR(devnum), MINOR(devnum));

    devices[d].full = 0;
    init_waitqueue_head(&devices[d].readq);
    init_waitqueue_head(&devices[d].writeq);
    
    dprint("cdev_init dev %p\n", &devices[d]);

    cdev_init(&devices[d].cdev, &fileops);
    devices[d].cdev.owner = THIS_MODULE;

    dprint("cdev_add\n");
    
    err = cdev_add(&devices[d].cdev, devnum, 1);
    if (err) {
      dprint("cdev add error %d\n", err);
      goto dev_add_fail;
    }

    dprint("device_create\n");

    devices[d].dev = 
      device_create(class, NULL, devnum, NULL, MODULE_NAME "%d", d);
    if (IS_ERR(devices[d].dev)) {
      err = PTR_ERR(devices[d].dev);
      dprint("device create error %d\n", err);
      goto dev_create_fail;
    }
  }

  // All OK
  return 0;

 dev_create_fail:
  // Remove last cdev
  cdev_del(&devices[d].cdev);

 dev_add_fail:
  // unroll device adds (failed device was d)
  for (--d; d >= 0; --d) {
    device_destroy(class, MKDEV(MAJOR(first_devnum), MINOR(first_devnum) + d));
    cdev_del(&devices[d].cdev);
  }

  unregister_chrdev_region(first_devnum, n_devices);  

 devnum_fail:
  class_destroy(class);

  kfree(devices);
  devices = NULL;

  return err;
}

static void demo_exit(void)
{
  dprint("exit\n");
 
  for (int d = n_devices - 1; d >= 0; --d) {
    dprint("device destroy %d:%d\n", MAJOR(first_devnum), MINOR(first_devnum) + d);
    device_destroy(class, MKDEV(MAJOR(first_devnum), MINOR(first_devnum) + d));
    dprint("cdev del %p\n", &devices[d]);
    cdev_del(&devices[d].cdev);
  }

  dprint("unregister devnums\n");
  unregister_chrdev_region(first_devnum, n_devices);

  dprint("destroy class\n");
  class_destroy(class);
  class = NULL;

  dprint("free devices\n");
  if (devices) {
    kfree(devices);
    devices = NULL;
  }
}

module_init(demo_init);
module_exit(demo_exit);
