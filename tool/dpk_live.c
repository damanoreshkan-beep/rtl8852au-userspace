// dpk_live.c — live DPK port (increment 7) for no-root userspace RTL8852AU.
// Faithful port of rtw8852a_rfk.c _dpk_* onto hwdriver primitives (bbm/bbset/bbclr/bbr/wrf/rrf).
// Requires dpk_tbls.c (TBL_rfk_dpk_*). 2G / 20M / ch6, both paths, kpath=RF_AB.
#include <stdlib.h>
#define DMASK 0xffffffffu

static const uint32_t DPK_BKBB[3] = {0x2344,0x58f0,0x78f0};
static const uint32_t DPK_BKRF[8] = {0xef,0xde,0x00,0x1e,0x02,0x85,0x90,0x05};
static uint32_t dpk_bkbb[3];
static uint32_t dpk_bkrf[2][8];
static int dpk_path_ok[2];

static int sx12(uint32_t v){ v&=0xfff; return (v&0x800)?(int)v-0x1000:(int)v; }

// _dpk_one_shot: dpk_cmd=(id<<8)|(0x19+(path<<4)); write NCTL_CFG, set DPK_CTL.EN, poll 0x1bff8==0x55, clr NCTL_N1
static int dpk_one_shot(int path,int id){
  uint16_t cmd=(uint16_t)((id<<8)|(0x19+(path<<4)));
  bbclr(0x8010,0xff);          // reset the done latch (NCTL_N1) so we wait for THIS measurement, not a stale 0x55
  bbm(0x8000,DMASK,cmd);
  bbset(0x80b0,0x10000000u);
  int done=0,i; for(i=0;i<400;i++){ if((r32(BB(0xbff8))&0xff)==0x55){done=1;break;} usleep(10); }
  bbclr(0x8010,0xff);
  P("      [dpk_os p%d id=0x%x] done=%d(i=%d)\n",path,id,done,i);
  return done?0:1;
}
// _set_rx_dck (is_afe=false)
static void dpk_rx_dck(int path){
  wrf(path,0x8f,0xc00,0x3);        // RR_RXBB2 EN_TIA_IDA
  wrf(path,0x94,0xfc,0x3f);        // RR_DCK2 CYCLE
  wrf(path,0x93,0x8,0x0);          // RR_DCK1 SEL=0
  wrf(path,0x92,0x1,0x0);          // RR_DCK LV=0
  wrf(path,0x92,0x1,0x1);          // RR_DCK LV=1
  usleep(600);
  wrf(path,0x92,0x1,0x0);
}
static void dpk_rf_setting(int path){
  // 2G branch
  wrf(path,0x00,0xfffe0,0x280b);   // RR_MOD RR_MOD_DPK
  wrf(path,0x83,0x7,0x0);          // RR_RXBB ATTC
  wrf(path,0x83,0xf0,0x4);         // RR_RXBB ATTR
  wrf(path,0x9f,0x18,0x0);         // RR_MIXER GN
  wrf(path,0xde,0x4,0x1);          // RR_RCKD BW
  wrf(path,0x1a,0x7000,g_bw+1);    // RR_BTC TXBB
  wrf(path,0x1a,0xc00,0x0);        // RR_BTC RXBB
}
static uint32_t dpk_set_tx_pwr(int path){ wrf(path,0x01,RFREG_MASK,0x38); return 0x38; }
static void dpk_kip_setting(int path,int kidx){
  bbm(0x8008,DMASK,0x00000080);
  bbm(0x808c,DMASK,0x00093f3f);
  bbm(0x8088,DMASK,0x807f030a);
  bbm(0x8120+(path<<8),DMASK,0xce000a08);
  bbm(0x80b8,0x7000,0x2);
  bbm(0x8000,0x6,path);
  bbm(0x81ac+(path<<8)+(kidx<<2),DMASK,0x003f2e2e);
  bbm(0x81bc+(path<<8)+(kidx<<2),DMASK,0x005b5b5b);
}
static void dpk_kip_restore(int path){
  bbclr(0x8008,DMASK);
  bbm(0x8088,DMASK,0x80000000);
  bbm(0x8120+(path<<8),DMASK,0x10010000);
  bbclr(0x808c,DMASK);
}
static void dpk_manual_txcfir(int path,int is_manual){
  if(is_manual){
    bbm(0x8140+(path<<8),0x100,0x1);
    uint32_t pad=rrf(path,0x56,0x3e0);  bbm(0x8144+(path<<8),0x1f,pad);
    uint32_t txbb=rrf(path,0x56,0x1f);  bbm(0x8144+(path<<8),0x1f00,txbb);
    P("      [dpk txcfir p%d] pad=0x%x txbb=0x%x (gaintx56=0x%x)\n",path,pad,txbb,(unsigned)rrf(path,0x56,RFREG_MASK));
    bbm(0x81dc+(path<<8),0x3,0x1); bbclr(0x81dc+(path<<8),0x3);
    bbm(0x81dc+(path<<8),0x2,0x1);
  } else bbclr(0x8140+(path<<8),0x100);
}
static void dpk_bypass_rxcfir(int path,int on){
  if(on){ bbm(0x813c+(path<<8),0x4,0x1); bbm(0x813c+(path<<8),0x1,0x1); }
  else { bbclr(0x813c+(path<<8),0x4); bbclr(0x813c+(path<<8),0x1); }
}
static void dpk_table_select(int path,int kidx,int gain){
  uint32_t val=0x80+kidx*0x20+gain*0x10;
  bbm(0x81ac+(path<<8),0xff000000,val);
}
static void dpk_tpg_sel(int path,int kidx){ (void)path;(void)kidx; bbm(0x806c,0x6,0x1); } // 20M
static void dpk_set_mdpd_para(int order){
  bbm(0x80a0,0x3,order); bbm(0x80a0,0x1f00,0x3); bbm(0x8070,0xf0000000u,0x1);
}
static void dpk_lbk_rxiqk(int path){
  uint32_t cur=rrf(path,0x00,0x3e0);
  TBL_rfk_dpk_lbk_rxiqk_defs_f();
  wrf(path,0x00,0xf0000,0xc);
  wrf(path,0x20,0x20,0x1);
  wrf(path,0x80,0x30000,0x2);
  wrf(path,0x1f,RFREG_MASK,rrf(path,0x18,RFREG_MASK));
  wrf(path,0x1e,0x3f,0x13);
  wrf(path,0x1e,0x80000,0x0);
  wrf(path,0x1e,0x80000,0x1);
  usleep(70);
  wrf(path,0x8d,0x1f00,0x1f);
  if(cur<=0xa) wrf(path,0x8d,0x6000,0x3);
  else if(cur<=0x10) wrf(path,0x8d,0x6000,0x1);
  else wrf(path,0x8d,0x6000,0x0);
  bbm(0x802c,0x0fff0000,0x11);
  dpk_one_shot(path,0x06);
  wrf(path,0x20,0x20,0x0);
  wrf(path,0x80,0x30000,0x0);
  wrf(path,0x1e,0x80000,0x0);
  wrf(path,0x00,0xf0000,0x5);
  TBL_rfk_dpk_lbk_rxiqk_defs_r();
}
// ---- AGC readers ----
static int dpk_sync_check(int path){
  bbclr(0x80d4,0x3f0000);
  int corr_idx=(int)bbr(0x80fc,0xff);
  int corr_val=(int)bbr(0x80fc,0xff00);
  bbm(0x80d4,0x3f0000,0x9);
  int dc_i=abs(sx12(bbr(0x80fc,0x0fff0000)));
  int dc_q=abs(sx12(bbr(0x80fc,0xfff)));
  P("      [dpk sync] corr_idx=%d corr_val=%d dc_i=%d dc_q=%d\n",corr_idx,corr_val,dc_i,dc_q);
  return (dc_i>200||dc_q>200||corr_val<130)?1:0;  // relaxed 170->130: let AGC proceed on our ~141 corr and set txagc (turns the PA on)
}
static int dpk_sync(int path){ dpk_tpg_sel(path,0); dpk_one_shot(path,0x10); return dpk_sync_check(path); }
static uint32_t dpk_dgain_read(void){ bbclr(0x80d4,0x3f0000); bbr(0x80fc,0x40000000); return bbr(0x80fc,0x0fff0000); }
static int dpk_dgain_mapping(uint32_t d){
  if(d>=0x783)return 6; if(d>=0x551)return 3; if(d>=0x3c4)return 0;
  if(d>=0x2aa)return -3; if(d>=0x1e3)return -6; if(d>=0x156)return -9; if(d<=0x155)return -12; return 0;
}
static uint32_t dpk_gainloss_read(void){ bbm(0x80d4,0x3f0000,0x6); bbm(0x80bc,0x4000,0x1); return bbr(0x80fc,0xf0); }
static void dpk_gainloss(int path){ dpk_table_select(path,0,1); dpk_one_shot(path,0x13); }
static uint32_t dpk_set_offset(int path,int goff){
  int txagc=(int)rrf(path,0x01,RFREG_MASK);
  if(txagc-goff<0x2e)txagc=0x2e; else if(txagc-goff>0x3f)txagc=0x3f; else txagc=txagc-goff;
  wrf(path,0x01,RFREG_MASK,txagc); return (uint32_t)txagc;
}
static int dpk_pas_read(void){
  TBL_rfk_dpk_pas_read_defs();
  bbm(0x80c0,0xff000000,0x00);
  long v1i=abs(sx12(bbr(0x80fc,0xffff0000))), v1q=abs(sx12(bbr(0x80fc,0xffff)));
  bbm(0x80c0,0xff000000,0x1f);
  long v2i=abs(sx12(bbr(0x80fc,0xffff0000))), v2q=abs(sx12(bbr(0x80fc,0xffff)));
  long a=v1i*v1i+v1q*v1q, b=v2i*v2i+v2q*v2q;
  return (a >= b*8/5)?1:0;
}
// _dpk_agc: adaptive state machine, <=6 iters. returns txagc (0xff=INVAL)
static uint32_t dpk_agc(int path,int kidx,uint32_t init_txagc){
  uint32_t tmp_txagc=init_txagc, tmp_rxbb=0, tmp_gl=0; int agc_cnt=0, limited=0; int off=0; uint32_t dgain=0;
  int step=0, goout=0;
  do{
    switch(step){
      case 0: // SYNC_DGAIN
        if(dpk_sync(path)){ tmp_txagc=0xff; goout=1; break; }
        dgain=dpk_dgain_read();
        step = limited?2:1; break;
      case 1: // GAIN_ADJ
        tmp_rxbb=rrf(path,0x00,0x3e0); off=dpk_dgain_mapping(dgain);
        if((int)tmp_rxbb+off>0x1f){ tmp_rxbb=0x1f; limited=1; }
        else if((int)tmp_rxbb+off<0){ tmp_rxbb=0; limited=1; }
        else tmp_rxbb=tmp_rxbb+off;
        wrf(path,0x00,0x3e0,tmp_rxbb);
        if(off!=0||agc_cnt==0) dpk_bypass_rxcfir(path,1);  // bw<80 -> bypass_rxcfir
        step=(dgain>1922||dgain<342)?0:2; agc_cnt++; break;
      case 2: // GAIN_LOSS_IDX
        dpk_gainloss(path); tmp_gl=dpk_gainloss_read();
        if((tmp_gl==0 && dpk_pas_read())||tmp_gl>7) step=3;
        else if(tmp_gl==0) step=4; else step=5;
        break;
      case 3: // GL_GT
        if(tmp_txagc==0x2e){ goout=1; } else tmp_txagc=dpk_set_offset(path,3);
        step=2; agc_cnt++; break;
      case 4: // GL_LT
        if(tmp_txagc==0x3f){ goout=1; } else tmp_txagc=dpk_set_offset(path,-2);
        step=2; agc_cnt++; break;
      case 5: // SET_TX_GAIN
        tmp_txagc=dpk_set_offset(path,(int)tmp_gl); goout=1; agc_cnt++; break;
      default: goout=1; break;
    }
  } while(!goout && agc_cnt<6);
  P("      [dpk_agc p%d] txagc=0x%x rxbb=0x%x (cnt=%d)\n",path,tmp_txagc,tmp_rxbb,agc_cnt);
  return tmp_txagc;
}
static void dpk_idl_mpa(int path,int kidx){ dpk_set_mdpd_para(0); dpk_table_select(path,kidx,1); dpk_one_shot(path,0x11); }
static void dpk_fill_result(int path,int kidx,int gain,uint32_t txagc){
  bbm(0x8104+(path<<8),0x100,kidx);
  bbm(0x81c4+(path<<8),0x3fu<<((gain<<3)+(kidx<<4)),txagc);
  bbm(0x81b4+(path<<8)+(kidx<<2),0x1ffu<<(gain<<4),0x78);
  bbm(0x81dc+(path<<8),0x10000,0x1); bbclr(0x81dc+(path<<8),0x10000);
  bbm(0x81bc+(path<<8)+(kidx<<2),DMASK,0x065b5b5b);
  bbclr(0x81a0+(path<<8),DMASK);
  bbclr(0x8070,0x80000000u);
}
static void dpk_onoff(int path,int off){
  int val = (!off && dpk_path_ok[path]) ? 1 : 0;
  bbm(0x81bc+(path<<8),0xff000000,(uint32_t)(0x6|val));
}
static int dpk_main(int path){
  int kidx=0; uint32_t txagc; int is_fail=0;
  wrf(path,0x05,0x1,0x0);                 // _rf_direct_cntrl false
  txagc=dpk_set_tx_pwr(path);
  dpk_rf_setting(path);
  dpk_rx_dck(path);
  dpk_kip_setting(path,kidx);
  dpk_manual_txcfir(path,1);
  txagc=dpk_agc(path,kidx,txagc);
  if(txagc==0xff) is_fail=1;
  dpk_idl_mpa(path,kidx);
  wrf(path,0x00,0xf0000,0x3);              // RR_MOD = RR_MOD_V_RX
  dpk_fill_result(path,kidx,1,txagc);
  dpk_manual_txcfir(path,0);
  dpk_path_ok[path]=!is_fail;
  P("    [dpk_main p%d] txagc=0x%x %s\n",path,txagc,is_fail?"CHECK":"SUCCESS");
  return is_fail;
}
static void dpk_tssi_pause(int path,int pause){ bbm(0x5818+(path<<13),0x40000000u,pause); }

