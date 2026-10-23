#include <pspsdk.h>
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspge.h>
#include <pspgu.h>

#include <stdio.h>
#include <string.h>

#include <systemctrl.h>

#include "ge_list.h"

PSP_MODULE_INFO("GePatch", 0x1007, 1, 0);

#define log(...) \
{ \
  char msg[256]; \
  sprintf(msg,__VA_ARGS__); \
  logmsg(msg); \
}

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
  // Games that call sceDisplaySetFrameBuf once per visual frame (e.g. 3D games
  // with multiple GE sub-passes) use that as the copy point. Copying here too
  // would blit a half-rendered frame and cause flickering every other frame.
  if (state.has_draws && !display_setframebuf_seen)
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
  display_setframebuf_seen = 1;
  copyFrameBuffer();
  return _sceDisplaySetFrameBuf(topaddr, bufferwidth, pixelformat, sync);
}

int module_start(SceSize args, void *argp) {
  SceCtrlData pad;
  sceCtrlPeekBufferPositive(&pad, 1);
  if (pad.Buttons & PSP_CTRL_LTRIGGER)
    return 0;

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
