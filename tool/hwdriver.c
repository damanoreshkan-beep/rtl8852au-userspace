// hwdriver.c — no-root userspace RTL8852AU (ASUS USB-AX56, 0b05:1997) fw-download + monitor RX, via libusb.
//
// Proves the full no-root userspace bring-up: cold power-on + firmware download (hwburst_fwdl, STS=7 BOOTED),
// then a faithful replay of a captured monitor-on-ch6 config tail, ending with real 802.11 beacons read off
// bulk-IN EP 0x84 — no kernel driver, no root beyond usbfs claim.
//
// Build (on a host with libusb-1.0 + pkg-config):
//   gcc -O2 -o hwdriver hwdriver.c $(pkg-config --cflags --libs libusb-1.0)
//
// Proven run (real beacons) — from a COLD chip (entry 0x1e0=0xc0, physical replug):
//   sudo ./hwdriver replay3.bin 2322184 hb
//   = one hwburst_fwdl (STS=7) + replay ONLY the captured cycle5 tail (byte 2322184..EOF), a self-contained
//     monitor-on-ch6 re-init (BB/RF init + RFK + set-channel + RX filter). EP 0x84 then delivers beacons
//     (fc=0x0080, broadcast DA, real diverse BSSIDs). A warm-dirty chip (0x1e0=0x23) fails hwburst -> aborts.
// args: <blob> <startByte> [hb]   blob = usbmon-capture-derived op stream; hb = do hwburst fwdl first.
// Needs /tmp/fw_cut2_nic.bin (cut2 nic firmware blob, gitignored) alongside the replay blob.
//
// Channel hopping needs no code change: it is just a different blob. Capture a full kernel monitor session on
// the target channel and parse it with tool/parse-usbmon.ts, then replay its last-cycle tail. Proven for ch1:
//   sudo ./hwdriver full_ch1.bin 2326916 hb   -> 113/140 beacons on ch1, none on ch6.
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>

static libusb_device_handle *dev;
static FILE *LOG;
#define P(...) do{ fprintf(LOG,__VA_ARGS__); fflush(LOG); }while(0)

static int g_txfd = -1;   // set from env TXFD when driven via termux-usb (no-root Android): wrap that fd
static const char* g_fwpath = "/tmp/fw_cut2_nic.bin";  // overridable via env FW (phone stages it elsewhere)
static int reopen(void){
  if(g_txfd >= 0){
    // Android/Termux no-root path: termux-usb already opened+claimed the device and passed us its fd.
    // Wrap it directly — no enumeration, no permission, no re-open (a wrapped fd cannot be reopened).
    if(dev) return 0;
    if(libusb_wrap_sys_device(NULL, (intptr_t)g_txfd, &dev) < 0 || !dev) return -1;
    libusb_claim_interface(dev,0);   // harmless if termux-usb already claimed
    return 0;
  }
  if(dev){ libusb_release_interface(dev,0); libusb_close(dev); dev=NULL; }
  dev = libusb_open_device_with_vid_pid(NULL, 0x0b05, 0x1997);
  if(!dev) return -1;
  libusb_set_auto_detach_kernel_driver(dev,1);
  libusb_claim_interface(dev,0);
  return 0;
}
static uint32_t r32(uint32_t addr){
  uint8_t b[4]={0};
  libusb_control_transfer(dev,0xC0,0x05,addr&0xffff,(addr>>16)&0xff,b,4,300);
  return b[0]|(b[1]<<8)|(b[2]<<16)|((uint32_t)b[3]<<24);
}
static void dump_regs(const char*path){
  FILE*d=fopen(path,"w"); if(!d)return;
  uint32_t ranges[][2]={{0x0000,0x0400},{0x1000,0x1200},{0x8000,0x9400},{0xA000,0xA200},{0xC000,0xE400},{0x10000,0x1e000}};
  for(int r=0;r<6;r++) for(uint32_t a=ranges[r][0];a<ranges[r][1];a+=4){ uint32_t v=r32(a); fprintf(d,"0x%05x=0x%08x\n",a,v); }
  fclose(d);
}
static int g_pending=0;
static void LIBUSB_CALL burst_cb(struct libusb_transfer*x){ (void)x; g_pending--; }
// burst-submit n bulk-OUT buffers on EP7 asynchronously (like the kernel), wait for all completions.
static int burst_ep7(uint8_t**bufs,int*lens,int n){
  struct libusb_transfer**t=calloc(n,sizeof(void*));
  g_pending=n;
  for(int i=0;i<n;i++){ t[i]=libusb_alloc_transfer(0);
    libusb_fill_bulk_transfer(t[i],dev,0x07,bufs[i],lens[i],burst_cb,NULL,3000);
    if(libusb_submit_transfer(t[i])<0){ g_pending--; } }
  struct timeval tv={5,0}; long start=0;
  while(g_pending>0){ libusb_handle_events_timeout(NULL,&tv); if(++start>200000)break; }
  for(int i=0;i<n;i++) if(t[i]) libusb_free_transfer(t[i]);
  free(t);
  return 0;
}

static void w32(uint32_t a,uint32_t v){ uint8_t b[4]={v&0xff,(v>>8)&0xff,(v>>16)&0xff,(v>>24)&0xff}; libusb_control_transfer(dev,0x40,0x05,a&0xffff,(a>>16)&0xff,b,4,300); }
// --- IQK infrastructure (increment 3): masked RMW + RF-LSSI direct path ---
// The BB/PHY register page is at USB +0x10000 (kernel BB 0x8000 == USB 0x18000). MAC regs stay direct.
#define BB(a) (0x10000u + (a))                       // kernel BB addr -> USB addr
static uint32_t maskshift(uint32_t m){ uint32_t s=0; if(!m)return 0; while(!((m>>s)&1))s++; return s; }
static void w32m(uint32_t a,uint32_t mask,uint32_t val){ uint32_t o=r32(a); uint32_t s=maskshift(mask); w32(a,(o&~mask)|((val<<s)&mask)); }
static void bbset(uint32_t bba,uint32_t bits){ uint32_t a=BB(bba); w32(a,r32(a)|bits); }
static void bbclr(uint32_t bba,uint32_t bits){ uint32_t a=BB(bba); w32(a,r32(a)&~bits); }
static void bbm(uint32_t bba,uint32_t mask,uint32_t val){ w32m(BB(bba),mask,val); }   // BB masked write (kernel addr)
static uint32_t bbr(uint32_t bba,uint32_t mask){ uint32_t v=r32(BB(bba)); return (v&mask)>>maskshift(mask); }
// RF direct access (8852A base path): BB addr = rf_base[path] + (rf_addr<<2), 20-bit reg. rf_base A=0xc000 B=0xd000.
static const uint32_t RF_BASE[2]={0xc000u,0xd000u};
#define RFREG_MASK 0xfffffu
static void wrf(int path,uint32_t rfa,uint32_t mask,uint32_t val){ uint32_t a=BB(RF_BASE[path]+((rfa&0xff)<<2)); w32m(a,mask&RFREG_MASK,val); usleep(1); }
static uint32_t rrf(int path,uint32_t rfa,uint32_t mask){ uint32_t v=r32(BB(RF_BASE[path]+((rfa&0xff)<<2)))&RFREG_MASK; return (v&mask)>>maskshift(mask); }
// scan blob forward from 'from' to the first non-section op after the next fwdl section run
static long skip_to_after_fwdl(uint8_t*blob,long from,long sz){
  long p=from; int sawHdr=0,inSec=0;
  while(p<sz){ int k=blob[p];
    if(k==1){ if(inSec)return p; int ln=blob[p+7]|(blob[p+8]<<8); p+=9+ln; }
    else if(k==2){ int ep=blob[p+1],ln=blob[p+2]|(blob[p+3]<<8); if(ep==7&&ln==112)sawHdr=1; else if(ep==7&&sawHdr)inSec=1; p+=4+ln; }
    else if(k==3){ if(inSec)return p; p+=5; }
    else if(k==4){ if(inSec)return p; p+=9; }
    else return p;
  }
  return sawHdr ? p : -1;   // EOF w/o a fwdl header seen -> not a real cycle (signal caller to replay normally)
}
// hwburst-proven init + fwdl. Works from cold; for warm re-init, stops the running CPU first.
static int hwburst_fwdl(const char*fwpath){
  { uint32_t plat=r32(0x88); if(plat&0x2){ w32(0x88,plat&~0x2u); for(int c=0;c<50;c++)r32(0x88); } } // stop running fw CPU (warm)
  w32(0xf4,0x20012248);w32(0x40,0);w32(0x1c,0xf38000);
  w32(0x8380,3);
  w32(0x8400,0x60440000);w32(0x8404,0x40000);w32(0x8400,0x60440000);w32(0x8404,0x4840000);
  w32(0x8c08,0);w32(0x9008,0x402001);
  w32(0x8c40,0);w32(0x8c44,0xc4);w32(0x8c4c,0);w32(0x8c50,0);
  uint32_t q[]={0x9040,0,0x9044,0,0x9048,0x100010,0x904c,0x300030,0x9050,0,0x9054,0,0x9058,0,0x905c,0,0x9060,0,0x9064,0,0x9068,0};
  for(int i=0;i<22;i+=2)w32(q[i],q[i+1]);
  w32(0x8400,0x64c40000);
  w32(0x8a00,0);w32(0x8a04,0x200000);w32(0x8a00,0x400);w32(0x8a00,0x408);
  w32(0x88,0x54d);
  { uint32_t v=r32(0x1e0)&~0x7u; w32(0x1e0,v); }
  w32(0x8,0x20ac21);
  w32(0xc04,0x18003040);w32(0x40000,0);w32(0xc04,0x18003044);w32(0xc04,0x18003044);w32(0x40000,0x100);
  w32(0x88,0x54c);w32(0x88,0x54d);
  w32(0x1f4,0);w32(0x1f8,0);w32(0x160,0);w32(0x164,0);w32(0x168,0);w32(0x16c,0);
  w32(0x8,0x20ec21);w32(0x1e0,1);w32(0x88,0x54f);
  int armed=0; for(int c=0;c<400000;c++){ if(r32(0x1e0)&2){armed=1;break;} }
  if(!armed){ P("hwburst: H2C not armed 0x1e0=0x%x\n",r32(0x1e0)); return -1; }
  FILE*f=fopen(fwpath,"rb"); if(!f){P("no fw\n");return -1;} fseek(f,0,SEEK_END);long fsz=ftell(f);fseek(f,0,SEEK_SET);
  uint8_t*fw=malloc(fsz);fread(fw,1,fsz,f);fclose(f);
  #define LE(o) (fw[o]|(fw[o+1]<<8)|(fw[o+2]<<16)|((uint32_t)fw[o+3]<<24))
  int secNum=(LE(24)>>8)&0xff; int hdrLen=32+secNum*16; int pkt=8+hdrLen; int hplen=24+pkt;
  uint8_t*hp=calloc(1,hplen);
  hp[2]=0x0c; hp[8]=pkt&0xff; hp[9]=(pkt>>8)&0xff;      // txdesc dword0=0xc0000, dword2=pktlen
  hp[24]=0x0d; hp[28]=pkt&0xff; hp[29]=(pkt>>8)&0x3f;   // fwcmd hdr
  memcpy(hp+32,fw,hdrLen);
  hp[60]=2020&0xff; hp[61]=(2020>>8)&0xff;              // patch part_size -> 2020
  int tr; libusb_bulk_transfer(dev,7,hp,hplen,&tr,3000);
  int fr=0; for(int c=0;c<400000;c++){ if(r32(0x1e0)&4){fr=1;break;} }
  uint8_t*sb[512]; int sl[512]; int ns=0; long body=hdrLen;
  for(int i=0;i<secNum;i++){ uint32_t d1=LE(32+i*16+4); long sz=d1&0xffffff; if(d1&(1<<28))sz+=8; long pp=body,rem=sz;
    while(rem>0){ int ch=rem>2020?2020:rem; uint8_t*b=calloc(1,24+ch); b[2]=0x1c; b[8]=ch&0xff;b[9]=(ch>>8)&0xff; memcpy(b+24,fw+pp,ch); sb[ns]=b;sl[ns]=24+ch;ns++; pp+=ch;rem-=ch; if(ns>=512)break; }
    body+=sz; }
  burst_ep7(sb,sl,ns);
  int booted=0; for(int c=0;c<400000;c++){ if(((r32(0x1e0)>>5)&7)==7){booted=1;break;} }
  P("hwburst fwdl: armed=%d fwdlrdy=%d sections=%d STS%s 0x1e0=0x%x\n",armed,fr,ns,booted?"=7 BOOTED":"!=7",r32(0x1e0));
  for(int i=0;i<ns;i++)free(sb[i]); free(hp);free(fw);
  return booted?0:-1;
}

