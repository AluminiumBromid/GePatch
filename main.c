#include <pspsdk.h>
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspge.h>
#include <pspgu.h>

#include <stdio.h>
#include <string.h>

#include <systemctrl.h>

#include "ge_constants.h"

PSP_MODULE_INFO("GePatch", 0x1007, 1, 0);

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

#define log(...) \
{ \
  char msg[256]; \
  sprintf(msg,__VA_ARGS__); \
  logmsg(msg); \
}

#ifndef GEPATCH_TRACE
#define GEPATCH_TRACE 0
#endif

#if GEPATCH_TRACE
#define TRACE_LOG(...) log(__VA_ARGS__)
#else
#define TRACE_LOG(...) do {} while (0)
#endif

#ifndef GEPATCH_TRACE_EXPORT
#define GEPATCH_TRACE_EXPORT GEPATCH_TRACE
#endif

#if GEPATCH_TRACE_EXPORT
#define TRACE_EXPORT(...) traceRecord(__VA_ARGS__)
#else
#define TRACE_EXPORT(...) do {} while (0)
#endif

void logmsg(char *msg) {
  int k1 = pspSdkSetK1(0);

  SceUID fd = sceIoOpen("ms0:/ge_patch.txt", PSP_O_WRONLY | PSP_O_CREAT, 0777);
  if (fd >= 0) {
    sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoWrite(fd, msg, strlen(msg));
    sceIoClose(fd);
  }

  pspSdkSetK1(k1);
}

void traceRecord(const char *event, u32 list_id, u32 pc, u32 cmd, u32 data, u32 arg0, u32 arg1, u32 arg2) {
  int k1 = pspSdkSetK1(0);
  char line[192];

  sprintf(line, "%s,%lu,%08lx,%02lx,%06lx,%08lx,%08lx,%08lx\n",
          event,
          (unsigned long)list_id,
          (unsigned long)pc,
          (unsigned long)cmd,
          (unsigned long)data,
          (unsigned long)arg0,
          (unsigned long)arg1,
          (unsigned long)arg2);

  SceUID fd = sceIoOpen("ms0:/ge_patch_trace.csv", PSP_O_WRONLY | PSP_O_CREAT, 0777);
  if (fd >= 0) {
    sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoWrite(fd, line, strlen(line));
    sceIoClose(fd);
  }

  pspSdkSetK1(k1);
}

void traceInitFile() {
  int k1 = pspSdkSetK1(0);
  const char *header = "event,list_id,pc,cmd,data,arg0,arg1,arg2\n";
  SceUID fd = sceIoOpen("ms0:/ge_patch_trace.csv", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, header, strlen(header));
    sceIoClose(fd);
  }
  pspSdkSetK1(k1);
}

int checkAddress(u32 addr) {
  addr &= 0x0fffffff;
  if (addr >= 0x08400000 && addr < 0x0A400000)
    return 0;
  if (addr >= 0x04000000 && addr < 0x04200000) {
    // log("vram address: %08x\n", addr);
    return -1;
  }
  log("invalid address: %08lx\n", (unsigned long)addr);
  return -1;
}

static const u8 tcsize[4] = { 0, 2, 4, 8 }, tcalign[4] = { 0, 1, 2, 4 };
static const u8 colsize[8] = { 0, 0, 0, 0, 2, 2, 2, 4 }, colalign[8] = { 0, 0, 0, 0, 2, 2, 2, 4 };
static const u8 nrmsize[4] = { 0, 3, 6, 12 }, nrmalign[4] = { 0, 1, 2, 4 };
static const u8 possize[4] = { 3, 3, 6, 12 }, posalign[4] = { 1, 1, 2, 4 };
static const u8 wtsize[4] = { 0, 1, 2, 4 }, wtalign[4] = { 0, 1, 2, 4 };

#define ALIGN(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

void getVertexInfo(u32 op, u8 *vertex_size, u8 *pos_off, u8 *visit_off) {
  int tc = (op & GE_VTYPE_TC_MASK) >> GE_VTYPE_TC_SHIFT;
  int col = (op & GE_VTYPE_COL_MASK) >> GE_VTYPE_COL_SHIFT;
  int nrm = (op & GE_VTYPE_NRM_MASK) >> GE_VTYPE_NRM_SHIFT;
  int pos = (op & GE_VTYPE_POS_MASK) >> GE_VTYPE_POS_SHIFT;
  int weight = (op & GE_VTYPE_WEIGHT_MASK) >> GE_VTYPE_WEIGHT_SHIFT;
  int weightCount = ((op & GE_VTYPE_WEIGHTCOUNT_MASK) >> GE_VTYPE_WEIGHTCOUNT_SHIFT) + 1;
  int morphCount = ((op & GE_VTYPE_MORPHCOUNT_MASK) >> GE_VTYPE_MORPHCOUNT_SHIFT) + 1;

  u8 biggest = 0;
  u8 size = 0;
  u8 aligned_size = 0;
  // u8 weightoff = 0, tcoff = 0, coloff = 0, nrmoff = 0;
  u8 posoff = 0;
  u8 visitoff = 0;

  if (weight) {
    // size = ALIGN(size, wtalign[weight]);
    // weightoff = size;
    size += wtsize[weight] * weightCount;
    if (wtalign[weight] > biggest)
      biggest = wtalign[weight];
  }

  if (tc) {
    aligned_size = ALIGN(size, tcalign[tc]);
    if (!visitoff && aligned_size != size)
      visitoff = size;
    size = aligned_size;
    // tcoff = size;
    size += tcsize[tc];
    if (tcalign[tc] > biggest)
      biggest = tcalign[tc];
  }

  if (col) {
    aligned_size = ALIGN(size, colalign[col]);
    if (!visitoff && aligned_size != size)
      visitoff = size;
    size = aligned_size;
    // coloff = size;
    size += colsize[col];
    if (colalign[col] > biggest)
      biggest = colalign[col];
  }

  if (nrm) {
    aligned_size = ALIGN(size, nrmalign[nrm]);
    if (!visitoff && aligned_size != size)
      visitoff = size;
    size = aligned_size;
    // nrmoff = size;
    size += nrmsize[nrm];
    if (nrmalign[nrm] > biggest)
      biggest = nrmalign[nrm];
  }

  if (pos) {
    aligned_size = ALIGN(size, posalign[pos]);
    if (!visitoff && aligned_size != size)
      visitoff = size;
    size = aligned_size;
    posoff = size;
    size += possize[pos];
    if (posalign[pos] > biggest)
      biggest = posalign[pos];
  }

  aligned_size = ALIGN(size, biggest);
  if (!visitoff && aligned_size != size)
    visitoff = size;
  size = aligned_size;
  size *= morphCount;

  *vertex_size = size;
  *pos_off = posoff;
  *visit_off = visitoff;
}

