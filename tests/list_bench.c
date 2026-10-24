// Runs ge_list.c's patchGeList on a gecapture frame (vita-lab tools/vita ge-capture) on the PC.
//   list_bench <capture dir> [--iters N] [--out ram_after.bin]
// Maps the captured RAM at its PSP address, patches the lists in capture order like the EnQueue and
// UpdateStallAddr hooks do, and reports time per pass and the command mix. --out writes the RAM after
// one pass, to check that a faster walker patches exactly the same.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>

#include "ge_list.h"

void ge_list_flush_cache(void) {}

typedef struct {
  u32 order, qid, list, stall_enq, stall_final;
  int32_t cbid;
  u32 head, has_args, args[4];
} ListEntry;

static void *read_file(const char *dir, const char *name, size_t *size) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END);
  *size = ftell(f);
  fseek(f, 0, SEEK_SET);
  void *data = malloc(*size);
  if (fread(data, 1, *size, f) != *size) { perror(path); exit(1); }
  fclose(f);
  return data;
}

static void *map_at(u32 addr, size_t size) {
  void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (p != (void *)(uintptr_t)addr) { fprintf(stderr, "cannot map %08x\n", addr); exit(1); }
  return p;
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static ListEntry *lists;
static u32 num_lists;

// --stream: the safe points patchGeList reports, per list (checked: inside the list's walk, rising).
static int stream;
static u32 safe_count[16], safe_last[16], safe_backwards, cur_list;
static u32 *safe_first[16];
static void record_safe(u32 *safe) {
  const u32 a = (u32)(uintptr_t)safe;
  if (safe_count[cur_list] && a <= safe_last[cur_list]) safe_backwards++;
  if (!safe_count[cur_list]) safe_first[cur_list] = safe;
  safe_count[cur_list]++;
  safe_last[cur_list] = a;
  if (ge_list_progress_every < 4096) ge_list_progress_every *= 2;  // the hook's schedule
}

// One frame: every list as the hooks see it (both reset the state at enqueue).
static void patch_frame(void) {
  for (u32 i = 0; i < num_lists; i++) {
    const ListEntry *e = &lists[i];
    u32 *list = (u32 *)(uintptr_t)(e->list & 0x0fffffff);
    u32 *stall = (u32 *)(uintptr_t)(e->stall_enq & 0x0fffffff);
    resetGeState();
    cur_list = i < 16 ? i : 15;
    if (stream && !e->stall_enq) {
      ge_list_progress = record_safe;
      ge_list_progress_every = 128;
    }
    const u32 before = ge_list_commands;
    patchGeList(list, stall);
    if (stream && getenv("LIST_SIZES")) printf("list %u: %u commands walked, streaming %s at the end; stopped at %08x by %03x after %u commands\n", i, ge_list_commands - before, ge_list_progress ? "still on" : "off", (u32)(uintptr_t)ge_list_stream_stop, ge_list_stream_stop_cmd, ge_list_stream_stop_walked);
    ge_list_progress = NULL;
    if (e->stall_enq && e->stall_final != e->stall_enq)
      patchGeList(stall, (u32 *)(uintptr_t)(e->stall_final & 0x0fffffff));
  }
}

// Command mix of one pass: a read-only walk with patchGeList's control flow.
static u32 hist[256];
// Commands at an address the walk already passed this frame (sub-lists called more than once),
// by command; and how many CALLs go to a target that was called before.
static u32 repeat_hist[256], repeat_total, calls_total, calls_repeat;
static uint8_t *visited;  // one bit per word of RAM
#define VISIT_INDEX(p) (((u32)(uintptr_t)(p) - ram_base) >> 2)
static u32 ram_base, ram_words;
static u32 prims_through;
// Where the GE reads and writes: VRAM (0x04xxxxxx) or RAM. Unique addresses per kind.
#define ADDRS_MAX 512
typedef struct { const char *name; u32 n, vram_uses, ram_uses; u32 addr[ADDRS_MAX]; } AddrSet;
static AddrSet tex_set = { "texture" }, clut_set = { "clut" }, fb_set = { "framebuffer" }, xsrc_set = { "transfer src" }, xdst_set = { "transfer dst" };
static u32 xfer_bytes_to_vram, xfer_bytes_from_ram_to_vram;
static void addr_use(AddrSet *s, u32 a) {
  a &= 0x0fffffff;
  if ((a >> 24) == 0x04) s->vram_uses++; else s->ram_uses++;
  for (u32 i = 0; i < s->n; i++) if (s->addr[i] == a) return;
  if (s->n < ADDRS_MAX) s->addr[s->n++] = a;
}
static void addr_print(const AddrSet *s) {
  u32 v = 0;
  for (u32 i = 0; i < s->n; i++) v += (s->addr[i] >> 24) == 0x04;
  printf("  %-13s %4u uses (VRAM %u, RAM %u), %u addresses (VRAM %u):", s->name, s->vram_uses + s->ram_uses, s->vram_uses, s->ram_uses, s->n, v);
  for (u32 i = 0, k = 0; i < s->n && k < 8; i++) if ((s->addr[i] >> 24) == 0x04) { printf(" %08x", s->addr[i]); k++; }
  printf("\n");
}  // draws in through mode (the only ones patchGeList has to look into)
static void count_mix(void) {
  visited = calloc(ram_words / 8 + 1, 1);
  for (u32 i = 0; i < num_lists; i++) {
    u32 *list = (u32 *)(uintptr_t)(lists[i].list & 0x0fffffff);
    u32 *stall = lists[i].stall_final ? (u32 *)(uintptr_t)(lists[i].stall_final & 0x0fffffff) : NULL;
    u32 base = 0, offset = 0, sp = 0, vtype = 0;
    u32 tex0 = 0, texw0 = 0, clut = 0, clutu = 0, fbp = 0, xs = 0, xsw = 0, xd = 0, xdw = 0, xsize = 0, fmt_bpp = 2;
    struct { u32 *list, base, offset; } stack[64];
    for (; list && list != stall; list++) {
      const u32 op = *list, cmd = op >> 24, data = op & 0xffffff;
      hist[cmd]++;
      const u32 vi = VISIT_INDEX(list);
      if (vi < ram_words) {
        if (visited[vi >> 3] & (1 << (vi & 7))) { repeat_hist[cmd]++; repeat_total++; }
        visited[vi >> 3] |= 1 << (vi & 7);
      }
      switch (cmd) {
        case GE_CMD_TEXADDR0: tex0 = data; break;
        case GE_CMD_TEXBUFWIDTH0: texw0 = data; addr_use(&tex_set, ((texw0 & 0x0f0000) << 8) | (tex0 & 0xffffff)); break;
        case GE_CMD_CLUTADDR: clut = data; break;
        case GE_CMD_CLUTADDRUPPER: clutu = data; break;
        case GE_CMD_LOADCLUT: addr_use(&clut_set, ((clutu & 0x0f0000) << 8) | (clut & 0xffffff)); break;
        case GE_CMD_FRAMEBUFPTR: fbp = data; break;
        case GE_CMD_FRAMEBUFWIDTH: addr_use(&fb_set, 0x04000000 | (fbp & 0x1fffff)); break;
        case GE_CMD_TRANSFERSRC: xs = data; break;
        case GE_CMD_TRANSFERSRCW: xsw = data; break;
        case GE_CMD_TRANSFERDST: xd = data; break;
        case GE_CMD_TRANSFERDSTW: xdw = data; break;
        case GE_CMD_TRANSFERSIZE: xsize = data; break;
        case GE_CMD_TRANSFERSTART: {
          fmt_bpp = (data & 1) ? 4 : 2;
          const u32 src = ((xsw & 0x0f0000) << 8) | (xs & 0xffffff), dst = ((xdw & 0x0f0000) << 8) | (xd & 0xffffff);
          addr_use(&xsrc_set, src);
          addr_use(&xdst_set, dst);
          const u32 bytes = ((xsize & 0x3ff) + 1) * (((xsize >> 10) & 0x3ff) + 1) * fmt_bpp;
          if ((dst >> 24) == 0x04) { xfer_bytes_to_vram += bytes; if ((src >> 24) != 0x04) xfer_bytes_from_ram_to_vram += bytes; }
          break;
        }
      }
      if (cmd == GE_CMD_VERTEXTYPE) vtype = data;
      else if ((cmd == GE_CMD_PRIM || cmd == GE_CMD_BEZIER || cmd == GE_CMD_SPLINE) && (vtype & GE_VTYPE_THROUGH_MASK) == GE_VTYPE_THROUGH) prims_through++;
      if (cmd == GE_CMD_BASE) base = (data << 8) & 0x0f000000;
      else if (cmd == GE_CMD_OFFSETADDR) offset = data << 8;
      else if (cmd == GE_CMD_ORIGIN) offset = (u32)(uintptr_t)list;
      else if (cmd == GE_CMD_CALL) {
        const u32 a = ((base | data) + offset) & 0x0ffffffc;
        const u32 *t = (u32 *)(uintptr_t)a;
        if (t[0] >> 24 == GE_CMD_BONEMATRIXDATA && t[11] >> 24 == GE_CMD_BONEMATRIXDATA && t[12] >> 24 == GE_CMD_RET)
          continue;
        calls_total++;
        const u32 ti = VISIT_INDEX(t);
        if (ti < ram_words && (visited[ti >> 3] & (1 << (ti & 7))))
          calls_repeat++;
        if (sp < 64) { stack[sp].list = list; stack[sp].offset = offset; sp++; list = (u32 *)(uintptr_t)(a - 4); }
      } else if (cmd == GE_CMD_JUMP || cmd == GE_CMD_BJUMP) {
        list = (u32 *)(uintptr_t)((((base | data) + offset) & 0x0ffffffc) - 4);
      } else if (cmd == GE_CMD_RET) {
        if (sp) { sp--; list = stack[sp].list; offset = stack[sp].offset; }
      } else if (cmd == GE_CMD_END) {
        const u32 prev = list[-1];
        if (prev >> 24 == GE_CMD_FINISH) break;
        if (prev >> 24 == GE_CMD_SIGNAL) { fprintf(stderr, "note: signal %02x not followed by the mix count\n", (prev >> 16) & 0xff); break; }
      }
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <capture dir> [--iters N] [--out file]\n", argv[0]); return 2; }
  const char *dir = argv[1], *out = NULL;
  int iters = 200;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "--stream")) stream = 1;
  }

  size_t cap_size, ram_size, vram_size;
  uint8_t *cap = read_file(dir, "capture.bin", &cap_size);
  u32 ram_start, ram_end;
  memcpy(&ram_start, cap + 0x18, 4);
  memcpy(&ram_end, cap + 0x1c, 4);
  memcpy(&num_lists, cap + 0x2c, 4);
  lists = (ListEntry *)(cap + 0x58);
  uint8_t *ram_img = read_file(dir, "ram.bin", &ram_size);
  uint8_t *vram_img = read_file(dir, "vram_start.bin", &vram_size);
  uint8_t *ram = map_at(ram_start, ram_size);
  uint8_t *vram = map_at(0x04000000, vram_size);
  printf("%u lists, RAM %08x-%08x\n", num_lists, ram_start, ram_end);
  ram_base = ram_start;
  ram_words = ram_size / 4;

  memcpy(ram, ram_img, ram_size);
  memcpy(vram, vram_img, vram_size);
  count_mix();

  // The game builds new lists every frame: each pass starts from the captured (unpatched) RAM.
  double total = 0, best = 1e9;
  u32 commands = 0, vertices = 0;
  for (int it = 0; it < iters; it++) {
    memcpy(ram, ram_img, ram_size);
    memcpy(vram, vram_img, vram_size);
    ge_list_commands = ge_list_vertices = 0;
    const double t0 = now_ms();
    patch_frame();
    const double t = now_ms() - t0;
    total += t;
    if (t < best) best = t;
    commands = ge_list_commands;
    vertices = ge_list_vertices;
    if (it == 0 && out) {
      FILE *f = fopen(out, "wb");
      fwrite(ram, 1, ram_size, f);
      fwrite(vram, 1, vram_size, f);
      fclose(f);
    }
  }
  if (stream) {
    for (u32 i = 0; i < num_lists && i < 16; i++)
      printf("list %u (%08x, stall %08x): %u safe points per pass, first %08x, last %08x, end %s\n", i, lists[i].list,
             lists[i].stall_enq, safe_count[i] / iters, (u32)(uintptr_t)safe_first[i], safe_last[i], "-");
    printf("safe points going backwards: %u\n", safe_backwards);
  }
  printf("patch: %.3f ms/frame (best %.3f, %d passes), %u cmds, %u verts\n", total / iters, best, iters, commands, vertices);

  u32 order[256], sum = 0;
  for (int c = 0; c < 256; c++) { order[c] = c; sum += hist[c]; }
  for (int a = 0; a < 256; a++)
    for (int b = a + 1; b < 256; b++)
      if (hist[order[b]] > hist[order[a]]) { u32 t = order[a]; order[a] = order[b]; order[b] = t; }
  printf("command mix (%u walked):", sum);
  for (int k = 0; k < 16 && hist[order[k]]; k++)
    printf(" %02x:%u(%u%%)", order[k], hist[order[k]], hist[order[k]] * 100 / sum);
  printf("\n");
  printf("repeat walks: %u of %u commands (%u%%) at an address already walked this frame; %u of %u calls to a target called before\n",
         repeat_total, sum, repeat_total * 100 / sum, calls_repeat, calls_total);
  for (int a = 0; a < 256; a++) order[a] = a;
  for (int a = 0; a < 256; a++)
    for (int b = a + 1; b < 256; b++)
      if (repeat_hist[order[b]] > repeat_hist[order[a]]) { u32 t = order[a]; order[a] = order[b]; order[b] = t; }
  printf("hist:");
  for (int c = 0; c < 256; c++)
    if (hist[c]) printf(" %02x:%u(", c, hist[c]);
  printf("\nthrough-mode draws: %u\n", prims_through);
  printf("GE memory use this frame:\n");
  addr_print(&tex_set); addr_print(&clut_set); addr_print(&fb_set); addr_print(&xsrc_set); addr_print(&xdst_set);
  printf("  block transfers into VRAM: %u bytes (%u from RAM)\n", xfer_bytes_to_vram, xfer_bytes_from_ram_to_vram);
  printf("repeated by command:");
  for (int k = 0; k < 10 && repeat_hist[order[k]]; k++)
    printf(" %02x:%u", order[k], repeat_hist[order[k]]);
  printf("\n");
  return 0;
}
