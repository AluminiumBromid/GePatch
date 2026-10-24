#ifndef GE_LIST_H
#define GE_LIST_H

#include <psptypes.h>
#include <pspge.h>

#include "ge_constants.h"

#define DRAW_NATIVE 0xABCDEF00
// Number of the last native frame in DISPLAY_BUFFER, for Adrenaline's display loop (redraws only
// new frames). Written by the GE after the copy, so it changes only once the picture is complete.
// Both ends of that transfer lie where the GE already works every frame: right after the picture,
// and in the copy list's area (Sony's emulator stops with C1-2858-3 on a transfer from GePatch's
// kernel memory to 0x0BCDEF10).
#define NATIVE_FRAME_COUNTER (DISPLAY_BUFFER + PITCH * HEIGHT * 2)
#define NATIVE_FRAME_SOURCE (RENDER_LIST + 0x1000)

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

// Totals of all walks: commands walked and through-mode vertices patched (tests/list_bench).
extern u32 ge_list_commands, ge_list_vertices;

// XMB only (set at start-up): GePatch renders 16-bit 565 colour, which turns the XMB's gradients
// (normally 32-bit) into visible bands. Dithering breaks the bands up; the walker keeps it on and
// keeps GePatch's dither pattern (the copy list sets both once per frame).
extern int ge_list_dither;
// XMB only (set at start-up): the game-boot video copies each frame (480x272, 32-bit) with a block
// transfer to the fixed VRAM address 0x04000000, i.e. into GePatch's render buffer. When the walker
// sees such a transfer it switches to pass-through: lists stay unpatched, so the XMB renders natively
// into VRAM where the video lands, and the frame hook shows the native picture. ge_list_video_seen is
// set for every video transfer (also while passing through); the frame hook ends the pass-through
// when none came for a while.
extern int ge_list_xmb;
extern int ge_list_passthrough, ge_list_video_seen;
// The XMB's GE state for the registers GePatch rewrites (patched = 0: the XMB's own values, for
// pass-through; 1: GePatch's, for returning to 2x) as a list ending in FINISH/END; returns its words.
u32 ge_list_xmb_state(u32 *out, int patched);
// Where that list is built (GePatch's copy-list area, read by the GE every frame)
#define XMB_STATE_LIST (RENDER_LIST + 0x2000)
// Standard 4x4 pattern (PSPSDK sceGuSetDither example: {-4,0,-3,1}, {2,-2,3,-1}, {-3,1,-4,0},
// {3,-1,2,-2}), one GE_CMD_DITH0..3 argument per row, 4-bit signed entries.
#define GE_DITHER_ROW0 0x1D0C
#define GE_DITHER_ROW1 0xF3E2
#define GE_DITHER_ROW2 0x0C1D
#define GE_DITHER_ROW3 0xE2F3

void resetGeState(void);
void patchGeList(u32 *list, u32 *stall);

// Streaming: while ge_list_progress is set, patchGeList calls it after ge_list_progress_every walked
// commands (the callback may change the interval for the next call) with a position in the main list before which everything the GE will execute is patched
// (only between top-level commands). Control flow the walk cannot follow exactly as the GE does
// (BJUMP, signals) clears it for the rest of the walk.
extern void (*ge_list_progress)(u32 *safe);
extern u32 ge_list_progress_every;
// After patchGeList: the list's END command if the walk ended at FINISH/END, else NULL.
extern u32 *ge_list_end;
// Where streaming stopped (the command), and the command type that stopped it; NULL/0 if it did not.
extern u32 *ge_list_stream_stop;
extern u32 ge_list_stream_stop_cmd;
extern u32 ge_list_stream_stop_walked;  // commands walked before the stop

// Writes the patched list back to memory for the GE (PSP: whole data cache; tests: nothing).
void ge_list_flush_cache(void);

#endif