void GetIndexBounds(const void *inds, int count, u32 vertType, u16 *indexLowerBound, u16 *indexUpperBound) {
  int i;
  int lowerBound = 0x7FFFFFFF;
  int upperBound = 0;
  u32 idx = vertType & GE_VTYPE_IDX_MASK;
  if (idx == GE_VTYPE_IDX_8BIT) {
    const u8 *ind8 = (const u8 *)inds;
    for (i = 0; i < count; i++) {
      u8 value = ind8[i];
      if (value > upperBound)
        upperBound = value;
      if (value < lowerBound)
        lowerBound = value;
    }
  } else if (idx == GE_VTYPE_IDX_16BIT) {
    const u16 *ind16 = (const u16 *)inds;
    for (i = 0; i < count; i++) {
      u16 value = ind16[i];
      if (value > upperBound)
        upperBound = value;
      if (value < lowerBound)
        lowerBound = value;
    }
  } else if (idx == GE_VTYPE_IDX_32BIT) {
    const u32 *ind32 = (const u32 *)inds;
    for (i = 0; i < count; i++) {
      u16 value = (u16)ind32[i];
      if (value > upperBound)
        upperBound = value;
      if (value < lowerBound)
        lowerBound = value;
    }
  } else {
    lowerBound = 0;
    upperBound = count - 1;
  }
  *indexLowerBound = (u16)lowerBound;
  *indexUpperBound = (u16)upperBound;
}

typedef struct {
  u32 *list;
  u32 base;
  u32 offset;
} StackEntry;

typedef struct {
  u32 addr;
  u16 pitch;
  u16 height;
  u8 format;
} FramebufInfo;

typedef struct {
  u32 allow_large_fb_texture_offsets;
  u32 allow_format_mismatch;
  u32 disable_ignore_framebuf;
  u32 disable_ignore_texture;
} CompatProfile;

typedef struct {
  u32 vertex_addr;
  u32 index_addr;
  u32 vertex_type;
  u32 draw_data;
  u8 cmd;
} DrawPatchKey;

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

  FramebufInfo framebuf_info[16];
  u32 framebuf_count;
  DrawPatchKey draw_patch_keys[64];
  u32 draw_patch_key_count;

  u32 trace_list_id;
  u32 trace_cmd_count;
  u32 trace_draw_count;
  u32 trace_draw_nopped;
  u32 trace_draw_nopped_framebuf;
  u32 trace_draw_nopped_texfb;
  u32 trace_draw_patched;
  u32 trace_advance_verts;
  u32 trace_control_flow;
  u32 trace_signal_ops;
  u32 trace_fb_updates;
  u32 trace_fb_ignored;
  u32 trace_tex_checks;
  u32 trace_tex_alias;
} GeState;

static GeState state;
static u32 trace_list_seq = 0;
static CompatProfile profile;

// Addresses the game has passed to sceDisplaySetFrameBuf — these are the real
// display framebuffers. We must never ignore draws that target these, regardless
// of their pitch.
#define DISPLAY_FB_TRACK_MAX 8
static u32 display_fb_addrs[DISPLAY_FB_TRACK_MAX];
static u32 display_fb_addr_count = 0;

static void trackDisplayFramebuf(u32 addr) {
  addr &= 0x0fffffff;
  if (!addr) return;
  u32 i;
  for (i = 0; i < display_fb_addr_count; i++) {
    if (display_fb_addrs[i] == addr) return;
  }
  if (display_fb_addr_count < DISPLAY_FB_TRACK_MAX)
    display_fb_addrs[display_fb_addr_count++] = addr;
}

static int isDisplayFramebuf(u32 addr) {
  addr &= 0x0fffffff;
  u32 i;
  for (i = 0; i < display_fb_addr_count; i++) {
    if (display_fb_addrs[i] == addr) return 1;
  }
  return 0;
}

void resetGeState() {
  memset(&state, 0, sizeof(GeState));
}

int push(StackEntry *data) {
  if (state.curr_stack < (sizeof(state.stack) / sizeof(StackEntry))) {
    memcpy(&state.stack[state.curr_stack++], data, sizeof(StackEntry));
    return 0;
  }

  return -1;
}

StackEntry *pop() {
  if (state.curr_stack > 0)
    return &state.stack[--state.curr_stack];
  return NULL;
}

u32 geBufferFormatBpp(u32 fmt) {
  switch (fmt) {
    case GE_FORMAT_565:
    case GE_FORMAT_5551:
    case GE_FORMAT_4444:
    case GE_FORMAT_DEPTH16:
      return 2;
    case GE_FORMAT_8888:
      return 4;
    default:
      return 0;
  }
}

u32 geTextureFormatBpp(u32 fmt) {
  switch (fmt) {
    case GE_TFMT_5650:
    case GE_TFMT_5551:
    case GE_TFMT_4444:
      return 2;
    case GE_TFMT_8888:
      return 4;
    default:
      return 0;
  }
}

int isTextureBufferFormatCompatible(u32 texfmt, u32 fbfmt) {
  if (texfmt > GE_TFMT_8888)
    return 0;
  if (profile.allow_format_mismatch)
    return 1;
  return texfmt == fbfmt;
}

int findFramebufExact(u32 framebuf) {
  int i;
  for (i = 0; i < state.framebuf_count; i++) {
    if (state.framebuf_info[i].addr == framebuf) {
      return i;
    }
  }

  return -1;
}

