/*
 * fw_blob.c — the firmware image handed to rtw_fwdl, selected for THIS chip's cut.
 *
 * Provision at app start: read the mfw container (native/fw/rtw8852a_fw.bin) shipped
 * as an APK asset, pick the inner blob whose cv matches rtw_read_cut() and type=nic
 * (same selection as tool/fw-validate.ts), and point these at it. Left null here so
 * the symbols resolve; nativeInitDriver logs "no firmware provisioned" until set.
 */
#include <stdint.h>
const uint8_t *ax56_fw_blob = 0;
uint32_t       ax56_fw_len  = 0;
