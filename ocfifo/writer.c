#include <stdio.h>
#include <errno.h>

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Needs the name argument");
    return 0;
  }

  FILE *f = fopen("/dev/ocfifo0", "w");
  if (!f) {
    printf("Needs access to the file\n");
    return 0;
  }

  setvbuf(f, NULL, _IONBF, 0);

  int err;

  for (char c = 'a'; c <= 'z'; ++c) {
    err = fwrite(&c, 1, 1, f);
    if (err < 0) {
      fprintf(stderr, "%s: error %d\n", argv[1], err);
      break;
    } else {
      fprintf(stderr, "%s: wrote %c\n", argv[1], c);
    }
  }

  fclose(f);

  return 0;
}
