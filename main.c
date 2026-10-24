#include <pspsdk.h>
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspinit.h>

#include <stdio.h>
#include <string.h>

#include <systemctrl.h>

#include "ge_list.h"

void sendCommandi(int cmd, int argument);  // gu.c

PSP_MODULE_INFO("GePatchUnofficial", 0x1007, 1, 0);

#define log(...) \
{ \
  char msg[256]; \
  sprintf(msg,__VA_ARGS__); \
  logmsg(msg); \
}

void logmsg(char *msg) {
  int k1 = pspSdkSetK1(0);

  SceUID fd = sceIoOpen("ms0:/ge_patch_unofficial.txt", PSP_O_WRONLY | PSP_O_CREAT, 0777);
  if (fd >= 0) {
    sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoWrite(fd, msg, strlen(msg));
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
  log("invalid address: %08x\n", addr);
  return -1;
}

static int display_setframebuf_seen = 0;

void ge_list_flush_cache(void) {
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

static u32 native_frame;

void copyFrameBuffer() {
  *(u32 *)DRAW_NATIVE = 1;
  // Uncached, so the GE reads the new value.
  *(volatile u32 *)(NATIVE_FRAME_SOURCE | 0xA0000000) = ++native_frame;

  // memcpy((void *)DISPLAY_BUFFER, (void *)VRAM_DRAW_BUFFER_OFFSET, 960*544*2);
  sceGuStart(0, (void *)(RENDER_LIST | 0xA0000000));
  sceGuCopyImage(PIXELFORMAT, 0, 0, WIDTH, HEIGHT, PITCH,
                 (void *)VRAM_DRAW_BUFFER_OFFSET,
                 0, 0, PITCH, (void *)DISPLAY_BUFFER);
  // One 32-bit pixel: the frame number, after the picture.
  sceGuCopyImage(GE_FORMAT_8888, 0, 0, 1, 1, 8, (void *)NATIVE_FRAME_SOURCE,
                 0, 0, 8, (void *)NATIVE_FRAME_COUNTER);
  if (ge_list_dither) {
    // For the next frame (the walker keeps the XMB from changing it)
    sendCommandi(GE_CMD_DITH0, GE_DITHER_ROW0);
    sendCommandi(GE_CMD_DITH1, GE_DITHER_ROW1);
    sendCommandi(GE_CMD_DITH2, GE_DITHER_ROW2);
    sendCommandi(GE_CMD_DITH3, GE_DITHER_ROW3);
    sendCommandi(GE_CMD_DITHERENABLE, 1);
  }
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

// Streaming: a complete list (no stall address from the game) is enqueued stalled at its start and
// released piece by piece while the walk patches it, so the GE starts rendering during the walk
// instead of after it. The stall only moves to positions before which everything is patched.
// The first release comes early, so the GE starts almost at once; the interval then doubles, as the GE
// (~2.5 commands per us in MHP3rd) falls behind the walk (~8 per us) and needs fewer releases.
#define STREAM_FIRST 128
#define STREAM_MAX 4096
static int stream_qid;
static u32 stream_hi;  // the game's address bits above 0x0fffffff (cached/uncached view)

static void stream_progress(u32 *safe) {
  sceKernelDcacheWritebackAll();
  _sceGeListUpdateStallAddr(stream_qid, (void *)((u32)safe | stream_hi));
  if (ge_list_progress_every < STREAM_MAX)
    ge_list_progress_every *= 2;
}

int sceGeListEnQueuePatched(const void *list, void *stall, int cbid, PspGeListArgs *arg) {
  // Games that call sceDisplaySetFrameBuf once per visual frame (e.g. 3D games
  // with multiple GE sub-passes) use that as the copy point. Copying here too
  // would blit a half-rendered frame and cause flickering every other frame.
  if (state.has_draws && !display_setframebuf_seen)
    copyFrameBuffer();
  if (!stall) {
    const int qid = _sceGeListEnQueue(list, (void *)list, cbid, arg);
    if (qid >= 0) {
      stream_qid = qid;
      stream_hi = (u32)list & 0xf0000000;
      ge_list_progress = stream_progress;
      ge_list_progress_every = STREAM_FIRST;
      resetGeState();
      patchGeList((u32 *)((u32)list & 0x0fffffff), NULL);
      ge_list_progress = NULL;
      // Release the rest: past the END that finished the walk, or no stall at all.
      _sceGeListUpdateStallAddr(qid, ge_list_end ? (void *)((u32)(ge_list_end + 1) | stream_hi) : NULL);
      return qid;
    }
    // Enqueue refused: patch first as usual and let the original call report the error.
  }
  resetGeState();
  patchGeList((u32 *)((u32)list & 0x0fffffff), (u32 *)((u32)stall & 0x0fffffff));
  return _sceGeListEnQueue(list, stall, cbid, arg);
}

int sceGeListEnQueueHeadPatched(const void *list, void *stall, int cbid, PspGeListArgs *arg) {
  resetGeState();
  patchGeList((u32 *)((u32)list & 0x0fffffff), (u32 *)((u32)stall & 0x0fffffff));
  return _sceGeListEnQueueHead(list, stall, cbid, arg);
}

// XMB boot video: frames since the last video transfer while passing through
#define PASSTHROUGH_QUIET_FRAMES 60
static u32 passthrough_quiet;
static int passthrough_state_sent;  // the GE has the state for this pass-through setting

int sceDisplaySetFrameBufPatched(void *topaddr, int bufferwidth, int pixelformat, int sync) {
  display_setframebuf_seen = 1;

  // A frame the GE did not draw: the game displays a buffer in main RAM outside the fake VRAM
  // (MHP3rd's intro video shows its decoded frames this way, 32-bit). Show it as it is, natively;
  // decided per frame, so the next GE frame is 2x again (the lists stay patched meanwhile).
  // Adrenaline hands the screen back to the PSP emulator when DRAW_NATIVE drops.
  const u32 shown = (u32)topaddr & 0x0fffffff;
  if (shown >= 0x08800000 && shown < FAKE_VRAM) {
    *(u32 *)DRAW_NATIVE = 0;
    return _sceDisplaySetFrameBuf(topaddr, bufferwidth, pixelformat, sync);
  }

  // XMB boot video (see ge_list_xmb): end the pass-through when no video transfer came for a while
  if (ge_list_passthrough) {
    if (ge_list_video_seen)
      passthrough_quiet = 0;
    else if (++passthrough_quiet > PASSTHROUGH_QUIET_FRAMES)
      ge_list_passthrough = 0;  // back to 2x from the next frame
  }
  ge_list_video_seen = 0;
  // Entering or leaving pass-through: set the GE registers GePatch rewrites (they stay in the GE
  // until set again) to the XMB's own values, or back to GePatch's. Runs before the next frame.
  if (ge_list_passthrough != passthrough_state_sent) {
    ge_list_xmb_state((u32 *)(XMB_STATE_LIST | 0xA0000000), !ge_list_passthrough);
    _sceGeListEnQueue((void *)XMB_STATE_LIST, NULL, -1, NULL);
    passthrough_state_sent = ge_list_passthrough;
  }
  if (ge_list_passthrough) {
    // Native picture: the XMB renders unpatched into VRAM, where the video is copied too. Show
    // that (Adrenaline copies the PSP frame from VRAM; for the PSP emulator's own display, point it there).
    *(u32 *)DRAW_NATIVE = 0;
    if (((u32)topaddr & 0x0fe00000) == FAKE_VRAM)
      topaddr = (void *)(0x44000000 | ((u32)topaddr & 0x001fffff));
    return _sceDisplaySetFrameBuf(topaddr, bufferwidth, pixelformat, sync);
  }

  copyFrameBuffer();
  return _sceDisplaySetFrameBuf(topaddr, bufferwidth, pixelformat, sync);
}

int module_start(SceSize args, void *argp) {
  SceCtrlData pad;
  sceCtrlPeekBufferPositive(&pad, 1);
  if (pad.Buttons & PSP_CTRL_LTRIGGER)
    return 0;

  // The XMB (same check as Adrenaline's plugin loader). Starting a game reboots the PSP side and
  // loads GePatch again for the game, so this never carries over into a game.
  ge_list_dither = sceKernelInitApitype() >= PSP_INIT_APITYPE_VSH_KERNEL;
  ge_list_xmb = ge_list_dither;

  _sceGeEdramGetAddr = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0xE47E40E4);
  _sceGeEdramGetSize = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0x1F6752AD);
  _sceGeGetList = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0x67B01D8E);
  _sceGeListUpdateStallAddr = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0xE0D68148);
  _sceGeListEnQueue = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0xAB49E76A);
  _sceGeListEnQueueHead = (void *)sctrlHENFindFunction("sceGE_Manager", "sceGe_driver", 0x1C0D95A6);

  sctrlHENPatchSyscall(_sceGeEdramGetAddr, sceGeEdramGetAddrPatched);
  sctrlHENPatchSyscall(_sceGeEdramGetSize, sceGeEdramGetSizePatched);
  sctrlHENPatchSyscall(_sceGeListUpdateStallAddr, sceGeListUpdateStallAddrPatched);
  sctrlHENPatchSyscall(_sceGeListEnQueue, sceGeListEnQueuePatched);
  sctrlHENPatchSyscall(_sceGeListEnQueueHead, sceGeListEnQueueHeadPatched);

  _sceDisplaySetFrameBuf = (void *)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver", 0x289D82FE);
  sctrlHENPatchSyscall(_sceDisplaySetFrameBuf, sceDisplaySetFrameBufPatched);

  sceKernelDcacheWritebackInvalidateAll();
  sceKernelIcacheClearAll();

  return 0;
}
