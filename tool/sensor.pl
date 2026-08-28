#!/data/data/com.termux/files/usr/bin/perl
# AX56 SENSOR — touch TUI (production). Fully button-driven, no hotkeys: two-column layout, big TX/RX buttons on
# the right under the thumb, live STATIONS + LIVE frame lists on the left, and a row of low-level frame templates
# (probe / rts / null / qos-null). Drives the no-root RTL8852AU driver (ax56ch) over a FIFO.
#
# Architecture (clean responsibility split, event-driven, no polling/throttling of DATA):
#   * DRIVER (ax56ch, forked via sensor.sh->termux-usb) owns the radio + hop timing; streams C/R/P lines -> FIFO.
#   * TUI ingests every line the instant it arrives (select() blocks until an event; bursts are drained whole).
#   * RENDER is decoupled: a dirty flag + a 30fps frame cap paces only the *repaint* (vsync-like) so there is no
#     tearing/flicker, while the model is always up to date. Idle = fully blocked (0% CPU).
#   * SIGNALS: SIGWINCH -> resize+repaint, SIGCHLD -> launcher death (NODEV), SIGINT/TERM -> clean teardown.
# Control path: sensor.ctl = channel lock, sensor.ping = "<mac> <ch> <tmpl>" TX request. Tap a list row -> target.
use strict; use warnings;
use POSIX qw(strftime :sys_wait_h);
use Time::HiRes qw(time);
use IO::Select;
use Fcntl;

my $T   = "/root/ax56-ctl/tool";
my $FIFO= "$T/sensor.fifo";
my $CTL = "$T/sensor.ctl";
my $PING= "$T/sensor.ping";
my $STAT= "$T/sensor.status";
sub dbg { return unless $ENV{AX56_DEBUG}; open(my $f,">>","$T/sensor.debug.log") or return; print $f "@_\n"; close $f; }

# TX payload catalogue — standard injectable 802.11 frames for AUTHORIZED testing of your own network
# (the benign aireplay-ng/802.11 set: discovery / keepalive / control / link-setup). Picked in the TX mode.
my @PAYLOADS = (
  { id=>"probe",  name=>"Probe Request",  cat=>"discovery", desc=>"broadcast scan; every AP answers" },
  { id=>"dprobe", name=>"Directed Probe", cat=>"discovery", desc=>"probe one AP -> its response" },
  { id=>"null",   name=>"Null Data",      cat=>"keepalive", desc=>"power-save keepalive to target" },
  { id=>"qos",    name=>"QoS-Null",       cat=>"keepalive", desc=>"QoS power-save trigger" },
  { id=>"rts",    name=>"RTS",            cat=>"control",   desc=>"request-to-send; expects CTS" },
  { id=>"cts",    name=>"CTS-to-self",    cat=>"control",   desc=>"reserve airtime (NAV)" },
  { id=>"pspoll", name=>"PS-Poll",        cat=>"control",   desc=>"power-save poll; elicits buffered data" },
  { id=>"bar",    name=>"Block-Ack Req",  cat=>"control",   desc=>"BAR; expects a block-ack" },
  { id=>"auth",   name=>"Auth (open)",    cat=>"link",      desc=>"open-system auth to AP" },
  { id=>"assoc",  name=>"Assoc Request",  cat=>"link",      desc=>"association request to AP" },
  { id=>"reassoc",name=>"Reassoc Req",    cat=>"link",      desc=>"reassociation request to AP" },
  { id=>"action", name=>"Action",         cat=>"mgmt",      desc=>"public action frame" },
  { id=>"atim",   name=>"ATIM",           cat=>"mgmt",      desc=>"IBSS announcement traffic map" },
  { id=>"beacon", name=>"Beacon SSID",    cat=>"advertise", desc=>"advertise AX56-TEST; a nearby scan sees it = TX proven", warn=>1 },
);
sub pid   { $PAYLOADS[$_[0]]{id} }
sub pname { $PAYLOADS[$_[0]]{name} }

# ---- 802.11 frame-type names for the live RX list ----
my %FN = (
  "0.0"=>"assoc-req","0.1"=>"assoc-rsp","0.2"=>"reassoc-req","0.3"=>"reassoc-rsp",
  "0.4"=>"probe-req","0.5"=>"probe-rsp","0.8"=>"beacon","0.9"=>"atim","0.10"=>"disassoc",
  "0.11"=>"auth","0.12"=>"deauth","0.13"=>"action",
  "1.8"=>"bar","1.9"=>"ba","1.11"=>"rts","1.12"=>"cts","1.13"=>"ack","1.14"=>"cf-end",
  "2.0"=>"data","2.4"=>"null","2.8"=>"qos-data","2.12"=>"qos-null",
);

# ---- ASCII banner for the welcome screen (pure ASCII, renders in any Termux font) ----
my @BANNER = split /\n/, <<'ART';
    _    __  __ ____    __
   / \   \ \/ /| ___|  / /_
  / _ \   \  / |___ \ | '_ \
 / ___ \  /  \  ___) || (_) |
/_/   \_\/_/\_\|____/  \___/
ART

# ---- mode ----
my $MODE = $ARGV[0] // "";                          # "" live, "demo" synthetic, "selftest"/"render" headless
my $HEADLESS = ($MODE eq "selftest" || $MODE eq "render" || $MODE eq "replay");

# ---- terminal setup (skipped when headless) ----
my $saved_stty = "";
$| = 1;
binmode(STDOUT, ":encoding(UTF-8)");                # box-drawing glyphs; ASCII escapes pass through unchanged
unless ($HEADLESS) {
  open(STDERR, ">", "$T/sensor.err.log");            # keep warnings/errors OFF the screen (a TUI must never leak stderr)
  $saved_stty = `stty -g </dev/tty`; chomp $saved_stty;
  # -icanon -echo: char-at-a-time, no echo (immediate taps). KEEP isig so Ctrl-C always exits even if touch fails.
  system("stty -icanon -echo -ixon min 1 time 0 </dev/tty");
  print "\e[?1049h\e[?25l\e[?1000h\e[?1006h\e[2J"; # alt screen, hide cursor, SGR mouse on, clear
}

