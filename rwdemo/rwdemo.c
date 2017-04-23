#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/semaphore.h>

MODULE_LICENSE("Dual BSD/GPL");

#define MODULE_NAME "rwdemo"

#define dprint(msg, ...) printk(KERN_ALERT MODULE_NAME ": " msg, ##__VA_ARGS__)

static char *buffer;
module_param(buffer, charp, 0644);

static struct class *class;
static struct cdev cdev;
static struct device *dev;
static dev_t devnum;

static DEFINE_SEMAPHORE(filesem);
static bool use_sem = 0;
module_param(use_sem, bool, 0644);

static int dev_open(struct inode *inode, struct file *filp)
{
  if (use_sem) {
    down(&filesem);
  }
  return 0;
}

static loff_t dev_llseek(struct file *filp, loff_t offs, int whence)
{
  if (whence) { // Relative SEEK_CUR
    filp->f_pos += offs;
  } else { // Absolute SEEK_SET
    filp->f_pos = offs;
  }

  return filp->f_pos;
}

static ssize_t dev_read(struct file *filp, char __user *ubuff, size_t len, loff_t *offs)
{
  if (*offs < 0) {
    return -EFAULT;
  }

  if (*offs >= PAGE_SIZE) {
    return 0; //EOF
  }

  if (*offs + len >= PAGE_SIZE) {
    len = PAGE_SIZE - *offs;
  }

  int uncopied = copy_to_user(ubuff, buffer+*offs, len);
  
  if (uncopied) {
    return -EFAULT;
  }

  *offs += len;

  return len;
}

static ssize_t dev_write(struct file *filp, const char __user *ubuff, size_t len, loff_t *offs)
{
  if (*offs < 0 || *offs + len >= PAGE_SIZE) {
    return -EFAULT;
  }

  int uncopied = copy_from_user(buffer+*offs, ubuff, len);
  
  if (uncopied) {
    return -EFAULT;
  }

  *offs += len;

  return len;
}

static int dev_release(struct inode *inode, struct file *filp)
{
  if (use_sem) {
    up(&filesem);
  }
  return 0;
}

static struct file_operations fileops = {
  .open = dev_open,
  .llseek = dev_llseek,
  .read = dev_read,
  .write = dev_write,
  .release = dev_release
};

static int demo_init(void)
{
  dprint("init\n");

  buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
  if (!buffer) {
    return -ENOMEM;
  }
  memset(buffer, 0, PAGE_SIZE);

  class = class_create(THIS_MODULE, MODULE_NAME);

  int err = alloc_chrdev_region(&devnum, 0, 1, MODULE_NAME);
  if (err) {
    dprint("chrdev alloc error %d\n", err);
  }

  dprint("chrdev %d:%d\n", MAJOR(devnum), MINOR(devnum));

  cdev_init(&cdev, &fileops);
  cdev.owner = THIS_MODULE;

  err = cdev_add(&cdev, devnum, 1);
  if (err) {
    dprint("cdev add error %d\n", err);
  }
  
  dev = device_create(class, NULL, devnum, NULL, MODULE_NAME);
  if (IS_ERR(dev)) {
    err = PTR_ERR(dev);
    dprint("device create error %d\n", err);
  }

  return 0;
}

static void demo_exit(void)
{

  device_destroy(class, devnum);
  cdev_del(&cdev);

  unregister_chrdev_region(devnum, 1);

  class_destroy(class);
  class = NULL;

  if (buffer) {
    kfree(buffer);
    buffer = NULL;
  }

  dprint("exit\n");
}

module_init(demo_init);
module_exit(demo_exit);