int insertFramebuf(u32 framebuf, u16 pitch, u8 format, u16 height) {
  int res = findFramebufExact(framebuf);
  if (res >= 0)
    goto update;

  if (state.framebuf_count < (sizeof(state.framebuf_info) / sizeof(FramebufInfo))) {
    res = state.framebuf_count++;
  } else {
    return -1;
  }

update:
  state.framebuf_info[res].addr = framebuf;
  state.framebuf_info[res].pitch = pitch;
  state.framebuf_info[res].format = format;
  state.framebuf_info[res].height = height;
  return res;
}

int matchFramebufferTexture(u32 texaddr, u16 texpitch, u32 texfmt, u32 texsize, u32 *match_offset, u32 *match_kind) {
  u32 tex_bpp = geTextureFormatBpp(texfmt);
  if (!tex_bpp || !texpitch)
    return 0;

  u32 tex_height = 1 << ((texsize >> 8) & 0xf);
  u32 tex_stride_bytes = texpitch * tex_bpp;

  int i;
  for (i = 0; i < state.framebuf_count; i++) {
    FramebufInfo *fb = &state.framebuf_info[i];
    u32 fb_bpp = geBufferFormatBpp(fb->format);
    if (!fb_bpp || !fb->pitch || !fb->height)
      continue;

    u32 fb_stride_bytes = fb->pitch * fb_bpp;
    if (!fb_stride_bytes)
      continue;

    if (texaddr == fb->addr) {
      if (isTextureBufferFormatCompatible(texfmt, fb->format) && tex_stride_bytes == fb_stride_bytes) {
        *match_offset = 0;
        *match_kind = 2;
        return 1;
      }
      continue;
    }

    if (texaddr > fb->addr) {
      u32 byte_offset = texaddr - fb->addr;
      u32 y_offset = byte_offset / fb_stride_bytes;
      if (y_offset >= fb->height || (!profile.allow_large_fb_texture_offsets && y_offset > 32))
        continue;

      if (tex_stride_bytes != fb_stride_bytes && tex_height > 1)
        continue;

      if (!isTextureBufferFormatCompatible(texfmt, fb->format))
        continue;

      *match_offset = byte_offset;
      *match_kind = 1;
      return 1;
    }
  }

  return 0;
}

u32 parseProfileValue(const char *str) {
  u32 val = 0;
  while (*str == ' ' || *str == '\t')
    str++;
  while (*str >= '0' && *str <= '9') {
    val = val * 10 + (u32)(*str - '0');
    str++;
  }
  return val;
}

void loadCompatProfile() {

  int k1 = pspSdkSetK1(0);
  char buf[512];
  SceUID fd = sceIoOpen("ms0:/ge_patch_profile.cfg", PSP_O_RDONLY, 0);
  if (fd < 0) {
    pspSdkSetK1(k1);
    return;
  }

  int len = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  if (len <= 0) {
    pspSdkSetK1(k1);
    return;
  }
  buf[len] = 0;

  char *line = strtok(buf, "\n");
  while (line) {
    while (*line == ' ' || *line == '\t')
      line++;

    if (strncmp(line, "allow_large_fb_texture_offsets=", 31) == 0) {
      profile.allow_large_fb_texture_offsets = parseProfileValue(line + 31);
    } else if (strncmp(line, "allow_format_mismatch=", 22) == 0) {
      profile.allow_format_mismatch = parseProfileValue(line + 22);
    } else if (strncmp(line, "disable_ignore_framebuf=", 24) == 0) {
      profile.disable_ignore_framebuf = parseProfileValue(line + 24);
    } else if (strncmp(line, "disable_ignore_texture=", 23) == 0) {
      profile.disable_ignore_texture = parseProfileValue(line + 23);
    }

    line = strtok(NULL, "\n");
  }

  TRACE_LOG("gepatch profile: large_offset=%lu format_mismatch=%lu no_ignore_fb=%lu no_ignore_tex=%lu\n",
            (unsigned long)profile.allow_large_fb_texture_offsets,
            (unsigned long)profile.allow_format_mismatch,
            (unsigned long)profile.disable_ignore_framebuf,
            (unsigned long)profile.disable_ignore_texture);
  pspSdkSetK1(k1);
}

int hasDrawPatchKey(u32 vertex_addr, u32 index_addr, u32 vertex_type, u32 draw_data, u8 cmd) {
  int i;
  for (i = 0; i < state.draw_patch_key_count; i++) {
    DrawPatchKey *key = &state.draw_patch_keys[i];
    if (key->vertex_addr == vertex_addr &&
        key->index_addr == index_addr &&
        key->vertex_type == vertex_type &&
        key->draw_data == draw_data &&
        key->cmd == cmd) {
      return 1;
    }
  }
  return 0;
}

void recordDrawPatchKey(u32 vertex_addr, u32 index_addr, u32 vertex_type, u32 draw_data, u8 cmd) {
  if (state.draw_patch_key_count >= (sizeof(state.draw_patch_keys) / sizeof(DrawPatchKey))) {
    return;
  }

  DrawPatchKey *key = &state.draw_patch_keys[state.draw_patch_key_count++];
  key->vertex_addr = vertex_addr;
  key->index_addr = index_addr;
  key->vertex_type = vertex_type;
  key->draw_data = draw_data;
  key->cmd = cmd;
}

