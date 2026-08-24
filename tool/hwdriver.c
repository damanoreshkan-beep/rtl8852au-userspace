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

static libusb_device_handle *dev;
static FILE *LOG;
#define P(...) do{ fprintf(LOG,__VA_ARGS__); fflush(LOG); }while(0)

static int reopen(void){
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

int main(int argc,char**argv){
  const char*blobpath = argc>1?argv[1]:"/tmp/replay3.bin";
  long startByte = argc>2?atol(argv[2]):0;
  int hbMode = argc>3 && argv[3][0]=='h';
  LOG=fopen("/tmp/hwdriver.log","w");
  if(libusb_init(NULL)<0){P("libusb init fail\n");return 1;}
  if(reopen()<0){P("open fail\n");return 1;}
  P("entry 0x1e0=0x%x 0xF0=0x%x\n", r32(0x1e0), r32(0xf0));

  FILE*f=fopen(blobpath,"rb"); if(!f){P("no blob\n");return 1;}
  fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
  uint8_t*blob=malloc(sz); fread(blob,1,sz,f); fclose(f);
  if(hbMode){ P("running hwburst init+fwdl (cycle 1)...\n");
    int rc=hwburst_fwdl("/tmp/fw_cut2_nic.bin");
    if(rc!=0){ P("*** FWDL DID NOT BOOT (STS!=7, 0x1e0=0x%x) -- chip dirty; COLD REPLUG needed. Aborting.\n",r32(0x1e0)); return 2; }
  }
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
          int rc=hwburst_fwdl("/tmp/fw_cut2_nic.bin"); bursts++;
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
      // lenient: wait until all bits SET in the captured 'ready' value are set (or exact) — matches "wait for ready bit"
      int hit=0; for(int c=0;c<600;c++){ uint32_t rr=r32(addr); if(rr==val || (rr&val)==val){hit=1;break;} } if(!hit)ptmo++; np++;
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
  P("  0x1e0=0x%x RX_FLTR(0xce20)=0x%x 0xF0=0x%x\n", r32(0x1e0), r32(0xce20), r32(0xf0));
  // quick scan of other bulk-IN EPs in case C2H rides a separate pipe
  P("probing other IN EPs ...\n");
  for(int e=0x83;e<=0x87;e++){ if(e==0x84)continue; uint8_t rb[4096]; int tr=0;
    int rc=libusb_bulk_transfer(dev,e,rb,sizeof rb,&tr,300);
    if(rc==0 && tr>0){ P("  EP0x%x: %dB ",e,tr); for(int j=0;j<32&&j<tr;j++)P("%02x ",rb[j]); P("\n"); }
    else P("  EP0x%x: rc=%d (empty/none)\n",e,rc);
  }
  // 0x84: walk AGGREGATED rxd units per transfer (rxd_short16/long32, frame off=rxdlen+drvsize*8+shift),
  // for WIFI(rt=0) units print 802.11 frame-control + addr3(BSSID). Dump first 6 raw transfers for offline check.
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
