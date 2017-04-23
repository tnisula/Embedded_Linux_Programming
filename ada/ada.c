/* Adafruit 1110 LCD and button driver
 * built on mcp23s08 GPIO driver.
 * (c) Lauri Pirttiaho, 2014
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/workqueue.h>
#include <linux/i2c.h>
#include <linux/spi/mcp23s08.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/delay.h>

/************************************************************
 * Module config
 */

#define MODULE_NAME "ada"

/************************************************************
 * Module data
 */

static int gpiobase = 240;

module_param(gpiobase, int, 0444);

/************************************************************
 * GPIO
 */

#define IN(pin) gpio_request_one(gpiobase+(pin), GPIOF_IN, #pin)
#define DIN(pin) gpio_direction_input(gpiobase+(pin))
#define OUT(pin) gpio_request_one(gpiobase+(pin), GPIOF_OUT_INIT_HIGH, #pin)
#define OUTL(pin) gpio_request_one(gpiobase+(pin), GPIOF_OUT_INIT_LOW, #pin)
#define DOUT(pin) gpio_direction_output(gpiobase+(pin))
#define FREE(pin) gpio_free(gpiobase+(pin))

#define SET(pin, value) gpio_set_value_cansleep(gpiobase+(pin), (value))
#define GET(pin) gpio_get_value_cansleep(gpiobase+(pin))

/************************************************************
 * I2C client and mcp23s08 driver
 */

static struct i2c_adapter* bus1;
static struct i2c_client* cli32;

static void ioexpander_init(void)
{
  // This should detect errors and return them!

  struct mcp23s08_platform_data mcp23017_pfdata = {
    .chip = {
      [0] = {
	.is_present = 1,
	.pullups = 0x001F /* pins [4:0] */
      }
    },
    .base = gpiobase
  };

  struct i2c_board_info mcp23017_info = {
    .type = "mcp23017",
    .addr = 32,
    .platform_data = &mcp23017_pfdata
  };

  bus1 = i2c_get_adapter(1);
  cli32 = i2c_new_device(bus1, &mcp23017_info); 
}

static void ioexpander_exit(void)
{
  if (cli32) {
    i2c_unregister_device(cli32);
  }
  if (bus1) {
    i2c_put_adapter(bus1);
  }
}

/************************************************************
 * Button control
 */

// Buttons (5 buttons of the Adafruit 1110)
#define SELECT 0
#define RIGHT  1
#define DOWN   2
#define UP     3
#define LEFT   4

static void buttons_init(void)
{
  IN(SELECT);
  IN(RIGHT);
  IN(DOWN);
  IN(UP);
  IN(LEFT);
}

static void buttons_exit(void)
{
  FREE(SELECT);
  FREE(RIGHT);
  FREE(DOWN);
  FREE(UP);
  FREE(LEFT);
}

/************************************************************
 * Backlights
 */

// LEDs (3 back-light leds of the Adafruit 1110)
#define RED   6
#define GREEN 7
#define BLUE  8

static void bl_init(void)
{
  OUT(RED);
  OUT(GREEN);
  OUT(BLUE);
}

static void bl_exit(void)
{
  DIN(RED);
  DIN(GREEN);
  DIN(BLUE);

  FREE(RED);
  FREE(GREEN);
  FREE(BLUE);
}

static void bl_color_set(int rgb)
{
  if ((rgb & 0x000000ff) > 0x0000007f) {
    SET(BLUE, 0);
  } else {
    SET(BLUE, 1);
  }
  if ((rgb & 0x0000ff00) > 0x00007f00) {
    SET(GREEN, 0);
  } else {
    SET(GREEN, 1);
  }
  if ((rgb & 0x00ff0000) > 0x007f0000) {
    SET(RED, 0);
  } else {
    SET(RED, 1);
  }
}

int bl_color = 0;

static int bl_set(const char *val, const struct kernel_param *kp)
{
  int color;

  int n_read = sscanf(val, "0x%x", &color);
  if (n_read != 1) return -EINVAL;

  bl_color = color;
  bl_color_set(color);

  return 0;
}

