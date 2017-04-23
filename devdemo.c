#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device.h>
#include <linux/fs.h>

MODULE_LICENSE("Dual BSD/GPL");

// Device vars
static struct class* my_class;
static dev_t my_devnum;

// Module parameters
static int my_parameter = 0;

/* Returns 0, or -errno.  arg is in kp->arg. */
static int my_param_set(const char *val, const struct kernel_param *kp)
{
  long l;
  int ret;
  
  ret = kstrtoul(val, 0, &l);
  if (ret < 0 || ((int)l != l))	
    return ret < 0 ? ret : -EINVAL;
  *((int *)kp->arg) = l;

  printk("Hello: New param %ld\n", l);

  return 0;
}

/* Returns length written or -errno.  Buffer is 4k (ie. be short!) */
static int my_param_get(char *buffer, const struct kernel_param *kp)
{
  return scnprintf(buffer, PAGE_SIZE, "%d", *((int *)kp->arg));
}


static struct kernel_param_ops my_ops = {
  .get = my_param_get,
  .set = my_param_set
};

module_param_cb(my_parameter, &my_ops, &my_parameter, S_IRUGO | S_IWUSR);



static int hello_init(void)
{
  int err;
  
  printk(KERN_ALERT "Hello, world. %d\n", my_parameter);

  my_class = class_create(THIS_MODULE, "devdemo");
  err = alloc_chrdev_region(&my_devnum, 0, 1, "devdemo");
  if (err) {
    printk(KERN_ERR "Error in reserving devnum %d\n", err);
  }

  printk(KERN_ALERT "Device number reserved %d:%d\n",
	 MAJOR(my_devnum), MINOR(my_devnum));

  return err;
}

static void hello_exit(void)
{
  printk(KERN_ALERT "Goodbye, cruel world. %d\n", my_parameter);

  unregister_chrdev_region(my_devnum, 1);
  class_destroy(my_class);
}

module_init(hello_init);
module_exit(hello_exit);
