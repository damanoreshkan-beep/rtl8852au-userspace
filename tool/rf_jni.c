// rf_jni.c — the JNI shim that makes the PROVEN no-root bring-up (hwdriver.c) callable from the microspec
// shell APK, so ax56chat radiates over a real AX56. It is a THIN wrapper: only the adaptive bring-up (fwdl +
// captured-config replay + live cal) is native — TX (EP5) and RX (EP0x84) stay on the pure-Java `usb.bulk`
// bridge and the rxd-parse lives in rf.js. So the single native entry point is nativeAttach().
//
// The device fd comes from Java (UsbDeviceConnection.getFileDescriptor()); we wrap it with libusb exactly as
// the termux-usb path does (LIBUSB_OPTION_NO_DEVICE_DISCOVERY + libusb_wrap_sys_device) — the same proven
// no-root path. Java opens+claims the interface for its own usb.bulk; our libusb_claim_interface on the SAME
// fd is idempotent (harmless double-claim), never EBUSY (that only happens across different fds).
//
// COLD chip only: a warm chip has degraded RX (analog front-end, not software-fixable). hwburst_fwdl returns
// non-zero (STS!=7) on a dirty chip; we surface that so the app tells the user to replug.

#define RF_LIB
#include "hwdriver.c"   // brings in every static: reopen, hwburst_fwdl, replay_ops, initcal, iqk_chain,
                        // iqk_afebb_restore, tssi_live, dpk_live_run, and the dev/LOG/g_txfd/g_fwpath globals.
#include <jni.h>
#include <android/log.h>

#define A(...) __android_log_print(ANDROID_LOG_INFO, "ax56rf", __VA_ARGS__)

static uint8_t* slurp(const char*path, long*outsz){
  FILE*f=fopen(path,"rb"); if(!f) return NULL;
  fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
  uint8_t*b=malloc(sz); if(!b){ fclose(f); return NULL; }
  if(fread(b,1,sz,f)!=(size_t)sz){ free(b); fclose(f); return NULL; }
  fclose(f); *outsz=sz; return b;
}

// nativeAttach(fd, channel, fwPath, blobPath, startByte, logPath) -> 0 ok, negative = failure code.
// Runs the proven cold-chip coexistence chain: fwdl -> replay captured monitor tail -> live cal (IQK/TSSI/DPK).
// After this returns 0 the adapter is on-air on `channel`; the caller uses usb.bulk EP5/EP0x84 for TX/RX.
JNIEXPORT jint JNICALL
Java_apk_microspec_Ax56_nativeAttach(JNIEnv*env, jclass cls, jint fd, jint channel,
    jstring jfw, jstring jblob, jlong startByte, jstring jlog, jint cal){
  (void)cls; (void)channel;
  const char*fw   = (*env)->GetStringUTFChars(env, jfw,   NULL);
  const char*blobp= (*env)->GetStringUTFChars(env, jblob, NULL);
  const char*logp = jlog ? (*env)->GetStringUTFChars(env, jlog, NULL) : NULL;

  LOG = logp ? fopen(logp,"w") : NULL; if(!LOG) LOG=stderr;
  g_txfd  = fd;
  g_fwpath = strdup(fw);   // hwburst_fwdl fopens this path
  A("nativeAttach fd=%d ch=%d fw=%s blob=%s start=%ld", fd, channel, fw, blobp, (long)startByte);

  int ret = 0;
  libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
  if(libusb_init(NULL) < 0){ P("libusb init fail\n"); ret=-1; goto done; }
  if(reopen() < 0){ P("wrap fd fail (fd=%d)\n", fd); ret=-2; goto done; }
  P("attach: entry 0x1e0=0x%x 0xF0=0x%x\n", r32(0x1e0), r32(0xf0));

  long bsz=0; uint8_t*blob=slurp(blobp,&bsz);
  if(!blob){ P("no blob at %s\n", blobp); ret=-3; goto done; }

  int rc=hwburst_fwdl(g_fwpath);
  if(rc!=0){ P("*** FWDL DID NOT BOOT (STS!=7, 0x1e0=0x%x) -- chip dirty; COLD REPLUG needed.\n", r32(0x1e0));
    free(blob); ret=-4; goto done; }

  replay_ops(blob, bsz, (long)startByte);          // captured monitor-ch tail: BB/RF init + set-channel + RX filter
  free(blob);

  // Live TX calibration (IQK/TSSI/DPK) is 2.4 GHz-tuned and proven for the ch6 coexistence chat; it is NOT
  // valid on a 5 GHz channel, and RX (the monitor map) needs no calibration at all. So cal is opt-in per channel:
  // ch6 runs the full chain for clean TX, 5 GHz stays replay-only (RX-capable, TX uncalibrated).
  if(cal){
    initcal();                                     // INITCAL / RCK / DACK
    iqk_chain(0); iqk_chain(1); iqk_afebb_restore(); // live IQK -> LOK, both paths, exit cal mode
    tssi_live();                                   // live TSSI
    dpk_live_run();                                // live DPK
  } else {
    P("attach: replay-only (cal skipped) — RX/monitor on channel %d\n", channel);
  }

  P("attach done: 0x1e0=0x%x 0xF0=0x%x (healthy=%s)\n", r32(0x1e0), r32(0xf0),
    r32(0xf0)==0xc492537 ? "YES" : "check");
  A("nativeAttach OK 0xF0=0x%x", r32(0xf0));

done:
  (*env)->ReleaseStringUTFChars(env, jfw,   fw);
  (*env)->ReleaseStringUTFChars(env, jblob, blobp);
  if(logp) (*env)->ReleaseStringUTFChars(env, jlog, logp);
  return ret;
}

// nativeDetach() -> release the interface + close libusb so a later attach re-wraps a fresh fd cleanly.
JNIEXPORT void JNICALL
Java_apk_microspec_Ax56_nativeDetach(JNIEnv*env, jclass cls){
  (void)env; (void)cls;
  if(dev){ libusb_release_interface(dev,0); libusb_close(dev); dev=NULL; }
  libusb_exit(NULL);
  g_txfd = -1;
  A("nativeDetach");
}