my ($ROWS,$COLS) = (24,80);
sub getsize { my $s = `stty size </dev/tty 2>/dev/null`; if ($s =~ /(\d+)\s+(\d+)/ && $1>=1 && $2>=1){ ($ROWS,$COLS)=($1,$2); }
              $ROWS=10 if $ROWS<10; $COLS=24 if $COLS<24; }   # never let a bad/zero size produce negative widths
getsize();

# ---- event-loop / signal state (declared before handlers that close over them) ----
my $dirty    = 1;           # repaint requested
my $got_chld = 0;           # SIGCHLD fired (launcher may have died)
my $kid;                    # launcher pid
my $demo     = 0;           # demo mode: synthetic activity, no adapter needed
my $demo_tx_at = 0;         # time a demo TX was fired (for the simulated reply)
my $demo_ch_at = 0;         # last demo channel-hop time
$SIG{WINCH} = sub { getsize(); $dirty=1; };

# ---- state ----
my @frames;                 # rolling live ring: {ts,ch,name,a2}
my %dev;                    # mac -> {vendor,ssid,ch,cnt,ap,bss,client,apof,clients{},last,ts}
my $target = "";            # target MAC
my $tch    = 0;             # target channel (0 = current)
my $ti     = 0;             # selected template index
my $cur_ch = 0;             # current channel from C markers
my $lock   = 0;             # 0 = hop all channels; else locked channel
my $lastp  = "";            # last TX result line
my $txstate= "";            # "firing" while a ping is in flight
my $status = "starting";
my $started= 0;             # got first C line
my @MODES  = qw(aps clients graph tx);   # left-column views + the TX payload picker
my $mode   = "aps";
my $confirm = -1;           # payload index awaiting a confirm tap (warn payloads); -1 = none
my @btn;                    # hit rects: [x1,y1,x2,y2,code]
my @rxrow;                  # visible list rows -> [row, mac, ch] for tap-to-target

