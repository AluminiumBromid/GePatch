// GE list walker: patches a PSP display list in place for 960x544 rendering.
// Platform-independent apart from the cache flush, so tests/ can run it on captured lists.

#include "ge_list.h"

#include <string.h>

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

GeState state;
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

int findFramebuf(u32 framebuf) {
  int i;
  for (i = 0; i < state.framebuf_count; i++) {
    // Some textures point to within the framebuf.
    // If we want to be correct we should record the pixelsize, linewidth and height.
    if (framebuf >= state.framebuf_addr[i] && framebuf < (state.framebuf_addr[i] + 0x400)) {
      return i;
    }
  }

  return -1;
}

int insertFramebuf(u32 framebuf) {
  int res = findFramebuf(framebuf);
  if (res >= 0)
    return res;

  if (state.framebuf_count < (sizeof(state.framebuf_addr) / sizeof(u32))) {
    state.framebuf_addr[state.framebuf_count] = framebuf;
    return state.framebuf_count++;
  }

  return -1;
}

void AdvanceVerts(int count, int vertex_size) {
  if ((state.vertex_type & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
    int index_shift = ((state.vertex_type & GE_VTYPE_IDX_MASK) >> GE_VTYPE_IDX_SHIFT) - 1;
    state.index_addr += count << index_shift;
  } else {
    state.vertex_addr += count * vertex_size;
  }
}

// TODO: when ignore_framebuf=1 or ignore_texture=1, dummy all non-control-flow instructions
u32 ge_list_commands, ge_list_vertices;

// What the walker does with each command. Most commands (matrix data, material colours, ...) need
// nothing, and draws only in through mode or when they are dropped: the loop skips those first.
enum { CMD_SKIP, CMD_HANDLE, CMD_DRAW };
static const u8 cmd_kind[256] = {
  [GE_CMD_BASE] = CMD_HANDLE, [GE_CMD_OFFSETADDR] = CMD_HANDLE, [GE_CMD_ORIGIN] = CMD_HANDLE,
  [GE_CMD_CALL] = CMD_HANDLE, [GE_CMD_BJUMP] = CMD_HANDLE, [GE_CMD_JUMP] = CMD_HANDLE,
  [GE_CMD_RET] = CMD_HANDLE, [GE_CMD_END] = CMD_HANDLE,
  [GE_CMD_IADDR] = CMD_HANDLE, [GE_CMD_VADDR] = CMD_HANDLE, [GE_CMD_VERTEXTYPE] = CMD_HANDLE,
  [GE_CMD_BEZIER] = CMD_DRAW, [GE_CMD_SPLINE] = CMD_DRAW, [GE_CMD_PRIM] = CMD_DRAW,
  [GE_CMD_BOUNDINGBOX] = CMD_HANDLE,
  [GE_CMD_TEXTUREMAPENABLE] = CMD_HANDLE,  // read by the draws (state.ge_cmd)
  [GE_CMD_FRAMEBUFPIXFORMAT] = CMD_HANDLE, [GE_CMD_FRAMEBUFPTR] = CMD_HANDLE, [GE_CMD_FRAMEBUFWIDTH] = CMD_HANDLE,
  [GE_CMD_ZBUFPTR] = CMD_HANDLE, [GE_CMD_ZBUFWIDTH] = CMD_HANDLE,
  [GE_CMD_TEXADDR0 ... GE_CMD_TEXADDR7] = CMD_HANDLE,
  [GE_CMD_TEXBUFWIDTH0 ... GE_CMD_TEXBUFWIDTH7] = CMD_HANDLE,
  [GE_CMD_VIEWPORTXSCALE] = CMD_HANDLE, [GE_CMD_VIEWPORTYSCALE] = CMD_HANDLE,
  [GE_CMD_VIEWPORTXCENTER] = CMD_HANDLE, [GE_CMD_VIEWPORTYCENTER] = CMD_HANDLE,
  [GE_CMD_OFFSETX] = CMD_HANDLE, [GE_CMD_OFFSETY] = CMD_HANDLE,
  [GE_CMD_REGION2] = CMD_HANDLE, [GE_CMD_SCISSOR2] = CMD_HANDLE,
  [GE_CMD_DITHERENABLE] = CMD_HANDLE, [GE_CMD_DITH0 ... GE_CMD_DITH3] = CMD_HANDLE,  // XMB dithering
};

int ge_list_dither;
static const u32 dither_rows[4] = { GE_DITHER_ROW0, GE_DITHER_ROW1, GE_DITHER_ROW2, GE_DITHER_ROW3 };

void (*ge_list_progress)(u32 *safe);
u32 ge_list_progress_every;
u32 *ge_list_end;
u32 *ge_list_stream_stop;
u32 ge_list_stream_stop_cmd;
u32 ge_list_stream_stop_walked;

static void stop_streaming(u32 *at, u32 cmd, u32 walked) {
  if (ge_list_progress) {
    ge_list_stream_stop = at;
    ge_list_stream_stop_cmd = cmd;
    ge_list_stream_stop_walked = walked;
  }
  ge_list_progress = NULL;
}

void patchGeList(u32 *list, u32 *stall) {
  union {
    float f;
    unsigned int i;
  } t;

  StackEntry *stack_entry;
  StackEntry stack_entry_buf;

  u32 walked = 0;
  u32 next_progress = ge_list_progress_every;
  ge_list_end = NULL;
  ge_list_stream_stop = NULL;
  ge_list_stream_stop_cmd = 0;

  for (; list && (!stall || (stall && list != stall)); list++) {
    walked++;
    // if (checkAddress((u32)list) < 0)
      // goto finish;

    u32 op = *list;
    u32 cmd = op >> 24;
    const u8 kind = cmd_kind[cmd];
    if (kind == CMD_SKIP)
      continue;
    if (kind == CMD_DRAW) {
      state.has_draws = 1;
      if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) != GE_VTYPE_THROUGH && !state.ignore_framebuf &&
          !(state.ignore_texture && state.ge_cmd[GE_CMD_TEXTUREMAPENABLE]))
        continue;
    }
    if (ge_list_progress && walked >= next_progress && state.curr_stack == 0) {
      ge_list_progress(list);  // may change ge_list_progress_every
      next_progress = walked + ge_list_progress_every;
    }
    u32 data = op & 0xffffff;

    state.ge_cmd[cmd] = data;

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
        break;

      case GE_CMD_OFFSETADDR:
        state.offset = data << 8;
        break;

      case GE_CMD_ORIGIN:
        state.offset = (u32)list;
        break;

      // TODO: need to save other states, too?
      case GE_CMD_CALL:
        state.address = ((state.base | data) + state.offset) & 0x0ffffffc;
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
        // The GE may not take it: stop streaming.
        stop_streaming(list, cmd, walked);
        state.address = ((state.base | data) + state.offset) & 0x0ffffffc;
        list = (u32 *)(state.address - 4);
        break;

      case GE_CMD_JUMP:
        state.address = ((state.base | data) + state.offset) & 0x0ffffffc;
        list = (u32 *)(state.address - 4);
        break;

      case GE_CMD_RET:
        // Ignore returns when the stack is empty. The GE continues there too (MHP3rd's main list
        // runs into inline bone-matrix blocks ending in RET and draws everything after them), so
        // streaming continues.
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
            stop_streaming(list, ((prev >> 16) & 0xff) | 0x100, walked);  // 0x1xx: signal behaviour
            u8 behaviour = (prev >> 16) & 0xff;
            u16 signal = prev & 0xffff;
            u16 enddata = data & 0xffff;
            u32 target;

            switch (behaviour) {
              case PSP_GE_SIGNAL_SYNC:
                list += 2;
                break;

              case PSP_GE_SIGNAL_JUMP:
                target = (((signal << 16) | enddata) & 0x0ffffffc);
                list = (u32 *)(target - 4);
                break;

              case PSP_GE_SIGNAL_CALL:
                target = (((signal << 16) | enddata) & 0x0ffffffc);
                stack_entry = &stack_entry_buf;
                stack_entry->list = list;
                stack_entry->base = state.base;
                stack_entry->offset = state.offset;
                if (push(stack_entry) == 0)
                  list = (u32 *)(target - 4);
                break;

              case PSP_GE_SIGNAL_RET:
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
            ge_list_end = list;
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
        break;

      case GE_CMD_BEZIER:
      case GE_CMD_SPLINE:
      {
        state.has_draws = 1;

        if (state.ignore_framebuf || (state.ignore_texture && state.ge_cmd[GE_CMD_TEXTUREMAPENABLE])) {
          *list = 0;
          break;
        }

        if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) == GE_VTYPE_THROUGH) {
          u8 num_points_u = data & 0xff;
          u8 num_points_v = (data >> 8) & 0xff;

          u32 count = num_points_u * num_points_v;

          u8 vertex_size = 0, pos_off = 0, visit_off = 0;
          getVertexInfo(state.vertex_type, &vertex_size, &pos_off, &visit_off);

          AdvanceVerts(count, vertex_size);
        }

        break;
      }

      case GE_CMD_BOUNDINGBOX:
      {
        if (state.ignore_framebuf || (state.ignore_texture && state.ge_cmd[GE_CMD_TEXTUREMAPENABLE]))
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

        // Dragon Ball Z Tenkaichi Tag Team uses the same GE list again,
        // therefore NOPing it makes character invisible.
        if (state.ignore_framebuf || (state.ignore_texture && state.ge_cmd[GE_CMD_TEXTUREMAPENABLE])) {
          *list = 0;
          break;
        }

        if ((state.vertex_type & GE_VTYPE_THROUGH_MASK) == GE_VTYPE_THROUGH) {
          u16 count = data & 0xffff;

          u8 vertex_size = 0, pos_off = 0, visit_off = 0;
          getVertexInfo(state.vertex_type, &vertex_size, &pos_off, &visit_off);

          u16 lower = 0;
          u16 upper = count;
          if ((state.vertex_type & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
            GetIndexBounds((void *)state.index_addr, count, state.vertex_type, &lower, &upper);
            upper += 1;
          }

          int pos = (state.vertex_type & GE_VTYPE_POS_MASK) >> GE_VTYPE_POS_SHIFT;
          int pos_size = possize[pos] / 3;

          // TODO: we may patch the same vertex again and again...
          u8 decoded = 0, encoded = 0;
          u32 vertex_addr = state.vertex_addr + lower * vertex_size;
          int i;
          for (i = lower; i < upper; i++, vertex_addr += vertex_size) {
            ge_list_vertices++;
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
          AdvanceVerts(count, vertex_size);
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
          // 480 is the tight-pack PSP display pitch (480px * 2bpp = 960 bytes/row),
          // 512 is the padded variant. Both scale to 960 to match vertex coordinates.
          u32 raw_pitch = op & 0xffff;
          u16 pitch = (raw_pitch == 480 || raw_pitch == 512) ? 960 : (u16)raw_pitch;
          *list = (cmd << 24) | ((VRAM_DRAW_BUFFER_OFFSET >> 24) << 16) | pitch;
          state.framebufwidth = op;
          state.framebufwidth_addr = list;
        }

        if (state.framebufptr && state.framebufwidth) {
          state.framebuf = FAKE_VRAM | (state.framebufptr & 0xffffff);

          // This allows more games to work, but causes weird triangles in Sonic.
          u32 pitch = state.framebufwidth & 0xffff;
          if (pitch == 480 || pitch == 512 || pitch == 960) {
            *state.framebufwidth_addr = (GE_CMD_FRAMEBUFWIDTH << 24) | ((VRAM_DRAW_BUFFER_OFFSET >> 24) << 16) | 960;
            state.ignore_framebuf = 0;
          } else {
            *state.framebufwidth_addr = (GE_CMD_FRAMEBUFWIDTH << 24) | ((VRAM_1KB >> 24) << 16) | 0;
            state.ignore_framebuf = 1;
          }

          insertFramebuf(state.framebuf);

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
          u32 texaddr = ((state.texbufwidth[index] & 0x0f0000) << 8) | (state.texbufptr[index] & 0xffffff);
          if (texaddr != state.framebuf && findFramebuf(texaddr) >= 0) {
            state.ignore_texture = 1;
          } else {
            state.ignore_texture = 0;
          }

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

      case GE_CMD_DITHERENABLE:
        if (ge_list_dither)
          *list = (cmd << 24) | 1;
        break;

      case GE_CMD_DITH0:
      case GE_CMD_DITH1:
      case GE_CMD_DITH2:
      case GE_CMD_DITH3:
        if (ge_list_dither)
          *list = (cmd << 24) | dither_rows[cmd - GE_CMD_DITH0];
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
  ge_list_commands += walked;
  ge_list_flush_cache();
}