// minimal pcap writer (DLT_IEEE802_11 = 105) so captured frames open in Wireshark/tcpdump
static void pcw(FILE*f,uint32_t v){ uint8_t b[4]={v&0xff,(v>>8)&0xff,(v>>16)&0xff,(v>>24)&0xff}; fwrite(b,1,4,f); }
static FILE* pcap_open(const char*path){
  FILE*f=fopen(path,"wb"); if(!f)return NULL;
  pcw(f,0xa1b2c3d4); uint8_t v[4]={2,0,4,0}; fwrite(v,1,4,f); // magic + ver 2.4
  pcw(f,0); pcw(f,0); pcw(f,65535); pcw(f,127);              // thiszone, sigfigs, snaplen, DLT=RADIOTAP
  return f;
}
// prepend a radiotap header (channel freq + optional dBm signal) so Wireshark shows band/channel/RSSI
static void pcap_frame(FILE*f,int freq,int sig,int haveSig,const uint8_t*data,int len){
  if(!f)return;
  uint8_t rt[13]; int rtlen;
  rt[0]=0; rt[1]=0;                                          // it_version, it_pad
  if(haveSig){ rtlen=13; rt[4]=0x28; }                       // present: CHANNEL(bit3) | DBM_ANTSIGNAL(bit5)
  else       { rtlen=12; rt[4]=0x08; }                       // present: CHANNEL(bit3)
  rt[2]=rtlen; rt[3]=0; rt[5]=0; rt[6]=0; rt[7]=0;
  rt[8]=freq&0xff; rt[9]=(freq>>8)&0xff; rt[10]=0xc0; rt[11]=0x00; // freq + flags: 2.4GHz(0x80)|OFDM(0x40)
  if(haveSig) rt[12]=(uint8_t)(int8_t)sig;                   // dBm antenna signal
  struct timeval tv; gettimeofday(&tv,NULL);
  pcw(f,(uint32_t)tv.tv_sec); pcw(f,(uint32_t)tv.tv_usec); pcw(f,rtlen+len); pcw(f,rtlen+len);
  fwrite(rt,1,rtlen,f); fwrite(data,1,len,f);
}

// ===================== IQK chain (increment 4: live IQK up to LOK) =====================
// Ported verbatim from rtw8852a_rfk.c (2.4G, 20M, ch6). BB via bbm/bbset/bbclr (kernel addr, +0x10000 in USB);
// RF via wrf/rrf. Order = _doiqk per path: get_ch_info -> macbb_setting -> preset -> txclk -> 3x[res_table +
// txk_setting + lok(2x one_shot)]. Stops after LOK (txk_group_sel/rxk = next increment).
static int g_band=0, g_bw=0, g_syn=0;   // 2G, 20M, syn1to2

