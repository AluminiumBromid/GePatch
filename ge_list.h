#ifndef GE_LIST_H
#define GE_LIST_H

#include <psptypes.h>
#include <pspge.h>

#include "ge_constants.h"

#define DRAW_NATIVE 0xABCDEF00

#define PITCH 960
#define WIDTH 960
#define HEIGHT 544
#define PIXELFORMAT GE_FORMAT_565

#define FAKE_VRAM 0x0A000000
#define DISPLAY_BUFFER 0x0A400000
#define VERTICES_BUFFER 0x0A600000
#define RENDER_LIST 0x0A800000

#define VRAM_DRAW_BUFFER_OFFSET 0x04000000
#define VRAM_DEPTH_BUFFER_OFFSET 0x04100000
#define VRAM_1KB 0x041ff000

typedef struct {
  u32 *list;
  u32 base;
  u32 offset;
} StackEntry;

typedef struct {
  u32 ge_cmd[0x100];

  u32 texbufptr[8];
  u32 texbufwidth[8];
  u32 framebufptr;
  u32 framebufwidth;
  u32 *framebufwidth_addr;
  u32 framebuf;

  u32 base;
  u32 offset;
  u32 address;

  u32 index_addr;
  u32 vertex_addr;
  u32 vertex_type;

  u32 ignore_framebuf;
  u32 ignore_texture;

  u32 has_draws;

  StackEntry stack[64];
  u32 curr_stack;

  u32 framebuf_addr[16];
  u32 framebuf_count;
} GeState;


extern GeState state;

void resetGeState(void);
void patchGeList(u32 *list, u32 *stall);

// Writes the patched list back to memory for the GE (PSP: whole data cache; tests: nothing).
void ge_list_flush_cache(void);

#endif
