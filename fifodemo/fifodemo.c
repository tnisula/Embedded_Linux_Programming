#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>

// Module vars
MODULE_LICENSE("Dual BSD/GPL");

// Device vars
static struct class *my_class;
static dev_t my_devnum;
static struct cdev my_cdev;
static struct device *my_device;

#define BUFSIZE 256
static char buf[BUFSIZE];
static int read_index = 0;
static int write_index = 0;

/*
	Module parameter
*/
static int my_parameter = 0; 

/* Returns 0, or -errno.  arg is in kp->arg. */
static int my_param_set(const char *val, const struct kernel_param *kp) {
   long l;
   int ret;

   ret = kstrtoul(val, 0, &l);
   if (ret < 0 || ((int)l != l))
      return ret < 0 ? ret : -EINVAL;
   *((int *)kp->arg) = l;
   printk("Fifodemo:  New param %ld\n", l);
   return 0;
}

/* Returns length written or -errno.  Buffer is 4k (ie. be short!) */
static int my_param_get(char *buffer, const struct kernel_param *kp) {
   return scnprintf(buffer, PAGE_SIZE, "%d", *((int *)kp->arg));
}

static struct kernel_param_ops my_ops = {
   .get = my_param_get,
   .set = my_param_set
};

module_param_cb(my_parameter, &my_ops, &my_parameter, S_IRUGO | S_IWUSR);

/*
        File operations
*/
static ssize_t my_read(struct file *filep, char __user *ubuff, size_t len, loff_t *offs) {

      int remaining;
      int available;

      printk(KERN_ALERT "my_read\n");

      available = write_index - read_index;
      if(available < 0) {
          available += BUFSIZE;
      }
      if(len > available) {
         len = available;
      }
      if (len > BUFSIZE - read_index) {
         len = BUFSIZE - read_index;
      }

      if(!access_ok(VERIFY_WRITE, ubuff, len)) {
         return -EFAULT;
      }

      remaining = copy_to_user(ubuff, buf + read_index, len);
      if(remaining) {
         return -EFAULT;
      }

      read_index += len;
      if(read_index >= BUFSIZE) {
         read_index -= BUFSIZE;
      }

      *offs += len;

      printk(KERN_ALERT "my_read got %d\n", len);

      return len;  // EOF = 0
}

static ssize_t my_write(struct file *filep, const char __user *ubuff, size_t len, loff_t *offs) {

      int remaining;
      int space_left;

      printk(KERN_ALERT "my_write\n");

      space_left = BUFSIZE - 1 - write_index + read_index;

      if (space_left > BUFSIZE) { 
         space_left -= BUFSIZE;
      }

      if (len > space_left) {
        len = space_left;
      }

      if (len > BUFSIZE - write_index) {
         len = BUFSIZE - write_index;
      }

      if(!access_ok(VERIFY_READ, ubuff, len)) {
         return -EFAULT;
      }

      remaining = copy_from_user(buf+write_index, ubuff, len);
      if(remaining) {
         return -EFAULT;
      }

      write_index += len;
      if (write_index >= BUFSIZE) { 
         write_index -= BUFSIZE;
      } 

      *offs += len;
      printk(KERN_ALERT "my_write got %d\n", len);

      return len; 
}

static int my_open(struct inode *inode, struct file *filep) {
       printk(KERN_ALERT "my_open\n"); 
       return 0;
}

static int my_release(struct inode *inode, struct file *filep) {
       printk(KERN_ALERT "my_release\n");
       return 0;
}

static struct file_operations my_fileops = {
   .read =  my_read,
   .write = my_write,
   .open =   my_open,
   .release = my_release
};



static int hello_init(void) {
  int err;
  printk(KERN_ALERT "Hello, fifodemo. %d\n", my_parameter);

  // 1. create class
  my_class = class_create(THIS_MODULE,  "fifodemo_class");

  // 2. allocate chardev num
  err = alloc_chrdev_region(&my_devnum, 0, 1, "fifodemo_chreg");
  if(err) {
	printk(KERN_ERR "Error in reserving fifodevnum %d\n", err);
  }

  // printk(KERN_ALERT "Device number reserved %d:%d\n", MAJOR(my_devnum, MINOR(my_devnum));
  printk(KERN_ALERT "Device number reserved %d:%d\n",
	 MAJOR(my_devnum), MINOR(my_devnum));

  // 3: init cdev
  cdev_init(&my_cdev, &my_fileops);
  my_cdev.owner = THIS_MODULE; 

  // 4. add cdev
  cdev_add(&my_cdev, my_devnum, 1);

  // 5. create device
  my_device = device_create(my_class, NULL, my_devnum, NULL, "fifodemo_dev");

  return err;
}

static void hello_exit(void) {

  printk(KERN_ALERT "Goodbye, cruel fifodemo world. %d\n", my_parameter);

  device_destroy(my_class, my_devnum);
  cdev_del(&my_cdev);
  unregister_chrdev_region(my_devnum, 1);
  class_destroy(my_class);

}

module_init(hello_init);
module_exit(hello_exit);


