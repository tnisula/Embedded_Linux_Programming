#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Need two arguments\n");
    return 1;
  }

  int len = strlen(argv[1]);
  char *buffer = (char *)malloc(len+1);

  if (!buffer) { 
    fprintf(stderr, "No memory\n");
    return 2;
  }

  int count = 0;

  while (1) {
    if (++count == 10000) {
      count = 0;
      fprintf(stderr, "%s", argv[2]);
    }
    FILE *f = fopen("/dev/rwdemo", "r+");
    fseek(f, 0, SEEK_SET);
    fwrite(argv[1], len+1, 1, f);
    fseek(f, 0, SEEK_SET);
    fread(buffer, len+1, 1, f);
    fclose(f);
    if (strncmp(argv[1], buffer, len)) {
      fprintf(stderr, "Mismatch: %s != %s\n", argv[1], buffer);
    }
  }

}