static int bl_get(char *val, const struct kernel_param *kp)
{
  return sprintf(val, "0x%08x", bl_color);
}

static struct kernel_param_ops bl_ops = {
  .set = bl_set,
  .get = bl_get
};

module_param_cb(backlight_color, &bl_ops, &bl_color, 0644);

/************************************************************
 * Button scanner
 */

static int buttons_before;

static int button_events;
static spinlock_t button_events_sl;
static wait_queue_head_t but_readq;

module_param(button_events, int, 0644);

static void scan_buttons(void)
{
  int buttons_now = 0;

  for (int i = 0; i < 5; ++i) {
    buttons_now |= GET(i) << i;
  }

  int new_events = (buttons_before & ~buttons_now) & 0x1F;

  spin_lock(&button_events_sl);
  button_events |= new_events;
  spin_unlock(&button_events_sl);

  if (new_events) {
    wake_up_interruptible(&but_readq);
  }

  buttons_before = buttons_now;
}

// Scanning work queue. Timer won't work since GPIO calls
// may block which is not allowed in timer's interrupt
// context!

static struct workqueue_struct* scanner_q;
static struct delayed_work scanner_w;

// Scanning frequency in Hz
#define SCAN_FRQ 50

static void scanner_work(struct work_struct *work)
{
  scan_buttons();
  PREPARE_DELAYED_WORK(&scanner_w, scanner_work);
  queue_delayed_work(scanner_q, &scanner_w, HZ/SCAN_FRQ);
}

static void scanner_init(void)
{
  buttons_before = 0x1F;
  button_events = 0;
  spin_lock_init(&button_events_sl);

  scanner_q = alloc_workqueue(MODULE_NAME "_q", WQ_UNBOUND, 1);
  INIT_DELAYED_WORK(&scanner_w, scanner_work);
  queue_delayed_work(scanner_q, &scanner_w, HZ/SCAN_FRQ);
}

static void scanner_exit(void)
{
  cancel_delayed_work_sync(&scanner_w);
  flush_workqueue(scanner_q);
  destroy_workqueue(scanner_q);
}

/************************************************************
 * Button file ops
 */

static int but_open(struct inode *inode, struct file *filp)
{
  filp->private_data = 0; // Event read flag
  return 0;
}

static ssize_t but_read(
    struct file *filp, char __user *ubuff, size_t len, loff_t *offs)
{
  int ret = 0;
  char buffer[32];

  if (filp->private_data) {
    filp->private_data = 0;
    return 0;
  }

  ret = wait_event_interruptible_exclusive(but_readq, button_events);
  
  // Wake up from interrupts, try again
  if (ret) return -ERESTARTSYS;

  filp->private_data = (void*) 1;
  len = 1;

  // Return first event and clear it
  spin_lock(&button_events_sl);
  for (int i = 0; i < 5; ++i) {
    if (button_events & (1<<i)) {
      buffer[0] = '0'+i;
      button_events &= ~(1<<i);
      break;
    }
  }
  spin_unlock(&button_events_sl);

  int uncopied = copy_to_user(ubuff, buffer, len);

  if (uncopied) {
    ret = -EFAULT;
  } else {
    ret = len;
  }

  return ret;
}

static ssize_t but_write(struct file *filp, const char __user *ubuff, size_t len, loff_t *offs)
{
  return -EPERM;
}

static int but_release(struct inode *inode, struct file *filp)
{
  return 0;
}

static struct file_operations but_fileops = {
  .owner = THIS_MODULE,
  .open = but_open,
  .read = but_read,
  .write = but_write,
  .release = but_release
};

/************************************************************
 * LCD buffer
 */

#define LCD_BUFFER_LENGTH 80

static char lcd_buffer[LCD_BUFFER_LENGTH] =
"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ9876543210zyxwvuts";

static struct lcd_size {
  int characters;
  int lines;
} lcd_size = { 16, 2 };