void AdvanceVerts(int count, int vertex_size) {
  state.trace_advance_verts++;
  if ((state.vertex_type & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
    int index_shift = ((state.vertex_type & GE_VTYPE_IDX_MASK) >> GE_VTYPE_IDX_SHIFT) - 1;
    state.index_addr += count << index_shift;
  } else {
    state.vertex_addr += count * vertex_size;
  }
}

void traceGeListSummary(u32 *list_start, u32 *stall) {
  TRACE_LOG(
      "gepatch[%lu] list=%08lx stall=%08lx cmds=%lu draws=%lu patched=%lu nopped=%lu (fb=%lu texfb=%lu) adv=%lu cf=%lu signal=%lu fbupd=%lu fbskip=%lu texchk=%lu texalias=%lu\n",
      (unsigned long)state.trace_list_id,
      (unsigned long)list_start,
      (unsigned long)stall,
      (unsigned long)state.trace_cmd_count,
      (unsigned long)state.trace_draw_count,
      (unsigned long)state.trace_draw_patched,
      (unsigned long)state.trace_draw_nopped,
      (unsigned long)state.trace_draw_nopped_framebuf,
      (unsigned long)state.trace_draw_nopped_texfb,
      (unsigned long)state.trace_advance_verts,
      (unsigned long)state.trace_control_flow,
      (unsigned long)state.trace_signal_ops,
      (unsigned long)state.trace_fb_updates,
      (unsigned long)state.trace_fb_ignored,
      (unsigned long)state.trace_tex_checks,
      (unsigned long)state.trace_tex_alias);
  TRACE_EXPORT("sum", state.trace_list_id, (u32)list_start, 0, 0,
               state.trace_cmd_count, state.trace_draw_count, state.trace_draw_nopped);
  TRACE_EXPORT("sum2", state.trace_list_id, (u32)stall, 0, 0,
               state.trace_draw_patched, state.trace_control_flow, state.trace_signal_ops);
  TRACE_EXPORT("sum3", state.trace_list_id, 0, 0, 0,
               state.trace_fb_updates, state.trace_tex_checks, state.trace_tex_alias);
}

// TODO: when ignore_framebuf=1 or ignore_texture=1, dummy all non-control-flow instructions
void patchGeList(u32 *list, u32 *stall) {
  union {
    float f;
    unsigned int i;
  } t;

  StackEntry *stack_entry;
  StackEntry stack_entry_buf;
  u32 *list_start = list;

  state.trace_list_id = ++trace_list_seq;
  TRACE_LOG("gepatch[%lu] begin list=%08lx stall=%08lx\n",
            (unsigned long)state.trace_list_id, (unsigned long)list, (unsigned long)stall);
  TRACE_EXPORT("begin", state.trace_list_id, (u32)list, 0, 0, (u32)stall, 0, 0);

  for (; list && (!stall || (stall && list != stall)); list++) {
    // if (checkAddress((u32)list) < 0)
      // goto finish;

    u32 op = *list;
    u32 cmd = op >> 24;
    u32 data = op & 0xffffff;

    state.trace_cmd_count++;
    state.ge_cmd[cmd] = data;
    TRACE_EXPORT("cmd", state.trace_list_id, (u32)list, cmd, data,
                 state.base, state.offset, state.vertex_type);

    switch (cmd) {
      // Skip matrix data

      // case GE_CMD_BONEMATRIXNUMBER:
        // if (*(list+12) >> 24 == GE_CMD_BONEMATRIXDATA)
          // list += 12;
        // break;

      // case GE_CMD_WORLDMATRIXNUMBER:
        // if (*(list+12) >> 24 == GE_CMD_WORLDMATRIXDATA)
          // list += 12;
        // break;

      // case GE_CMD_VIEWMATRIXNUMBER:
        // if (*(list+12) >> 24 == GE_CMD_VIEWMATRIXDATA)
          // list += 12;
        // break;

      // case GE_CMD_TGENMATRIXNUMBER:
        // if (*(list+12) >> 24 == GE_CMD_TGENMATRIXDATA)
          // list += 12;
        // break;

      // case GE_CMD_PROJMATRIXNUMBER:
        // if (*(list+16) >> 24 == GE_CMD_PROJMATRIXDATA)
          // list += 16;
        // break;

      // Handle control flow commands

      case GE_CMD_BASE:
        state.base = (data << 8) & 0x0f000000;
        TRACE_EXPORT("state", state.trace_list_id, (u32)list, cmd, data,
                     state.base, state.offset, 0);
        break;

      case GE_CMD_OFFSETADDR:
        state.offset = data << 8;
        TRACE_EXPORT("state", state.trace_list_id, (u32)list, cmd, data,
                     state.base, state.offset, 0);
        break;

      case GE_CMD_ORIGIN:
        state.offset = (u32)list;
        TRACE_EXPORT("state", state.trace_list_id, (u32)list, cmd, data,
                     state.base, state.offset, 0);
        break;

      // TODO: need to save other states, too?
      case GE_CMD_CALL:
        state.trace_control_flow++;
        state.address = ((state.base | data) + state.offset) & 0x0ffffffc;
        TRACE_EXPORT("cf", state.trace_list_id, (u32)list, cmd, data,
                     state.address, state.curr_stack, 0);
        if (checkAddress(state.address) < 0 || checkAddress(state.address + 12 * 4) < 0) {
          TRACE_EXPORT("cf_invalid", state.trace_list_id, (u32)list, cmd, data,
                       state.address, state.base, state.offset);
          break;
        }
        if (*(u32 *)(state.address) >> 24 == GE_CMD_BONEMATRIXDATA &&
            *(u32 *)(state.address+11*4) >> 24 == GE_CMD_BONEMATRIXDATA &&
            *(u32 *)(state.address+12*4) >> 24 == GE_CMD_RET) {
          break;
        }
        stack_entry = &stack_entry_buf;
        stack_entry->list = list;
        stack_entry->offset = state.offset;
        if (push(stack_entry) == 0) {
          list = (u32 *)(state.address - 4);
        }
        break;

      // TODO: is it okay to always take the branch?
      case GE_CMD_BJUMP:
        state.trace_control_flow++;
        state.address = ((state.base | data) + state.offset) & 0x0ffffffc;
        TRACE_EXPORT("cf", state.trace_list_id, (u32)list, cmd, data,
                     state.address, state.curr_stack, 0);
        if (checkAddress(state.address) < 0) {
          TRACE_EXPORT("cf_invalid", state.trace_list_id, (u32)list, cmd, data,
                       state.address, state.base, state.offset);
          break;
        }
        list = (u32 *)(state.address - 4);
        break;

      case GE_CMD_JUMP:
        state.trace_control_flow++;
        state.address = ((state.base | data) + state.offset) & 0x0ffffffc;
        TRACE_EXPORT("cf", state.trace_list_id, (u32)list, cmd, data,
                     state.address, state.curr_stack, 0);
        if (checkAddress(state.address) < 0) {
          TRACE_EXPORT("cf_invalid", state.trace_list_id, (u32)list, cmd, data,
                       state.address, state.base, state.offset);
          break;
        }
        list = (u32 *)(state.address - 4);
        break;

      case GE_CMD_RET:
        state.trace_control_flow++;
        TRACE_EXPORT("cf", state.trace_list_id, (u32)list, cmd, data,
                     state.address, state.curr_stack, 0);
        // Ignore returns when the stack is empty
        stack_entry = pop();
        if (stack_entry) {
          list = stack_entry->list;
          state.offset = stack_entry->offset;
        }
        break;

      case GE_CMD_END:
      {
        u32 prev = *(list-1);
        switch (prev >> 24) {
          case GE_CMD_SIGNAL:
          {
            u8 behaviour = (prev >> 16) & 0xff;
            u16 signal = prev & 0xffff;
            u16 enddata = data & 0xffff;
            u32 target;

            switch (behaviour) {
              case PSP_GE_SIGNAL_SYNC:
                state.trace_signal_ops++;
                TRACE_EXPORT("signal", state.trace_list_id, (u32)list, prev >> 24, prev & 0xffffff,
                             behaviour, signal, enddata);
                list += 2;
                break;

              case PSP_GE_SIGNAL_JUMP:
                state.trace_signal_ops++;
                target = (((signal << 16) | enddata) & 0x0ffffffc);
                TRACE_EXPORT("signal", state.trace_list_id, (u32)list, prev >> 24, prev & 0xffffff,
                             behaviour, signal, target);
                if (checkAddress(target) < 0) {
                  TRACE_EXPORT("cf_invalid", state.trace_list_id, (u32)list, prev >> 24, prev & 0xffffff,
                               target, signal, enddata);
                  break;
                }
                list = (u32 *)(target - 4);
                break;

              case PSP_GE_SIGNAL_CALL:
                state.trace_signal_ops++;
                target = (((signal << 16) | enddata) & 0x0ffffffc);
                TRACE_EXPORT("signal", state.trace_list_id, (u32)list, prev >> 24, prev & 0xffffff,
                             behaviour, signal, target);
                if (checkAddress(target) < 0) {
                  TRACE_EXPORT("cf_invalid", state.trace_list_id, (u32)list, prev >> 24, prev & 0xffffff,
                               target, signal, enddata);
                  break;
                }
                stack_entry = &stack_entry_buf;
                stack_entry->list = list;
                stack_entry->base = state.base;
                stack_entry->offset = state.offset;
                if (push(stack_entry) == 0)
                  list = (u32 *)(target - 4);
                break;

              case PSP_GE_SIGNAL_RET:
                state.trace_signal_ops++;
                TRACE_EXPORT("signal", state.trace_list_id, (u32)list, prev >> 24, prev & 0xffffff,
                             behaviour, signal, enddata);
                // Ignore returns when the stack is empty
                stack_entry = pop();
                if (stack_entry) {
                  list = stack_entry->list;
                  state.base = stack_entry->base;
                  state.offset = stack_entry->offset;
                }
                break;

              default:
                break;
            }
            break;
          }

          case GE_CMD_FINISH:
            goto finish;

          default:
            break;
        }
        break;
      }

      // Patch vertices

      case GE_CMD_IADDR:
        state.index_addr = ((state.base | data) + state.offset) & 0x0fffffff;
        break;

      case GE_CMD_VADDR:
        state.vertex_addr = ((state.base | data) + state.offset) & 0x0fffffff;
        break;

      case GE_CMD_VERTEXTYPE:
        state.vertex_type = data;
        TRACE_EXPORT("vtype", state.trace_list_id, (u32)list, cmd, data,
                     state.vertex_addr, state.index_addr, 0);
        break;

      case GE_CMD_BEZIER:
      case GE_CMD_SPLINE:
      {
        state.has_draws = 1;
        state.trace_draw_count++;

        u32 skip_framebuf = state.ignore_framebuf && !profile.disable_ignore_framebuf;
        u32 skip_texture = state.ignore_texture && !profile.disable_ignore_texture;
        if (skip_framebuf || (skip_texture && state.ge_cmd[GE_CMD_TEXTUREMAPENABLE])) {
          state.trace_draw_nopped++;
          if (skip_framebuf) {
            state.trace_draw_nopped_framebuf++;
          } else {
            state.trace_draw_nopped_texfb++;
          }
          *list = 0;
          TRACE_EXPORT("draw_skip", state.trace_list_id, (u32)list, cmd, data,
                       state.ignore_framebuf, state.ignore_texture, state.ge_cmd[GE_CMD_TEXTUREMAPENABLE]);
          break;
        }

        if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) == GE_VTYPE_THROUGH) {
          state.trace_draw_patched++;
          u8 num_points_u = data & 0xff;
          u8 num_points_v = (data >> 8) & 0xff;

          u32 count = num_points_u * num_points_v;
          u32 patch_vertex_addr = state.vertex_addr;
          u32 patch_index_addr = state.index_addr;

          u8 vertex_size = 0, pos_off = 0, visit_off = 0;
          getVertexInfo(state.vertex_type, &vertex_size, &pos_off, &visit_off);

          if (hasDrawPatchKey(patch_vertex_addr, patch_index_addr, state.vertex_type, data, cmd)) {
            AdvanceVerts(count, vertex_size);
            TRACE_EXPORT("draw_reuse", state.trace_list_id, (u32)list, cmd, data,
                         count, patch_vertex_addr, patch_index_addr);
            break;
          }

          recordDrawPatchKey(patch_vertex_addr, patch_index_addr, state.vertex_type, data, cmd);
          AdvanceVerts(count, vertex_size);
          TRACE_EXPORT("draw_patch", state.trace_list_id, (u32)list, cmd, data,
                       count, patch_vertex_addr, patch_index_addr);
        }

        break;
      }

      case GE_CMD_BOUNDINGBOX:
      {
        u32 skip_framebuf = state.ignore_framebuf && !profile.disable_ignore_framebuf;
        u32 skip_texture = state.ignore_texture && !profile.disable_ignore_texture;
        if (skip_framebuf || (skip_texture && state.ge_cmd[GE_CMD_TEXTUREMAPENABLE]))
          break;

        if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) == GE_VTYPE_THROUGH) {
          u32 count = data;

          u8 vertex_size = 0, pos_off = 0, visit_off = 0;
          getVertexInfo(state.vertex_type, &vertex_size, &pos_off, &visit_off);

          AdvanceVerts(count, vertex_size);
        }

        break;
      }

      case GE_CMD_PRIM:
      {
        state.has_draws = 1;
        state.trace_draw_count++;

        // Dragon Ball Z Tenkaichi Tag Team uses the same GE list again,
        // therefore NOPing it makes character invisible.
        u32 skip_framebuf = state.ignore_framebuf && !profile.disable_ignore_framebuf;
        u32 skip_texture = state.ignore_texture && !profile.disable_ignore_texture;
        if (skip_framebuf || (skip_texture && state.ge_cmd[GE_CMD_TEXTUREMAPENABLE])) {
          state.trace_draw_nopped++;
          if (skip_framebuf) {
            state.trace_draw_nopped_framebuf++;
          } else {
            state.trace_draw_nopped_texfb++;
          }
          *list = 0;
          TRACE_EXPORT("draw_skip", state.trace_list_id, (u32)list, cmd, data,
                       state.ignore_framebuf, state.ignore_texture, state.ge_cmd[GE_CMD_TEXTUREMAPENABLE]);
          break;
        }

        if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) == GE_VTYPE_THROUGH) {
          state.trace_draw_patched++;
          u16 count = data & 0xffff;
          u32 patch_vertex_addr = state.vertex_addr;
          u32 patch_index_addr = state.index_addr;

          u8 vertex_size = 0, pos_off = 0, visit_off = 0;
          getVertexInfo(state.vertex_type, &vertex_size, &pos_off, &visit_off);

          if (hasDrawPatchKey(patch_vertex_addr, patch_index_addr, state.vertex_type, data, cmd)) {
            AdvanceVerts(count, vertex_size);
            TRACE_EXPORT("draw_reuse", state.trace_list_id, (u32)list, cmd, data,
                         count, patch_vertex_addr, patch_index_addr);
            break;
          }

          u16 lower = 0;
          u16 upper = count;
          if ((state.vertex_type & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
            GetIndexBounds((void *)state.index_addr, count, state.vertex_type, &lower, &upper);
            upper += 1;
          }

          int pos = (state.vertex_type & GE_VTYPE_POS_MASK) >> GE_VTYPE_POS_SHIFT;
          int pos_size = possize[pos] / 3;
          if (pos_size != 2 && pos_size != 4) {
            recordDrawPatchKey(patch_vertex_addr, patch_index_addr, state.vertex_type, data, cmd);
            AdvanceVerts(count, vertex_size);
            TRACE_EXPORT("draw_patch", state.trace_list_id, (u32)list, cmd, data,
                         count, patch_vertex_addr, patch_index_addr);
            break;
          }

          // TODO: we may patch the same vertex again and again...
          u8 decoded = 0, encoded = 0;
          u32 vertex_addr = state.vertex_addr + lower * vertex_size;
          int i;
          for (i = lower; i < upper; i++, vertex_addr += vertex_size) {
            int j;
            for (j = 0; j < 2; j++) {
              u32 addr = vertex_addr + pos_off + j * pos_size;
              switch (pos_size) {
                case 2:
                {
                  short val = *(short *)addr;
                  if (val != 0) {
                    // Decode and check if we already doubled at least one of the vertices
                    // If that's the case, let's assume all other vertices have been doubled, too
                    if (!decoded && visit_off && upper - i >= 2) {
                      if (*(u8 *)(vertex_addr + visit_off + 0 * vertex_size) == ((val >> 0) & 0xff) &&
                          *(u8 *)(vertex_addr + visit_off + 1 * vertex_size) == ((val >> 8) & 0xff)) {
                        goto exit_loop;
                      }
                      decoded = 1;
                    }

                    if (val == 480 || val == 960)
                      *(short *)addr = 960;
                    else if (val == 272 || val == 544)
                      *(short *)addr = 544;
                    else if (val > -2048 && val < 2048)
                      *(short *)addr *= 2;

                    // Encode that we already doubled one of the vertices
                    if (!encoded && visit_off && upper - i >= 2) {
                      val = *(short *)addr;
                      *(u8 *)(vertex_addr + visit_off + 0 * vertex_size) = (val >> 0) & 0xff;
                      *(u8 *)(vertex_addr + visit_off + 1 * vertex_size) = (val >> 8) & 0xff;
                      encoded = 1;
                    }
                  }
                  break;
                }

                case 4:
                {
                  t.i = *(u32 *)addr;
                  if (t.f != 0) {
                    // Decode and check if we already doubled at least one of the vertices
                    // If that's the case, let's assume all other vertices have been doubled, too
                    if (!decoded && visit_off && upper - i >= 4) {
                      if (*(u8 *)(vertex_addr + visit_off + 0 * vertex_size) == ((t.i >> 0) & 0xff) &&
                          *(u8 *)(vertex_addr + visit_off + 1 * vertex_size) == ((t.i >> 8) & 0xff) &&
                          *(u8 *)(vertex_addr + visit_off + 2 * vertex_size) == ((t.i >> 16) & 0xff) &&
                          *(u8 *)(vertex_addr + visit_off + 3 * vertex_size) == ((t.i >> 24) & 0xff)) {
                        goto exit_loop;
                      }
                      decoded = 1;
                    }

                    if (t.f == 480 || t.f == 960) {
                      t.f = 960;
                      *(u32 *)addr = t.i;
                    } else if (t.f == 272 || t.f == 544) {
                      t.f = 544;
                      *(u32 *)addr = t.i;
                    } else if (t.f > -2048 && t.f < 2048) {
                      t.f *= 2;
                      *(u32 *)addr = t.i;
                    }

                    // Encode that we already doubled one of the vertices
                    if (!encoded && visit_off && upper - i >= 4) {
                      *(u8 *)(vertex_addr + visit_off + 0 * vertex_size) = (t.i >> 0) & 0xff;
                      *(u8 *)(vertex_addr + visit_off + 1 * vertex_size) = (t.i >> 8) & 0xff;
                      *(u8 *)(vertex_addr + visit_off + 2 * vertex_size) = (t.i >> 16) & 0xff;
                      *(u8 *)(vertex_addr + visit_off + 3 * vertex_size) = (t.i >> 24) & 0xff;
                      encoded = 1;
                    }
                  }
                  break;
                }
              }
            }
          }

exit_loop:
          recordDrawPatchKey(patch_vertex_addr, patch_index_addr, state.vertex_type, data, cmd);
          AdvanceVerts(count, vertex_size);
          TRACE_EXPORT("draw_patch", state.trace_list_id, (u32)list, cmd, data,
                       count, patch_vertex_addr, patch_index_addr);
        }

        break;
      }

      // Patch GE commands

      // case GE_CMD_DITHERENABLE:
        // *list = (cmd << 24) | 1;
        // break;

      case GE_CMD_FRAMEBUFPIXFORMAT:
        *list = (cmd << 24) | PIXELFORMAT;
        break;

      case GE_CMD_FRAMEBUFPTR:
      case GE_CMD_FRAMEBUFWIDTH:
      {
        if (cmd == GE_CMD_FRAMEBUFPTR) {
          *list = (cmd << 24) | (VRAM_DRAW_BUFFER_OFFSET & 0xffffff);
          state.framebufptr = op;
        } else {
          u16 pitch = (op & 0xffff) != 512 ? (op & 0xffff) : 960;
          *list = (cmd << 24) | ((VRAM_DRAW_BUFFER_OFFSET >> 24) << 16) | pitch;
          state.framebufwidth = op;
          state.framebufwidth_addr = list;
        }

        if (state.framebufptr && state.framebufwidth) {
          state.trace_fb_updates++;
          state.framebuf = FAKE_VRAM | (state.framebufptr & 0xffffff);

          // Reconstruct full game-side framebuf address to check if it is a
          // display framebuffer. High byte comes from bits 16-23 of FRAMEBUFWIDTH.
          u32 game_fb_highbyte = (state.framebufwidth >> 16) & 0x0f;
          u32 game_fb_addr = (game_fb_highbyte << 24) | (state.framebufptr & 0xffffff);

          // This allows more games to work, but causes weird triangles in Sonic.
          u32 pitch = state.framebufwidth & 0xffff;
          // Never ignore a framebuffer that the game has actually displayed — it
          // is a real display surface regardless of its pitch value.
          if (pitch == 512 || pitch == 960 || isDisplayFramebuf(game_fb_addr)) {
            state.ignore_framebuf = 0;
          } else {
            state.trace_fb_ignored++;
            *state.framebufwidth_addr = (GE_CMD_FRAMEBUFWIDTH << 24) | ((VRAM_1KB >> 24) << 16) | 0;
            state.ignore_framebuf = 1;
          }

          TRACE_LOG("gepatch[%lu] fb=%08lx pitch=%lu ignore_fb=%lu\n",
                    (unsigned long)state.trace_list_id,
                    (unsigned long)state.framebuf,
                    (unsigned long)pitch,
                    (unsigned long)state.ignore_framebuf);
          TRACE_EXPORT("fb", state.trace_list_id, (u32)list, cmd, data,
                       state.framebuf, pitch, state.ignore_framebuf);

          insertFramebuf(state.framebuf, pitch, state.ge_cmd[GE_CMD_FRAMEBUFPIXFORMAT], HEIGHT);

          state.framebufptr = 0;
          state.framebufwidth = 0;
        }

        break;
      }

      case GE_CMD_ZBUFPTR:
        *list = (cmd << 24) | (VRAM_DEPTH_BUFFER_OFFSET & 0xffffff);
        break;

      case GE_CMD_ZBUFWIDTH:
        *list = (cmd << 24) | ((VRAM_DEPTH_BUFFER_OFFSET >> 24) << 16) | PITCH;
        break;

      case GE_CMD_TEXADDR0:
      case GE_CMD_TEXADDR1:
      case GE_CMD_TEXADDR2:
      case GE_CMD_TEXADDR3:
      case GE_CMD_TEXADDR4:
      case GE_CMD_TEXADDR5:
      case GE_CMD_TEXADDR6:
      case GE_CMD_TEXADDR7:
      case GE_CMD_TEXBUFWIDTH0:
      case GE_CMD_TEXBUFWIDTH1:
      case GE_CMD_TEXBUFWIDTH2:
      case GE_CMD_TEXBUFWIDTH3:
      case GE_CMD_TEXBUFWIDTH4:
      case GE_CMD_TEXBUFWIDTH5:
      case GE_CMD_TEXBUFWIDTH6:
      case GE_CMD_TEXBUFWIDTH7:
      {
        int index;
        if (cmd >= GE_CMD_TEXADDR0 && cmd <= GE_CMD_TEXADDR7) {
          index = cmd - GE_CMD_TEXADDR0;
          state.texbufptr[index] = op;
        } else {
          index = cmd - GE_CMD_TEXBUFWIDTH0;
          state.texbufwidth[index] = op;
        }

        if (state.texbufptr[index] && state.texbufwidth[index]) {
          state.trace_tex_checks++;
          u32 texaddr = ((state.texbufwidth[index] & 0x0f0000) << 8) | (state.texbufptr[index] & 0xffffff);
          u16 texpitch = state.texbufwidth[index] & 0xffff;
          u32 texsize = state.ge_cmd[GE_CMD_TEXSIZE0 + index];
          u32 texfmt = state.ge_cmd[GE_CMD_TEXFORMAT] & 0x0f;
          u32 match_offset = 0;
          u32 match_kind = 0;
          if (texaddr != state.framebuf && matchFramebufferTexture(texaddr, texpitch, texfmt, texsize, &match_offset, &match_kind)) {
            state.ignore_texture = 1;
            state.trace_tex_alias++;
          } else {
            state.ignore_texture = 0;
          }

          TRACE_LOG("gepatch[%lu] tex[%lu]=%08lx ignore_tex=%lu\n",
                    (unsigned long)state.trace_list_id,
                    (unsigned long)index,
                    (unsigned long)texaddr,
                    (unsigned long)state.ignore_texture);
          TRACE_EXPORT("tex", state.trace_list_id, (u32)list, cmd, data,
                       texaddr, match_offset, match_kind);

          state.texbufptr[index] = 0;
          state.texbufwidth[index] = 0;
        }

        break;
      }

      case GE_CMD_VIEWPORTXSCALE:
        t.f = ((data << 8) >> 31) ? -(WIDTH / 2) : (WIDTH / 2);
        *list = (cmd << 24) | (t.i >> 8);
        break;

      case GE_CMD_VIEWPORTYSCALE:
        t.f = ((data << 8) >> 31) ? -(HEIGHT / 2) : (HEIGHT / 2);
        *list = (cmd << 24) | (t.i >> 8);
        break;

      case GE_CMD_VIEWPORTXCENTER:
        t.f = 2048;
        *list = (cmd << 24) | (t.i >> 8);
        break;

      case GE_CMD_VIEWPORTYCENTER:
        t.f = 2048;
        *list = (cmd << 24) | (t.i >> 8);
        break;

      case GE_CMD_OFFSETX:
        *list = (cmd << 24) | ((2048 - (WIDTH / 2)) << 4);
        break;

      case GE_CMD_OFFSETY:
        *list = (cmd << 24) | ((2048 - (HEIGHT / 2)) << 4);
        break;

      case GE_CMD_REGION2:
      case GE_CMD_SCISSOR2:
        *list = (cmd << 24) | ((HEIGHT - 1) << 10) | (WIDTH - 1);
        break;

      default:
        break;
    }
  }

