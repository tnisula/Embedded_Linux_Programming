#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>

// Module vars
MODULE_LICENSE("Dual BSD/GPL");
#define MODULE_NAME "blockdemo"

// Device vars
static struct class *my_class;
static dev_t my_devnum;
static struct cdev my_cdev;
static struct device *my_device;

#define BUFSIZE 256
static char buf[BUFSIZE];


/*
       GPIO
*/
static int gpiobase = 240;
module_param(gpiobase, int, 0444);

#define SELECT (gpiobase+0)
#define RIGHT  (gpiobase+1)
#define RED    (gpiobase+6)
#define GREEN  (gpiobase+7)
#define BLUE   (gpiobase+8)

#define C_IN(pin)  gpio_request_one(pin, GPIOF_IN , #pin) 
#define C_OUTH(pin)  gpio_request_one(pin, GPIOF_OUT_INIT_HIGH, #pin)
#define C_OUTL(pin)  gpio_request_one(pin, GPIOF_OUT_INIT_LOW, #pin)
#define UC(pin) gpio_free(pin)




static void my_gpio_init(void)
{
	C_IN(SELECT);
        C_IN(RIGHT);

        C_OUTH(RED);
        C_OUTH(GREEN);
        C_OUTH(BLUE);
}

static void my_gpio_exit(void)
{
        UC(SELECT);
        UC(RIGHT);

        UC(RED);
        UC(GREEN);
        UC(BLUE);
}

static int pin(char c) 
{
   int pin = RED;
   if(c == 'g') {
      pin = GREEN;
   } else if(c== 'b') {
      pin = BLUE;
   }
   return pin;
}

static inline void set_on(char color)
{
   if(!color) return;
   gpio_set_value(pin(color), 0);
}

static inline void set_off(char color)
{
   if(!color) return;
   gpio_set_value(pin(color), 1);

}


static void process_buffer(void)
{
   int index = 0;
   char color = 0;

   while(buf[index]) {
      switch(buf[index]) {
         case 'r':
         case 'R':
            color = 'r';
            break;
         case 'g':
         case 'G':
            color = 'g';
            break;
         case 'b':
         case 'B':
            color = 'b';
            break;
         case '0':
            set_off(color); 
            color = 0;
            break;
         case '1':
            set_on(color);
            color = 0;   
            break;
         default:
            color = 0;
      }
      ++index;
   }
   memset(buf, 0, BUFSIZE);
}

/*
        File operations
*/
static ssize_t my_read(struct file *filep, char __user *ubuff, size_t len, loff_t *offs) {

      int remaining;
      printk(KERN_ALERT "my_read\n");

      if(*offs + len >= BUFSIZE) {
         len = BUFSIZE - *offs;
         if(len < 0) {
	    len = 0;
         }
      }
      if(!access_ok(VERIFY_WRITE, ubuff, len)) {
         return -EFAULT;
      }

      remaining = copy_to_user(ubuff, buf, len);
      if(remaining) {
         return -EFAULT;
      }
//    *offs += len; 

      return len;  // EOF = 0
}

static ssize_t my_write(struct file *filep, const char __user *ubuff, size_t len, loff_t *offs) {

      int remaining;
      printk(KERN_ALERT "my_write\n");

      if(len >= BUFSIZE) {
         len = BUFSIZE;
      }
      if(!access_ok(VERIFY_READ, ubuff, len)) {
         return -EFAULT;
      }

      remaining = copy_from_user(buf, ubuff, len);
      if(remaining) {
         return -EFAULT;
      }
//    *offs += len;

      process_buffer();
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

static long my_ioctl(struct file *filp, unsigned int cmd,  unsigned long data)
{
       printk(KERN_ALERT "my_ioctl 0x%08x/0x%08lx\n", cmd, data );
       return 0;
}

static struct file_operations my_fileops = {
   .read =  my_read,
   .write = my_write,
   .open =   my_open,
   .release = my_release,
   .unlocked_ioctl = my_ioctl
};

static int hello_init(void) {
  int err;
  printk(KERN_ALERT "Hello, world.\n");

  // 1. create class
  my_class = class_create(THIS_MODULE,  "blockdemo_class");
  if(IS_ERR(my_class)) {
      err=PTR_ERR(my_class);
      printk(KERN_ERR "Error in class creation %d\n", err);
      goto class_create_error;
   }
  // 2. allocate chardev num
  err = alloc_chrdev_region(&my_devnum, 0, 1, "blockdemo_chreg");
  if(err) {
      printk(KERN_ERR "Error in reserving devnum %d\n", err);
      goto chardev_reg_error;
   }

  // printk(KERN_ALERT "Device number reserved %d:%d\n", MAJOR(my_devnum, MINOR(my_devnum));
  printk(KERN_ALERT "Device number reserved %d:%d\n",
	 MAJOR(my_devnum), MINOR(my_devnum));

   // 3: init cdev
   cdev_init(&my_cdev, &my_fileops);
   my_cdev.owner = THIS_MODULE; 

   // 4. add cdev
   err = cdev_add(&my_cdev, my_devnum, 1);
   if(err) {
      printk(KERN_ERR "Error in reserving devnum %d\n", err);
      goto cdev_add_error;
   }

   // 5. create device
   my_device = device_create(my_class, NULL, my_devnum, NULL, "blockdemo_dev");
   if(IS_ERR(my_device)) {
      err=PTR_ERR(my_device);
      printk(KERN_ERR "Error in device creation %d\n", err);
      goto device_create_error;
   }

   my_gpio_init();

   return 0;

device_create_error:
   cdev_del(&my_cdev);

cdev_add_error:
   unregister_chrdev_region(my_devnum, 1);

chardev_reg_error:
  class_destroy(my_class);

class_create_error:

   return err;
}

static void hello_exit(void) {

  printk(KERN_ALERT "Goodbye, cruel world.\n");

  my_gpio_exit();

  device_destroy(my_class, my_devnum);
  cdev_del(&my_cdev);
  unregister_chrdev_region(my_devnum, 1);
  class_destroy(my_class);

}

module_init(hello_init);
module_exit(hello_exit);