static int output_display(char *output_buffer, const char *display_buffer)
{
  switch (lcd_size.lines) {
  case 1:
    strncpy(output_buffer, display_buffer, lcd_size.characters);
    output_buffer[lcd_size.characters] = '\n';
    return lcd_size.characters+1;
    break;
  case 2:
    strncpy(output_buffer, display_buffer, lcd_size.characters);
    output_buffer[lcd_size.characters] = '\n';
    strncpy(output_buffer+lcd_size.characters+1, display_buffer+40, lcd_size.characters);
    output_buffer[2 * lcd_size.characters + 1] = '\n';
    return 2 * lcd_size.characters + 2;
    break;
  case 4:
    strncpy(output_buffer, display_buffer, lcd_size.characters);
    output_buffer[lcd_size.characters] = '\n';
    strncpy(output_buffer+lcd_size.characters+1, display_buffer+20, lcd_size.characters);
    output_buffer[2 * lcd_size.characters + 1] = '\n';
    strncpy(output_buffer+2*lcd_size.characters+2, display_buffer+40, lcd_size.characters);
    output_buffer[3 * lcd_size.characters + 2] = '\n';
    strncpy(output_buffer+3*lcd_size.characters+3, display_buffer+60, lcd_size.characters);
    output_buffer[4 * lcd_size.characters + 3] = '\n';
    return 4 * lcd_size.characters + 4;
    break;
  default:
    break;
  }
  return 0;
}

static int size_set(const char *val, const struct kernel_param *kp)
{
  int characters = 0;
  int lines = 0;

  int n_read = sscanf(val, "%dx%d", &characters, &lines);
  if (n_read != 2) return -EINVAL;

  if (lines != 1 && lines != 2 && lines != 4) return -EINVAL;
  if (characters <= 0 || characters > 80) return -EINVAL;
  if (lines*characters > 80) return -EINVAL;

  struct lcd_size *ls = kp->arg;
  ls->characters = characters;
  ls->lines = lines;

  return 0;
}

static int size_get(char *val, const struct kernel_param *kp)
{
  struct lcd_size *ls = kp->arg;
  return sprintf(val, "%dx%d", ls->characters, ls->lines);
}

static int display_set(const char *val, const struct kernel_param *kp)
{
  // Do nothing!
  return -EPERM;
}

static int display_get(char *val, const struct kernel_param *kp)
{
  return output_display(val, (char *)kp->arg);
}

static struct kernel_param_ops size_ops = {
  .set = size_set,
  .get = size_get
};

static struct kernel_param_ops display_ops = {
  .set = display_set,
  .get = display_get
};

module_param_cb(lcd_size, &size_ops, &lcd_size, 0644);
module_param_cb(display, &display_ops, lcd_buffer, 0644);

/************************************************************
 * HD44780U (KS0066U) driver
 */

// LCD pins
#define LCD_RS 15
#define LCD_RW 14
#define LCD_E  13
#define LCD_D4 12
#define LCD_D5 11
#define LCD_D6 10
#define LCD_D7  9

static void lcd_write_nybble(int n)
{
  SET(LCD_D4, (n>>0) & 1);
  SET(LCD_D5, (n>>1) & 1);
  SET(LCD_D6, (n>>2) & 1);
  SET(LCD_D7, (n>>3) & 1);
  SET(LCD_E, 1);
  SET(LCD_E, 0);
}

static void lcd_write_byte(int b)
{
  lcd_write_nybble(b>>4);
  lcd_write_nybble(b>>0);
}

static void lcd_write_data(int b)
{
  SET(LCD_RS, 1);
  lcd_write_byte(b);
}

static void lcd_write_cmd(int b)
{
  SET(LCD_RS, 0);
  lcd_write_byte(b);
}

static void lcd_init(void)
{
  OUTL(LCD_RS);
  OUTL(LCD_RW);
  OUTL(LCD_E);
  OUTL(LCD_D4);
  OUTL(LCD_D5);
  OUTL(LCD_D6);
  OUTL(LCD_D7);

  lcd_write_nybble(3);
  mdelay(4);
  lcd_write_nybble(3);
  // lcd writes taek longer than the required delays
  lcd_write_nybble(3);
  lcd_write_nybble(2);

  lcd_write_cmd(0x28); // 2 lines 5x8 font
  lcd_write_cmd(0x0C); // Display on
  lcd_write_cmd(0x06); // Cursor moves right
  lcd_write_cmd(0x01); // Clear
}