finish:
  traceGeListSummary(list_start, stall);
  TRACE_EXPORT("end", state.trace_list_id, (u32)list_start, 0, 0, (u32)stall, 0, 0);
  sceKernelDcacheWritebackInvalidateAll();
}

void *(* _sceGeEdramGetAddr)(void);
unsigned int *(* _sceGeEdramGetSize)(void);
int (* _sceGeGetList)(int qid, void *list, int *flag);
int (* _sceGeListUpdateStallAddr)(int qid, void *stall);
int (* _sceGeListEnQueue)(const void *list, void *stall, int cbid, PspGeListArgs *arg);
int (* _sceGeListEnQueueHead)(const void *list, void *stall, int cbid, PspGeListArgs *arg);

int (* _sceDisplaySetFrameBuf)(void *topaddr, int bufferwidth, int pixelformat, int sync);

void *sceGeEdramGetAddrPatched(void) {
  return (void *)FAKE_VRAM;
}

unsigned int sceGeEdramGetSizePatched(void) {
  return 4 * 1024 * 1024;
}

void copyFrameBuffer() {
  *(u32 *)DRAW_NATIVE = 1;

  // memcpy((void *)DISPLAY_BUFFER, (void *)VRAM_DRAW_BUFFER_OFFSET, 960*544*2);
  sceGuStart(0, (void *)(RENDER_LIST | 0xA0000000));
  sceGuCopyImage(PIXELFORMAT, 0, 0, WIDTH, HEIGHT, PITCH,
                 (void *)VRAM_DRAW_BUFFER_OFFSET,
                 0, 0, PITCH, (void *)DISPLAY_BUFFER);
  sceGuFinish();
  _sceGeListEnQueue((void *)RENDER_LIST, NULL, -1, NULL);
}