# ---- capabilities (graceful degradation: NO_COLOR / non-UTF-8 terminals) ----
my $USE_COLOR = !$ENV{NO_COLOR};
my $UTF = (($ENV{LANG}//"").($ENV{LC_ALL}//"")) =~ /utf/i ? 1 : 1;   # Termux is UTF-8; default on

# ---- OUI vendor resolution (nmap prefix table; loaded once, lazily) ----
my %OUI; my $OUI_LOADED=0;
sub load_oui {
  return if $OUI_LOADED; $OUI_LOADED=1;
  open(my $fh,"<","/usr/share/nmap/nmap-mac-prefixes") or return;
  while (my $l=<$fh>) { next if length($l)<8 || substr($l,0,1) eq "#" || substr($l,6,1) ne " ";
    my $v=substr($l,7); $v=~s/\s+$//; $OUI{uc substr($l,0,6)}=$v; }
  close $fh;
}
sub clean_vendor {                                    # trim corporate noise for a premium, compact name
  my $v=shift; return $v unless $v;
  $v =~ s/[,].*$//;
  $v =~ s/\b(Technologies|Technology|Electronics|Electronic|Communications?|Systems?|Corporation|Corp|Company|Co|Inc|Ltd|Limited|GmbH|International|Networks?|Solutions?|Foundation|Devices?)\b//gi;
  $v =~ s/[.\s]+$//; $v =~ s/^\s+//; $v =~ s/\s{2,}/ /g;
  return $v;
}
sub vendor_of {                                       # real maker, "randomized" (locally-administered), or "?"
  my $m=shift; return "?" unless $m && $m=~/^[0-9a-f:]{17}$/i;
  return "randomized" if (hex(substr($m,0,2)) & 0x02);
  load_oui(); my $p=uc($m); $p=~s/://g; my $v=$OUI{substr($p,0,6)};
  return $v ? clean_vendor($v) : "?";
}
sub is_ap     { my $d=$dev{$_[0]}; $d && ($d->{ap}||$d->{bss}); }
sub is_client { my $d=$dev{$_[0]}; $d && !($d->{ap}||$d->{bss}) && ($d->{client}||$d->{apof}); }
# friendly display: SSID/vendor when known (MAC hidden), "randomized" + tail for private MACs, full MAC otherwise
sub disp {
  my $m=shift; my $d=$dev{$m}||{}; my $v=$d->{vendor}||vendor_of($m);
  return $d->{ssid} if defined $d->{ssid} && $d->{ssid} ne "";
  return $v if $v ne "?" && $v ne "randomized";
  return "random ".substr($m,9) if $v eq "randomized";
  return $m;
}

# ---- helpers ----
sub write_ctl { my $c = shift; open(my $f,">",$CTL) or return; print $f "$c 0\n"; close $f; }
sub clear_ping{ open(my $f,">",$PING) or return; print $f "-\n"; close $f; }
sub fire_tx {
  return unless $target =~ /^[0-9a-fA-F:]{17}$/;
  # warn payloads (active/advertising) require an explicit second tap to confirm
  if ($PAYLOADS[$ti]{warn} && $confirm != $ti) { $confirm=$ti; $txstate=""; $lastp=""; $dirty=1; return; }
  $confirm=-1;
  my $ch = $tch>0 ? $tch : ($cur_ch>0?$cur_ch:6);
  $txstate = "firing ".pid($ti)." -> $target ch$ch"; $lastp = ""; $dirty=1;
  if ($demo) { $demo_tx_at = time; return; }                  # demo: reply is synthesized in demo_tick
  open(my $f,">",$PING) or return; print $f "$target $ch ".pid($ti)."\n"; close $f;
}

# ---- device model helpers ----
sub bmac { join(":", map { sprintf "%02x",ord } split //, substr($_[0],$_[1],6)); }   # 6 raw bytes -> mac
sub is_bcast { my $m=shift; $m eq "ff:ff:ff:ff:ff:ff" || $m eq "00:00:00:00:00:00"; }
sub touch_dev {                                        # create/update a device record, resolve vendor once
  my ($m,$name,$ch)=@_; return undef if !$m || is_bcast($m);
  my $d=($dev{$m}||={cnt=>0,clients=>{}}); $d->{cnt}++; $d->{ch}=$ch; $d->{last}=$name; $d->{ts}=time;
  $d->{vendor}=vendor_of($m) unless defined $d->{vendor};
  return $d;
}
sub link_assoc {                                       # client <-> AP(bssid); preserves the AP's real channel
  my ($cl,$ap)=@_; return if !$cl||!$ap||is_bcast($cl)||is_bcast($ap)||$cl eq $ap;
  my $a=($dev{$ap}||={cnt=>0,clients=>{}}); $a->{bss}=1; $a->{clients}{$cl}=($a->{clients}{$cl}||0)+1;
  $a->{vendor}//=vendor_of($ap);
  my $c=touch_dev($cl,"data",$cur_ch); $c->{client}=1; $c->{apof}=$ap;
}
sub beacon_ssid {                                      # SSID IE from a beacon/probe-resp body
  my ($raw,$foff)=@_; my $p=$foff+36; my $L=length $raw; return undef if $p+2>$L;
  my $tag=ord(substr($raw,$p,1)); my $len=ord(substr($raw,$p+1,1));
  return undef unless $tag==0 && $len>0 && $p+2+$len<=$L;
  my $s=substr($raw,$p+2,$len); $s=~s/[^\x20-\x7e]//g; return $s;
}

# ---- RX decode: one `R <hex>` transfer -> device model + live ring (mirrors the C rxd walk) ----
sub decode_R {
  my $raw = pack("H*", shift);
  my $tr = length $raw; my $off = 0; my $guard=0;
  while ($off + 16 <= $tr && $guard++ < 128) {
    my $d0 = unpack("V", substr($raw,$off,4));
    my $pktsize = $d0 & 0x3fff;
    last if $pktsize == 0;
    my $shift = ($d0>>14)&3; my $rt=($d0>>24)&0xf; my $drv=($d0>>28)&7;
    my $rxdlen = ($d0>>31)&1 ? 32 : 16;
    my $foff = $off + $rxdlen + $drv*8 + $shift;
    if ($rt==0 && $pktsize>=10 && $foff+16<=$tr) {
      my $fc  = ord(substr($raw,$foff,1)) | (ord(substr($raw,$foff+1,1))<<8);
      my $typ=($fc>>2)&3; my $sub=($fc>>4)&0xf; my $tods=($fc>>8)&1; my $fromds=($fc>>9)&1;
      my $a1 = bmac($raw,$foff+4);
      my $a2 = ($foff+16<=$tr) ? bmac($raw,$foff+10) : "";
      my $a3 = ($foff+22<=$tr) ? bmac($raw,$foff+16) : "";
      my $name = $FN{"$typ.$sub"} || sprintf("t%d/s%d",$typ,$sub);
      push @frames, { ts=>time, ch=>$cur_ch, name=>$name, a2=>$a2 };
      my $src = touch_dev($a2,$name,$cur_ch);
      if ($typ==0 && ($sub==8 || $sub==5)) {           # beacon / probe-resp -> access point (+ SSID)
        if ($src){ $src->{ap}=1; my $ss=beacon_ssid($raw,$foff); $src->{ssid}=$ss if defined $ss && $ss ne ""; }
      } elsif ($typ==0 && ($sub==4 || $sub==0 || $sub==11)) {   # probe-req/assoc/auth -> client
        $src->{client}=1 if $src;
      } elsif ($typ==2) {                              # data -> client<->AP association via to/fromDS
        if    ($tods && !$fromds) { link_assoc($a2,$a1); }
        elsif ($fromds && !$tods) { link_assoc($a1,$a2); }
        elsif (!$tods && !$fromds && $a3 && !is_bcast($a3)) { link_assoc($a2,$a3); }
      }
    }
    my $unit = $rxdlen + $drv*8 + $shift + $pktsize; $unit = ($unit+7) & ~7; $off += $unit;
  }
  shift @frames while @frames > 200;
}

sub handle_line {
  my $ln = shift;
  if    ($ln =~ /^C (\d+)/)      { my $w=$started; $cur_ch=$1; $started=1; dbg("C ch=$1 started $w->1") unless $w; }
  elsif ($ln =~ /^R ([0-9a-fA-F]+)/) { decode_R($1); }
  elsif ($ln =~ /^P (\S+) (\d+) (.*)/) {
    my ($rep,$rest)=($2,$3);
    my ($seen)=$rest=~/seen=(\d+)/; my ($tx)=$rest=~/txok=(\d+)/; my ($tm)=$rest=~/tmpl=(\S+)/;
    $lastp = sprintf("%s reply=%d seen=%s tx=%s", $tm||"?", $rep, $seen//"?", $tx//"?");
    $txstate="";
  }
}

# ---- demo mode: synthetic APs, clients, associations & vendors so every mode works with no adapter ----
my @DEMO_AP = (
  { mac=>"50:c7:bf:11:22:33", ssid=>"HomeWiFi",  ch=>6  },
  { mac=>"24:a4:3c:44:55:66", ssid=>"Office-5G", ch=>36 },
  { mac=>"00:1a:2f:77:88:99", ssid=>"Guest",     ch=>1  },
);
my @DEMO_CL = (
  { mac=>"b8:27:eb:0a:1b:2c", ap=>"50:c7:bf:11:22:33", ch=>6,  kinds=>["qos-data","null"]    },
  { mac=>"a4:83:e7:aa:bb:cc", ap=>"50:c7:bf:11:22:33", ch=>6,  kinds=>["qos-data"]           },
  { mac=>"34:14:5f:de:ad:01", ap=>"24:a4:3c:44:55:66", ch=>36, kinds=>["qos-data","null"]    },
  { mac=>"6e:a3:41:d9:0f:5b", ap=>"",                  ch=>11, kinds=>["probe-req","null"]   },   # randomized, roaming
  { mac=>"da:a1:19:77:44:e1", ap=>"00:1a:2f:77:88:99", ch=>1,  kinds=>["probe-req","qos-data"] }, # randomized, on Guest
);
sub demo_seed {                                                # build the device model (roles, ssids, links)
  for my $a (@DEMO_AP){ my $d=touch_dev($a->{mac},"beacon",$a->{ch}); $d->{ap}=1; $d->{ssid}=$a->{ssid}; $d->{ch}=$a->{ch}; }
  for my $c (@DEMO_CL){ my $d=touch_dev($c->{mac},"data",$c->{ch}); $d->{client}=1; link_assoc($c->{mac},$c->{ap}) if $c->{ap}; }
}
sub demo_emit { my($mac,$name,$ch)=@_; touch_dev($mac,$name,$ch); push @frames,{ts=>time,ch=>$ch,name=>$name,a2=>$mac}; shift @frames while @frames>200; }
sub demo_tick {
  my $now=time;
  if ($lock) { $cur_ch=$lock; }
  elsif ($now-$demo_ch_at > 0.4) { my @plan=(1,6,11,36); $cur_ch=$plan[int($now/0.4)%@plan]; $demo_ch_at=$now; }
  for my $a (@DEMO_AP){ next unless $a->{ch}==$cur_ch; demo_emit($a->{mac},"beacon",$cur_ch) if rand()<0.2; }
  for my $c (@DEMO_CL){ next unless $c->{ch}==$cur_ch; demo_emit($c->{mac},$c->{kinds}[int(rand @{$c->{kinds}})],$cur_ch) if rand()<0.1; }
  if ($txstate ne "" && $demo_tx_at && $now-$demo_tx_at > 0.6) {
    my $t=pid($ti);
    if ($t eq "probe" || $t eq "dprobe") { demo_emit($target,"probe-rsp",$cur_ch); $lastp="$t reply=2 seen=9 tx=40"; }
    else               { $lastp="$t reply=0 seen=7 tx=40"; }
    $txstate=""; $demo_tx_at=0;
  }
  $dirty=1;
}
sub enter_demo {
  $demo=1; $started=1; $status="DEMO";
  @frames=(); %dev=(); $cur_ch=6; $demo_ch_at=time; demo_seed();
  system(q{for p in $(ps -eo pid,args 2>/dev/null | grep -E "[a]x56ch|[s]ensor_cb.sh" | awk '{print $1}'); do kill "$p" 2>/dev/null; done});
  $kid=0; $dirty=1;
}

# ---- drawing: palette (semantic colors) + box-drawing glyphs (graceful ASCII fallback) ----
my %C = $USE_COLOR ? (
  acc=>"38;5;44", accb=>"38;5;44;1", hdr=>"48;5;23;38;5;231;1", tabon=>"48;5;44;38;5;16;1", taboff=>"38;5;66",
  dim=>"38;5;244", warn=>"38;5;214", ok=>"38;5;42", danger=>"48;5;160;38;5;231;1",
  ap=>"38;5;44", cl=>"38;5;180", rnd=>"38;5;140", hot=>"48;5;236;38;5;227;1",
  txarm=>"48;5;160;38;5;231;1", txfire=>"48;5;214;38;5;16;1", txoff=>"38;5;240", rx=>"48;5;24;38;5;231;1", rxlock=>"48;5;28;38;5;231;1", quit=>"38;5;231;48;5;238",
) : ();
sub col { my($s,$k)=@_; return $s unless $USE_COLOR && $C{$k}; return "\e[$C{$k}m$s\e[0m"; }
my %BX = $UTF ? (h=>"\x{2500}",v=>"\x{2502}",tl=>"\x{256d}",tr=>"\x{256e}",bl=>"\x{2570}",br=>"\x{256f}",
                 vr=>"\x{251c}",lr=>"\x{2570}",dot=>"\x{00b7}",bul=>"\x{25cf}",arr=>"\x{2192}",blk=>"\x{2588}",lo=>"\x{2591}",warn=>"\x{26a0}")
             : (h=>"-",v=>"|",tl=>"+",tr=>"+",bl=>"+",br=>"+",vr=>"+",lr=>"\\",dot=>".",bul=>"*",arr=>"->",blk=>"#",lo=>".",warn=>"!");
# ---- drawing ----
sub color { my ($s,$c)=@_; return $s unless $USE_COLOR; return "\e[${c}m$s\e[0m"; }
sub at { my($r,$c)=@_; return "\e[${r};${c}H"; }
sub pad { my($s,$w)=@_; $w=0 if $w<0; $s=substr($s,0,$w); my $n=$w-length $s; return $n>0 ? $s.(" "x$n) : $s; }

sub draw { $started ? draw_main() : draw_welcome(); }

# welcome / bring-up screen: ASCII banner + status + fully-tappable QUIT / DEMO / RETRY buttons
sub draw_welcome {
  @btn=(); @rxrow=();
  my $W=$COLS; my $H=$ROWS; my $o="\e[H";
  $o .= at($_,1).(" "x$W) for (1..$H);                         # full clear (low-frequency screen)
  my $bw=0; for (@BANNER){ $bw=length($_) if length($_)>$bw; }
  my $bx=int(($W-$bw)/2); $bx=1 if $bx<1;
  my $by=int($H*0.24); $by=2 if $by<2;
  my $i=0;
  for my $ln (@BANNER){
    if ($W>=$bw) { $o .= at($by+$i,$bx).color($ln,"36;1"); }
    else         { $o .= cput($by+$i,$ln,"36;1",$W); }
    $i++;
  }
  $o .= cput($by+$i+1,"S  E  N  S  O  R","36",$W);
  $o .= cput($by+$i+3,"no-root 802.11 . RTL8852AU . proot","2",$W);
  my $nodev=($status=~/NO ADAPTER/);
  my $sy=$by+$i+5;
  $o .= cput($sy,$status,$nodev?"31;1":"33",$W);
  my $hint = $nodev ? "replug and RETRY, or try DEMO"
           : $status=~/bringing up|firmware/ ? "cold bring-up ~15s . hold on..."
           : $status=~/switching/            ? "switching adapter to Wi-Fi..."
           : "tap the USB popup when it appears";
  $o .= cput($sy+1,$hint,"2",$W);
  # buttons (3 rows tall for touch): DEMO always, RETRY on nodev, QUIT always
  my @b=(["demo"," DEMO "]);
  push @b,["retry"," RETRY "] if $nodev;
  push @b,["quit"," QUIT "];
  my $tw=0; $tw+=length($_->[1])+2 for @b; $tw-=2;
  my $by1=$H-5; my $by2=$H-3; my $bxx=int(($W-$tw)/2); $bxx=1 if $bxx<1;
  my $mid=int(($by1+$by2)/2);
  for my $btn (@b){
    my $lbl=$btn->[1]; my $w=length($lbl);
    my $col = $btn->[0] eq "quit" ? "41;1" : ($btn->[0] eq "demo" ? "42;1" : "43;30");
    for my $r ($by1..$by2){ $o.=at($r,$bxx)."\e[${col}m".($r==$mid?$lbl:(" "x$w))."\e[0m"; }
    push @btn,[$bxx,$by1,$bxx+$w-1,$by2,$btn->[0]];
    $bxx+=$w+2;
  }
  $o .= cput($H,"touch-driven . Ctrl-C force-quit","2",$W);
  print "\e[?2026h".$o."\e[?2026l";   # synchronized output: terminal shows the whole frame atomically (no tearing)
}

sub draw_main {
  @btn=(); @rxrow=();
  my $W=$COLS; my $H=$ROWS; my $o="\e[H";
  # split: LEFT = lists, RIGHT = big control buttons (right-thumb reach)
  my $RW = int($W*0.42); $RW=24 if $RW>24; $RW=16 if $RW<16; $RW=$W-14 if $RW>$W-14; $RW=10 if $RW<10;
  my $rx0 = $W-$RW+1;                                 # right column first col
  my $LW  = $rx0-2; $LW=1 if $LW<1;                   # left width (1-col gutter at rx0-1)

  # ===== LEFT: tab bar (APs / Clients / Graph) + active mode view =====
  my $naps=grep{is_ap($_)}keys %dev; my $ncl=grep{is_client($_)}keys %dev;
  my @full=(["aps","APs"],["clients","Clients"],["graph","Graph"],["tx","TX"]);
  my @short=(["aps","APs"],["clients","Cli"],["graph","Grf"],["tx","TX"]);
  my $fw=0; $fw+=length($_->[1])+2 for @full;
  my @tabs = $fw<=$LW-1 ? @full : @short;
  { my $cx=1;
    for my $t (@tabs){ my $lbl=" ".$t->[1]." "; $o.=at(1,$cx).col($lbl,$t->[0] eq $mode?"tabon":"taboff");
      push @btn,[$cx,1,$cx+length($lbl)-1,1,"mode:".$t->[0]]; $cx+=length($lbl); }
    $o .= at(1,$cx).col(pad("",$LW-$cx+1),"taboff") if $cx<=$LW;
    my $tag=sprintf(" %daps %dcl ",$naps,$ncl);
    $o .= at(1,$LW-length($tag)+1).col($tag,"dim") if $LW-length($tag) > $cx;
  }
  # build the active mode's rows: {t=>text, m=>mac|undef, ch=>ch, k=>colorkey}
  my @rows;
  if ($mode eq "aps") {
    for my $m (sort {$dev{$b}{cnt}<=>$dev{$a}{cnt}} grep{is_ap($_)} keys %dev) {
      my $d=$dev{$m}; my $extra=sprintf("ch%-3s %dcl",$d->{ch}||"?",scalar keys %{$d->{clients}});
      push @rows,{t=>dev_row($m,$extra,$LW),m=>$m,ch=>$d->{ch},k=>"ap"};
    }
  } elsif ($mode eq "clients") {
    for my $m (sort {$dev{$b}{cnt}<=>$dev{$a}{cnt}} grep{is_client($_)} keys %dev) {
      my $d=$dev{$m}; my $ap=$d->{apof}?disp($d->{apof}):"roaming";
      push @rows,{t=>dev_row($m,$BX{arr}." ".substr($ap,0,10),$LW),m=>$m,ch=>$d->{ch},k=>(vendor_of($m) eq "randomized"?"rnd":"cl")};
    }
  } elsif ($mode eq "graph") {                         # graph: AP -> its clients (tree)
    for my $ap (sort {$dev{$b}{cnt}<=>$dev{$a}{cnt}} grep{is_ap($_)} keys %dev) {
      my $d=$dev{$ap};
      push @rows,{t=>" ".$BX{bul}." ".substr(disp($ap),0,$LW-8)." c".($d->{ch}||"?"),m=>$ap,ch=>$d->{ch},k=>"ap"};
      my @cs=sort keys %{$d->{clients}};
      for my $i (0..$#cs){ my $c=$cs[$i];
        push @rows,{t=>"  ".($i==$#cs?$BX{lr}:$BX{vr}).$BX{h}." ".substr(disp($c),0,$LW-6),m=>$c,ch=>$dev{$c}{ch},k=>(vendor_of($c) eq "randomized"?"rnd":"cl")};
      }
    }
    my @orphan=sort grep{is_client($_) && !$dev{$_}{apof}} keys %dev;
    if (@orphan) { push @rows,{t=>" ".$BX{dot}." unassociated",m=>undef,k=>"dim"};
      for my $c (@orphan){ push @rows,{t=>"  ".$BX{lr}.$BX{h}." ".substr(disp($c),0,$LW-6),m=>$c,ch=>$dev{$c}{ch},k=>(vendor_of($c) eq "randomized"?"rnd":"cl")}; }
    }
  } else {                                             # tx: target line + payload picker (tap to arm; big TX fires)
    my $tcs=$tch>0?$tch:($cur_ch||"?");
    push @rows,{t=>" TARGET ".$BX{arr}." ".($target ne""?disp($target)." ch$tcs":"tap here to pick"),m=>undef,k=>"accb",tgt=>1};
    my $lastcat="";
    for my $i (0..$#PAYLOADS){ my $p=$PAYLOADS[$i];
      if ($p->{cat} ne $lastcat){ push @rows,{t=>" ".uc($p->{cat}),m=>undef,k=>"dim"}; $lastcat=$p->{cat}; }
      my $sel=$i==$ti; my $wm=$p->{warn}?$BX{warn}." ":"";
      push @rows,{t=>" ".($sel?$BX{bul}:$BX{dot})." ".$wm.$p->{name}.($sel?"  armed":""),m=>undef,k=>($sel?"accb":($p->{warn}?"warn":"cl")),pay=>$i};
    }
  }
  my $ry=2;
  for my $rr (@rows) { last if $ry>$H;
    my $hot=($target ne "" && $rr->{m} && lc($rr->{m}) eq lc($target));
    $o .= at($ry,1).col(pad($rr->{t},$LW), $hot?"hot":$rr->{k});
    push @rxrow,[$ry,$rr->{m},$rr->{ch}||$cur_ch] if $rr->{m};
    push @btn,[1,$ry,$LW,$ry,"pay".$rr->{pay}] if defined $rr->{pay};
    push @btn,[1,$ry,$LW,$ry,"gotoaps"] if $rr->{tgt};
    $ry++;
  }
  $o .= at($_,1).(" "x$LW) for ($ry..$H);
  $o .= at($_,$rx0-1).col($BX{v},"dim") for (1..$H);   # column divider

  # ===== RIGHT: control panel =====
  # TARGET box: header bar + friendly name (accent) + raw MAC/ch (dim). Tap = clear.
  $o .= at(1,$rx0).col(pad(" TARGET",$RW),"hdr");
  $o .= at(2,$rx0).col(pad(" ".($target ne ""?disp($target):"tap a device >"),$RW),"accb");
  $o .= at(3,$rx0).col(pad(($target ne ""?" ".$target." ":" ")."ch".($tch>0?$tch:($cur_ch||"?")),$RW),"dim");
  push @btn,[$rx0,1,$W,3,"clear"];
  # PAYLOAD selector (rows 5-7); tap opens the TX mode picker on the left
  $o .= at(5,$rx0).col(pad(" PAYLOAD",$RW),"hdr");
  $o .= at(6,$rx0).col(pad(" ".$BX{bul}." ".pname($ti),$RW),"accb");
  $o .= at(7,$rx0).col(pad(" tap -> TX tab to change",$RW),"dim");
  push @btn,[$rx0,5,$W,7,"payload"];
  # info panel: template hint, TX result, live counter, per-channel activity histogram (block bars)
  my $can=($target=~/^[0-9a-fA-F:]{17}$/);
  my @info;   # [text, colorkey]
  push @info, [pname($ti)." ".$BX{dot}." ".$PAYLOADS[$ti]{desc},"dim"];
  push @info, [($txstate ne"" ? $txstate : ($lastp ne"" ? "TX ".$lastp : "TX idle")), ($txstate ne""?"warn":($lastp=~/reply=[1-9]/?"ok":"dim"))];
  push @info, [($started ? "listen ch".($lock||$cur_ch)." ".$BX{dot}." ".scalar(@frames)."fr" : $status), "acc"];
  push @info, ["",undef];
  push @info, ["CHANNELS","accb"];
  my %chc; $chc{$_->{ch}}++ for @frames; my $maxc=1; for(values %chc){ $maxc=$_ if $_>$maxc; }
  my $barw=$RW-9; $barw=1 if $barw<1;
  for my $ch (sort {$a<=>$b} keys %chc){ my $n=int($chc{$ch}/$maxc*$barw);
    push @info, [sprintf("ch%-3s %s %d",$ch,($BX{blk}x$n).($BX{lo}x($barw-$n)),$chc{$ch}),"acc"]; }
  my $qy1=$H-1; my $qy2=$H;                             # QUIT rows
  my $bregBot=$qy1-1;
  my $infoEnd=8+scalar(@info); my $maxIE=$bregBot-8;
  $infoEnd=$maxIE if $infoEnd>$maxIE; $infoEnd=8 if $infoEnd<8;
  my $iy=9;
  for my $ln (@info){ last if $iy>$infoEnd; $o.=at($iy,$rx0).col(pad(" ".substr($ln->[0],0,$RW-1),$RW),$ln->[1]||"dim"); $iy++; }
  $o .= at($_,$rx0).(" "x$RW) for ($iy..$infoEnd);
  my $bmid=int(($infoEnd+1+$bregBot)/2);
  my ($ty1,$ty2,$ry1,$ry2)=($infoEnd+1,$bmid,$bmid+1,$bregBot);
  # TX (big) — warn payloads show a confirm prompt (danger) that a second TX tap sends
  my $pend=($confirm==$ti);
  my $txk=$pend?"danger":($txstate ne""?"txfire":($can?"txarm":"txoff"));
  my $tcen=int(($ty1+$ty2)/2);
  my $l1=$pend?$BX{warn}." SEND ".pid($ti)."?":"TX  ".pid($ti);
  my $l2=$pend?"tap TX again to send":($txstate ne""?"firing...":($can?"tap to fire":"pick a target"));
  for my $r ($ty1..$ty2){
    my $t=$r==$tcen?center($l1,$RW):($r==$tcen+1?center($l2,$RW):(" "x$RW));
    $o.=at($r,$rx0).col($t,$txk);
  }
  push @btn,[$rx0,$ty1,$W,$ty2,"tx"];
  # RX (big)
  my $rxk=$lock?"rxlock":"rx"; my $rcen=int(($ry1+$ry2)/2);
  for my $r ($ry1..$ry2){
    my $t=$r==$rcen?center($lock?"RX  LOCK ch$lock":"RX  HOP",$RW):($r==$rcen+1?center($lock?"tap = hop all":"tap = lock ch".($tch>0?$tch:($cur_ch||"?")),$RW):(" "x$RW));
    $o.=at($r,$rx0).col($t,$rxk);
  }
  push @btn,[$rx0,$ry1,$W,$ry2,"rx"];
  # QUIT
  for my $r ($qy1..$qy2){ $o.=at($r,$rx0).col($r==$qy1?center("QUIT",$RW):(" "x$RW),"quit"); }
  push @btn,[$rx0,$qy1,$W,$qy2,"quit"];
  print "\e[?2026h".$o."\e[?2026l";   # synchronized output: terminal shows the whole frame atomically (no tearing)
}
sub center { my($s,$w)=@_; $w=0 if $w<0; $s=substr($s,0,$w); my $l=int(($w-length $s)/2); $l=0 if $l<0; my $r=$w-length($s)-$l; $r=0 if $r<0; return (" "x$l).$s.(" "x$r); }
# a left-list row: label (truncatable) + FULL 17-char MAC that is never cut (the address is the point)
sub lrow { my($lbl,$mac,$W)=@_; my $L=$W-19; return " ".$mac if $L<1; return sprintf(" %-*s %s",$L,substr($lbl,0,$L),$mac); }
# a device row: bullet + friendly name (SSID/vendor) with a right-aligned extra; a FULL MAC (unresolved) is never cut
sub dev_row {
  my($m,$extra,$W)=@_; my $nm=disp($m); my $pre=" ".$BX{bul}." ";
  if ($nm =~ /^[0-9a-f:]{17}$/i) { my $s=$pre.$m; my $sp=$W-length($s)-length($extra); $sp=1 if $sp<1; return $s.(" "x$sp).$extra; }
  my $avail=$W-length($pre)-length($extra)-1; $avail=1 if $avail<1;
  return sprintf("%s%-*s %s",$pre,$avail,substr($nm,0,$avail),$extra);
}
# centered text on row $r within width $W, truncated to fit and clamped to col>=1 (never a negative cursor col)
sub cput { my($r,$s,$col,$W)=@_; $s=substr($s,0,$W); my $c=int(($W-length $s)/2)+1; $c=1 if $c<1; return at($r,$c).($col?"\e[${col}m$s\e[0m":$s); }

# ---- headless capture: run draw() into a scalar, flatten ANSI into a plain COLS x ROWS grid (screenshots) ----
sub capture_draw { my $cap=""; { local *STDOUT; open(STDOUT,">",\$cap); binmode STDOUT,":encoding(UTF-8)"; draw(); close STDOUT; } utf8::decode($cap); return $cap; }
sub flatten {
  my ($s,$W,$H)=@_;
  my @g = map { [ (" ") x $W ] } (1..$H);
  my ($r,$c)=(1,1);
  while (length $s) {
    if ($s=~s/^\e\[H//)            { ($r,$c)=(1,1); next; }
    if ($s=~s/^\e\[(\d+);(\d+)H//) { ($r,$c)=($1,$2); next; }
    if ($s=~s/^\e\[[0-9;]*m//)     { next; }              # SGR color
    if ($s=~s/^\e\[\?[0-9;]*[hlp]//){ next; }             # private modes
    if ($s=~s/^\e\[[0-9;]*[A-Za-z]//){ next; }            # any other CSI
    my $ch=substr($s,0,1,"");
    if ($ch eq "\n") { $r++; $c=1; next; }
    next if $ch eq "\e" || $ch eq "\r";
    $g[$r-1][$c-1]=$ch if ($r>=1 && $r<=$H && $c>=1 && $c<=$W);
    $c++;
  }
  return join("\n", map { join("",@$_) } @g);
}

# ---- input ----
sub on_mouse {
  my ($mx,$my) = @_;
  for my $b (@btn) {
    if ($mx>=$b->[0] && $mx<=$b->[2] && $my>=$b->[1] && $my<=$b->[3]) {
      my $code=$b->[4];
      if    ($code eq "tx")    { fire_tx(); }
      elsif ($code eq "rx")    { toggle_lock(); }
      elsif ($code eq "clear") { $target=""; $tch=0; }
      elsif ($code eq "quit")  { cleanup(); exit 0; }
      elsif ($code eq "demo")  { enter_demo(); }
      elsif ($code eq "retry") { retry_driver(); }
      elsif ($code =~ /^mode:(\w+)/){ $mode=$1; }
      elsif ($code eq "payload"){ $mode="tx"; }
      elsif ($code eq "gotoaps"){ $mode="aps"; }
      elsif ($code =~ /^pay(\d+)/){ $ti=$1; }
      $confirm=-1 if $code ne "tx";                    # any other tap cancels a pending confirm
      $dirty=1; return;
    }
  }
  for my $r (@rxrow) {                                # tap a list row -> pick its source as target
    if ($my==$r->[0]) { $target=$r->[1]; $tch=$r->[2]; $confirm=-1; $dirty=1; return; }
  }
}
sub toggle_lock {
  if ($lock) { $lock=0; write_ctl(0); }
  else { $lock = $tch>0 ? $tch : ($cur_ch||6); write_ctl($lock); }
}

sub kill_driver {                                    # proot-safe: match by ps, kill by pid (never pkill -f)
  local $SIG{CHLD}='DEFAULT';
  system(q{for p in $(ps -eo pid,args 2>/dev/null | grep -E "[a]x56ch|[s]ensor_cb.sh" | awk '{print $1}'); do kill "$p" 2>/dev/null; done});
}
sub cleanup {
  $SIG{CHLD}='DEFAULT';
  print "\e[?1000l\e[?1006l\e[?25h\e[?1049l";
  system("stty $saved_stty </dev/tty") if $saved_stty;
  kill_driver();
}
$SIG{INT}=$SIG{TERM}=sub { cleanup(); exit 0; };

sub launch_driver {                                  # fork the launcher (device find/switch -> termux-usb -> FIFO)
  my $k = fork();
  return 0 unless defined $k;
  if (!$k) { open(STDOUT,">","/dev/null"); open(STDERR,">","/dev/null"); exec("bash","$T/sensor.sh"); exit 1; }
  return $k;
}
sub retry_driver {
  kill_driver(); @frames=(); %dev=(); $started=0; $demo=0; $status="starting"; unlink $STAT;
  $kid = launch_driver(); $dirty=1;
}
sub refresh_status {                                 # pre-stream only: reflect the launcher's status file
  return if $started;
  return unless -s $STAT;
  open(my $s,"<",$STAT) or return; my $l=<$s>||""; close $s; chomp $l;
  my $old=$status;
  $status = $l=~/^SWITCH/ ? "switching to Wi-Fi mode" : $l=~/^NODEV/ ? "NO ADAPTER FOUND"
          : $l=~/^DEV/    ? "bringing up firmware"    : $status;
  if($old ne $status){ dbg("status: $status (stat='$l')"); $dirty=1; }
}

# ---- selftest: headless model+demo check (no tty), also used to render README screenshots ----
if ($MODE eq "selftest") {
  $demo=1; $started=1; $cur_ch=6; $demo_ch_at=time; demo_seed();
  demo_tick() for (1..400);
  print "== ax56 sensor selftest ==\n";
  print "devices=",scalar(keys %dev)," frames=",scalar(@frames),"\n";
  print "-- APs --\n";
  for my $m (sort keys %dev){ next unless is_ap($m); my $d=$dev{$m};
    printf "  %-17s %-10s %-12s ch%-3s clients=%d\n",$m,$d->{vendor}//"?",$d->{ssid}//"",$d->{ch}//"?",scalar keys %{$d->{clients}}; }
  print "-- Clients --\n";
  for my $m (sort keys %dev){ next unless is_client($m); my $d=$dev{$m};
    printf "  %-17s %-12s -> %s\n",$m,$d->{vendor}//"?",($d->{apof}?disp($d->{apof}):"(none)"); }
  $target="b8:27:eb:0a:1b:2c"; $ti=0; fire_tx(); $demo_tx_at=time-1; demo_tick();
  print "TX probe -> [$lastp]\n";
  print "OK\n"; exit 0;
}

# ---- replay: feed real C/R lines from stdin through the model (validation on captured data) ----
if ($MODE eq "replay") {
  while (my $l=<STDIN>){ chomp $l; handle_line($l); }
  print "devices=",scalar(keys %dev)," frames=",scalar(@frames),"\n";
  print "-- APs --\n";
  for my $m (sort {$dev{$b}{cnt}<=>$dev{$a}{cnt}} grep{is_ap($_)} keys %dev){ my $d=$dev{$m};
    printf "  %-17s %-16s %-16s ch%-3s cl=%d\n",$m,substr($d->{vendor}//"?",0,16),substr($d->{ssid}//"",0,16),$d->{ch}//"?",scalar keys %{$d->{clients}}; }
  print "-- Clients --\n";
  for my $m (sort {$dev{$b}{cnt}<=>$dev{$a}{cnt}} grep{is_client($_)} keys %dev){ my $d=$dev{$m};
    printf "  %-17s %-16s -> %s\n",$m,substr($d->{vendor}//"?",0,16),($d->{apof}?disp($d->{apof}):"-"); }
  exit 0;
}

# ---- render: headless flat screenshot at a given size (proportions check + README) ----
#   sensor.pl render [COLS ROWS]         -> main UI
#   sensor.pl render welcome [COLS ROWS] -> welcome screen
if ($MODE eq "render") {
  my $which = ($ARGV[1]//"") eq "welcome" ? "welcome" : "main";
  my @n = grep { /^\d+$/ } @ARGV[1..$#ARGV];
  $COLS = $n[0]||48; $ROWS = $n[1]||40;
  $mode = $ARGV[1] if ($ARGV[1]//"") =~ /^(aps|clients|graph|tx)$/;
  if ($which eq "welcome") { $status="NO ADAPTER FOUND"; }
  else {
    $demo=1; $started=1; $demo_ch_at=time; demo_seed();
    for (1..40){ for my $ch (1,6,11,36){ $cur_ch=$ch; demo_tick(); } }
    $cur_ch=6; $target="b8:27:eb:0a:1b:2c"; $tch=6; $ti=1;
    $lastp="rts reply=0 seen=7 tx=40";
  }
  print "+"."-"x$COLS."+\n";
  print "|$_|\n" for split /\n/, flatten(capture_draw(),$COLS,$ROWS);
  print "+"."-"x$COLS."+\n";
  exit 0;
}

# ---- boot: fifo + launcher ----
unlink $FIFO; system("mkfifo", $FIFO); clear_ping(); write_ctl(0); unlink $STAT;
$demo = 1 if ($MODE eq "demo");                      # `sensor.pl demo` starts straight in demo mode
if ($demo) { enter_demo(); } else { $kid = launch_driver(); }
dbg("boot demo=$demo kid=".($kid//'-')." fifo=$FIFO");
$SIG{CHLD} = sub { $got_chld=1; $dirty=1; };         # launcher death -> flagged; reaped in the loop (avoids system() races)

sysopen(my $fifo, $FIFO, O_RDONLY|O_NONBLOCK) or do { cleanup(); die "fifo open: $!\n"; };
dbg("fifo opened OK, entering loop");
my $sel = IO::Select->new();
$sel->add($fifo); $sel->add(\*STDIN);
my $fbuf=""; my $ibuf=""; my $last_draw=0; my $FRAME=1/30;

while (1) {
  refresh_status() unless $started;
  if ($got_chld) { $got_chld=0; while ((my $p=waitpid(-1,WNOHANG))>0){} refresh_status(); $dirty=1; }
  # event-driven select timeout: dirty -> wait only until the next 30fps frame slot; demo -> tick cadence;
  # bring-up -> 0.25s to poll the status file; steady idle -> block until an event (0% CPU).
  my $now = time; my $to;
  if    ($dirty) { my $due=$last_draw+$FRAME-$now; $to = $due>0?$due:0; }
  elsif ($demo)  { $to = 0.08; }
  elsif (!$started) { $to = 0.25; }
  else { $to = undef; }
  my @ready = $sel->can_read($to);
  for my $fh (@ready) {
    if ($fh == $fifo) {                               # drain the whole burst, parse complete lines
      my $buf; my $r = sysread($fifo,$buf,1<<16);
      if (defined $r && $r>0) { $fbuf .= $buf;
        dbg("fifo read $r bytes (rd#".(++$::DBGRD).")") if ($ENV{AX56_DEBUG} && ($::DBGRD//0)<6);
        while ($fbuf =~ s/^(.*?)\n//) { handle_line($1); } $dirty=1;
      } elsif (defined $r && $r==0) {                # writer gone/none -> reopen the FIFO
        $::DBGEOF=($::DBGEOF//0)+1; dbg("fifo EOF(r=0) reopen #$::DBGEOF") if ($ENV{AX56_DEBUG} && $::DBGEOF<=6);
        $sel->remove($fifo); close $fifo;
        sysopen($fifo,$FIFO,O_RDONLY|O_NONBLOCK) and $sel->add($fifo);
      }
    } else {                                          # STDIN: touch only (all actions are buttons; no hotkeys)
      my $buf; my $r = sysread(STDIN,$buf,4096); next unless defined $r && $r>0;
      $ibuf .= $buf;
      while (length $ibuf) {
        if ($ibuf =~ s/^\e\[<(\d+);(\d+);(\d+)([Mm])//) {   # SGR mouse / tap
          my ($bcode,$mx,$my,$pr)=($1,$2,$3,$4);
          on_mouse($mx,$my) if ($pr eq "M" && ($bcode & 0x43)==0);  # left press / tap
          next;
        }
        substr($ibuf,0,1,"");                          # drain any other byte (keys ignored by design)
      }
    }
  }
  demo_tick() if $demo;                               # synthesize activity so every feature works with no adapter
  # paced repaint: draw only when dirty and at most once per frame slot (vsync-like; data path is never throttled)
  $now = time;
  if ($dirty && ($now-$last_draw) >= $FRAME) { draw(); $last_draw=$now; $dirty=0; }
}
cleanup();
