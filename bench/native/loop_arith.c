#include <stdio.h>

int main(void) {
  long sum = 0;
  for (long i = 0; i < 10000000; i++) sum += i % 7;
  printf("%ld\n", sum);
  return 0;
}