int sceGeListUpdateStallAddrPatched(int qid, void *stall) {
  int k1 = pspSdkSetK1(0);
  char info[64];
  if (_sceGeGetList(qid, info, NULL) == 0) {
    u16 state = *(u16 *)(info + 0x08);
    if (state != 3) { // completed
      void *list = *(void **)(info + 0x18); // previous stall
      if (!list)
        list = *(void **)(info + 0x14); // list
      if (((u32)list & 0x0fffffff) < ((u32)stall & 0x0fffffff)) {
        patchGeList((u32 *)((u32)list & 0x0fffffff), (u32 *)((u32)stall & 0x0fffffff));
      }
    }
  }
  pspSdkSetK1(k1);
  return _sceGeListUpdateStallAddr(qid, stall);
}

int sceGeListEnQueuePatched(const void *list, void *stall, int cbid, PspGeListArgs *arg) {
  if (state.has_draws)
    copyFrameBuffer();
  resetGeState();
  patchGeList((u32 *)((u32)list & 0x0fffffff), (u32 *)((u32)stall & 0x0fffffff));
  return _sceGeListEnQueue(list, stall, cbid, arg);
}

int sceGeListEnQueueHeadPatched(const void *list, void *stall, int cbid, PspGeListArgs *arg) {
  resetGeState();
  patchGeList((u32 *)((u32)list & 0x0fffffff), (u32 *)((u32)stall & 0x0fffffff));
  return _sceGeListEnQueueHead(list, stall, cbid, arg);
}