static void iqk_get_ch_info(int path){
  g_band=0; g_bw=0;
  uint32_t reg35c = bbr(0x35c, 0x00000c00);
  g_syn = (reg35c==0x01)?1:0;
  bbm(0x9fe4, (0x000fu<<(path*16)), g_band);
  bbm(0x9fe4, (0x00f0u<<(path*16)), g_bw);
  bbm(0x9fe4, (0xff00u<<(path*16)), 6);
  P("  [get_ch_info p%d] band=%d bw=%d syn1to2=%d (0x35c[11:10]=%u)\n", path, g_band, g_bw, g_syn, reg35c);
}
static const uint32_t IQK_MACBB[][3]={
  {0x20fc,0xffff0000,0x00000303},{0x5864,0x18000000,0x00000003},{0x7864,0x18000000,0x00000003},
  {0x12b8,0x40000000,0x00000001},{0x32b8,0x40000000,0x00000001},{0x030c,0xff000000,0x00000013},
  {0x032c,0xffff0000,0x00000001},{0x12b8,0x10000000,0x00000001},{0x58c8,0x01000000,0x00000001},
  {0x78c8,0x01000000,0x00000001},{0x5864,0xc0000000,0x00000003},{0x7864,0xc0000000,0x00000003},
  {0x2008,0x01ffffff,0x01ffffff},{0x0c1c,0x00000004,0x00000001},{0x0700,0x08000000,0x00000001},
  {0x0c70,0x000003ff,0x000003ff},{0x0c60,0x00000003,0x00000003},{0x0c6c,0x00000001,0x00000001},
  {0x58ac,0x08000000,0x00000001},{0x78ac,0x08000000,0x00000001},{0x0c3c,0x00000200,0x00000001},
  {0x2344,0x80000000,0x00000001},{0x4490,0x80000000,0x00000001},{0x12a0,0x00007000,0x00000007},
  {0x12a0,0x00008000,0x00000001},{0x12a0,0x00070000,0x00000003},{0x12a0,0x00080000,0x00000001},
  {0x32a0,0x00070000,0x00000003},{0x32a0,0x00080000,0x00000001},{0x0700,0x01000000,0x00000001},
  {0x0700,0x06000000,0x00000002},{0x20fc,0xffff0000,0x00003333},{0x58f0,0x00080000,0x00000000},
  {0x78f0,0x00080000,0x00000000},
};
static void iqk_macbb_setting(void){ for(unsigned i=0;i<sizeof(IQK_MACBB)/sizeof(IQK_MACBB[0]);i++) bbm(IQK_MACBB[i][0],IQK_MACBB[i][1],IQK_MACBB[i][2]); }
static void iqk_preset(int path){
  bbm(0x8104+(path<<8),0x1,0); bbm(0x8154+(path<<8),0x4,0); wrf(path,0x05,0x1,0x0);
  bbm(0x8008,0xffffffffu,0x00000080); bbm(0x8080,0xffffffffu,0x0); bbm(0x8088,0xffffffffu,0x81ff010a);
  bbm(0x80d0,0xffffffffu,0x00200000); bbm(0x8074,0xffffffffu,0x80000000); bbm(0x81dc+(path<<8),0xffffffffu,0x0);
}
static void iqk_txclk_setting(int path){ bbm(0x8120+(path<<8),0xffffffffu,0xce000a08); }
static void lok_res_table(int path,int ibias){
  wrf(path,0xef,RFREG_MASK,0x2); wrf(path,0x33,RFREG_MASK,(g_band==0)?0x0:0x1);
  wrf(path,0x3f,RFREG_MASK,ibias); wrf(path,0xef,RFREG_MASK,0x0);
}
static void iqk_txk_setting(int path){
  bbset(0x12b8+(path<<13),(1u<<30));
  bbm(0x030c,0xff000000,0x1f); usleep(1); bbm(0x030c,0xff000000,0x13);
  bbm(0x032c,0xffff0000,0x0001); usleep(1); bbm(0x032c,0xffff0000,0x0041); usleep(1);
  bbm(0x20fc,0xffff0000,0x0303); bbm(0x20fc,0xffff0000,0x0000);
  wrf(path,0x90,0x3,0x00); wrf(path,0xde,(0x7fu<<13),0x3f); wrf(path,0x51,(1u<<19),0x0);
  wrf(path,0x51,(1u<<11),0x1); wrf(path,0x52,(1u<<11),0x1); wrf(path,0x55,0x1,0x0);
  wrf(path,0xef,0x4,0x1); wrf(path,0xdf,0x4,0x0); wrf(path,0x33,0x3ff,0x000);
  wrf(path,0x09,RFREG_MASK,0x80200); wrf(path,0x08,RFREG_MASK,0x80200);
  wrf(path,0x00,RFREG_MASK,0x403e0|g_syn); usleep(1);
}
static void iqk_one_shot(int path,int fine){
  uint32_t rfc=(path==0)?0x5864:0x7864;
  bbset(rfc,0x20000000); bbm(0x802c,0xfff,0x009);
  uint32_t cmd=(fine?0x208:0x108)|(1u<<(4+path));
  bbm(0x8000,0xffffffffu,cmd+1); bbset(0x80b0,(1u<<28)); usleep(1);
  int done=0,i; for(i=0;i<120;i++){ if((r32(BB(0xbff8))&0xff)==0x55){done=1;break;} usleep(1); }
  bbclr(0x8010,0xff); uint32_t rpt=r32(BB(0x8008)); bbclr(rfc,0x20000000);
  P("    [one_shot p%d %s] done=%d(i=%d) rpt=0x%x\n", path, fine?"FINE":"COARSE", done, i, rpt);
}
static int iqk_lok(int path){
  uint32_t itqt=(g_band==0)?0x09:0x12;
  wrf(path,0x56,0xffff,(g_band==0)?0xe5e0:0xe4e0);
  bbset(0x8034,0x30); uint32_t rf0=rrf(path,0x00,RFREG_MASK); bbm(0x8020,0xfffff,rf0|g_syn);
  bbclr(0x8124+(path<<8),0xf00); bbm(0x8154+(path<<8),0x100,0x1); bbm(0x8154+(path<<8),0x8,0x1);
  bbm(0x8154+(path<<8),0x3,0x0); bbset(0x0c60,0x2); bbclr(0x8010,0xff);
  bbm(0x81cc+(path<<8),0xffffffffu,itqt); iqk_one_shot(path,0); usleep(10000);
  bbm(0x81cc+(path<<8),0xffffffffu,itqt); iqk_one_shot(path,1);
  uint32_t tmp=rrf(path,0x58,RFREG_MASK); uint32_t ci=(tmp>>15)&0x1f, cq=(tmp>>10)&0x1f;
  int fail=(ci<2||ci>0x1d||cq<2||cq>0x1d);
  P("    [lok p%d] TXMO=0x%x i=0x%x q=0x%x fail=%d\n", path, tmp, ci, cq, fail);
  return fail;
}
// TXK IQ-imbalance cal: ID_TXK one-shot (clears the rfc bit, TXT=0x025, cmd 0x808|(1<<(path+4)) for 2G-20M).
static uint32_t g_nb_txcfir[2]={0x40000000,0x40000000}, g_nb_rxcfir[2]={0x40000000,0x40000000};
static int txk_one_shot(int path){
  uint32_t rfc=(path==0)?0x5864:0x7864;
  bbclr(rfc,0x20000000); bbm(0x802c,0xfff,0x025);          // ID_TXK: clr rfc bit, B_IQK_DIF4_TXT=0x025
  uint32_t cmd=0x808|(1u<<(4+path));                       // 0x008 | (1<<(path+4)) | ((0x8+iqk_bw=0)<<8)
  bbm(0x8000,0xffffffffu,cmd+1); bbset(0x80b0,(1u<<28)); usleep(1);
  int done=0,i; for(i=0;i<400;i++){ if((r32(BB(0xbff8))&0xff)==0x55){done=1;break;} usleep(1); }
  bbclr(rfc,0x20000000);
  return done;
}
static void txk_group_sel(int path){
  static const uint32_t g_txgain[4]={0x60e8,0x60f0,0x61e8,0x61ed};
  static const uint32_t g_itqt[4]={0x09,0x12,0x12,0x12};
  static const uint32_t g_attsmxr[4]={0x0,0x1,0x1,0x1};
  for(int gp=0;gp<4;gp++){
    bbm(0x8148+(path<<8),0x1f,0x08);                       // R_RFGAIN_BND B_RFGAIN_BND=0x08 (2G)
    wrf(path,0x56,0xffff,g_txgain[gp]);                    // RR_GAINTX all
    wrf(path,0x51,(1u<<11),g_attsmxr[gp]);                 // RR_TXG1 ATT1
    wrf(path,0x52,(1u<<11),g_attsmxr[gp]);                 // RR_TXG2 ATT0
    bbm(0x81cc+(path<<8),0xffffffffu,g_itqt[gp]);          // R_KIP_IQP
    bbclr(0x8124+(path<<8),(0xfu<<8));                     // R_IQK_RES B_IQK_RES_TXCFIR
    bbset(0x8154+(path<<8),(1u<<8));                       // R_CFIR_LUT SEL
    bbset(0x8154+(path<<8),(1u<<3));                       // R_CFIR_LUT G3
    bbm(0x8154+(path<<8),0x3,gp);                          // R_CFIR_LUT GP=gp
    bbclr(0x8010,0xff);                                    // R_NCTL_N1 CIP
    int done=txk_one_shot(path);
    P("    [txk p%d gp%d] done=%d 0xF0=0x%x\n", path, gp, done, r32(0xf0));
  }
  bbm(0x8124+(path<<8),(0xfu<<8),0x5);                     // R_IQK_RES.TXCFIR=0x5 (wideband)
  g_nb_txcfir[path]=0x40000000;
}
// restore BB/RF out of cal mode into operating mode — WITHOUT this TX will not run.
static void iqk_restore(int path){
  bbm(0x8138+(path<<8),0xffffffffu,g_nb_txcfir[path]);     // R_TXIQC
  bbm(0x813c+(path<<8),0xffffffffu,g_nb_rxcfir[path]);     // R_RXIQC
  bbclr(0x8008,0xffffffffu);                               // R_NCTL_RPT
  bbclr(0x8074,0xffffffffu);                               // R_MDPK_RX_DCK
  bbm(0x8088,0xffffffffu,0x80000000);                      // R_KIP_SYSCFG
  bbclr(0x80d0,0xffffffffu);                               // R_KPATH_CFG
  bbclr(0x80e0,0x1);                                       // R_GAPK B_GAPK_ADR
  bbm(0x8120+(path<<8),0xffffffffu,0x10010000);            // R_CFIR_SYS
  bbclr(0x8140+(path<<8),(1u<<8));                         // R_KIP B_KIP_RFGAIN
  bbm(0x8150+(path<<8),0xffffffffu,0xe4e4e4e4);            // R_CFIR_MAP
  bbclr(0x8154+(path<<8),(1u<<8));                         // R_CFIR_LUT SEL
  bbclr(0x81cc+(path<<8),0x3f);                            // R_KIP_IQP B_KIP_IQP_IQSW
  bbm(0x81dc+(path<<8),0xffffffffu,0x00000002);            // R_LOAD_COEF
  wrf(path,0xef,(1u<<2),0x0);                              // RR_LUTWE LOK
  wrf(path,0xde,(0x7fu<<13),0x0);                          // RR_RCKD POW
  wrf(path,0xef,(1u<<2),0x0);
  wrf(path,0x00,(0xfu<<16),0x3);                           // RR_MOD = V_RX(0x3)
  wrf(path,0x5c,(1u<<19),0x0);                             // RR_TXRSV GAPK
  wrf(path,0x5e,(1u<<19),0x0);                             // RR_BIAS GAPK
  wrf(path,0x05,(1u<<0),0x1);                              // RR_RSV1 RST
}
static const uint32_t IQK_AFEBB_RESTORE[20][3]={
  {0x20fc,0xffff0000,0x00000303},{0x12b8,0x40000000,0x0},{0x32b8,0x40000000,0x0},
  {0x5864,0xc0000000,0x0},{0x7864,0xc0000000,0x0},{0x2008,0x01ffffff,0x0},
  {0x0c1c,0x00000004,0x0},{0x0700,0x08000000,0x0},{0x0c70,0x0000001f,0x00000003},
  {0x0c70,0x000003e0,0x00000003},{0x12a0,0x000ff000,0x0},{0x32a0,0x000ff000,0x0},
  {0x0700,0x07000000,0x0},{0x5864,0x20000000,0x0},{0x7864,0x20000000,0x0},
  {0x0c3c,0x00000200,0x0},{0x2320,0x00000001,0x0},{0x20fc,0xffff0000,0x0},
  {0x58c8,0x01000000,0x0},{0x78c8,0x01000000,0x0},
};
static void iqk_afebb_restore(void){ for(int i=0;i<20;i++) bbm(IQK_AFEBB_RESTORE[i][0],IQK_AFEBB_RESTORE[i][1],IQK_AFEBB_RESTORE[i][2]); }
static void iqk_chain(int path){
  iqk_get_ch_info(path); iqk_macbb_setting(); iqk_preset(path); iqk_txclk_setting(path);
  int ibias=1, lokfail=1;
  for(int t=0;t<3 && lokfail;t++){
    lok_res_table(path,ibias++); iqk_txk_setting(path);
    if(t==0){ wrf(path,0x56,0xffff,0xe5e0); uint32_t g=rrf(path,0x56,0xffff);
      P("    [gain-hold p%d] RR_GAINTX after txk_setting=0x%04x %s\n", path, g, g==0xe5e0?"HELD (RF LOK mode OK)":"NOT held"); }
    lokfail=iqk_lok(path);
    P("    [lok-try %d p%d] fail=%d chip0xF0=0x%x\n", t, path, lokfail, r32(0xf0));
  }
  txk_group_sel(path);          // TXK IQ cal (4 groups)
  iqk_restore(path);            // restore this path out of cal mode (per _doiqk)
}

