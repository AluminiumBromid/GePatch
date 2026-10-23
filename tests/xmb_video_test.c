// Host test for the XMB boot-video pass-through in ge_list.c: a 32-bit block transfer into
// GePatch's render buffer switches the walker to pass-through (lists stay unpatched).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

#include "ge_list.h"

void ge_list_flush_cache(void) {}

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define W(cmd, data) (((u32)(cmd) << 24) | ((data) & 0xffffff))

static void *map_at(u32 addr, size_t size) {
  void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (p != (void *)(uintptr_t)addr) { fprintf(stderr, "cannot map %08x\n", addr); exit(1); }
  return p;
}

// Builds a list like the XMB's boot-video frames; returns the index of TRANSFERSTART.
static int build(u32 *l, int with_setup_word, int bits32) {
  int n = 0;
  l[n++] = W(GE_CMD_BASE, 0x080000);
  l[n++] = W(GE_CMD_VADDR, 0x200000);
  l[n++] = W(GE_CMD_VERTEXTYPE, 0x00011c);
  l[n++] = W(GE_CMD_TEXTUREMAPENABLE, 1);
  l[n++] = W(GE_CMD_ALPHABLENDENABLE, 1);
  l[n++] = W(GE_CMD_TEXADDR0, 0x123450);
  l[n++] = W(GE_CMD_TEXBUFWIDTH0, 0x090100);
  l[n++] = W(GE_CMD_TRANSFERSRC, 0xfd1a00);
  l[n++] = W(GE_CMD_TRANSFERSRC + 1, 0x0801e0);  // 0x08xxxxxx, width 480
  l[n++] = W(GE_CMD_TRANSFERSRC + 2, 0x000000);
  l[n++] = W(GE_CMD_TRANSFERSRC + 3, 0x040200);  // 0x04000000, width 512
  l[n++] = W(GE_CMD_TRANSFERSRCPOS, 0);
  l[n++] = W(GE_CMD_TRANSFERDSTPOS, 0);
  l[n++] = W(GE_CMD_TRANSFERSIZE, (271 << 10) | 479);
  if (!with_setup_word)
    l[n++] = 0;  // NOP between setup and start
  const int start = n;
  l[n++] = W(GE_CMD_TRANSFERSTART, bits32 ? 1 : 0);
  l[n++] = W(GE_CMD_VADDR, 0x210000);
  l[n++] = W(GE_CMD_PRIM, (3 << 16) | 3);  // overlay, transformed
  l[n++] = W(GE_CMD_FINISH, 0);
  l[n++] = W(GE_CMD_END, 0);
  return start;
}

int main(void) {
  map_at(0x08000000, 0x01000000);
  u32 *l = (u32 *)(uintptr_t)0x08100000;

  // 1. Outside the XMB nothing happens
  int t = build(l, 1, 1);
  resetGeState();
  patchGeList(l, NULL);
  CHECK(l[t] == W(GE_CMD_TRANSFERSTART, 1) && !ge_list_passthrough && !ge_list_video_seen);

  // 2. A 16-bit transfer in the XMB: no pass-through, the list is patched as usual
  ge_list_xmb = ge_list_dither = 1;
  t = build(l, 1, 0);
  resetGeState();
  patchGeList(l, NULL);
  CHECK(!ge_list_passthrough && !ge_list_video_seen);

  // 3. The boot video: pass-through from the transfer on; transfer and the rest of the list unchanged
  u32 copy[64];
  t = build(l, 1, 1);
  memcpy(copy, l, sizeof(copy));
  resetGeState();
  patchGeList(l, NULL);
  CHECK(ge_list_passthrough && ge_list_video_seen);
  CHECK(memcmp(l + t - 1, copy + t - 1, 6 * 4) == 0);  // setup word, start, VADDR, PRIM, FINISH, END

  // 4. While passing through: a new list stays completely unpatched; its transfer is seen
  ge_list_video_seen = 0;
  build(l, 1, 1);
  l[1] = W(GE_CMD_VIEWPORTXSCALE, 0x437000);  // 240.0 (native): GePatch would otherwise rewrite it to 480.0
  memcpy(copy, l, sizeof(copy));
  patchGeList(l, NULL);
  CHECK(memcmp(l, copy, sizeof(copy)) == 0 && ge_list_video_seen);

  // 4b. State lists: the XMB's own values for pass-through, GePatch's for returning to 2x
  {
    u32 *x = (u32 *)(uintptr_t)0x08200000;
    int k = 0;
    x[k++] = W(GE_CMD_FRAMEBUFPIXFORMAT, 3);        // 32-bit
    x[k++] = W(GE_CMD_VIEWPORTXSCALE, 0x437000);    // 240.0
    x[k++] = W(GE_CMD_OFFSETX, (2048 - 240) << 4);
    x[k++] = W(GE_CMD_FINISH, 0);
    x[k++] = W(GE_CMD_END, 0);
    patchGeList(x, NULL);  // still passing through: only recorded
    u32 st[32];
    u32 n = ge_list_xmb_state(st, 0);
    CHECK(n == 5 && st[0] == W(GE_CMD_FRAMEBUFPIXFORMAT, 3) && st[1] == W(GE_CMD_VIEWPORTXSCALE, 0x437000) &&
          st[2] == W(GE_CMD_OFFSETX, (2048 - 240) << 4) && st[3] == W(GE_CMD_FINISH, 0) && st[4] == W(GE_CMD_END, 0));
    n = ge_list_xmb_state(st, 1);
    CHECK(n == 5 && st[0] == W(GE_CMD_FRAMEBUFPIXFORMAT, PIXELFORMAT) && st[1] == W(GE_CMD_VIEWPORTXSCALE, 0x43f000) &&
          st[2] == W(GE_CMD_OFFSETX, (2048 - 480) << 4));
  }

  // 5. Pass-through ended by the frame hook: patching again
  ge_list_passthrough = 0;
  ge_list_xmb = ge_list_dither = 0;
  build(l, 1, 0);
  l[1] = W(GE_CMD_VIEWPORTXSCALE, 0x437000);
  resetGeState();
  patchGeList(l, NULL);
  CHECK(l[1] != W(GE_CMD_VIEWPORTXSCALE, 0x437000));

  printf("%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
