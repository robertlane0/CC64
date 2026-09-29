/* Measure the target heap from a compiler-produced program.
 *
 * The target maps a fixed identity range and hands the rest to the MCB chain,
 * so the heap size is not a compile-time constant CC64 can see. This program
 * asks the target allocator directly: it claims the largest block it can and
 * reports the size in MiB. A target that reserves more than 6 MiB has been
 * changed, and a program that must fit in the compiler's own working set needs
 * a measurement rather than an assumption.
 *
 * The size is found by bisection so the program does not depend on how many
 * probes fit in the time budget: each probe frees what it claimed, and the
 * answer is the largest size that still succeeds.
 */

#include <stdio.h>
#include <stdlib.h>

int cc64_write(int handle, const void *data, unsigned long size);

static unsigned long claim(unsigned long size) {
  void *block = malloc(size);
  if (block == 0) {
    return 0;
  }
  /* Touch both ends so the claim is a real reservation, not just bookkeeping. */
  ((volatile unsigned char *)block)[0] = 1;
  ((volatile unsigned char *)block)[size - 1] = 1;
  free(block);
  return size;
}

int main(void) {
  unsigned long low = 0;
  unsigned long high = 256u * 1024u * 1024u; /* 256 MiB: far past any target */
  unsigned long probe;

  if (claim(high) != 0) {
    cc64_write(1, "heap: implausibly large\n", 23);
    return 2;
  }
  /* Invariant: 0 always fits, high never does. */
  while (high - low > 1024u * 1024u) {
    probe = low + (high - low) / 2;
    probe &= ~1023ul;
    if (claim(probe) != 0) {
      low = probe;
    } else {
      high = probe;
    }
  }
  /* The allocator's own bookkeeping and the process block sit in the same
   * chain, so the measured figure is below the nominal heap size. Rounding
   * down to a whole MiB keeps the number stable across runs. */
  low = (low / (1024u * 1024u)) * 1024u * 1024u;
  printf("heap: %lu MiB usable\n", low / (1024u * 1024u));
  return 0;
}