// live TSSI (rtw8852a_tssi order): 26 static rfk tables auto-generated verbatim + tmeter (thermal=0xff) +
// enable. Runs AFTER live IQK to restore kernel cal order rx_dck->IQK->TSSI. Generated by genfn.awk.
#include "tssi_all.c"
#include "dpk_tbls.c"
#include "dpk_live.c"
#include "initcal.c"

// ---- software power-on table (rtw89 mac_pwron_nic_8852a, ported from native/cpp/rtw_pwron.c) ----
// Byte R/W RMW + poll, filtered by cut+intf. Run on a WARM chip it re-walks the platform power-up (0x0005/
// 0x0006 handshake + 0x0088 enable toggles), to test whether it substitutes for a physical cold replug.
static uint8_t r8(uint32_t a){ uint8_t b=0; libusb_control_transfer(dev,0xc0,0x05,a&0xffff,(a>>16)&0xff,&b,1,300); return b; }
static void w8(uint32_t a,uint8_t v){ libusb_control_transfer(dev,0x40,0x05,a&0xffff,(a>>16)&0xff,&v,1,300); }
struct pwr_cfg { uint16_t addr; uint8_t cut; uint8_t intf; uint8_t cmd; uint8_t msk; uint8_t val; };
// cmd: 0=WRITE 1=POLL 2=DELAY 3=END. cut: CAV1 CBV2 CCV4 ALL0xFF. intf: SDIO1 USB2=2 USB3=4 PCIE8 ALL0xF.
static const struct pwr_cfg PWRON_TBL[] = {
  {0x00C6,0x02,0x08,0,0x40,0x40},{0x1086,0xFF,0x01,0,0x01,0},{0x1086,0xFF,0x01,1,0x02,0x02},
  {0x0005,0xFF,0x0F,0,0x18,0},{0x0005,0xFF,0x0F,0,0x80,0},{0x0005,0xFF,0x0F,0,0x04,0},
  {0x0006,0xFF,0x0F,1,0x02,0x02},{0x0006,0xFF,0x0F,0,0x01,0x01},{0x0005,0xFF,0x0F,0,0x01,0x01},
  {0x0005,0xFF,0x0F,1,0x01,0},{0x106D,0x06,0x06,0,0x40,0},
  {0x0088,0xFF,0x0F,0,0x01,0x01},{0x0088,0xFF,0x0F,0,0x01,0},{0x0088,0xFF,0x0F,0,0x01,0x01},
  {0x0088,0xFF,0x0F,0,0x01,0},{0x0088,0xFF,0x0F,0,0x01,0x01},
  {0x0083,0xFF,0x0F,0,0x40,0},{0x0080,0xFF,0x0F,0,0x20,0x20},{0x0024,0xFF,0x0F,0,0x1F,0},
  {0x02A0,0xFF,0x0F,0,0x02,0x02},{0x02A2,0xFF,0x0F,0,0xE0,0},{0x0071,0xFF,0x08,0,0x10,0},
  {0x0010,0x01,0x08,0,0x04,0x04},{0x02A0,0x01,0x0F,0,0xC0,0},{0xFFFF,0xFF,0x0F,3,0,0},
};
static int pwr_seq_run(uint8_t cut,uint8_t intf){
  int wr=0,pl=0;
  for(const struct pwr_cfg*s=PWRON_TBL; s->cmd!=3; s++){
    if(!(s->intf & intf) || !(s->cut & cut)) continue;
    if(s->cmd==0){ uint8_t v=r8(s->addr); v=(v&~s->msk)|(s->val&s->msk); w8(s->addr,v); wr++; }
    else if(s->cmd==1){ int ok=0; for(int c=2000;c;c--){ if((r8(s->addr)&s->msk)==(s->val&s->msk)){ok=1;break;} usleep(1000);} pl++; if(!ok){ P("  PWRON poll FAIL @0x%x\n",s->addr); return -2; } }
    else if(s->cmd==2){ usleep(s->val==0? s->addr : s->addr*1000); }
  }
  P("  PWRON: %d writes %d polls\n",wr,pl); return 0;
}

// mac_pwroff_nic_8852a (rtw8852a.c rtw8852a_pwroff[]) — USB2 rows, the full MAC power-DOWN. A warm-dirty chip
// (0x1e0=0x23) that hwburst refuses is recovered by running this teardown first, then the power-on + fwdl again
// (what the kernel does on rebind). Same {addr,cut,intf,cmd,msk,val} encoding as PWRON_TBL.
static const struct pwr_cfg PWROFF_TBL[] = {
  {0x02F0,0xFF,0x0F,0,0xFF,0},{0x02F1,0xFF,0x0F,0,0xFF,0},
  {0x0006,0xFF,0x0F,0,0x01,0x01},{0x0002,0xFF,0x0F,0,0x03,0},{0x0082,0xFF,0x0F,0,0x03,0},
  {0x106D,0x06,0x02,0,0x40,0x40},{0x0005,0xFF,0x0F,0,0x02,0x02},{0x0005,0xFF,0x0F,1,0x02,0},
  {0x0007,0xFF,0x02,0,0x10,0},{0x0005,0x7C,0x02,0,0x18,0x08},{0xFFFF,0xFF,0x0F,3,0,0},
};
static int pwroff_run(uint8_t cut,uint8_t intf){
  int wr=0,pl=0;
  for(const struct pwr_cfg*s=PWROFF_TBL; s->cmd!=3; s++){
    if(!(s->intf & intf) || !(s->cut & cut)) continue;
    if(s->cmd==0){ uint8_t v=r8(s->addr); v=(v&~s->msk)|(s->val&s->msk); w8(s->addr,v); wr++; }
    else if(s->cmd==1){ int ok=0; for(int c=2000;c;c--){ if((r8(s->addr)&s->msk)==(s->val&s->msk)){ok=1;break;} usleep(1000);} pl++; if(!ok){ P("  PWROFF poll FAIL @0x%x\n",s->addr); return -2; } }
  }
  P("  PWROFF: %d writes %d polls\n",wr,pl); return 0;
}

// RXPROBE: a lightweight EP0x84 read that counts WIFI units + beacons, to see at which stage RX dies.
static void rx_probe(const char*label){
  int frames=0, wifi=0, beacons=0, empty=0, tcnt[16]={0};
  for(int k=0;k<60 && empty<25;k++){ uint8_t rb[16384]; int tr=0;
    int rc=libusb_bulk_transfer(dev,0x84,rb,sizeof rb,&tr,200);
    if(rc!=0||tr<4){ empty++; continue; }
    empty=0; frames++;
    int off=0,guard=0;
    while(off+16<=tr && guard++<64){
      uint32_t d0=rb[off]|(rb[off+1]<<8)|(rb[off+2]<<16)|((uint32_t)rb[off+3]<<24);
      int pktsize=d0&0x3fff, shift=(d0>>14)&3, rt=(d0>>24)&0xf, drvsize=(d0>>28)&7, rxdlen=((d0>>31)&1)?32:16;
      if(pktsize==0) break;
      tcnt[rt&0xf]++;
      int foff=off+rxdlen+drvsize*8+shift;
      if(rt==0 && pktsize>=24 && foff+24<=tr){ wifi++; uint16_t fc=rb[foff]|(rb[foff+1]<<8); if((fc&0xfc)==0x80) beacons++; }
      int unit=rxdlen+drvsize*8+shift+pktsize; unit=(unit+7)&~7; off+=unit;
    }
  }
  P("RXPROBE[%s]: bulk=%d wifi=%d beacons=%d types:", label, frames, wifi, beacons);
  for(int t=0;t<16;t++) if(tcnt[t]) P(" t%d=%d",t,tcnt[t]);
  P("\n");
}

// ── live channel retune (the farm ax56.js recipe, via bbm/wrf which already carry the +0x10000 BB page) ──
static uint8_t sco(int ch){ if(ch==1)return 109; if(ch<=6)return 108; if(ch<=10)return 107; if(ch<=14)return 106;
  if(ch==36||ch==38)return 51; if(ch<=58)return 50; if(ch<=64)return 49; if(ch==100||ch==102)return 48;
  if(ch<=126)return 47; if(ch<=151)return 46; if(ch<=177)return 45; return 0; }
