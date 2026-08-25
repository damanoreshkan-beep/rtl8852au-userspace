# Native `rf.attach` in the shell APK — grounded plan

Goal: make the deployed **ax56chat** PWA radiate/receive off-grid chat over a **real** connected AX56 when run
inside an installable APK — by adding a native `rf.attach`/`rf.tx`/`rf.rx` bridge action that reuses the
**proven** `tool/hwdriver.c` driver. Research complete + feasibility confirmed 2026-08-25; this is the build map.

## The architecture (confirmed by reading the edge + template)

- The microspec **shell is a template APK** whose source is `microspec-edge/template/` (real Gradle project:
  `MainActivity.java`, `Catalogue.java`, `ShellBridge.java`, `Usb.java`, …). It is **built by GitHub Actions**
  (`microspec-edge/.github/workflows/build-template-apk.yml`, which has the Android SDK/NDK), embedded as
  base64 by `edge/apk/embed-template.ts` → `edge/apk/template-full.b64.js` (currently 82 KB, pure Java).
- The **per-request `/feed/apk`** flow only PATCHES that template (sets the app URL + icon) + **pure-Deno
  v1-sign** (`apk.js buildApk` → edge). It CANNOT add native/assets — the manifest + native are baked.
  **So new native/actions require a TEMPLATE REBUILD** (a CI round), not a per-request patch.
- Actions are declared in `microspec/packages/shell/actions.json` (each: `capability`, `minBridge`),
  generate `shell-actions.js` (`tools/shell-gen.mjs`), dispatched natively in `ShellBridge.java` (full flavor).
- **`Usb.java` (full flavor) already implements `usb.*` in PURE JAVA** — `UsbDeviceConnection` control/bulk,
  `switchMode()` (SCSI eject), and `batch()` (a flat op list of control/bulk/**poll**, so a thousands-of-ops
  bring-up is ONE bridge round-trip). It keeps one `conn`/`iface`. **The USB fd + transfer plumbing is there.**

## The plan — reuse the proven driver as a JNI lib (NOT the wall-hitting native-bridge.cpp)

The cal (initcal/iqk/tssi/dpk) is **adaptive** (read→compute→write) → cannot be a static `usb.batch` op list.
So it runs natively. Reuse **exactly** the proven `tool/hwdriver.c` (hwburst_fwdl + replay + live cal), driven
by the Android USB fd via `libusb_wrap_sys_device` — **the same fd-wrap we already proved through termux-usb.**

1. **Native lib `libax56rf.so`** (in the template):
   - `tool/hwdriver.c` refactored: guard `main()` with `#ifndef RF_LIB`; extract the replay-blob loop
     (lines ~442–494: kind 1 write / 3 read / 4 poll / 2 bulk, with the `0x2f0` re-fwdl hwburst substitution
     and the `0x1bff8` IQK/DPK done-poll + the ep7 section burst) into `static int replay_ops(blob, sz, start)`.
   - `rf_jni.c`: `#define RF_LIB`, `#include "hwdriver.c"`; JNI entries:
     - `rf_attach(fd, channel, fw[], blob[])`: `g_txfd=fd` → `reopen()` (`libusb_wrap_sys_device` +
       `LIBUSB_OPTION_NO_DEVICE_DISCOVERY`) → `hwburst_fwdl` → `replay_ops(blob,…,START=2329470)` →
       `initcal()` → `iqk_chain(0)`+`iqk_chain(1)`+`iqk_afebb_restore()` → `tssi_live()` → `dpk_live_run()`.
       (This is exactly the proven coexistence chain — RX + TX both clean **from a COLD chip**.)
     - `rf_tx(fd, frame[])`: bulk-OUT EP5 (`[txdesc][beacon]`, built farm-side by `rf.js buildTxPacket`).
     - `rf_rx(fd, out[])`: bulk-IN EP0x84 → `parseRx` → return WIFI frames (farm-side `extractChunk` decodes).
   - **libusb for Android**: cross-compiled with the NDK (a small Android config). Link static into the .so.
2. **Java `Ax56.java`**: reuse `Usb.open` to grant + `UsbManager.openDevice`, then `conn.getFileDescriptor()`
   → JNI. **Do NOT `claimInterface` in Java** — native claims via the fd (matches the termux-usb path, avoids
   a "busy" double-claim). `switchMode()` first if the adapter is in storage (`0bda:1a2b`).
   `ShellBridge` (full) dispatches `rf.attach`/`rf.tx`/`rf.rx`.
3. **Catalogue.java + `actions.json`**: add `rf.attach`/`rf.tx`/`rf.rx` (capability `"usb"`), **bump
   `CATALOGUE_BRIDGE`** so `shell.has()` negotiates it; regen `shell-actions.js` (`tools/shell-gen.mjs`).
4. **Assets** (private edge, NEVER the public farm): `fw_cut2_nic.bin` + `full_ch6.bin.gz` in
   `template/app/src/full/assets/` — passed to `rf_attach` as `byte[]`. (fw is proprietary Realtek; the edge
   repo is private, so this is fine; the public microspec farm never ships them.)
5. **Build**: the **box** (Linux + NDK r26d) cross-compiles libusb + `hwdriver.c`/`rf_jni.c` →
   `template/app/src/full/jniLibs/arm64-v8a/libax56rf.so` (committed prebuilt → the template CI just packages
   it; simpler than externalNativeBuild). `build-template-apk` CI builds the APK → `embed-template.ts full` →
   `template-full.b64.js` → deploy edge (`edge/apk/*.js` are deployed together — see apk-sdk-plan §Deploy).
6. **`rf.js`** (`microspec/packages/runtime/rf.js`): `createRfCarrier.start()` → `shell.call("rf.attach",
   {channel, fw?, blob?})`; `send(chunk)` → `shell.call("rf.tx",{frame: hex(buildTxPacket(chunk))})`;
   poll `shell.call("rf.rx")` → `extractChunk` → cb. (Or keep `usb.bulk` for tx/rx and only `rf.attach` new.)

## Hard rules / risks (so the on-device test is first-try)

- **COLD chip only.** A warm chip gives degraded/near-dead RX (proven — analog front-end, not fixable in
  software; see [[project-ax56chat]]). The APK must tell the user to REPLUG before `rf.attach`, then
  `switchMode` (storage→wifi cold 0xc0) → attach.
- **Java must not claim the interface** — native claims via the fd (double-claim = EBUSY).
- **libusb Android build** is the main native unknown; validate the cross-compile on the box first.
- **APK size**: the `full_ch6.bin` blob is ~2.5 MB (gz ~33 KB) — ship gzipped, decompress in native/Java.
- The bring-up wedges a dirty chip → the app surfaces "replug" honestly (the driver already aborts on STS!=7).

## Progress (2026-08-25)

Research complete; the replay loop + cal chain understood (`hwdriver.c` 430–520). NDK r26d being staged onto
the box (box has no DNS over its VPN uplink → download on the phone, `scp` to box). Remaining: cross-compile
`.so`, the Java (`Ax56.java` + `ShellBridge`/`Catalogue`/`actions.json`), assets, CI template rebuild, edge
deploy, `rf.js`, then the on-device test. This is a multi-step build, driven on the box + CI (no toolchain on
the phone/VPS).
