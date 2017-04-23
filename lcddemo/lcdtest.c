#include <stdio.h>
#include <stropts.h>

int main(int argc, char argv[])
{
   char buf[256];
   FILE *f = fopen("/dev/lcddemo_dev", "r+");
   if(!f) {
      printf("file not found!\n");
      return -1;
   }
   // ioctl(fileno(f), 2, 40);
   // fprintf(f, "Timon testi");
   fread(buf, 1, 16, f);

   printf("%s\n", buf);
 
   fclose(f);
   return 0;
}