static void do_setch(int ch){ int is2g=ch<=14;
  bbm(0x20fc,0xff000000,0xf); bbm(0x0704,0x2,0); usleep(40000);                 // bracket enter
  for(int p=0;p<2;p++){ uint32_t rf=rrf(p,0x18,RFREG_MASK); rf&=~0x303ffu; rf|=ch; if(ch>14) rf|=(1u<<16)|(1u<<8); wrf(p,0x18,RFREG_MASK,rf); }
  bbm(0x4644,0xc0000000,is2g?1:0); bbm(0x4718,0xc0000000,is2g?1:0);
  bbm(0x4974,0x7f,sco(ch)); bbm(0x4498,0x40000000,is2g?1:0);
  bbm(0x4974,0xc0000000,0); bbm(0x4978,0x3000,0); bbm(0x4978,0xf00,0);          // 20 MHz
  uint32_t adc[2]={0x12d0,0x32d0},wbadc[2]={0x12ec,0x32ec};
  for(int p=0;p<2;p++){ bbm(adc[p],0x6000,0); bbm(wbadc[p],0x30,2); uint32_t rf=rrf(p,0x18,RFREG_MASK); rf|=(1u<<11)|(1u<<10); wrf(p,0x18,RFREG_MASK,rf); }
  bbm(0x20fc,0xff000000,0); bbm(0x0704,0x2,1);                                  // bracket leave (lock)
}
// _rck port (rtw8852a_rfk.c) — RC calibration per path; the lightest RFK step, touches RR_MOD(RX) + the RF clock
// enable, the candidate for making the synthesiser re-lock to a freshly written RR_CFGCH.
static void do_rck(int path){
  uint32_t rf5=rrf(path,0x05,RFREG_MASK);          // save RR_RSV1
  wrf(path,0x05,0x1,0x0);                           // RR_RSV1_RST=0
  wrf(path,0x00,(0xfu<<16),0x3);                    // RR_MOD = V_RX
  wrf(path,0x1b,RFREG_MASK,0x00240);                // RR_RCKC trigger
  for(int i=0;i<20;i++){ if(rrf(path,0x1c,0x8)) break; usleep(2); }   // poll RF0x1c BIT3
  uint32_t ca=rrf(path,0x1b,0x7c00);                // RR_RCKC_CA GENMASK(14,10)
  wrf(path,0x1b,RFREG_MASK,ca);
  wrf(path,0x1d,(0x1fu<<9),0x4);                    // RR_RCKO_OFF = 4
  wrf(path,0xf0,0x2,0x1); wrf(path,0xf0,0x2,0x0);   // RR_RFC_CKEN toggle
  wrf(path,0x05,RFREG_MASK,rf5);                    // restore RR_RSV1
}
// read EP0x84 for ~ms, tally beacon DS-channels, return the dominant channel (-1 if none), set *nbc = beacon count.
static int rx_dom_ch(int ms,int*nbc){ int tally[200]={0},total=0; struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
  for(;;){ uint8_t rb[16384]; int tr=0; int rc=libusb_bulk_transfer(dev,0x84,rb,sizeof rb,&tr,60);
    if(rc==0 && tr>=4){ int off=0,guard=0;
      while(off+16<=tr && guard++<64){ uint32_t d0=rb[off]|(rb[off+1]<<8)|(rb[off+2]<<16)|((uint32_t)rb[off+3]<<24);
        int pktsize=d0&0x3fff, shift=(d0>>14)&3, rt=(d0>>24)&0xf, drvsize=(d0>>28)&7, rxdlen=((d0>>31)&1)?32:16;
        if(pktsize==0) break; int foff=off+rxdlen+drvsize*8+shift;
        if(rt==0 && pktsize>=36 && foff+pktsize<=tr){ uint16_t fc=rb[foff]|(rb[foff+1]<<8);
          if((fc&0xfc)==0x80){ int p=foff+36,end=foff+pktsize; while(p+2<=end){ int tag=rb[p],ln=rb[p+1];
            if(tag==3&&ln>=1){ int c=rb[p+2]; if(c>0&&c<200){tally[c]++;total++;} break;} p+=2+ln; } } }
        int unit=rxdlen+drvsize*8+shift+pktsize; unit=(unit+7)&~7; off+=unit; } }
    clock_gettime(CLOCK_MONOTONIC,&t1); if((t1.tv_sec-t0.tv_sec)*1000+(t1.tv_nsec-t0.tv_nsec)/1000000>=ms) break; }
  int dom=-1,best=0; for(int c=0;c<200;c++) if(tally[c]>best){best=tally[c];dom=c;} if(nbc)*nbc=total; return dom; }

// The captured-config replay loop, shared by main()'s box/termux path and the RF_LIB JNI bring-up: walk the
// blob from startByte applying control writes(1)/reads(3)/polls(4)/bulks(2), with the 0x2f0 re-fwdl hwburst
// substitution, the 0x1bff8 IQK/DPK done-poll, and the ep7 fwdl section burst. Aborts on wedge / 30 consec fails.
static int replay_ops(uint8_t*blob, long sz, long startByte){
  long p=startByte;
  #define G8() (blob[p++])
  #define G16() (p+=2, (blob[p-2]|(blob[p-1]<<8)))
  #define G32() (p+=4,(uint32_t)(blob[p-4]|(blob[p-3]<<8)|(blob[p-2]<<16)|((uint32_t)blob[p-1]<<24)))
  long nw=0,nr=0,np=0,nb=0,ptmo=0,fail=0,ri=0,consecFail=0,bursts=0;
  uint8_t rbuf[64];
  char lastW[64]="";
  uint8_t*bbufs[512]; int blens[512];
  while(p<sz){
    int kind=G8(); ri++;
    if(kind==1){ int brt=G8(),br=G8(),wv=G16(),wi=G16(),ln=G16();
      int addr=wv|(wi<<16);
      uint32_t v0 = ln>=4 ? (blob[p]|(blob[p+1]<<8)|(blob[p+2]<<16)|((uint32_t)blob[p+3]<<24)) : (ln>=1?blob[p]:0);
      if(addr==0x2f0 && v0==0){ // maybe re-fwdl cycle start: substitute proven hwburst_fwdl, skip captured init+fwdl
        long endp=skip_to_after_fwdl(blob,p+ln,sz);
        if(endp>=0){ // real cycle: a fwdl header follows
          int rc=hwburst_fwdl(g_fwpath); bursts++;
          P("  [re-fwdl cycle @op %ld] hwburst rc=%d, skip %ld->%ld 0x1e0=0x%x\n",ri,rc,p+ln,endp,r32(0x1e0));
          p=endp; consecFail=0; continue;
        }
        P("  [0x2f0=0 @op %ld] no fwdl follows -> replaying as normal write (keeps monitor/channel tail)\n",ri);
      }
      snprintf(lastW,sizeof lastW,"0x%x len%d",addr,ln);
      int rc=libusb_control_transfer(dev,brt,br,wv,wi,blob+p,ln,300); p+=ln;
      if(rc<0){ fail++; consecFail++; if(rc==LIBUSB_ERROR_NO_DEVICE){P("\n*** NO_DEVICE at op %ld write %s\n",ri,lastW);break;} } else consecFail=0;
      nw++;
    } else if(kind==3){ int wv=G16(),wi=G16();
      libusb_control_transfer(dev,0xC0,0x05,wv,wi,rbuf,4,300); nr++;
    } else if(kind==4){ int wv=G16(),wi=G16(); uint32_t val=G32(); int addr=wv|(wi<<16);
      // 0x1bff8 = the IQK/DPK one-shot DONE flag (byte0==0x55). Wait STRICTLY for it, longer, and log — this is
      // the TX-calibration completion we need to verify actually happens during replay.
      if(addr==0x1bff8){ int hit=0; uint32_t rr=0; for(int c=0;c<4000;c++){ rr=r32(addr); if((rr&0xff)==0x55){hit=1;break;} } if(!hit)ptmo++;
        static int n1bff8=0; if(n1bff8<50){ n1bff8++; P("  [1bff8 #%d] %s rr=0x%x (want byte0=0x55, capval=0x%x)\n",n1bff8,hit?"DONE":"TIMEOUT",rr,val); } np++; }
      else {
      // lenient: wait until all bits SET in the captured 'ready' value are set (or exact) — matches "wait for ready bit"
      int hit=0; for(int c=0;c<600;c++){ uint32_t rr=r32(addr); if(rr==val || (rr&val)==val){hit=1;break;} } if(!hit)ptmo++; np++; }
    } else if(kind==2){ int ep=G8(),ln=G16();
      if(ep==7 && ln!=112){ // fwdl SECTION run: collect all consecutive ep7 non-header bulks, BURST them
        int cnt=0; long q=p-4; // rewind to this bulk's kind byte
        // re-walk from q collecting consecutive kind2/ep7/len!=112
        long pp=q;
        while(pp<sz && blob[pp]==2){ int e=blob[pp+1]; int l=blob[pp+2]|(blob[pp+3]<<8); if(e!=7||l==112)break;
          bbufs[cnt]=blob+pp+4; blens[cnt]=l; cnt++; pp+=4+l; if(cnt>=512)break; }
        burst_ep7(bbufs,blens,cnt); bursts++; nb+=cnt; p=pp;
        // wait for fw boot (STS[7:5]==7) like the kernel's check_rdy, before continuing mac init
        int booted=0; for(int c=0;c<3000;c++){ if(((r32(0x1e0)>>5)&7)==7){booted=1;break;} }
        P("  [burst] %d sections @op%ld -> STS%s 0x1e0=0x%x\n",cnt,ri,booted?"=7 BOOTED":"!=7",r32(0x1e0));
        consecFail=0;
      } else if(ep==7 && ln==112){ // fwdl HEADER: poll H2C_PATH_RDY(bit1), send, poll FWDL_PATH_RDY(bit2)
        int a=0; for(int c=0;c<3000;c++){ if(r32(0x1e0)&2){a=1;break;} }
        int tr=0; int rc=libusb_bulk_transfer(dev,7,blob+p,ln,&tr,3000); p+=ln;
        int b=0; for(int c=0;c<3000;c++){ if(r32(0x1e0)&4){b=1;break;} }
        P("  [fwdl hdr] H2C_RDY=%d FWDL_RDY=%d rc=%d 0x1e0=0x%x\n",a,b,rc,r32(0x1e0));
        if(rc<0)fail++; else consecFail=0; nb++;
      } else { int tr=0; int rc=libusb_bulk_transfer(dev,ep,blob+p,ln,&tr,3000); p+=ln;
        if(rc<0){fail++;consecFail++; if(rc==LIBUSB_ERROR_NO_DEVICE){P("\n*** NO_DEVICE bulk op %ld\n",ri);break;}} else consecFail=0; nb++; }
    } else { P("bad kind %d\n",kind); break; }
    if(consecFail>=30){ P("\n*** 30 consec fails op %ld lastW=%s\n",ri,lastW); break; }
    if(ri%25==0){ uint32_t fv=r32(0xf0); if(fv==0||fv==0xffffffff||((fv>>12)&0xf)!=2){ P("\n*** WEDGE op %ld 0xF0=0x%x lastW=%s (w=%ld r=%ld p=%ld b=%ld burst=%ld ptmo=%ld)\n",ri,fv,lastW,nw,nr,np,nb,bursts,ptmo); break; } }
    if(ri%2000==0) P("  ..%ld (w=%ld r=%ld p=%ld b=%ld burst=%ld ptmo=%ld fail=%ld) 0x1e0=0x%x ce20=0x%x\n",ri,nw,nr,np,nb,bursts,ptmo,fail,r32(0x1e0),r32(0xce20));
  }
  P("REPLAY done: %ld ops (w=%ld r=%ld p=%ld b=%ld burst=%ld ptmo=%ld fail=%ld)\n",ri,nw,nr,np,nb,bursts,ptmo,fail);
  #undef G8
  #undef G16
  #undef G32
  return (fail || ptmo>50) ? -1 : 0;
}