static void lcd_exit(void)
{
  FREE(LCD_D7);
  FREE(LCD_D6);
  FREE(LCD_D5);
  FREE(LCD_D4);
  FREE(LCD_E);
  FREE(LCD_RW);
  FREE(LCD_RS);
}

static const int line_starts[] = {0, 64, 20, 84};
static void lcd_copy_line(int line)
{
  lcd_write_cmd(0x80 + line_starts[line]);
  int line_offset = 20;
  if (lcd_size.lines == 2) {
    line_offset = 40;
  }
  for (int i = 0; i < lcd_size.characters; ++i) {
    lcd_write_data(lcd_buffer[i + line * line_offset]);
  }
}

static void lcd_write_to_panel(void)
{
  for (int i = 0; i < lcd_size.lines; ++i) {
    lcd_copy_line(i);
  }
}

/************************************************************
 * Write stream parser
 */

#define NCOLS lcd_size.characters
#define NROWS lcd_size.lines

/* Parser state */

typedef struct write_stream_parser {
  int col;
  int row;
  int clear_from;
  int clear_count;
  int ansi_n;
  int ansi_m;
  const char *buffer;
  size_t len;
  int index;
  void (*state_fn) (struct write_stream_parser *);
} write_stream_parser_t;

/* Parser state function protos */
static void wsp_scroll(write_stream_parser_t *parser);
static void wsp_copy(write_stream_parser_t *parser);
static void wsp_clear(write_stream_parser_t *parser);
static void wsp_csi(write_stream_parser_t *parser);
static void wsp_ansi_n(write_stream_parser_t *parser);
static void wsp_ansi_m(write_stream_parser_t *parser);
static void wsp_ed(write_stream_parser_t *parser);
static void wsp_cup(write_stream_parser_t *parser);

/* Parser state functions */

static void wsp_copy(write_stream_parser_t *parser)
{
  if (parser->buffer[parser->index] == 0x1B /*ESC*/) {
    ++parser->index;
    parser->state_fn = wsp_csi;
    return;
  }

  // State change conditions
  if (parser->buffer[parser->index] == '\n') {
    ++parser->index;
    ++parser->row;
    parser->col = 0;
    return;
  }

  // -- scroll if on last line
  if (parser->row == NROWS) {
    parser->state_fn = wsp_scroll;
    return;
  }

  // In state process
  int lcd_index = parser->col + parser->row * 80 / NROWS;
  lcd_buffer[lcd_index] = parser->buffer[parser->index];
  ++parser->index;
  if (++parser->col == NCOLS) {
    parser->col = 0;
    ++parser->row;
  }
}

static void wsp_scroll(write_stream_parser_t *parser)
{
  switch (NROWS) {
  case 1:
    parser->clear_from = 0;
    parser->clear_count = 80;
    break;
  case 2:
    for (int i = 0; i < 40; ++i) {
      lcd_buffer[i] = lcd_buffer[i+40];
    }
    parser->clear_from = 40;
    parser->clear_count = 40;
    break;
  case 4:
    for (int i = 0; i < 60; ++i) {
      lcd_buffer[i] = lcd_buffer[i+20];
    }
    parser->clear_from = 60;
    parser->clear_count = 20;
    break;
  }
  --parser->row;
  
  parser->state_fn = wsp_clear;
}

static void wsp_clear(write_stream_parser_t *parser)
{
  while (parser->clear_count--) {
    lcd_buffer[parser->clear_from++] = ' ';
  }

  parser->state_fn = wsp_copy;
}

static void wsp_csi(write_stream_parser_t *parser)
{
  // Ignore lone ESC, assume next character is printable
  // even though in real ANSI that isn't really so
  if (parser->buffer[parser->index] != '[') {
    parser->state_fn = wsp_copy;
    return;
  }

  ++parser->index;
  
  parser->ansi_n = 0;
  if (parser->buffer[parser->index] == ';') {
    parser->ansi_n = 1;
  }
  parser->ansi_m = 0;
  parser->state_fn = wsp_ansi_n;  
}