int sceDisplaySetFrameBufPatched(void *topaddr, int bufferwidth, int pixelformat, int sync) {
  trackDisplayFramebuf((u32)topaddr);
  copyFrameBuffer();
  return _sceDisplaySetFrameBuf(topaddr, bufferwidth, pixelformat, sync);
}

int module_start(SceSize args, void *argp) {
  SceCtrlData pad;
  sceCtrlPeekBufferPositive(&pad, 1);
  if (pad.Buttons & PSP_CTRL_LTRIGGER)
    return 0;

  loadCompatProfile();

#if GEPATCH_TRACE_EXPORT
  traceInitFile();
#endif

  _sceGeEdramGetAddr = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0xE47E40E4);
  _sceGeEdramGetSize = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0x1F6752AD);
  _sceGeGetList = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0x67B01D8E);
  _sceGeListUpdateStallAddr = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0xE0D68148);
  _sceGeListEnQueue = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0xAB49E76A);
  _sceGeListEnQueueHead = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0x1C0D95A6);

  sctrlHENPatchSyscall((void *)_sceGeEdramGetAddr, (void *)sceGeEdramGetAddrPatched);
  sctrlHENPatchSyscall((void *)_sceGeEdramGetSize, (void *)sceGeEdramGetSizePatched);
  sctrlHENPatchSyscall((void *)_sceGeListUpdateStallAddr, (void *)sceGeListUpdateStallAddrPatched);
  sctrlHENPatchSyscall((void *)_sceGeListEnQueue, (void *)sceGeListEnQueuePatched);
  sctrlHENPatchSyscall((void *)_sceGeListEnQueueHead, (void *)sceGeListEnQueueHeadPatched);

  _sceDisplaySetFrameBuf = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver", 0x289D82FE);
  sctrlHENPatchSyscall((void *)_sceDisplaySetFrameBuf, (void *)sceDisplaySetFrameBufPatched);

  sceKernelDcacheWritebackInvalidateAll();
  sceKernelIcacheClearAll();

  return 0;
}
