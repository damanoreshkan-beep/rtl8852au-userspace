// live RCK + DACK (rtw8852a_rfk_init) — init-time cals we previously only replayed (stale per-chip DC).
#include "initcal_tbls.c"
static uint8_t  g_msbk[2][2][16];
static uint16_t g_biask[2][2];
static uint8_t  g_dadck[2][2];
static uint16_t g_addck[2][2];
static int bbpoll(uint32_t bba,uint32_t bit,int tries){ for(int c=0;c<tries;c++){ if(r32(BB(bba))&bit) return 1; usleep(1);} return 0; }
static void rck(int path){
  uint32_t rf5=rrf(path,0x05,RFREG_MASK);
  wrf(path,0x05,0x1,0x0); wrf(path,0x00,0xf0000,0x3); wrf(path,0x1b,RFREG_MASK,0x00240);
  int ok=0; for(int c=0;c<40;c++){ if(rrf(path,0x1c,0x8)){ok=1;break;} usleep(2);}
  uint32_t rckv=rrf(path,0x1b,0x7c00); wrf(path,0x1b,RFREG_MASK,rckv);
  wrf(path,0x1d,0x3e00,0x4); wrf(path,0xf0,0x2,0x1); wrf(path,0xf0,0x2,0x0);
  wrf(path,0x05,RFREG_MASK,rf5);
  P("[RCK]S%d ok=%d rckv=0x%x\n",path,ok,rckv);
}
static void addck_backup(void){
  bbclr(0x12D8,0x300); g_addck[0][0]=bbr(0x1E00,0xffc00); g_addck[0][1]=bbr(0x1E00,0x3ff);
  bbclr(0x32D8,0x300); g_addck[1][0]=bbr(0x3E00,0xffc00); g_addck[1][1]=bbr(0x3E00,0x3ff);
}
static void addck_reload(void){
  bbm(0x12D4,0x3ff0000,g_addck[0][0]); bbm(0x12D8,0xf,g_addck[0][1]>>6); bbm(0x12D4,0xfc000000,g_addck[0][1]&0x3f); bbset(0x12D8,0x30);
  bbm(0x32D4,0x3ff0000,g_addck[1][0]); bbm(0x32D8,0xf,g_addck[1][1]>>6); bbm(0x32D4,0xfc000000,g_addck[1][1]&0x3f); bbset(0x32D8,0x30);
}
static void addck(void){
  TBL_rfk_addck_reset_defs_a(); TBL_rfk_check_addc_defs_a(); TBL_rfk_addck_trigger_defs_a();
  int a=bbpoll(0x1e00,0x1,3000); TBL_rfk_check_addc_defs_a(); TBL_rfk_addck_restore_defs_a();
  TBL_rfk_addck_reset_defs_b(); TBL_rfk_check_addc_defs_b(); TBL_rfk_addck_trigger_defs_b();
  int b=bbpoll(0x3e00,0x1,3000); TBL_rfk_check_addc_defs_b(); TBL_rfk_addck_restore_defs_b();
  P("[ADDCK]s0=%d s1=%d\n",a,b);
}
static void dack_backup_s0(void){
  bbset(0x5E00,0x8); bbset(0x5E50,0x8); bbset(0x12B8,0x40000000);
  for(int i=0;i<16;i++){ bbm(0x5E00,0xf0000000,i); g_msbk[0][0][i]=bbr(0x5E44,0xff00); bbm(0x5E50,0xf0000000,i); g_msbk[0][1][i]=bbr(0x5E94,0xff00);}
  g_biask[0][0]=bbr(0x5E30,0x3ff000); g_biask[0][1]=bbr(0x5E80,0x3ff000);
  g_dadck[0][0]=(uint8_t)(bbr(0x5E48,0xff00)-8); g_dadck[0][1]=(uint8_t)(bbr(0x5E98,0xff00)-8);
}
static void dack_backup_s1(void){
  bbset(0x7E00,0x8); bbset(0x7E50,0x8); bbset(0x32B8,0x40000000);
  for(int i=0;i<16;i++){ bbm(0x7E00,0xf0000000,i); g_msbk[1][0][i]=bbr(0x7E44,0xff00); bbm(0x7E50,0xf0000000,i); g_msbk[1][1][i]=bbr(0x7E94,0xff00);}
  g_biask[1][0]=bbr(0x7E30,0x3ff000); g_biask[1][1]=bbr(0x7E80,0x3ff000);
  g_dadck[1][0]=(uint8_t)(bbr(0x7E48,0xff00)-8); g_dadck[1][1]=(uint8_t)(bbr(0x7E98,0xff00)-8);
}
static void dack_reload_by_path(int path,int idx){
  uint32_t off=(idx?0x50:0)+(path?0x2000:0),tmp;
  tmp=0; for(int i=0;i<4;i++) tmp|=g_msbk[path][idx][i+12]<<(i*8); w32(BB(0x5e14+off),tmp);
  tmp=0; for(int i=0;i<4;i++) tmp|=g_msbk[path][idx][i+8]<<(i*8);  w32(BB(0x5e18+off),tmp);
  tmp=0; for(int i=0;i<4;i++) tmp|=g_msbk[path][idx][i+4]<<(i*8);  w32(BB(0x5e1c+off),tmp);
  tmp=0; for(int i=0;i<4;i++) tmp|=g_msbk[path][idx][i]<<(i*8);    w32(BB(0x5e20+off),tmp);
  tmp=((uint32_t)g_biask[path][idx]<<22)|((uint32_t)g_dadck[path][idx]<<14); w32(BB(0x5e24+off),tmp);
}
static void dack_reload(int path){ dack_reload_by_path(path,0); dack_reload_by_path(path,1); if(path==0)TBL_rfk_dack_reload_defs_a(); else TBL_rfk_dack_reload_defs_b(); }
static void dack_s0(void){
  TBL_rfk_dack_defs_f_a(); int m0=bbpoll(0x5e28,0x8000,3000)&bbpoll(0x5e78,0x8000,3000);
  TBL_rfk_dack_defs_m_a(); int d0=bbpoll(0x5e48,0x20000,3000)&bbpoll(0x5e98,0x20000,3000);
  TBL_rfk_dack_defs_r_a(); dack_backup_s0(); dack_reload(0); bbclr(0x12B8,0x40000000);
  P("[DACK]S0 msbk=%d dad=%d m0=0x%x bias=0x%x dadck=0x%x\n",m0,d0,g_msbk[0][0][0],g_biask[0][0],g_dadck[0][0]);
}
static void dack_s1(void){
  TBL_rfk_dack_defs_f_b(); int m1=bbpoll(0x7e28,0x8000,3000)&bbpoll(0x7e78,0x8000,3000);
  TBL_rfk_dack_defs_m_b(); int d1=bbpoll(0x7e48,0x20000,3000)&bbpoll(0x7e98,0x20000,3000);
  TBL_rfk_dack_defs_r_b(); dack_backup_s1(); dack_reload(1); bbclr(0x32B8,0x40000000);
  P("[DACK]S1 msbk=%d dad=%d m0=0x%x bias=0x%x dadck=0x%x\n",m1,d1,g_msbk[1][0][0],g_biask[1][0],g_dadck[1][0]);
}
static void initcal(void){
  P("[INITCAL] live RCK+DACK\n");
  rck(0); rck(1);
  uint32_t rf0=rrf(0,0x00,RFREG_MASK),rf1=rrf(1,0x00,RFREG_MASK);
  TBL_rfk_afe_init_defs();
  wrf(0,0x05,0x1,0x0); wrf(1,0x05,0x1,0x0); wrf(0,0x00,RFREG_MASK,0x30001); wrf(1,0x00,RFREG_MASK,0x30001);
  addck(); addck_backup(); addck_reload();
  wrf(0,0x00,RFREG_MASK,0x40001); wrf(1,0x00,RFREG_MASK,0x40001); wrf(0,0x01,RFREG_MASK,0x0); wrf(1,0x01,RFREG_MASK,0x0);
  dack_s0(); dack_s1();
  wrf(0,0x00,RFREG_MASK,rf0); wrf(1,0x00,RFREG_MASK,rf1); wrf(0,0x05,0x1,0x1); wrf(1,0x05,0x1,0x1);
  P("[INITCAL] done addck=%d,%d/%d,%d 0xF0=0x%x\n",g_addck[0][0],g_addck[0][1],g_addck[1][0],g_addck[1][1],r32(0xf0));
}