#ifndef RF_LIB
int main(int argc,char**argv){
  // Env overrides argv so the same binary runs on the box (argv) and on Android via termux-usb (env + TXFD).
  const char*blobpath = getenv("BLOB") ? getenv("BLOB") : (argc>1?argv[1]:"/tmp/replay3.bin");
  long startByte = getenv("START") ? atol(getenv("START")) : (argc>2?atol(argv[2]):0);
  int hbMode = getenv("HB") ? 1 : (argc>3 && argv[3][0]=='h');
   if(getenv("TXFD")) g_txfd = atoi(getenv("TXFD"));
  if(getenv("FW")) g_fwpath = getenv("FW");
  LOG=fopen(getenv("LOG") ? getenv("LOG") : "/tmp/hwdriver.log","w");
  if(!LOG) LOG=stderr;
  // Android/no-root: libusb_init would try to enumerate USB via sysfs/udev and fail. Tell it not to discover
  // devices — we hand it the fd from termux-usb directly. (No effect on the box path, which has no TXFD.)
  if(g_txfd >= 0) libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
  if(libusb_init(NULL)<0){P("libusb init fail\n");return 1;}
  if(reopen()<0){P("open fail (txfd=%d)\n", g_txfd);return 1;}
  if(getenv("RESET")){ uint32_t before=r32(0x1e0); int rc=libusb_reset_device(dev); P("RESET: entry 0x1e0=0x%x, libusb_reset_device rc=%d (%s)\n", before, rc, libusb_error_name(rc)); return 0; }
  if(getenv("PWRON")){ uint32_t b=r32(0x1e0); int rc=pwr_seq_run(0x04,0x02); P("PWRON: rc=%d, 0x1e0 0x%x->0x%x 0x88=0x%x 0xF0=0x%x\n", rc, b, r32(0x1e0), r32(0x88), r32(0xf0)); if(getenv("PWRON_ONLY")) return 0; }
  P("entry 0x1e0=0x%x 0xF0=0x%x\n", r32(0x1e0), r32(0xf0));

  // DUMP_ONLY: read a broad register swath onto whatever state the chip is in (e.g. after a kernel bring-up
  // + modprobe -r), write to the named file. Diff against a userspace-bring-up dump to find TX-enable regs.
  if(getenv("DUMP_ONLY")){ dump_regs(getenv("DUMP_ONLY")); P("DUMP_ONLY -> %s (entry 0x1e0=0x%x)\n", getenv("DUMP_ONLY"), r32(0x1e0)); return 0; }

  // INJECT_ONLY: no bring-up at all — just bulk-OUT the TX packets on EP5 onto whatever state the chip is in.
  // Used to isolate: if the kernel left the chip TX-enabled and this radiates, our BRING-UP is the gap; if it
  // still does not, our SUBMISSION differs from the kernel's.
  if(getenv("INJECT_ONLY")){ char*injf=getenv("INJECT");
    if(injf){ FILE*jf=fopen(injf,"rb"); if(jf){ fseek(jf,0,SEEK_END); long js=ftell(jf); fseek(jf,0,SEEK_SET); uint8_t*jb=malloc(js); fread(jb,1,js,jf); fclose(jf);
      int reps=getenv("INJECT_REPS")?atoi(getenv("INJECT_REPS")):1; int okc=0,errc=0;
      P("INJECT_ONLY: EP5, %ld bytes, %d reps, entry 0x1e0=0x%x\n", js, reps, r32(0x1e0));
      for(int rep=0;rep<reps;rep++){ long q=0; while(q+2<=js){ int ln=jb[q]|(jb[q+1]<<8); q+=2; if(q+ln>js)break; int tr=0; int rc=libusb_bulk_transfer(dev,0x05,jb+q,ln,&tr,1000); if(rc==0)okc++; else{errc++; if(errc<=3)P("  rc=%d %s\n",rc,libusb_error_name(rc));} q+=ln; } usleep(4000); }
      P("INJECT_ONLY done: ok=%d err=%d 0x1e0=0x%x 0xF0=0x%x\n", okc, errc, r32(0x1e0), r32(0xf0));
    } }
    return 0;
  }

  // DPKMEASURE: run ONLY the DPK (setup+sync+agc) onto whatever analog state the chip is already in (e.g.
  // after a kernel bring-up + modprobe -r), NO userspace bring-up. Decides: does our DPK-sync measurement
  // give kernel-level corr (~234) on a kernel-established analog foundation? If yes -> our replay bring-up is
  // the culprit (live-init port will fix); if it stays ~141 -> our measurement/USB is the culprit.
  if(getenv("DPKMEASURE")){
    P("DPKMEASURE: DPK sync/agc on current chip state (NO bring-up), entry 0x1e0=0x%x 0xF0=0x%x\n", r32(0x1e0), r32(0xf0));
    dpk_live_run();
    return 0;
  }

  // RECOVER: rescue a warm-dirty chip (0x1e0=0x23, which hwburst refuses) without a physical replug — the full
  // MAC power-down then power-up, exactly what a kernel rebind does. Runs before fwdl so the cold path can proceed.
  if(getenv("RECOVER")){ uint32_t b=r32(0x1e0);
    P("RECOVER: entry 0x1e0=0x%x -> pwroff+pwron\n", b);
    pwroff_run(0x04,0x02); usleep(10000); pwr_seq_run(0x04,0x02);
    P("RECOVER done: 0x1e0=0x%x 0x88=0x%x 0xF0=0x%x\n", r32(0x1e0), r32(0x88), r32(0xf0)); }
  FILE*f=fopen(blobpath,"rb"); if(!f){P("no blob\n");return 1;}
  fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
  uint8_t*blob=malloc(sz); fread(blob,1,sz,f); fclose(f);
  if(hbMode){ P("running hwburst init+fwdl (cycle 1)...\n");
    int rc=hwburst_fwdl(g_fwpath);
    if(rc!=0){ P("*** FWDL DID NOT BOOT (STS!=7, 0x1e0=0x%x) -- chip dirty; COLD REPLUG needed. Aborting.\n",r32(0x1e0)); return 2; }
  }
  replay_ops(blob, sz, startByte);
  // WRFTEST: verify the RF-LSSI write path + masked-RMW helpers on real silicon after bring-up.
  if(getenv("WRFTEST")){
    P("WRFTEST: RF read-back after bring-up (chip 0xF0=0x%x)\n", r32(0xf0));
    for(int p=0;p<2;p++){
      uint32_t mod=rrf(p,0x00,RFREG_MASK), cfgch=rrf(p,0x18,RFREG_MASK), gaintx=rrf(p,0x56,0xffff);
      P("  path%d: RR_MOD(0x00)=0x%05x  RR_CFGCH(0x18)=0x%05x (ch low=%u)  RR_GAINTX(0x56)=0x%04x\n",
        p, mod, cfgch, cfgch&0xff, gaintx);
    }
    // masked write+readback: set RR_GAINTX low16 to the 2.4G LOK value 0xe5e0 (what _iqk_lok writes), read back
    uint32_t before=rrf(0,0x56,0xffff);
    wrf(0,0x56,0xffff,0xe5e0);
    uint32_t after=rrf(0,0x56,0xffff);
    P("  wrf RR_GAINTX: 0x%04x -> wrote 0xe5e0 -> 0x%04x  %s\n", before, after, after==0xe5e0?"OK (RF write path works)":"MISMATCH");
    // masked-RMW on a BB reg: R_IQK_CFG(0x8034) B_IQK_CFG_SET(bits5:4) set to 3, read back
    uint32_t b0=bbr(0x8034,0x30);
    bbm(0x8034,0x30,0x3);
    uint32_t b1=bbr(0x8034,0x30);
    P("  bbm R_IQK_CFG[5:4]: %u -> wrote 3 -> %u  %s\n", b0, b1, b1==3?"OK (BB RMW works)":"MISMATCH");
    // RF write on a host-controlled LUT scratch reg (RR_LUTWA=0x33) — not hardware-driven, so it must hold
    uint32_t l0=rrf(0,0x33,RFREG_MASK); wrf(0,0x33,RFREG_MASK,0x155); uint32_t l1=rrf(0,0x33,RFREG_MASK);
    P("  wrf RR_LUTWA(0x33): 0x%05x -> wrote 0x155 -> 0x%05x  %s  (chip 0xF0=0x%x)\n", l0, l1, (l1&0x1ff)==0x155?"OK (RF WRITE path works)":"still not held", r32(0xf0));
  }
  if(getenv("RXPROBE")) rx_probe("post-bringup");
  if(getenv("INITCAL")){ initcal(); }
  if(getenv("IQKCHAIN")){
    P("IQKCHAIN: live IQK->LOK after bring-up (chip 0xF0=0x%x, entry)\n", r32(0xf0));
    iqk_chain(0);
    iqk_chain(1);
    iqk_afebb_restore();        // AFE/BB restore (nondbcc, both paths) — final exit from cal mode
    P("IQKCHAIN done: chip 0xF0=0x%x 0x1e0=0x%x (healthy=%s)\n", r32(0xf0), r32(0x1e0), r32(0xf0)==0xc492537?"YES":"NO-WEDGED");
  }
  if(getenv("TSSILIVE")){
    P("TSSILIVE: live TSSI after IQK (chip 0xF0=0x%x)\n", r32(0xf0));
    tssi_live();
    P("TSSILIVE done: chip 0xF0=0x%x 0x1e0=0x%x (healthy=%s)\n", r32(0xf0), r32(0x1e0), r32(0xf0)==0xc492537?"YES":"NO-WEDGED");
  }
  if(getenv("DPKLIVE")){ dpk_live_run(); }
  if(getenv("RXPROBE")) rx_probe("post-cal");
  if(getenv("DUMP")){ dump_regs(getenv("DUMP")); P("DUMP -> %s\n", getenv("DUMP")); }
  // TXEN: after our bring-up, write a list of "0xADDR 0xVAL" lines (candidate TX-enable regs lifted from the
  // kernel-vs-userspace diff) to see which make injection radiate. Applied before INJECT.
  if(getenv("TXEN")){ FILE*tf=fopen(getenv("TXEN"),"r"); if(tf){ unsigned a,v; int nn=0;
    while(fscanf(tf,"%x %x",&a,&v)==2){ uint8_t b[4]={v&0xff,(v>>8)&0xff,(v>>16)&0xff,(v>>24)&0xff}; libusb_control_transfer(dev,0x40,0x05,a&0xffff,(a>>16)&0xff,b,4,300); nn++; }
    fclose(tf); P("TXEN: applied %d writes\n", nn); } }
  P("  0x1e0=0x%x RX_FLTR(0xce20)=0x%x 0xF0=0x%x 0x1c060=0x%x\n", r32(0x1e0), r32(0xce20), r32(0xf0), r32(0x1c060));
  // WARM-DELTA test: apply a captured `iw set channel` op stream onto the already-brought-up chip, exactly
  // as an instant channel hop would. No fwdl, no 0x2f0 substitution — this is the warm retune under test.
  if(argc>4){
    FILE*wf=fopen(argv[4],"rb");
    if(wf){ fseek(wf,0,SEEK_END); long wsz=ftell(wf); fseek(wf,0,SEEK_SET); uint8_t*wb=malloc(wsz); fread(wb,1,wsz,wf); fclose(wf);
      long q=0,ww=0,wr=0,wp=0,wtmo=0; uint8_t rb2[64];
      P("warm-delta: replaying %s (%ld bytes)\n", argv[4], wsz);
      while(q<wsz){ int k=wb[q++];
        if(k==1){ int brt=wb[q++],br=wb[q++],wv=wb[q]|(wb[q+1]<<8),wi=wb[q+2]|(wb[q+3]<<8),ln=wb[q+4]|(wb[q+5]<<8); q+=6; libusb_control_transfer(dev,brt,br,wv,wi,wb+q,ln,300); q+=ln; ww++; }
        else if(k==3){ int wv=wb[q]|(wb[q+1]<<8),wi=wb[q+2]|(wb[q+3]<<8); q+=4; libusb_control_transfer(dev,0xC0,0x05,wv,wi,rb2,4,300); wr++; }
        else if(k==4){ int wv=wb[q]|(wb[q+1]<<8),wi=wb[q+2]|(wb[q+3]<<8); uint32_t val=wb[q+4]|(wb[q+5]<<8)|(wb[q+6]<<16)|((uint32_t)wb[q+7]<<24); q+=8; int addr=wv|(wi<<16); int hit=0; for(int c=0;c<600;c++){ uint32_t rr=r32(addr); if(rr==val||(rr&val)==val){hit=1;break;} } if(!hit)wtmo++; wp++; }
        else if(k==2){ int ep=wb[q++]; int ln=wb[q]|(wb[q+1]<<8); q+=2; int tr; libusb_bulk_transfer(dev,ep,wb+q,ln,&tr,1000); q+=ln; }
        else break;
      }
      P("warm-delta done: %ld writes %ld reads %ld polls (%ld poll-timeouts) 0x1c060=0x%x 0x1e0=0x%x\n", ww,wr,wp,wtmo, r32(0x1c060), r32(0x1e0));
      free(wb);
    } else P("warm-delta: cannot open %s\n", argv[4]);
  }
  // DPKOFF: force DPK bypass before injecting. Hypothesis: our replay left DPK ENABLED with stale/wrong
  // predistortion coefficients, which mangle the TX signal to nothing. _dpk_onoff(off) writes R_DPD_CH0A
  // (0x81BC, USB +0x10000 = 0x181BC) byte3 = 0x6 (val=0) to disable. path A=0x181BC, path B=0x182BC.
  if(getenv("DPKOFF")){
    uint32_t regs[2]={0x181bc,0x182bc};
    for(int i=0;i<2;i++){ uint32_t v=r32(regs[i]); uint32_t nv=(v&0x00ffffff)|(0x06u<<24); w32(regs[i],nv); P("DPKOFF: 0x%x 0x%x -> 0x%x (read 0x%x)\n",regs[i],v,nv,r32(regs[i])); }
  }
  // INJECT test: bulk-OUT captured TX packets ([txdesc][802.11 frame]) on EP5, the endpoint aireplay-ng uses.
  // Proves the no-root userspace TX path. File format: [len:2LE][data] per packet.
  { char*injf=getenv("INJECT");
    if(injf){ FILE*jf=fopen(injf,"rb"); if(jf){ fseek(jf,0,SEEK_END); long js=ftell(jf); fseek(jf,0,SEEK_SET); uint8_t*jb=malloc(js); fread(jb,1,js,jf); fclose(jf);
      int reps=getenv("INJECT_REPS")?atoi(getenv("INJECT_REPS")):1; long qtot=0; int okc=0,errc=0;
      P("INJECT: bulk-OUT on EP5, %ld bytes, %d reps\n", js, reps);
      for(int rep=0;rep<reps;rep++){ long q=0; while(q+2<=js){ int ln=jb[q]|(jb[q+1]<<8); q+=2; if(q+ln>js)break; int tr=0; int rc=libusb_bulk_transfer(dev,0x05,jb+q,ln,&tr,1000); if(rc==0)okc++; else {errc++; if(errc<=3)P("  inject rc=%d (%s)\n",rc,libusb_error_name(rc));} q+=ln; qtot++; } }
      P("INJECT done: %ld packets, ok=%d err=%d 0x1e0=0x%x 0xF0=0x%x\n", qtot, okc, errc, r32(0x1e0), r32(0xf0));
      free(jb);
    } else P("INJECT: cannot open %s\n", injf); }
  }
  // quick scan of other bulk-IN EPs in case C2H rides a separate pipe
  P("probing other IN EPs ...\n");
  for(int e=0x83;e<=0x87;e++){ if(e==0x84)continue; uint8_t rb[4096]; int tr=0;
    int rc=libusb_bulk_transfer(dev,e,rb,sizeof rb,&tr,300);
    if(rc==0 && tr>0){ P("  EP0x%x: %dB ",e,tr); for(int j=0;j<32&&j<tr;j++)P("%02x ",rb[j]); P("\n"); }
    else P("  EP0x%x: rc=%d (empty/none)\n",e,rc);
  }
  // 0x84: walk AGGREGATED rxd units per transfer (rxd_short16/long32, frame off=rxdlen+drvsize*8+shift),
  // for WIFI(rt=0) units print 802.11 frame-control + addr3(BSSID). Dump first 6 raw transfers for offline check.
  // SETCH: one live retune off the ch6 bring-up, confirmed by RX + timed. SWEEP: a full band pass, timed.
  if(getenv("SETCH")){ int ch=atoi(getenv("SETCH"));
    struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a); do_setch(ch);
    if(getenv("RCK")){ do_rck(0); do_rck(1); P("  + RCK\n"); }     // synth re-lock candidate
    clock_gettime(CLOCK_MONOTONIC,&b);
    double setms=(b.tv_sec-a.tv_sec)*1000.0+(b.tv_nsec-a.tv_nsec)/1e6;
    uint32_t cfg=rrf(0,0x18,RFREG_MASK); int nbc=0,dom=rx_dom_ch(700,&nbc);
    int ok=(dom==ch)||(dom>0&&(dom<=14)==(ch<=14)&&abs(dom-ch)<=2);
    P("SETCH ch6->ch%d: RR_CFGCH=0x%05x(low=%u) retune=%.1fms  RX dom=ch%d beacons=%d  %s\n",
      ch,cfg,cfg&0xff,setms,dom,nbc, ok?"*** RETUNE CONFIRMED ***":"(not confirmed)"); return 0; }
  if(getenv("SWEEP")){ int chs[]={1,2,3,4,5,6,7,8,9,10,11,12,13,36,40,44,48,149,153,157,161,165};
    int n=getenv("SWEEP5")?22:13, dwell=getenv("DWELL")?atoi(getenv("DWELL")):200;
    P("SWEEP: %d channels, %dms dwell each\n",n,dwell);
    struct timespec a,b,c,d; double setsum=0; int hits=0; clock_gettime(CLOCK_MONOTONIC,&a);
    for(int i=0;i<n;i++){ int ch=chs[i]; clock_gettime(CLOCK_MONOTONIC,&c); do_setch(ch);
      if(getenv("RCK")){ do_rck(0); do_rck(1); } clock_gettime(CLOCK_MONOTONIC,&d);
      setsum+=(d.tv_sec-c.tv_sec)*1000.0+(d.tv_nsec-c.tv_nsec)/1e6;
      int nbc=0,dom=rx_dom_ch(dwell,&nbc); int ok=dom>0&&(dom<=14)==(ch<=14)&&abs(dom-ch)<=2; if(ok)hits++;
      P("  ch%-3d dom=ch%-3d beacons=%-3d %s\n",ch,dom,nbc,ok?"ok":dom<0?"quiet":"MISS"); }
    clock_gettime(CLOCK_MONOTONIC,&b); double tot=(b.tv_sec-a.tv_sec)*1000.0+(b.tv_nsec-a.tv_nsec)/1e6;
    P("SWEEP done: %d ch in %.0fms (%.1fms/ch: retune %.1fms + dwell %dms)  confirmed %d/%d\n",
      n,tot,tot/n,setsum/n,dwell,hits,n); return 0; }
  // SCAN: hop the plan (do_setch + RCK per channel) and dump every EP0x84 transfer as `R <hex>` on stdout, for a
  // Deno pass to parse into an airodump table. LOG (chip chatter) stays on the log file; only R-lines hit stdout.
  if(getenv("SCAN")){ int chs[]={1,2,3,4,5,6,7,8,9,10,11,12,13,36,40,44,48,149,153,157,161,165};
    int n=getenv("SCAN5")?22:13, dwell=getenv("DWELL")?atoi(getenv("DWELL")):500, loop=getenv("LOOP")?1:0;
    do { for(int i=0;i<n;i++){ int ch=chs[i]; do_setch(ch); do_rck(0); do_rck(1);
      printf("C %d\n",ch); fflush(stdout);                     // channel marker: R-lines until the next C are on `ch`
      struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
      for(;;){ uint8_t rb[16384]; int tr=0;
        if(libusb_bulk_transfer(dev,0x84,rb,sizeof rb,&tr,60)==0 && tr>=4){ fputs("R ",stdout); for(int j=0;j<tr;j++) printf("%02x",rb[j]); fputc('\n',stdout); fflush(stdout); }
        clock_gettime(CLOCK_MONOTONIC,&t1); if((t1.tv_sec-t0.tv_sec)*1000+(t1.tv_nsec-t0.tv_nsec)/1000000>=dwell) break; } } } while(loop);
    fflush(stdout); return 0;
  }
  P("reading RX EP 0x84 (aggregated rxd parse) ...\n");
  FILE*pc=pcap_open("/tmp/ax56.pcap"); int pcn=0;
  int curSig=0, haveSig=0, dumped2=0; // RSSI (dBm) from the most recent PPDU-status, applied to that PPDU's frames
  int frames=0, empty=0, wifi=0, beacons=0, tcnt[16]={0}, dumped=0;
  for(int k=0;k<300 && empty<40;k++){ uint8_t rb[16384]; int tr=0;
    int rc=libusb_bulk_transfer(dev,0x84,rb,sizeof rb,&tr,200);
    if(rc!=0 || tr<4){ empty++; continue; }
    empty=0; frames++;
    if(dumped<6){ P("  RAW %dB: ",tr); for(int j=0;j<80&&j<tr;j++)P("%02x ",rb[j]); P("\n"); dumped++; }
    int off=0, guard=0;
    while(off+16<=tr && guard++<64){
      uint32_t d0=rb[off]|(rb[off+1]<<8)|(rb[off+2]<<16)|((uint32_t)rb[off+3]<<24);
      int pktsize=d0&0x3fff, shift=(d0>>14)&3, rt=(d0>>24)&0xf, drvsize=(d0>>28)&7;
      int rxdlen=((d0>>31)&1)?32:16;
      if(pktsize==0) break;
      tcnt[rt]++;
      int foff=off+rxdlen+drvsize*8+shift;
      if(rt==1){ // PPDU-status: phy_sts_hdr sits AFTER the MAC-info block; then per-path RSSI. signal=(max(A,B)>>1)-110
        int po=foff; // rxinfo (MAC info) = packet payload start
        if(po+8<=tr){
          uint32_t iw0=rb[po]|(rb[po+1]<<8)|(rb[po+2]<<16)|((uint32_t)rb[po+3]<<24);
          uint32_t iw1=rb[po+4]|(rb[po+5]<<8)|(rb[po+6]<<16)|((uint32_t)rb[po+7]<<24);
          int usr=iw0&0xf, rxcnt=(iw0>>29)&1, plcp=((iw1>>16)&0xff)<<3;
          int hs=po+8+usr*4+((usr&1)?4:0)+(rxcnt?96:0)+plcp; // MAC_INFO_SIZE 8 + usr*USR_SIZE 4 (+align) + RX_CNT 96 + plcp
          if(hs+8<=tr){
            uint32_t hw0=rb[hs]|(rb[hs+1]<<8)|(rb[hs+2]<<16)|((uint32_t)rb[hs+3]<<24);
            uint32_t hw1=rb[hs+4]|(rb[hs+5]<<8)|(rb[hs+6]<<16)|((uint32_t)rb[hs+7]<<24);
            int valid=(hw0>>7)&1, rA=hw1&0xff, rB=(hw1>>8)&0xff, raw=rA>rB?rA:rB;
            if(valid && raw){ curSig=(raw>>1)-110; haveSig=1;
              if(dumped2<6){ dumped2++; P("  PPDU sig: usr=%d plcp=%d phy@+%d rssiA=%d rssiB=%d -> %d dBm\n",usr,plcp,hs-po,rA,rB,curSig); } }
          }
        }
      }
      if(rt==0 && pktsize>=24 && foff+24<=tr){
        wifi++;
        if(foff+pktsize<=tr){ pcap_frame(pc,2437,curSig,haveSig,rb+foff,pktsize); pcn++; } // frame + radiotap (ch6)
        uint16_t fc=rb[foff]|(rb[foff+1]<<8);
        const uint8_t*a3=rb+foff+16;
        int isbcn=((fc&0xfc)==0x80);
        if(isbcn)beacons++;
        if(wifi<=24) P("  WIFI fc=0x%04x %s a1=%02x:%02x:%02x:%02x:%02x:%02x a3=%02x:%02x:%02x:%02x:%02x:%02x len=%d\n",
          fc, isbcn?"BEACON":"      ",
          rb[foff+4],rb[foff+5],rb[foff+6],rb[foff+7],rb[foff+8],rb[foff+9],
          a3[0],a3[1],a3[2],a3[3],a3[4],a3[5], pktsize);
      }
      int unit=rxdlen+drvsize*8+shift+pktsize; unit=(unit+7)&~7;
      off+=unit;
    }
  }
  if(pc){ fclose(pc); P("wrote %d frames to /tmp/ax56.pcap\n",pcn); }
  P("bulk-INs=%d  rxd-units by type:",frames);
  for(int t=0;t<16;t++) if(tcnt[t]) P(" t%d=%d",t,tcnt[t]);
  P("\n  wifi-units=%d beacons=%d\n",wifi,beacons);
  if(beacons)     P(">>> REAL BEACONS sniffed no-root in userspace! <<<\n");
  else if(wifi)   P(">>> WIFI-type units but no clean beacon fc -- likely mistuned/misaligned <<<\n");
  else if(frames) P(">>> data flowing but no valid WIFI units parsed <<<\n");
  else            P(">>> no RX on 0x84 <<<\n");
  return 0;
}
#endif // RF_LIB