static void wsp_ansi_n(write_stream_parser_t *parser)
{
  if ('0' <= parser->buffer[parser->index] &&
      parser->buffer[parser->index] <= '9' ) {
    parser->ansi_n *= 10;
    parser->ansi_n += parser->buffer[parser->index] - '0';
    ++parser->index;
    return;
  }

  if (parser->buffer[parser->index] == ';') {
    ++parser->index;

    if (parser->buffer[parser->index] < '0' ||
	'9' < parser->buffer[parser->index] ) {
      parser->ansi_m = 1;
    }

    parser->state_fn = wsp_ansi_m;
    return;
  }

  if (parser->buffer[parser->index] == 'J') {
    ++parser->index;
    parser->state_fn = wsp_ed;
    return;
  }

  // Others treated as normal text
  parser->state_fn = wsp_copy;
}

static void wsp_ansi_m(write_stream_parser_t *parser)
{
  if ('0' <= parser->buffer[parser->index] &&
      parser->buffer[parser->index] <= '9' ) {
    parser->ansi_m *= 10;
    parser->ansi_m += parser->buffer[parser->index] - '0';
    ++parser->index;
    return;
  }

  if (parser->buffer[parser->index] == 'H') {
    ++parser->index;
    parser->state_fn = wsp_cup;
    return;
  }

  // Others treated as normal text
  parser->state_fn = wsp_copy;
}

static void wsp_ed(write_stream_parser_t *parser)
{
  int lcd_index = parser->col + parser->row * 80 / NROWS;

  switch (parser->ansi_n) {
  case 0:
    // n == 0: clear form cursor to end
    parser->clear_from = lcd_index;
    parser->clear_count = 80 - lcd_index;
    break;
  case 1:
    // n == 1: clear from beginning to cursor
    parser->clear_from = 0;
    parser->clear_count = lcd_index + 1;
    break;
  case 2:
    // n == 2: clear entire screen
    parser->clear_from = 0;
    parser->clear_count = 80;
    break;
  }
  parser->state_fn = wsp_clear;
}

static void wsp_cup(write_stream_parser_t *parser)
{
  // go to n,m (n, m are 1 based)
  int row = parser->ansi_n - 1;
  int col = parser->ansi_m - 1;
  if (row >= NROWS) {
    row = NROWS - 1;
  }
  if (col >= NCOLS) {
    col = NCOLS - 1;
  }
  parser->row = row;
  parser->col = col;

  parser->state_fn = wsp_copy;
}

/* Parser state driver */

static void wsp_init(write_stream_parser_t *parser)
{
  parser->col = 0;
  parser->row = 0;
  parser->len = 0;
  parser->state_fn = wsp_copy;
} 

static void wsp_process_init(write_stream_parser_t *parser, const char *buffer, size_t len)
{
  parser->buffer = buffer;
  parser->len = len;
  parser->index = 0;
} 

static void wsp_process(write_stream_parser_t *parser)
{
  while (parser->index < parser->len) {
    parser->state_fn(parser);
  }
}

/************************************************************
 * LCD file ops
 */

typedef enum {
  DO_READ,
  READ_DONE
} lcd_read_state_e;

typedef struct {
  write_stream_parser_t parser;
  lcd_read_state_e read_state;
} lcd_file_state_t;

static int lcd_open(struct inode *inode, struct file *filp)
{
  lcd_file_state_t *fs = kmalloc(sizeof(lcd_file_state_t), GFP_KERNEL);
  if (!fs) return -ENOMEM;

  filp->private_data = fs;

  fs->read_state = DO_READ;
  wsp_init(&fs->parser);

  return 0;
}

static ssize_t lcd_read(
    struct file *filp, char __user *ubuff, size_t len, loff_t *offs)
{
  lcd_file_state_t *fs = filp->private_data;
  if (fs->read_state == READ_DONE) {
    fs->read_state = DO_READ;
    return 0;
  }

