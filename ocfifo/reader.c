#include <stdio.h>
#include <errno.h>

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Needs the name argument");
    return 0;
  }

  FILE *f = fopen("/dev/ocfifo0", "r");
  if (!f) {
    printf("Needs access to the file\n");
    return 0;
  }
  char c;
  int err;

  for (char i = 'a'; i <= 'f'; ++i) {
    err = fread(&c, 1, 1, f);
    if (err < 0) {
      fprintf(stderr, "%s: error %d\n", argv[1], err);
      break;
    } else {
      fprintf(stderr, "%s: read %c\n", argv[1], c);
    }
  }

  fclose(f);

  return 0;
}
