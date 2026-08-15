
#include "../utility/utility.h"
#include "../Avfs/Avfs.h"
#include "../terminal/terminal.h"
#include "createFiles.h"

void createFiles(void) {
    // System directories
    avfs_create_dir("/etc");
    avfs_create_dir("/bin");
    avfs_create_dir("/home");
    avfs_create_dir("/home/user");
    avfs_create_dir("/home/user/documents");
    avfs_create_dir("/home/user/projects");
    avfs_create_dir("/home/user/scripts");
    avfs_create_dir("/tmp");
    avfs_create_dir("/var");
    avfs_create_dir("/var/log");
    avfs_create_dir("/dev");
    avfs_create_dir("/unholy");
    avfs_create_dir("/plugins");
    avfs_create_dir("./!/"); // Emergency system directory (ONLY USE WHEN NEEDED LIKE RM COMMAND BOMB)
    avfs_create_dir("./~");  // Hidden system directory
    
    done("Created directory structure", "filesystem");
    
    const char* erm = "\n ";
    avfs_create_file("/unholy/page1.txt", strlen(erm));
    avfs_write_file("/unholy/page1.txt", erm, strlen(erm), 0);
    done("Created Satanic Bible Notes (page 1)", "/unholy/page1.txt");

    // ===== SYSTEM CONFIGURATION FILES (/etc) =====
    
    // Username configuration
    const char* username = "root";
    avfs_create_file("/etc/.username.cfg", strlen(username));
    avfs_write_file("/etc/.username.cfg", username, strlen(username), 0);
    done("Created username configuration", ".username.cfg");
    
    // Password configuration
    const char* password = "toor";
    avfs_create_file("/etc/.password.cfg", strlen(password));
    avfs_write_file("/etc/.password.cfg", password, strlen(password), 0);
    done("Created password configuration", ".password.cfg");
    
    // Auth key configuration
    const char* authkey = "root";
    avfs_create_file("/etc/.authkey.cfg", strlen(authkey));
    avfs_write_file("/etc/.authkey.cfg", authkey, strlen(authkey), 0);
    done("Created auth key configuration", ".authkey.cfg");
    
    // VOP setting
    const char* vop = "off";
    avfs_create_file("/etc/vop", strlen(vop));
    avfs_write_file("/etc/vop", vop, strlen(vop), 0);
    done("VOP setting initialized", "vop");
    
    // System config
    const char* config = "# RadiumOS System Configuration\n"
                        "hostname=radiumos\n"
                        "version=1.0\n"
                        "bootmode=normal\n"
                        "shell=/bin/rsh\n"
                        "timezone=UTC\n"
                        "locale=en_US\n";
    avfs_create_file("/etc/config.sys", strlen(config));
    avfs_write_file("/etc/config.sys", config, strlen(config), 0);
    done("Created system configuration", "config.sys");
    
    // Network configuration
    const char* network = "# Network Configuration\n"
                         "ip=192.168.1.100\n"
                         "netmask=255.255.255.0\n"
                         "gateway=192.168.1.1\n"
                         "dns=8.8.8.8\n"
                         "hostname=radiumos\n";
    avfs_create_file("/etc/network.cfg", strlen(network));
    avfs_write_file("/etc/network.cfg", network, strlen(network), 0);
    done("Created network configuration", "network.cfg");
    
    // Hosts file
    const char* hosts = "127.0.0.1    localhost\n"
                       "192.168.1.100    radiumos\n";
    avfs_create_file("/etc/hosts", strlen(hosts));
    avfs_write_file("/etc/hosts", hosts, strlen(hosts), 0);
    done("Created hosts file", "hosts");
    
    
        // ===== STARTUP SCRIPT: MEOW.RSH (Hardware Probe) =====
    // Purpose: M.E.O.W. (Machine Environment Operational Watch)
    // A daily-bootable script that verifies system hardware integrity.
    


const char* meow_script =
"function setb\n"
" inb 0x61 _c\n"
" math _v $_c | $1\n"
" outb 0x61 $_v\n"
"endfunction\n"
"\n"
"function clrb\n"
" inb 0x61 _c\n"
" math _m ~$1\n"
" math _v $_c & $_m\n"
" outb 0x61 $_v\n"
"endfunction\n"
"\n"
"function main\n"
" inb 0x61 old\n"
" echo Start: $old\n"
" setb 0x40\n"
" inb 0x61 s1\n"
" echo Set: $s1\n"
" clrb 0x40\n"
" inb 0x61 s2\n"
" echo Clr: $s2\n"
" outb 0x61 $old\n"
" echo Done\n"
"endfunction\n"
"call main\n";

    avfs_create_file("/home/user/meow.rsh", strlen(meow_script));
    avfs_write_file("/home/user/meow.rsh", meow_script, strlen(meow_script), 0);
    done("Created M.E.O.W. diagnostic script", "meow.rsh");


// ===== PLUGINS =====

static const char *plugin_net =

// ── Plugin identity variables ─────────────────────────────────────────────────
"set PLUGIN_NAME    Rinptt\n"
"set PLUGIN_VERSION 1.2\n"
"set PLUGIN_AUTHOR  scp_2801\n"
"set PLUGIN_DESC    RTL8139 Network Interface Port Testing Toolkit\n"
"set PLUGIN_CMD     net\n"

// ── information function (intentionally blank — filled by help system) ────────
"function information\n"
"endfunction\n"

// ── command function ──────────────────────────────────────────────────────────
"function command\n"

// Initialise the config map if it doesn't exist yet.
// Stores the NIC I/O base so it persists across calls within the session.
"    if ! defined RINPTT_READY do\n"
"        map.new net_cfg editable\n"
"        map.set net_cfg base 0xC000\n"
"        set RINPTT_READY 1\n"
"    endif\n"

// Pull the current base into $BASE for all subcommands to use.
"    map.get net_cfg base BASE\n"

// Dispatch on $1
"    set subcmd $1\n"
"    if empty subcmd do\n"
"        set subcmd help\n"
"    endif\n"

// ── subcommand: help ──────────────────────────────────────────────────────────
"    if $subcmd == help do\n"
"        echo Rinptt v1.2 -- RTL8139 Network Interface Port Testing Toolkit\n"
"        echo Usage: net <subcommand> [args]\n"
"        echo\n"
"        echo   probe            Detect NIC presence by reading CR register\n"
"        echo   status           Full NIC status: CR ISR RCR TCR decoded\n"
"        echo   mac              Read and display the hardware MAC address\n"
"        echo   isr              Decode Interrupt Status Register\n"
"        echo   rcr              Decode Receive Configuration Register\n"
"        echo   tcr              Decode Transmit Configuration Register\n"
"        echo   irqmask          Show Interrupt Mask Register state\n"
"        echo   rxbuf            Show RX buffer read/write pointer positions\n"
"        echo   missed           Read missed-packet counter (MPC)\n"
"        echo   reset            Check if NIC is stuck in reset\n"
"        echo   loopback         Read TCR loopback mode bits\n"
"        echo   watch [ms] [n]   Poll ISR every ms for n iterations\n"
"        echo   base [port]      Show or set the NIC I/O base port\n"
"        echo   txtest           Read TCR twice and check for stability\n"
"        echo   rxflood          RX FIFO threshold bits and overflow state\n"
"        echo   phyread          Read PHYAR register (PHY/MII link state)\n"
"        echo   report           Run all diagnostics and print summary\n"
"        echo\n"
"        echo Current base port: $BASE\n"
"    endif\n"

// ── subcommand: base ──────────────────────────────────────────────────────────
"    if $subcmd == base do\n"
"        if empty $2 do\n"
"            echo Current NIC base port: $BASE\n"
"        endif\n"
"        if ! empty $2 do\n"
"            map.set net_cfg base $2\n"
"            map.get net_cfg base BASE\n"
"            echo NIC base port set to: $BASE\n"
"        endif\n"
"    endif\n"

// ── subcommand: probe ─────────────────────────────────────────────────────────
// CR is at base+0x37. Valid RTL8139 CR reads 0x0C after init (TE+RE set).
// Reading 0xFF means device absent or bus returned all-ones.
"    if $subcmd == probe do\n"
"        echo Probing RTL8139 at base $BASE ...\n"
"        math CR_PORT $BASE + 0x37\n"
"        inb $CR_PORT CR_VAL\n"
"        hex $CR_VAL CR_HEX\n"
"        echo   CR raw: $CR_HEX\n"
"        if $CR_VAL == 255 do\n"
"            error NIC not present -- bus returned 0xFF at base $BASE\n"
"        endif\n"
"        if $CR_VAL != 255 do\n"
"            echo   NIC appears present (CR != 0xFF)\n"
"            bit.test $CR_VAL 4 RST_BIT\n"
"            bit.test $CR_VAL 3 TE_BIT\n"
"            bit.test $CR_VAL 2 RE_BIT\n"
"            if $RST_BIT == 1 do echo   RST = 1 [RESET IN PROGRESS] endif\n"
"            if $RST_BIT == 0 do echo   RST = 0 [ready] endif\n"
"            if $TE_BIT  == 1 do echo   TE  = 1 [TX enabled] endif\n"
"            if $TE_BIT  == 0 do echo   TE  = 0 [TX disabled] endif\n"
"            if $RE_BIT  == 1 do echo   RE  = 1 [RX enabled] endif\n"
"            if $RE_BIT  == 0 do echo   RE  = 0 [RX disabled] endif\n"
"        endif\n"
"    endif\n"

// ── subcommand: mac ───────────────────────────────────────────────────────────
// MAC bytes are at base+0x00 through base+0x05, one byte each.
"    if $subcmd == mac do\n"
"        echo Reading MAC address from base $BASE ...\n"
"        math P0 $BASE + 0\n"
"        math P1 $BASE + 1\n"
"        math P2 $BASE + 2\n"
"        math P3 $BASE + 3\n"
"        math P4 $BASE + 4\n"
"        math P5 $BASE + 5\n"
"        inb $P0 M0\n"
"        inb $P1 M1\n"
"        inb $P2 M2\n"
"        inb $P3 M3\n"
"        inb $P4 M4\n"
"        inb $P5 M5\n"
"        hex $M0 H0\n"
"        hex $M1 H1\n"
"        hex $M2 H2\n"
"        hex $M3 H3\n"
"        hex $M4 H4\n"
"        hex $M5 H5\n"
"        echo   MAC: $H0:$H1:$H2:$H3:$H4:$H5\n"
"    endif\n"

// ── subcommand: isr ───────────────────────────────────────────────────────────
// ISR 16-bit at base+0x3E. Read low byte only.
// Bits: 0=ROK 1=RER 2=TOK 3=TER 5=RXOVW 6=FOVW
"    if $subcmd == isr do\n"
"        echo Reading ISR at base+0x3E ...\n"
"        math ISR_PORT $BASE + 0x3E\n"
"        inb $ISR_PORT ISR_VAL\n"
"        hex $ISR_VAL ISR_HEX\n"
"        echo   ISR raw: $ISR_HEX\n"
"        bit.test $ISR_VAL 0 B_ROK\n"
"        bit.test $ISR_VAL 1 B_RER\n"
"        bit.test $ISR_VAL 2 B_TOK\n"
"        bit.test $ISR_VAL 3 B_TER\n"
"        bit.test $ISR_VAL 5 B_RXOVW\n"
"        bit.test $ISR_VAL 6 B_FOVW\n"
"        if $B_ROK   == 1 do echo   [SET] ROK   -- RX packet received OK endif\n"
"        if $B_ROK   == 0 do echo   [ - ] ROK   -- no RX event endif\n"
"        if $B_RER   == 1 do echo   [SET] RER   -- RX error endif\n"
"        if $B_RER   == 0 do echo   [ - ] RER   -- no RX error endif\n"
"        if $B_TOK   == 1 do echo   [SET] TOK   -- TX packet sent OK endif\n"
"        if $B_TOK   == 0 do echo   [ - ] TOK   -- no TX event endif\n"
"        if $B_TER   == 1 do echo   [SET] TER   -- TX error endif\n"
"        if $B_TER   == 0 do echo   [ - ] TER   -- no TX error endif\n"
"        if $B_RXOVW == 1 do echo   [SET] RXOVW -- RX buffer overflow! endif\n"
"        if $B_RXOVW == 0 do echo   [ - ] RXOVW -- no RX overflow endif\n"
"        if $B_FOVW  == 1 do echo   [SET] FOVW  -- FIFO overflow! endif\n"
"        if $B_FOVW  == 0 do echo   [ - ] FOVW  -- no FIFO overflow endif\n"
"    endif\n"

// ── subcommand: rcr ───────────────────────────────────────────────────────────
// RCR at base+0x44. Low byte = accept flags; bits 8-10 = RBLEN.
"    if $subcmd == rcr do\n"
"        echo Reading RCR at base+0x44 ...\n"
"        math RCR_PORT $BASE + 0x44\n"
"        inb $RCR_PORT RCR_VAL\n"
"        hex $RCR_VAL RCR_HEX\n"
"        echo   RCR low byte: $RCR_HEX\n"
"        bit.test $RCR_VAL 0 AAP\n"
"        bit.test $RCR_VAL 1 APM\n"
"        bit.test $RCR_VAL 2 AM\n"
"        bit.test $RCR_VAL 3 AB\n"
"        bit.test $RCR_VAL 4 AR\n"
"        bit.test $RCR_VAL 5 AER\n"
"        if $AAP == 1 do echo   [SET] AAP -- Accept All Packets (promiscuous) endif\n"
"        if $AAP == 0 do echo   [ - ] AAP -- not promiscuous endif\n"
"        if $APM == 1 do echo   [SET] APM -- Accept Physical Match endif\n"
"        if $APM == 0 do echo   [ - ] APM -- no physical match filter endif\n"
"        if $AM  == 1 do echo   [SET] AM  -- Accept Multicast endif\n"
"        if $AM  == 0 do echo   [ - ] AM  -- multicast off endif\n"
"        if $AB  == 1 do echo   [SET] AB  -- Accept Broadcast endif\n"
"        if $AB  == 0 do echo   [ - ] AB  -- broadcast off endif\n"
"        if $AR  == 1 do echo   [SET] AR  -- Accept Runt packets endif\n"
"        if $AR  == 0 do echo   [ - ] AR  -- runt packets dropped endif\n"
"        if $AER == 1 do echo   [SET] AER -- Accept Error packets endif\n"
"        if $AER == 0 do echo   [ - ] AER -- error packets dropped endif\n"
"    endif\n"

// ── subcommand: tcr ───────────────────────────────────────────────────────────
// TCR at base+0x40. Bits 8-10 = MXDMA. Bits 4-5 of byte0 = LBK.
// Read byte0 for LBK, byte1 for MXDMA.
"    if $subcmd == tcr do\n"
"        echo Reading TCR at base+0x40 ...\n"
"        math TCR_PORT  $BASE + 0x40\n"
"        math TCR_PORT1 $BASE + 0x41\n"
"        inb $TCR_PORT  TCR_LO\n"
"        inb $TCR_PORT1 TCR_HI\n"
"        hex $TCR_LO TCR_LO_HEX\n"
"        hex $TCR_HI TCR_HI_HEX\n"
"        echo   TCR byte0: $TCR_LO_HEX  byte1: $TCR_HI_HEX\n"
"        band $TCR_HI 7 MXDMA\n"
"        echo   MXDMA (max DMA burst): $MXDMA\n"
"        if $MXDMA == 0 do echo     -> 16 bytes endif\n"
"        if $MXDMA == 1 do echo     -> 32 bytes endif\n"
"        if $MXDMA == 2 do echo     -> 64 bytes endif\n"
"        if $MXDMA == 3 do echo     -> 128 bytes endif\n"
"        if $MXDMA == 4 do echo     -> 256 bytes endif\n"
"        if $MXDMA == 5 do echo     -> 512 bytes endif\n"
"        if $MXDMA == 6 do echo     -> 1024 bytes endif\n"
"        if $MXDMA == 7 do echo     -> unlimited endif\n"
"        bit.test $TCR_LO 4 LBK0\n"
"        bit.test $TCR_LO 5 LBK1\n"
"        echo   Loopback LBK[1:0]: $LBK1 $LBK0\n"
"        if $LBK1 == 0 do if $LBK0 == 0 do echo     -> normal operation endif endif\n"
"        if $LBK1 == 1 do echo     -> loopback mode active endif\n"
"    endif\n"

// ── subcommand: irqmask ───────────────────────────────────────────────────────
// IMR at base+0x3C (16-bit). Read low byte.
"    if $subcmd == irqmask do\n"
"        echo Reading IMR at base+0x3C ...\n"
"        math IMR_PORT $BASE + 0x3C\n"
"        inb $IMR_PORT IMR_VAL\n"
"        hex $IMR_VAL IMR_HEX\n"
"        echo   IMR raw: $IMR_HEX\n"
"        bit.test $IMR_VAL 0 M_ROK\n"
"        bit.test $IMR_VAL 1 M_RER\n"
"        bit.test $IMR_VAL 2 M_TOK\n"
"        bit.test $IMR_VAL 3 M_TER\n"
"        bit.test $IMR_VAL 5 M_RXOVW\n"
"        bit.test $IMR_VAL 6 M_FOVW\n"
"        if $M_ROK   == 1 do echo   [UNMASKED] ROK   interrupt endif\n"
"        if $M_ROK   == 0 do echo   [masked  ] ROK   interrupt endif\n"
"        if $M_RER   == 1 do echo   [UNMASKED] RER   interrupt endif\n"
"        if $M_RER   == 0 do echo   [masked  ] RER   interrupt endif\n"
"        if $M_TOK   == 1 do echo   [UNMASKED] TOK   interrupt endif\n"
"        if $M_TOK   == 0 do echo   [masked  ] TOK   interrupt endif\n"
"        if $M_TER   == 1 do echo   [UNMASKED] TER   interrupt endif\n"
"        if $M_TER   == 0 do echo   [masked  ] TER   interrupt endif\n"
"        if $M_RXOVW == 1 do echo   [UNMASKED] RXOVW interrupt endif\n"
"        if $M_RXOVW == 0 do echo   [masked  ] RXOVW interrupt endif\n"
"        if $M_FOVW  == 1 do echo   [UNMASKED] FOVW  interrupt endif\n"
"        if $M_FOVW  == 0 do echo   [masked  ] FOVW  interrupt endif\n"
"    endif\n"

// ── subcommand: rxbuf ─────────────────────────────────────────────────────────
// CAPR = base+0x38 (16-bit), CBR = base+0x3A (16-bit). Read as two inb each.
"    if $subcmd == rxbuf do\n"
"        echo Reading RX buffer pointers ...\n"
"        math CAPR_LO $BASE + 0x38\n"
"        math CAPR_HI $BASE + 0x39\n"
"        math CBR_LO  $BASE + 0x3A\n"
"        math CBR_HI  $BASE + 0x3B\n"
"        inb $CAPR_LO CL\n"
"        inb $CAPR_HI CH\n"
"        inb $CBR_LO  BL\n"
"        inb $CBR_HI  BH\n"
"        shl $CH 8 CH_SHIFTED\n"
"        bor $CL $CH_SHIFTED CAPR_VAL\n"
"        shl $BH 8 BH_SHIFTED\n"
"        bor $BL $BH_SHIFTED CBR_VAL\n"
"        hex $CAPR_VAL CAPR_HEX\n"
"        hex $CBR_VAL  CBR_HEX\n"
"        echo   CAPR (read ptr) : $CAPR_HEX\n"
"        echo   CBR  (write ptr): $CBR_HEX\n"
"        if $CAPR_VAL == $CBR_VAL do\n"
"            echo   Buffer state: IDLE (read == write)\n"
"        endif\n"
"        if $CAPR_VAL != $CBR_VAL do\n"
"            echo   Buffer state: DATA PENDING (read != write)\n"
"        endif\n"
"    endif\n"

// ── subcommand: missed ────────────────────────────────────────────────────────
// MPC at base+0x4C (3-byte counter). Read low two bytes.
"    if $subcmd == missed do\n"
"        echo Reading Missed Packet Counter (MPC) at base+0x4C ...\n"
"        math MPC_LO $BASE + 0x4C\n"
"        math MPC_HI $BASE + 0x4D\n"
"        inb $MPC_LO ML\n"
"        inb $MPC_HI MH\n"
"        shl $MH 8 MH_SHIFTED\n"
"        bor $ML $MH_SHIFTED MPC_VAL\n"
"        echo   Missed packets: $MPC_VAL\n"
"        if $MPC_VAL == 0 do echo   No missed packets -- good endif\n"
"        if $MPC_VAL != 0 do echo   WARNING: packets were dropped! endif\n"
"    endif\n"

// ── subcommand: reset ─────────────────────────────────────────────────────────
// Check CR bit 4 (RST). If set, NIC is still resetting.
"    if $subcmd == reset do\n"
"        echo Checking reset state (CR bit 4) ...\n"
"        math CR_PORT $BASE + 0x37\n"
"        inb $CR_PORT CR_VAL\n"
"        hex $CR_VAL CR_HEX\n"
"        echo   CR: $CR_HEX\n"
"        bit.test $CR_VAL 4 RST_BIT\n"
"        if $RST_BIT == 1 do\n"
"            error NIC stuck in RESET (CR bit 4 = 1)\n"
"        endif\n"
"        if $RST_BIT == 0 do\n"
"            echo   NIC is NOT in reset -- OK\n"
"        endif\n"
"    endif\n"

// ── subcommand: loopback ──────────────────────────────────────────────────────
// TCR bits 4-5 = LBK[1:0]. 00 = normal, 10/11 = loopback.
"    if $subcmd == loopback do\n"
"        echo Checking TCR loopback bits at base+0x40 ...\n"
"        math TCR_PORT $BASE + 0x40\n"
"        inb $TCR_PORT TCR_VAL\n"
"        hex $TCR_VAL TCR_HEX\n"
"        echo   TCR low byte: $TCR_HEX\n"
"        bit.test $TCR_VAL 4 LBK0\n"
"        bit.test $TCR_VAL 5 LBK1\n"
"        echo   LBK1=$LBK1  LBK0=$LBK0\n"
"        if $LBK1 == 0 do\n"
"            if $LBK0 == 0 do echo   Loopback: DISABLED (normal operation) endif\n"
"        endif\n"
"        if $LBK1 == 1 do echo   Loopback: ENABLED (internal) endif\n"
"        if $LBK0 == 1 do\n"
"            if $LBK1 == 0 do echo   Loopback: ENABLED (external) endif\n"
"        endif\n"
"    endif\n"

// ── subcommand: phyread ───────────────────────────────────────────────────────
// PHYAR at base+0x60 (32-bit). Read as four inb bytes.
// Bit 31 = Flag. Bits 16-20 = PHY reg addr. Bits 0-15 = data.
"    if $subcmd == phyread do\n"
"        echo Reading PHYAR at base+0x60 ...\n"
"        math P0 $BASE + 0x60\n"
"        math P1 $BASE + 0x61\n"
"        math P2 $BASE + 0x62\n"
"        math P3 $BASE + 0x63\n"
"        inb $P0 PH0\n"
"        inb $P1 PH1\n"
"        inb $P2 PH2\n"
"        inb $P3 PH3\n"
"        hex $PH0 H0\n"
"        hex $PH1 H1\n"
"        hex $PH2 H2\n"
"        hex $PH3 H3\n"
"        echo   PHYAR [3..0]: $H3 $H2 $H1 $H0\n"
"        bit.test $PH3 7 PHY_FLAG\n"
"        if $PHY_FLAG == 1 do echo   Flag: SET (read done / write pending) endif\n"
"        if $PHY_FLAG == 0 do echo   Flag: CLEAR (idle) endif\n"
"        band $PH3 0x1F PHY_ADDR\n"
"        echo   PHY reg addr: $PHY_ADDR\n"
"        shl $PH1 8 DATA_HI\n"
"        bor $PH0 $DATA_HI PHY_DATA\n"
"        hex $PHY_DATA PHY_DATA_HEX\n"
"        echo   PHY data: $PHY_DATA_HEX\n"
"    endif\n"

// ── subcommand: txtest ────────────────────────────────────────────────────────
// Reads TCR twice and checks for stability. No actual TX issued.
"    if $subcmd == txtest do\n"
"        echo TX register stability test at base+0x40 ...\n"
"        math TCR_PORT $BASE + 0x40\n"
"        inb $TCR_PORT BEFORE\n"
"        hex $BEFORE BEFORE_HEX\n"
"        echo   TCR read 1: $BEFORE_HEX\n"
"        if $BEFORE == 255 do\n"
"            error Read returned 0xFF -- NIC not present at base $BASE\n"
"        endif\n"
"        if $BEFORE != 255 do\n"
"            echo   Bus access OK\n"
"            inb $TCR_PORT AFTER\n"
"            hex $AFTER AFTER_HEX\n"
"            echo   TCR read 2: $AFTER_HEX\n"
"            if $BEFORE == $AFTER do echo   Stable -- OK endif\n"
"            if $BEFORE != $AFTER do echo   WARNING: unstable read! endif\n"
"        endif\n"
"    endif\n"

// ── subcommand: rxflood ───────────────────────────────────────────────────────
// RBLEN from RCR bits 13-14 and RXOVW from ISR bit 5.
"    if $subcmd == rxflood do\n"
"        echo Checking RX overflow and buffer config ...\n"
"        math RCR_PORT $BASE + 0x44\n"
"        math RCR_HB   $BASE + 0x45\n"
"        math ISR_PORT $BASE + 0x3E\n"
"        inb $RCR_PORT RCR_LO\n"
"        inb $RCR_HB   RCR_HI\n"
"        inb $ISR_PORT ISR_VAL\n"
"        shr $RCR_HI 5 RBLEN_RAW\n"
"        band $RBLEN_RAW 3 RBLEN\n"
"        echo   RBLEN field: $RBLEN\n"
"        if $RBLEN == 0 do echo     -> 8 KB RX buffer endif\n"
"        if $RBLEN == 1 do echo     -> 16 KB RX buffer endif\n"
"        if $RBLEN == 2 do echo     -> 32 KB RX buffer endif\n"
"        if $RBLEN == 3 do echo     -> 64 KB RX buffer endif\n"
"        bit.test $ISR_VAL 5 RXOVW\n"
"        if $RXOVW == 1 do error RXOVW set -- buffer was exceeded! endif\n"
"        if $RXOVW == 0 do echo   No RX overflow -- OK endif\n"
"    endif\n"

// ── subcommand: status ────────────────────────────────────────────────────────
// Full status: CR + ISR + RCR + TCR in one shot.
"    if $subcmd == status do\n"
"        echo ==========================================\n"
"        echo  Rinptt -- Full NIC Status  base: $BASE\n"
"        echo ==========================================\n"
"        math CR_PORT  $BASE + 0x37\n"
"        math ISR_PORT $BASE + 0x3E\n"
"        math RCR_PORT $BASE + 0x44\n"
"        math TCR_PORT $BASE + 0x40\n"
"        inb $CR_PORT  CR_VAL\n"
"        inb $ISR_PORT ISR_VAL\n"
"        inb $RCR_PORT RCR_VAL\n"
"        inb $TCR_PORT TCR_VAL\n"
"        hex $CR_VAL  CR_HEX\n"
"        hex $ISR_VAL ISR_HEX\n"
"        hex $RCR_VAL RCR_HEX\n"
"        hex $TCR_VAL TCR_HEX\n"
"        echo  CR  (0x37): $CR_HEX\n"
"        echo  ISR (0x3E): $ISR_HEX\n"
"        echo  RCR (0x44): $RCR_HEX\n"
"        echo  TCR (0x40): $TCR_HEX\n"
"        echo ------------------------------------------\n"
"        bit.test $CR_VAL  4 RST\n"
"        bit.test $CR_VAL  3 TE\n"
"        bit.test $CR_VAL  2 RE\n"
"        bit.test $ISR_VAL 0 ROK\n"
"        bit.test $ISR_VAL 1 RER\n"
"        bit.test $ISR_VAL 2 TOK\n"
"        bit.test $ISR_VAL 3 TER\n"
"        bit.test $ISR_VAL 5 RXOVW\n"
"        bit.test $RCR_VAL 0 AAP\n"
"        bit.test $RCR_VAL 3 AB\n"
"        bit.test $TCR_VAL 5 LBK\n"
"        if $RST   == 1 do echo  [!] NIC in RESET endif\n"
"        if $RST   == 0 do echo  [+] NIC not in reset endif\n"
"        if $TE    == 1 do echo  [+] TX enabled endif\n"
"        if $TE    == 0 do echo  [-] TX disabled endif\n"
"        if $RE    == 1 do echo  [+] RX enabled endif\n"
"        if $RE    == 0 do echo  [-] RX disabled endif\n"
"        if $ROK   == 1 do echo  [+] ROK -- last RX OK endif\n"
"        if $RER   == 1 do echo  [!] RER -- RX error pending endif\n"
"        if $TOK   == 1 do echo  [+] TOK -- last TX OK endif\n"
"        if $TER   == 1 do echo  [!] TER -- TX error pending endif\n"
"        if $RXOVW == 1 do echo  [!] RXOVW -- RX buffer overflow! endif\n"
"        if $AAP   == 1 do echo  [i] promiscuous mode on endif\n"
"        if $AAP   == 0 do echo  [i] promiscuous mode off endif\n"
"        if $AB    == 1 do echo  [i] broadcast accept on endif\n"
"        if $LBK   == 1 do echo  [!] loopback mode active endif\n"
"        if $LBK   == 0 do echo  [+] no loopback endif\n"
"        echo ==========================================\n"
"    endif\n"

// ── subcommand: watch ─────────────────────────────────────────────────────────
// Poll ISR every $2 ms for $3 iterations (defaults: 500ms, 20 polls).
"    if $subcmd == watch do\n"
"        set WATCH_MS 500\n"
"        set WATCH_N  20\n"
"        if ! empty $2 do set WATCH_MS $2 endif\n"
"        if ! empty $3 do set WATCH_N  $3 endif\n"
"        echo Watching ISR at $BASE -- $WATCH_MS ms interval $WATCH_N polls\n"
"        math ISR_PORT $BASE + 0x3E\n"
"        set LAST_ISR 999\n"
"        set POLL 0\n"
"        while $POLL < $WATCH_N\n"
"            inb $ISR_PORT ISR_NOW\n"
"            if $ISR_NOW != $LAST_ISR do\n"
"                hex $ISR_NOW ISR_HEX\n"
"                echo  [poll $POLL] ISR: $ISR_HEX\n"
"                bit.test $ISR_NOW 0 ROK\n"
"                bit.test $ISR_NOW 1 RER\n"
"                bit.test $ISR_NOW 2 TOK\n"
"                bit.test $ISR_NOW 3 TER\n"
"                bit.test $ISR_NOW 5 RXOVW\n"
"                if $ROK   == 1 do echo    ROK endif\n"
"                if $RER   == 1 do echo    RER endif\n"
"                if $TOK   == 1 do echo    TOK endif\n"
"                if $TER   == 1 do echo    TER endif\n"
"                if $RXOVW == 1 do echo    RXOVW endif\n"
"                set LAST_ISR $ISR_NOW\n"
"            endif\n"
"            pause $WATCH_MS\n"
"            inc POLL\n"
"        endwhile\n"
"        echo Watch complete.\n"
"    endif\n"

// ── subcommand: report ────────────────────────────────────────────────────────
// Runs all diagnostics in sequence.
"    if $subcmd == report do\n"
"        echo\n"
"        echo ============================================================\n"
"        echo  RINPTT FULL DIAGNOSTIC REPORT  base: $BASE\n"
"        echo ============================================================\n"
"        echo [1/6] PROBE\n"
"        call command probe\n"
"        echo\n"
"        echo [2/6] MAC ADDRESS\n"
"        call command mac\n"
"        echo\n"
"        echo [3/6] INTERRUPT STATUS REGISTER\n"
"        call command isr\n"
"        echo\n"
"        echo [4/6] RECEIVE CONFIGURATION\n"
"        call command rcr\n"
"        echo\n"
"        echo [5/6] MISSED PACKET COUNTER\n"
"        call command missed\n"
"        echo\n"
"        echo [6/6] RX BUFFER POINTERS\n"
"        call command rxbuf\n"
"        echo\n"
"        echo ============================================================\n"
"        echo  Report complete.\n"
"        echo ============================================================\n"
"    endif\n"

"endfunction\n";

avfs_create_file("/plugins/net.rsh", sizeof(plugin_net) - 1);
avfs_write_file("/plugins/net.rsh", plugin_net, sizeof(plugin_net) - 1, 0);
done("Created networking test toolkit plugin", "net.rsh");


static const char *plugin_uptime =
"    set PLUGIN_NAME uptime\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC System uptime counter via PIT\n"
"function information\n"
"endfunction\n"
"\n"
"function command\n"
"    outb 0x43 0\n"
"    inb 0x40 PIT_LO\n"
"    inb 0x40 PIT_HI\n"
"    math TICKS $PIT_HI * 256\n"
"    math TICKS $TICKS + $PIT_LO\n"
"    math SECS 1193180 / $TICKS\n"
"    math MINS $SECS / 60\n"
"    math HOURS $MINS / 60\n"
"    math MINS $MINS % 60\n"
"    math SECS $SECS % 60\n"
"    color 5\n"
"    echo   Uptime:\n"
"    color 15\n"
"    print $HOURS\n"
"    print h \n"
"    print $MINS\n"
"    print m \n"
"    print $SECS\n"
"    echo s\n"
"    color 7\n"
"endfunction\n";

avfs_create_file("/plugins/uptime.rsh", strlen(plugin_uptime));
avfs_write_file("/plugins/uptime.rsh", plugin_uptime, strlen(plugin_uptime), 0);
done("Created uptime plugin", "uptime.rsh");

static const char *plugin_meminfo =
"    set PLUGIN_NAME meminfo\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC Memory usage display\n"
"function information\n"
"endfunction\n"
"\n"
"function command\n"
"    mem_total MEM_TOTAL\n"
"    mem_used MEM_USED\n"
"    mem_free MEM_FREE\n"
"    math PCT $MEM_USED * 100\n"
"    math PCT $PCT / $MEM_TOTAL\n"
"    color 5\n"
"    echo   Memory:\n"
"    color 8\n"
"    print   Total: \n"
"    color 15\n"
"    print $MEM_TOTAL\n"
"    echo  KB\n"
"    color 8\n"
"    print   Used:  \n"
"    color 14\n"
"    print $MEM_USED\n"
"    echo  KB\n"
"    color 8\n"
"    print   Free:  \n"
"    color 10\n"
"    print $MEM_FREE\n"
"    echo  KB\n"
"    color 8\n"
"    print   Usage: \n"
"    color 15\n"
"    print $PCT\n"
"    echo %\n"
"    color 7\n"
"endfunction\n";

avfs_create_file("/plugins/meminfo.rsh", strlen(plugin_meminfo));
avfs_write_file("/plugins/meminfo.rsh", plugin_meminfo, strlen(plugin_meminfo), 0);
done("Created meminfo plugin", "meminfo.rsh");

static const char *plugin_diskinfo =
"    set PLUGIN_NAME diskinfo\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC Disk and filesystem usage\n"
"function information\n"
"endfunction\n"
"\n"
"function command\n"
"    set fs_total FS_TOT\n"
"    set fs_used FS_USE\n"
"    set fs_free FS_FREE\n"
"    math PCT $FS_USE * 100\n"
"    math PCT $PCT / $FS_TOT\n"
"    color 5\n"
"    echo   Disk (/):\n"
"    color 8\n"
"    print   Total:  \n"
"    color 15\n"
"    print $FS_TOT\n"
"    echo  KB\n"
"    color 8\n"
"    print   Used:   \n"
"    color 14\n"
"    print $FS_USE\n"
"    echo  KB\n"
"    color 8\n"
"    print   Free:   \n"
"    color 10\n"
"    print $FS_FREE\n"
"    echo  KB\n"
"    color 7\n"
"endfunction\n";

avfs_create_file("/plugins/diskinfo.rsh", strlen(plugin_diskinfo));
avfs_write_file("/plugins/diskinfo.rsh", plugin_diskinfo, strlen(plugin_diskinfo), 0);
done("Created diskinfo plugin", "diskinfo.rsh");

static const char *plugin_processes =
"    set PLUGIN_NAME processes\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC List running tasks\n"
"function information\n"
"endfunction\n"
"\n"
"function command\n"
"    color 5\n"
"    echo   PID  STATE    NAME\n"
"    color 8\n"
"    echo   ---  -------  ----\n"
"    color 7\n"
"    tasks\n"
"endfunction\n";

avfs_create_file("/plugins/processes.rsh", strlen(plugin_processes));
avfs_write_file("/plugins/processes.rsh", plugin_processes, strlen(plugin_processes), 0);
done("Created processes plugin", "processes.rsh");

static const char *plugin_network =
"    set PLUGIN_NAME network\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC Network interface info\n"
"function information\n"
"endfunction\n"
"\n"
"function command\n"
"    inb 0x60 _k\n"
"    color 5\n"
"    echo   Network:\n"
"    color 8\n"
"    print   Interface: \n"
"    color 15\n"
"    echo eth0 (RTL8139)\n"
"    color 8\n"
"    print   IP:        \n"
"    color 15\n"
"    echo 192.168.1.100\n"
"    color 8\n"
"    print   Gateway:   \n"
"    color 15\n"
"    echo 192.168.1.1\n"
"    color 8\n"
"    print   DNS:       \n"
"    color 15\n"
"    echo 8.8.8.8\n"
"    color 8\n"
"    print   MAC:       \n"
"    color 15\n"
"    echo 52:54:00:12:34:56\n"
"    color 7\n"
"endfunction\n";

avfs_create_file("/plugins/network.rsh", strlen(plugin_network));
avfs_write_file("/plugins/network.rsh", plugin_network, strlen(plugin_network), 0);
done("Created network plugin", "network.rsh");

static const char *plugin_datetime =
"    set PLUGIN_NAME datetime\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC RTC date and time display\n"
"function information\n"
"endfunction\n"
"\n"
"function command\n"
"    outb 0x70 0\n"
"    inb 0x71 RTC_SEC\n"
"    outb 0x70 2\n"
"    inb 0x71 RTC_MIN\n"
"    outb 0x70 4\n"
"    inb 0x71 RTC_HOUR\n"
"    outb 0x70 7\n"
"    inb 0x71 RTC_DAY\n"
"    outb 0x70 8\n"
"    inb 0x71 RTC_MON\n"
"    outb 0x70 9\n"
"    inb 0x71 RTC_YEAR\n"
"    shr $RTC_HOUR 4 H_HI\n"
"    band $RTC_HOUR 0xF H_LO\n"
"    math HOUR $H_HI * 10\n"
"    math HOUR $HOUR + $H_LO\n"
"    shr $RTC_MIN 4 M_HI\n"
"    band $RTC_MIN 0xF M_LO\n"
"    math MIN $M_HI * 10\n"
"    math MIN $MIN + $M_LO\n"
"    shr $RTC_SEC 4 S_HI\n"
"    band $RTC_SEC 0xF S_LO\n"
"    math SEC $S_HI * 10\n"
"    math SEC $SEC + $S_LO\n"
"    shr $RTC_DAY 4 D_HI\n"
"    band $RTC_DAY 0xF D_LO\n"
"    math DAY $D_HI * 10\n"
"    math DAY $DAY + $D_LO\n"
"    shr $RTC_MON 4 MO_HI\n"
"    band $RTC_MON 0xF MO_LO\n"
"    math MON $MO_HI * 10\n"
"    math MON $MON + $MO_LO\n"
"    shr $RTC_YEAR 4 Y_HI\n"
"    band $RTC_YEAR 0xF Y_LO\n"
"    math YEAR $Y_HI * 10\n"
"    math YEAR $YEAR + $Y_LO\n"
"    color 5\n"
"    echo   Date/Time:\n"
"    color 15\n"
"    print $YEAR\n"
"    print -\n"
"    print $MON\n"
"    print -\n"
"    print $DAY\n"
"    print   \n"
"    print $HOUR\n"
"    print :\n"
"    print $MIN\n"
"    print :\n"
"    echo $SEC\n"
"    color 7\n"
"endfunction\n";

avfs_create_file("/plugins/datetime.rsh", strlen(plugin_datetime));
avfs_write_file("/plugins/datetime.rsh", plugin_datetime, strlen(plugin_datetime), 0);
done("Created datetime plugin", "datetime.rsh");

static const char *plugin_cpuinfo =
"    set PLUGIN_NAME cpuinfo\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC CPU information via CPUID\n"
"function information\n"
"endfunction\n"
"\n"
"function command\n"
"    cpuid_0 CPU_MAX\n"
"    cpuid_1 CPU_EAX CPU_EBX CPU_ECX CPU_EDX\n"
"    band $CPU_EDX 1 FPU\n"
"    band $CPU_EDX 8 CX8\n"
"    band $CPU_EDX 16 CMOV\n"
"    band $CPU_EDX 0x80000000 SSE\n"
"    color 5\n"
"    echo   CPU:\n"
"    color 8\n"
"    print   Vendor:   \n"
"    color 15\n"
"    echo GenuineIntel\n"
"    color 8\n"
"    print   Max CPUID:\n"
"    color 15\n"
"    echo $CPU_MAX\n"
"    color 8\n"
"    print   FPU:      \n"
"    color 10\n"
"    echo $FPU\n"
"    color 8\n"
"    print   CX8:      \n"
"    color 10\n"
"    echo $CX8\n"
"    color 8\n"
"    print   CMOV:     \n"
"    color 10\n"
"    echo $CMOV\n"
"    color 7\n"
"endfunction\n";

avfs_create_file("/plugins/cpuinfo.rsh", strlen(plugin_cpuinfo));
avfs_write_file("/plugins/cpuinfo.rsh", plugin_cpuinfo, strlen(plugin_cpuinfo), 0);
done("Created cpuinfo plugin", "cpuinfo.rsh");

static const char *plugin_beep =
"    set PLUGIN_NAME beep\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC PC speaker beep with frequency\n"
"function information\n"
"endfunction\n"
"\n"
"function command\n"
"    math FREQ $1\n"
"    if $FREQ == 0\n"
"        set FREQ 440\n"
"    endif\n"
"    math DIV 1193180 / $FREQ\n"
"    outb 0x43 0xB6\n"
"    band $DIV 0xFF LO\n"
"    shr $DIV 8 HI\n"
"    outb 0x42 $LO\n"
"    outb 0x42 $HI\n"
"    inb 0x61 TMP\n"
"    math TMP $TMP | 3\n"
"    outb 0x61 $TMP\n"
"    sleep 200\n"
"    inb 0x61 TMP\n"
"    math TMP $TMP & 0xFC\n"
"    outb 0x61 $TMP\n"
"    color 10\n"
"    echo Beep at $FREQ Hz\n"
"    color 7\n"
"endfunction\n";

avfs_create_file("/plugins/beep.rsh", strlen(plugin_beep));
avfs_write_file("/plugins/beep.rsh", plugin_beep, strlen(plugin_beep), 0);
done("Created beep plugin", "beep.rsh");

static const char *plugin_colortest =
"    set PLUGIN_NAME colortest\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC Test terminal colors 0-15\n"
"function information\n"
"endfunction\n"
"\n"
"function command\n"
"    echo\n"
"    set I 0\n"
"    while $I < 16\n"
"        color $I\n"
"        print   [###]\n"
"        inc I\n"
"    endwhile\n"
"    color 7\n"
"    echo\n"
"endfunction\n";

avfs_create_file("/plugins/colortest.rsh", strlen(plugin_colortest));
avfs_write_file("/plugins/colortest.rsh", plugin_colortest, strlen(plugin_colortest), 0);
done("Created colortest plugin", "colortest.rsh");

static const char *plugin_hexdump =
"    set PLUGIN_NAME hexdump\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC Hex dump of a file\n"
"function information\n"
"endfunction\n"
"\n"
"atl function command\n"
"    set FNAME $1\n"
"    if $FNAME == 0\n"
"        set FNAME /README.txt\n"
"    endif\n"
"    color 5\n"
"    echo   Hex Dump: $FNAME\n"
"    color 8\n"
"    echo   ------------------------\n"
"    color 7\n"
"    hexdump $FNAME\n"
"endfunction\n";

avfs_create_file("/plugins/hexdump.rsh", strlen(plugin_hexdump));
avfs_write_file("/plugins/hexdump.rsh", plugin_hexdump, strlen(plugin_hexdump), 0);
done("Created hexdump plugin", "hexdump.rsh");

static const char *plugin_base64 =
"    set PLUGIN_NAME base64\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC Base64 encode a string\n"
"function information\n"
"endfunction\n"
"\n"
"function command\n"
"    set INPUT $1\n"
"    if $INPUT == 0\n"
"        set INPUT RadiumOS\n"
"    endif\n"
"    b64encode OUTPUT $INPUT\n"
"    color 8\n"
"    print   Input:  \n"
"    color 15\n"
"    echo $INPUT\n"
"    color 8\n"
"    print   Base64: \n"
"    color 14\n"
"    echo $OUTPUT\n"
"    color 7\n"
"endfunction\n";

avfs_create_file("/plugins/base64.rsh", strlen(plugin_base64));
avfs_write_file("/plugins/base64.rsh", plugin_base64, strlen(plugin_base64), 0);
done("Created base64 plugin", "base64.rsh");

static const char *plugin_timer =
"    set PLUGIN_NAME timer\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC Stopwatch with start/stop\n"
"function information\n"
"endfunction\n"
"\n"
"function command\n"
"    color 5\n"
"    echo   Timer started. Press any key to stop.\n"
"    color 7\n"
"    outb 0x70 0\n"
"    inb 0x71 START_S\n"
"    outb 0x70 2\n"
"    inb 0x71 START_M\n"
"    set DONE 0\n"
"    while $DONE == 0\n"
"        poll_scancode K\n"
"        if $K > 0\n"
"            set DONE 1\n"
"        endif\n"
"        sleep 50\n"
"    endwhile\n"
"    outb 0x70 0\n"
"    inb 0x71 END_S\n"
"    outb 0x70 2\n"
"    inb 0x71 END_M\n"
"    math ELAPSED $END_S - $START_S\n"
"    if $ELAPSED < 0\n"
"        math ELAPSED $ELAPSED + 60\n"
"    endif\n"
"    color 10\n"
"    print   Elapsed: \n"
"    print $ELAPSED\n"
"    echo  seconds\n"
"    color 7\n"
"endfunction\n";

avfs_create_file("/plugins/timer.rsh", strlen(plugin_timer));
avfs_write_file("/plugins/timer.rsh", plugin_timer, strlen(plugin_timer), 0);
done("Created timer plugin", "timer.rsh");

static const char *plugin_kbdtest =
"    set PLUGIN_NAME keyboard_test\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC Test keyboard scancode input\n"
"function information\n"
"endfunction\n"
"\n"
"function command\n"
"    color 5\n"
"    echo   Keyboard Test (ESC to quit)\n"
"    color 8\n"
"    echo   ---------------------------\n"
"    color 7\n"
"    set DONE 0\n"
"    while $DONE == 0\n"
"        poll_scancode KEY\n"
"        if $KEY == 1\n"
"            set DONE 1\n"
"        endif\n"
"        if $KEY > 1\n"
"            color 14\n"
"            print   Scancode: \n"
"            color 15\n"
"            echo $KEY\n"
"        endif\n"
"        sleep 10\n"
"    endwhile\n"
"    color 7\n"
"    echo Done.\n"
"endfunction\n";

avfs_create_file("/plugins/keyboard_test.rsh", strlen(plugin_kbdtest));
avfs_write_file("/plugins/keyboard_test.rsh", plugin_kbdtest, strlen(plugin_kbdtest), 0);
done("Created keyboard_test plugin", "keyboard_test.rsh");

static const char *plugin_syslog =
"    set PLUGIN_NAME syslog\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC View system log files\n"
"function information\n"
"endfunction\n"
"\n"
"function command\n"
"    set LOGFILE $1\n"
"    if $LOGFILE == 0\n"
"        set LOGFILE /var/log/system.log\n"
"    endif\n"
"    color 5\n"
"    echo   System Log: $LOGFILE\n"
"    color 8\n"
"    echo   --------------------------\n"
"    color 7\n"
"    cat $LOGFILE\n"
"endfunction\n";

avfs_create_file("/plugins/syslog.rsh", strlen(plugin_syslog));
avfs_write_file("/plugins/syslog.rsh", plugin_syslog, strlen(plugin_syslog), 0);
done("Created syslog plugin", "syslog.rsh");

static const char *plugin_env =
"    set PLUGIN_NAME env\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC Display environment variables\n"
"function information\n"
"endfunction\n"
"\n"
"function command\n"
"    color 5\n"
"    echo   Environment Variables:\n"
"    color 8\n"
"    echo   ----------------------\n"
"    color 10\n"
"    print   USER:     \n"
"    color 15\n"
"    echo $USER\n"
"    color 10\n"
"    print   HOME:     \n"
"    color 15\n"
"    echo $HOME\n"
"    color 10\n"
"    print   SHELL:    \n"
"    color 15\n"
"    echo $SHELL\n"
"    color 10\n"
"    print   HOSTNAME: \n"
"    color 15\n"
"    echo $HOSTNAME\n"
"    color 10\n"
"    print   PATH:     \n"
"    color 15\n"
"    echo /bin:/plugins:/home/user/scripts\n"
"    color 10\n"
"    print   TERM:     \n"
"    color 15\n"
"    echo vga80x50\n"
"    color 7\n"
"endfunction\n";

avfs_create_file("/plugins/env.rsh", strlen(plugin_env));
avfs_write_file("/plugins/env.rsh", plugin_env, strlen(plugin_env), 0);
done("Created env plugin", "env.rsh");

static const char *fetch_plugin =
"    set PLUGIN_NAME fetch\n"
"    set PLUGIN_VER 1.0\n"
"    set PLUGIN_AUTHOR scp_2801\n"
"    set PLUGIN_DESC sysinfo\n"
"function information\n"
"endfunction\n"
"\n"
"function command\n"
"    % ── live hardware reads ──────────────────────────────────\n"
"    outb 0x70 0\n"
"    inb 0x71 RTC_SEC\n"
"    outb 0x70 2\n"
"    inb 0x71 RTC_MIN\n"
"    outb 0x70 4\n"
"    inb 0x71 RTC_HOUR\n"
"    outb 0x43 0\n"
"    inb 0x40 PIT_LO\n"
"    inb 0x40 PIT_HI\n"
"    inb 0x21 PIC_MASK\n"
"    inb 0x64 KBD_STAT\n"
"    outb 0x3D4 0x0F\n"
"    inb 0x3D5 VGA_LO\n"
"    outb 0x3D4 0x0E\n"
"    inb 0x3D5 VGA_HI\n"
"\n"
"    % ── derive values ────────────────────────────────────────\n"
"    math PIT_COUNT $PIT_HI * 256\n"
"    math PIT_COUNT $PIT_COUNT + $PIT_LO\n"
"    if $PIT_COUNT > 0\n"
"        math PIT_FREQ 1193180 / $PIT_COUNT\n"
"    endif\n"
"    if $PIT_COUNT == 0\n"
"        set PIT_FREQ 0\n"
"    endif\n"
"    math VGA_POS $VGA_HI * 256\n"
"    math VGA_POS $VGA_POS + $VGA_LO\n"
"    math VGA_ROW $VGA_POS / 80\n"
"    band $KBD_STAT 1 KBD_OBF\n"
"    band $PIC_MASK 1 IRQ0_MASKED\n"
"    band $PIC_MASK 2 IRQ1_MASKED\n"
"    shr $IRQ1_MASKED 1 IRQ1_MASKED\n"
"\n"
"    % ── bcd decode time ──────────────────────────────────────\n"
"    shr $RTC_HOUR 4 H_HI\n"
"    band $RTC_HOUR 0xF H_LO\n"
"    math HOUR $H_HI * 10\n"
"    math HOUR $HOUR + $H_LO\n"
"    shr $RTC_MIN 4 M_HI\n"
"    band $RTC_MIN 0xF M_LO\n"
"    math MIN $M_HI * 10\n"
"    math MIN $MIN + $M_LO\n"
"    shr $RTC_SEC 4 S_HI\n"
"    band $RTC_SEC 0xF S_LO\n"
"    math SEC $S_HI * 10\n"
"    math SEC $SEC + $S_LO\n"
"\n"
"    % ── draw ─────────────────────────────────────────────────\n"
"    color 5\n"
"    echo\n"
"    color 7\n"
"    echo\n"
"\n"
"    color 8\n"
"    print   OS:        \n"
"    color 15\n"
"    echo RadiumOS i686\n"
"\n"
"    color 8\n"
"    print   Kernel:    \n"
"    color 15\n"
"    echo Rust/C no_std bare-metal\n"
"\n"
"    color 8\n"
"    print   Author:    \n"
"    color 15\n"
"    echo scp_2801\n"
"\n"
"    color 8\n"
"    print   Arch:      \n"
"    color 15\n"
"    echo x86 Protected Mode 32-bit\n"
"\n"
"    color 8\n"
"    print   Shell:     \n"
"    color 15\n"
"    echo RSH v2.3\n"
"\n"
"    color 8\n"
"    print   Time:      \n"
"    color 15\n"
"    print $HOUR\n"
"    print :\n"
"    print $MIN\n"
"    print :\n"
"    echo $SEC\n"
"\n"
"    color 8\n"
"    print   PIT:       \n"
"    color 15\n"
"    print $PIT_FREQ\n"
"    echo  Hz\n"
"\n"
"    color 8\n"
"    print   VGA Row:   \n"
"    color 15\n"
"    echo $VGA_ROW\n"
"\n"
"    color 8\n"
"    print   IRQ0:      \n"
"    if $IRQ0_MASKED == 0\n"
"        color 10\n"
"        echo enabled\n"
"    endif\n"
"    if $IRQ0_MASKED == 1\n"
"        color 12\n"
"        echo MASKED\n"
"    endif\n"
"\n"
"    color 8\n"
"    print   Keyboard:  \n"
"    if $KBD_OBF == 1\n"
"        color 10\n"
"        echo ready\n"
"    endif\n"
"    if $KBD_OBF == 0\n"
"        color 8\n"
"        echo idle\n"
"    endif\n"
"\n"
"    color 8\n"
"    print   Display:   \n"
"    color 15\n"
"    echo VBE 800x600 + VGA Text 80x50\n"
"\n"
"    color 8\n"
"    print   NIC:       \n"
"    color 15\n"
"    echo RTL8139\n"
"\n"
"    color 8\n"
"    print   FS:        \n"
"    color 15\n"
"    echo AVFS custom\n"
"\n"
"    color 7\n"
"    echo\n"
"endfunction\n";

avfs_create_file("/plugins/fetch.rsh", strlen(fetch_plugin));
avfs_write_file("/plugins/fetch.rsh", fetch_plugin, strlen(fetch_plugin), 0);
done("Created fetch plugin", "fetch.rsh");

    const char* autoexec =
"^include \"rsh:main\"\n"
"^include \"rsh:math\"\n"
"^include \"rsh:string\"\n"
"^entrypoint\n"
"% ═══════════════════════════════════════════════════════════════════\n"
"% autoexec.rsh  —  RadiumOS session init (Updated for New Features)\n"
"% ═══════════════════════════════════════════════════════════════════\n"

/* ── Environment & Booleans ─────────────────────────────────────── */
"set OSNAME RadiumOS\n"
"set SHELL RSH\n"
"set ARCH i686\n"
"set HOSTNAME thornesNitro\n"
"set USER thorne\n"
"set HOME /~/home/thorne\n"
"set TMPDIR /~/tmp\n"
"set EDITOR onan\n"
"set PROMPT thorne@RadiumOS>\n"
"set VERSION 1.0\n"
"set LOGLEVEL 1\n"
"set HISTSIZE 100\n"
"set MAXJOBS 16\n"
"set DEBUGMODE 0\n"
"set SCRIPTDIR /~/bin\n"
"set LOGFILE /~/tmp/session.log\n"

"% New: Boolean Support\n"
"bool set SYSTEM_READY true\n"
"bool set SOUND_ENABLED false\n"
"bool set VERBOSE_BOOT false\n"
"bool is SOUND_ENABLED\n"
"if $? == 1 echo [INIT] Sound system active.\n"

/* ── One-Time Initialization (ONT) & Memory Test ──────────────── */
"ont function boot_init\n"
"    echo [BOOT] Running one-time initialization...\n"
"    \n"
"    % Test memory management: Alloc 512 bytes\n"
"    mem_alloc 512 SCRATCH_BUF\n"
"    if $SCRATCH_BUF != 0\n"
"        echo [BOOT] Memory test: Allocated 512B at $SCRATCH_BUF.\n"
"        mem_free $SCRATCH_BUF\n"
"        echo [BOOT] Memory test: Freed buffer.\n"
"    else\n"
"        echo [BOOT] Memory test: Failed to allocate.\n"
"    endif\n"
"    \n"
"    echo [BOOT] System ready.\n"
"endfunction\n"
"call boot_init\n"

/* ── Recursive Function Demo (Factorial) ───────────────────────── */
"function factorial_rec\n"
"    if $1 <= 1\n"
"        return 1\n"
"    endif\n"
"    math _n $1 - 1\n"
"    call factorial_rec $_n\n"
"    math $1 $1 * $_n\n"
"endfunction\n"
"^allowedRecursive\n"
"\n"
"call factorial_rec 5\n"
"echo [MATH] 5! (Recursive) = $1\n"

/* ── Aliases ───────────────────────────────────────────────── */
"alias ll ls -l\n"
"alias la ls -a\n"
"alias cls clear\n"
"alias q exit 0\n"
"alias hi echo Hello $1\n"
"alias upper str_upper _u_tmp $1\n"
"alias lower str_lower _l_tmp $1\n"
"alias bye echo Goodbye $USER. Shutting down...\n"
"alias ver echo $OSNAME $VERSION on $ARCH\n"
"alias myip ifconfig\n"
"alias top tasks\n"
"alias md mkdir\n"
"alias del rm\n"
"alias copy cp\n"
"alias move mv\n"
"alias here pwd\n"
"alias back cd ..\n"
"alias root cd /~\n"
"alias tmp cd /~/tmp\n"
"alias scripts cd /~/bin\n"
"alias home cd /~/home/thorne\n"
"alias show cat\n"
"alias count wc\n"
"alias find_file find\n"
"alias calc math\n"
"alias now uptime\n"
"alias procs tasks\n"
"alias mem_info meminfo\n"
"alias disk_info df\n"
"alias net_info ifconfig\n"
"alias reload_env run /bin/autoexec.rsh\n"

/* ── Output Redirection Demo ──────────────────────────────────── */
"echo [BOOT] Capturing initial system status to log...\n"
"sysinfo > /tmp/startup_log.txt\n"

/* ── Navigation functions ───────────────────────────────────── */
"function cdh\n"
"    cd /~/home/thorne\n"
"    pwd\n"
"endfunction\n"

"function cdbin\n"
"    cd /~/bin\n"
"    pwd\n"
"endfunction\n"

"function cdtmp\n"
"    cd /~/tmp\n"
"    pwd\n"
"endfunction\n"

"function cdroot\n"
"    cd /~\n"
"    pwd\n"
"endfunction\n"

"function mkcd\n"
"    mkdir $1\n"
"    cd $1\n"
"    print_ok Created and entered: $1\n"
"endfunction\n"

"function up\n"
"    cd ..\n"
"    pwd\n"
"endfunction\n"

"function up2\n"
"    cd ..\n"
"    cd ..\n"
"    pwd\n"
"endfunction\n"

"function up3\n"
"    cd ..\n"
"    cd ..\n"
"    cd ..\n"
"    pwd\n"
"endfunction\n"

"function goto\n"
"    cd $1\n"
"    pwd\n"
"    ls\n"
"endfunction\n"

/* ── File utilities ─────────────────────────────────────────── */
"function mktemp\n"
"    set _mt_name tmp_$1\n"
"    touch /~/tmp/$_mt_name\n"
"    print_ok Created temp file: /~/tmp/$_mt_name\n"
"endfunction\n"

"function backup\n"
"    concat _bk_dest $1 .bak\n"
"    cp $1 $_bk_dest\n"
"    print_ok Backed up $1 to $_bk_dest\n"
"endfunction\n"

"function show_file\n"
"    print_header $1\n"
"    cat $1\n"
"    print_sep\n"
"endfunction\n"

"function file_size\n"
"    print_kv Size $1\n"
"    wc $1\n"
"endfunction\n"

"function touch_many\n"
"    touch $1\n"
"    touch $2\n"
"    touch $3\n"
"    print_ok Created files\n"
"endfunction\n"

"function rm_safe\n"
"    confirm Remove $1?\n"
"    rm $1\n"
"    print_ok Removed: $1\n"
"endfunction\n"

"function cp_safe\n"
"    if_exists $2 confirm Overwrite $2?\n"
"    cp $1 $2\n"
"    print_ok Copied $1 to $2\n"
"endfunction\n"

"function mv_safe\n"
"    if_exists $2 confirm Overwrite $2?\n"
"    mv $1 $2\n"
"    print_ok Moved $1 to $2\n"
"endfunction\n"

/* ── System info functions ──────────────────────────────────── */
"function sysinfo\n"
"    print_banner RadiumOS System Info\n"
"    print_kv OS $OSNAME\n"
"    print_kv Version $VERSION\n"
"    print_kv Arch $ARCH\n"
"    print_kv Shell $SHELL\n"
"    print_kv User $USER\n"
"    print_kv Host $HOSTNAME\n"
"    print_kv Home $HOME\n"
"    print_kv Tmpdir $TMPDIR\n"
"    print_kv Editor $EDITOR\n"
"    print_kv LogLevel $LOGLEVEL\n"
"    print_kv DebugMode $DEBUGMODE\n"
"    print_kv ScriptDir $SCRIPTDIR\n"
"    print_sep\n"
"endfunction\n"

"function disk\n"
"    print_header Disk Usage\n"
"    df\n"
"    print_sep\n"
"endfunction\n"

"function mem\n"
"    print_header Memory\n"
"    meminfo\n"
"    print_sep\n"
"endfunction\n"

"function ps\n"
"    print_header Processes\n"
"    tasks\n"
"    print_sep\n"
"endfunction\n"

"function net\n"
"    print_header Network\n"
"    ifconfig\n"
"    print_sep\n"
"endfunction\n"

"function full_status\n"
"    sysinfo\n"
"    disk\n"
"    mem\n"
"    net\n"
"    ps\n"
"endfunction\n"

/* ── Math-powered functions (uses rsh:math) ─────────────────── */
"function calc_add\n"
"    show_add $1 $2\n"
"endfunction\n"

"function calc_sub\n"
"    show_sub $1 $2\n"
"endfunction\n"

"function calc_mul\n"
"    show_mul $1 $2\n"
"endfunction\n"

"function calc_div\n"
"    show_div $1 $2\n"
"endfunction\n"

"function calc_mod\n"
"    show_mod $1 $2\n"
"endfunction\n"

"function calc_pow\n"
"    show_pow $1 $2\n"
"endfunction\n"

"function calc_fib\n"
"    show_fib $1\n"
"endfunction\n"

"function calc_fact\n"
"    show_fact $1\n"
"endfunction\n"

"function calc_prime\n"
"    show_prime $1\n"
"endfunction\n"

"function calc_gcd\n"
"    show_gcd $1 $2\n"
"endfunction\n"

"function calc_lcm\n"
"    show_lcm $1 $2\n"
"endfunction\n"

"function calc_sqrt\n"
"    isqrt _csq_r $1\n"
"    echo sqrt($1) ~= $_csq_r\n"
"endfunction\n"

"function calc_pct\n"
"    percent _cp_r $1 $2\n"
"    echo $1 of $2 = $_cp_r%\n"
"endfunction\n"

"function calc_bytes\n"
"    to_kb _cb_kb $1\n"
"    to_mb _cb_mb $1\n"
"    print_kv Bytes $1\n"
"    print_kv KB $_cb_kb\n"
"    print_kv MB $_cb_mb\n"
"endfunction\n"

"function calc_temp\n"
"    celsius_to_f _ct_f $1\n"
"    echo $1 C = $_ct_f F\n"
"endfunction\n"

"function math_demo_run\n"
"    math_demo\n"
"endfunction\n"

/* ── String-powered functions (uses rsh:string) ─────────────── */
"function str_info\n"
"    str_inspect $1\n"
"endfunction\n"

"function str_up\n"
"    upper _su_r $1\n"
"    echo $_su_r\n"
"endfunction\n"

"function str_lo\n"
"    lower _sl_r $1\n"
"    echo $_sl_r\n"
"endfunction\n"

"function str_rev\n"
"    reverse _sr_r $1\n"
"    echo $_sr_r\n"
"endfunction\n"

"function str_len_show\n"
"    length _sls_r $1\n"
"    print_kv Length $_sls_r\n"
"endfunction\n"

"function str_has_check\n"
"    str_has _shc_r $1 $2\n"
"    if $_shc_r == 1\n"
"        print_ok $1 contains $2\n"
"    else\n"
"        print_warn $1 does not contain $2\n"
"    endif\n"
"endfunction\n"

"function str_demo_run\n"
"    string_demo\n"
"endfunction\n"

"function str_build_path\n"
"    build_path _sbp_r $1 $2\n"
"    echo $_sbp_r\n"
"endfunction\n"

"function str_validate\n"
"    validate_nonempty $1 $1\n"
"    validate_is_num $1 $1\n"
"endfunction\n"

"function str_pad_show\n"
"    pad_right _sps_r $1 20 .\n"
"    echo $_sps_r\n"
"endfunction\n"

/* ── Logging functions (uses LOGLEVEL from env) ──────────────── */
"function log\n"
"    if $LOGLEVEL >= 1\n"
"        echo [LOG] $1\n"
"    endif\n"
"endfunction\n"

"function log_info\n"
"    if $LOGLEVEL >= 1\n"
"        print_info $1\n"
"    endif\n"
"endfunction\n"

"function log_warn\n"
"    print_warn $1\n"
"endfunction\n"

"function log_error\n"
"    print_error $1\n"
"endfunction\n"

"function log_debug\n"
"    if $DEBUGMODE == 1\n"
"        echo [DEBUG] $1\n"
"    endif\n"
"endfunction\n"

"function log_step\n"
"    if $LOGLEVEL >= 1\n"
"        print_step $1 $2\n"
"    endif\n"
"endfunction\n"

"function debug_on\n"
"    set DEBUGMODE 1\n"
"    print_ok Debug mode enabled\n"
"endfunction\n"

"function debug_off\n"
"    set DEBUGMODE 0\n"
"    print_ok Debug mode disabled\n"
"endfunction\n"

"function loglevel_set\n"
"    set LOGLEVEL $1\n"
"    print_kv LogLevel $LOGLEVEL\n"
"endfunction\n"

/* ── Script runner functions ────────────────────────────────── */
"function run_and_check\n"
"    log_step 1 Running $1\n"
"    run $1\n"
"    print_ok Done: $1\n"
"endfunction\n"

"function confirm_run\n"
"    confirm Run $1?\n"
"    run_and_check $1\n"
"endfunction\n"

"function run_quiet\n"
"    run $1\n"
"endfunction\n"

"function run_verbose\n"
"    print_header Running: $1\n"
"    run $1\n"
"    print_done $1\n"
"endfunction\n"

/* ── Counter / loop utilities ───────────────────────────────── */
"function count_files\n"
"    set _cf_n 0\n"
"    for _cf_f in $1/*\n"
"        inc _cf_n\n"
"    endfor\n"
"    print_kv Files $_cf_n\n"
"endfunction\n"

"function repeat_cmd\n"
"    set _rc_i 0\n"
"    while $_rc_i < $1\n"
"        call $2\n"
"        inc _rc_i\n"
"    endwhile\n"
"endfunction\n"

"function countdown_go\n"
"    countdown $1\n"
"endfunction\n"

"function count_to\n"
"    countup 1 $1\n"
"endfunction\n"

"function sum_to\n"
"    sum_range _st_r 1 $1\n"
"    echo Sum 1..$1 = $_st_r\n"
"endfunction\n"

/* ── Environment management ─────────────────────────────────── */
"function env_check\n"
"    str_empty _ec_r $1\n"
"    if $_ec_r == 1\n"
"        print_warn $1 is not set\n"
"    else\n"
"        print_ok $1 is set\n"
"    endif\n"
"endfunction\n"

"function env_show\n"
"    print_kv $1 $1\n"
"endfunction\n"

"function env_require\n"
"    require_var $1\n"
"endfunction\n"

"function env_default\n"
"    set_default $1 $2\n"
"endfunction\n"

"function env_dump\n"
"    print_header Environment\n"
"    vars\n"
"    print_sep\n"
"endfunction\n"

"function env_reset\n"
"    set LOGLEVEL 1\n"
"    set DEBUGMODE 0\n"
"    print_ok Environment reset to defaults\n"
"endfunction\n"

/* ── Alias management ───────────────────────────────────────── */
"function show_aliases\n"
"    print_header Aliases and Functions\n"
"    aliases\n"
"    print_sep\n"
"endfunction\n"

"function inspect\n"
"    which $1\n"
"endfunction\n"

"function zap\n"
"    confirm Remove alias $1?\n"
"    unalias $1\n"
"    print_ok Removed: $1\n"
"endfunction\n"

"function new_alias\n"
"    alias $1 $2\n"
"    print_ok Alias created: $1\n"
"endfunction\n"

/* ── Session management ─────────────────────────────────────── */
"function reload\n"
"    print_info Reloading autoexec...\n"
"    run /bin/autoexec.rsh\n"
"    print_ok Autoexec reloaded\n"
"endfunction\n"

"function session_info\n"
"    print_banner Session Info\n"
"    print_kv User $USER\n"
"    print_kv Host $HOSTNAME\n"
"    print_kv Shell $SHELL\n"
"    print_kv Home $HOME\n"
"    print_kv Editor $EDITOR\n"
"    print_kv LogLevel $LOGLEVEL\n"
"    print_kv DebugMode $DEBUGMODE\n"
"    print_sep\n"
"endfunction\n"

"function goodbye\n"
"    echo\n"
"    echo Goodbye $USER. Session ended.\n"
"    echo\n"
"    exit 0\n"
"endfunction\n"

"function lock\n"
"    confirm Lock session?\n"
"    clear\n"
"    echo Session locked.\n"
"endfunction\n"

/* ── Path / file existence helpers ──────────────────────────── */
"function path_exists\n"
"    if_exists $1 print_ok $1 exists\n"
"endfunction\n"

"function path_check\n"
"    if_exists $1 print_ok $1 found\n"
"    if_not_exists $1 print_warn $1 not found\n"
"endfunction\n"

"function ls_here\n"
"    pwd\n"
"    ls\n"
"endfunction\n"

"function ls_long\n"
"    pwd\n"
"    ls -l\n"
"endfunction\n"

"function ls_all\n"
"    pwd\n"
"    ls -a\n"
"endfunction\n"

/* ── String inspect / test wrappers ─────────────────────────── */
"function str_inspect_var\n"
"    str_inspect $1\n"
"endfunction\n"

"function str_test\n"
"    print_header String Test: $1\n"
"    str_has_check $1 $2\n"
"    starts_with _st_sw $1 $2\n"
"    print_kv StartsWith $_st_sw\n"
"    ends_with _st_ew $1 $2\n"
"    print_kv EndsWith $_st_ew\n"
"    print_sep\n"
"endfunction\n"

/* ── Quick math wrappers ─────────────────────────────────────── */
"function is_prime_check\n"
"    is_prime _ipc_r $1\n"
"    print_bool $_ipc_r\n"
"endfunction\n"

"function fib_seq\n"
"    set _fs_i 0\n"
"    while $_fs_i <= $1\n"
"        show_fib $_fs_i\n"
"        inc _fs_i\n"
"    endwhile\n"
"endfunction\n"

"function primes_to\n"
"    set _pt_i 2\n"
"    while $_pt_i <= $1\n"
"        is_prime _pt_r $_pt_i\n"
"        if $_pt_r == 1\n"
"            echo $_pt_i\n"
"        endif\n"
"        inc _pt_i\n"
"    endwhile\n"
"endfunction\n"

"function times_table\n"
"    print_header Times Table: $1\n"
"    set _tt_i 1\n"
"    while $_tt_i <= 12\n"
"        mul _tt_r $1 $_tt_i\n"
"        echo $1 x $_tt_i = $_tt_r\n"
"        inc _tt_i\n"
"    endwhile\n"
"    print_sep\n"
"endfunction\n"

/* ── Startup checks ─────────────────────────────────────────── */
"env_check USER\n"
"env_check HOME\n"
"env_check HOSTNAME\n"
"env_check SHELL\n"
"env_check VERSION\n"

/* ── Welcome banner ─────────────────────────────────────────── */
"function main\n"
"clear\n"
"echo\n"
"echo ########################################\n"
"echo #                                      #\n"
"echo #   RadiumOS  //  i686 bare-metal      #\n"
"echo #   Shell: RSH        User: thorne     #\n"
"echo #   Type  help         for commands    #\n"
"echo #   Type  sysinfo      for system info #\n"
"echo #   Type  show_aliases for aliases     #\n"
"echo #   Type  math_demo_run for math demo  #\n"
"echo #   Type  str_demo_run  for str demo   #\n"
"echo #   Type  full_status  for all info    #\n"
"echo #                                      #\n"
"echo ########################################\n"
"echo\n"
"ver\n"
"echo\n"
"echo Session ready. Welcome back, $USER.\n"
"echo\n"
"endfunction\n"
"call main\n";


avfs_create_file("/bin/autoexec.rsh", strlen(autoexec));
avfs_write_file("/bin/autoexec.rsh", autoexec, strlen(autoexec), 0);
done("Created autoexec script", "autoexec.rsh");
    // ═══════════════════════════════════════════════════════════════════════════
//  RSH Standard Library  —  rsh:main
//  Include with:  ^include "rsh:main"
//  Comment syntax: lines beginning with  %  or  #  or  __~~%~~__
// ═══════════════════════════════════════════════════════════════════════════


    const char* rshmain =

"__~~%~~__ RSH Standard Library - Main Module\n"
"__~~%~~__ Include with: ^include \"rsh:main\"\n"
"__~~%~~__ RadiumOS / RSH interpreter  (i686 bare-metal)\n"

/* ═══════════════════════════════════════════════════════════════════
   1  OUTPUT / FORMATTING
   ═══════════════════════════════════════════════════════════════════ */

"% ── 1 Output helpers ───────────────────────────────────────────────\n"
/* strrev DEST SRC  — reverse a string character by character */
"function strrev\n"
"    set $1\n"
"    strlen _srr_len $2\n"
"    if $_srr_len == 0\n"
"        set $1\n"
"    else\n"
"        set _srr_i 0\n"
"        while $_srr_i < $_srr_len\n"
"            math _srr_idx $_srr_len - $_srr_i\n"
"            dec _srr_idx\n"
"            substr _srr_ch $2 $_srr_idx 1\n"
"            concat $1 $1 $_srr_ch\n"
"            inc _srr_i\n"
"        endwhile\n"
"    endif\n"
"endfunction\n"
"function if_not_exists\n"
"    set _ine_flag 1\n"
"    if_exists $1 set _ine_flag 0\n"
"    if $_ine_flag == 1\n"
"        call $2\n"
"    endif\n"
"endfunction\n"
"function print_banner\n"
"    echo\n"
"    echo ========================================\n"
"    echo   $1\n"
"    echo ========================================\n"
"    echo\n"
"endfunction\n"

"function print_header\n"
"    echo\n"
"    echo -- $1 --\n"
"    echo ----------------------------------------\n"
"endfunction\n"

"function print_sep\n"
"    echo ----------------------------------------\n"
"endfunction\n"

"function print_error\n"
"    echo [ERROR] $1\n"
"endfunction\n"

"function print_ok\n"
"    echo [OK] $1\n"
"endfunction\n"

"function print_warn\n"
"    echo [WARN] $1\n"
"endfunction\n"

"function println\n"
"    echo $1\n"
"endfunction\n"

"function print_kv\n"
"    echo $1 : $2\n"
"endfunction\n"

"function print_bool\n"
"    if $1 == 1\n"
"        echo true\n"
"    else\n"
"        echo false\n"
"    endif\n"
"endfunction\n"

"function print_list_item\n"
"    echo  - $1\n"
"endfunction\n"

"function print_numbered\n"
"    echo $1. $2\n"
"endfunction\n"

"function print_indent\n"
"    echo     $1\n"
"endfunction\n"

"function print_title\n"
"    echo\n"
"    echo *** $1 ***\n"
"    echo\n"
"endfunction\n"

"function print_section\n"
"    echo\n"
"    echo [ $1 ]\n"
"    echo\n"
"endfunction\n"

"function print_tag\n"
"    echo <$1> $2\n"
"endfunction\n"

"function print_pair\n"
"    echo $1 = $2\n"
"endfunction\n"

"function print_arrow\n"
"    echo => $1\n"
"endfunction\n"

"function print_bullet\n"
"    echo * $1\n"
"endfunction\n"

"function print_done\n"
"    echo [DONE] $1\n"
"endfunction\n"

"function print_info\n"
"    echo [INFO] $1\n"
"endfunction\n"

"function print_step\n"
"    echo [STEP $1] $2\n"
"endfunction\n"

"function print_result\n"
"    echo Result: $1\n"
"endfunction\n"

"function print_count\n"
"    echo Count: $1\n"
"endfunction\n"

"function print_status\n"
"    echo Status: $1\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   2  MATH HELPERS
   ═══════════════════════════════════════════════════════════════════ */

"% ── 2 Math helpers ─────────────────────────────────────────────────\n"

"function clamp\n"
"    set _cv $2\n"
"    if $_cv < $3\n"
"        set _cv $3\n"
"    endif\n"
"    if $_cv > $4\n"
"        set _cv $4\n"
"    endif\n"
"    set $1 $_cv\n"
"endfunction\n"

"function abs_val\n"
"    if $2 < 0\n"
"        math $1 0 - $2\n"
"    else\n"
"        set $1 $2\n"
"    endif\n"
"endfunction\n"

"function max_val\n"
"    if $2 > $3\n"
"        set $1 $2\n"
"    else\n"
"        set $1 $3\n"
"    endif\n"
"endfunction\n"

"function min_val\n"
"    if $2 < $3\n"
"        set $1 $2\n"
"    else\n"
"        set $1 $3\n"
"    endif\n"
"endfunction\n"

"function is_even\n"
"    math _ie_r $2 % 2\n"
"    if $_ie_r == 0\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function is_odd\n"
"    math _io_r $2 % 2\n"
"    if $_io_r == 0\n"
"        set $1 0\n"
"    else\n"
"        set $1 1\n"
"    endif\n"
"endfunction\n"

"function sign\n"
"    if $2 > 0\n"
"        set $1 1\n"
"    elif $2 < 0\n"
"        set $1 -1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function square\n"
"    math $1 $2 * $2\n"
"endfunction\n"

"function cube\n"
"    math _cb_sq $2 * $2\n"
"    math $1 $_cb_sq * $2\n"
"endfunction\n"

"function pow2\n"
"    set $1 1\n"
"    set _pw_i 0\n"
"    while $_pw_i < $2\n"
"        math $1 $1 * 2\n"
"        inc _pw_i\n"
"    endwhile\n"
"endfunction\n"

"function factorial\n"
"    set $1 1\n"
"    set _fc_i 2\n"
"    while $_fc_i <= $2\n"
"        math $1 $1 * $_fc_i\n"
"        inc _fc_i\n"
"    endwhile\n"
"endfunction\n"

"function gcd\n"
"    set _ga $2\n"
"    set _gb $3\n"
"    while $_gb != 0\n"
"        math _gt $_ga % $_gb\n"
"        set _ga $_gb\n"
"        set _gb $_gt\n"
"    endwhile\n"
"    set $1 $_ga\n"
"endfunction\n"

"function sum_range\n"
"    set $1 0\n"
"    for _sr2_v in $2..$3\n"
"        math $1 $1 + $_sr2_v\n"
"    endfor\n"
"endfunction\n"

"function product_range\n"
"    set $1 1\n"
"    for _pr_v in $2..$3\n"
"        math $1 $1 * $_pr_v\n"
"    endfor\n"
"endfunction\n"

"function div_safe\n"
"    if $3 == 0\n"
"        echo [ERROR] Division by zero\n"
"        set $1 0\n"
"    else\n"
"        math $1 $2 / $3\n"
"    endif\n"
"endfunction\n"

"function mod_safe\n"
"    if $3 == 0\n"
"        echo [ERROR] Modulo by zero\n"
"        set $1 0\n"
"    else\n"
"        math $1 $2 % $3\n"
"    endif\n"
"endfunction\n"

"function percent\n"
"    math _pct_t $2 * 100\n"
"    math $1 $_pct_t / $3\n"
"endfunction\n"

"function average\n"
"    math $1 $2 + $3\n"
"    math $1 $1 / 2\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   3  STRING HELPERS
   ═══════════════════════════════════════════════════════════════════ */

"% ── 3 String helpers ───────────────────────────────────────────────\n"

"function str_empty\n"
"    strlen _se_len $2\n"
"    if $_se_len == 0\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function str_equals\n"
"    if $2 == $3\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function str_contains\n"
"    contains $1 $2 $3\n"
"endfunction\n"

"function str_len\n"
"    strlen $1 $2\n"
"endfunction\n"

"function str_upper\n"
"    toupper $1 $2\n"
"endfunction\n"

"function str_lower\n"
"    tolower $1 $2\n"
"endfunction\n"

"function str_sub\n"
"    substr $1 $2 $3 $4\n"
"endfunction\n"

"function str_repeat\n"
"    set $1\n"
"    set _sr_i 0\n"
"    while $_sr_i < $3\n"
"        concat $1 $1 $2\n"
"        inc _sr_i\n"
"    endwhile\n"
"endfunction\n"

"function str_pad_right\n"
"    set $1 $2\n"
"    strlen _spr_len $2\n"
"    while $_spr_len < $3\n"
"        concat $1 $1 $4\n"
"        inc _spr_len\n"
"    endwhile\n"
"endfunction\n"

"function str_pad_left\n"
"    strlen _spl_len $2\n"
"    set _spl_pad\n"
"    while $_spl_len < $3\n"
"        concat _spl_pad $4 $_spl_pad\n"
"        inc _spl_len\n"
"    endwhile\n"
"    concat $1 $_spl_pad $2\n"
"endfunction\n"

"function str_not_empty\n"
"    strlen _sne_len $2\n"
"    if $_sne_len > 0\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function str_starts\n"
"    startswith $1 $2 $3\n"
"endfunction\n"

"function str_ends\n"
"    endswith $1 $2 $3\n"
"endfunction\n"

"function str_concat\n"
"    concat $1 $2 $3\n"
"endfunction\n"

"function str_copy\n"
"    set $1 $2\n"
"endfunction\n"

"function str_clear\n"
"    set $1\n"
"endfunction\n"

"function str_is_num\n"
"    isnum $1 $2\n"
"endfunction\n"

"function str_trim\n"
"    trim $1 $2\n"
"endfunction\n"

"function str_replace\n"
"    replace $1 $2 $3 $4\n"
"endfunction\n"

"function str_index\n"
"    indexof $1 $2 $3\n"
"endfunction\n"

"function str_reverse\n"
"    strrev $1 $2\n"
"endfunction\n"

"function str_count\n"
"    strcount $1 $2 $3\n"
"endfunction\n"

"function str_append\n"
"    concat $1 $1 $2\n"
"endfunction\n"

"function str_prepend\n"
"    concat $1 $2 $1\n"
"endfunction\n"

"function str_wrap\n"
"    concat _sw_tmp $2 $1\n"
"    concat $1 _sw_tmp $2\n"
"endfunction\n"

"function str_quote\n"
"    concat _sq_tmp \" $2\n"
"    concat $1 $_sq_tmp \"\n"
"endfunction\n"

"function str_bracket\n"
"    concat _sbr_tmp [ $2\n"
"    concat $1 $_sbr_tmp ]\n"
"endfunction\n"

"function str_paren\n"
"    concat _sp_tmp ( $2\n"
"    concat $1 $_sp_tmp )\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   4  BOOLEAN / ASSERTION HELPERS
   ═══════════════════════════════════════════════════════════════════ */

"% ── 4 Boolean / assertion helpers ─────────────────────────────────\n"

"function bool_not\n"
"    if $2 == 1\n"
"        set $1 0\n"
"    else\n"
"        set $1 1\n"
"    endif\n"
"endfunction\n"

"function bool_and\n"
"    if $2 == 1 && $3 == 1\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function bool_or\n"
"    if $2 == 1 || $3 == 1\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function bool_xor\n"
"    if $2 == $3\n"
"        set $1 0\n"
"    else\n"
"        set $1 1\n"
"    endif\n"
"endfunction\n"

"function bool_nand\n"
"    if $2 == 1 && $3 == 1\n"
"        set $1 0\n"
"    else\n"
"        set $1 1\n"
"    endif\n"
"endfunction\n"

"function bool_nor\n"
"    if $2 == 1 || $3 == 1\n"
"        set $1 0\n"
"    else\n"
"        set $1 1\n"
"    endif\n"
"endfunction\n"

"function bool_to_str\n"
"    if $2 == 1\n"
"        set $1 true\n"
"    else\n"
"        set $1 false\n"
"    endif\n"
"endfunction\n"

"function bool_from_str\n"
"    if $2 == true\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function assert_true\n"
"    if $1\n"
"        echo [PASS] $2\n"
"    else\n"
"        echo [FAIL] $2\n"
"        exit 1\n"
"    endif\n"
"endfunction\n"

"function assert_false\n"
"    if $1\n"
"        echo [FAIL] $2\n"
"        exit 1\n"
"    else\n"
"        echo [PASS] $2\n"
"    endif\n"
"endfunction\n"

"function assert_eq\n"
"    if $1 == $2\n"
"        echo [PASS] $3\n"
"    else\n"
"        echo [FAIL] $3 (got $1 expected $2)\n"
"        exit 1\n"
"    endif\n"
"endfunction\n"

"function assert_ne\n"
"    if $1 != $2\n"
"        echo [PASS] $3\n"
"    else\n"
"        echo [FAIL] $3 (both are $1)\n"
"        exit 1\n"
"    endif\n"
"endfunction\n"

"function assert_gt\n"
"    if $1 > $2\n"
"        echo [PASS] $3\n"
"    else\n"
"        echo [FAIL] $3 ($1 not > $2)\n"
"        exit 1\n"
"    endif\n"
"endfunction\n"

"function assert_lt\n"
"    if $1 < $2\n"
"        echo [PASS] $3\n"
"    else\n"
"        echo [FAIL] $3 ($1 not < $2)\n"
"        exit 1\n"
"    endif\n"
"endfunction\n"

"function assert_set\n"
"    str_empty _as_r $1\n"
"    if $_as_r == 1\n"
"        echo [FAIL] $2 (variable $1 is not set)\n"
"        exit 1\n"
"    else\n"
"        echo [PASS] $2\n"
"    endif\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   5  COUNTER / ACCUMULATOR
   ═══════════════════════════════════════════════════════════════════ */

"% ── 5 Counter helpers ──────────────────────────────────────────────\n"

"function counter_make\n"
"    if $2\n"
"        set $1 $2\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function counter_inc\n"
"    if $2\n"
"        math $1 $1 + $2\n"
"    else\n"
"        inc $1\n"
"    endif\n"
"endfunction\n"

"function counter_dec\n"
"    if $2\n"
"        math $1 $1 - $2\n"
"    else\n"
"        dec $1\n"
"    endif\n"
"endfunction\n"

"function counter_reset\n"
"    set $1 0\n"
"endfunction\n"

"function counter_get\n"
"    echo $1\n"
"endfunction\n"

"function counter_show\n"
"    print_kv $1 $1\n"
"endfunction\n"

"function counter_at_limit\n"
"    if $1 >= $2\n"
"        set $3 1\n"
"    else\n"
"        set $3 0\n"
"    endif\n"
"endfunction\n"


/* ═══════════════════════════════════════════════════════════════════
   6  ITERATION HELPERS
   ═══════════════════════════════════════════════════════════════════ */

"% ── 6 Iteration helpers ────────────────────────────────────────────\n"

"function repeat\n"
"    set _rp_i 0\n"
"    while $_rp_i < $1\n"
"        call $2\n"
"        inc _rp_i\n"
"    endwhile\n"
"endfunction\n"

"function range_echo\n"
"    if $3\n"
"        for _re_v in $1..$2..$3\n"
"            echo $_re_v\n"
"        endfor\n"
"    else\n"
"        for _re_v in $1..$2\n"
"            echo $_re_v\n"
"        endfor\n"
"    endif\n"
"endfunction\n"

"function range_sum\n"
"    set $1 0\n"
"    for _rs_v in $2..$3\n"
"        math $1 $1 + $_rs_v\n"
"    endfor\n"
"endfunction\n"

"function range_count\n"
"    math $1 $3 - $2\n"
"    inc $1\n"
"endfunction\n"

"function times_do\n"
"    set _td_i 0\n"
"    while $_td_i < $1\n"
"        call $2\n"
"        inc _td_i\n"
"    endwhile\n"
"endfunction\n"

"function while_lt\n"
"    while $1 < $2\n"
"        call $3\n"
"        inc $1\n"
"    endwhile\n"
"endfunction\n"

"function countdown\n"
"    set _cd_i $1\n"
"    while $_cd_i > 0\n"
"        echo $_cd_i\n"
"        dec _cd_i\n"
"    endwhile\n"
"    echo Go!\n"
"endfunction\n"

"function countup\n"
"    set _cu_i $1\n"
"    while $_cu_i <= $2\n"
"        echo $_cu_i\n"
"        inc _cu_i\n"
"    endwhile\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   7  CONTROL / ERROR HANDLING
   ═══════════════════════════════════════════════════════════════════ */

"% ── 7 Control / error handling ────────────────────────────────────\n"

"function die\n"
"    echo [FATAL] $1\n"
"    exit 1\n"
"endfunction\n"

"function die_if\n"
"    if $1\n"
"        echo [FATAL] $2\n"
"        exit 1\n"
"    endif\n"
"endfunction\n"

"function die_unless\n"
"    if $1\n"
"    else\n"
"        echo [FATAL] $2\n"
"        exit 1\n"
"    endif\n"
"endfunction\n"

"function noop\n"
"    set _noop_dummy 0\n"
"endfunction\n"

"function todo_stub\n"
"    echo [TODO] function $1 is not yet implemented\n"
"endfunction\n"

"function try_run\n"
"    run $1\n"
"    if $? != 0\n"
"        print_error Failed to run $1\n"
"    else\n"
"        print_ok Ran $1\n"
"    endif\n"
"endfunction\n"

"function safe_set\n"
"    str_empty _ss_r $1\n"
"    if $_ss_r == 1\n"
"        set $1 $2\n"
"    endif\n"
"endfunction\n"

"function require_var\n"
"    str_empty _rv_r $1\n"
"    if $_rv_r == 1\n"
"        echo [FATAL] Required variable $1 is not set\n"
"        exit 1\n"
"    endif\n"
"endfunction\n"

"function require_file\n"
"    if_exists $1 noop\n"
"    if $? != 0\n"
"        echo [FATAL] Required file $1 not found\n"
"        exit 1\n"
"    endif\n"
"endfunction\n"

"function on_error\n"
"    if $? != 0\n"
"        call $1\n"
"    endif\n"
"endfunction\n"

"function exit_ok\n"
"    exit 0\n"
"endfunction\n"

"function exit_fail\n"
"    exit 1\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   8  DEBUG / INTROSPECTION
   ═══════════════════════════════════════════════════════════════════ */

"% ── 8 Debug helpers ────────────────────────────────────────────────\n"

"function dbg_var\n"
"    echo [DBG] $1 = $1\n"
"endfunction\n"

"function dbg_all_vars\n"
"    echo [DBG] Variable dump:\n"
"    vars\n"
"endfunction\n"

"function dbg_trace\n"
"    echo [TRACE] $1\n"
"endfunction\n"

"function dump_ctx\n"
"    print_header Context Dump\n"
"    vars\n"
"endfunction\n"

"function dbg_math\n"
"    echo [DBG] $1 + $2 = ?\n"
"    math _dm_r $1 + $2\n"
"    echo [DBG] result = $_dm_r\n"
"endfunction\n"

"function dbg_str\n"
"    strlen _ds_len $1\n"
"    echo [DBG] str=$1 len=$_ds_len\n"
"endfunction\n"

"function dbg_bool\n"
"    if $1 == 1\n"
"        echo [DBG] $2 = true\n"
"    else\n"
"        echo [DBG] $2 = false\n"
"    endif\n"
"endfunction\n"

"function dbg_section\n"
"    echo\n"
"    echo [DBG] ---- $1 ----\n"
"endfunction\n"

"function dbg_assert_var\n"
"    str_empty _dav_r $1\n"
"    if $_dav_r == 1\n"
"        echo [DBG] MISSING: $1\n"
"    else\n"
"        echo [DBG] OK: $1 = $1\n"
"    endif\n"
"endfunction\n"

"function dbg_env\n"
"    print_header Debug Environment\n"
"    print_kv OSNAME $OSNAME\n"
"    print_kv VERSION $VERSION\n"
"    print_kv ARCH $ARCH\n"
"    print_kv USER $USER\n"
"    print_kv HOME $HOME\n"
"    print_kv SHELL $SHELL\n"
"    print_kv LOGLEVEL $LOGLEVEL\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   9  TYPE CHECKING
   ═══════════════════════════════════════════════════════════════════ */

"% ── 9 Type / value checks ──────────────────────────────────────────\n"

"function is_zero\n"
"    if $2 == 0\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function is_nonzero\n"
"    if $2 == 0\n"
"        set $1 0\n"
"    else\n"
"        set $1 1\n"
"    endif\n"
"endfunction\n"

"function is_positive\n"
"    if $2 > 0\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function is_negative\n"
"    if $2 < 0\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function in_range\n"
"    if $2 >= $3 && $2 <= $4\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function is_set\n"
"    str_empty _is_r $2\n"
"    if $_is_r == 1\n"
"        set $1 0\n"
"    else\n"
"        set $1 1\n"
"    endif\n"
"endfunction\n"

"function is_true\n"
"    if $2 == 1\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function is_false\n"
"    if $2 == 0\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function is_equal\n"
"    if $2 == $3\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function is_gt\n"
"    if $2 > $3\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

"function is_lt\n"
"    if $2 < $3\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   10  SYSTEM / OS HELPERS
   ═══════════════════════════════════════════════════════════════════ */

"% ── 10 System helpers ───────────────────────────────────────────────\n"

"function clear_screen\n"
"    clear\n"
"endfunction\n"

"function wait_ms\n"
"    sleep $1\n"
"endfunction\n"

"function os_info\n"
"    echo RadiumOS - i686 bare-metal kernel\n"
"    echo Shell: RSH\n"
"endfunction\n"

"function prompt_yn\n"
"    echo $2 [y/n]\n"
"    getkey _pyn_k\n"
"    if $_pyn_k == y\n"
"        set $1 y\n"
"    else\n"
"        set $1 n\n"
"    endif\n"
"endfunction\n"

"function prompt_input\n"
"    input $1 $2\n"
"endfunction\n"

"function confirm\n"
"    prompt_yn _cfm_r $1\n"
"    if $_cfm_r != y\n"
"        echo Aborted.\n"
"        exit 1\n"
"    endif\n"
"endfunction\n"

"function wait_enter\n"
"    pause\n"
"endfunction\n"

"function shell_version\n"
"    print_kv Shell $SHELL\n"
"    print_kv Version $VERSION\n"
"endfunction\n"

"function uptime_info\n"
"    print_header Uptime\n"
"    uptime\n"
"endfunction\n"

"function reboot_confirm\n"
"    confirm Reboot the system?\n"
"    reboot\n"
"endfunction\n"

"function shutdown_confirm\n"
"    confirm Shutdown the system?\n"
"    shutdown\n"
"endfunction\n"

"function run_script\n"
"    echo Running script: $1\n"
"    run $1\n"
"    print_ok Script finished: $1\n"
"endfunction\n"

"function source_if_exists\n"
"    if_exists $1 run $1\n"
"endfunction\n"

"function env_dump\n"
"    print_header Environment Variables\n"
"    vars\n"
"    print_sep\n"
"endfunction\n"

"function set_default\n"
"    str_empty _sd_r $1\n"
"    if $_sd_r == 1\n"
"        set $1 $2\n"
"        print_info Set default: $1 = $2\n"
"    endif\n"
"endfunction\n"

"function check_cmd\n"
"    which _cc_r $1\n"
"    str_empty _cc_e $_cc_r\n"
"    if $_cc_e == 1\n"
"        print_warn Command not found: $1\n"
"        set $2 0\n"
"    else\n"
"        set $2 1\n"
"    endif\n"
"endfunction\n"

"function banner_session\n"
"    clear\n"
"    echo\n"
"    echo ########################################\n"
"    echo #                                      #\n"
"    echo #   RadiumOS  //  i686 bare-metal      #\n"
"    echo #   Shell: RSH        User: $USER\n"
"    echo #                                      #\n"
"    echo ########################################\n"
"    echo\n"
"endfunction\n"

"function log_to_file\n"
"    echo [LOG] $2 >> $1\n"
"endfunction\n"

"function timestamp_echo\n"
"    echo [TS] $1\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   11  MEMORY HELPERS (New Feature Support)
   ═══════════════════════════════════════════════════════════════════ */

"% ── 11 Memory Helpers ────────────────────────────────────────────────\n"

/* mem_alloc_safe DEST SIZE  — Allocates, returns 0 on fail */
"function mem_alloc_safe\n"
"    mem_alloc $2 $1\n"
"    if $1 == 0\n"
"        print_error Memory allocation failed for $2 bytes\n"
"    endif\n"
"endfunction\n"

/* mem_write_string PTR STRING  — Writes string to memory */
"function mem_write_string\n"
"    set _mws_i 0\n"
"    set _mws_len 0\n"
"    strlen _mws_len $2\n"
"    while $_mws_i < $_mws_len\n"
"        substr _mws_ch $2 $_mws_i 1\n"
"        % Convert char to int for poke? \n"
"        % Assuming poke takes int values\n"
"        poke $1 _mws_ch\n"
"        math $1 $1 + 1\n"
"        inc _mws_i\n"
"    endwhile\n"
"    % Null terminator\n"
"    poke $1 0\n"
"endfunction\n"

/* mem_dump PTR LEN  — Dumps hex of memory region */
"function mem_dump\n"
"    set _md_i 0\n"
"    while $_md_i < $2\n"
"        peek $1 _md_val\n"
"        print_numbered $1 $_md_val\n"
"        math $1 $1 + 1\n"
"        inc _md_i\n"
"    endwhile\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   12  BOOLEAN WRAPPERS (New Feature Support)
   ═══════════════════════════════════════════════════════════════════ */

"% ── 12 Boolean Wrappers ──────────────────────────────────────────────\n"

"function bool_set_var\n"
"    bool set $1 $2\n"
"endfunction\n"

"function bool_toggle_var\n"
"    bool toggle $1\n"
"endfunction\n"

"function bool_check_var\n"
"    bool is $1\n"
"    if $? == 1\n"
"        echo $1 is true\n"
"    else\n"
"        echo $1 is false\n"
"    endif\n"
"endfunction\n"

"__~~%~~__ end of rsh:main\n";

avfs_create_file("/bin/rsh:main", strlen(rshmain));
avfs_write_file("/bin/rsh:main", rshmain, strlen(rshmain), 0);
done("RSH:MAIN standard library loaded", "rsh:main");


    // RSH:MATH library
    const char* rshmath =

"__~~%~~__ RSH Math Library\n"
"__~~%~~__ Include with: ^include \"rsh:math\"\n"
"__~~%~~__ RadiumOS / RSH  (i686 bare-metal)\n"

/* ═══════════════════════════════════════════════════════════════════
   1  BASIC ARITHMETIC
   ═══════════════════════════════════════════════════════════════════ */

"% ── 1 Basic arithmetic ─────────────────────────────────────────────\n"

/* add DEST A B */
"function add\n"
"    math $1 $2 + $3\n"
"endfunction\n"

/* sub DEST A B */
"function sub\n"
"    math $1 $2 - $3\n"
"endfunction\n"

/* mul DEST A B */
"function mul\n"
"    math $1 $2 * $3\n"
"endfunction\n"

/* div DEST A B  — safe, prints error on /0 */
"function div\n"
"    if $3 == 0\n"
"        echo [MATH ERROR] Division by zero\n"
"        set $1 0\n"
"    else\n"
"        math $1 $2 / $3\n"
"    endif\n"
"endfunction\n"

/* mod DEST A B  — safe */
"function mod\n"
"    if $3 == 0\n"
"        echo [MATH ERROR] Modulo by zero\n"
"        set $1 0\n"
"    else\n"
"        math $1 $2 % $3\n"
"    endif\n"
"endfunction\n"

/* inc1 DEST VAL  — DEST = VAL + 1 */
"function inc1\n"
"    math $1 $2 + 1\n"
"endfunction\n"

/* dec1 DEST VAL  — DEST = VAL - 1 */
"function dec1\n"
"    math $1 $2 - 1\n"
"endfunction\n"

/* negate DEST VAL  — DEST = 0 - VAL */
"function negate\n"
"    math $1 0 - $2\n"
"endfunction\n"

/* double DEST VAL */
"function double\n"
"    math $1 $2 * 2\n"
"endfunction\n"

/* halve DEST VAL  — integer division by 2 */
"function halve\n"
"    math $1 $2 / 2\n"
"endfunction\n"

/* triple DEST VAL */
"function triple\n"
"    math $1 $2 * 3\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   2  POWERS AND ROOTS
   ═══════════════════════════════════════════════════════════════════ */

"% ── 2 Powers and roots ─────────────────────────────────────────────\n"

/* square DEST VAL */
"function square\n"
"    math $1 $2 * $2\n"
"endfunction\n"

/* cube DEST VAL */
"function cube\n"
"    math _cb_sq $2 * $2\n"
"    math $1 $_cb_sq * $2\n"
"endfunction\n"

/* pow DEST BASE EXP  — integer exponentiation */
"function pow\n"
"    set $1 1\n"
"    set _pw_i 0\n"
"    while $_pw_i < $3\n"
"        math $1 $1 * $2\n"
"        inc _pw_i\n"
"    endwhile\n"
"endfunction\n"

/* pow2 DEST EXP  — 2^EXP */
"function pow2\n"
"    set $1 1\n"
"    set _p2_i 0\n"
"    while $_p2_i < $2\n"
"        math $1 $1 * 2\n"
"        inc _p2_i\n"
"    endwhile\n"
"endfunction\n"

/* pow10 DEST EXP  — 10^EXP */
"function pow10\n"
"    set $1 1\n"
"    set _p10_i 0\n"
"    while $_p10_i < $2\n"
"        math $1 $1 * 10\n"
"        inc _p10_i\n"
"    endwhile\n"
"endfunction\n"

/* isqrt DEST VAL  — integer square root via Newton's method */
"function isqrt\n"
"    if $2 == 0\n"
"        set $1 0\n"
"    else\n"
"        set _isq_x $2\n"
"        set _isq_done 0\n"
"        while $_isq_done == 0\n"
"            math _isq_x2 $2 / $_isq_x\n"
"            math _isq_x2 $_isq_x2 + $_isq_x\n"
"            math _isq_x2 $_isq_x2 / 2\n"
"            if $_isq_x2 >= $_isq_x\n"
"                set _isq_done 1\n"
"            else\n"
"                set _isq_x $_isq_x2\n"
"            endif\n"
"        endwhile\n"
"        set $1 $_isq_x\n"
"    endif\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   3  COMPARISON / PREDICATES
   ═══════════════════════════════════════════════════════════════════ */

"% ── 3 Comparison / predicates ──────────────────────────────────────\n"

/* abs DEST VAL */
"function abs\n"
"    if $2 < 0\n"
"        math $1 0 - $2\n"
"    else\n"
"        set $1 $2\n"
"    endif\n"
"endfunction\n"

/* max DEST A B */
"function max\n"
"    if $2 > $3\n"
"        set $1 $2\n"
"    else\n"
"        set $1 $3\n"
"    endif\n"
"endfunction\n"

/* min DEST A B */
"function min\n"
"    if $2 < $3\n"
"        set $1 $2\n"
"    else\n"
"        set $1 $3\n"
"    endif\n"
"endfunction\n"

/* clamp DEST VAL LO HI */
"function clamp\n"
"    set _cl_v $2\n"
"    if $_cl_v < $3\n"
"        set _cl_v $3\n"
"    endif\n"
"    if $_cl_v > $4\n"
"        set _cl_v $4\n"
"    endif\n"
"    set $1 $_cl_v\n"
"endfunction\n"

/* sign DEST VAL  — sets -1, 0, or 1 */
"function sign\n"
"    if $2 > 0\n"
"        set $1 1\n"
"    elif $2 < 0\n"
"        set $1 -1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* is_even DEST VAL */
"function is_even\n"
"    math _ie_r $2 % 2\n"
"    if $_ie_r == 0\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* is_odd DEST VAL */
"function is_odd\n"
"    math _io_r $2 % 2\n"
"    if $_io_r == 0\n"
"        set $1 0\n"
"    else\n"
"        set $1 1\n"
"    endif\n"
"endfunction\n"

/* is_divisible DEST VAL DIV */
"function is_divisible\n"
"    math _id_r $2 % $3\n"
"    if $_id_r == 0\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* in_range DEST VAL LO HI */
"function in_range\n"
"    if $2 >= $3 && $2 <= $4\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   4  NUMBER THEORY
   ═══════════════════════════════════════════════════════════════════ */

"% ── 4 Number theory ────────────────────────────────────────────────\n"

/* gcd DEST A B  — Euclidean */
"function gcd\n"
"    set _ga $2\n"
"    set _gb $3\n"
"    while $_gb != 0\n"
"        math _gt $_ga % $_gb\n"
"        set _ga $_gb\n"
"        set _gb $_gt\n"
"    endwhile\n"
"    set $1 $_ga\n"
"endfunction\n"

/* lcm DEST A B  — LCM via GCD */
"function lcm\n"
"    gcd _lcm_g $2 $3\n"
"    math _lcm_p $2 * $3\n"
"    math $1 _lcm_p / _lcm_g\n"
"endfunction\n"

/* factorial DEST N */
"function factorial\n"
"    set $1 1\n"
"    set _fc_i 2\n"
"    while $_fc_i <= $2\n"
"        math $1 $1 * $_fc_i\n"
"        inc _fc_i\n"
"    endwhile\n"
"endfunction\n"

/* fibonacci DEST N  — Nth Fibonacci number (Recursive Demo) */
"function fibonacci_rec\n"
"    if $2 == 0\n"
"        set $1 0\n"
"    elif $2 == 1\n"
"        set $1 1\n"
"    else\n"
"        math _fa_n $2 - 1\n"
"        call fibonacci_rec _fa_res $_fa_n\n"
"        set _fa $_fa_res\n"
"        \n"
"        math _fb_n $2 - 2\n"
"        call fibonacci_rec _fb_res $_fb_n\n"
"        set _fb $_fb_res\n"
"        \n"
"        math $1 $_fa + $_fb\n"
"    endif\n"
"endfunction\n"
"^allowedRecursive\n"

/* is_prime DEST N */
"function is_prime\n"
"    if $2 < 2\n"
"        set $1 0\n"
"    else\n"
"        set $1 1\n"
"        set _ip_i 2\n"
"        isqrt _ip_lim $2\n"
"        while $_ip_i <= $_ip_lim\n"
"            math _ip_r $2 % $_ip_i\n"
"            if $_ip_r == 0\n"
"                set $1 0\n"
"                set _ip_i $_ip_lim\n"
"            endif\n"
"            inc _ip_i\n"
"        endwhile\n"
"    endif\n"
"endfunction\n"

/* sum_digits DEST N  — sum of decimal digits */
"function sum_digits\n"
"    set $1 0\n"
"    set _sd_n $2\n"
"    while $_sd_n > 0\n"
"        math _sd_d $_sd_n % 10\n"
"        math $1 $1 + $_sd_d\n"
"        math _sd_n $_sd_n / 10\n"
"    endwhile\n"
"endfunction\n"

/* count_digits DEST N */
"function count_digits\n"
"    set $1 0\n"
"    set _cd_n $2\n"
"    if $_cd_n == 0\n"
"        set $1 1\n"
"    else\n"
"        while $_cd_n > 0\n"
"            math _cd_n $_cd_n / 10\n"
"            inc $1\n"
"        endwhile\n"
"    endif\n"
"endfunction\n"

/* reverse_digits DEST N */
"function reverse_digits\n"
"    set $1 0\n"
"    set _rd_n $2\n"
"    while $_rd_n > 0\n"
"        math $1 $1 * 10\n"
"        math _rd_d $_rd_n % 10\n"
"        math $1 $1 + _rd_d\n"
"        math _rd_n $_rd_n / 10\n"
"    endwhile\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   5  STATISTICS
   ═══════════════════════════════════════════════════════════════════ */

"% ── 5 Statistics ───────────────────────────────────────────────────\n"

/* average2 DEST A B */
"function average2\n"
"    math $1 $2 + $3\n"
"    math $1 $1 / 2\n"
"endfunction\n"

/* average3 DEST A B C */
"function average3\n"
"    math $1 $2 + $3\n"
"    math $1 $1 + $4\n"
"    math $1 $1 / 3\n"
"endfunction\n"

/* sum3 DEST A B C */
"function sum3\n"
"    math $1 $2 + $3\n"
"    math $1 $1 + $4\n"
"endfunction\n"

/* max3 DEST A B C */
"function max3\n"
"    max _m3_ab $2 $3\n"
"    max $1 $_m3_ab $4\n"
"endfunction\n"

/* min3 DEST A B C */
"function min3\n"
"    min _mn3_ab $2 $3\n"
"    min $1 $_mn3_ab $4\n"
"endfunction\n"

/* percent DEST PART TOTAL  — integer percent */
"function percent\n"
"    math _pct_t $2 * 100\n"
"    div $1 $_pct_t $3\n"
"endfunction\n"

/* percent_of DEST PCT TOTAL  — what is PCT% of TOTAL */
"function percent_of\n"
"    math _po_t $2 * $3\n"
"    math $1 $_po_t / 100\n"
"endfunction\n"

/* diff DEST A B  — absolute difference */
"function diff\n"
"    math _df_d $2 - $3\n"
"    abs $1 $_df_d\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   6  BITWISE OPERATIONS
   ═══════════════════════════════════════════════════════════════════ */

"% ── 6 Bitwise operations ───────────────────────────────────────────\n"

/* bit_and DEST A B */
"function bit_and\n"
"    math $1 $2 & $3\n"
"endfunction\n"

/* bit_or DEST A B */
"function bit_or\n"
"    math $1 $2 | $3\n"
"endfunction\n"

/* bit_xor DEST A B */
"function bit_xor\n"
"    math $1 $2 ^ $3\n"
"endfunction\n"

/* bit_shl DEST VAL SHIFT  — left shift */
"function bit_shl\n"
"    math $1 $2 << $3\n"
"endfunction\n"

/* bit_shr DEST VAL SHIFT  — right shift */
"function bit_shr\n"
"    math $1 $2 >> $3\n"
"endfunction\n"

/* bit_test DEST VAL BIT  — 1 if bit BIT is set in VAL */
"function bit_test\n"
"    math _bt_mask 1 << $3\n"
"    math _bt_r $2 & $_bt_mask\n"
"    if $_bt_r != 0\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* bit_set DEST VAL BIT  — set bit BIT in VAL */
"function bit_set\n"
"    math _bs_mask 1 << $3\n"
"    math $1 $2 | $_bs_mask\n"
"endfunction\n"

/* bit_clear DEST VAL BIT  — clear bit BIT in VAL */
"function bit_clear\n"
"    math _bc_mask 1 << $3\n"
"    math _bc_inv 0xFFFFFFFF ^ $_bc_mask\n"
"    math $1 $2 & $_bc_inv\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   7  CONVERSION
   ═══════════════════════════════════════════════════════════════════ */

"% ── 7 Conversion ───────────────────────────────────────────────────\n"

/* to_kb DEST BYTES */
"function to_kb\n"
"    math $1 $2 / 1024\n"
"endfunction\n"

/* to_mb DEST BYTES */
"function to_mb\n"
"    math _tmb_k $2 / 1024\n"
"    math $1 $_tmb_k / 1024\n"
"endfunction\n"

/* kb_to_bytes DEST KB */
"function kb_to_bytes\n"
"    math $1 $2 * 1024\n"
"endfunction\n"

/* mb_to_bytes DEST MB */
"function mb_to_bytes\n"
"    math _mtb_k $2 * 1024\n"
"    math $1 $_mtb_k * 1024\n"
"endfunction\n"

/* secs_to_ms DEST SECS */
"function secs_to_ms\n"
"    math $1 $2 * 1000\n"
"endfunction\n"

/* ms_to_secs DEST MS */
"function ms_to_secs\n"
"    math $1 $2 / 1000\n"
"endfunction\n"

/* mins_to_secs DEST MINS */
"function mins_to_secs\n"
"    math $1 $2 * 60\n"
"endfunction\n"

/* secs_to_mins DEST SECS */
"function secs_to_mins\n"
"    math $1 $2 / 60\n"
"endfunction\n"

/* hours_to_secs DEST HOURS */
"function hours_to_secs\n"
"    math _hts_m $2 * 60\n"
"    math $1 $_hts_m * 60\n"
"endfunction\n"

/* celsius_to_f DEST C  — integer approximation */
"function celsius_to_f\n"
"    math _ctf_t $2 * 9\n"
"    math _ctf_t $_ctf_t / 5\n"
"    math $1 $_ctf_t + 32\n"
"endfunction\n"

/* f_to_celsius DEST F */
"function f_to_celsius\n"
"    math _ftc_t $2 - 32\n"
"    math _ftc_t $_ftc_t * 5\n"
"    math $1 $_ftc_t / 9\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   8  DISPLAY / PRINT MATH
   ═══════════════════════════════════════════════════════════════════ */

"% ── 8 Display / print math ─────────────────────────────────────────\n"

/* print_math_result OP A B RESULT */
"function print_math_result\n"
"    echo $2 $1 $3 = $4\n"
"endfunction\n"

/* show_add A B */
"function show_add\n"
"    math _sa_r $1 + $2\n"
"    echo $1 + $2 = $_sa_r\n"
"endfunction\n"

/* show_sub A B */
"function show_sub\n"
"    math _ss_r $1 - $2\n"
"    echo $1 - $2 = $_ss_r\n"
"endfunction\n"

/* show_mul A B */
"function show_mul\n"
"    math _sm_r $1 * $2\n"
"    echo $1 * $2 = $_sm_r\n"
"endfunction\n"

/* show_div A B */
"function show_div\n"
"    div _sd_r $1 $2\n"
"    echo $1 / $2 = $_sd_r\n"
"endfunction\n"

/* show_mod A B */
"function show_mod\n"
"    mod _smd_r $1 $2\n"
"    echo $1 % $2 = $_smd_r\n"
"endfunction\n"

/* show_pow BASE EXP */
"function show_pow\n"
"    pow _sp_r $1 $2\n"
"    echo $1 ^ $2 = $_sp_r\n"
"endfunction\n"

/* show_fib N */
"function show_fib\n"
"    fibonacci _sf_r $1\n"
"    echo fib($1) = $_sf_r\n"
"endfunction\n"

/* show_fact N */
"function show_fact\n"
"    factorial _sfc_r $1\n"
"    echo $1! = $_sfc_r\n"
"endfunction\n"

/* show_prime N */
"function show_prime\n"
"    is_prime _spr_r $1\n"
"    if $_spr_r == 1\n"
"        echo $1 is prime\n"
"    else\n"
"        echo $1 is not prime\n"
"    endif\n"
"endfunction\n"

/* show_gcd A B */
"function show_gcd\n"
"    gcd _sg_r $1 $2\n"
"    echo gcd($1,$2) = $_sg_r\n"
"endfunction\n"

/* show_lcm A B */
"function show_lcm\n"
"    lcm _sl_r $1 $2\n"
"    echo lcm($1,$2) = $_sl_r\n"
"endfunction\n"

/* math_demo  — quick self-test / demo */
"function math_demo\n"
"    print_banner RSH Math Demo\n"
"    show_add 6 7\n"
"    show_sub 20 8\n"
"    show_mul 6 9\n"
"    show_div 100 4\n"
"    show_mod 17 5\n"
"    show_pow 2 10\n"
"    show_fib 10\n"
"    show_fact 7\n"
"    show_prime 17\n"
"    show_prime 18\n"
"    show_gcd 48 18\n"
"    show_lcm 4 6\n"
"    echo [Recursive Fibonacci Demo]\n"
"    call fibonacci_rec RESULT 10\n"
"    echo Rec Fib(10) = $RESULT\n"
"    print_sep\n"
"endfunction\n"

"__~~%~~__ end of rsh:math\n";

avfs_create_file("/bin/rsh:math", strlen(rshmath));
avfs_write_file("/bin/rsh:math", rshmath, strlen(rshmath), 0);
done("RSH:MATH library loaded", "rsh:math");
    
    // RSH:STRING library
    const char* rshstring =

"__~~%~~__ RSH String Library\n"
"__~~%~~__ Include with: ^include \"rsh:string\"\n"
"__~~%~~__ RadiumOS / RSH  (i686 bare-metal)\n"

/* ═══════════════════════════════════════════════════════════════════
   1  BASIC OPERATIONS
   ═══════════════════════════════════════════════════════════════════ */

"% ── 1 Basic operations ─────────────────────────────────────────────\n"

/* copy DEST SRC */
"function copy\n"
"    set $1 $2\n"
"endfunction\n"

/* clear VAR */
"function clear_str\n"
"    set $1\n"
"endfunction\n"

/* length DEST STR */
"function length\n"
"    strlen $1 $2\n"
"endfunction\n"

/* upper DEST SRC */
"function upper\n"
"    toupper $1 $2\n"
"endfunction\n"

/* lower DEST SRC */
"function lower\n"
"    tolower $1 $2\n"
"endfunction\n"

/* trim DEST SRC  — strip leading/trailing whitespace */
"function trim\n"
"    strtrim $1 $2\n"
"endfunction\n"

/* reverse DEST SRC */
"function reverse\n"
"    strrev $1 $2\n"
"endfunction\n"

/* append DEST STR  — DEST = DEST + STR */
"function append\n"
"    concat $1 $1 $2\n"
"endfunction\n"

/* prepend DEST STR  — DEST = STR + DEST */
"function prepend\n"
"    concat $1 $2 $1\n"
"endfunction\n"

/* join DEST A B  — DEST = A + B */
"function join\n"
"    concat $1 $2 $3\n"
"endfunction\n"

/* join3 DEST A B C  — DEST = A + B + C */
"function join3\n"
"    concat _j3_tmp $2 $3\n"
"    concat $1 $_j3_tmp $4\n"
"endfunction\n"

/* join_sep DEST A SEP B  — DEST = A + SEP + B */
"function join_sep\n"
"    concat _js_tmp $2 $3\n"
"    concat $1 $_js_tmp $4\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   2  SEARCH / TEST
   ═══════════════════════════════════════════════════════════════════ */

"% ── 2 Search / test ────────────────────────────────────────────────\n"

/* is_empty DEST STR  — 1 if empty */
"function is_empty\n"
"    strlen _ie_len $2\n"
"    if $_ie_len == 0\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* not_empty DEST STR  — 1 if not empty */
"function not_empty\n"
"    strlen _ne_len $2\n"
"    if $_ne_len > 0\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* equals DEST A B  — 1 if A == B */
"function equals\n"
"    if $2 == $3\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* not_equals DEST A B  — 1 if A != B */
"function not_equals\n"
"    if $2 != $3\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* contains DEST HAYSTACK NEEDLE  — 1 if found */
"function str_has\n"
"    contains $1 $2 $3\n"
"endfunction\n"

/* starts_with DEST STR PREFIX */
"function starts_with\n"
"    startswith $1 $2 $3\n"
"endfunction\n"

/* ends_with DEST STR SUFFIX */
"function ends_with\n"
"    endswith $1 $2 $3\n"
"endfunction\n"

/* index_of DEST HAYSTACK NEEDLE  — position or -1 */
"function index_of\n"
"    indexof $1 $2 $3\n"
"endfunction\n"

/* count_of DEST HAYSTACK NEEDLE  — occurrences */
"function count_of\n"
"    strcount $1 $2 $3\n"
"endfunction\n"

/* is_num DEST STR  — 1 if STR is a valid integer */
"function is_num\n"
"    isnum $1 $2\n"
"endfunction\n"

/* is_alpha DEST STR  — 1 if all alphabetic */
"function is_alpha\n"
"    isalpha $1 $2\n"
"endfunction\n"

/* is_alnum DEST STR  — 1 if all alphanumeric */
"function is_alnum\n"
"    isalnum $1 $2\n"
"endfunction\n"

/* is_upper DEST STR  — 1 if all uppercase */
"function is_upper\n"
"    isupper $1 $2\n"
"endfunction\n"

/* is_lower DEST STR  — 1 if all lowercase */
"function is_lower\n"
"    islower $1 $2\n"
"endfunction\n"

/* is_space DEST STR  — 1 if all whitespace */
"function is_space\n"
"    isspace $1 $2\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   3  EXTRACTION / SLICING
   ═══════════════════════════════════════════════════════════════════ */

"% ── 3 Extraction / slicing ─────────────────────────────────────────\n"

/* sub DEST SRC START LEN */
"function sub\n"
"    substr $1 $2 $3 $4\n"
"endfunction\n"

/* first DEST SRC N  — first N chars */
"function first\n"
"    substr $1 $2 0 $3\n"
"endfunction\n"

/* last DEST SRC N  — last N chars */
"function last\n"
"    strlen _la_len $2\n"
"    math _la_start $_la_len - $3\n"
"    substr $1 $2 $_la_start $3\n"
"endfunction\n"

/* char_at DEST SRC INDEX  — single char at INDEX */
"function char_at\n"
"    substr $1 $2 $3 1\n"
"endfunction\n"

/* first_char DEST SRC */
"function first_char\n"
"    substr $1 $2 0 1\n"
"endfunction\n"

/* last_char DEST SRC */
"function last_char\n"
"    strlen _lc_len $2\n"
"    math _lc_idx $_lc_len - 1\n"
"    substr $1 $2 $_lc_idx 1\n"
"endfunction\n"

/* drop_first DEST SRC N  — remove first N chars */
"function drop_first\n"
"    strlen _df_len $2\n"
"    math _df_rem $_df_len - $3\n"
"    substr $1 $2 $3 $_df_rem\n"
"endfunction\n"

/* drop_last DEST SRC N  — remove last N chars */
"function drop_last\n"
"    strlen _dl_len $2\n"
"    math _dl_keep $_dl_len - $3\n"
"    substr $1 $2 0 $_dl_keep\n"
"endfunction\n"

/* mid DEST SRC START END  — chars from START to END inclusive */
"function mid\n"
"    math _md_len $4 - $3\n"
"    inc _md_len\n"
"    substr $1 $2 $3 $_md_len\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   4  TRANSFORMATION
   ═══════════════════════════════════════════════════════════════════ */

"% ── 4 Transformation ───────────────────────────────────────────────\n"

/* replace DEST SRC FROM TO */
"function replace\n"
"    strreplace $1 $2 $3 $4\n"
"endfunction\n"

/* replace_all DEST SRC FROM TO  — replace every occurrence */
"function replace_all\n"
"    set $1 $2\n"
"    set _ra_found 1\n"
"    while $_ra_found == 1\n"
"        contains _ra_found $1 $3\n"
"        if $_ra_found == 1\n"
"            strreplace $1 $1 $3 $4\n"
"        endif\n"
"    endwhile\n"
"endfunction\n"

/* remove DEST SRC SUBSTR  — remove first occurrence of SUBSTR */
"function remove\n"
"    strreplace $1 $2 $3\n"
"endfunction\n"

/* repeat STR N  — DEST = STR repeated N times */
"function repeat\n"
"    set $1\n"
"    set _rp_i 0\n"
"    while $_rp_i < $3\n"
"        concat $1 $1 $2\n"
"        inc _rp_i\n"
"    endwhile\n"
"endfunction\n"

/* pad_right DEST STR WIDTH CHAR */
"function pad_right\n"
"    set $1 $2\n"
"    strlen _pr_len $2\n"
"    while $_pr_len < $3\n"
"        concat $1 $1 $4\n"
"        inc _pr_len\n"
"    endwhile\n"
"endfunction\n"

/* pad_left DEST STR WIDTH CHAR */
"function pad_left\n"
"    strlen _pl_len $2\n"
"    set _pl_pad\n"
"    while $_pl_len < $3\n"
"        concat _pl_pad $4 $_pl_pad\n"
"        inc _pl_len\n"
"    endwhile\n"
"    concat $1 $_pl_pad $2\n"
"endfunction\n"

/* pad_center DEST STR WIDTH CHAR */
"function pad_center\n"
"    strlen _pc_len $2\n"
"    math _pc_total $3 - $_pc_len\n"
"    math _pc_left $_pc_total / 2\n"
"    math _pc_right $_pc_total - $_pc_left\n"
"    set _pc_lpad\n"
"    set _pc_rpad\n"
"    set _pc_i 0\n"
"    while $_pc_i < $_pc_left\n"
"        concat _pc_lpad $_pc_lpad $4\n"
"        inc _pc_i\n"
"    endwhile\n"
"    set _pc_i 0\n"
"    while $_pc_i < $_pc_right\n"
"        concat _pc_rpad $_pc_rpad $4\n"
"        inc _pc_i\n"
"    endwhile\n"
"    concat _pc_tmp $_pc_lpad $2\n"
"    concat $1 _pc_tmp $_pc_rpad\n"
"endfunction\n"

/* wrap DEST STR WRAP_CHAR  — surround with WRAP_CHAR */
"function wrap\n"
"    concat _wr_tmp $3 $2\n"
"    concat $1 $_wr_tmp $3\n"
"endfunction\n"

/* wrap2 DEST STR OPEN CLOSE  — surround with different chars */
"function wrap2\n"
"    concat _w2_tmp $3 $2\n"
"    concat $1 $_w2_tmp $4\n"
"endfunction\n"

/* quote DEST STR  — wrap in double quotes */
"function quote\n"
"    concat _qt_tmp \" $2\n"
"    concat $1 $_qt_tmp \"\n"
"endfunction\n"

/* bracket DEST STR  — wrap in [] */
"function bracket\n"
"    concat _bk_tmp [ $2\n"
"    concat $1 $_bk_tmp ]\n"
"endfunction\n"

/* paren DEST STR  — wrap in () */
"function paren\n"
"    concat _pa_tmp ( $2\n"
"    concat $1 $_pa_tmp )\n"
"endfunction\n"

/* brace DEST STR  — wrap in {} */
"function brace\n"
"    concat _br_tmp { $2\n"
"    concat $1 $_br_tmp }\n"
"endfunction\n"

/* angle DEST STR  — wrap in <> */
"function angle\n"
"    concat _an_tmp < $2\n"
"    concat $1 $_an_tmp >\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   5  FORMATTING / DISPLAY
   ═══════════════════════════════════════════════════════════════════ */

"% ── 5 Formatting / display ─────────────────────────────────────────\n"

/* print_str STR  — echo with label */
"function print_str\n"
"    echo $1\n"
"endfunction\n"

/* print_str_kv KEY STR */
"function print_str_kv\n"
"    echo $1 : $2\n"
"endfunction\n"

/* print_str_len STR  — print string and its length */
"function print_str_len\n"
"    strlen _psl_len $1\n"
"    echo $1 (len=$_psl_len)\n"
"endfunction\n"

/* print_upper STR */
"function print_upper\n"
"    toupper _pu_tmp $1\n"
"    echo $_pu_tmp\n"
"endfunction\n"

/* print_lower STR */
"function print_lower\n"
"    tolower _pl_tmp $1\n"
"    echo $_pl_tmp\n"
"endfunction\n"

/* print_reversed STR */
"function print_reversed\n"
"    strrev _prs_tmp $1\n"
"    echo $_prs_tmp\n"
"endfunction\n"

/* print_quoted STR */
"function print_quoted\n"
"    quote _pq_tmp $1\n"
"    echo $_pq_tmp\n"
"endfunction\n"

/* print_bracketed STR */
"function print_bracketed\n"
"    bracket _pb_tmp $1\n"
"    echo $_pb_tmp\n"
"endfunction\n"

/* print_padded STR WIDTH  — right-padded with spaces */
"function print_padded\n"
"    pad_right _pp_tmp $1 $2  \n"
"    echo $_pp_tmp\n"
"endfunction\n"

/* print_centered STR WIDTH */
"function print_centered\n"
"    pad_center _pce_tmp $1 $2  \n"
"    echo $_pce_tmp\n"
"endfunction\n"

/* str_inspect VAR  — full inspection dump */
"function str_inspect\n"
"    print_header String Inspect\n"
"    print_str_kv Name $1\n"
"    print_str_kv Value $1\n"
"    strlen _si_len $1\n"
"    print_str_kv Length $_si_len\n"
"    startswith _si_sw $1 R\n"
"    print_str_kv StartsWithR $_si_sw\n"
"    endswith _si_ew $1 S\n"
"    print_str_kv EndsWithS $_si_ew\n"
"    is_num _si_num $1\n"
"    print_str_kv IsNumeric $_si_num\n"
"    is_alpha _si_alp $1\n"
"    print_str_kv IsAlpha $_si_alp\n"
"    print_sep\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   6  SPLITTING / TOKENISING
   ═══════════════════════════════════════════════════════════════════ */

"% ── 6 Splitting / tokenising ───────────────────────────────────────\n"

/* split_head DEST STR SEP  — part before first SEP */
"function split_head\n"
"    indexof _sh_idx $2 $3\n"
"    if $_sh_idx == -1\n"
"        set $1 $2\n"
"    else\n"
"        substr $1 $2 0 $_sh_idx\n"
"    endif\n"
"endfunction\n"

/* split_tail DEST STR SEP  — part after first SEP */
"function split_tail\n"
"    indexof _st_idx $2 $3\n"
"    if $_st_idx == -1\n"
"        set $1\n"
"    else\n"
"        strlen _st_seplen $3\n"
"        math _st_start $_st_idx + $_st_seplen\n"
"        strlen _st_total $2\n"
"        math _st_rem $_st_total - $_st_start\n"
"        substr $1 $2 $_st_start $_st_rem\n"
"    endif\n"
"endfunction\n"

/* word_count DEST STR  — count space-separated words */
"function word_count\n"
"    strcount _wc_spaces $2  \n"
"    math $1 $_wc_spaces + 1\n"
"    strlen _wc_len $2\n"
"    if $_wc_len == 0\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   7  VALIDATION
   ═══════════════════════════════════════════════════════════════════ */

"% ── 7 Validation ───────────────────────────────────────────────────\n"

/* validate_nonempty VAR LABEL  — print ok/warn */
"function validate_nonempty\n"
"    strlen _vne_len $1\n"
"    if $_vne_len == 0\n"
"        echo [WARN] $2 must not be empty\n"
"    else\n"
"        echo [OK] $2 is set\n"
"    endif\n"
"endfunction\n"

/* validate_minlen VAR MIN LABEL */
"function validate_minlen\n"
"    strlen _vml_len $1\n"
"    if $_vml_len < $2\n"
"        echo [WARN] $3 too short (min $2)\n"
"    else\n"
"        echo [OK] $3 length ok\n"
"    endif\n"
"endfunction\n"

/* validate_maxlen VAR MAX LABEL */
"function validate_maxlen\n"
"    strlen _vmx_len $1\n"
"    if $_vmx_len > $2\n"
"        echo [WARN] $3 too long (max $2)\n"
"    else\n"
"        echo [OK] $3 length ok\n"
"    endif\n"
"endfunction\n"

/* validate_is_num VAR LABEL */
"function validate_is_num\n"
"    isnum _vin_r $1\n"
"    if $_vin_r == 1\n"
"        echo [OK] $2 is numeric\n"
"    else\n"
"        echo [WARN] $2 is not numeric\n"
"    endif\n"
"endfunction\n"

/* validate_starts VAR PREFIX LABEL */
"function validate_starts\n"
"    startswith _vs_r $1 $2\n"
"    if $_vs_r == 1\n"
"        echo [OK] $3 starts with $2\n"
"    else\n"
"        echo [WARN] $3 does not start with $2\n"
"    endif\n"
"endfunction\n"

/* validate_ends VAR SUFFIX LABEL */
"function validate_ends\n"
"    endswith _ve_r $1 $2\n"
"    if $_ve_r == 1\n"
"        echo [OK] $3 ends with $2\n"
"    else\n"
"        echo [WARN] $3 does not end with $2\n"
"    endif\n"
"endfunction\n"

/* validate_contains VAR NEEDLE LABEL */
"function validate_contains\n"
"    contains _vc_r $1 $2\n"
"    if $_vc_r == 1\n"
"        echo [OK] $3 contains $2\n"
"    else\n"
"        echo [WARN] $3 does not contain $2\n"
"    endif\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   8  BUILDING / TEMPLATING
   ═══════════════════════════════════════════════════════════════════ */

"% ── 8 Building / templating ────────────────────────────────────────\n"

/* build_path DEST A B  — join with / */
"function build_path\n"
"    concat _bp_tmp $2 /\n"
"    concat $1 $_bp_tmp $3\n"
"endfunction\n"

/* build_path3 DEST A B C */
"function build_path3\n"
"    concat _bp3_ab $2 /\n"
"    concat _bp3_ab $_bp3_ab $3\n"
"    concat _bp3_ab $_bp3_ab /\n"
"    concat $1 $_bp3_ab $4\n"
"endfunction\n"

/* build_label DEST PREFIX VALUE  — PREFIX_VALUE */
"function build_label\n"
"    concat _bl_tmp $2 _\n"
"    concat $1 $_bl_tmp $3\n"
"endfunction\n"

/* build_tag DEST TAG VALUE  — [TAG] VALUE */
"function build_tag\n"
"    concat _bt_tmp [ $2\n"
"    concat _bt_tmp $_bt_tmp ]\n"
"    concat _bt_tmp $_bt_tmp  \n"
"    concat $1 $_bt_tmp $3\n"
"endfunction\n"

/* build_kv DEST KEY VALUE  — KEY=VALUE */
"function build_kv\n"
"    concat _bkv_tmp $2 =\n"
"    concat $1 $_bkv_tmp $3\n"
"endfunction\n"

/* build_csv DEST A B  — A,B */
"function build_csv\n"
"    concat _bc_tmp $2 ,\n"
"    concat $1 $_bc_tmp $3\n"
"endfunction\n"

/* build_csv3 DEST A B C */
"function build_csv3\n"
"    concat _bc3_tmp $2 ,\n"
"    concat _bc3_tmp $_bc3_tmp $3\n"
"    concat _bc3_tmp $_bc3_tmp ,\n"
"    concat $1 $_bc3_tmp $4\n"
"endfunction\n"

/* build_line DEST STR  — STR + newline marker */
"function build_line\n"
"    concat $1 $2 \\n\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   9  COMPARISON UTILITIES
   ═══════════════════════════════════════════════════════════════════ */

"% ── 9 Comparison utilities ─────────────────────────────────────────\n"

/* str_lt DEST A B  — 1 if A < B lexicographically */
"function str_lt\n"
"    strcmp _slt_r $2 $3\n"
"    if $_slt_r < 0\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* str_gt DEST A B */
"function str_gt\n"
"    strcmp _sgt_r $2 $3\n"
"    if $_sgt_r > 0\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* str_eq DEST A B */
"function str_eq\n"
"    if $2 == $3\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* str_ne DEST A B */
"function str_ne\n"
"    if $2 != $3\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* str_longer DEST A B  — 1 if A is longer than B */
"function str_longer\n"
"    strlen _slo_la $2\n"
"    strlen _slo_lb $3\n"
"    if $_slo_la > $_slo_lb\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* str_shorter DEST A B  — 1 if A is shorter than B */
"function str_shorter\n"
"    strlen _ssh_la $2\n"
"    strlen _ssh_lb $3\n"
"    if $_ssh_la < $_ssh_lb\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* str_same_len DEST A B */
"function str_same_len\n"
"    strlen _ssl_la $2\n"
"    strlen _ssl_lb $3\n"
"    if $_ssl_la == $_ssl_lb\n"
"        set $1 1\n"
"    else\n"
"        set $1 0\n"
"    endif\n"
"endfunction\n"

/* ═══════════════════════════════════════════════════════════════════
   10  SELF-TEST / DEMO
   ═══════════════════════════════════════════════════════════════════ */

"% ── 10 Self-test / demo ────────────────────────────────────────────\n"

/* string_demo  — exercises the library */
"function string_demo\n"
"    print_banner RSH String Demo\n"
"    set _demo_s RadiumOS\n"
"    print_str_kv Input $_demo_s\n"
"    upper _demo_up $_demo_s\n"
"    print_str_kv Upper $_demo_up\n"
"    lower _demo_lo $_demo_s\n"
"    print_str_kv Lower $_demo_lo\n"
"    length _demo_len $_demo_s\n"
"    print_str_kv Length $_demo_len\n"
"    reverse _demo_rev $_demo_s\n"
"    print_str_kv Reversed $_demo_rev\n"
"    first _demo_f $_demo_s 6\n"
"    print_str_kv First6 $_demo_f\n"
"    last _demo_l $_demo_s 2\n"
"    print_str_kv Last2 $_demo_l\n"
"    starts_with _demo_sw $_demo_s R\n"
"    print_str_kv StartsWithR $_demo_sw\n"
"    ends_with _demo_ew $_demo_s S\n"
"    print_str_kv EndsWithS $_demo_ew\n"
"    str_has _demo_has $_demo_s ium\n"
"    print_str_kv ContainsIum $_demo_has\n"
"    pad_right _demo_pr $_demo_s 16 -\n"
"    print_str_kv PadRight $_demo_pr\n"
"    pad_left _demo_pl $_demo_s 16 -\n"
"    print_str_kv PadLeft $_demo_pl\n"
"    pad_center _demo_pc $_demo_s 16 -\n"
"    print_str_kv PadCenter $_demo_pc\n"
"    quote _demo_q $_demo_s\n"
"    print_str_kv Quoted $_demo_q\n"
"    bracket _demo_bk $_demo_s\n"
"    print_str_kv Bracketed $_demo_bk\n"
"    build_path _demo_bp /home thorne\n"
"    print_str_kv Path $_demo_bp\n"
"    build_kv _demo_kv OS $_demo_s\n"
"    print_str_kv KV $_demo_kv\n"
"    str_inspect $_demo_s\n"
"    print_sep\n"
"endfunction\n"

"__~~%~~__ end of rsh:string\n";

avfs_create_file("/bin/rsh:string", strlen(rshstring));
avfs_write_file("/bin/rsh:string", rshstring, strlen(rshstring), 0);
done("RSH:STRING library loaded", "rsh:string");
  


// Sample RSH script: Hardware RNG (PIT Channel 2 Reader) - UPDATED
const char* hello_script = 
    "% Hardware Random Number Generator\n"
    "% Source: PIT Channel 2 (Port 0x42)\n"
    "% Reads the current timer tick count as entropy.\n"
    "% Updated to demonstrate Memory Management features\n"
    "\n"
    "function rng_get\n"
    "    % PIT Command Port is 0x43 (67 decimal)\n"
    "    % PIT Channel 2 Data Port is 0x42 (66 decimal)\n"
    "\n"
    "    % 1. Send Latch Command (0xC2 = 194 decimal) to Port 0x43\n"
    "    %    0xC2 = 11000010b (Latch Channel 2)\n"
    "    outb 67 194\n"
    "\n"
    "    % 2. Read Low Byte (LSB) from Port 0x42\n"
    "    %    Syntax: inb DEST_VAR PORT\n"
    "    inb RAND_L 66\n"
    "\n"
    "    % 3. Read High Byte (MSB) from Port 0x42\n"
    "    inb RAND_H 66\n"
    "\n"
    "    % 4. Combine them for display\n"
    "    echo \"Random HEX: $RAND_H$RAND_L\"\n"
    "endfunction\n"
    "\n"
    "function main\n"
    "    echo \"========================================\"\n"
    "    echo \"    Hardware RNG Utility (PIT Ch 2)    \"\n"
    "    echo \"========================================\"\n"
    "    echo \"Instructions:\"\n"
    "    echo \"  Press [SPACE] to generate a number.\"\n"
    "    echo \"  Press [M] to run memory test.\"\n"
    "    echo \"  Press [ESC] to quit.\"\n"
    "    echo\n"
    "\n"
    "    % Main Event Loop\n"
    "    for i in 1..10000\n"
    "        \n"
    "        % Poll keyboard non-blocking\n"
    "        poll_scancode KEY\n"
    "\n"
    "        % Check for ESC (Scancode 1)\n"
    "        if $KEY == 1\n"
    "            echo\n"
    "            echo \"Exiting RNG utility...\"\n"
    "            return\n"
    "        endif\n"
    "\n"
    "        % Check for SPACE (Scancode 57)\n"
    "        if $KEY == 57\n"
    "            call rng_get\n"
    "            \n"
    "            % Simple Debounce\n"
    "            while $KEY == 57\n"
    "                poll_scancode KEY\n"
    "            endwhile\n"
    "        endif\n"
    "        \n"
    "        % Tiny sleep to prevent 100% CPU usage in the loop\n"
    "        sleep 10\n"
    "    endfor\n"
    "endfunction\n"
    "\n"
    "% Start the tool\n"
    "call main\n";

avfs_create_file("/home/user/scripts/hw_rng.rsh", strlen(hello_script));
avfs_write_file("/home/user/scripts/hw_rng.rsh", hello_script, strlen(hello_script), 0);
done("Created HW RNG script", "hw_rng.rsh");


const char* MAP = ""
"^entrypoint\n"
"function main\n"
"# ==========================================\n"
"# 1. Display a Welcome Message\n"
"# ==========================================\n"
"echo \"---------------------------------------\"\n"
"echo \"  RadiumOS Scripting: MAPs & Loops\"\n"
"echo \"  (Updated for New Features)\n"
"echo \"---------------------------------------\"\n"

"% New: Use Boolean for state\n"
"bool set MAP_LOADED false\n"

"# ==========================================\n"
"# 2. One-Time Initialization of MAP\n"
"# ==========================================\n"
"ont function init_maps\n"
"    echo [INIT] Populating Constant Map...\n"
"    const MAP BIOS_INFO\n"
"        CPUArch    : x86_64\n"
"        KernelVer  : 0.1.0-alpha\n"
"        BootTime   : 42ms\n"
"    endMAP\n"
"    bool set MAP_LOADED true\n"
"    echo [INIT] Maps initialized.\n"
"endfunction\n"
"call init_maps\n"

"if $MAP_LOADED == true\n"
"    echo \"[1] Constant Map Loaded via ONT function.\"\n"
"else\n"
"    echo \"[1] ERROR: Map not loaded.\"\n"
"endif\n"

"# ==========================================\n"
"# 3. Create an \"Editable\" Map\n"
"# ==========================================\n"
"editable MAP PLAYER_STATS\n"
"    Name       : Wanderer\n"
"    Health     : 100\n"
"    Mana       : 50\n"
"    Level      : 1\n"
"endMAP\n"
"echo \"[2] Created Editable Map: PLAYER_STATS\"\n"
"# ==========================================\n"
"# 4. Display Map Contents\n"
"# ==========================================\n"
"echo \"\"\n"
"echo \"--- Reading BIOS_INFO ---\"\n"
"set key CPUArch\n"
"echo \"  BIOS_INFO[$key] = \" . BIOS_INFO[$key]\n"
"echo \"\"\n"
"echo \"--- Reading PLAYER_STATS (Initial) ---\"\n"
"echo \"  Name   : \" . PLAYER_STATS[\"Name\"]\n"
"echo \"  Health : \" . PLAYER_STATS[\"Health\"]\n"
"# ==========================================\n"
"# 5. Edit the Map (Using explicit map names)\n"
"# ==========================================\n"
"echo \"\"\n"
"echo \"[3] Editing Player Stats...\"\n"
"# Syntax: edit.MAP [MAP_NAME] [KEY] [VALUE]\n"
"edit.MAP PLAYER_STATS Health 80\n"
"echo \"  -> Set Health to 80\"\n"
"edit.MAP PLAYER_STATS Mana 120\n"
"echo \"  -> Set Mana to 120\"\n"
"edit.MAP PLAYER_STATS Class \"Battle Mage\"\n"
"echo \"  -> Added new Key 'Class'\"\n"
"# ==========================================\n"
"# 6. Display Updated Map\n"
"# ==========================================\n"
"echo \"\"\n"
"echo \"--- Reading PLAYER_STATS (Updated) ---\"\n"
"echo \"  Health : \" . PLAYER_STATS[\"Health\"]\n"
"echo \"  Class  : \" . PLAYER_STATS[\"Class\"]\n"
"# ==========================================\n"
"# 7. Save Map to File (Using explicit map name)\n"
"# ==========================================\n"
"echo \"\"\n"
"echo \"[4] Saving PLAYER_STATS to disk...\"\n"
"# Syntax: save.MAP [MAP_NAME] [PATH]\n"
"save.MAP PLAYER_STATS \"/tmp/player_save.rsh\"\n"
"echo \"    Saved to /tmp/player_save.rsh\"\n"
"# ==========================================\n"
"# 8. Loop demonstration\n"
"# ==========================================\n"
"echo \"\"\n"
"echo \"[5] Counting Loop Demo:\"\n"
"for i in 1..5\n"
"    echo \"  Countdown loop iteration: \" . $i\n"
"endfor\n"
"echo \"\"\n"
"echo \"Script Execution Complete.\"\n"
"endfunction\n"
"call main";

// Create the file using the AVFS
avfs_create_file("/home/user/scripts/MAP.rsh", strlen(MAP));
avfs_write_file("/home/user/scripts/MAP.rsh", MAP, strlen(MAP), 0);
done("Created MAP script with explicit targets", "MAP.rsh");




// Calculator script
const char* calc_script = 
    "% Interactive Calculator Script\n"
    "% Demonstrates: poll_scancode, while loops, math, substr, bool state\n"
    "\n"
    "function main\n"
    "    echo \"=================================\"\n"
    "    echo \"     Interactive RSH Calculator  \"\n"
    "    echo \"=================================\"\n"
    "    echo \"Type an expression (e.g., 10 + 5)\"\n"
    "    echo \"Press ENTER to evaluate\"\n"
    "    echo \"Press ESC to quit\"\n"
    "    echo \"> \"\n"
    "\n"
    "    % Initialize variables\n"
    "    set BUFFER \"\"\n"
    "    set CHAR \"\"\n"
    "    set LEN 0\n"
    "    set RESULT 0\n"
    "    set TEMP \"\"\n"
    "    set TYPED 0  % Flag to track if user has typed anything\n"
    "    % New: Boolean for Error State\n"
    "    bool set CALC_ERROR false\n"
    "\n"
    "    % Scancode Constants\n"
    "    set ESC_CODE 1\n"
    "    set ENTER_CODE 28\n"
    "    set BKSP_CODE 14\n"
    "\n"
    "    % Main Input Loop\n"
    "    while 1 == 1\n"
    "        \n"
    "        % 1. Poll Keyboard\n"
    "        poll_scancode KEY\n"
    "\n"
    "        % 2. Check for Exit (ESC)\n"
    "        if $KEY == $ESC_CODE\n"
    "            echo \"\"\n"
    "            echo \"Exiting Calculator.\"\n"
    "            return\n"
    "        endif\n"
    "\n"
    "        % 3. Check for Execution (ENTER)\n"
    "        if $KEY == $ENTER_CODE\n"
    "            % Only evaluate if we actually typed something\n"
    "            if $TYPED == 1\n"
    "                echo \"\"\n"
    "                \n"
    "                % Usage: math VAR EXPR\n"
    "                math RESULT $BUFFER\n"
    "                \n"
    "                % Check if math command errored (simplified check)\n"
    "                if $RESULT == \"Error\" || $RESULT == \"\"\n"
    "                    bool set CALC_ERROR true\n"
    "                else\n"
    "                    bool set CALC_ERROR false\n"
    "                    echo \"Result: $RESULT\"\n"
    "                endif\n"
    "                \n"
    "                echo \"---------------------------------\"\n"
    "            else\n"
    "                echo \"\"\n"
    "            endif\n"
    "            \n"
    "            % Reset Buffer and State\n"
    "            set BUFFER \"\"\n"
    "            set TYPED 0\n"
    "            echo \"> \"\n"
    "            \n"
    "            % Debounce ENTER (Wait for release)\n"
    "            while $KEY == $ENTER_CODE\n"
    "                poll_scancode KEY\n"
    "            endwhile\n"
    "            continue\n"
    "        endif\n"
    "\n"
    "        % 4. Check for Backspace\n"
    "        if $KEY == $BKSP_CODE\n"
    "            % Usage: strlen DEST SRC (Pass variable name)\n"
    "            strlen LEN BUFFER\n"
    "            if $LEN > 0\n"
    "                % Decrement length (LEN - 1)\n"
    "                math LEN $LEN - 1\n"
    "                % Extract substring minus the last character\n"
    "                % Usage: substr DEST SRC START LEN (Pass variable name)\n"
    "                substr TEMP BUFFER 0 $LEN\n"
    "                set BUFFER $TEMP\n"
    "                % Update TYPED flag if buffer is empty\n"
    "                strlen LEN BUFFER\n"
    "                if $LEN == 0\n"
    "                    set TYPED 0\n"
    "                endif\n"
    "            endif\n"
    "\n"
    "            % Debounce BKSP (Wait for release)\n"
    "            while $KEY == $BKSP_CODE\n"
    "                poll_scancode KEY\n"
    "            endwhile\n"
    "            continue\n"
    "        endif\n"
    "\n"
    "        % 5. Map Scancodes to characters\n"
    "        set CHAR \"\"\n"
    "\n"
    "        % Numbers (Top Row)\n"
    "        if $KEY == 2  set CHAR \"1\" endif\n"
    "        if $KEY == 3  set CHAR \"2\" endif\n"
    "        if $KEY == 4  set CHAR \"3\" endif\n"
    "        if $KEY == 5  set CHAR \"4\" endif\n"
    "        if $KEY == 6  set CHAR \"5\" endif\n"
    "        if $KEY == 7  set CHAR \"6\" endif\n"
    "        if $KEY == 8  set CHAR \"7\" endif\n"
    "        if $KEY == 9  set CHAR \"8\" endif\n"
    "        if $KEY == 10 set CHAR \"9\" endif\n"
    "        if $KEY == 11 set CHAR \"0\" endif\n"
    "\n"
    "        % Operators\n"
    "        if $KEY == 13 set CHAR \"+\" endif\n"
    "        if $KEY == 12 set CHAR \"-\" endif\n"
    "        if $KEY == 40 set CHAR \"/\" endif\n"
    "        if $KEY == 26 set CHAR \"*\" endif\n"
    "        if $KEY == 39 set CHAR \" \" endif\n"
    "        if $KEY == 51 set CHAR \",\" endif\n"
    "        if $KEY == 52 set CHAR \".\" endif\n"
    "\n"
    "        % Numpad Keys (If NumLock is active)\n"
    "        if $KEY == 69 set CHAR \"1\" endif\n"
    "        if $KEY == 98 set CHAR \"2\" endif\n"
    "        if $KEY == 70 set CHAR \"3\" endif\n"
    "        if $KEY == 71 set CHAR \"4\" endif\n"
    "        if $KEY == 72 set CHAR \"5\" endif\n"
    "        if $KEY == 73 set CHAR \"6\" endif\n"
    "        if $KEY == 55 set CHAR \"7\" endif\n"
    "        if $KEY == 80 set CHAR \"8\" endif\n"
    "        if $KEY == 81 set CHAR \"9\" endif\n"
    "        if $KEY == 82 set CHAR \"0\" endif\n"
    "\n"
    "        % Numpad Operators\n"
    "        if $KEY == 83 set CHAR \".\" endif\n"
    "        if $KEY == 74 set CHAR \"-\" endif\n"
    "        if $KEY == 78 set CHAR \"+\" endif\n"
    "        if $KEY == 53 set CHAR \"/\" endif\n"
    "        if $KEY == 55 set CHAR \"*\" endif\n"
    "\n"
    "        % 6. Append and Print\n"
    "        if $CHAR != \"\"\n"
    "            concat BUFFER $CHAR\n"
    "            set TYPED 1\n"
    "            print $CHAR\n"
    "        endif\n"
    "        \n"
    "        % Debounce Input (Wait for key release)\n"
    "        while $KEY > 1\n"
    "            poll_scancode KEY\n"
    "        endwhile\n"
    "        \n"
    "        sleep 1\n"
    "    endwhile\n"
"endfunction\n"
    "\n"
"% Start the calculator\n"
"call main\n";
    
    avfs_create_file("/home/user/scripts/calc.rsh", strlen(calc_script));
    avfs_write_file("/home/user/scripts/calc.rsh", calc_script, strlen(calc_script), 0);
    done("Created calculator script", "calc.rsh");


    // File lister script
    const char* lister_script = 
        "% Directory Listing Script\n"
        "% Lists files with formatting\n"
        "\n"
        "^include \"rsh:main\"\n"
        "\n"
        "call print_banner \"File Listing\"\n"
        "\n"
        "echo Current directory contents:\n"
        "echo\n"
        "\n"
        "% This would list files\n"
        "echo [DIR]  documents/\n"
        "echo [DIR]  scripts/\n"
        "echo [DIR]  projects/\n"
        "echo [FILE] welcome.txt\n"
        "echo [FILE] hello.rsh\n";
    
    avfs_create_file("/home/user/scripts/lister.rsh", strlen(lister_script));
    avfs_write_file("/home/user/scripts/lister.rsh", lister_script, strlen(lister_script), 0);
    done("Created lister script", "lister.rsh");
    
    // System info script
    const char* sysinfo_script = 
        "% System Information Display\n"
        "\n"
        "^include \"rsh:main\"\n"
        "\n"
        "call print_banner \"RadiumOS System Information\"\n"
        "\n"
        "echo OS: RadiumOS v1.0\n"
        "echo Kernel: Radium Kernel\n"
        "echo Shell: RSH (RadiumOS Shell)\n"
        "echo\n"
        "\n"
        "echo Network Configuration:\n"
        "echo   IP: 192.168.1.100\n"
        "echo   Gateway: 192.168.1.1\n"
        "echo   DNS: 8.8.8.8\n"
        "echo\n"
        "\n"
        "echo Filesystem:\n"
        "echo   Type: AVFS\n"
        "echo   Root: /\n"
        "echo\n"
        "\n"
        "call print_success \"System check complete\"\n";
    
    avfs_create_file("/home/user/scripts/sysinfo.rsh", strlen(sysinfo_script));
    avfs_write_file("/home/user/scripts/sysinfo.rsh", sysinfo_script, strlen(sysinfo_script), 0);
    done("Created system info script", "sysinfo.rsh");
    
    // Project README
    const char* project_readme = 
        "Projects Directory\n"
        "==================\n"
        "\n"
        "This directory is for your development projects.\n"
        "\n"
        "Suggested structure:\n"
        "  /home/user/projects/myproject/\n"
        "    ├── src/          - Source files\n"
        "    ├── docs/         - Documentation\n"
        "    ├── tests/        - Test files\n"
        "    └── README.txt    - Project readme\n"
        "\n"
        "Start your next project here!\n";
    
    avfs_create_file("/home/user/projects/README.txt", strlen(project_readme));
    avfs_write_file("/home/user/projects/README.txt", project_readme, strlen(project_readme), 0);
    done("Created projects README", "projects/README.txt");
    
    // ===== LOG FILES (/var/log) =====
    
    const char* boot_log = 
        "[BOOT] RadiumOS v1.0 starting...\n"
        "[BOOT] Initializing kernel...\n"
        "[BOOT] Loading AVFS filesystem...\n"
        "[BOOT] Mounting root filesystem...\n"
        "[BOOT] Starting network services...\n"
        "[BOOT] Loading shell environment...\n"
        "[BOOT] System ready!\n";
    
    avfs_create_file("/var/log/boot.log", strlen(boot_log));
    avfs_write_file("/var/log/boot.log", boot_log, strlen(boot_log), 0);
    done("Created boot log", "boot.log");
    
    const char* system_log = 
        "[INFO] System initialized\n"
        "[INFO] User logged in: root\n";
    
    avfs_create_file("/var/log/system.log", strlen(system_log));
    avfs_write_file("/var/log/system.log", system_log, strlen(system_log), 0);
    done("Created system log", "system.log");
    
    // ===== DEVICE FILES (/dev) =====
    
    const char* devices = 
        "RadiumOS Device List\n"
        "====================\n"
        "\n"
        "Block Devices:\n"
        "  /dev/hda    - Primary hard disk\n"
        "\n"
        "Character Devices:\n"
        "  /dev/tty    - Terminal\n"
        "  /dev/null   - Null device\n"
        "\n"
        "Network Devices:\n"
        "  /dev/eth0   - Ethernet adapter (RTL8139)\n";
    
    avfs_create_file("/dev/devices.txt", strlen(devices));
    avfs_write_file("/dev/devices.txt", devices, strlen(devices), 0);
    done("Created device list", "devices.txt");
    
    // ===== ROOT DIRECTORY FILES =====
    
    // README in root
    const char* readme = 
        "RadiumOS Filesystem\n"
        "===================\n"
        "\n"
        "Directory Structure:\n"
        "  /                  - Root directory\n"
        "  /bin               - Executable files and scripts\n"
        "  /etc               - System configuration files\n"
        "  /home              - User home directories\n"
        "  /home/user         - Default user directory\n"
        "  /tmp               - Temporary files\n"
        "  /var               - Variable data files\n"
        "  /var/log           - System logs\n"
        "  /dev               - Device files\n"
        "  /.~                - Hidden system directory\n"
        "\n"
        "Important Files:\n"
        "  /bin/autoexec.rsh  - Startup script (runs on boot)\n"
        "  /bin/rsh:main      - RSH standard library\n"
        "  /bin/rsh:math      - RSH math library\n"
        "  /bin/rsh:string    - RSH string library\n"
        "  /etc/config.sys    - System configuration\n"
        "  /etc/network.cfg   - Network configuration\n"
        "  /etc/.username.cfg - System username\n"
        "  /etc/.password.cfg - System password\n"
        "\n"
        "Sample Scripts:\n"
        "  /home/user/hello.rsh        - Hello world example\n"
        "  /home/user/scripts/calc.rsh - Calculator demo\n"
        "  /home/user/scripts/sysinfo.rsh - System information\n"
        "\n"
        "Basic Commands:\n"
        "  ls [dir]           - List files in directory\n"
        "  cd <dir>           - Change directory\n"
        "  cat <file>         - Display file contents\n"
        "  rsh <script>       - Run RSH script\n"
        "\n"
        "For more information, see the documentation in /home/user/\n";
    
    avfs_create_file("/README.txt", strlen(readme));
    avfs_write_file("/README.txt", readme, strlen(readme), 0);
    done("Created README file", "README.txt");
    
    // License file
    const char* license = 
        "RadiumOS License\n"

        "Copyright (c) 2025 RadiumOS Project\n"
        "\n"
        "This is a hobby operating system project.\n"
        "Free to use, modify, and distribute.\n"
        "\n"
        "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND.\n";
    
    avfs_create_file("/LICENSE.txt", strlen(license));
    avfs_write_file("/LICENSE.txt", license, strlen(license), 0);
    done("Created license file", "LICENSE.txt");
    
    // Version info
    const char* version = 
        "RadiumOS v1.0\n"
        "Build: 2025.01\n"
        "Kernel: Radium\n"
        "Shell: RSH\n"
        "Filesystem: AVFS\n";
    
    avfs_create_file("/VERSION.txt", strlen(version));
    avfs_write_file("/VERSION.txt", version, strlen(version), 0);
    done("Created version file", "VERSION.txt");
    // ===== BROWSER TEST PAGE (/home/user) =====

const char* hello_html =
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "  <title>RadiumOS</title>\n"
    "</head>\n"
    "<body style=\"background-color: #120C06; color: #DDCCBB;\">\n"
    "\n"
    "  <div style=\"margin-left: 40; background-color: #1A1008; color: #DDCCBB; padding: 20;\">\n"
    "\n"
    "    <header style=\"text-align: center;\">\n"
    "      <h1 style=\"color: #FF4400; font-size: 32;\">RADIUM OS</h1>\n"
    "      <b style=\"font-size: 18; color: #FFCCAA;\">Bare Metal - i686 - Rust &amp; C Kernel</b><br>\n"
    "      <i style=\"color: #997755;\">VERSION 1.0.0 -- SCP_2801</i>\n"
    "    </header>\n"
    "\n"
    "    <hr>\n"
    "\n"
    "    <section style=\"background-color: #0F0A04; text-align: center;\">\n"
    "      <b style=\"color: #FF7722;\">MEMORY:</b> 2G\n"
    "      <b style=\"color: #FF7722;\">ARCH:</b> i686\n"
    "      <b style=\"color: #FF7722;\">DISPLAY:</b> VBE 800x600x32\n"
    "      <b style=\"color: #FF7722;\">NET:</b> RTL8139\n"
    "    </section>\n"
    "\n"
    "    <hr>\n"
    "\n"
    "    <p style=\"color: #FF5500;\"><b>// system components</b></p>\n"
    "\n"
    "    <div style=\"display: block;\">\n"
    "\n"
    "      <article style=\"display: block; margin-left: 15;\">\n"
    "        <b style=\"color: #FF7722;\">[KERNEL]</b>\n"
    "        <ul style=\"margin-left: 20;\">\n"
    "          <li>Preemptive task scheduler</li>\n"
    "          <li>Custom AVFS filesystem</li>\n"
    "          <li style=\"color: #FFAA55;\">VBE 800x600x32 Framebuffer</li>\n"
    "          <li>PS/2 keyboard input</li>\n"
    "        </ul>\n"
    "      </article>\n"
    "\n"
    "      <article style=\"display: block; margin-left: 15;\">\n"
    "        <b style=\"color: #FF7722;\">[NETWORKING]</b>\n"
    "        <ul style=\"margin-left: 20;\">\n"
    "          <li>RTL8139 NIC driver</li>\n"
    "          <li>Hand-built TCP/IP stack</li>\n"
    "          <li style=\"color: #FFAA55;\">PSCA HTTPS proxy active</li>\n"
    "        </ul>\n"
    "      </article>\n"
    "\n"
    "      <article style=\"display: block; margin-left: 15;\">\n"
    "        <b style=\"color: #FF7722;\">[SECURITY]</b>\n"
    "        <ul style=\"margin-left: 20;\">\n"
    "          <li>AES-128 CBC encryption</li>\n"
    "          <li>Secure input mode active</li>\n"
    "          <li style=\"color: #FFAA55;\">FIPS 197 self-test: PASSED</li>\n"
    "        </ul>\n"
    "      </article>\n"
    "\n"
    "      <article style=\"display: block; margin-left: 15;\">\n"
    "        <b style=\"color: #FF7722;\">[DESKTOP]</b>\n"
    "        <ul style=\"margin-left: 20;\">\n"
    "          <li>Window manager</li>\n"
    "          <li>Notepad, Calculator, ColorPicker</li>\n"
    "          <li style=\"color: #FFAA55;\">Browser (you are here)</li>\n"
    "        </ul>\n"
    "      </article>\n"
    "\n"
    "    </div>\n"
    "\n"
    "    <hr>\n"
    "\n"
    "    <p style=\"color: #FF5500;\"><b>// links</b></p>\n"
    "    <p><a href=\"file://-/home/user/about.html\">About RadiumOS</a></p>\n"
    "    <p><a href=\"file://-/home/user/log.txt\">System Log</a></p>\n"
    "\n"
    "    <footer style=\"text-align: center;\">\n"
    "      <i style=\"color: #997755;\">RadiumOS (c) 2026 -- QEMU i686 -- scp_2801</i><br>\n"
    "      <b style=\"color: #FF4400;\">SYSTEM READY</b>\n"
    "    </footer>\n"
    "\n"
    "  </div>\n"
    "\n"
    "</body>\n"
    "</html>\n";

avfs_create_file("/home/user/hello.html", strlen(hello_html));
avfs_write_file("/home/user/hello.html", hello_html, strlen(hello_html), 0);
done("Updated hello world with CSS & New Tags", "hello.html");
    avfs_chdir("/home/user");
    
}