  int ret = 0;
  char *buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);

  if (!buffer) return -ENOMEM;

  int blen = output_display(buffer, lcd_buffer);

  if (blen < len) {
    len = blen;
  }

  int uncopied = copy_to_user(ubuff, buffer, len);

  if (uncopied) {
    ret = -EFAULT;
  } else {
    ret = len;
  }

  kfree(buffer);

  fs->read_state = READ_DONE;

  return ret;
}

static ssize_t lcd_write(struct file *filp, const char __user *ubuff, size_t len, loff_t *offs)
{
  lcd_file_state_t *fs = filp->private_data;

  char *buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
  if (!buffer) return -ENOMEM;

  int ret = 0;

  if (len > PAGE_SIZE) {
    len = PAGE_SIZE;
  }

  int uncopied = copy_from_user(buffer, ubuff, len);

  if (uncopied) {
    ret = -EFAULT;
  } else {
    ret = len;
  }

  wsp_process_init(&fs->parser, buffer, len);
  wsp_process(&fs->parser);
  lcd_write_to_panel();

  kfree(buffer);

  return ret;
}

static int lcd_release(struct inode *inode, struct file *filp)
{
  if (filp->private_data) {
    kfree(filp->private_data);
  }
  return 0;
}

static struct file_operations lcd_fileops = {
  .owner = THIS_MODULE,
  .open = lcd_open,
  .read = lcd_read,
  .write = lcd_write,
  .release = lcd_release
};

/************************************************************
 * Device data
 */

static struct class *class;
static dev_t but_devnum;
static dev_t lcd_devnum;
struct cdev but_cdev;
struct cdev lcd_cdev;
struct device *but_dev;
struct device *lcd_dev;

/************************************************************
 * Init and exit
 */

static int ada_init(void)
{
  int err = 0;

  // Create device class
  class = class_create(THIS_MODULE, MODULE_NAME);

  // Allocate device number
  err = alloc_chrdev_region(&lcd_devnum, 0, 2, MODULE_NAME);
  if (err) {
    goto devnum_fail;
  }
  but_devnum = MKDEV(MAJOR(lcd_devnum), MINOR(lcd_devnum)+1);

  // Create cdev
  cdev_init(&lcd_cdev, &lcd_fileops);
  err = cdev_add(&lcd_cdev, lcd_devnum, 1);
  if (err) {
    goto lcd_dev_add_fail;
  }
  cdev_init(&but_cdev, &but_fileops);
  err = cdev_add(&but_cdev, but_devnum, 1);
  if (err) {
    goto but_dev_add_fail;
  }

  // Create devices to /dev
  lcd_dev = device_create(class, NULL, lcd_devnum, NULL, MODULE_NAME "lcd");
  if (IS_ERR(lcd_dev)) {
    err = PTR_ERR(lcd_dev);
    goto lcd_dev_create_fail;
  }

  but_dev = device_create(class, NULL, but_devnum, NULL, MODULE_NAME "but");
  if (IS_ERR(but_dev)) {
    err = PTR_ERR(but_dev);
    goto but_dev_create_fail;
  }

  // All OK
  init_waitqueue_head(&but_readq);
  ioexpander_init();
  bl_init();
  buttons_init();
  lcd_init();
  scanner_init();

  return 0;

 but_dev_create_fail:
  device_destroy(class, lcd_devnum);

 lcd_dev_create_fail:
  cdev_del(&but_cdev);

 but_dev_add_fail:
  cdev_del(&lcd_cdev);

 lcd_dev_add_fail:
  unregister_chrdev_region(lcd_devnum, 2);  

 devnum_fail:
  class_destroy(class);

  return err;
}

static void ada_exit(void)
{
  printk(KERN_ALERT "---exit\n");  
  
  scanner_exit();
  lcd_exit();
  buttons_exit();
  bl_exit();
  ioexpander_exit();

  device_destroy(class, but_devnum);
  device_destroy(class, lcd_devnum);
  cdev_del(&but_cdev);
  cdev_del(&lcd_cdev); 
  unregister_chrdev_region(lcd_devnum, 2);  
  class_destroy(class);
}

module_init(ada_init);
module_exit(ada_exit);

MODULE_AUTHOR("Lauri Pirttiaho <lapi@cw.fi>");
MODULE_LICENSE("Dual BSD/GPL");