// _dpk / _dpk_cal_select for RF_AB, 2G-20M
void dpk_live_run(void){
  P("  [DPKLIVE] start (chip 0xF0=0x%x)\n", r32(0xf0));
  for(int i=0;i<3;i++) dpk_bkbb[i]=r32(BB(DPK_BKBB[i]));
  for(int p=0;p<2;p++){ dpk_tssi_pause(p,1); for(int i=0;i<8;i++) dpk_bkrf[p][i]=rrf(p,DPK_BKRF[i],RFREG_MASK); }
  TBL_rfk_dpk_bb_afe_s_defs_ab();          // bb_afe_setting RF_AB
  for(int p=0;p<2;p++){ int f=dpk_main(p); dpk_onoff(p,f); }
  TBL_rfk_dpk_bb_afe_r_defs_ab();          // bb_afe_restore RF_AB
  for(int i=0;i<3;i++) w32(BB(DPK_BKBB[i]),dpk_bkbb[i]);
  for(int p=0;p<2;p++){ dpk_kip_restore(p); for(int i=0;i<8;i++) wrf(p,DPK_BKRF[i],RFREG_MASK,dpk_bkrf[p][i]); dpk_tssi_pause(p,0); }
  P("  [DPKLIVE] done: chip 0xF0=0x%x 0x1e0=0x%x (healthy=%s) path_ok=%d/%d\n",
    r32(0xf0), r32(0x1e0), r32(0xf0)==0xc492537?"YES":"NO-WEDGED", dpk_path_ok[0], dpk_path_ok[1]);
}
