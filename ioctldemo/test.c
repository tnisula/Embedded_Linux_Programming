#include <stdio.h>
#include <errno.h>

int main(int argc, char *argv[])
{
  FILE *f = fopen("/dev/ioctldemo", "r+");
  int err = 0;

  printf("ioctl: 0x12345678, 0x87654321\n");
  err = ioctl(fileno(f), 0x12345678, 0x87654321);
  if (err) {
    printf("Error %d\n", err);
    perror("Error: ");
  }

  printf("ioctl: 0x00005678, 0x87654321\n");
  err = ioctl(fileno(f), 0x00005678, 0x87654321);
  if (err) {
    printf("Error %d\n", err);
    perror("Error: ");
  }

  fclose(f);
}
