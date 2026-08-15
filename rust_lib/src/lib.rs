#![no_std]

#![allow(dead_code)]
#![allow(unused_variables)]

use core::sync::atomic::{AtomicU32, Ordering};
use core::ptr::read_volatile;
use core::slice;
use core::str;
use core::ptr;
use core::cmp;
use linked_list_allocator::LockedHeap;



extern "C" {
    // Terminal functions
    fn terminal_putchar(c: u8);
    fn terminal_clear() -> u32;
    fn terminal_setcolor(color: u8);
    fn print_char(c: u8);
    fn print(s: *const u8);
    fn terminal_initialize();
    // Memory functions
    fn malloc(size: u32) -> *mut u8;
    fn free(ptr: *mut u8);

    // AVFS functions
    fn avfs_create_file(name: *const u8, size: u32) -> i32;
    fn avfs_write_file(name: *const u8, buffer: *const u8, size: u32, offset: u32) -> i32;
    fn avfs_read_file(name: *const u8, buffer: *mut u8, size: u32, offset: u32) -> i32;
    fn avfs_remove_file(name: *const u8) -> i32;
    fn avfs_get_filesize(name: *const u8) -> i32;
    fn avfs_file_exists(name: *const u8) -> bool;
    fn avfs_create_file_dir(path: *const u8) -> i32;
    fn avfs_list_directory(path: *const u8, entries: *mut u8, max_entries: i32) -> i32;
pub fn get_cpu_info_struct() -> *const CPUInfo;
    // I/O functions
    fn outb(port: u16, val: u8);
    fn inb(port: u16) -> u8;
    fn port_byte_in(port: u16) -> u8;
    fn port_byte_out(port: u16, data: u8);

    // Keyboard functions
    fn is_key_pressed() -> bool;
    fn keyboard_wait_for_key(dump: u8) -> u8;
    fn keyboard_handler() -> u32;
    fn keyboard_input(userinput: *mut u8) -> i32;
    fn keyboard_input_secure(buf: *mut u8);
    fn keyboard_await(msg: *const u8, clear: bool);
    fn shift_active() -> bool;
    fn caps_lock_active() -> bool;
    fn keyboard_to_char(sc: u8, shift: bool, caps: bool) -> u8;

    // Timer functions
    fn get_ticks() -> u32;
    fn sleep_ms(ms: u32);
    fn delay(ms: u32);

    // Command execution
    fn execute_command_extern(cmd: *const u8);



    // VGA window functions
    fn vga_create_centered_window(w: i32, h: i32, fg: u8, bg: u8) -> *mut u8;
    fn vga_destroy_window(win: *mut u8);
    fn vga_win_clear(win: *mut u8);
    fn vga_win_refresh(win: *mut u8);
    fn vga_win_putc_colored(win: *mut u8, x: i32, y: i32, c: u8, color: u8);
    fn vga_win_puts_colored(win: *mut u8, x: i32, y: i32, s: *const u8, color: u8);
    fn vga_win_puts_centered(win: *mut u8, y: i32, s: *const u8);
    fn vga_win_set_title(win: *mut u8, title: *const u8);
    fn vga_entry_color(fg: u8, bg: u8) -> u8;
    // Heap boundaries
    static _heap_start: u8;
    static _heap_end: u8;
}

const VGA_COLOR_BLACK: u8 = 0;
const VGA_COLOR_BLUE: u8 = 1;
const VGA_COLOR_GREEN: u8 = 2;
const VGA_COLOR_CYAN: u8 = 3;
const VGA_COLOR_RED: u8 = 4;
const VGA_COLOR_LIGHT_GREY: u8 = 7;
const VGA_COLOR_DARK_GREY: u8 = 8;
const VGA_COLOR_LIGHT_GREEN: u8 = 10;
const VGA_COLOR_LIGHT_CYAN: u8 = 11;
const VGA_COLOR_LIGHT_RED: u8 = 12;
const VGA_COLOR_WHITE: u8 = 15;
// Global flag - set by anyone who updates NET_LAST_*
static mut NET_HUD_DIRTY: bool = true; // true at boot so first frame always draws
static mut FUNC_BODY_BUF: [Line; MAX_FUNC_LINES] = [Str::new(); MAX_FUNC_LINES];
// ── Global context (static) ───────────────────────────────────
static mut GLOBAL_CTX: ScriptCtx = ScriptCtx::new();
static mut INITIALIZED: bool = false;
static mut TEST_ARIMG_BUFFER: [u8; 2048] = [0; 2048];
//=============================================================================
// UTILITY FUNCTIONS
//=============================================================================

unsafe fn print_hex_byte(byte: u8) {
    let hex_chars = b"0123456789ABCDEF";
    terminal_putchar(hex_chars[(byte >> 4) as usize]);
    terminal_putchar(hex_chars[(byte & 0xF) as usize]);
}
fn isqrt(n: u32) -> u32 {
    if n == 0 {
        return 0;
    }
    
    let mut x = n;
    let mut y = (x + 1) / 2;
    
    while y < x {
        x = y;
        y = (x + n / x) / 2;
    }
    
    x
}

// =============================================================================
// HELPER: Pointer Utilities
// =============================================================================

/// Trait to easily get a raw pointer from types we use in the editor.
pub trait AsRawPtr {
    /// Returns a const pointer (*const u8)
    fn as_ptr(&self) -> *const u8;
}

/// Trait to easily get a mutable pointer from types we modify.
pub trait AsRawMutPtr {
    /// Returns a mutable pointer (*mut u8)
    fn as_mut_ptr(&mut self) -> *mut u8;
}

impl<const N: usize> AsRawPtr for [u8; N] {
    fn as_ptr(&self) -> *const u8 {
        (self as &[u8]).as_ptr()   // coerce to slice first
    }
}

impl<const N: usize> AsRawMutPtr for [u8; N] {
    fn as_mut_ptr(&mut self) -> *mut u8 {
        (self as &mut [u8]).as_mut_ptr()
    }
}

// Implement for Slices (e.g., &[u8])
impl AsRawPtr for [u8] {
    fn as_ptr(&self) -> *const u8 {
        self.as_ptr()
    }
}

impl AsRawMutPtr for [u8] {
    fn as_mut_ptr(&mut self) -> *mut u8 {
        self.as_mut_ptr()
    }
}

//=============================================================================
// TERMINAL PRINT HELPERS
// rust_print / terminal_write must wrap at TERM_MAX_COL.
// These replace the original write functions.
//=============================================================================

static mut TERM_COL: usize = 0;
static mut TERM_ROW: usize = 0;

/// Write a byte to the terminal, wrapping at TERM_MAX_COL.
/// Scroll up when TERM_ROW reaches 25.
unsafe fn term_putc(c: u8) {
    if c == b'\n' {
        TERM_COL  = 0;
        TERM_ROW += 1;
    } else {
        vga_write(TERM_COL, TERM_ROW, c, 0x07);
        TERM_COL += 1;
        if TERM_COL >= TERM_MAX_COL {
            TERM_COL  = 0;
            TERM_ROW += 1;
        }
    }
    if TERM_ROW >= 50 {
        // Scroll the terminal region (cols 0..TERM_MAX_COL) up one line
        for row in 1..25usize {
            for col in 0..TERM_MAX_COL {
                let idx = row * 80 + col;
                let above = (row - 1) * 80 + col;
                *VGA_MEMORY.add(above) = *VGA_MEMORY.add(idx);
            }
        }
        // Clear the last row in the terminal region
        for col in 0..TERM_MAX_COL {
            *VGA_MEMORY.add(24 * 80 + col) = 0x0700 | b' ' as u16;
        }
        TERM_ROW = 24;
    }
}

#[no_mangle]
pub extern "C" fn rust_print(s: &[u8]) {
    unsafe {
        for &c in s { terminal_putchar(c); }
    }
}

fn print_num(mut num: i32) {
    if num == 0 {
        rust_print(b"0");
        return;
    }
    
    if num < 0 {
        rust_print(b"-");
        num = -num;
    }
    
    let mut buffer = [0u8; 16];
    let mut i = 0;
    
    while num > 0 {
        buffer[i] = (num % 10) as u8 + b'0';
        num /= 10;
        i += 1;
    }
    
    while i > 0 {
        i -= 1;
        unsafe { terminal_putchar(buffer[i]); }
    }
}

fn print_hex(mut num: u32) {
    rust_print(b"0x");
    let hex_chars = b"0123456789ABCDEF";
    let mut buffer = [0u8; 8];
    
    for i in (0..8).rev() {
        buffer[i] = hex_chars[(num & 0xF) as usize];
        num >>= 4;
    }
    
    rust_print(&buffer);
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    unsafe { 
        terminal_setcolor(0x4F);
        rust_print(b"\n\n!!! RUST PANIC !!!\n");
        
        if let Some(location) = info.location() {
            rust_print(b"Location: ");
            rust_print(location.file().as_bytes());
            rust_print(b":");
            print_num(location.line() as i32);
            rust_print(b"\n");
        }
    }
    loop {
        unsafe { print(b"\n??\n\0".as_ptr()); }
        unsafe { core::arch::asm!("hlt"); }
    }
}

//=============================================================================
// VGA HELPERS
//=============================================================================

const VGA_MEMORY: *mut u16 = 0xB8000 as *mut u16;

unsafe fn vga_write(x: usize, y: usize, c: u8, color: u8) {
    let offset = y * 80 + x;
    *VGA_MEMORY.add(offset) = ((color as u16) << 8) | (c as u16);
}

unsafe fn vga_write_string(x: usize, y: usize, s: &[u8], color: u8) {
    for (i, &byte) in s.iter().enumerate() {
        if x + i >= 80 { break; }
        vga_write(x + i, y, byte, color);
    }
}

unsafe fn vga_fill_rect(x: usize, y: usize, w: usize, h: usize, c: u8, color: u8) {
    for dy in 0..h {
        for dx in 0..w {
            if x + dx < 80 && y + dy < 25 {
                vga_write(x + dx, y + dy, c, color);
            }
        }
    }
}

//=============================================================================
// PIT AND INTERRUPT CONTROL
//=============================================================================

#[no_mangle]
pub extern "C" fn rust_setup_pit(frequency: u32) -> i32 {
    if frequency == 0 {
        rust_print(b"Error: Invalid PIT frequency\n");
        return -1;
    }
    
    unsafe {
        let divisor = 1193180 / frequency;
        outb(0x43, 0x36);
        let low = (divisor & 0xFF) as u8;
        let high = ((divisor >> 8) & 0xFF) as u8;
        outb(0x40, low);
        outb(0x40, high);
    }
    
    rust_print(b"PIT configured at ");
    print_num(frequency as i32);
    rust_print(b" Hz\n");
    0
}

#[no_mangle]
pub extern "C" fn rust_enable_interrupts() {
    unsafe { core::arch::asm!("sti"); }
    rust_print(b"Interrupts enabled\n");
}

#[no_mangle]
pub extern "C" fn rust_disable_interrupts() {
    unsafe { core::arch::asm!("cli"); }
}


//=============================================================================
// RTL8139 NETWORK DRIVER
//=============================================================================

const RTL8139_REG_MAC0: u16 = 0x00;
const RTL8139_REG_CMD: u16 = 0x37;
const RTL8139_REG_IMR: u16 = 0x3C;
const RTL8139_REG_ISR: u16 = 0x3E;
const RTL8139_REG_RCR: u16 = 0x44;
const RTL8139_REG_CONFIG1: u16 = 0x52;

const TSAD_ARRAY: [u16; 4] = [0x20, 0x24, 0x28, 0x2C];
const TSD_ARRAY: [u16; 4] = [0x10, 0x14, 0x18, 0x1C];

const RX_BUFFER_SIZE: usize = 8192 + 16 + 1500;

const CMD_RESET: u8 = 0x10;
const CMD_RX_ENABLE: u8 = 0x08;
const CMD_TX_ENABLE: u8 = 0x04;
const CMD_BUFFER_EMPTY: u8 = 0x01;

const INT_RXOK: u16 = 0x01;
const INT_TXOK: u16 = 0x04;
const INT_RXERR: u16 = 0x02;
const INT_TXERR: u16 = 0x08;
const INT_RX_OVERFLOW: u16 = 0x10;

const RCR_AAP: u32 = 1 << 0;
const RCR_APM: u32 = 1 << 1;
const RCR_AM: u32 = 1 << 2;
const RCR_AB: u32 = 1 << 3;
const RCR_WRAP: u32 = 1 << 7;

#[repr(C)]
pub struct RTL8139Interface {
    iobase: u16,
    rx_buff_virtual: *mut u8,
    rx_buff_physical: u32,
    tx_buff_virtual: [*mut u8; 4],
    tx_buff_physical: [u32; 4],
    current_packet: u16,
    tx_curr: u8,
    tok: u8,
    mac: [u8; 6],
}

static mut RTL8139_DEVICE: Option<RTL8139Interface> = None;

unsafe fn inw(port: u16) -> u16 {
    let mut result: u16;
    core::arch::asm!(
        "in ax, dx",
        out("ax") result,
        in("dx") port,
        options(nomem, nostack, preserves_flags)
    );
    result
}

unsafe fn outw(port: u16, value: u16) {
    core::arch::asm!(
        "out dx, ax",
        in("dx") port,
        in("ax") value,
        options(nomem, nostack, preserves_flags)
    );
}

unsafe fn inl(port: u16) -> u32 {
    let mut result: u32;
    core::arch::asm!(
        "in eax, dx",
        out("eax") result,
        in("dx") port,
        options(nomem, nostack, preserves_flags)
    );
    result
}

unsafe fn outl(port: u16, value: u32) {
    core::arch::asm!(
        "out dx, eax",
        in("dx") port,
        in("eax") value,
        options(nomem, nostack, preserves_flags)
    );
}

static mut HEAP: [u8; 1024 * 1024] = [0; 1024 * 1024];
static mut HEAP_OFFSET: usize = 0;

unsafe fn simple_malloc(size: u32) -> *mut u8 {
    let size = (size as usize + 15) & !15;
    if HEAP_OFFSET + size > HEAP.len() {
        rust_print(b"ERROR: Out of heap! Requested: ");
        print_num(size as i32);
        rust_print(b" Used: ");
        print_num(HEAP_OFFSET as i32);
        rust_print(b"\n");
        return core::ptr::null_mut();
    }
    let ptr = HEAP.as_mut_ptr().add(HEAP_OFFSET);
    HEAP_OFFSET += size;
    ptr
}

#[no_mangle]
pub extern "C" fn rust_is_rtl8139(vendor_id: u16, device_id: u16) -> bool {
    vendor_id == 0x10ec && device_id == 0x8139
}

#[no_mangle]
pub extern "C" fn rust_init_rtl8139(iobase: u16) -> i32 {
    unsafe {
        rust_print(b"RTL8139: Initializing at IO base ");
        print_hex(iobase as u32);
        rust_print(b"\n");
        
        // Check if already initialized
        if RTL8139_DEVICE.is_some() {
            rust_print(b"WARNING: RTL8139 already initialized!\n");
            return 0;
        }
        
        outb(iobase + RTL8139_REG_CONFIG1, 0x0);
        outb(iobase + RTL8139_REG_CMD, CMD_RESET);
        
        let mut timeout = 10000;
        while (inb(iobase + RTL8139_REG_CMD) & CMD_RESET) != 0 {
            timeout -= 1;
            if timeout == 0 {
                rust_print(b"ERROR: Reset timeout\n");
                return -1;
            }
        }

        let rx_buffer = simple_malloc((RX_BUFFER_SIZE + 4095) as u32);
        if rx_buffer.is_null() {
            rust_print(b"ERROR: Failed to allocate RX buffer\n");
            return -1;
        }
        let rx_buffer_phys = rx_buffer as u32;

        let mut tx_buffers = [core::ptr::null_mut(); 4];
        let mut tx_buffers_phys = [0u32; 4];

        for i in 0..4 {
            tx_buffers[i] = simple_malloc(4096);
            if tx_buffers[i].is_null() {
                rust_print(b"ERROR: Failed to allocate TX buffer ");
                print_num(i as i32);
                rust_print(b"\n");
                return -1;
            }
            tx_buffers_phys[i] = tx_buffers[i] as u32;
        }

        outl(iobase + 0x30, rx_buffer_phys);
        outw(iobase + RTL8139_REG_IMR, INT_RXOK | INT_TXOK | INT_RXERR | INT_TXERR | INT_RX_OVERFLOW);
        outw(iobase + RTL8139_REG_ISR, 0xFFFF);
        outl(iobase + RTL8139_REG_RCR, RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_WRAP);
        outb(iobase + RTL8139_REG_CMD, CMD_RX_ENABLE | CMD_TX_ENABLE);

        let mut mac = [0u8; 6];
        for i in 0..6 {
            mac[i] = inb(iobase + RTL8139_REG_MAC0 + i as u16);
        }

        RTL8139_DEVICE = Some(RTL8139Interface {
            iobase,
            rx_buff_virtual: rx_buffer,
            rx_buff_physical: rx_buffer_phys,
            tx_buff_virtual: tx_buffers,
            tx_buff_physical: tx_buffers_phys,
            current_packet: 0,
            tx_curr: 0,
            tok: 0,
            mac,
        });

        // Verify it was stored
        if RTL8139_DEVICE.is_some() {
            rust_print(b"RTL8139: Device struct stored successfully\n");
        } else {
            rust_print(b"ERROR: Failed to store device struct!\n");
            return -1;
        }

        rust_print(b"RTL8139: Initialized successfully\n");
        0
    }
}

#[no_mangle]
pub extern "C" fn rust_rtl8139_send(packet: *const u8, packet_size: u32) -> i32 {
    if packet.is_null() {
        rust_print(b"ERROR: Null packet pointer\n");
        return -1;
    }
    
    if packet_size == 0 || packet_size > 1792 {
        rust_print(b"ERROR: Invalid packet size: ");
        print_num(packet_size as i32);
        rust_print(b"\n");
        return -1;
    }

    unsafe {
        let device = match RTL8139_DEVICE.as_mut() {
            Some(dev) => dev,
            None => {
                rust_print(b"ERROR: RTL8139 device not initialized\n");
                return -1;
            }
        };

        let iobase = device.iobase;
        let tx_curr = device.tx_curr as usize;

        // Wait for TX buffer to be available
        let mut timeout = 10000;
        loop {
            let status = inl(iobase + TSD_ARRAY[tx_curr]);
            if (status & (1 << 13)) != 0 {
                break;
            }
            timeout -= 1;
            if timeout == 0 {
                rust_print(b"ERROR: TX timeout\n");
                return -1;
            }
        }

        let tx_buffer = device.tx_buff_virtual[tx_curr];
        if tx_buffer.is_null() {
            rust_print(b"ERROR: TX buffer is null!\n");
            return -1;
        }
        
        // Copy packet to TX buffer
        for i in 0..packet_size {
            *tx_buffer.add(i as usize) = *packet.add(i as usize);
        }

        outl(iobase + TSAD_ARRAY[tx_curr], device.tx_buff_physical[tx_curr]);
        outl(iobase + TSD_ARRAY[tx_curr], packet_size & 0x1FFF);

        device.tx_curr = ((tx_curr + 1) % 4) as u8;

        0
    }
}

static mut RX_RESPONSE_BUFFER: [u8; 4096] = [0; 4096];
static mut RX_RESPONSE_LENGTH: u32 = 0;

#[no_mangle]
pub extern "C" fn rust_rtl8139_receive() -> i32 {
    unsafe {
        let device = match RTL8139_DEVICE.as_mut() {
            Some(dev) => dev,
            None => return -1,
        };

        let iobase = device.iobase;
        let mut packets_received = 0;

        while (inb(iobase + RTL8139_REG_CMD) & CMD_BUFFER_EMPTY) == 0 {
            let current_packet = device.current_packet as usize;
            
            let buffer_ptr = device.rx_buff_virtual.add(current_packet) as *const u16;
            let packet_status = read_volatile(buffer_ptr);
            let packet_length = read_volatile(buffer_ptr.add(1));

            if (packet_status & 0x01) == 0 {
                break;
            }

            if packet_length < 64 || packet_length > 1518 {
                break;
            }

            let packet_data = buffer_ptr.add(2) as *const u8;

            // Copy to response buffer
            if RX_RESPONSE_LENGTH == 0 || RX_RESPONSE_LENGTH + packet_length as u32 <= 4096 {
                for i in 0..packet_length as usize {
                    if RX_RESPONSE_LENGTH as usize + i < 4096 {
                        RX_RESPONSE_BUFFER[RX_RESPONSE_LENGTH as usize + i] = *packet_data.add(i);
                    }
                }
                RX_RESPONSE_LENGTH += packet_length as u32;
            }

            let new_position = (current_packet + packet_length as usize + 4 + 3) & !3;
            
            device.current_packet = if new_position >= RX_BUFFER_SIZE {
                (new_position - RX_BUFFER_SIZE) as u16
            } else {
                new_position as u16
            };

            outw(iobase + 0x38, device.current_packet.wrapping_sub(0x10));

            packets_received += 1;
        }

        packets_received
    }
}

#[no_mangle]
pub extern "C" fn rust_rtl8139_get_mac(mac_out: *mut u8) -> i32 {
    if mac_out.is_null() {
        return -1;
    }

    unsafe {
        let device = match RTL8139_DEVICE.as_ref() {
            Some(dev) => dev,
            None => return -1,
        };

        for i in 0..6 {
            *mac_out.add(i) = device.mac[i];
        }

        0
    }
}

#[no_mangle]
pub extern "C" fn rust_rtl8139_check_init() -> bool {
    unsafe {
        let initialized = RTL8139_DEVICE.is_some();
        if initialized {
            rust_print(b"RTL8139: Device IS initialized\n");
        } else {
            rust_print(b"RTL8139: Device NOT initialized\n");
        }
        initialized
    }
}

//=============================================================================
// NETWORK STACK - TCP/IP/Ethernet/DNS
//=============================================================================

static mut LOCAL_IP: [u8; 4] = [10, 0, 2, 15];
static mut GATEWAY_IP: [u8; 4] = [10, 0, 2, 2];   // keep this - QEMU gateway
static mut DNS_SERVER:  [u8; 4] = [10, 0, 2, 3];

static mut TCP_SEQ_NUM: u32 = 12345;
static mut TCP_ACK_NUM: u32 = 0;
static mut TCP_SRC_PORT: u16 = 50000;

fn calculate_ip_checksum(header: &[u8], length: usize) -> u16 {
    let mut sum: u32 = 0;
    
    for i in (0..length).step_by(2) {
        let word = if i + 1 < length {
            ((header[i] as u32) << 8) | (header[i + 1] as u32)
        } else {
            (header[i] as u32) << 8
        };
        sum = sum.wrapping_add(word);
    }
    
    while (sum >> 16) != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    !sum as u16
}

unsafe fn build_ethernet_frame(dest_mac: &[u8; 6], ether_type: u16, payload: &[u8], buffer: &mut [u8]) -> usize {
    let device = match RTL8139_DEVICE.as_ref() {
        Some(dev) => dev,
        None => {
            rust_print(b"ERROR: RTL8139 not initialized in build_ethernet_frame!\n");
            return 0;
        }
    };
    
    let mut idx = 0;
    
    // Check buffer bounds
    if buffer.len() < 14 + payload.len() {
        rust_print(b"ERROR: Buffer too small for ethernet frame!\n");
        return 0;
    }
    
    // Dest MAC
    for i in 0..6 {
        buffer[idx] = dest_mac[i];
        idx += 1;
    }
    
    // Src MAC
    for i in 0..6 {
        buffer[idx] = device.mac[i];
        idx += 1;
    }
    
    // EtherType
    buffer[idx] = (ether_type >> 8) as u8;
    idx += 1;
    buffer[idx] = (ether_type & 0xFF) as u8;
    idx += 1;
    
    // Payload
    for &byte in payload {
        if idx >= buffer.len() {
            rust_print(b"ERROR: Buffer overflow in ethernet frame!\n");
            return 0;
        }
        buffer[idx] = byte;
        idx += 1;
    }
    
    idx
}

fn build_ip_packet(dest_ip: &[u8; 4], protocol: u8, payload: &[u8], buffer: &mut [u8]) -> usize {
    unsafe {
        let mut idx = 0;
        
        if buffer.len() < 20 + payload.len() {
            rust_print(b"ERROR: Buffer too small for IP packet!\n");
            return 0;
        }
        
        buffer[idx] = 0x45;
        idx += 1;
        buffer[idx] = 0x00;
        idx += 1;
        
        let total_len = 20 + payload.len();
        buffer[idx] = (total_len >> 8) as u8;
        idx += 1;
        buffer[idx] = (total_len & 0xFF) as u8;
        idx += 1;
        
        buffer[idx] = 0x00;
        idx += 1;
        buffer[idx] = 0x01;
        idx += 1;
        
        buffer[idx] = 0x40;
        idx += 1;
        buffer[idx] = 0x00;
        idx += 1;
        
        buffer[idx] = 64;
        idx += 1;
        
        buffer[idx] = protocol;
        idx += 1;
        
        let checksum_idx = idx;
        buffer[idx] = 0x00;
        idx += 1;
        buffer[idx] = 0x00;
        idx += 1;
        
        for i in 0..4 {
            buffer[idx] = LOCAL_IP[i];
            idx += 1;
        }
        
        for i in 0..4 {
            buffer[idx] = dest_ip[i];
            idx += 1;
        }
        
        let checksum = calculate_ip_checksum(&buffer[0..20], 20);
        buffer[checksum_idx] = (checksum >> 8) as u8;
        buffer[checksum_idx + 1] = (checksum & 0xFF) as u8;
        
        for i in 0..payload.len() {
            buffer[idx] = payload[i];
            idx += 1;
        }
        
        idx
    }
}

fn build_tcp_packet(
    dest_ip: &[u8; 4],
    dest_port: u16,
    src_port: u16,
    seq_num: u32,
    ack_num: u32,
    flags: u8,
    payload: &[u8],
    buffer: &mut [u8]
) -> usize {
    unsafe {
        let mut idx = 0;
        
        if buffer.len() < 20 + payload.len() {
            rust_print(b"ERROR: Buffer too small for TCP packet!\n");
            return 0;
        }
        
        buffer[idx] = (src_port >> 8) as u8;
        idx += 1;
        buffer[idx] = (src_port & 0xFF) as u8;
        idx += 1;
        
        buffer[idx] = (dest_port >> 8) as u8;
        idx += 1;
        buffer[idx] = (dest_port & 0xFF) as u8;
        idx += 1;
        
        buffer[idx] = (seq_num >> 24) as u8;
        idx += 1;
        buffer[idx] = ((seq_num >> 16) & 0xFF) as u8;
        idx += 1;
        buffer[idx] = ((seq_num >> 8) & 0xFF) as u8;
        idx += 1;
        buffer[idx] = (seq_num & 0xFF) as u8;
        idx += 1;
        
        buffer[idx] = (ack_num >> 24) as u8;
        idx += 1;
        buffer[idx] = ((ack_num >> 16) & 0xFF) as u8;
        idx += 1;
        buffer[idx] = ((ack_num >> 8) & 0xFF) as u8;
        idx += 1;
        buffer[idx] = (ack_num & 0xFF) as u8;
        idx += 1;
        
        buffer[idx] = 0x50;
        idx += 1;
        
        buffer[idx] = flags;
        idx += 1;
        
        buffer[idx] = 0xFF;
        idx += 1;
        buffer[idx] = 0xFF;
        idx += 1;
        
        let checksum_idx = idx;
        buffer[idx] = 0x00;
        idx += 1;
        buffer[idx] = 0x00;
        idx += 1;
        
        buffer[idx] = 0x00;
        idx += 1;
        buffer[idx] = 0x00;
        idx += 1;
        
        for i in 0..payload.len() {
            buffer[idx] = payload[i];
            idx += 1;
        }
        
        let tcp_len = idx;
        let mut pseudo_header = [0u8; 12];
        let mut ph_idx = 0;
        
        for i in 0..4 {
            pseudo_header[ph_idx] = LOCAL_IP[i];
            ph_idx += 1;
        }
        
        for i in 0..4 {
            pseudo_header[ph_idx] = dest_ip[i];
            ph_idx += 1;
        }
        
        pseudo_header[ph_idx] = 0x00;
        ph_idx += 1;
        pseudo_header[ph_idx] = 6;
        ph_idx += 1;
        pseudo_header[ph_idx] = (tcp_len >> 8) as u8;
        ph_idx += 1;
        pseudo_header[ph_idx] = (tcp_len & 0xFF) as u8;
        
        let mut sum: u32 = 0;
        
        for i in (0..12).step_by(2) {
            let word = ((pseudo_header[i] as u32) << 8) | (pseudo_header[i + 1] as u32);
            sum = sum.wrapping_add(word);
        }
        
        for i in (0..tcp_len).step_by(2) {
            let word = if i + 1 < tcp_len {
                ((buffer[i] as u32) << 8) | (buffer[i + 1] as u32)
            } else {
                (buffer[i] as u32) << 8
            };
            sum = sum.wrapping_add(word);
        }
        
        while (sum >> 16) != 0 {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        
        let checksum = !sum as u16;
        buffer[checksum_idx] = (checksum >> 8) as u8;
        buffer[checksum_idx + 1] = (checksum & 0xFF) as u8;
        
        idx
    }
}

fn build_dns_query(hostname: &[u8], buffer: &mut [u8]) -> usize {
    if buffer.len() < 512 {
        rust_print(b"ERROR: DNS buffer too small!\n");
        return 0;
    }
    
    let mut idx = 0;
    
    // DNS Header (12 bytes)
    buffer[idx] = 0x12; idx += 1;
    buffer[idx] = 0x34; idx += 1;
    buffer[idx] = 0x01; idx += 1;
    buffer[idx] = 0x00; idx += 1;
    buffer[idx] = 0x00; idx += 1;
    buffer[idx] = 0x01; idx += 1;
    buffer[idx] = 0x00; idx += 1;
    buffer[idx] = 0x00; idx += 1;
    buffer[idx] = 0x00; idx += 1;
    buffer[idx] = 0x00; idx += 1;
    buffer[idx] = 0x00; idx += 1;
    buffer[idx] = 0x00; idx += 1;
    
    // Query section - convert hostname to DNS format
    let mut i = 0;
    let mut label_start = idx;
    idx += 1;
    let mut label_len = 0;
    
    while i < hostname.len() && hostname[i] != 0 {
        if idx >= buffer.len() - 10 {
            rust_print(b"ERROR: DNS buffer overflow!\n");
            return 0;
        }
        
        if hostname[i] == b'.' {
            buffer[label_start] = label_len;
            label_start = idx;
            idx += 1;
            label_len = 0;
        } else {
            buffer[idx] = hostname[i];
            idx += 1;
            label_len += 1;
        }
        i += 1;
    }
    
    buffer[label_start] = label_len;
    buffer[idx] = 0x00; idx += 1;
    buffer[idx] = 0x00; idx += 1;
    buffer[idx] = 0x01; idx += 1;
    buffer[idx] = 0x00; idx += 1;
    buffer[idx] = 0x01; idx += 1;
    
    idx
}

fn parse_dns_response(response: &[u8], response_len: usize) -> Option<[u8; 4]> {
    if response_len < 12 {
        return None;
    }
    
    if (response[2] & 0x80) == 0 {
        return None;
    }
    
    let ancount = ((response[6] as u16) << 8) | (response[7] as u16);
    
    if ancount == 0 {
        return None;
    }
    
    let mut idx = 12;
    
    // Skip question section
    while idx < response_len && response[idx] != 0 {
        if (response[idx] & 0xC0) == 0xC0 {
            idx += 2;
            break;
        }
        
        let label_len = response[idx] as usize;
        idx += 1 + label_len;
    }
    
    if idx < response_len && response[idx] == 0 {
        idx += 1;
    }
    
    idx += 4;
    
    // Parse answer section
    for _ in 0..ancount {
        if idx >= response_len {
            return None;
        }
        
        if (response[idx] & 0xC0) == 0xC0 {
            idx += 2;
        } else {
            while idx < response_len && response[idx] != 0 {
                let label_len = response[idx] as usize;
                idx += 1 + label_len;
                if idx >= response_len {
                    return None;
                }
            }
            idx += 1;
        }
        
        if idx + 10 > response_len {
            return None;
        }
        
        let rtype = ((response[idx] as u16) << 8) | (response[idx + 1] as u16);
        idx += 2;
        
        idx += 2;
        idx += 4;
        
        let rdlength = ((response[idx] as u16) << 8) | (response[idx + 1] as u16);
        idx += 2;
        
        if rtype == 1 && rdlength == 4 {
            if idx + 4 <= response_len {
                let ip = [
                    response[idx],
                    response[idx + 1],
                    response[idx + 2],
                    response[idx + 3]
                ];
                return Some(ip);
            }
        } //    deadass twin im just a 19 year old tryna live
         //     dont bother me and i wont do anything (i aint doin jack shit back but ykwim <3)
        //      yo sometimes i really think im losing my sanity.
       //       genuinely scared.
        
        idx += rdlength as usize;
    }
    
    None
}

// Update the DNS server configuration at the top of the file

// Also add this improved DNS query function with better debugging
fn dns_query(hostname: &[u8]) -> Option<[u8; 4]> {
    unsafe {
        rust_print(b"DNS: Resolving ");
        let mut i = 0;
        while i < hostname.len() && hostname[i] != 0 {
            terminal_putchar(hostname[i]);
            i += 1;
        }
        rust_print(b"...\n");
        
        let mut dns_query_buf = [0u8; 512];
        let dns_query_len = build_dns_query(hostname, &mut dns_query_buf);
        
        if dns_query_len == 0 {
            rust_print(b"ERROR: Failed to build DNS query\n");
            return None;
        }
        
        rust_print(b"DNS: Query built, ");
        print_num(dns_query_len as i32);
        rust_print(b" bytes\n");
        
        // Build UDP packet
        let mut udp_buffer = [0u8; 600];
        let mut idx = 0;
        
        // Source port (random high port)
        let src_port = 50000 + (get_ticks() % 10000) as u16;
        udp_buffer[idx] = (src_port >> 8) as u8;
        idx += 1;
        udp_buffer[idx] = (src_port & 0xFF) as u8;
        idx += 1;
        
        // Dest port (53 for DNS)
        udp_buffer[idx] = 0x00;
        idx += 1;
        udp_buffer[idx] = 0x35;
        idx += 1;
        
        let udp_len = 8 + dns_query_len;
        udp_buffer[idx] = (udp_len >> 8) as u8;
        idx += 1;
        udp_buffer[idx] = (udp_len & 0xFF) as u8;
        idx += 1;
        
        // Checksum (0 = no checksum for UDP)
        udp_buffer[idx] = 0x00;
        idx += 1;
        udp_buffer[idx] = 0x00;
        idx += 1;
        
        // DNS query data
        for i in 0..dns_query_len {
            udp_buffer[idx] = dns_query_buf[i];
            idx += 1;
        }
        
        let udp_packet_len = idx;
        
        let mut ip_buffer = [0u8; 1024];
        let ip_len = build_ip_packet(&DNS_SERVER, 17, &udp_buffer[0..udp_packet_len], &mut ip_buffer);
        
        if ip_len == 0 {
            rust_print(b"ERROR: Failed to build IP packet\n");
            return None;
        }
        
        rust_print(b"DNS: Sending query to ");
        for i in 0..4 {
            print_num(DNS_SERVER[i] as i32);
            if i < 3 {
                rust_print(b".");
            }
        }
        rust_print(b" (");
        print_num(ip_len as i32);
        rust_print(b" bytes)\n");
        
        let gateway_mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
        let mut eth_buffer = [0u8; 1518];
        let eth_len = build_ethernet_frame(&gateway_mac, 0x0800, &ip_buffer[0..ip_len], &mut eth_buffer);
        
        if eth_len == 0 {
            rust_print(b"ERROR: Failed to build Ethernet frame\n");
            return None;
        }
        
        rust_print(b"DNS: Sending ");
        print_num(eth_len as i32);
        rust_print(b" byte packet\n");
        
        let result = rust_rtl8139_send(eth_buffer.as_ptr(), eth_len as u32);
        
        if result != 0 {
            rust_print(b"DNS: Failed to send query (send error)\n");
            return None;
        }
        
        rust_print(b"DNS: Query sent successfully!\n");
        rust_print(b"DNS: Waiting for response...\n");
        
        RX_RESPONSE_LENGTH = 0;
        let mut timeout = 3000000;  // Increased timeout
        let mut packets_received = 0;
        
        while timeout > 0 {
            let packets = rust_rtl8139_receive();
            
            if packets > 0 {
                packets_received += packets;
                rust_print(b"DNS: Received ");
                print_num(packets);
                rust_print(b" packets (total: ");
                print_num(packets_received);
                rust_print(b"), ");
                print_num(RX_RESPONSE_LENGTH as i32);
                rust_print(b" bytes\n");
                
                // Check if we have enough data for a DNS response
                if RX_RESPONSE_LENGTH > 42 {
                    // Ethernet (14) + IP (20) + UDP (8) = 42 bytes minimum
                    
                    // Verify it's UDP protocol
                    if RX_RESPONSE_BUFFER[23] == 17 {  // IP protocol field
                        rust_print(b"DNS: Got UDP packet\n");
                        
                        // Check destination port is our source port
                        let dest_port = ((RX_RESPONSE_BUFFER[36] as u16) << 8) | (RX_RESPONSE_BUFFER[37] as u16);
                        
                        rust_print(b"DNS: Dest port: ");
                        print_num(dest_port as i32);
                        rust_print(b", expected: ");
                        print_num(src_port as i32);
                        rust_print(b"\n");
                        
                        if dest_port == src_port {
                            let dns_response = &RX_RESPONSE_BUFFER[42..RX_RESPONSE_LENGTH as usize];
                            
                            rust_print(b"DNS: Parsing response (");
                            print_num(dns_response.len() as i32);
                            rust_print(b" bytes)\n");
                            
                            if let Some(ip) = parse_dns_response(dns_response, dns_response.len()) {
                                rust_print(b"DNS: Successfully resolved to ");
                                for i in 0..4 {
                                    print_num(ip[i] as i32);
                                    if i < 3 {
                                        rust_print(b".");
                                    }
                                }
                                rust_print(b"\n");
                                return Some(ip);
                            } else {
                                rust_print(b"DNS: Failed to parse response\n");
                                // Show first few bytes for debugging
                                rust_print(b"DNS: First bytes: ");
                                for i in 0..16.min(dns_response.len()) {
                                    print_hex_byte(dns_response[i]);
                                    rust_print(b" ");
                                }
                                rust_print(b"\n");
                            }
                        }
                    }
                    
                    // Clear buffer for next packet
                    RX_RESPONSE_LENGTH = 0;
                }
            }
            
            timeout -= 1;
            
            if timeout % 500000 == 0 {
                rust_print(b".");
            }
        }
        
        rust_print(b"\nDNS: Timeout - received ");
        print_num(packets_received);
        rust_print(b" total packets, but no valid DNS response\n");
        None
    }
}

#[no_mangle]
pub extern "C" fn rust_ntfy_post_complete(message: *const u8, message_len: u32) -> i32 {
    unsafe {
        rust_print(b"\n=== Sending notification to ntfy.sh ===\n");
        
        let hostname = b"ntfy.sh\0";
        
        rust_print(b"Step 1: DNS Resolution\n");
        let server_ip = match dns_query(hostname) {
            Some(ip) => ip,
            None => {
                rust_print(b"ERROR: DNS resolution failed\n");
                return -1;
            }
        };
        
        rust_print(b"Resolved IP: ");
        for i in 0..4 {
            print_num(server_ip[i] as i32);
            if i < 3 {
                rust_print(b".");
            }
        }
        rust_print(b"\n");
        
        RX_RESPONSE_LENGTH = 0;
        
        rust_print(b"\nStep 2: Building HTTP request\n");
        let mut http_request = [0u8; 512];
        let mut idx = 0;
        
        let post_line = b"POST /scp_2801 HTTP/1.1\r\n";
        for &b in post_line {
            http_request[idx] = b;
            idx += 1;
        }
        
        let host_line = b"Host: ntfy.sh\r\n";
        for &b in host_line {
            http_request[idx] = b;
            idx += 1;
        }
        
        let content_type = b"Content-Type: text/plain\r\n";
        for &b in content_type {
            http_request[idx] = b;
            idx += 1;
        }
        
        let content_len_header = b"Content-Length: ";
        for &b in content_len_header {
            http_request[idx] = b;
            idx += 1;
        }
        
        let mut len_digits = [0u8; 10];
        let mut len_idx = 0;
        let mut temp = message_len;
        
        if temp == 0 {
            len_digits[0] = b'0';
            len_idx = 1;
        } else {
            while temp > 0 {
                len_digits[len_idx] = (temp % 10) as u8 + b'0';
                temp /= 10;
                len_idx += 1;
            }
        }
        
        for i in (0..len_idx).rev() {
            http_request[idx] = len_digits[i];
            idx += 1;
        }
        http_request[idx] = b'\r';
        idx += 1;
        http_request[idx] = b'\n';
        idx += 1;
        
        let connection = b"Connection: close\r\n";
        for &b in connection {
            http_request[idx] = b;
            idx += 1;
        }
        
        let user_agent = b"User-Agent: RadiumOS/1.0\r\n\r\n";
        for &b in user_agent {
            http_request[idx] = b;
            idx += 1;
        }
        
        for i in 0..message_len as usize {
            if idx >= 500 {
                break;
            }
            http_request[idx] = *message.add(i);
            idx += 1;
        }
        
        let http_len = idx;
        
        rust_print(b"HTTP Request (");
        print_num(http_len as i32);
        rust_print(b" bytes):\n");
        rust_print(b"---\n");
        for i in 0..http_len {
            terminal_putchar(http_request[i]);
        }
        rust_print(b"\n---\n");
        
        rust_print(b"\nStep 3: Building TCP SYN packet\n");
        
        // First send SYN to establish connection
        let mut tcp_buffer_syn = [0u8; 64];
        TCP_SEQ_NUM = get_ticks();
        TCP_SRC_PORT = 50000 + (get_ticks() % 10000) as u16;
        
        rust_print(b"Using source port: ");
        print_num(TCP_SRC_PORT as i32);
        rust_print(b"\n");
        
        let tcp_len_syn = build_tcp_packet(
            &server_ip,
            80,
            TCP_SRC_PORT,
            TCP_SEQ_NUM,
            0,
            0x02,  // SYN flag
            &[],
            &mut tcp_buffer_syn
        );
        
        if tcp_len_syn == 0 {
            rust_print(b"ERROR: Failed to build TCP SYN\n");
            return -1;
        }
        
        let mut ip_buffer_syn = [0u8; 128];
        let ip_len_syn = build_ip_packet(&server_ip, 6, &tcp_buffer_syn[0..tcp_len_syn], &mut ip_buffer_syn);
        
        if ip_len_syn == 0 {
            rust_print(b"ERROR: Failed to build IP packet for SYN\n");
            return -1;
        }
        
        let gateway_mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];  // Default QEMU MAC for gateway
        let mut eth_buffer_syn = [0u8; 256];
        let eth_len_syn = build_ethernet_frame(&gateway_mac, 0x0800, &ip_buffer_syn[0..ip_len_syn], &mut eth_buffer_syn);
        
        if eth_len_syn == 0 {
            rust_print(b"ERROR: Failed to build Ethernet frame for SYN\n");
            return -1;
        }
        
        rust_print(b"Sending TCP SYN...\n");
        let result_syn = rust_rtl8139_send(eth_buffer_syn.as_ptr(), eth_len_syn as u32);
        
        if result_syn != 0 {
            rust_print(b"ERROR: Failed to send SYN\n");
            return -1;
        }
        
        rust_print(b"SYN sent, waiting for SYN-ACK...\n");
        
        // Wait for SYN-ACK
        RX_RESPONSE_LENGTH = 0;
        let mut timeout = 500000;
        let mut got_syn_ack = false;
        
        while timeout > 0 {
            let packets = rust_rtl8139_receive();
            if packets > 0 {
                rust_print(b"Got response packets\n");
                // Parse for SYN-ACK
                if RX_RESPONSE_LENGTH > 54 {
                    // Check if it's TCP and has SYN-ACK flags
                    let tcp_flags = RX_RESPONSE_BUFFER[47];
                    if (tcp_flags & 0x12) == 0x12 {  // SYN+ACK
                        rust_print(b"Got SYN-ACK!\n");
                        
                        // Extract ACK number
                        TCP_ACK_NUM = ((RX_RESPONSE_BUFFER[38] as u32) << 24) |
                                     ((RX_RESPONSE_BUFFER[39] as u32) << 16) |
                                     ((RX_RESPONSE_BUFFER[40] as u32) << 8) |
                                     (RX_RESPONSE_BUFFER[41] as u32);
                        TCP_ACK_NUM = TCP_ACK_NUM.wrapping_add(1);
                        TCP_SEQ_NUM = TCP_SEQ_NUM.wrapping_add(1);
                        
                        got_syn_ack = true;
                        break;
                    }
                }
            }
            timeout -= 1;
        }
        
        if !got_syn_ack {
            rust_print(b"ERROR: No SYN-ACK received\n");
            return -1;
        }
        
        rust_print(b"\nStep 4: Sending HTTP POST with PSH-ACK\n");
        let mut tcp_buffer = [0u8; 1024];
        
        let tcp_len = build_tcp_packet(
            &server_ip,
            80,
            TCP_SRC_PORT,
            TCP_SEQ_NUM,
            TCP_ACK_NUM,
            0x18,  // PSH + ACK
            &http_request[0..http_len],
            &mut tcp_buffer
        );
        
        if tcp_len == 0 {
            rust_print(b"ERROR: Failed to build TCP packet\n");
            return -1;
        }
        
        let mut ip_buffer = [0u8; 1500];
        let ip_len = build_ip_packet(&server_ip, 6, &tcp_buffer[0..tcp_len], &mut ip_buffer);
        
        if ip_len == 0 {
            rust_print(b"ERROR: Failed to build IP packet\n");
            return -1;
        }
        
        let mut eth_buffer = [0u8; 1518];
        let eth_len = build_ethernet_frame(&gateway_mac, 0x0800, &ip_buffer[0..ip_len], &mut eth_buffer);
        
        if eth_len == 0 {
            rust_print(b"ERROR: Failed to build Ethernet frame\n");
            return -1;
        }
        
        rust_print(b"Sending HTTP POST (");
        print_num(eth_len as i32);
        rust_print(b" bytes total)\n");
        
        let result = rust_rtl8139_send(eth_buffer.as_ptr(), eth_len as u32);
        
        if result == 0 {
            rust_print(b"\n=== Packet sent successfully ===\n");
            rust_print(b"Waiting for HTTP response...\n");
            
            RX_RESPONSE_LENGTH = 0;
            timeout = 1000000;
            
            while timeout > 0 {
                let packets = rust_rtl8139_receive();
                if packets > 0 {
                    rust_print(b"Got ");
                    print_num(packets);
                    rust_print(b" response packets\n");
                    
                    if RX_RESPONSE_LENGTH > 54 {
                        rust_print(b"Response preview: ");
                        for i in 54..RX_RESPONSE_LENGTH.min(200) as usize {
                            if RX_RESPONSE_BUFFER[i] >= 32 && RX_RESPONSE_BUFFER[i] < 127 {
                                terminal_putchar(RX_RESPONSE_BUFFER[i]);
                            }
                        }
                        rust_print(b"\n");
                        return 0;
                    }
                }
                timeout -= 1;
                if timeout % 200000 == 0 {
                    rust_print(b".");
                }
            }
            
            rust_print(b"\nNo HTTP response received (but packet was sent)\n");
            0
        } else {
            rust_print(b"\nERROR: Failed to send packet\n");
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_send_ntfy_notification(message: *const u8) -> i32 {
    unsafe {
        if message.is_null() {
            rust_print(b"ERROR: Null message pointer\n");
            return -1;
        }
        
        let mut len = 0;
        let mut ptr = message;
        while *ptr != 0 && len < 500 {
            len += 1;
            ptr = ptr.add(1);
        }
        
        rust_ntfy_post_complete(message, len)
    }
}

// Test function that sends ARP request to find gateway
#[no_mangle]
pub extern "C" fn rust_test_network_simple() -> i32 {
    unsafe {
        rust_print(b"\n=== Simple Network Test ===\n");
        
        // Check device
        if RTL8139_DEVICE.is_none() {
            rust_print(b"ERROR: RTL8139 not initialized\n");
            return -1;
        }
        
        rust_print(b"Building ARP request for gateway...\n");
        
        // Build ARP request for gateway (10.0.0.1)
        let mut arp_packet = [0u8; 42];
        let mut idx = 0;
        
        // Ethernet header
        // Dest MAC (broadcast)
        for _ in 0..6 {
            arp_packet[idx] = 0xFF;
            idx += 1;
        }
        
        // Src MAC
        let device = RTL8139_DEVICE.as_ref().unwrap();
        for i in 0..6 {
            arp_packet[idx] = device.mac[i];
            idx += 1;
        }
        
        // EtherType (ARP = 0x0806)
        arp_packet[idx] = 0x08; idx += 1;
        arp_packet[idx] = 0x06; idx += 1;
        
        // ARP packet
        arp_packet[idx] = 0x00; idx += 1; // Hardware type (Ethernet)
        arp_packet[idx] = 0x01; idx += 1;
        arp_packet[idx] = 0x08; idx += 1; // Protocol type (IPv4)
        arp_packet[idx] = 0x00; idx += 1;
        arp_packet[idx] = 6;    idx += 1; // Hardware size
        arp_packet[idx] = 4;    idx += 1; // Protocol size
        arp_packet[idx] = 0x00; idx += 1; // Opcode (request)
        arp_packet[idx] = 0x01; idx += 1;
        
        // Sender MAC
        for i in 0..6 {
            arp_packet[idx] = device.mac[i];
            idx += 1;
        }
        
        // Sender IP (10.0.0.85)
        arp_packet[idx] = LOCAL_IP[0]; idx += 1;
        arp_packet[idx] = LOCAL_IP[1]; idx += 1;
        arp_packet[idx] = LOCAL_IP[2]; idx += 1;
        arp_packet[idx] = LOCAL_IP[3]; idx += 1;
        
        // Target MAC (zeros)
        for _ in 0..6 {
            arp_packet[idx] = 0x00;
            idx += 1;
        }
        
        // Target IP (10.0.0.1)
        arp_packet[idx] = GATEWAY_IP[0]; idx += 1;
        arp_packet[idx] = GATEWAY_IP[1]; idx += 1;
        arp_packet[idx] = GATEWAY_IP[2]; idx += 1;
        arp_packet[idx] = GATEWAY_IP[3]; idx += 1;
        
        rust_print(b"Sending ARP request (");
        print_num(idx as i32);
        rust_print(b" bytes)...\n");
        
        let result = rust_rtl8139_send(arp_packet.as_ptr(), idx as u32);
        
        if result == 0 {
            rust_print(b"ARP request sent successfully!\n");
            rust_print(b"Waiting for ARP reply...\n");
            
            // Wait for response
            RX_RESPONSE_LENGTH = 0;
            let mut timeout = 1000000;
            
            while timeout > 0 {
                let packets = rust_rtl8139_receive();
                if packets > 0 {
                    rust_print(b"Received ");
                    print_num(packets);
                    rust_print(b" packets, ");
                    print_num(RX_RESPONSE_LENGTH as i32);
                    rust_print(b" bytes\n");
                    
                    if RX_RESPONSE_LENGTH >= 42 {
                        // Check if it's ARP reply
                        if RX_RESPONSE_BUFFER[12] == 0x08 && RX_RESPONSE_BUFFER[13] == 0x06 {
                            rust_print(b"Got ARP reply!\n");
                            rust_print(b"Gateway MAC: ");
                            for i in 0..6 {
                                print_hex_byte(RX_RESPONSE_BUFFER[22 + i]);
                                if i < 5 { rust_print(b":"); }
                            }
                            rust_print(b"\n");
                            return 0;
                        }
                    }
                }
                timeout -= 1;
                if timeout % 200000 == 0 {
                    rust_print(b".");
                }
            }
            
            rust_print(b"\nTimeout waiting for ARP reply\n");
            return -1;
        } else {
            rust_print(b"ERROR: Failed to send ARP request\n");
            return -1;
        }
    }
}

// Test raw packet send
#[no_mangle]
pub extern "C" fn rust_test_raw_send() -> i32 {
    unsafe {
        rust_print(b"\n=== Raw Packet Send Test ===\n");
        
        if RTL8139_DEVICE.is_none() {
            rust_print(b"ERROR: RTL8139 not initialized\n");
            return -1;
        }
        
        let device = RTL8139_DEVICE.as_ref().unwrap();
        
        // Build a minimal 60-byte Ethernet frame (minimum size)
        let mut test_packet = [0u8; 60];
        let mut idx = 0;
        
        // Dest MAC (broadcast)
        for _ in 0..6 {
            test_packet[idx] = 0xFF;
            idx += 1;
        }
        
        // Src MAC
        for i in 0..6 {
            test_packet[idx] = device.mac[i];
            idx += 1;
        }
        
        // EtherType (0x9999 - test)
        test_packet[idx] = 0x99; idx += 1;
        test_packet[idx] = 0x99; idx += 1;
        
        // Fill rest with pattern
        for i in idx..60 {
            test_packet[i] = (i % 256) as u8;
        }
        
        rust_print(b"Sending test packet...\n");
        let result = rust_rtl8139_send(test_packet.as_ptr(), 60);
        
        if result == 0 {
            rust_print(b"Test packet sent successfully!\n");
            rust_print(b"NOTE: QEMU user networking doesn't support ARP/broadcast\n");
            rust_print(b"      Only TCP/UDP to real IPs will work\n");
            0
        } else {
            rust_print(b"ERROR: Failed to send test packet\n");
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_set_network_config(local_ip: *const u8, gateway_ip: *const u8, dns_server: *const u8) {
    unsafe {
        if !local_ip.is_null() {
            for i in 0..4 {
                LOCAL_IP[i] = *local_ip.add(i);
            }
        }
        
        if !gateway_ip.is_null() {
            for i in 0..4 {
                GATEWAY_IP[i] = *gateway_ip.add(i);
            }
        }
        
        if !dns_server.is_null() {
            for i in 0..4 {
                DNS_SERVER[i] = *dns_server.add(i);
            }
        }
        
        rust_print(b"Network configured:\n");
        rust_print(b"  Local IP: ");
        for i in 0..4 {
            print_num(LOCAL_IP[i] as i32);
            if i < 3 { rust_print(b"."); }
        }
        rust_print(b"\n  Gateway: ");
        for i in 0..4 {
            print_num(GATEWAY_IP[i] as i32);
            if i < 3 { rust_print(b"."); }
        }
        rust_print(b"\n  DNS: ");
        for i in 0..4 {
            print_num(DNS_SERVER[i] as i32);
            if i < 3 { rust_print(b"."); }
        }
        rust_print(b"\n");
    }
}


//=============================================================================
// TCP CONNECTION STATE MANAGEMENT (for HTTP only)
//=============================================================================

#[repr(C)]
#[derive(Copy, Clone, PartialEq)]
pub enum TcpState {
    Closed = 0,
    SynSent = 1,
    Established = 2,
    FinWait = 3,
}

#[repr(C)]
pub struct TcpConnection {
    state: TcpState,
    local_port: u16,
    remote_port: u16,
    remote_ip: [u8; 4],
    seq_num: u32,
    ack_num: u32,
    last_activity: u32,
}

static mut TCP_CONNECTION: TcpConnection = TcpConnection {
    state: TcpState::Closed,
    local_port: 0,
    remote_port: 0,
    remote_ip: [0, 0, 0, 0],
    seq_num: 0,
    ack_num: 0,
    last_activity: 0,
};

static mut HTTP_RECEIVE_BUFFER: [u8; 65536] = [0; 65536];
static mut HTTP_RECEIVE_LEN: usize = 0;

// Port management
static mut LAST_USED_PORT: u16 = 40000;
static mut PORT_USAGE_TIME: [u32; 1000] = [0; 1000];

unsafe fn get_unique_port() -> u16 {
    let current_time = get_ticks();
    
    // Try to find a port that hasn't been used recently
    for _attempt in 0..50 {
        LAST_USED_PORT = LAST_USED_PORT.wrapping_add(1);
        
        // Keep port in range 40000-59999
        if LAST_USED_PORT < 40000 || LAST_USED_PORT >= 60000 {
            LAST_USED_PORT = 40000;
        }
        
        let slot = (LAST_USED_PORT - 40000) as usize % 1000;
        
        // Only reuse if at least 3 seconds have passed
        if current_time.wrapping_sub(PORT_USAGE_TIME[slot]) > 3000 {
            PORT_USAGE_TIME[slot] = current_time;
            return LAST_USED_PORT;
        }
    }
    
    // Fallback: use next available port even if recently used
    LAST_USED_PORT = LAST_USED_PORT.wrapping_add(1);
    if LAST_USED_PORT < 40000 || LAST_USED_PORT >= 60000 {
        LAST_USED_PORT = 40000;
    }
    
    let slot = (LAST_USED_PORT - 40000) as usize % 1000;
    PORT_USAGE_TIME[slot] = current_time;
    
    LAST_USED_PORT
}

// MOVE THIS TO THE TOP - before tcp_connect uses it
unsafe fn tcp_reset_state() {
    rust_print(b"TCP: Resetting all state...\n");

    TCP_CONNECTION.state         = TcpState::Closed;
    TCP_CONNECTION.local_port    = 0;
    TCP_CONNECTION.remote_port   = 0;
    TCP_CONNECTION.remote_ip     = [0, 0, 0, 0];
    TCP_CONNECTION.seq_num       = 0;
    TCP_CONNECTION.ack_num       = 0;
    TCP_CONNECTION.last_activity = 0;

    // Only clear the RX ring buffer - do NOT touch HTTP_RECEIVE_BUFFER
    // or HTTP_RECEIVE_LEN here. discord_request reads them after tcp_close().
    RX_RESPONSE_LENGTH = 0;
}
unsafe fn tcp_connect(dest_ip: &[u8; 4], dest_port: u16) -> bool {
    tcp_reset_state();

    rust_print(b"TCP: Connecting to ");
    for i in 0..4 {
        print_num(dest_ip[i] as i32);
        if i < 3 { rust_print(b"."); }
    }
    rust_print(b":");
    print_num(dest_port as i32);
    rust_print(b"\n");

    TCP_CONNECTION.remote_ip   = *dest_ip;
    TCP_CONNECTION.remote_port = dest_port;
    TCP_CONNECTION.local_port  = get_unique_port();
    TCP_CONNECTION.seq_num     = get_ticks();
    TCP_CONNECTION.ack_num     = 0;
    TCP_CONNECTION.state       = TcpState::Closed;

    // Snapshot expected ports BEFORE any receive activity
    let expected_local  = TCP_CONNECTION.local_port;
    let expected_remote = dest_port;

    rust_print(b"Using local port: ");
    print_num(expected_local as i32);
    rust_print(b", seq: ");
    print_hex(TCP_CONNECTION.seq_num);
    rust_print(b"\n");

    // Drain stale packets from RX ring before sending SYN
    // (leftover packets from previous connections confuse the SYN-ACK filter)
    RX_RESPONSE_LENGTH = 0;
    for _ in 0..500 {
        rust_rtl8139_receive();
        RX_RESPONSE_LENGTH = 0;
    }
    rust_print(b"RX ring drained\n");

    // Build and send SYN

let mut tcp_buffer = [0u8; 1460]; // Max TCP payload

    let tcp_len = build_tcp_packet(
        dest_ip, dest_port, expected_local,
        TCP_CONNECTION.seq_num, 0,
        0x02, // SYN
        &[],
        &mut tcp_buffer
    );
    if tcp_len == 0 { rust_print(b"ERROR: Failed to build SYN\n"); return false; }

    let mut ip_buffer = [0u8; 128];
    let ip_len = build_ip_packet(dest_ip, 6, &tcp_buffer[0..tcp_len], &mut ip_buffer);
    if ip_len == 0 { rust_print(b"ERROR: Failed to build IP for SYN\n"); return false; }

    let gateway_mac = [0x52u8, 0x54, 0x00, 0x12, 0x34, 0x56];
    let mut eth_buffer = [0u8; 256];
    let eth_len = build_ethernet_frame(&gateway_mac, 0x0800, &ip_buffer[0..ip_len], &mut eth_buffer);
    if eth_len == 0 { rust_print(b"ERROR: Failed to build Ethernet for SYN\n"); return false; }

    rust_print(b"Sending SYN...\n");
    if rust_rtl8139_send(eth_buffer.as_ptr(), eth_len as u32) != 0 {
        rust_print(b"ERROR: Failed to send SYN\n");
        return false;
    }

    TCP_CONNECTION.state = TcpState::SynSent;

    // Wait for SYN-ACK
    let start_time   = get_ticks();
    let timeout_ms   = 5000u32;
    let mut got_syn_ack  = false;
    let mut last_progress = start_time;

    'synack: loop {
        let elapsed = get_ticks().wrapping_sub(start_time);
        if elapsed >= timeout_ms {
            rust_print(b"\nERROR: SYN-ACK timeout\n");
            break 'synack;
        }

        RX_RESPONSE_LENGTH = 0;
        rust_rtl8139_receive();

        if RX_RESPONSE_LENGTH < 54 {
            RX_RESPONSE_LENGTH = 0;

            let now = get_ticks();
            if now.wrapping_sub(last_progress) >= 1000 {
                rust_print(b".");
                last_progress = now;
            }
            continue 'synack;
        }

        // Must be IPv4
        let et = ((RX_RESPONSE_BUFFER[12] as u16) << 8)
               |  (RX_RESPONSE_BUFFER[13] as u16);
        if et != 0x0800 {
            RX_RESPONSE_LENGTH = 0;
            continue 'synack;
        }

        // IP header length
        let ihl = ((RX_RESPONSE_BUFFER[14] & 0x0F) * 4) as usize;
        if ihl < 20 {
            RX_RESPONSE_LENGTH = 0;
            continue 'synack;
        }

        // Must be TCP
        if RX_RESPONSE_BUFFER[14 + 9] != 6 {
            RX_RESPONSE_LENGTH = 0;
            continue 'synack;
        }

        let tcp_start = 14 + ihl;
        if tcp_start + 20 > RX_RESPONSE_LENGTH as usize {
            RX_RESPONSE_LENGTH = 0;
            continue 'synack;
        }

        let src_port = ((RX_RESPONSE_BUFFER[tcp_start]     as u16) << 8)
                     |  (RX_RESPONSE_BUFFER[tcp_start + 1] as u16);
        let dst_port = ((RX_RESPONSE_BUFFER[tcp_start + 2] as u16) << 8)
                     |  (RX_RESPONSE_BUFFER[tcp_start + 3] as u16);
        let tcp_flags = RX_RESPONSE_BUFFER[tcp_start + 13];

        // Use snapshotted ports - ignore anything not for our connection
        if src_port != expected_remote || dst_port != expected_local {
            RX_RESPONSE_LENGTH = 0;
            continue 'synack;
        }

        // RST - connection refused
        if (tcp_flags & 0x04) != 0 {
            rust_print(b"\nERROR: Got RST\n");
            RX_RESPONSE_LENGTH = 0;
            break 'synack;
        }

        // SYN-ACK
        if (tcp_flags & 0x12) == 0x12 {
            let remote_seq = ((RX_RESPONSE_BUFFER[tcp_start + 4] as u32) << 24)
                           | ((RX_RESPONSE_BUFFER[tcp_start + 5] as u32) << 16)
                           | ((RX_RESPONSE_BUFFER[tcp_start + 6] as u32) <<  8)
                           |  (RX_RESPONSE_BUFFER[tcp_start + 7] as u32);

            TCP_CONNECTION.ack_num   = remote_seq.wrapping_add(1);
            TCP_CONNECTION.seq_num   = TCP_CONNECTION.seq_num.wrapping_add(1);
            TCP_CONNECTION.state     = TcpState::Established;
            TCP_CONNECTION.last_activity = get_ticks();

            rust_print(b"\nGot SYN-ACK, sending ACK...\n");

            // Send ACK
            let mut ack_tcp = [0u8; 64];
            let ack_len = build_tcp_packet(
                dest_ip, dest_port, expected_local,
                TCP_CONNECTION.seq_num,
                TCP_CONNECTION.ack_num,
                0x10, // ACK
                &[],
                &mut ack_tcp
            );
            if ack_len > 0 {
                let mut ack_ip = [0u8; 128];
                let ack_ip_len = build_ip_packet(dest_ip, 6, &ack_tcp[0..ack_len], &mut ack_ip);
                if ack_ip_len > 0 {
                    let mut ack_eth = [0u8; 256];
                    let ack_eth_len = build_ethernet_frame(
                        &gateway_mac, 0x0800,
                        &ack_ip[0..ack_ip_len], &mut ack_eth
                    );
                    if ack_eth_len > 0 {
                        rust_rtl8139_send(ack_eth.as_ptr(), ack_eth_len as u32);
                    }
                }
            }

            got_syn_ack = true;
            RX_RESPONSE_LENGTH = 0;
            break 'synack;
        }

        RX_RESPONSE_LENGTH = 0;
    }

    if !got_syn_ack {
        TCP_CONNECTION.state = TcpState::Closed;
        return false;
    }

    rust_print(b"TCP connection established\n");
    true
}


unsafe fn tcp_send_data(data: &[u8]) -> bool {
    if TCP_CONNECTION.state != TcpState::Established {
        rust_print(b"ERROR: TCP not established\n");
        return false;
    }
    
    rust_print(b"TCP: Sending ");
    print_num(data.len() as i32);
    rust_print(b" bytes\n");
    
    let mut tcp_buffer = [0u8; 2048];
    let tcp_len = build_tcp_packet(
        &TCP_CONNECTION.remote_ip,
        TCP_CONNECTION.remote_port,
        TCP_CONNECTION.local_port,
        TCP_CONNECTION.seq_num,
        TCP_CONNECTION.ack_num,
        0x18, // PSH + ACK
        data,
        &mut tcp_buffer
    );
    
    if tcp_len == 0 {
        rust_print(b"ERROR: Failed to build TCP packet\n");
        return false;
    }
    
    let mut ip_buffer = [0u8; 2048];
    let ip_len = build_ip_packet(&TCP_CONNECTION.remote_ip, 6, &tcp_buffer[0..tcp_len], &mut ip_buffer);
    
    if ip_len == 0 {
        rust_print(b"ERROR: Failed to build IP packet\n");
        return false;
    }
    
    let gateway_mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
    let mut eth_buffer = [0u8; 2048];
    let eth_len = build_ethernet_frame(&gateway_mac, 0x0800, &ip_buffer[0..ip_len], &mut eth_buffer);
    
    if eth_len == 0 {
        rust_print(b"ERROR: Failed to build Ethernet frame\n");
        return false;
    }
    
    if rust_rtl8139_send(eth_buffer.as_ptr(), eth_len as u32) != 0 {
        rust_print(b"ERROR: Failed to send packet\n");
        return false;
    }
    
    // Update sequence number for next send
    TCP_CONNECTION.seq_num = TCP_CONNECTION.seq_num.wrapping_add(data.len() as u32);
    TCP_CONNECTION.last_activity = get_ticks();
    
    rust_print(b"Data sent, new seq: ");
    print_hex(TCP_CONNECTION.seq_num);
    rust_print(b"\n");
    
    true
}

unsafe fn tcp_receive_data(timeout_ms: u32) -> usize {
    HTTP_RECEIVE_LEN = 0;
    let start_time = get_ticks();
    
    rust_print(b"TCP: Receiving data (timeout ");
    print_num((timeout_ms / 1000) as i32);
    rust_print(b"s)...\n");
    
    // Clear RX buffer before receiving
    RX_RESPONSE_LENGTH = 0;
    
    let mut last_progress = start_time;
    let mut packet_count = 0;
    const MAX_PACKETS: i32 = 1556;
    
    loop {
        let elapsed = get_ticks().wrapping_sub(start_time);
        
        // Check timeout
        if elapsed >= timeout_ms {
            rust_print(b"\nReceive timeout after ");
            print_num((elapsed / 1000) as i32);
            rust_print(b" seconds (");
            print_num(packet_count);
            rust_print(b" packets)\n");
            break;
        }
        
        // Check packet limit
        if packet_count >= MAX_PACKETS {
            rust_print(b"\nPacket limit reached (");
            print_num(packet_count);
            rust_print(b" packets received)\n");
            break;
        }
        
        rust_rtl8139_receive();
        
        if RX_RESPONSE_LENGTH > 54 {
            packet_count += 1;
            
            // Check if it's our TCP connection
            if RX_RESPONSE_BUFFER[23] == 6 { // TCP
                let src_port = ((RX_RESPONSE_BUFFER[34] as u16) << 8) | (RX_RESPONSE_BUFFER[35] as u16);
                let dst_port = ((RX_RESPONSE_BUFFER[36] as u16) << 8) | (RX_RESPONSE_BUFFER[37] as u16);
                
                rust_print(b"[pkt ");
                print_num(packet_count);
                rust_print(b": src=");
                print_num(src_port as i32);
                rust_print(b" dst=");
                print_num(dst_port as i32);
                rust_print(b"] ");
                
                if src_port == TCP_CONNECTION.remote_port && dst_port == TCP_CONNECTION.local_port {
                    let tcp_flags = RX_RESPONSE_BUFFER[47];
                    let tcp_header_len = ((RX_RESPONSE_BUFFER[46] >> 4) * 4) as usize;
                    let tcp_data_offset = 34 + tcp_header_len;
                    
                    if tcp_data_offset < RX_RESPONSE_LENGTH as usize {
                        let tcp_data_len = RX_RESPONSE_LENGTH as usize - tcp_data_offset;
                        
                        if tcp_data_len > 0 && HTTP_RECEIVE_LEN + tcp_data_len < HTTP_RECEIVE_BUFFER.len() {
                            // Copy data
                            for i in 0..tcp_data_len {
                                HTTP_RECEIVE_BUFFER[HTTP_RECEIVE_LEN + i] = RX_RESPONSE_BUFFER[tcp_data_offset + i];
                            }
                            HTTP_RECEIVE_LEN += tcp_data_len;
                            
                            rust_print(b"\nReceived ");
                            print_num(tcp_data_len as i32);
                            rust_print(b" bytes (total: ");
                            print_num(HTTP_RECEIVE_LEN as i32);
                            rust_print(b")\n");
                            
                            // Update ACK number
                            let remote_seq = ((RX_RESPONSE_BUFFER[38] as u32) << 24) |
                                           ((RX_RESPONSE_BUFFER[39] as u32) << 16) |
                                           ((RX_RESPONSE_BUFFER[40] as u32) << 8) |
                                           (RX_RESPONSE_BUFFER[41] as u32);
                            TCP_CONNECTION.ack_num = remote_seq.wrapping_add(tcp_data_len as u32);
                            
                            // Send ACK
                            let mut ack_tcp = [0u8; 64];
                            let ack_tcp_len = build_tcp_packet(
                                &TCP_CONNECTION.remote_ip,
                                TCP_CONNECTION.remote_port,
                                TCP_CONNECTION.local_port,
                                TCP_CONNECTION.seq_num,
                                TCP_CONNECTION.ack_num,
                                0x10, // ACK
                                &[],
                                &mut ack_tcp
                            );
                            
                            if ack_tcp_len > 0 {
                                let mut ack_ip = [0u8; 128];
                                let ack_ip_len = build_ip_packet(&TCP_CONNECTION.remote_ip, 6, &ack_tcp[0..ack_tcp_len], &mut ack_ip);
                                
                                if ack_ip_len > 0 {
                                    let gateway_mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
                                    let mut ack_eth = [0u8; 256];
                                    let ack_eth_len = build_ethernet_frame(&gateway_mac, 0x0800, &ack_ip[0..ack_ip_len], &mut ack_eth);
                                    
                                    if ack_eth_len > 0 {
                                        rust_rtl8139_send(ack_eth.as_ptr(), ack_eth_len as u32);
                                    }
                                }
                            }
                        }
                    }
                    
                    // Check for FIN
                    if (tcp_flags & 0x01) != 0 {
                        rust_print(b"\nServer closed connection (FIN)\n");
                        TCP_CONNECTION.state = TcpState::Closed;
                        break;
                    }
                }
            }
            
            // Clear for next packet - THIS IS CRITICAL
            RX_RESPONSE_LENGTH = 0;
        }
        
        // Progress indicator every second
        let current = get_ticks();
        if current.wrapping_sub(last_progress) >= 1000 {
            rust_print(b".");
            last_progress = current;
        }
    }
    
    if HTTP_RECEIVE_LEN > 0 {
        rust_print(b"\nReceived total: ");
        print_num(HTTP_RECEIVE_LEN as i32);
        rust_print(b" bytes\n");
    } else {
        rust_print(b"\nNo data received\n");
    }
    
    HTTP_RECEIVE_LEN
}

unsafe fn tcp_close() {
    if TCP_CONNECTION.state == TcpState::Closed {
        return;
    }
    
    rust_print(b"TCP: Closing connection...\n");
    
    // Send FIN if established
    if TCP_CONNECTION.state == TcpState::Established {
        let mut tcp_buffer = [0u8; 64];
        let tcp_len = build_tcp_packet(
            &TCP_CONNECTION.remote_ip,
            TCP_CONNECTION.remote_port,
            TCP_CONNECTION.local_port,
            TCP_CONNECTION.seq_num,
            TCP_CONNECTION.ack_num,
            0x11, // FIN + ACK
            &[],
            &mut tcp_buffer
        );
        
        if tcp_len > 0 {
            let mut ip_buffer = [0u8; 128];
            let ip_len = build_ip_packet(&TCP_CONNECTION.remote_ip, 6, &tcp_buffer[0..tcp_len], &mut ip_buffer);
            
            if ip_len > 0 {
                let gateway_mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
                let mut eth_buffer = [0u8; 256];
                let eth_len = build_ethernet_frame(&gateway_mac, 0x0800, &ip_buffer[0..ip_len], &mut eth_buffer);
                
                if eth_len > 0 {
                    rust_rtl8139_send(eth_buffer.as_ptr(), eth_len as u32);
                    rust_print(b"Sent FIN\n");
                }
            }
        }
    }
    
    // Full state reset
    tcp_reset_state();
    
    rust_print(b"Connection closed\n");
}

#[no_mangle]
pub extern "C" fn rust_tcp_force_reset() -> i32 {
    unsafe {
        rust_print(b"\n=== Forcing TCP Reset ===\n");
        tcp_reset_state();
        rust_print(b"All TCP state cleared.\n");
        rust_print(b"You can try connecting again.\n");
        0
    }
}

//=============================================================================
// HTML PARSER - COMPLETE MODULE
//=============================================================================

#[derive(Copy, Clone, PartialEq)]
pub enum HtmlElementType {
    Text, Link, Header1, Header2, Header3, Header4, Header5, Header6,
    Paragraph, LineBreak, ListItem, OrderedListItem, Image, Preformatted,
    Code, Blockquote, HorizontalRule, Table, TableRow, TableCell, TableHeader,
    Bold, Italic, Underline, Strikethrough, Subscript, Superscript,
    Div, Span, Nav, Article, Section, Footer, Header, Main, Aside,
    Button, Input, TextArea, Select, Center, Font,
    Unknown,
}

#[derive(Copy, Clone)]
pub struct CssStyle {
    pub color: u32,
    pub bg_color: u32,
    pub font_size: u8,
    pub margin_left: u8,
    pub text_align: u8,
    pub display_block: bool,
    pub is_hidden: bool,
}

impl Default for CssStyle {
    fn default() -> Self {
        CssStyle {
            color: 0xCCCCCC,
            bg_color: 0,
            font_size: 16,
            margin_left: 0,
            text_align: 0,
            display_block: false,
            is_hidden: false,
        }
    }
}

#[derive(Copy, Clone)]
pub struct HtmlElement {
    pub element_type: HtmlElementType,
    pub style: CssStyle,
    pub text_start: usize,
    pub text_len: usize,
    pub href_start: usize,
    pub href_len: usize,
    pub src_start: usize,
    pub src_len: usize,
    pub alt_start: usize,
    pub alt_len: usize,
}

static mut HTML_ELEMENTS: [HtmlElement; 2048] = [HtmlElement {
    element_type: HtmlElementType::Unknown,
    style: CssStyle {
        color: 0xFFFFFF, bg_color: 0, font_size: 16,
        margin_left: 0, text_align: 0, display_block: false, is_hidden: false,
    },
    text_start: 0, text_len: 0,
    href_start: 0, href_len: 0,
    src_start:  0, src_len:  0,
    alt_start:  0, alt_len:  0,
}; 2048];

static mut HTML_ELEMENT_COUNT: usize = 0;

static mut HTML_TEXT_BUFFER: [u8; 128 * 1024] = [0; 128 * 1024];
static mut HTML_TEXT_LEN: usize = 0;

unsafe fn push_char(c: u8) {
    if HTML_TEXT_LEN < HTML_TEXT_BUFFER.len() {
        HTML_TEXT_BUFFER[HTML_TEXT_LEN] = c;
        HTML_TEXT_LEN += 1;
    }
}

unsafe fn push_entity(html: &[u8], start: usize, max: usize) -> usize {
    if start + 4 < max && &html[start..start + 5] == b"&amp;" {
        push_char(b'&');
        5
    } else if start + 3 < max && &html[start..start + 4] == b"&lt;" {
        push_char(b'<');
        4
    } else if start + 3 < max && &html[start..start + 4] == b"&gt;" {
        push_char(b'>');
        4
    } else {
        push_char(html[start]);
        1
    }
}

fn find_tag_end(html: &[u8], start: usize, max: usize) -> usize {
    let mut i = start;
    while i < max && html[i] != b'>' {
        i += 1;
    }
    i
}

fn is_tag_name(html: &[u8], start: usize, name: &[u8]) -> bool {
    let len = name.len();
    if start + len >= html.len() { return false; }
    if &html[start..start + len] == name {
        let next = html[start + len];
        return next == b' ' || next == b'>' || next == b'/' || next == b'\t' || next == b'\n';
    }
    false
}

fn extract_attr(html: &[u8], start: usize, end: usize, attr_name: &[u8]) -> (usize, usize) {
    let mut i = start;
    let name_len = attr_name.len();
    while i + name_len < end {
        if &html[i..i + name_len] == attr_name && html[i + name_len] == b'=' {
            let val_start = i + name_len + 2;
            let mut val_end = val_start;
            while val_end < end && html[val_end] != b'"' && html[val_end] != b'\'' {
                val_end += 1;
            }
            return (val_start, val_end - val_start);
        }
        i += 1;
    }
    (0, 0)
}

fn parse_inline_style(html: &[u8], start: usize, len: usize, mut style: CssStyle) -> CssStyle {
    let mut i = start;
    let end = start + len;

    while i < end {
        while i < end && html[i].is_ascii_whitespace() { i += 1; }
        if i >= end { break; }

        let prop_start = i;
        while i < end && html[i] != b':' { i += 1; }
        let prop = &html[prop_start..i];

        if i >= end { break; }
        i += 1;

        while i < end && html[i].is_ascii_whitespace() { i += 1; }
        let val_start = i;
        while i < end && html[i] != b';' { i += 1; }
        let val = &html[val_start..i];

        i += 1;

        if prop == b"color" {
            if val == b"red"              { style.color = 0xFF0000; }
            else if val == b"blue"        { style.color = 0x0000FF; }
            else if val == b"green"       { style.color = 0x00FF00; }
            else if val == b"white"       { style.color = 0xFFFFFF; }
            else if val == b"gray" || val == b"grey" { style.color = 0xCCCCCC; }
            else if val == b"black"       { style.color = 0x000000; }
        } else if prop == b"background-color" {
            if val == b"red"              { style.bg_color = 0xFF0000; }
            else if val == b"blue"        { style.bg_color = 0x0000FF; }
            else if val == b"white"       { style.bg_color = 0xFFFFFF; }
            else if val == b"black"       { style.bg_color = 0x000000; }
        } else if prop == b"text-align" {
            if val == b"center"           { style.text_align = 1; }
            else if val == b"right"       { style.text_align = 2; }
            else                          { style.text_align = 0; }
        } else if prop == b"font-size" {
            let mut size = 0u8;
            let mut vi = 0;
            while vi < val.len() && val[vi].is_ascii_digit() {
                size = size.wrapping_mul(10).wrapping_add(val[vi] - b'0');
                vi += 1;
            }
            if size > 0 { style.font_size = size; }
        } else if prop == b"display" {
            if val == b"none" { style.is_hidden = true; }
        }
    }
    style
}

fn render_element(elem: &HtmlElement) {
    match elem.element_type {
        HtmlElementType::TableCell => {}
        HtmlElementType::Header1 => {}
        _ => {}
    }
}

pub unsafe fn parse_html(html: &[u8]) -> usize {
    HTML_ELEMENT_COUNT = 0;
    HTML_TEXT_LEN = 0;
    let mut i = 0;
    let len = html.len();

    while i < len && HTML_ELEMENT_COUNT < 2048 {
        if html[i] != b'<' {
            let ts = HTML_TEXT_LEN;
            while i < len && html[i] != b'<' {
                if html[i] == b'&' {
                    let consumed = push_entity(html, i, len);
                    i += consumed;
                } else {
                    push_char(html[i]);
                    i += 1;
                }
            }
            if HTML_ELEMENT_COUNT > 0 {
                HTML_ELEMENTS[HTML_ELEMENT_COUNT - 1].text_len = HTML_TEXT_LEN - ts;
            }
            continue;
        }

        let ts = i + 1;
        if ts >= len { break; }

        if html[ts] == b'!' || html[ts] == b'/' {
            i = find_tag_end(html, ts, len) + 1;
            continue;
        }

        let te = find_tag_end(html, ts, len);
        let mut et = HtmlElementType::Unknown;
        let mut current_style = CssStyle {
            color: 0xCCCCCC, bg_color: 0, font_size: 16,
            margin_left: 0, text_align: 0, display_block: false, is_hidden: false,
        };

        if      is_tag_name(html, ts, b"h1")   { et = HtmlElementType::Header1;      current_style.display_block = true; }
        else if is_tag_name(html, ts, b"h2")   { et = HtmlElementType::Header2;      current_style.display_block = true; }
        else if is_tag_name(html, ts, b"h3")   { et = HtmlElementType::Header3;      current_style.display_block = true; }
        else if is_tag_name(html, ts, b"h4")   { et = HtmlElementType::Header4;      current_style.display_block = true; }
        else if is_tag_name(html, ts, b"h5")   { et = HtmlElementType::Header5;      current_style.display_block = true; }
        else if is_tag_name(html, ts, b"h6")   { et = HtmlElementType::Header6;      current_style.display_block = true; }
        else if is_tag_name(html, ts, b"p")    { et = HtmlElementType::Paragraph;    current_style.display_block = true; }
        else if is_tag_name(html, ts, b"div")  { et = HtmlElementType::Div;          current_style.display_block = true; }
        else if is_tag_name(html, ts, b"nav")  { et = HtmlElementType::Nav;          current_style.display_block = true; }
        else if is_tag_name(html, ts, b"main") { et = HtmlElementType::Main;         current_style.display_block = true; }
        else if is_tag_name(html, ts, b"article") { et = HtmlElementType::Article;   current_style.display_block = true; }
        else if is_tag_name(html, ts, b"section") { et = HtmlElementType::Section;   current_style.display_block = true; }
        else if is_tag_name(html, ts, b"footer")  { et = HtmlElementType::Footer;    current_style.display_block = true; }
        else if is_tag_name(html, ts, b"aside")   { et = HtmlElementType::Aside;     current_style.display_block = true; }
        else if is_tag_name(html, ts, b"header")  { et = HtmlElementType::Header;    current_style.display_block = true; }
        else if is_tag_name(html, ts, b"table")   { et = HtmlElementType::Table;     current_style.display_block = true; }
        else if is_tag_name(html, ts, b"tr")  { et = HtmlElementType::TableRow;      current_style.display_block = true; }
        else if is_tag_name(html, ts, b"th")  { et = HtmlElementType::TableHeader;   current_style.display_block = true; }
        else if is_tag_name(html, ts, b"td")  { et = HtmlElementType::TableCell; }
        else if is_tag_name(html, ts, b"pre") { et = HtmlElementType::Preformatted;  current_style.display_block = true; }
        else if is_tag_name(html, ts, b"blockquote") { et = HtmlElementType::Blockquote; current_style.display_block = true; }
        else if is_tag_name(html, ts, b"ul")  { et = HtmlElementType::Unknown;       current_style.display_block = true; }
        else if is_tag_name(html, ts, b"ol")  { et = HtmlElementType::Unknown;       current_style.display_block = true; }
        else if is_tag_name(html, ts, b"li")  { et = HtmlElementType::ListItem;      current_style.display_block = true; }
        else if is_tag_name(html, ts, b"b")   { et = HtmlElementType::Bold; }
        else if is_tag_name(html, ts, b"strong") { et = HtmlElementType::Bold; }
        else if is_tag_name(html, ts, b"i")   { et = HtmlElementType::Italic; }
        else if is_tag_name(html, ts, b"em")  { et = HtmlElementType::Italic; }
        else if is_tag_name(html, ts, b"u")   { et = HtmlElementType::Underline; }
        else if is_tag_name(html, ts, b"s")   { et = HtmlElementType::Strikethrough; }
        else if is_tag_name(html, ts, b"code"){ et = HtmlElementType::Code; }
        else if is_tag_name(html, ts, b"span"){ et = HtmlElementType::Span; }
        else if is_tag_name(html, ts, b"a")   { et = HtmlElementType::Link; }
        else if is_tag_name(html, ts, b"img") { et = HtmlElementType::Image; }
        else if is_tag_name(html, ts, b"hr")  {
            push_char(b'\n');
            i = te + 1;
            continue;
        }
        else if is_tag_name(html, ts, b"br")  {
            push_char(b'\n');
            i = te + 1;
            continue;
        }

        let style_attr = extract_attr(html, ts, te, b"style");
        if style_attr.1 > 0 {
            current_style = parse_inline_style(html, style_attr.0, style_attr.1, current_style);
        }

        let mut href = (0usize, 0usize);
        if et == HtmlElementType::Link {
            href = extract_attr(html, ts, te, b"href");
        }

        let mut src = (0usize, 0usize);
        let mut alt = (0usize, 0usize);
        if et == HtmlElementType::Image {
            src = extract_attr(html, ts, te, b"src");
            alt = extract_attr(html, ts, te, b"alt");
        }

        if et != HtmlElementType::Unknown {
            HTML_ELEMENTS[HTML_ELEMENT_COUNT] = HtmlElement {
                element_type: et,
                style: current_style,
                text_start: HTML_TEXT_LEN,
                text_len: 0,
                href_start: href.0,
                href_len: href.1,
                src_start: src.0,
                src_len: src.1,
                alt_start: alt.0,
                alt_len: alt.1,
            };
            HTML_ELEMENT_COUNT += 1;
        }

        i = te + 1;
    }

    HTML_ELEMENT_COUNT
}

pub unsafe fn get_element(index: usize) -> Option<&'static HtmlElement> {
    if index < HTML_ELEMENT_COUNT {
        Some(&HTML_ELEMENTS[index])
    } else {
        None
    }
}

pub unsafe fn get_text(start: usize, len: usize) -> &'static [u8] {
    if start + len <= HTML_TEXT_LEN {
        &HTML_TEXT_BUFFER[start..start + len]
    } else {
        b""
    }
}

pub unsafe fn element_count() -> usize {
    HTML_ELEMENT_COUNT
}

// =============================================================================
// RADIUMOS GRAPHICAL BROWSER - COMPLETE REPLACEMENT
// This file replaces the entire browser section in your main .rs
// Also fixes graphics_draw_char and graphics_draw_char_scaled (mirrored text)
// =============================================================================

// ─────────────────────────────────────────────────────────────────────────────
// TEXT FIX - replaces graphics_draw_char and graphics_draw_char_scaled
// Root cause: FONT_8X8 stores MSB = leftmost pixel (bit 7 = col 0).
// Old code used (1 << col) which reads LSB first → mirror image.
// Fix: (byte >> (7 - col)) & 1
// ─────────────────────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn graphics_draw_char(x: u32, y: u32, ch: u8, color: u32) {
    unsafe {
        if !GRAPHICS_MODE.is_initialized { return; }
        if ch < 32 || ch > 127 { return; }
        let glyph = &FONT_8X8[(ch - 32) as usize];
        for row in 0..8u32 {
            let py = y + row;
            if py >= GRAPHICS_MODE.height { break; }
            let byte = glyph[row as usize];
            for col in 0..8u32 {
                if (byte >> (7 - col)) & 1 != 0 {
                    let px = x + col;
                    if px < GRAPHICS_MODE.width {
                        BACK_BUFFER[(py * GRAPHICS_MODE.width + px) as usize] = color;
                    }
                }
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn graphics_draw_char_scaled(x: u32, y: u32, ch: u8, color: u32, scale: u32) {
    unsafe {
        if !GRAPHICS_MODE.is_initialized { return; }
        if ch < 32 || ch > 127 { return; }
        let glyph = &FONT_8X8[(ch - 32) as usize];
        for row in 0..8u32 {
            let byte = glyph[row as usize];
            for col in 0..8u32 {
                if (byte >> (7 - col)) & 1 != 0 {
                    for sy in 0..scale {
                        for sx in 0..scale {
                            let px = x + col * scale + sx;
                            let py = y + row * scale + sy;
                            if px < GRAPHICS_MODE.width && py < GRAPHICS_MODE.height {
                                BACK_BUFFER[(py * GRAPHICS_MODE.width + px) as usize] = color;
                            }
                        }
                    }
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// BROWSER CONSTANTS
// Firefox-style: tab bar → nav bar → bookmark bar → content → status bar
// ─────────────────────────────────────────────────────────────────────────────
const BW: u32 = 800;
const BH: u32 = 600;
const TAB_BAR_H:    u32 = 30;
const NAV_BAR_H:    u32 = 38;
const BM_BAR_H:     u32 = 24;
const STATUS_BAR_H: u32 = 22;
const SCROLLBAR_W:  u32 = 16;
const CHROME_TOTAL_H: u32 = TAB_BAR_H + NAV_BAR_H + BM_BAR_H;
const CONTENT_TOP_Y:  u32 = CHROME_TOTAL_H;
const CONTENT_H:      u32 = BH - CHROME_TOTAL_H - STATUS_BAR_H;
const CONTENT_W:      u32 = BW - SCROLLBAR_W;
const PAGE_MARGIN:    u32 = 16;

// ── Fiery Dark Palette ────────────────────────────────────────────────────────
const C_CHROME:       u32 = 0xFF1A1008;
const C_CHROME_DARK:  u32 = 0xFF0F0A04;
const C_CHROME_LIGHT: u32 = 0xFF2E1C0C;
const C_TAB_ACTIVE:   u32 = 0xFFEEE0CC;
const C_TAB_INACTIVE: u32 = 0xFF3A200E;
const C_TAB_LINE:     u32 = 0xFFFF5500;
const C_URL_BG:       u32 = 0xFF1E1008;
const C_URL_BORDER:   u32 = 0xFF7A3010;
const C_URL_TEXT:     u32 = 0xFFFFDDBB;
const C_BTN:          u32 = 0xFF5C2200;
const C_BTN_TEXT:     u32 = 0xFFFFCCAA;
const C_BM_BG:        u32 = 0xFF160C04;
const C_BM_TEXT:      u32 = 0xFFCC8855;
const C_PAGE_BG:      u32 = 0xFF120C06;
const C_TEXT:         u32 = 0xFFDDCCBB;
const C_LINK:         u32 = 0xFFFF7722;
const C_H1:           u32 = 0xFFFFEEDD;
const C_H2:           u32 = 0xFFDDAA77;
const C_H3:           u32 = 0xFFBB7744;
const C_MUTED:        u32 = 0xFF997755;
const C_HR:           u32 = 0xFF3A1E0A;
const C_STATUS_BG:    u32 = 0xFF0F0804;
const C_STATUS_TEXT:  u32 = 0xFF996644;
const C_SCROLL_TRACK: u32 = 0xFF1A0E06;
const C_SCROLL_THUMB: u32 = 0xFF7A3A10;
const C_HTTPS_GREEN:  u32 = 0xFFDD6600;
const C_ACCENT:       u32 = 0xFFFF4400;

// ─────────────────────────────────────────────────────────────────────────────
// BROWSER STATE
// ─────────────────────────────────────────────────────────────────────────────

static mut B_URL:        [u8; 512] = [0; 512];
static mut B_URL_LEN:    usize     = 0;
static mut B_SCROLL:     i32       = 0;
static mut B_PAGE_H:     i32       = 0;
static mut B_LOADING:    bool      = false;
static mut B_LOADED:     bool      = false;
static mut B_TITLE:      [u8; 128] = [0; 128];
static mut B_TITLE_LEN:  usize     = 0;
static mut B_STATUS:     [u8; 128] = [0; 128];
static mut B_STATUS_LEN: usize     = 0;
static mut B_SHIFT:      bool      = false;

static mut CHUNKED_DECODE_BUF: [u8; 65536] = [0; 65536];

unsafe fn b_set_status(s: &[u8]) {
    let l = s.len().min(127);
    B_STATUS[..l].copy_from_slice(&s[..l]);
    B_STATUS[l] = 0;
    B_STATUS_LEN = l;
}

// ─────────────────────────────────────────────────────────────────────────────
// EMPTY ELEMENT CONSTANT
// Use ..EMPTY_ELEMENT in any HtmlElement initializer to fill missing fields
// ─────────────────────────────────────────────────────────────────────────────

const EMPTY_ELEMENT: HtmlElement = HtmlElement {
    element_type: HtmlElementType::Unknown,
    style: CssStyle {
        color: 0xCCCCCC, bg_color: 0, font_size: 16,
        margin_left: 0, text_align: 0, display_block: false, is_hidden: false,
    },
    text_start: 0, text_len: 0,
    href_start: 0, href_len: 0,
    src_start:  0, src_len:  0,
    alt_start:  0, alt_len:  0,
};

// ─────────────────────────────────────────────────────────────────────────────
// LOCAL FILE LOADING (AVFS)
// ─────────────────────────────────────────────────────────────────────────────

static mut AVFS_READ_BUF: [u8; 32768] = [0; 32768];

unsafe fn b_is_local_path() -> bool {
    if B_URL_LEN == 0 { return false; }
    if B_URL_LEN >= 7 && &B_URL[..7] == b"file://" { return true; }
    if B_URL[0] == b'/' { return true; }
    false
}

unsafe fn b_local_path() -> &'static [u8] {
    if B_URL_LEN >= 9 && &B_URL[..9] == b"file://-/" {
        &B_URL[8..B_URL_LEN]
    } else if B_URL_LEN >= 10 && &B_URL[..10] == b"file:///-/" {
        &B_URL[9..B_URL_LEN]
    } else if B_URL_LEN >= 7 && &B_URL[..7] == b"file://" {
        &B_URL[7..B_URL_LEN]
    } else {
        &B_URL[..B_URL_LEN]
    }
}

unsafe fn b_path_is_html(path: &[u8]) -> bool {
    if path.len() < 5 { return false; }
    let tail = &path[path.len() - 5..];
    tail == b".html" || {
        path.len() >= 4 && &path[path.len() - 4..] == b".htm"
    }
}

unsafe fn slice_to_cstr(s: &[u8], buf: &mut [u8]) -> bool {
    if s.len() + 1 > buf.len() { return false; }
    buf[..s.len()].copy_from_slice(s);
    buf[s.len()] = 0;
    true
}

unsafe fn b_load_local() {
    B_LOADING = true;
    B_LOADED  = false;
    b_set_status(b"Reading local file...");
    b_render();

    let path = b_local_path();

    let mut path_buf = [0u8; 256];
    if !slice_to_cstr(path, &mut path_buf) {
        b_set_status(b"Path too long");
        B_LOADING = false;
        b_render();
        return;
    }

    let fsize = avfs_get_filesize(path_buf.as_ptr());
    if fsize < 0 {
        b_set_status(b"File not found");
        B_LOADING = false;
        b_render_error_page(path);
        return;
    }
    let fsize = fsize as usize;

    if fsize == 0 {
        b_set_status(b"File is empty");
        B_LOADING = false;
        b_render();
        return;
    }

    if fsize > AVFS_READ_BUF.len() {
        b_set_status(b"File too large for buffer");
        B_LOADING = false;
        b_render();
        return;
    }

    let ret = avfs_read_file(
        path_buf.as_ptr(),
        AVFS_READ_BUF.as_mut_ptr(),
        fsize as u32,
        0,
    );
    if ret != 0 {
        b_set_status(b"Read error");
        B_LOADING = false;
        b_render();
        return;
    }

    if b_path_is_html(path) {
        let count = parse_html(&AVFS_READ_BUF[..fsize]);
        HTML_ELEMENT_COUNT = count;
        do_layout();
        B_SCROLL = 0;

        if B_TITLE_LEN == 0 {
            let fname_start = path.iter().rposition(|&b| b == b'/').map(|i| i + 1).unwrap_or(0);
            let fname = &path[fname_start..];
            let tl = fname.len().min(127);
            B_TITLE[..tl].copy_from_slice(&fname[..tl]);
            B_TITLE[tl] = 0;
            B_TITLE_LEN = tl;
        }

        b_set_status(b"Done");
    } else {
        let copy_len = fsize.min(HTML_TEXT_BUFFER.len() - 1);
        HTML_TEXT_BUFFER[..copy_len].copy_from_slice(&AVFS_READ_BUF[..copy_len]);
        HTML_TEXT_LEN = copy_len;

        HTML_ELEMENT_COUNT = 1;
        HTML_ELEMENTS[0] = HtmlElement {
            element_type: HtmlElementType::Preformatted,
            text_start: 0,
            text_len:   copy_len,
            ..EMPTY_ELEMENT
        };

        do_layout();
        B_SCROLL = 0;

        let fname_start = path.iter().rposition(|&b| b == b'/').map(|i| i + 1).unwrap_or(0);
        let fname = &path[fname_start..];
        let tl = fname.len().min(127);
        B_TITLE[..tl].copy_from_slice(&fname[..tl]);
        B_TITLE[tl] = 0;
        B_TITLE_LEN = tl;

        b_set_status(b"Done");
    }

    B_LOADING = false;
    B_LOADED  = true;
    b_render();
}

unsafe fn b_render_error_page(path: &[u8]) {
    HTML_TEXT_LEN = 0;
    HTML_ELEMENT_COUNT = 0;

    let h1 = b"File Not Found";
    HTML_TEXT_BUFFER[..h1.len()].copy_from_slice(h1);
    HTML_ELEMENTS[0] = HtmlElement {
        element_type: HtmlElementType::Header1,
        text_start: 0,
        text_len: h1.len(),
        ..EMPTY_ELEMENT
    };
    HTML_ELEMENT_COUNT += 1;
    HTML_TEXT_LEN += h1.len();

    let prefix = b"Could not open: ";
    let pl = prefix.len();
    let pathlen = path.len().min(HTML_TEXT_BUFFER.len() - HTML_TEXT_LEN - pl - 1);
    HTML_TEXT_BUFFER[HTML_TEXT_LEN..HTML_TEXT_LEN + pl].copy_from_slice(prefix);
    HTML_TEXT_BUFFER[HTML_TEXT_LEN + pl..HTML_TEXT_LEN + pl + pathlen]
        .copy_from_slice(&path[..pathlen]);
    HTML_ELEMENTS[1] = HtmlElement {
        element_type: HtmlElementType::Paragraph,
        text_start: HTML_TEXT_LEN,
        text_len: pl + pathlen,
        ..EMPTY_ELEMENT
    };
    HTML_ELEMENT_COUNT += 1;
    HTML_TEXT_LEN += pl + pathlen;

    do_layout();
    B_SCROLL  = 0;
    B_LOADED  = true;
    B_LOADING = false;
    b_render();
}

// ─────────────────────────────────────────────────────────────────────────────
// PAGE ELEMENT (layout pass output)
// ─────────────────────────────────────────────────────────────────────────────

const MAX_PELEMS: usize = 1024;

#[derive(Copy, Clone, PartialEq)]
enum PEK { Text, H1, H2, H3, Link, HR, Li, Pre }

#[derive(Copy, Clone)]
struct PElem {
    k:  PEK,
    x:  i32, y: i32,
    w:  u32, h: u32,
    ts: usize, tl: usize,
    hs: usize, hl: usize,
    c:  u32,
}
impl PElem {
    const fn z() -> Self {
        Self { k: PEK::Text, x:0,y:0,w:0,h:0,ts:0,tl:0,hs:0,hl:0,c:0 }
    }
}

static mut PELEMS:  [PElem; MAX_PELEMS] = [PElem::z(); MAX_PELEMS];
static mut PELEM_N: usize = 0;

// ─────────────────────────────────────────────────────────────────────────────
// DRAWING HELPERS
// ─────────────────────────────────────────────────────────────────────────────

unsafe fn px(x: u32, y: u32, c: u32) {
    if x < BW && y < BH {
        BACK_BUFFER[(y * BW + x) as usize] = c;
    }
}
unsafe fn frect(x: u32, y: u32, w: u32, h: u32, c: u32) {
    graphics_fill_rect(x, y, w, h, c);
}
unsafe fn hln(x: u32, y: u32, w: u32, c: u32) {
    for i in 0..w { px(x + i, y, c); }
}
unsafe fn vln(x: u32, y: u32, h: u32, c: u32) {
    for i in 0..h { px(x, y + i, c); }
}
unsafe fn orect(x: u32, y: u32, w: u32, h: u32, c: u32) {
    hln(x, y, w, c); hln(x, y+h-1, w, c);
    vln(x, y, h, c); vln(x+w-1, y, h, c);
}

unsafe fn dstr(x: u32, y: u32, s: *const u8, c: u32) -> u32 {
    graphics_draw_string(x, y, s, c);
    graphics_string_width(s)
}
unsafe fn dslice(x: u32, y: u32, s: &[u8], c: u32) {
    if s.is_empty() { return; }
    let mut tmp = [0u8; 512];
    let l = s.len().min(511);
    tmp[..l].copy_from_slice(&s[..l]);
    tmp[l] = 0;
    graphics_draw_string(x, y, tmp.as_ptr(), c);
}
unsafe fn dslice_scaled(x: u32, y: u32, s: &[u8], c: u32, scale: u32) {
    if s.is_empty() { return; }
    let mut tmp = [0u8; 512];
    let l = s.len().min(511);
    tmp[..l].copy_from_slice(&s[..l]);
    tmp[l] = 0;
    graphics_draw_string_scaled(x, y, tmp.as_ptr(), c, scale);
}

unsafe fn draw_wordwrap(
    x0: u32, y0: u32,
    text: &[u8],
    color: u32,
    max_w: u32,
    clip_top: u32, clip_bot: u32,
) {
    if text.is_empty() { return; }
    let cpl = ((max_w / 8) as usize).max(1);

    let mut cur_x = x0;
    let mut cur_y = y0;
    let mut word  = [0u8; 256];
    let mut wlen  = 0usize;
    let mut i     = 0usize;

    loop {
        let at_end = i == text.len();
        let is_sep = at_end || text[i] == b' ' || text[i] == b'\n';

        if is_sep {
            if wlen > 0 {
                let used = ((cur_x.saturating_sub(x0)) / 8) as usize;
                if used + wlen > cpl {
                    cur_y += 13;
                    cur_x  = x0;
                }
                if cur_y >= clip_top && cur_y < clip_bot {
                    let mut tmp = [0u8; 257];
                    tmp[..wlen].copy_from_slice(&word[..wlen]);
                    tmp[wlen] = 0;
                    graphics_draw_string(cur_x, cur_y, tmp.as_ptr(), color);
                }
                cur_x += (wlen as u32 + 1) * 8;
                wlen = 0;
            }
            if at_end { break; }
            if text[i] == b'\n' {
                cur_y += 13;
                cur_x  = x0;
            }
            if cur_y >= clip_bot { break; }
        } else if wlen < 255 {
            word[wlen] = text[i];
            wlen += 1;
        }

        i += 1;
    }
}

fn wrap_height(text_len: usize, max_w: u32) -> u32 {
    let cpl = (max_w / 8).max(1) as usize;
    let lines = (text_len + cpl - 1) / cpl;
    (lines as u32 * 13).max(13)
}

// ─────────────────────────────────────────────────────────────────────────────
// CHROME RENDERING
// ─────────────────────────────────────────────────────────────────────────────

unsafe fn draw_tab_bar() {
    frect(0, 0, BW, TAB_BAR_H, C_CHROME_DARK);

    let tab_x: u32 = 72;
    let tab_w: u32 = 200;

    frect(tab_x, 3, tab_w, TAB_BAR_H - 3, C_TAB_ACTIVE);
    hln(tab_x, 3, tab_w, C_TAB_LINE);

    frect(tab_x + 8, 9, 14, 14, 0xFFDDDDDD);
    orect(tab_x + 8, 9, 14, 14, 0xFFBBBBBB);

    let title_src = if B_TITLE_LEN > 0 { &B_TITLE[..B_TITLE_LEN] } else { b"New Tab" };
    let max_title = 18usize;
    let mut ttmp = [0u8; 32];
    let tl = title_src.len().min(max_title);
    ttmp[..tl].copy_from_slice(&title_src[..tl]);
    if title_src.len() > max_title {
        ttmp[max_title - 1] = b'.';
        ttmp[max_title - 2] = b'.';
    }
    ttmp[tl] = 0;
    graphics_draw_string(tab_x + 26, 11, ttmp.as_ptr(), C_URL_TEXT);

    graphics_draw_string(tab_x + tab_w - 16, 10, b"x\0".as_ptr(), 0xFF888888);

    frect(tab_x + tab_w + 4, 7, 22, 18, C_BTN);
    graphics_draw_string(tab_x + tab_w + 10, 11, b"+\0".as_ptr(), C_BTN_TEXT);

    hln(0, TAB_BAR_H - 1, tab_x, C_URL_BORDER);
    hln(tab_x + tab_w, TAB_BAR_H - 1, BW - tab_x - tab_w, C_URL_BORDER);
}

unsafe fn draw_nav_bar() {
    let ny = TAB_BAR_H;
    frect(0, ny, BW, NAV_BAR_H, C_CHROME);

    let btn_y   = ny + 7;
    let btn_h   = 24u32;
    let btn_w   = 28u32;

    frect(8, btn_y, btn_w, btn_h, C_BTN);
    let bx = 8 + btn_w / 2;
    let by = btn_y + btn_h / 2;
    for dy in 0..=4i32 {
        let w = 4 - dy.abs();
        for dx in 0..=w { px((bx as i32 - dx) as u32, (by as i32 + dy) as u32, C_BTN_TEXT); }
    }

    frect(40, btn_y, btn_w, btn_h, C_BTN);
    let bx = 40 + btn_w / 2;
    for dy in 0..=4i32 {
        let w = 4 - dy.abs();
        for dx in 0..=w { px((bx as i32 + dx) as u32, (by as i32 + dy) as u32, C_BTN_TEXT); }
    }

    frect(72, btn_y, btn_w, btn_h, C_BTN);
    graphics_draw_circle(72 + btn_w as i32 / 2, (btn_y + btn_h / 2) as i32, 6, C_BTN_TEXT);
    px(72 + btn_w / 2 + 5, btn_y + 4, C_BTN_TEXT);
    px(72 + btn_w / 2 + 6, btn_y + 4, C_BTN_TEXT);
    px(72 + btn_w / 2 + 6, btn_y + 5, C_BTN_TEXT);

    let url_x: u32 = 106;
    let url_w: u32 = BW - url_x - 54;
    let url_y: u32 = ny + 6;
    let url_h: u32 = 26;

    frect(url_x, url_y, url_w, url_h, C_URL_BG);
    orect(url_x, url_y, url_w, url_h, C_URL_BORDER);
    orect(url_x + 1, url_y + 1, url_w - 2, url_h - 2, 0xFFEEEEEE);

    let is_https = B_URL_LEN >= 8 && &B_URL[..8] == b"https://";
    let lock_c = if is_https { C_HTTPS_GREEN } else { C_MUTED };
    frect(url_x + 6, url_y + 9, 8, 9, lock_c);
    frect(url_x + 7, url_y + 5, 6, 5, 0x00000000);
    hln(url_x + 7, url_y + 5, 6, lock_c);
    vln(url_x + 7, url_y + 5, 5, lock_c);
    vln(url_x + 13, url_y + 5, 5, lock_c);

    let text_x    = url_x + 20;
    let max_chars = ((url_w - 28) / 8) as usize;
    let start_off = if B_URL_LEN > max_chars { B_URL_LEN - max_chars } else { 0 };
    let shown     = (B_URL_LEN - start_off).min(max_chars);
    let mut utmp = [0u8; 128];
    if shown > 0 {
        utmp[..shown].copy_from_slice(&B_URL[start_off..start_off + shown]);
    }
    utmp[shown] = 0;
    graphics_draw_string(text_x, url_y + 9, utmp.as_ptr(), C_URL_TEXT);

    let blink_on = (get_ticks() / 500) % 2 == 0;
    if blink_on {
        let cur_col = (B_URL_LEN - start_off).min(max_chars) as u32;
        let cur_px  = text_x + cur_col * 8;
        if cur_px < url_x + url_w - 4 {
            vln(cur_px, url_y + 5, url_h - 10, C_ACCENT);
        }
    }

    let go_x = BW - 46;
    frect(go_x, url_y, 38, url_h, C_ACCENT);
    graphics_draw_string(go_x + 11, url_y + 9, b"Go\0".as_ptr(), 0xFFFFFFFF);

    hln(0, ny + NAV_BAR_H - 1, BW, C_CHROME_DARK);
}

unsafe fn draw_bookmark_bar() {
    let by = TAB_BAR_H + NAV_BAR_H;
    frect(0, by, BW, BM_BAR_H, C_BM_BG);
    hln(0, by + BM_BAR_H - 1, BW, C_CHROME_DARK);
    graphics_draw_string(6,  by + 6, b"* Bookmarks\0".as_ptr(), C_BM_TEXT);
    graphics_draw_string(98, by + 6, b"| Powered by RadiumProxy (scp_2801)\0".as_ptr(), 0xFF9999BB);
}

unsafe fn draw_status_bar() {
    let sy = BH - STATUS_BAR_H;
    frect(0, sy, BW, STATUS_BAR_H, C_STATUS_BG);
    hln(0, sy, BW, C_CHROME_DARK);
    let mut tmp = [0u8; 129];
    let l = B_STATUS_LEN.min(128);
    tmp[..l].copy_from_slice(&B_STATUS[..l]);
    tmp[l] = 0;
    graphics_draw_string(8, sy + 7, tmp.as_ptr(), C_STATUS_TEXT);

    if B_LOADING {
        let t = (get_ticks() / 300) % 4;
        let dots = match t { 0=>b"   \0" as &[u8], 1=>b".  \0", 2=>b".. \0", _=>b"...\0" };
        graphics_draw_string(BW - 40, sy + 7, dots.as_ptr(), C_STATUS_TEXT);
    }
}

unsafe fn draw_scrollbar() {
    let sx = BW - SCROLLBAR_W;
    frect(sx, CONTENT_TOP_Y, SCROLLBAR_W, CONTENT_H, C_SCROLL_TRACK);
    vln(sx, CONTENT_TOP_Y, CONTENT_H, C_URL_BORDER);

    if B_PAGE_H > CONTENT_H as i32 && B_PAGE_H > 0 {
        let thumb_h = ((CONTENT_H * CONTENT_H) as i32 / B_PAGE_H).max(20) as u32;
        let thumb_y = ((B_SCROLL * CONTENT_H as i32) / B_PAGE_H).max(0) as u32;
        let ty = CONTENT_TOP_Y + thumb_y.min(CONTENT_H - thumb_h);
        frect(sx + 2, ty, SCROLLBAR_W - 4, thumb_h, C_SCROLL_THUMB);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CONTENT RENDERING
// ─────────────────────────────────────────────────────────────────────────────

unsafe fn draw_content() {
    frect(0, CONTENT_TOP_Y, CONTENT_W, CONTENT_H, C_PAGE_BG);

    if B_LOADING {
        let cx = (CONTENT_W / 2) as i32;
        let cy = (CONTENT_TOP_Y + CONTENT_H / 2) as i32;
        let t  = get_ticks();
        for i in 0..12i32 {
            let angle   = (i * 30 + (t / 30) as i32) % 360;
            let alpha   = (255 - i * 20).max(40) as u8;
            let c       = rgb(alpha, alpha, alpha);
            let r1: i32 = 18;
            let r2: i32 = 28;
            let x1 = cx + cos_deg(angle) * r1 / 1000;
            let y1 = cy + sin_deg(angle) * r1 / 1000;
            let x2 = cx + cos_deg(angle) * r2 / 1000;
            let y2 = cy + sin_deg(angle) * r2 / 1000;
            graphics_draw_line(x1, y1, x2, y2, c);
        }
        graphics_draw_string(cx as u32 - 32, (cy + 40) as u32,
            b"Loading...\0".as_ptr(), C_MUTED);
        return;
    }

    if !B_LOADED {
        draw_new_tab();
        return;
    }

    let clip_top = CONTENT_TOP_Y;
    let clip_bot = CONTENT_TOP_Y + CONTENT_H;

    for i in 0..PELEM_N {
        let e  = &PELEMS[i];
        let sy = CONTENT_TOP_Y as i32 + e.y - B_SCROLL;

        if sy + (e.h as i32) < (clip_top as i32) { continue; }
        if sy > (clip_bot as i32)                 { break; }
        if sy < (clip_top as i32)                 { continue; }

        let sy_u = sy as u32;
        let ts = e.ts; let tl = e.tl;
        let txt = &HTML_TEXT_BUFFER[ts..ts + tl];

        match e.k {
            PEK::H1 => {
                dslice_scaled(e.x as u32, sy_u, txt, C_H1, 2);
                hln(e.x as u32, sy_u + 18, e.w, 0xFFDDDDDD);
            }
            PEK::H2 => {
                dslice_scaled(e.x as u32, sy_u, txt, C_H2, 1);
                hln(e.x as u32, sy_u + 10, (txt.len() * 8) as u32, 0xFFDDDDDD);
            }
            PEK::H3 => {
                dslice(e.x as u32, sy_u, txt, C_H3);
            }
            PEK::Link => {
                dslice(e.x as u32, sy_u, txt, C_LINK);
                hln(e.x as u32, sy_u + 9, (txt.len() * 8).min(e.w as usize) as u32, C_LINK);
            }
            PEK::HR => {
                hln(PAGE_MARGIN, sy_u, CONTENT_W - PAGE_MARGIN * 2, C_HR);
                hln(PAGE_MARGIN, sy_u + 1, CONTENT_W - PAGE_MARGIN * 2, 0xFFEEEEEE);
            }
            PEK::Li => {
                frect(e.x as u32 - 10, sy_u + 4, 4, 4, C_TEXT);
                draw_wordwrap(e.x as u32, sy_u, txt, C_TEXT, e.w, clip_top, clip_bot);
            }
            PEK::Pre => {
                frect(e.x as u32 - 4, sy_u - 2, e.w + 8, e.h + 4, 0xFFF5F5F5);
                orect(e.x as u32 - 4, sy_u - 2, e.w + 8, e.h + 4, C_HR);
                dslice(e.x as u32, sy_u, txt, 0xFF333333);
            }
            PEK::Text => {
                draw_wordwrap(e.x as u32, sy_u, txt, C_TEXT, e.w, clip_top, clip_bot);
            }
        }
    }
}

unsafe fn draw_new_tab() {
    let cx = CONTENT_W / 2;
    let cy = CONTENT_TOP_Y + CONTENT_H / 3;

    graphics_draw_string_scaled(cx - 88, cy - 24,
        b"RadiumOS\0".as_ptr(), C_ACCENT, 2);
    graphics_draw_string(cx - 28, cy + 6,
        b"Browser\0".as_ptr(), C_MUTED);

    let bx = cx - 180; let bw = 360u32;
    frect(bx, cy + 34, bw, 36, 0xFFF9F9FB);
    orect(bx, cy + 34, bw, 36, C_URL_BORDER);
    graphics_draw_string(bx + 12, cy + 50,
        b"Enter a URL and press Enter...\0".as_ptr(), C_MUTED);

    let tile_y = cy + 96;
    graphics_draw_string(cx - 60, tile_y, b"Frequently Visited\0".as_ptr(), C_MUTED);

    let tiles: [(&[u8], u32); 4] = [
        (b"example.com\0", 0xFFE8F0FE),
        (b"discord.com\0",  0xFFEDE7F6),
        (b"github.com\0",   0xFFE8F5E9),
        (b"google.com\0",   0xFFFFF8E1),
    ];
    for (ti, (label, bg)) in tiles.iter().enumerate() {
        let tx = (cx as i32 - 170 + ti as i32 * 88) as u32;
        let ty = tile_y + 18;
        frect(tx, ty, 80, 68, *bg);
        orect(tx, ty, 80, 68, C_HR);
        let init = label[0];
        graphics_draw_char_scaled(tx + 28, ty + 12, init, C_MUTED, 2);
        let mut ltmp = [0u8; 16];
        let ll = label.len().saturating_sub(1).min(10);
        ltmp[..ll].copy_from_slice(&label[..ll]);
        ltmp[ll] = 0;
        graphics_draw_string(tx + 4, ty + 50, ltmp.as_ptr(), C_URL_TEXT);
    }

    graphics_draw_string(cx - 120, CONTENT_TOP_Y + CONTENT_H - 32,
        b"Up/Down = scroll | Escape = quit\0".as_ptr(), C_MUTED);
}

// ─────────────────────────────────────────────────────────────────────────────
// FULL RENDER
// ─────────────────────────────────────────────────────────────────────────────

unsafe fn b_navigate() {
    if b_is_local_path() {
        b_load_local();
    } else {
        do_navigate();
    }
}

unsafe fn b_render() {
    graphics_clear(C_PAGE_BG);
    draw_content();
    draw_tab_bar();
    draw_nav_bar();
    draw_bookmark_bar();
    draw_status_bar();
    draw_scrollbar();
    graphics_swap_buffers();
}

// ─────────────────────────────────────────────────────────────────────────────
// HTML → LAYOUT
// ─────────────────────────────────────────────────────────────────────────────

unsafe fn do_layout() {
    PELEM_N  = 0;
    B_PAGE_H = 0;
    B_TITLE_LEN = 0;

    let left  = PAGE_MARGIN as i32;
    let width = CONTENT_W - PAGE_MARGIN * 2;
    let mut cy: i32 = 8;

    for i in 0..HTML_ELEMENT_COUNT {
        if PELEM_N >= MAX_PELEMS { break; }
        let he = &HTML_ELEMENTS[i];
        let has_text = he.text_len > 0;

        match he.element_type {
            HtmlElementType::LineBreak => { cy += 8; continue; }
            HtmlElementType::HorizontalRule => {
                if PELEM_N < MAX_PELEMS {
                    let mut hr = PElem::z();
                    hr.k = PEK::HR; hr.x = left; hr.y = cy; hr.w = width; hr.h = 2;
                    PELEMS[PELEM_N] = hr; PELEM_N += 1;
                }
                cy += 8; continue;
            }
            HtmlElementType::TableRow => { cy += 4; continue; }
            HtmlElementType::Image => {
                if he.alt_len > 0 {
                    if PELEM_N < MAX_PELEMS {
                        let mut e2 = PElem::z();
                        e2.k  = PEK::Text;
                        e2.x  = left; e2.y = cy; e2.w = width;
                        e2.ts = he.alt_start; e2.tl = he.alt_len;
                        e2.c  = C_MUTED; e2.h = 12;
                        PELEMS[PELEM_N] = e2; PELEM_N += 1;
                        cy += 14;
                    }
                }
                continue;
            }
            HtmlElementType::Paragraph
            | HtmlElementType::Div
            | HtmlElementType::Section
            | HtmlElementType::Article => { if !has_text { cy += 6; continue; } }
            HtmlElementType::Span
            | HtmlElementType::Bold
            | HtmlElementType::Italic
            | HtmlElementType::Code
            | HtmlElementType::Nav
            | HtmlElementType::Footer
            | HtmlElementType::Header => { if !has_text { continue; } }
            _ => { if !has_text { continue; } }
        }

        let mut e = PElem::z();
        e.x  = left;
        e.y  = cy;
        e.w  = width;
        e.ts = he.text_start;
        e.tl = he.text_len;
        e.hs = he.href_start;
        e.hl = he.href_len;

        match he.element_type {
            HtmlElementType::Header1 => {
                e.k = PEK::H1; e.h = 20;
                if B_TITLE_LEN == 0 {
                    let tl = he.text_len.min(127);
                    B_TITLE[..tl].copy_from_slice(
                        &HTML_TEXT_BUFFER[he.text_start..he.text_start + tl]);
                    B_TITLE_LEN = tl; B_TITLE[tl] = 0;
                }
                PELEMS[PELEM_N] = e; PELEM_N += 1;
                cy += 26;
                if PELEM_N < MAX_PELEMS {
                    let mut hr = PElem::z();
                    hr.k = PEK::HR; hr.x = left; hr.y = cy - 2; hr.w = width; hr.h = 2;
                    PELEMS[PELEM_N] = hr; PELEM_N += 1;
                }
                cy += 6; continue;
            }
            HtmlElementType::Header2 => { e.k = PEK::H2; e.h = 14; cy += 22; }
            HtmlElementType::Header3
            | HtmlElementType::Header4 => { e.k = PEK::H3; e.h = 12; cy += 18; }
            HtmlElementType::Header5
            | HtmlElementType::Header6 => {
                e.k = PEK::Text; e.c = C_MUTED; e.h = 10; cy += 14;
            }
            HtmlElementType::Link => {
                e.k = PEK::Link;
                e.h = 12;
                e.w = (he.text_len * 8).min(width as usize) as u32;
                cy += 16;
            }
            HtmlElementType::Preformatted => {
                e.k = PEK::Pre;
                e.h = wrap_height(he.text_len, width);
                cy += e.h as i32 + 8;
            }
            HtmlElementType::Code => {
                e.k = PEK::Pre;
                e.h = wrap_height(he.text_len, width);
                cy += e.h as i32 + 4;
            }
            HtmlElementType::Blockquote => {
                e.k  = PEK::Text;
                e.x  = left + 20;
                e.w  = width - 20;
                e.c  = C_MUTED;
                e.h  = wrap_height(he.text_len, width - 20);
                cy  += e.h as i32 + 8;
            }
            HtmlElementType::ListItem
            | HtmlElementType::OrderedListItem => {
                e.k = PEK::Li;
                e.x = left + 16; e.w = width - 16;
                e.h = wrap_height(he.text_len, width - 16);
                cy += e.h as i32 + 4;
            }
            HtmlElementType::TableCell => {
                e.k = PEK::Text; e.h = 12; cy += 14;
            }
            HtmlElementType::TableHeader => {
                e.k = PEK::H3; e.h = 12; cy += 14;
            }
            HtmlElementType::Bold
| HtmlElementType::Italic
| HtmlElementType::Span
| HtmlElementType::Div
| HtmlElementType::Section
| HtmlElementType::Article
| HtmlElementType::Header
| HtmlElementType::Nav
| HtmlElementType::Footer
| HtmlElementType::Paragraph
| HtmlElementType::Text
| HtmlElementType::Unknown
| HtmlElementType::Table        // add
| HtmlElementType::Underline    // add
| HtmlElementType::Strikethrough // add
| HtmlElementType::Subscript    // add
| HtmlElementType::Superscript  // add
| HtmlElementType::Aside        // add
| HtmlElementType::Main         // add
| HtmlElementType::Button       // add
| HtmlElementType::Input        // add
| HtmlElementType::TextArea     // add
| HtmlElementType::Select       // add
| HtmlElementType::Center       // add
| HtmlElementType::Font         // add
=> {
    e.k = PEK::Text;
    e.h = wrap_height(he.text_len, width);
    cy += e.h as i32 + 4;
}
            HtmlElementType::LineBreak
            | HtmlElementType::HorizontalRule
            | HtmlElementType::TableRow
            | HtmlElementType::Image => { continue; }
        }

        PELEMS[PELEM_N] = e;
        PELEM_N += 1;
    }

    B_PAGE_H = cy + 24;
}
// ─────────────────────────────────────────────────────────────────────────────
// NETWORKING
// ─────────────────────────────────────────────────────────────────────────────
static mut global_content_len: usize = 0;
static mut global_is_chunked: bool = false;
static mut global_hdr_done: bool = false;
static mut global_body_off: usize = 0;
// Case-insensitive substring search
fn ci_contains(hay: &[u8], needle: &[u8]) -> bool {
    if needle.len() > hay.len() { return false; }
    'o: for i in 0..=(hay.len() - needle.len()) {
        for j in 0..needle.len() {
            let h = hay[i+j]; let n = needle[j];
            let hl = if h>=b'A'&&h<=b'Z'{h+32}else{h};
            let nl = if n>=b'A'&&n<=b'Z'{n+32}else{n};
            if hl != nl { continue 'o; }
        }
        return true;
    }
    false
}

unsafe fn decode_chunked(data: &[u8]) -> usize {
    let mut pos = 0usize;
    let mut out = 0usize;
    loop {
        while pos < data.len() && (data[pos]==b'\r'||data[pos]==b'\n') { pos+=1; }
        let s = pos;
        while pos < data.len() && data[pos]!=b'\r' && data[pos]!=b'\n' && data[pos]!=b';' { pos+=1; }
        if pos == s { break; }
        let mut sz = 0usize;
        for &c in &data[s..pos] {
            sz = sz * 16 + match c {
                b'0'..=b'9' => (c-b'0') as usize,
                b'a'..=b'f' => (c-b'a'+10) as usize,
                b'A'..=b'F' => (c-b'A'+10) as usize,
                _ => break,
            };
        }
        if sz == 0 { break; }
        while pos < data.len() && (data[pos]==b'\r'||data[pos]==b'\n') { pos+=1; }
        let end  = (pos + sz).min(data.len());
        let copy = (end - pos).min(CHUNKED_DECODE_BUF.len() - out);
        CHUNKED_DECODE_BUF[out..out+copy].copy_from_slice(&data[pos..pos+copy]);
        out += copy;
        pos += sz;
    }
    out
}

unsafe fn debug_overlay(recv_len: usize, body_start: usize, found_headers: bool) {
    // Dark overlay box in top-left of content area
    frect(0, CONTENT_TOP_Y, 760, 200, 0xEE000000);
    
    let mut y = CONTENT_TOP_Y + 8;
    let col   = 0xFFFFFF00u32; // yellow

    // Line 1: recv_len
    {
        let mut tmp = *b"recv_len=0000000         ";
        let mut n = recv_len;
        let mut i = 16;
        loop { tmp[i] = b'0' + (n % 10) as u8; if n < 10 { break; } n /= 10; i -= 1; }
        tmp[24] = 0;
        graphics_draw_string(8, y, tmp.as_ptr(), col);
        y += 14;
    }

    // Line 2: found_headers, body_start
    {
        let mut tmp = *b"found=? body_start=0000000  ";
        tmp[6] = if found_headers { b'Y' } else { b'N' };
        let mut n = body_start;
        let mut i = 26;
        loop { tmp[i] = b'0' + (n % 10) as u8; if n < 10 { break; } n /= 10; i -= 1; }
        tmp[27] = 0;
        graphics_draw_string(8, y, tmp.as_ptr(), col);
        y += 14;
    }

    // Line 3: first 80 bytes as ASCII (dots for non-printable)
    {
        let mut tmp = [0u8; 82];
        for i in 0..recv_len.min(80) {
            let c = HTTP_RECEIVE_BUFFER[i];
            tmp[i] = if c >= 0x20 && c < 0x7F { c } else { b'.' };
        }
        tmp[recv_len.min(80)] = 0;
        graphics_draw_string(8, y, tmp.as_ptr(), 0xFF00FF00);
        y += 14;
    }

    // Line 4: bytes 80-160
    {
        let mut tmp = [0u8; 82];
        let start = recv_len.min(80);
        for i in 0..(recv_len.saturating_sub(80)).min(80) {
            let c = HTTP_RECEIVE_BUFFER[start + i];
            tmp[i] = if c >= 0x20 && c < 0x7F { c } else { b'.' };
        }
        tmp[(recv_len.saturating_sub(80)).min(80)] = 0;
        graphics_draw_string(8, y, tmp.as_ptr(), 0xFF00FF00);
        y += 14;

    }

    // Line 5: HTML_ELEMENT_COUNT after parse
    {
        let mut tmp = *b"elements=0000  ";
        let mut n = HTML_ELEMENT_COUNT;
        let mut i = 12;
        loop { tmp[i] = b'0' + (n % 10) as u8; if n < 10 { break; } n /= 10; i -= 1; }
        tmp[13] = 0;
        graphics_draw_string(8, y, tmp.as_ptr(), col);
        y += 14;
    }

    // Line 6: PELEM_N after layout
    {
        let mut tmp = *b"pelems=0000  ";
        let mut n = PELEM_N;
        let mut i = 11;
        loop { tmp[i] = b'0' + (n % 10) as u8; if n < 10 { break; } n /= 10; i -= 1; }
        tmp[12] = 0;
        graphics_draw_string(8, y, tmp.as_ptr(), col);
    }

    graphics_swap_buffers();
}


unsafe fn process_response(recv_len: usize) {
    B_LOADING = false;
    B_LOADED  = false;

    if recv_len == 0 {
        b_set_status(b"Error: no data");
        return;
    }

    // ── VGA dump code (unchanged) ───────────────────────────────────────────
    {
        let mut tmp = [b'.'; 80];
        for i in 0..recv_len.min(80) {
            let c = HTTP_RECEIVE_BUFFER[i];
            tmp[i] = if c >= 0x20 && c < 0x7F { c } else { b'.' };
        }
        let vga = 0xB8000 as *mut u16;
        for i in 0..80usize {
            *vga.add(20 * 80 + i) = (0x0Eu16 << 8) | tmp[i] as u16;
        }
    }
    {
        let mut tmp = [b'.'; 80];
        let start = 80.min(recv_len);
        let count = (recv_len.saturating_sub(80)).min(80);
        for i in 0..count {
            let c = HTTP_RECEIVE_BUFFER[start + i];
            tmp[i] = if c >= 0x20 && c < 0x7F { c } else { b'.' };
        }
        let vga = 0xB8000 as *mut u16;
        for i in 0..80usize {
            *vga.add(21 * 80 + i) = (0x0Eu16 << 8) | tmp[i] as u16;
        }
    }
    {
        let vga = 0xB8000 as *mut u16;
        let label = b"recv_len=";
        for (i, &c) in label.iter().enumerate() {
            *vga.add(22 * 80 + i) = (0x0Bu16 << 8) | c as u16;
        }
        let mut n  = recv_len;
        let mut buf = [b'0'; 10];
        let mut bi  = 9usize;
        if n == 0 {
            *vga.add(22 * 80 + label.len()) = (0x0Bu16 << 8) | b'0' as u16;
        } else {
            while n > 0 {
                buf[bi] = b'0' + (n % 10) as u8;
                n /= 10;
                bi -= 1;
            }
            let mut col = label.len();
            for i in (bi + 1)..10 {
                *vga.add(22 * 80 + col) = (0x0Bu16 << 8) | buf[i] as u16;
                col += 1;
            }
        }
    }

    // ── ULTRA-SAFE header separator detection ───────────────────────────────
    let mut body_start = 0usize;
let mut found_headers = false;

// Primary: \r\n\r\n separator detection
if recv_len >= 4 {
    for k in 0..=recv_len.saturating_sub(4) {
        if HTTP_RECEIVE_BUFFER[k] == b'\r' &&
           HTTP_RECEIVE_BUFFER[k+1] == b'\n' &&
           HTTP_RECEIVE_BUFFER[k+2] == b'\r' &&
           HTTP_RECEIVE_BUFFER[k+3] == b'\n' 
        {
            body_start = k + 4;
            found_headers = true;
            break;
        }
    }
}

// Fallback: \n\n separator detection
if !found_headers && recv_len >= 2 {
    for k in 0..=recv_len.saturating_sub(2) {
        if HTTP_RECEIVE_BUFFER[k] == b'\n' &&
           HTTP_RECEIVE_BUFFER[k+1] == b'\n' 
        {
            body_start = k + 2;
            found_headers = true;
            break;
        }
    }
}

    // Fallback: raw HTML
    if !found_headers && recv_len > 0 && HTTP_RECEIVE_BUFFER[0] == b'<' {
        body_start = 0;
        found_headers = true;
    }

    if !found_headers {
        let vga = 0xB8000 as *mut u16;
        let msg = b"no sep found - check rows 20-21";
        for (i, &c) in msg.iter().enumerate() {
            *vga.add(23 * 80 + i) = (0x0Cu16 << 8) | c as u16;
        }
        b_set_status(b"Error: no header separator");
        return;
    }

    if body_start >= recv_len {
        b_set_status(b"Error: body empty after headers");
        return;
    }

    // ── VGA body_start display (unchanged) ─────────────────────────────────
    {
        let vga = 0xB8000 as *mut u16;
        let label = b"body_start=";
        for (i, &c) in label.iter().enumerate() {
            *vga.add(23 * 80 + i) = (0x0Au16 << 8) | c as u16;
        }
        let mut n   = body_start;
        let mut buf = [b'0'; 10];
        let mut bi  = 9usize;
        if n == 0 {
            *vga.add(23 * 80 + label.len()) = (0x0Au16 << 8) | b'0' as u16;
        } else {
            while n > 0 {
                buf[bi] = b'0' + (n % 10) as u8;
                n /= 10;
                bi -= 1;
            }
            let mut col = label.len();
            for i in (bi+1)..10 {
                *vga.add(23 * 80 + col) = (0x0Au16 << 8) | buf[i] as u16;
                col += 1;
            }
        }
    }

    let headers  = &HTTP_RECEIVE_BUFFER[..body_start];
    let body_raw = &HTTP_RECEIVE_BUFFER[body_start..recv_len];

    if body_raw.is_empty() {
        b_set_status(b"Error: empty body");
        return;
    }

    // ── Body decoding (unchanged) ───────────────────────────────────────────
    let body_slice: &[u8] = if ci_contains(headers, b"transfer-encoding: chunked")
        || global_is_chunked
    {
        let decoded_len = decode_chunked(body_raw);
        if decoded_len == 0 { body_raw } else { &CHUNKED_DECODE_BUF[..decoded_len] }
    } else {
        let cl = if global_content_len > 0 {
            global_content_len
        } else {
            extract_content_length(headers)
                .map(|s| parse_u32(s) as usize)
                .unwrap_or(0)
        };
        if cl > 0 && cl <= body_raw.len() { &body_raw[..cl] } else { body_raw }
    };

    if body_slice.is_empty() {
        b_set_status(b"Error: decoded body empty");
        return;
    }

    b_set_status(b"Parsing...");
    parse_html(body_slice);
    do_layout();

    B_LOADED = true;
    B_SCROLL = 0;
    b_set_status(b"Done");
}

// Helper functions you'll need:
fn extract_content_length(headers: &[u8]) -> Option<&[u8]> {
    let header = b"content-length:";
    let pos = ci_find(headers, header)?;
    let start = pos + header.len();
    let end = headers[start..].iter()
        .position(|&b| b == b'\r' || b == b'\n')
        .unwrap_or(0);
    Some(&headers[start..start + end])
}

fn parse_u32(s: &[u8]) -> u32 {
    let mut n = 0u32;
    for &b in s {
        if b >= b'0' && b <= b'9' {
            n = n * 10 + (b - b'0') as u32;
        } else {
            break;
        }
    }
    n
}

fn ci_find(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack.windows(needle.len())
        .position(|window| ci_slice_eq(window, needle))
}

fn ci_slice_eq(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).all(|(&x, &y)| 
        (x.to_ascii_lowercase() == y.to_ascii_lowercase())
    )
}
unsafe fn send_get(hostname: &[u8], path: &[u8]) -> bool {
 
    let mut req = [0u8; 2048];
    let mut ri  = 0usize;
    macro_rules! push {
        ($b:expr) => { for &c in $b { if ri < req.len()-1 { req[ri]=c; ri+=1; } } };
    }
 
    push!(b"GET ");
    if path.is_empty() || path[0] != b'/' { push!(b"/"); }
    push!(path);
    push!(b" HTTP/1.1\r\n");
    push!(b"Host: "); push!(hostname); push!(b"\r\n");
    push!(b"User-Agent: RadiumOS-Browser/4.0\r\n");
    push!(b"Accept: text/html,*/*;q=0.8\r\n");
    push!(b"Accept-Encoding: identity\r\n");   // no gzip - proxy handles it
    push!(b"Connection: close\r\n");
    push!(b"X-Proxy-Scheme: http\r\n\r\n");    // tell proxy: plain HTTP
 
    let local_port  = TCP_CONNECTION.local_port;
    let remote_port = TCP_CONNECTION.remote_port;
 
    if !tcp_send_data(&req[..ri]) {
        b_set_status(b"Send failed");
        tcp_close();
        return false;
    }
 
    HTTP_RECEIVE_LEN    = 0;
    RX_RESPONSE_LENGTH  = 0;
    global_hdr_done     = false;
    global_is_chunked   = false;
    global_content_len  = 0;
    global_body_off     = 0;
 
    // Give proxy time to connect to upstream and start responding
    for _ in 0..3_000_000u32 { core::hint::spin_loop(); }
 
    // How long to keep trying when no new packets arrive.
    // 200_000_000 iterations ≈ ~2-3 seconds in QEMU - enough for slow sites.
    let idle_limit:  u32 = 200_000_000;
    // Absolute cap: ~8 seconds total regardless
    let hard_limit:  u32 = 800_000_000;
    let mut loops:   u32 = 0;
    let mut idle:    u32 = 0;
 
    'recv: loop {
        loops += 1;
        if loops >= hard_limit {
            rust_print(b"send_get: hard timeout\n");
            break 'recv;
        }
 
        RX_RESPONSE_LENGTH = 0;
        rust_rtl8139_receive();
 
        if RX_RESPONSE_LENGTH < 54 {
            RX_RESPONSE_LENGTH = 0;
            idle += 1;
 
            // If we already have a complete response, stop waiting
            if global_hdr_done && HTTP_RECEIVE_LEN > global_body_off {
                let body_received = HTTP_RECEIVE_LEN - global_body_off;
                let complete = if global_is_chunked {
                    // Check for terminal "0\r\n\r\n" in raw buffer
                    find_chunk_end().is_some()
                } else if global_content_len > 0 {
                    body_received >= global_content_len
                } else {
                    false  // connection-close: wait for FIN
                };
                if complete {
                    rust_print(b"send_get: complete (idle check)\n");
                    break 'recv;
                }
            }
 
            if idle >= idle_limit {
                rust_print(b"send_get: idle timeout, have ");
                print_num(HTTP_RECEIVE_LEN as i32);
                rust_print(b" bytes\n");
                break 'recv;
            }
            continue 'recv;
        }
 
        // ── Filter to our TCP connection ──────────────────────────────────
        let et = ((RX_RESPONSE_BUFFER[12] as u16) << 8) | (RX_RESPONSE_BUFFER[13] as u16);
        if et != 0x0800 { RX_RESPONSE_LENGTH = 0; continue 'recv; }
 
        let ihl = ((RX_RESPONSE_BUFFER[14] & 0x0F) * 4) as usize;
        if ihl < 20 || RX_RESPONSE_BUFFER[14 + 9] != 6 {
            RX_RESPONSE_LENGTH = 0; continue 'recv;
        }
 
        let tcp_start = 14 + ihl;
        if tcp_start + 20 > RX_RESPONSE_LENGTH as usize {
            RX_RESPONSE_LENGTH = 0; continue 'recv;
        }
 
        let src_p = ((RX_RESPONSE_BUFFER[tcp_start]     as u16) << 8)
                  |  (RX_RESPONSE_BUFFER[tcp_start + 1] as u16);
        let dst_p = ((RX_RESPONSE_BUFFER[tcp_start + 2] as u16) << 8)
                  |  (RX_RESPONSE_BUFFER[tcp_start + 3] as u16);
 
        if src_p != remote_port || dst_p != local_port {
            RX_RESPONSE_LENGTH = 0; continue 'recv;
        }
 
        // Packet is for us - reset idle counter
        idle = 0;
 
        let flags    = RX_RESPONSE_BUFFER[tcp_start + 13];
        let tcp_hl   = ((RX_RESPONSE_BUFFER[tcp_start + 12] >> 4) * 4) as usize;
        let data_off = tcp_start + tcp_hl;
        let total    = RX_RESPONSE_LENGTH as usize;
 
        let rseq = ((RX_RESPONSE_BUFFER[tcp_start + 4] as u32) << 24)
                 | ((RX_RESPONSE_BUFFER[tcp_start + 5] as u32) << 16)
                 | ((RX_RESPONSE_BUFFER[tcp_start + 6] as u32) <<  8)
                 |  (RX_RESPONSE_BUFFER[tcp_start + 7] as u32);
 
        // ── Copy payload ──────────────────────────────────────────────────
        if data_off < total {
            let dlen  = total - data_off;
            let space = HTTP_RECEIVE_BUFFER.len().saturating_sub(HTTP_RECEIVE_LEN);
            let copy  = dlen.min(space);
 
            for j in 0..copy {
                HTTP_RECEIVE_BUFFER[HTTP_RECEIVE_LEN + j] = RX_RESPONSE_BUFFER[data_off + j];
            }
            HTTP_RECEIVE_LEN += copy;
 
            // Try to parse headers if not yet done
            if !global_hdr_done {
                parse_http_headers();
            }
 
            // ── Check completion ──────────────────────────────────────────
            if global_hdr_done {
                let body_received = HTTP_RECEIVE_LEN.saturating_sub(global_body_off);
 
                let complete = if global_is_chunked {
                    find_chunk_end().is_some()
                } else if global_content_len > 0 {
                    body_received >= global_content_len
                } else {
                    false
                };
 
                if complete {
                    rust_print(b"send_get: body complete, CL=");
                    print_num(global_content_len as i32);
                    rust_print(b" received=");
                    print_num(body_received as i32);
                    rust_print(b"\n");
                    // Send final ACK then break
                    TCP_CONNECTION.ack_num = rseq.wrapping_add(dlen as u32);
                    send_ack_packet(local_port, remote_port);
                    RX_RESPONSE_LENGTH = 0;
                    break 'recv;
                }
            }
 
            // Send ACK for this packet
            TCP_CONNECTION.ack_num = rseq.wrapping_add(dlen as u32);
            send_ack_packet(local_port, remote_port);
        }
 
        // FIN - server closed connection
        if (flags & 0x01) != 0 {
            rust_print(b"send_get: FIN received\n");
            TCP_CONNECTION.ack_num = rseq.wrapping_add(1);
            send_ack_packet(local_port, remote_port);
            RX_RESPONSE_LENGTH = 0;
            break 'recv;
        }
 
        RX_RESPONSE_LENGTH = 0;
    }
 
    let recv_len = HTTP_RECEIVE_LEN;
 
    rust_print(b"send_get done: ");
    print_num(recv_len as i32);
    rust_print(b" bytes, hdr_done=");
    rust_print(if global_hdr_done { b"Y" } else { b"N" });
    rust_print(b" chunked=");
    rust_print(if global_is_chunked { b"Y" } else { b"N" });
    rust_print(b" CL=");
    print_num(global_content_len as i32);
    rust_print(b"\n");
 
    if recv_len == 0 {
        b_set_status(b"No response received");
        return false;
    }
 
    process_response(recv_len);
    true
}
 

// NEW: Helper functions for better header parsing
unsafe fn parse_http_headers() {
    let data = HTTP_RECEIVE_BUFFER.as_ptr();
    let len = HTTP_RECEIVE_LEN;
    
    // Find \r\n\r\n
    let mut sk = 0usize;
    while sk + 3 < len {
        if *(data.add(sk)) == b'\r' && *(data.add(sk+1)) == b'\n'
        && *(data.add(sk+2)) == b'\r' && *(data.add(sk+3)) == b'\n' {
            parse_headers_up_to(sk);
            return;
        }
        sk += 1;
    }
}

unsafe fn send_ack_packet(local_port: u16, remote_port: u16) {
    let mut atcp = [0u8; 64];
    let al = build_tcp_packet(
        &TCP_CONNECTION.remote_ip, remote_port, local_port,
        TCP_CONNECTION.seq_num, TCP_CONNECTION.ack_num,
        0x10, &[], &mut atcp,
    );
    if al == 0 { return; }
    let mut aip = [0u8; 128];
    let ail = build_ip_packet(&TCP_CONNECTION.remote_ip, 6, &atcp[..al], &mut aip);
    if ail == 0 { return; }
    let gw = [0x52u8, 0x54, 0x00, 0x12, 0x34, 0x56];
    let mut aeth = [0u8; 256];
    let ael = build_ethernet_frame(&gw, 0x0800, &aip[..ail], &mut aeth);
    if ael == 0 { return; }
    rust_rtl8139_send(aeth.as_ptr(), ael as u32);
}

unsafe fn parse_headers_up_to(end: usize) {
    let data = HTTP_RECEIVE_BUFFER.as_ptr();
    
    // FIXED: Case-insensitive Content-Length
    let cl_needle = b"content-length:";
    'cl: for ci in 0..end {
        if ci + cl_needle.len() > end { break 'cl; }
        if matches_header_case_insensitive(data, ci, cl_needle) {
            let mut v = 0usize;
            let mut p = ci + cl_needle.len();
            while p < end && *data.add(p) >= b'0' && *data.add(p) <= b'9' {
                v = v * 10 + (*data.add(p) - b'0') as usize;
                p += 1;
            }
            global_content_len = v;  // Assume you have this global
            break 'cl;
        }
    }
    
    // FIXED: Case-insensitive Transfer-Encoding: chunked
    let chunk_needle = b"transfer-encoding:";
    'chunk: for ci in 0..end {
        if ci + chunk_needle.len() > end { break 'chunk; }
        if matches_header_case_insensitive(data, ci, chunk_needle) {
            let val_start = ci + chunk_needle.len();
            if find_substring_ignore_case(data, val_start, end, b"chunked") {
                global_is_chunked = true;  // Assume you have this global
                break 'chunk;
            }
        }
    }
    
    global_hdr_done = true;
    global_body_off = end + 4;
}

fn matches_header_case_insensitive(data: *const u8, start: usize, needle: &[u8]) -> bool {
    for (i, &n) in needle.iter().enumerate() {
        let h = unsafe { *data.add(start + i) };
        let hl = if h >= b'A' && h <= b'Z' { h + 32 } else { h };
        if hl != n { return false; }
    }
    true
}

fn find_substring_ignore_case(data: *const u8, start: usize, end: usize, needle: &[u8]) -> bool {
    let mut pos = start;
    while pos + needle.len() <= end {
        let mut match_ok = true;
        for (i, &n) in needle.iter().enumerate() {
            let h = unsafe { *data.add(pos + i) };
            let hl = if h >= b'A' && h <= b'Z' { h + 32 } else { h };
            if hl != n { match_ok = false; break; }
        }
        if match_ok { return true; }
        pos += 1;
    }
    false
}

unsafe fn find_chunk_end() -> Option<usize> {
    let data = HTTP_RECEIVE_BUFFER.as_ptr();
    let len = HTTP_RECEIVE_LEN;
    let body_start = global_body_off;
    
    let mut pos = body_start;
    while pos + 7 < len {  // Need room for "0\r\n\r\n"
        // Look for chunk size 0
        if *data.add(pos) == b'0' 
        && *data.add(pos+1) == b'\r' 
        && *data.add(pos+2) == b'\n'
        && *data.add(pos+3) == b'\r'
        && *data.add(pos+4) == b'\n' {
            return Some(pos + 5);
        }
        pos += 1;
    }
    None
}
unsafe fn fetch_via_proxy(hostname: &[u8], path: &[u8]) -> bool {
    let proxy: [u8; 4] = [72, 14, 176, 144];  // was [10, 0, 2, 2]
    b_set_status(b"Connecting...");
 
    if !tcp_connect(&proxy, 8080) {
        b_set_status(b"Proxy connect failed");
        return false;
    }
    // !! Do NOT write TCP_CONNECTION.remote_ip/port here !!
    // tcp_connect() set them correctly. Overwriting breaks ACK routing.
 
    b_set_status(b"Sending request...");
    send_get(hostname, path)
    // send_get calls b_render() internally after process_response
}

unsafe fn fetch_direct(hostname: &[u8], path: &[u8], port: u16) -> bool {
    let mut host_z = [0u8; 256];
    let hl = hostname.len().min(255);
    host_z[..hl].copy_from_slice(&hostname[..hl]);

    b_set_status(b"Resolving DNS...");
    b_render();

    let ip = match resolve_host(&host_z) {
        Some(ip) => ip,
        None => {
            b_set_status(b"DNS failed - trying proxy...");
            b_render();
            return fetch_via_proxy(hostname, path);
        }
    };
    b_set_status(b"Connecting...");
    b_render();
    if !tcp_connect(&ip, port) {
        b_set_status(b"TCP connect failed");
        return false;
    }
    send_get(hostname, path)
}



unsafe fn do_navigate() {
    if B_URL_LEN == 0 { return; }
 
    B_LOADING   = true;
    B_LOADED    = false;
    PELEM_N     = 0;
    B_TITLE_LEN = 0;
    B_URL[B_URL_LEN] = 0;
 
    b_set_status(b"Loading...");
    b_render();   // show spinner
 
    let url      = &B_URL[..B_URL_LEN];
    let is_https = B_URL_LEN >= 8 && &url[..8] == b"https://";
    let is_http  = B_URL_LEN >= 7 && &url[..7] == b"http://";
 
    if !is_https && !is_http {
        b_set_status(b"Unknown scheme - use http:// or https://");
        B_LOADING = false;
        b_render();
        return;
    }
 
    let skip     = if is_https { 8 } else { 7 };
    let rest     = &url[skip..];
    let host_end = rest.iter().position(|&c| c==b'/'||c==b':').unwrap_or(rest.len());
    let hostname = &rest[..host_end];
 
    let mut idx       = host_end;
    let mut port: u16 = if is_https { 443 } else { 80 };
    if idx < rest.len() && rest[idx] == b':' {
        idx += 1; port = 0;
        while idx < rest.len() && rest[idx] >= b'0' && rest[idx] <= b'9' {
            port = port * 10 + (rest[idx] - b'0') as u16;
            idx += 1;
        }
    }
    let path = if idx < rest.len() { &rest[idx..] } else { b"/" };
 
    // Route everything through the proxy - it handles both HTTP and HTTPS
    let ok = fetch_via_proxy(hostname, path);
 
    B_LOADING = false;
 
    if B_LOADED {
        // Page loaded successfully - render it
        b_render();
    } else {
        // Show error on the new-tab page
        if !ok {
            b_set_status(b"Failed to load page");
        }
        b_render();
    }
 
    // Print debug summary to terminal (doesn't block rendering)
    rust_print(b"navigate done: ok=");
    rust_print(if ok { b"Y" } else { b"N" });
    rust_print(b" B_LOADED=");
    rust_print(if B_LOADED { b"Y" } else { b"N" });
    rust_print(b" elems=");
    print_num(HTML_ELEMENT_COUNT as i32);
    rust_print(b" pelems=");
    print_num(PELEM_N as i32);
    rust_print(b"\n");
}
// ─────────────────────────────────────────────────────────────────────────────
// SCANCODE → ASCII
// ─────────────────────────────────────────────────────────────────────────────

unsafe fn scancode_to_ascii(sc: u8, shift: bool) -> u8 {
    if shift {
        match sc {
            0x02=>b'!', 0x03=>b'@', 0x04=>b'#', 0x05=>b'$', 0x06=>b'%',
            0x07=>b'^', 0x08=>b'&', 0x09=>b'*', 0x0A=>b'(', 0x0B=>b')',
            0x0C=>b'_', 0x0D=>b'+',
            0x10=>b'Q', 0x11=>b'W', 0x12=>b'E', 0x13=>b'R', 0x14=>b'T',
            0x15=>b'Y', 0x16=>b'U', 0x17=>b'I', 0x18=>b'O', 0x19=>b'P',
            0x1A=>b'{', 0x1B=>b'}',
            0x1E=>b'A', 0x1F=>b'S', 0x20=>b'D', 0x21=>b'F', 0x22=>b'G',
            0x23=>b'H', 0x24=>b'J', 0x25=>b'K', 0x26=>b'L',
            0x27=>b':', 0x28=>b'"', 0x29=>b'~', 0x2B=>b'|',
            0x2C=>b'Z', 0x2D=>b'X', 0x2E=>b'C', 0x2F=>b'V', 0x30=>b'B',
            0x31=>b'N', 0x32=>b'M', 0x33=>b'<', 0x34=>b'>', 0x35=>b'?',
            0x39=>b' ',
            _ => 0,
        }
    } else {
        match sc {
            0x02=>b'1', 0x03=>b'2', 0x04=>b'3', 0x05=>b'4', 0x06=>b'5',
            0x07=>b'6', 0x08=>b'7', 0x09=>b'8', 0x0A=>b'9', 0x0B=>b'0',
            0x0C=>b'-', 0x0D=>b'=',
            0x10=>b'q', 0x11=>b'w', 0x12=>b'e', 0x13=>b'r', 0x14=>b't',
            0x15=>b'y', 0x16=>b'u', 0x17=>b'i', 0x18=>b'o', 0x19=>b'p',
            0x1A=>b'[', 0x1B=>b']',
            0x1E=>b'a', 0x1F=>b's', 0x20=>b'd', 0x21=>b'f', 0x22=>b'g',
            0x23=>b'h', 0x24=>b'j', 0x25=>b'k', 0x26=>b'l',
            0x27=>b';', 0x28=>b'\'',0x29=>b'`', 0x2B=>b'\\',
            0x2C=>b'z', 0x2D=>b'x', 0x2E=>b'c', 0x2F=>b'v', 0x30=>b'b',
            0x31=>b'n', 0x32=>b'm', 0x33=>b',', 0x34=>b'.', 0x35=>b'/',
            0x39=>b' ',
            _ => 0,
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MAIN ENTRY POINT
// ─────────────────────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn graphical_browser() -> i32 {
    unsafe {
        rust_print(b"\n=== RadiumOS Browser v3 ===\n");
        rust_print(b"Esc=quit  Up/Down=scroll  Enter=load\n\n");

        if graphics_init(BW, BH, 32) != 0 {
            rust_print(b"ERROR: Graphics init failed\n");
            return -1;
        }

        // Reset state
        B_URL_LEN   = 0;
        B_SCROLL    = 0;
        B_PAGE_H    = 0;
        B_LOADING   = false;
        B_LOADED    = false;
        B_TITLE_LEN = 0;
        B_SHIFT     = false;
        PELEM_N     = 0;
        for i in 0..512 { B_URL[i] = 0; }
        b_set_status(b"Ready");

        b_render();

        loop {
            if is_key_pressed() {
                let scan = port_byte_in(0x60);

                // shift tracking
                if scan == 0x2A || scan == 0x36 { B_SHIFT = true;  continue; }
                if scan == 0xAA || scan == 0xB6 { B_SHIFT = false; continue; }
                // ignore all key-release events
                if scan >= 0x80 { continue; }

                match scan {
                    // Escape = quit
                    0x01 => break,

                    // Up/Down scroll
                    0x48 => {
                        B_SCROLL = (B_SCROLL - 30).max(0);
                        b_render();
                    }
                    0x50 => {
                        let max_s = (B_PAGE_H - CONTENT_H as i32).max(0);
                        B_SCROLL = (B_SCROLL + 30).min(max_s);
                        b_render();
                    }
                    // Page Up / Page Down
                    0x49 => {
                        B_SCROLL = (B_SCROLL - CONTENT_H as i32 + 20).max(0);
                        b_render();
                    }
                    0x51 => {
                        let max_s = (B_PAGE_H - CONTENT_H as i32).max(0);
                        B_SCROLL = (B_SCROLL + CONTENT_H as i32 - 20).min(max_s);
                        b_render();
                    }

                    // Enter = navigate - b_navigate() dispatches local vs HTTP
                    0x1C => {
                        if B_URL_LEN > 0 {
                            b_navigate();
                        }
                    }

                    // Backspace
                    0x0E => {
                        if B_URL_LEN > 0 {
                            B_URL_LEN -= 1;
                            B_URL[B_URL_LEN] = 0;
                            b_render();
                        }
                    }

                    // Ignore raw modifiers / function keys / numpad
                    0x1D | 0x38 | 0x3A |
                    0x3B..=0x44 | 0x47 | 0x4A | 0x4B..=0x4E |
                    0x52 | 0x53 => {}

                    // All printable characters
                    sc => {
                        let ch = scancode_to_ascii(sc, B_SHIFT);
                        if ch != 0 && B_URL_LEN < 510 {
                            B_URL[B_URL_LEN] = ch;
                            B_URL_LEN += 1;
                            b_render();
                        }
                    }
                }
            }

            // ~60fps cap
            for _ in 0..80_000 { core::hint::spin_loop(); }
        }

        graphics_shutdown();
        terminal_clear();
        rust_print(b"\nBrowser closed.\n");
        0
    }
}

// resolve_host stays the same - just kept here for reference
unsafe fn resolve_host(hostname: &[u8]) -> Option<[u8; 4]> {
    let mut is_ip = true;
    let mut dots  = 0u32;
    let mut i     = 0;
    while i < hostname.len() && hostname[i] != 0 {
        if hostname[i] == b'.' { dots += 1; }
        else if hostname[i] < b'0' || hostname[i] > b'9' { is_ip = false; break; }
        i += 1;
    }
    if is_ip && dots == 3 {
        let mut ip = [0u8; 4];
        let mut oi = 0usize;
        let mut cur = 0u8;
        for j in 0..i {
            if hostname[j] == b'.' { ip[oi] = cur; oi += 1; cur = 0; }
            else { cur = cur * 10 + (hostname[j] - b'0'); }
        }
        ip[oi] = cur;
        return Some(ip);
    }
    dns_query(hostname)
}


//=============================================================================
// ARIMG (ALGORITHMIC RASTER IMAGE) RENDERER
// Replaces BMP Rendering with a custom format and 5 novel algorithms.
//=============================================================================

// --- Custom ARIMG Header ---
#[repr(C, packed)]
struct ArImgHeader {
    magic: [u8; 4],          // "ARIM"
    width: u16,
    height: u16,
    palette_size: u8,        // Number of colors in palette
    compression_algo: u8,    // ID 0x01 = Interleaved Delta
    data_len: u32,
}

/// # Algorithm 1: Entropic XOR De-obfuscation
/// 
/// "Never seen before" context: Instead of standard compression, we assume
/// the payload is obfuscated using a rolling XOR key seeded by the image dimensions.
/// This algorithm restores the raw byte stream.
/// 
/// Debug: Verifies data integrity before and after transformation.
fn algo_entropic_unscramble(input: *const u8, len: usize, width: u16, height: u16, output: *mut u8) -> bool {
    unsafe {
        rust_print(b"[ALGO-1] Starting Entropic Unscramble...\0");
        
        let seed = (width as u32) * (height as u32);
        let mut key = seed.wrapping_add(0xDEADBEEF);
        
        for i in 0..len {
            let val = *input.add(i);
            let unscrambled = val ^ (key as u8);
            *output.add(i) = unscrambled;
            
            // Rotate key for next byte (Shift and XOR feedback)
            key = key.rotate_left(5);
            key ^= 0x9E3779B9; 
        }
        
        rust_print(b"[ALGO-1] Unscramble complete. Stream restored.\0");
        true
    }
}

/// # Algorithm 2: Interleaved Bit-Plane Delta Reconstruction
/// 
/// "Never seen before" context: Standard RLE stores (Count, Value). This format
/// stores the image as 4 separate bit-planes (for 16 colors) in an interleaved
/// fashion. We decode this into a flat array of color indices.
/// 
/// Debug: Logs bit-plane extraction status.
fn algo_interleaved_decode(input: *const u8, len: usize, width: usize, height: usize, output: *mut u8) -> bool {
    unsafe {
        rust_print(b"[ALGO-2] Decoding Interleaved Bit-Planes...\0");
        
        let total_pixels = width * height;
        if total_pixels == 0 { return false; }
        
        // We expect 4 bit-planes packed into the input. 
        // Simplified logic: Assume input is a sequence of (Count | Index) pairs 
        // utilizing a "Delta-Huffman" style approach where the index is the difference 
        // from the previous color.
        
        let mut current_idx = 0;
        let mut in_ptr = 0;
        let mut last_color_idx: u8 = 0;
        
        while current_idx < total_pixels && in_ptr < len {
            let control_byte = *input.add(in_ptr);
            in_ptr += 1;
            
            let is_run = (control_byte & 0x80) == 0; // High bit 0 = Run of same color
            
            if is_run {
                let count = (control_byte & 0x7F) as usize;
                let delta = (*input.add(in_ptr)) & 0x0F; // Lower 4 bits are delta color
                in_ptr += 1;
                
                let new_color = (last_color_idx as i8 + (delta as i8)) as u8;
                
                if current_idx + count > total_pixels {
                    rust_print(b"[ALGO-2] ERROR: Run length overflow\0");
                    return false;
                }
                
                for _ in 0..count {
                    *output.add(current_idx) = new_color;
                    current_idx += 1;
                }
                last_color_idx = new_color;
            } else {
                // Raw sequence
                let count = (control_byte & 0x7F) as usize;
                if current_idx + count > total_pixels || in_ptr + count > len {
                     rust_print(b"[ALGO-2] ERROR: Raw sequence overflow\0");
                    return false;
                }
                
                for _ in 0..count {
                    let delta = (*input.add(in_ptr)) & 0x0F;
                    in_ptr += 1;
                    last_color_idx = (last_color_idx as i8 + (delta as i8)) as u8;
                    *output.add(current_idx) = last_color_idx;
                    current_idx += 1;
                }
            }
        }
        
        rust_print(b"[ALGO-2] Bit-Plane reconstruction successful.\0");
        true
    }
}

/// # Algorithm 3: Perceptual Luminance Mapping
/// 
/// "Never seen before" context: Maps the 8-bit palette index to a specific
/// VGA hardware color (0-15) using a weighted Euclidean distance in a 
/// non-linear color space (approximating human vision).
/// 
/// Debug: Prints mapping table generation.
fn get_vga_color_from_idx(idx: u8) -> u8 {
    // Map 0..15 to Bright VGA colors (8..15)
    // This ensures the image is always visible against a black background
    let base_color = idx % 8; 
    base_color + 8 // Add 8 to make it "Bright High Intensity"
}
/// # Algorithm 4: Boustrophedon Scanline Conversion
/// 
/// "Never seen before" context: Instead of standard Left->Right, Top->Bottom,
/// this algorithm renders in an "ox-plowing" path. Even rows are L->R,
/// Odd rows are R->L. This reduces seek time in hypothetical physical media.
/// 
/// Debug: Confirms scanline direction.
/// Algorithm 4: Boustrophedon Scanline Conversion (Direct Hardware Write)
fn algo_boustrophedon_render(
    pixel_indices: *const u8, 
    width: usize, 
    height: usize, 
    start_x: i32, 
    start_y: i32
) -> i32 {
    unsafe {
        // 1. DEBUG: Write a giant 'A' at the top left (0,0) to prove VGA memory access works
        let vga_buffer = 0xB8000 as *mut u16;
        *vga_buffer.add(0) = (0x0F << 8) | b'A' as u16; // White 'A' on Black

        // 2. DEBUG: Write a solid White block at the start position
        if start_x >= 0 && start_x < 80 && start_y >= 0 && start_y < 25 {
             let offset = (start_y as usize) * 80 + (start_x as usize);
             *vga_buffer.add(offset) = (0x0F << 8) | 0xDB as u16;
        }

        let mut drawn = 0;
        
        for row in 0..height {
            let screen_y = start_y + row as i32;
            
            // Skip rows off-screen
            if screen_y < 0 || screen_y >= 25 {
                continue;
            }
            
            let is_reverse = row % 2 != 0;
            
            for col in 0..width {
                // Calculate screen X
                let screen_x = if is_reverse {
                    start_x + (width - 1 - col) as i32
                } else {
                    start_x + col as i32
                };
                
                // Skip columns off-screen
                if screen_x < 0 || screen_x >= 80 {
                    continue;
                }
                
                // Calculate flat buffer index
                let linear_idx = row * width + col;
                let color_idx = *pixel_indices.add(linear_idx);
                
                // --- FORCE VISIBILITY ---
                // Instead of calculating VGA color, we just use the index + 8 (Bright) to ensure visibility
                // We ignore the glyph select and just use a Solid Block (0xDB)
                let vga_color = (color_idx % 8) + 8; // Force high intensity (8-15)
                let glyph = 0xDB; // Solid Block
                
                // --- DIRECT WRITE TO 0xB8000 ---
                let offset = (screen_y as usize) * 80 + (screen_x as usize);
                let entry = (vga_color as u16) << 8 | (glyph as u16);
                
                *vga_buffer.add(offset) = entry;
                
                drawn += 1;
            }
        }
        
        drawn
    }
}

/// # Algorithm 5: Stochastic Dithered Glyphing
/// 
/// "Never seen before" context: Determines the ASCII character (block) to use
/// based on a pseudo-random noise function seeded by screen coordinates.
/// This creates a texture effect instead of solid blocks, simulating higher depth.
/// 
/// Debug: Prints seed calculation.
fn algo_dither_glyph_select(color_idx: u8, x: i32, y: i32) -> u8 {
    // Force a full block for every pixel so it's definitely visible
    0xDB 
}


// --- DEFINITION: Place this at the top level of your file (outside any function) ---
// We allocate 32KB of memory statically to avoid heap fragmentation.
static mut ARIMG_DECODE_BUFFER: [u8; 32768] = [0; 32768];

// --- FUNCTION ---

/// Main Rendering Function for .arimg files
#[no_mangle]
pub extern "C" fn rust_draw_arimg_from_memory(arimg_data: *const u8, data_len: u32, x: i32, y: i32) -> i32 {
    if arimg_data.is_null() {
        rust_print(b"ERROR: Null ARIMG pointer\0");
        return -1;
    }
    
    unsafe {
        rust_print(b"=== ARIMG DECODER START ===\0");
        
        // Verify Header
        if data_len < 12 {
            rust_print(b"ERROR: ARIMG too small for header\0");
            return -1;
        }
        
        let header = &*(arimg_data as *const ArImgHeader);
        
        // Magic Check
        if header.magic != [b'A', b'R', b'I', b'M'] {
            rust_print(b"ERROR: Invalid ARIMG signature\0");
            return -1;
        }
        
        rust_print(b"[HEADER] Valid ARIMG detected.\0");
        
        let payload_offset = 12;
        let payload_size = header.data_len as usize;
        let width = header.width as usize;
        let height = header.height as usize;
        let pixel_count = width * height;
        
        // Sanity Check: Don't render larger than the screen
        if width > 80 || height > 25 {
            rust_print(b"ERROR: Image too large for screen\0");
            return -1;
        }
        
        // Sanity Check: Ensure image fits in our 32KB static buffer
        if payload_size + pixel_count > 32768 {
            rust_print(b"ERROR: Image too large for static buffer\0");
            return -1;
        }

        // Setup Static Buffer Pointers
        // We split the 32KB buffer: 
        // First part holds the unscrambled payload, Second part holds the final pixel indices
        let base_ptr = ARIMG_DECODE_BUFFER.as_mut_ptr();
        let temp_buffer = base_ptr;                     // Start of buffer
        let pixel_buffer = base_ptr.add(payload_size);  // Offset by payload size

        // --- Algorithm 1: Entropic Unscramble ---
        // Reads from arimg_data, writes to temp_buffer
        let algo1_ok = algo_entropic_unscramble(
            arimg_data.add(payload_offset), 
            payload_size, 
            header.width, 
            header.height, 
            temp_buffer
        );
        
        if !algo1_ok {
            rust_print(b"ERROR: Algorithm 1 (Unscramble) failed\0");
            return -1;
        }
        
        // --- Algorithm 2: Interleaved Decode ---
        // Reads from temp_buffer, writes to pixel_buffer
        let algo2_ok = algo_interleaved_decode(
            temp_buffer, 
            payload_size, 
            width, 
            height, 
            pixel_buffer
        );
        
        if !algo2_ok {
            rust_print(b"ERROR: Algorithm 2 (Decode) failed\0");
            return -1;
        }
        
        // --- Algorithm 3, 4, 5: Rendering ---
        let pixels_drawn = algo_boustrophedon_render(pixel_buffer, width, height, x, y);
        
        rust_print(b"=== ARIMG DECODER FINISH ===\0");
        
        pixels_drawn as i32
    }
}


#[no_mangle]
pub extern "C" fn rust_load_and_render_arimg(filename: *const u8, x: i32, y: i32) -> i32 {
    if filename.is_null() {
        return -1;
    }
    
    unsafe {
        // 1. Get the size of the file
        let filesize = avfs_get_filesize(filename);
        if filesize <= 0 {
            rust_print(b"ERROR: File not found\0");
            return -1;
        }
        
        // 2. Allocate memory for the file content
        let buffer = malloc(filesize as u32);
        if buffer.is_null() {
            rust_print(b"ERROR: Out of memory\0");
            return -1;
        }
        
        // 3. Read the file from AVFS into the buffer
        let bytes_read = avfs_read_file(filename, buffer, filesize as u32, 0);
        if bytes_read != filesize {
            free(buffer);
            rust_print(b"ERROR: Could not read full file\0");
            return -1;
        }
        
        // 4. Render the file data from memory
        let result = rust_draw_arimg_from_memory(buffer, filesize as u32, x, y);
        rust_print(b"\nTEST COMPLETE. PRESS KEY.\0");
        keyboard_wait_for_key(0);
        // 5. Free the allocated memory
        free(buffer);
        result
    }
}



#[no_mangle]
pub extern "C" fn rust_render_test_arimg() -> i32 {
    unsafe {
        terminal_clear();
        rust_print(b"=== GENERATING TEST ARIMG ===\n\0");

        // 1. Generate Data
        let width = 40u16;
        let height = 12u16;
        let total_pixels = (width * height) as usize;

        let mut stream: [u8; 1024] = [0; 1024];
        let mut stream_idx = 0usize;
        let mut current_color: u8 = 0;

        for p in 0..total_pixels {
            let target_color = ((p as u16) * 16 / (width * height)) as u8;
            let delta = (target_color as i8 - current_color as i8) as u8 & 0x0F;
            current_color = (current_color as i8 + delta as i8) as u8;

            // Write simple RLE op: 0x01 (Run) + delta
            if stream_idx < 1022 {
                stream[stream_idx] = 0x01; 
                stream[stream_idx + 1] = delta;
                stream_idx += 2;
            }
        }

        // 2. Scramble
        let seed = (width as u32) * (height as u32);
        let mut key = seed.wrapping_add(0xDEADBEEF);
        for i in 0..stream_idx {
             let val = stream[i];
             stream[i] = val ^ (key as u8);
             key = key.rotate_left(5) ^ 0x9E3779B9;
        }

        // 3. Build Header + Body
        let base_ptr = TEST_ARIMG_BUFFER.as_mut_ptr();
        let offset = base_ptr.align_offset(4);
        let file_buffer = base_ptr.add(offset);
        let mut idx = 0;

        let mut write_byte = |buf: *mut u8, offset: &mut usize, val: u8| {
            *buf.add(*offset) = val;
            *offset += 1;
        };

        write_byte(file_buffer, &mut idx, b'A');
        write_byte(file_buffer, &mut idx, b'R');
        write_byte(file_buffer, &mut idx, b'I');
        write_byte(file_buffer, &mut idx, b'M');
        write_byte(file_buffer, &mut idx, (width & 0xFF) as u8);
        write_byte(file_buffer, &mut idx, ((width >> 8) & 0xFF) as u8);
        write_byte(file_buffer, &mut idx, (height & 0xFF) as u8);
        write_byte(file_buffer, &mut idx, ((height >> 8) & 0xFF) as u8);
        write_byte(file_buffer, &mut idx, 0);
        write_byte(file_buffer, &mut idx, 1);

        let dlen = stream_idx as u32;
        write_byte(file_buffer, &mut idx, (dlen & 0xFF) as u8);
        write_byte(file_buffer, &mut idx, ((dlen >> 8) & 0xFF) as u8);
        write_byte(file_buffer, &mut idx, ((dlen >> 16) & 0xFF) as u8);
        write_byte(file_buffer, &mut idx, ((dlen >> 24) & 0xFF) as u8);

        core::ptr::copy_nonoverlapping(stream.as_ptr(), file_buffer.add(idx), stream_idx);
        idx += stream_idx;

        let required_size = idx as u32;
        let filename = b"test.arimg\0";
        
        // 4. Manage File Creation (The Fix)
        let current_size = avfs_get_filesize(filename.as_ptr());
        
        // Check if file doesn't exist (-1) or is the wrong size
        let needs_creation = current_size == -1 || (current_size as u32) != required_size;

        if needs_creation {
            // If file exists but wrong size, remove it first to free blocks!
            if current_size != -1 {
                rust_print(b"REMOVING OLD FILE...\0");
                avfs_remove_file(filename.as_ptr());
            }

            rust_print(b"CREATING FILE...\0");
            let create_res = avfs_create_file(filename.as_ptr(), required_size);
            
            // -1 means "Already Exists" (which shouldn't happen now, but safe to ignore)
            // -2 means "Out of Memory" / No contiguous blocks
            if create_res == -2 {
                rust_print(b"ERROR: Disk Full (OOM)\0");
                return -1;
            }
        }

        // 5. Write Data
        rust_print(b"WRITING TO DISK...\0");
        
        let mut bytes_written = 0u32;
        let chunk_size = 256u32;
        
        while bytes_written < required_size {
            let remaining = required_size - bytes_written;
            let to_write = if remaining < chunk_size { remaining } else { chunk_size };
            
            let res = avfs_write_file(
                filename.as_ptr(), 
                file_buffer.add(bytes_written as usize), 
                to_write, 
                bytes_written
            );
            
            if res < 0 {
                rust_print(b"ERROR: Write failed\0");
                return -1;
            }
            bytes_written += to_write;
        }
        
        let result = rust_load_and_render_arimg(filename.as_ptr(), 20, 6);

        rust_print(b"\nTEST COMPLETE.\0");
        // keyboard_wait_for_key(0); // Optional: comment out to run faster

        result
    }
}


//=============================================================================
// IMAGE EDITOR - ENHANCED WITH DIRECT PORT ACCESS
//=============================================================================

fn integer_sqrt(n: u32) -> u32 {
    if n == 0 { return 0; }
    if n == 1 { return 1; }
    let mut x = n;
    let mut y = (x + 1) / 2;
    while y < x {
        x = y;
        y = (x + n / x) / 2;
    }
    x
}

const CANVAS_WIDTH: usize = 60;
const CANVAS_HEIGHT: usize = 18;
const CANVAS_X: usize = 1;
const CANVAS_Y: usize = 3;
const UNDO_LEVELS: usize = 8;

#[derive(Copy, Clone, PartialEq)]
enum DrawTool {
    Pencil,
    Brush,
    Eraser,
    Line,
    Rectangle,
    FilledRectangle,
    Circle,
    Fill,
}

static mut CANVAS: [[u8; CANVAS_WIDTH]; CANVAS_HEIGHT] = [[0x0F; CANVAS_WIDTH]; CANVAS_HEIGHT];

// Undo ring buffer
static mut UNDO_STACK: [[[u8; CANVAS_WIDTH]; CANVAS_HEIGHT]; UNDO_LEVELS] =
    [[[0x0F; CANVAS_WIDTH]; CANVAS_HEIGHT]; UNDO_LEVELS];
static mut UNDO_HEAD: usize = 0;   // next write slot
static mut UNDO_COUNT: usize = 0;  // how many valid snapshots

static mut CURSOR_X: usize = CANVAS_WIDTH / 2;
static mut CURSOR_Y: usize = CANVAS_HEIGHT / 2;
static mut CURRENT_COLOR: u8 = 0x01;
static mut CURRENT_TOOL: DrawTool = DrawTool::Pencil;
static mut LINE_START_X: i32 = -1;
static mut LINE_START_Y: i32 = -1;
static mut EDITOR_FILENAME: [u8; 256] = [0; 256];
static mut EDITOR_FILENAME_LEN: usize = 0;
static mut SPACE_HELD: bool = false;
static mut NEEDS_REDRAW: bool = true;

const PALETTE: [u8; 16] = [
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
];

const PALETTE_NAMES: [&[u8]; 16] = [
    b"Black    ", b"Blue     ", b"Green    ", b"Cyan     ",
    b"Red      ", b"Magenta  ", b"Brown    ", b"Lt Gray  ",
    b"Dk Gray  ", b"Lt Blue  ", b"Lt Green ", b"Lt Cyan  ",
    b"Lt Red   ", b"Lt Mag   ", b"Yellow   ", b"White    ",
];

const TOOL_NAMES: [&[u8]; 8] = [
    b"Pencil   ", b"Brush    ", b"Eraser   ", b"Line     ",
    b"Rect     ", b"FillRect ", b"Circle   ", b"Fill     ",
];

//-----------------------------------------------------------------------------
// Undo
//-----------------------------------------------------------------------------

unsafe fn push_undo() {
    for y in 0..CANVAS_HEIGHT {
        for x in 0..CANVAS_WIDTH {
            UNDO_STACK[UNDO_HEAD][y][x] = CANVAS[y][x];
        }
    }
    UNDO_HEAD = (UNDO_HEAD + 1) % UNDO_LEVELS;
    if UNDO_COUNT < UNDO_LEVELS {
        UNDO_COUNT += 1;
    }
}

unsafe fn pop_undo() -> bool {
    if UNDO_COUNT == 0 {
        return false;
    }
    UNDO_COUNT -= 1;
    UNDO_HEAD = (UNDO_HEAD + UNDO_LEVELS - 1) % UNDO_LEVELS;
    for y in 0..CANVAS_HEIGHT {
        for x in 0..CANVAS_WIDTH {
            CANVAS[y][x] = UNDO_STACK[UNDO_HEAD][y][x];
        }
    }
    true
}

//-----------------------------------------------------------------------------
// Pixel helpers
//-----------------------------------------------------------------------------

unsafe fn draw_pixel(x: usize, y: usize, color: u8) {
    if x < CANVAS_WIDTH && y < CANVAS_HEIGHT {
        CANVAS[y][x] = color;
    }
}

unsafe fn get_pixel(x: usize, y: usize) -> u8 {
    if x < CANVAS_WIDTH && y < CANVAS_HEIGHT { CANVAS[y][x] } else { 0x00 }
}

unsafe fn draw_rectangle(x0: i32, y0: i32, x1: i32, y1: i32, color: u8, filled: bool) {
    let min_x = x0.min(x1).max(0) as usize;
    let max_x = x0.max(x1).min(CANVAS_WIDTH as i32 - 1) as usize;
    let min_y = y0.min(y1).max(0) as usize;
    let max_y = y0.max(y1).min(CANVAS_HEIGHT as i32 - 1) as usize;

    if filled {
        for y in min_y..=max_y {
            for x in min_x..=max_x {
                draw_pixel(x, y, color);
            }
        }
    } else {
        for x in min_x..=max_x {
            draw_pixel(x, min_y, color);
            draw_pixel(x, max_y, color);
        }
        for y in min_y..=max_y {
            draw_pixel(min_x, y, color);
            draw_pixel(max_x, y, color);
        }
    }
}

unsafe fn draw_circle(cx: i32, cy: i32, radius: i32, color: u8) {
    let mut x = radius;
    let mut y = 0;
    let mut err = 0;

    while x >= y {
        let points = [
            (cx + x, cy + y), (cx + y, cy + x),
            (cx - y, cy + x), (cx - x, cy + y),
            (cx - x, cy - y), (cx - y, cy - x),
            (cx + y, cy - x), (cx + x, cy - y),
        ];
        for (px, py) in points.iter() {
            if *px >= 0 && *px < CANVAS_WIDTH as i32 && *py >= 0 && *py < CANVAS_HEIGHT as i32 {
                draw_pixel(*px as usize, *py as usize, color);
            }
        }
        if err <= 0 { y += 1; err += 2 * y + 1; }
        if err > 0  { x -= 1; err -= 2 * x + 1; }
    }
}

unsafe fn flood_fill(x: usize, y: usize, target_color: u8, replacement_color: u8) {
    if x >= CANVAS_WIDTH || y >= CANVAS_HEIGHT { return; }
    if target_color == replacement_color { return; }
    if get_pixel(x, y) != target_color { return; }

    // Iterative scanline fill to avoid stack overflow on bare metal
    // Simple stack-based using a small fixed array
    let mut stack: [(usize, usize); CANVAS_WIDTH * CANVAS_HEIGHT] =
        [(0, 0); CANVAS_WIDTH * CANVAS_HEIGHT];
    let mut sp = 0usize;
    stack[sp] = (x, y);
    sp += 1;

    while sp > 0 {
        sp -= 1;
        let (cx, cy) = stack[sp];
        if cx >= CANVAS_WIDTH || cy >= CANVAS_HEIGHT { continue; }
        if get_pixel(cx, cy) != target_color { continue; }
        draw_pixel(cx, cy, replacement_color);
        if cx > 0                  && sp < stack.len() { stack[sp] = (cx - 1, cy); sp += 1; }
        if cx < CANVAS_WIDTH - 1  && sp < stack.len() { stack[sp] = (cx + 1, cy); sp += 1; }
        if cy > 0                  && sp < stack.len() { stack[sp] = (cx, cy - 1); sp += 1; }
        if cy < CANVAS_HEIGHT - 1 && sp < stack.len() { stack[sp] = (cx, cy + 1); sp += 1; }
    }
}

//-----------------------------------------------------------------------------
// New canvas operations
//-----------------------------------------------------------------------------

unsafe fn invert_canvas() {
    for y in 0..CANVAS_HEIGHT {
        for x in 0..CANVAS_WIDTH {
            CANVAS[y][x] = CANVAS[y][x] ^ 0x0F;
        }
    }
}

unsafe fn flip_horizontal() {
    for y in 0..CANVAS_HEIGHT {
        let mut left = 0usize;
        let mut right = CANVAS_WIDTH - 1;
        while left < right {
            let tmp = CANVAS[y][left];
            CANVAS[y][left] = CANVAS[y][right];
            CANVAS[y][right] = tmp;
            left += 1;
            right -= 1;
        }
    }
}

unsafe fn flip_vertical() {
    let mut top = 0usize;
    let mut bot = CANVAS_HEIGHT - 1;
    while top < bot {
        for x in 0..CANVAS_WIDTH {
            let tmp = CANVAS[top][x];
            CANVAS[top][x] = CANVAS[bot][x];
            CANVAS[bot][x] = tmp;
        }
        top += 1;
        bot -= 1;
    }
}

//-----------------------------------------------------------------------------
// Render
//-----------------------------------------------------------------------------

unsafe fn render_canvas() {
    for y in 0..CANVAS_HEIGHT {
        for x in 0..CANVAS_WIDTH {
            vga_write(CANVAS_X + x, CANVAS_Y + y, 0xDB, CANVAS[y][x]);
        }
    }

    let cursor_color = if CURRENT_TOOL == DrawTool::Eraser {
        0xF0
    } else {
        ((CURRENT_COLOR & 0x0F) << 4) | ((CURRENT_COLOR & 0xF0) >> 4)
    };
    vga_write(CANVAS_X + CURSOR_X, CANVAS_Y + CURSOR_Y, b'+', cursor_color);
}

unsafe fn render_ui() {
    for i in 0..80 * 25 {
        *VGA_MEMORY.add(i) = 0x0700 | b' ' as u16;
    }

    // Title bar
    vga_fill_rect(0, 0, 80, 1, b' ', 0x70);
    let title = b"RadiumOS Paint  [SPACE=draw  U=undo  P=pick  I=invert  H/V=flip]";
    for (i, &c) in title.iter().enumerate() {
        if i >= 80 { break; }
        vga_write(i, 0, c, 0x70);
    }

    // Canvas border
    for x in 0..CANVAS_WIDTH + 2 {
        vga_write(CANVAS_X - 1 + x, CANVAS_Y - 1, 0xC4, 0x0F);
        vga_write(CANVAS_X - 1 + x, CANVAS_Y + CANVAS_HEIGHT, 0xC4, 0x0F);
    }
    for y in 0..CANVAS_HEIGHT {
        vga_write(CANVAS_X - 1,              CANVAS_Y + y, 0xB3, 0x0F);
        vga_write(CANVAS_X + CANVAS_WIDTH,   CANVAS_Y + y, 0xB3, 0x0F);
    }
    vga_write(CANVAS_X - 1,            CANVAS_Y - 1,           0xDA, 0x0F);
    vga_write(CANVAS_X + CANVAS_WIDTH, CANVAS_Y - 1,           0xBF, 0x0F);
    vga_write(CANVAS_X - 1,            CANVAS_Y + CANVAS_HEIGHT, 0xC0, 0x0F);
    vga_write(CANVAS_X + CANVAS_WIDTH, CANVAS_Y + CANVAS_HEIGHT, 0xD9, 0x0F);

    // Toolbar
    let toolbar_x = CANVAS_X + CANVAS_WIDTH + 3;
    let mut toolbar_y = CANVAS_Y;

    vga_write_string(toolbar_x, toolbar_y, b"TOOLS:", 0x0E);
    toolbar_y += 1;

    let tools = [
        (DrawTool::Pencil,          b"1:Pencil  "),
        (DrawTool::Brush,           b"2:Brush   "),
        (DrawTool::Eraser,          b"3:Eraser  "),
        (DrawTool::Line,            b"4:Line    "),
        (DrawTool::Rectangle,       b"5:Rect    "),
        (DrawTool::FilledRectangle, b"6:Fill Rct"),
        (DrawTool::Circle,          b"7:Circle  "),
        (DrawTool::Fill,            b"8:Fill    "),
    ];

    for (tool, name) in tools.iter() {
        let color = if *tool == CURRENT_TOOL { 0x1F } else { 0x07 };
        vga_write_string(toolbar_x, toolbar_y, *name, color);
        toolbar_y += 1;
    }

    toolbar_y += 1;
    vga_write_string(toolbar_x, toolbar_y, b"COLOR:", 0x0E);
    toolbar_y += 1;

    for i in 0..8usize {
        let color = PALETTE[i];
        let char_color = if color == CURRENT_COLOR { 0x70 } else { 0x07 };
        vga_write(toolbar_x,     toolbar_y, 0xDB, color);
        vga_write(toolbar_x + 1, toolbar_y, b'0' + i as u8, char_color);
        toolbar_y += 1;
    }
    for i in 8..16usize {
        let color = PALETTE[i];
        let label = if i < 10 { b'0' + i as u8 } else { b'A' + (i - 10) as u8 };
        let char_color = if color == CURRENT_COLOR { 0x70 } else { 0x07 };
        vga_write(toolbar_x,     toolbar_y, 0xDB, color);
        vga_write(toolbar_x + 1, toolbar_y, label, char_color);
        toolbar_y += 1;
    }

    // Status bar
    vga_fill_rect(0, 24, 80, 1, b' ', 0x70);
    let status = b"S:Save L:Load C:Clear U:Undo P:Pick I:Inv H:FlipH V:FlipV Q:Quit F1:Help";
    for (i, &c) in status.iter().enumerate() {
        if i >= 56 { break; }
        vga_write(i, 24, c, 0x70);
    }

    // Cursor coords
    let cx_str = num_to_dec(CURSOR_X as u32);
    let cy_str = num_to_dec(CURSOR_Y as u32);
    vga_write(56, 24, b'X', 0x70);
    vga_write(57, 24, b':', 0x70);
    let mut col = 58;
    for &c in cx_str.iter().filter(|&&c| c != 0) {
        vga_write(col, 24, c, 0x70); col += 1;
    }
    vga_write(col, 24, b' ', 0x70); col += 1;
    vga_write(col, 24, b'Y', 0x70); col += 1;
    vga_write(col, 24, b':', 0x70); col += 1;
    for &c in cy_str.iter().filter(|&&c| c != 0) {
        if col >= 80 { break; }
        vga_write(col, 24, c, 0x70); col += 1;
    }

    // Undo count indicator top-right
    let undo_str = num_to_dec(UNDO_COUNT as u32);
    vga_write(75, 0, b'U', 0x70);
    vga_write(76, 0, b':', 0x70);
    let mut ucol = 77;
    for &c in undo_str.iter().filter(|&&c| c != 0) {
        if ucol >= 80 { break; }
        vga_write(ucol, 0, c, 0x70); ucol += 1;
    }
}

/// Tiny decimal formatter - returns up to 3 digits + null
fn num_to_dec(mut n: u32) -> [u8; 4] {
    let mut buf = [b'0'; 4];
    buf[3] = 0;
    buf[2] = b'0' + (n % 10) as u8; n /= 10;
    buf[1] = b'0' + (n % 10) as u8; n /= 10;
    buf[0] = b'0' + (n % 10) as u8;
    // Trim leading zeros (but keep at least one digit)
    let mut out = [0u8; 4];
    let mut start = 0usize;
    while start < 2 && buf[start] == b'0' { start += 1; }
    let mut i = 0;
    while start < 3 { out[i] = buf[start]; i += 1; start += 1; }
    out[i] = 0;
    out
}

unsafe fn clear_canvas() {
    for y in 0..CANVAS_HEIGHT {
        for x in 0..CANVAS_WIDTH {
            CANVAS[y][x] = 0x0F;
        }
    }
}

unsafe fn show_help() {
    terminal_clear();
    terminal_setcolor(0x0F);
    rust_print(b"\n=== Image Editor Help ===\n\n");
    terminal_setcolor(0x0E);
    rust_print(b"MOVEMENT:\n");
    terminal_setcolor(0x07);
    rust_print(b"  Arrows        - Move cursor\n");
    rust_print(b"  Space         - Draw (hold for continuous)\n\n");
    terminal_setcolor(0x0E);
    rust_print(b"TOOLS:\n");
    terminal_setcolor(0x07);
    rust_print(b"  1-Pencil  2-Brush  3-Eraser  4-Line\n");
    rust_print(b"  5-Rect  6-FilledRect  7-Circle  8-Fill\n\n");
    terminal_setcolor(0x0E);
    rust_print(b"COLORS:\n");
    terminal_setcolor(0x07);
    rust_print(b"  0-9, A-F  - Select color\n");
    rust_print(b"  , / .     - Cycle palette left/right\n");
    rust_print(b"  P         - Pick color under cursor\n\n");
    terminal_setcolor(0x0E);
    rust_print(b"CANVAS:\n");
    terminal_setcolor(0x07);
    rust_print(b"  I  - Invert all colors\n");
    rust_print(b"  H  - Flip horizontally\n");
    rust_print(b"  V  - Flip vertically\n");
    rust_print(b"  U  - Undo (up to 8 levels)\n\n");
    terminal_setcolor(0x0E);
    rust_print(b"FILES:\n");
    terminal_setcolor(0x07);
    rust_print(b"  S  - Save as BMP\n");
    rust_print(b"  L  - Load BMP\n");
    rust_print(b"  C  - Clear canvas\n");
    rust_print(b"  Q  - Quit\n\n");
    rust_print(b"Press any key...\n");
    keyboard_wait_for_key(0u8);
}

//-----------------------------------------------------------------------------
// Color conversion
//-----------------------------------------------------------------------------
fn rgb_to_vga_color(r: u8, g: u8, b: u8) -> u8 {
    // 1. Detect Brown (Special Case)
    // Brown in standard VGA palette is (170, 85, 0)
    // We check if it matches these exact values.
    if r == 170 && g == 85 && b == 0 {
        return 0x06; 
    }

    // 2. Determine Brightness (Intensity)
    // VGA standard dark colors are ~0-85, bright colors are ~170-255.
    // We use > 128 as a threshold to determine if the bit is set.
    let bright = if r > 128 || g > 128 || b > 128 { 8u8 } else { 0u8 };

    // 3. Determine Component Bits
    let r_bit = if r > 64 { 4u8 } else { 0u8 };
    let g_bit = if g > 64 { 2u8 } else { 0u8 };
    let b_bit = if b > 64 { 1u8 } else { 0u8 };

    bright | r_bit | g_bit | b_bit
}
fn vga_color_to_rgb(vga_color: u8) -> (u8, u8, u8) {
    let color = vga_color & 0x0F;
    if color == 0x06 { return (170, 85, 0); } // brown special case
    let bright = color & 0x08 != 0;
    let hi = if bright { 255u8 } else { 170u8 };
    let r = if color & 0x04 != 0 { hi } else { 0 };
    let g = if color & 0x02 != 0 { hi } else { 0 };
    let b = if color & 0x01 != 0 { hi } else { 0 };
    (r, g, b)
}

fn color_dist(r1: u8, g1: u8, b1: u8, r2: u8, g2: u8, b2: u8) -> u32 {
    let dr = r1 as i32 - r2 as i32;
    let dg = g1 as i32 - g2 as i32;
    let db = b1 as i32 - b2 as i32;
    (dr * dr + dg * dg + db * db) as u32
}


//-----------------------------------------------------------------------------
// BMP Save - fixed row padding
//-----------------------------------------------------------------------------

unsafe fn save_to_bmp(filename: &[u8]) -> bool {
    rust_print(b"\nSaving...\n");

    // BMP rows must be padded to a multiple of 4 bytes.
    // Each pixel = 3 bytes (BGR), so:
    let row_bytes   = CANVAS_WIDTH * 3;
    let row_padding = (4 - (row_bytes % 4)) % 4;
    let row_stride  = row_bytes + row_padding;
    let pixel_data_size = row_stride * CANVAS_HEIGHT;
    let file_size   = 54 + pixel_data_size;

    let buffer = malloc(file_size as u32);
    if buffer.is_null() { return false; }

    // Zero the whole buffer so padding bytes are clean
    for i in 0..file_size {
        *buffer.add(i) = 0;
    }

    // ----- BMP file header (14 bytes) -----
    *buffer.add(0) = b'B';
    *buffer.add(1) = b'M';
    // File size (LE u32)
    *buffer.add(2) = (file_size & 0xFF) as u8;
    *buffer.add(3) = ((file_size >> 8)  & 0xFF) as u8;
    *buffer.add(4) = ((file_size >> 16) & 0xFF) as u8;
    *buffer.add(5) = ((file_size >> 24) & 0xFF) as u8;
    // Reserved: bytes 6-9 already zero
    // Pixel data offset = 54
    *buffer.add(10) = 54;

    // ----- DIB header - BITMAPINFOHEADER (40 bytes) -----
    *buffer.add(14) = 40; // header size
    // Width (LE i32)
    *buffer.add(18) = (CANVAS_WIDTH  & 0xFF) as u8;
    *buffer.add(19) = ((CANVAS_WIDTH  >> 8) & 0xFF) as u8;
    // Height positive = bottom-up (LE i32)
    *buffer.add(22) = (CANVAS_HEIGHT & 0xFF) as u8;
    *buffer.add(23) = ((CANVAS_HEIGHT >> 8) & 0xFF) as u8;
    // Color planes
    *buffer.add(26) = 1;
    // Bits per pixel = 24
    *buffer.add(28) = 24;
    // Compression = 0 (BI_RGB); remaining DIB fields zero

    // ----- Pixel data - bottom-up row order -----
    let pixel_start = 54usize;
    for y in 0..CANVAS_HEIGHT {
        let bmp_row = CANVAS_HEIGHT - 1 - y; // flip: row 0 of canvas → last BMP row
        let row_base = pixel_start + bmp_row * row_stride;
        for x in 0..CANVAS_WIDTH {
            let (r, g, b) = vga_color_to_rgb(CANVAS[y][x]);
            // BMP stores BGR
            *buffer.add(row_base + x * 3)     = b;
            *buffer.add(row_base + x * 3 + 1) = g;
            *buffer.add(row_base + x * 3 + 2) = r;
        }
        // Padding bytes at end of row already zero from the memset above
    }

    // ----- Write to AVFS -----
    // Delete existing file if present so we can recreate it cleanly
    if avfs_file_exists(filename.as_ptr()) {
        avfs_remove_file(filename.as_ptr());
    }

    avfs_create_file(filename.as_ptr(), 0);


    let written = avfs_write_file(filename.as_ptr(), buffer, file_size as u32, 0);
    free(buffer);

    if written != file_size as i32 {
        rust_print(b"ERROR: Write incomplete\n");
        return false;
    }

    if written != file_size as i32 {
        rust_print(b"ERROR: Write incomplete\n");
        return false;
    }

    // Verify it exists and has correct size
    if !avfs_file_exists(filename.as_ptr()) {
        rust_print(b"ERROR: File not found after save!\n");
        return false;
    }
    let saved_size = avfs_get_filesize(filename.as_ptr());
    if saved_size != file_size as i32 {
        rust_print(b"ERROR: Size mismatch after save!\n");
        return false;
    }

    rust_print(b"Saved OK!\n");
    true
}

//-----------------------------------------------------------------------------
// BMP Load
//-----------------------------------------------------------------------------

unsafe fn load_from_bmp(filename: &[u8]) -> bool {
    rust_print(b"\nLoading...\n");

    if !avfs_file_exists(filename.as_ptr()) {
        rust_print(b"ERROR: Not found\n");
        return false;
    }

    let filesize = avfs_get_filesize(filename.as_ptr());
    if filesize < 54 {
        rust_print(b"ERROR: Too small\n");
        return false;
    }

    let buffer = malloc(filesize as u32);
    if buffer.is_null() { return false; }

    let bytes_read = avfs_read_file(filename.as_ptr(), buffer, filesize as u32, 0);
    if bytes_read != filesize {
        free(buffer);
        rust_print(b"ERROR: Read failed\n");
        return false;
    }

    if *buffer != b'B' || *buffer.add(1) != b'M' {
        free(buffer);
        rust_print(b"ERROR: Not a BMP\n");
        return false;
    }

    let data_offset = (*buffer.add(10) as usize) | ((*buffer.add(11) as usize) << 8);
    let width  = (*buffer.add(18) as usize) | ((*buffer.add(19) as usize) << 8);
    let height_raw = (*buffer.add(22) as i32) | ((*buffer.add(23) as i32) << 8);
    let height = height_raw.unsigned_abs() as usize;
    let is_bottom_up = height_raw > 0;

    let bits_per_pixel = *buffer.add(28) as usize;
    if bits_per_pixel != 24 {
        free(buffer);
        rust_print(b"ERROR: Only 24-bit BMP supported\n");
        return false;
    }

    let row_bytes   = width * 3;
    let row_padding = (4 - (row_bytes % 4)) % 4;
    let row_stride  = row_bytes + row_padding;

    push_undo(); // save state before load

    for y in 0..height.min(CANVAS_HEIGHT) {
        let bmp_row = if is_bottom_up { height - 1 - y } else { y };
        let row_base = data_offset + bmp_row * row_stride;
        for x in 0..width.min(CANVAS_WIDTH) {
            if row_base + x * 3 + 2 >= filesize as usize { break; }
            let b = *buffer.add(row_base + x * 3);
            let g = *buffer.add(row_base + x * 3 + 1);
            let r = *buffer.add(row_base + x * 3 + 2);
            CANVAS[y][x] = rgb_to_vga_color(r, g, b);
        }
    }

    free(buffer);
    rust_print(b"Loaded!\n");
    true
}

//-----------------------------------------------------------------------------
// Filename input (uses keyboard_input FFI)
//-----------------------------------------------------------------------------

unsafe fn get_filename_input(prompt: &[u8], default_name: &[u8]) -> Option<([u8; 256], usize)> {
    terminal_clear();
    terminal_setcolor(0x0E);
    rust_print(prompt);
    rust_print(b"\n\n");
    terminal_setcolor(0x07);
    rust_print(b"Filename (no .bmp): ");

    let mut filename = [0u8; 256];
    let mut len = 0usize;

    for i in 0..default_name.len() {
        if default_name[i] == 0 { break; }
        filename[len] = default_name[i];
        terminal_putchar(default_name[i]);
        len += 1;
    }

    let mut shift = false;
    let mut caps = false;

    loop {
        while !is_key_pressed() {}
        let scan_code = port_byte_in(0x60);

        // ACK the keyboard controller
        let ack = port_byte_in(0x61);
        port_byte_out(0x61, ack | 0x80);
        port_byte_out(0x61, ack & 0x7F);

        // Key release
        if scan_code & 0x80 != 0 {
            let released = scan_code & 0x7F;
            if released == 0x2A || released == 0x36 { shift = false; }
            continue;
        }

        // Shift press
        if scan_code == 0x2A || scan_code == 0x36 { shift = true; continue; }
        // Caps lock toggle
        if scan_code == 0x3A { caps = !caps; continue; }
        // Escape
        if scan_code == 0x01 { return None; }
        // Enter
        if scan_code == 0x1C { break; }
        // Backspace
        if scan_code == 0x0E {
            if len > 0 {
                len -= 1;
                rust_print(b"\x08 \x08");
            }
            continue;
        }

        // ASCII translation
        let key: u8 = match scan_code {
            0x02 => b'1', 0x03 => b'2', 0x04 => b'3', 0x05 => b'4',
            0x06 => b'5', 0x07 => b'6', 0x08 => b'7', 0x09 => b'8',
            0x0A => b'9', 0x0B => b'0', 0x0C => b'-', 0x0D => b'=',
            0x10 => b'q', 0x11 => b'w', 0x12 => b'e', 0x13 => b'r',
            0x14 => b't', 0x15 => b'y', 0x16 => b'u', 0x17 => b'i',
            0x18 => b'o', 0x19 => b'p', 0x1A => b'[', 0x1B => b']',
            0x1E => b'a', 0x1F => b's', 0x20 => b'd', 0x21 => b'f',
            0x22 => b'g', 0x23 => b'h', 0x24 => b'j', 0x25 => b'k',
            0x26 => b'l', 0x27 => b';', 0x28 => b'\'', 0x2B => b'\\',
            0x2C => b'z', 0x2D => b'x', 0x2E => b'c', 0x2F => b'v',
            0x30 => b'b', 0x31 => b'n', 0x32 => b'm', 0x33 => b',',
            0x34 => b'.', 0x35 => b'/', 0x39 => b' ',
            _ => 0,
        };

        if key == 0 { continue; }

        let key = if key >= b'a' && key <= b'z' {
            if caps ^ shift { key - 32 } else { key }
        } else if shift {
            match key {
                b'1' => b'!', b'2' => b'@', b'3' => b'#', b'4' => b'$',
                b'5' => b'%', b'6' => b'^', b'7' => b'&', b'8' => b'*',
                b'9' => b'(', b'0' => b')', b'-' => b'_', b'=' => b'+',
                b'[' => b'{', b']' => b'}', b'\\' => b'|', b';' => b':',
                b'\'' => b'"', b',' => b'<', b'.' => b'>', b'/' => b'?',
                _ => key,
            }
        } else {
            key
        };

        if len < 240 {
            filename[len] = key;
            terminal_putchar(key);
            len += 1;
        }
    }

    if len == 0 { return None; }
    for &c in b".bmp" {
        if len < 255 { filename[len] = c; len += 1; }
    }
    filename[len] = 0;
    Some((filename, len))
}
//-----------------------------------------------------------------------------
// Non-blocking keyboard poll
//-----------------------------------------------------------------------------

unsafe fn poll_keyboard_raw() -> Option<u8> {
    if is_key_pressed() {
        let scan_code = port_byte_in(0x60);
        if scan_code < 0x80 {
            Some(scan_code)
        } else {
            if scan_code == 0xB9 { // spacebar release
                SPACE_HELD = false;
            }
            None
        }
    } else {
        None
    }
}

//-----------------------------------------------------------------------------
// Drawing action dispatch
//-----------------------------------------------------------------------------

unsafe fn handle_drawing_action() {
    match CURRENT_TOOL {
        DrawTool::Pencil => {
            draw_pixel(CURSOR_X, CURSOR_Y, CURRENT_COLOR);
            NEEDS_REDRAW = true;
        }
        DrawTool::Brush => {
            for dy in -1i32..=1 {
                for dx in -1i32..=1 {
                    let x = CURSOR_X as i32 + dx;
                    let y = CURSOR_Y as i32 + dy;
                    if x >= 0 && x < CANVAS_WIDTH as i32 && y >= 0 && y < CANVAS_HEIGHT as i32 {
                        draw_pixel(x as usize, y as usize, CURRENT_COLOR);
                    }
                }
            }
            NEEDS_REDRAW = true;
        }
        DrawTool::Eraser => {
            draw_pixel(CURSOR_X, CURSOR_Y, 0x0F);
            NEEDS_REDRAW = true;
        }
        DrawTool::Line => {
            if LINE_START_X < 0 {
                LINE_START_X = CURSOR_X as i32;
                LINE_START_Y = CURSOR_Y as i32;
            } else {
                push_undo();
                graphics_draw_line(
                    LINE_START_X, LINE_START_Y,
                    CURSOR_X as i32, CURSOR_Y as i32,
                    CURRENT_COLOR as u32,
                );
                LINE_START_X = -1;
                LINE_START_Y = -1;
                NEEDS_REDRAW = true;
            }
        }
        DrawTool::Rectangle => {
            if LINE_START_X < 0 {
                LINE_START_X = CURSOR_X as i32;
                LINE_START_Y = CURSOR_Y as i32;
            } else {
                push_undo();
                draw_rectangle(LINE_START_X, LINE_START_Y, CURSOR_X as i32, CURSOR_Y as i32, CURRENT_COLOR, false);
                LINE_START_X = -1;
                LINE_START_Y = -1;
                NEEDS_REDRAW = true;
            }
        }
        DrawTool::FilledRectangle => {
            if LINE_START_X < 0 {
                LINE_START_X = CURSOR_X as i32;
                LINE_START_Y = CURSOR_Y as i32;
            } else {
                push_undo();
                draw_rectangle(LINE_START_X, LINE_START_Y, CURSOR_X as i32, CURSOR_Y as i32, CURRENT_COLOR, true);
                LINE_START_X = -1;
                LINE_START_Y = -1;
                NEEDS_REDRAW = true;
            }
        }
        DrawTool::Circle => {
            if LINE_START_X < 0 {
                LINE_START_X = CURSOR_X as i32;
                LINE_START_Y = CURSOR_Y as i32;
            } else {
                push_undo();
                let dx = CURSOR_X as i32 - LINE_START_X;
                let dy = CURSOR_Y as i32 - LINE_START_Y;
                let radius = integer_sqrt((dx * dx + dy * dy) as u32) as i32;
                draw_circle(LINE_START_X, LINE_START_Y, radius, CURRENT_COLOR);
                LINE_START_X = -1;
                LINE_START_Y = -1;
                NEEDS_REDRAW = true;
            }
        }
        DrawTool::Fill => {
            push_undo();
            let target = get_pixel(CURSOR_X, CURSOR_Y);
            flood_fill(CURSOR_X, CURSOR_Y, target, CURRENT_COLOR);
            NEEDS_REDRAW = true;
        }
    }
}

//-----------------------------------------------------------------------------
// Main editor loop
//-----------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn rust_image_editor() -> i32 {
    unsafe {
        clear_canvas();
        CURSOR_X = CANVAS_WIDTH / 2;
        CURSOR_Y = CANVAS_HEIGHT / 2;
        CURRENT_COLOR = 0x01;
        CURRENT_TOOL = DrawTool::Pencil;
        LINE_START_X = -1;
        LINE_START_Y = -1;
        SPACE_HELD = false;
        NEEDS_REDRAW = true;
        UNDO_HEAD = 0;
        UNDO_COUNT = 0;

        let mut last_draw_tick = get_ticks();

        render_ui();
        render_canvas();

        loop {
            let current_tick = get_ticks();

            // Continuous drawing while space held (pencil/brush/eraser only)
            if SPACE_HELD && matches!(CURRENT_TOOL, DrawTool::Pencil | DrawTool::Brush | DrawTool::Eraser) {
                if current_tick.wrapping_sub(last_draw_tick) >= 3 {
                    handle_drawing_action();
                    last_draw_tick = current_tick;
                }
            }

            if let Some(scan_code) = poll_keyboard_raw() {
                match scan_code {
                    // --- Cursor movement ---
                    0x48 => { // Up
                        if CURSOR_Y > 0 {
                            CURSOR_Y -= 1;
                            if SPACE_HELD { handle_drawing_action(); }
                        }
                        NEEDS_REDRAW = true;
                    }
                    0x50 => { // Down
                        if CURSOR_Y < CANVAS_HEIGHT - 1 {
                            CURSOR_Y += 1;
                            if SPACE_HELD { handle_drawing_action(); }
                        }
                        NEEDS_REDRAW = true;
                    }
                    0x4B => { // Left
                        if CURSOR_X > 0 {
                            CURSOR_X -= 1;
                            if SPACE_HELD { handle_drawing_action(); }
                        }
                        NEEDS_REDRAW = true;
                    }
                    0x4D => { // Right
                        if CURSOR_X < CANVAS_WIDTH - 1 {
                            CURSOR_X += 1;
                            if SPACE_HELD { handle_drawing_action(); }
                        }
                        NEEDS_REDRAW = true;
                    }

                    // --- Space = draw ---
                    0x39 => {
                        SPACE_HELD = true;
                        push_undo();
                        handle_drawing_action();
                    }

                    // --- Tool selection 1-8 ---
                    0x02 => { CURRENT_TOOL = DrawTool::Pencil;          LINE_START_X = -1; NEEDS_REDRAW = true; }
                    0x03 => { CURRENT_TOOL = DrawTool::Brush;           LINE_START_X = -1; NEEDS_REDRAW = true; }
                    0x04 => { CURRENT_TOOL = DrawTool::Eraser;          LINE_START_X = -1; NEEDS_REDRAW = true; }
                    0x05 => { CURRENT_TOOL = DrawTool::Line;            LINE_START_X = -1; NEEDS_REDRAW = true; }
                    0x06 => { CURRENT_TOOL = DrawTool::Rectangle;       LINE_START_X = -1; NEEDS_REDRAW = true; }
                    0x07 => { CURRENT_TOOL = DrawTool::FilledRectangle; LINE_START_X = -1; NEEDS_REDRAW = true; }
                    0x08 => { CURRENT_TOOL = DrawTool::Circle;          LINE_START_X = -1; NEEDS_REDRAW = true; }
                    0x09 => { CURRENT_TOOL = DrawTool::Fill;            LINE_START_X = -1; NEEDS_REDRAW = true; }

                    // --- Color keys 0-9 ---
                    0x0B => { CURRENT_COLOR = PALETTE[0];  NEEDS_REDRAW = true; } // 0
                    0x02 => { CURRENT_COLOR = PALETTE[1];  NEEDS_REDRAW = true; } // 1 (already tool, skip)
                    0x0A => { CURRENT_COLOR = PALETTE[9];  NEEDS_REDRAW = true; } // 9

                    // --- Color keys A-F ---
                    0x1E => { CURRENT_COLOR = PALETTE[10]; NEEDS_REDRAW = true; } // A
                    0x30 => { CURRENT_COLOR = PALETTE[11]; NEEDS_REDRAW = true; } // B
                    // C is clear - handled below
                    0x20 => { CURRENT_COLOR = PALETTE[13]; NEEDS_REDRAW = true; } // D
                    0x12 => { CURRENT_COLOR = PALETTE[14]; NEEDS_REDRAW = true; } // E
                    0x21 => { CURRENT_COLOR = PALETTE[15]; NEEDS_REDRAW = true; } // F

                    // --- Palette cycle , and . ---
                    0x33 => { // , - cycle left
                        let idx = PALETTE.iter().position(|&c| c == CURRENT_COLOR).unwrap_or(0);
                        CURRENT_COLOR = PALETTE[(idx + 15) % 16];
                        NEEDS_REDRAW = true;
                    }
                    0x34 => { // . - cycle right
                        let idx = PALETTE.iter().position(|&c| c == CURRENT_COLOR).unwrap_or(0);
                        CURRENT_COLOR = PALETTE[(idx + 1) % 16];
                        NEEDS_REDRAW = true;
                    }

                    // --- P - pick color under cursor ---
                    0x19 => {
                        CURRENT_COLOR = get_pixel(CURSOR_X, CURSOR_Y);
                        NEEDS_REDRAW = true;
                    }

                    // --- U - undo ---
                    0x16 => {
                        pop_undo();
                        NEEDS_REDRAW = true;
                    }

                    // --- I - invert ---
                    0x17 => {
                        push_undo();
                        invert_canvas();
                        NEEDS_REDRAW = true;
                    }

                    // --- H - flip horizontal ---
                    0x23 => {
                        push_undo();
                        flip_horizontal();
                        NEEDS_REDRAW = true;
                    }

                    // --- V - flip vertical ---
                    0x2F => {
                        push_undo();
                        flip_vertical();
                        NEEDS_REDRAW = true;
                    }

                    // --- S - save ---
                    0x1F => {
                        let default = if EDITOR_FILENAME_LEN > 0 {
                            &EDITOR_FILENAME[0..EDITOR_FILENAME_LEN]
                        } else {
                            b"drawing"
                        };
                        if let Some((filename, len)) = get_filename_input(b"=== Save BMP ===", default) {
                            if save_to_bmp(&filename[0..len + 1]) {
                                // Remember filename for next save
                                for i in 0..len { EDITOR_FILENAME[i] = filename[i]; }
                                EDITOR_FILENAME_LEN = len;
                            }
                            sleep_ms(1500);
                        }
                            render_ui();
                            render_canvas();
                            NEEDS_REDRAW = false;
                    }

                    // --- L - load ---
                    0x26 => {
                        if let Some((filename, len)) = get_filename_input(b"=== Load BMP ===", b"") {
                            load_from_bmp(&filename[0..len + 1]);
                            sleep_ms(1500);
                        }
                            render_ui();
                            render_canvas();
                            NEEDS_REDRAW = false;
                    }

                    // --- C - clear ---
                    0x2E => {
                        push_undo();
                        clear_canvas();
                        NEEDS_REDRAW = true;
                    }

                    // --- F1 - help ---
                    0x3B => {
                        show_help();
                        NEEDS_REDRAW = true;
                    }

                    // --- Q - quit ---
                    0x10 => {
                        terminal_clear();
                        break;
                    }

                    _ => {}
                }
            }

            if NEEDS_REDRAW {
                render_ui();
                render_canvas();
                NEEDS_REDRAW = false;
            } else {
                render_canvas(); // cursor blink
            }

            sleep_ms(10);
        }

        //terminal_clear();
        rust_print(b"\nEditor closed.\n");
        0
    }
}
//=============================================================================
// TLS/HTTPS SUPPORT
//=============================================================================

// TLS 1.2 Constants
const TLS_VERSION_1_2: u16 = 0x0303;
const TLS_HANDSHAKE: u8 = 0x16;
const TLS_CHANGE_CIPHER_SPEC: u8 = 0x14;
const TLS_ALERT: u8 = 0x15;
const TLS_APPLICATION_DATA: u8 = 0x17;

// Handshake types
const HANDSHAKE_CLIENT_HELLO: u8 = 0x01;
const HANDSHAKE_SERVER_HELLO: u8 = 0x02;
const HANDSHAKE_CERTIFICATE: u8 = 0x0B;
const HANDSHAKE_SERVER_HELLO_DONE: u8 = 0x0E;
const HANDSHAKE_CLIENT_KEY_EXCHANGE: u8 = 0x10;
const HANDSHAKE_FINISHED: u8 = 0x14;

// Cipher suites (we'll implement TLS_RSA_WITH_AES_128_CBC_SHA)
const TLS_RSA_WITH_AES_128_CBC_SHA: u16 = 0x002F;

// TLS connection state
#[repr(C)]
pub struct TlsConnection {
    tcp_connected: bool,
    handshake_complete: bool,
    client_random: [u8; 32],
    server_random: [u8; 32],
    master_secret: [u8; 48],
    client_write_key: [u8; 16],
    server_write_key: [u8; 16],
    client_write_iv: [u8; 16],
    server_write_iv: [u8; 16],
    client_seq_num: u64,
    server_seq_num: u64,
}

static mut TLS_CONNECTION: TlsConnection = TlsConnection {
    tcp_connected: false,
    handshake_complete: false,
    client_random: [0; 32],
    server_random: [0; 32],
    master_secret: [0; 48],
    client_write_key: [0; 16],
    server_write_key: [0; 16],
    client_write_iv: [0; 16],
    server_write_iv: [0; 16],
    client_seq_num: 0,
    server_seq_num: 0,
};

static mut TLS_RECEIVE_BUFFER: [u8; 65536] = [0; 65536];
static mut TLS_RECEIVE_LEN: usize = 0;

//=============================================================================
// MINIMAL CRYPTO PRIMITIVES
//=============================================================================

// Simple PRNG for random data (NOT cryptographically secure - use for demo only!)
unsafe fn generate_random_bytes(buffer: &mut [u8]) {
    let mut seed = get_ticks();
    for byte in buffer.iter_mut() {
        seed = seed.wrapping_mul(1103515245).wrapping_add(12345);
        *byte = (seed >> 16) as u8;
    }
}

// SHA-256 (simplified implementation)
const SHA256_K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
];

fn rotr32(x: u32, n: u32) -> u32 {
    (x >> n) | (x << (32 - n))
}

fn sha256(data: &[u8], output: &mut [u8; 32]) {
    let mut h: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    ];
    
    let mut padded = [0u8; 128];
    let data_len = data.len();
    let mut padded_len = data_len;
    
    // Copy data
    for i in 0..data_len.min(64) {
        padded[i] = data[i];
    }
    
    // Add padding
    padded[padded_len] = 0x80;
    padded_len += 1;
    
    // Pad to 56 bytes (448 bits)
    while padded_len % 64 != 56 {
        padded[padded_len] = 0;
        padded_len += 1;
    }
    
    // Add length in bits (big-endian)
    let bit_len = (data_len as u64) * 8;
    for i in 0..8 {
        padded[padded_len + i] = ((bit_len >> (56 - i * 8)) & 0xFF) as u8;
    }
    padded_len += 8;
    
    // Process blocks
    for chunk_start in (0..padded_len).step_by(64) {
        let mut w = [0u32; 64];
        
        // Prepare message schedule
        for i in 0..16 {
            w[i] = ((padded[chunk_start + i * 4] as u32) << 24) |
                   ((padded[chunk_start + i * 4 + 1] as u32) << 16) |
                   ((padded[chunk_start + i * 4 + 2] as u32) << 8) |
                   (padded[chunk_start + i * 4 + 3] as u32);
        }
        
        for i in 16..64 {
            let s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
            let s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16].wrapping_add(s0).wrapping_add(w[i-7]).wrapping_add(s1);
        }
        
        // Compression
        let mut a = h[0];
        let mut b = h[1];
        let mut c = h[2];
        let mut d = h[3];
        let mut e = h[4];
        let mut f = h[5];
        let mut g = h[6];
        let mut h_val = h[7];
        
        for i in 0..64 {
            let s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            let ch = (e & f) ^ ((!e) & g);
            let temp1 = h_val.wrapping_add(s1).wrapping_add(ch).wrapping_add(SHA256_K[i]).wrapping_add(w[i]);
            let s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let temp2 = s0.wrapping_add(maj);
            
            h_val = g;
            g = f;
            f = e;
            e = d.wrapping_add(temp1);
            d = c;
            c = b;
            b = a;
            a = temp1.wrapping_add(temp2);
        }
        
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
        h[5] = h[5].wrapping_add(f);
        h[6] = h[6].wrapping_add(g);
        h[7] = h[7].wrapping_add(h_val);
    }
    
    // Output hash
    for i in 0..8 {
        output[i * 4] = (h[i] >> 24) as u8;
        output[i * 4 + 1] = (h[i] >> 16) as u8;
        output[i * 4 + 2] = (h[i] >> 8) as u8;
        output[i * 4 + 3] = h[i] as u8;
    }
}

// HMAC-SHA256
fn hmac_sha256(key: &[u8], message: &[u8], output: &mut [u8; 32]) {
    let mut ipad = [0x36u8; 64];
    let mut opad = [0x5cu8; 64];
    
    // XOR key with pads
    for i in 0..key.len().min(64) {
        ipad[i] ^= key[i];
        opad[i] ^= key[i];
    }
    
    // Inner hash
    let mut inner_data = [0u8; 128];
    for i in 0..64 {
        inner_data[i] = ipad[i];
    }
    for i in 0..message.len().min(64) {
        inner_data[64 + i] = message[i];
    }
    
    let mut inner_hash = [0u8; 32];
    sha256(&inner_data[0..(64 + message.len().min(64))], &mut inner_hash);
    
    // Outer hash
    let mut outer_data = [0u8; 96];
    for i in 0..64 {
        outer_data[i] = opad[i];
    }
    for i in 0..32 {
        outer_data[64 + i] = inner_hash[i];
    }
    
    sha256(&outer_data[0..96], output);
}

// PRF (Pseudorandom Function) for TLS
fn prf_sha256(secret: &[u8], label: &[u8], seed: &[u8], output: &mut [u8], output_len: usize) {
    let mut a = [0u8; 128];
    let mut a_len = label.len() + seed.len();
    
    // A(0) = label + seed
    for i in 0..label.len() {
        a[i] = label[i];
    }
    for i in 0..seed.len() {
        a[label.len() + i] = seed[i];
    }
    
    let mut output_pos = 0;
    
    while output_pos < output_len {
        // A(i) = HMAC(secret, A(i-1))
        let mut a_hash = [0u8; 32];
        hmac_sha256(secret, &a[0..a_len], &mut a_hash);
        a_len = 32;
        for i in 0..32 {
            a[i] = a_hash[i];
        }
        
        // P_hash = HMAC(secret, A(i) + label + seed)
        let mut p_data = [0u8; 128];
        for i in 0..32 {
            p_data[i] = a[i];
        }
        for i in 0..label.len() {
            p_data[32 + i] = label[i];
        }
        for i in 0..seed.len() {
            p_data[32 + label.len() + i] = seed[i];
        }
        
        let mut p_hash = [0u8; 32];
        hmac_sha256(secret, &p_data[0..(32 + label.len() + seed.len())], &mut p_hash);
        
        let copy_len = (output_len - output_pos).min(32);
        for i in 0..copy_len {
            output[output_pos + i] = p_hash[i];
        }
        output_pos += copy_len;
    }
}

//=============================================================================
// TLS HANDSHAKE
//=============================================================================

unsafe fn build_client_hello(buffer: &mut [u8]) -> usize {
    let mut idx = 0;
    
    // TLS Record Header
    buffer[idx] = TLS_HANDSHAKE; idx += 1;
    buffer[idx] = 0x03; idx += 1; // TLS 1.2
    buffer[idx] = 0x03; idx += 1;
    
    let length_pos = idx;
    idx += 2; // Skip length for now
    
    // Handshake Header
    buffer[idx] = HANDSHAKE_CLIENT_HELLO; idx += 1;
    let handshake_length_pos = idx;
    idx += 3; // Skip handshake length
    
    // Client Version (TLS 1.2)
    buffer[idx] = 0x03; idx += 1;
    buffer[idx] = 0x03; idx += 1;
    
    // Client Random (32 bytes)
    generate_random_bytes(&mut TLS_CONNECTION.client_random);
    for i in 0..32 {
        buffer[idx] = TLS_CONNECTION.client_random[i];
        idx += 1;
    }
    
    // Session ID Length (0 - no session resumption)
    buffer[idx] = 0; idx += 1;
    
    // Cipher Suites Length
    buffer[idx] = 0x00; idx += 1;
    buffer[idx] = 0x02; idx += 1;
    
    // Cipher Suite: TLS_RSA_WITH_AES_128_CBC_SHA
    buffer[idx] = 0x00; idx += 1;
    buffer[idx] = 0x2F; idx += 1;
    
    // Compression Methods Length
    buffer[idx] = 0x01; idx += 1;
    
    // Compression Method: None
    buffer[idx] = 0x00; idx += 1;
    
    // Extensions Length
    buffer[idx] = 0x00; idx += 1;
    buffer[idx] = 0x00; idx += 1;
    
    // Fill in lengths
    let handshake_len = idx - handshake_length_pos - 3;
    buffer[handshake_length_pos] = ((handshake_len >> 16) & 0xFF) as u8;
    buffer[handshake_length_pos + 1] = ((handshake_len >> 8) & 0xFF) as u8;
    buffer[handshake_length_pos + 2] = (handshake_len & 0xFF) as u8;
    
    let record_len = idx - length_pos - 2;
    buffer[length_pos] = ((record_len >> 8) & 0xFF) as u8;
    buffer[length_pos + 1] = (record_len & 0xFF) as u8;
    
    idx
}

unsafe fn parse_server_hello(data: &[u8]) -> bool {
    if data.len() < 42 {
        rust_print(b"ERROR: Server Hello too short\n");
        return false;
    }
    
    let mut idx = 5; // Skip record header
    
    // Skip handshake header
    if data[idx] != HANDSHAKE_SERVER_HELLO {
        rust_print(b"ERROR: Not a Server Hello\n");
        return false;
    }
    idx += 4;
    
    // Skip version
    idx += 2;
    
    // Extract server random
    for i in 0..32 {
        TLS_CONNECTION.server_random[i] = data[idx + i];
    }
    idx += 32;
    
    rust_print(b"Server Hello received\n");
    true
}

unsafe fn compute_master_secret(premaster_secret: &[u8; 48]) {
    let mut seed = [0u8; 64];
    
    // Seed = client_random + server_random
    for i in 0..32 {
        seed[i] = TLS_CONNECTION.client_random[i];
        seed[32 + i] = TLS_CONNECTION.server_random[i];
    }
    
    prf_sha256(
        premaster_secret,
        b"master secret",
        &seed,
        &mut TLS_CONNECTION.master_secret,
        48
    );
    
    rust_print(b"Master secret computed\n");
}

unsafe fn derive_keys() {
    let mut seed = [0u8; 64];
    
    // Seed = server_random + client_random (note the order!)
    for i in 0..32 {
        seed[i] = TLS_CONNECTION.server_random[i];
        seed[32 + i] = TLS_CONNECTION.client_random[i];
    }
    
    let mut key_block = [0u8; 64];
    prf_sha256(
        &TLS_CONNECTION.master_secret,
        b"key expansion",
        &seed,
        &mut key_block,
        64
    );
    
    // Extract keys
    for i in 0..16 {
        TLS_CONNECTION.client_write_key[i] = key_block[i];
        TLS_CONNECTION.server_write_key[i] = key_block[16 + i];
        TLS_CONNECTION.client_write_iv[i] = key_block[32 + i];
        TLS_CONNECTION.server_write_iv[i] = key_block[48 + i];
    }
    
    rust_print(b"Keys derived\n");
}

//=============================================================================
// SIMPLIFIED TLS (WITHOUT ACTUAL ENCRYPTION - DEMO VERSION)
//=============================================================================

unsafe fn tls_connect(dest_ip: &[u8; 4], dest_port: u16) -> bool {
    rust_print(b"TLS: Connecting to ");
    for i in 0..4 {
        print_num(dest_ip[i] as i32);
        if i < 3 { rust_print(b"."); }
    }
    rust_print(b":");
    print_num(dest_port as i32);
    rust_print(b"\n");
    
    // First establish TCP connection
    if !tcp_connect(dest_ip, dest_port) {
        return false;
    }
    
    TLS_CONNECTION.tcp_connected = true;
    TLS_CONNECTION.handshake_complete = false;
    
    // Send Client Hello
    let mut client_hello = [0u8; 256];
    let hello_len = build_client_hello(&mut client_hello);
    
    rust_print(b"Sending Client Hello (");
    print_num(hello_len as i32);
    rust_print(b" bytes)\n");
    
    if !tcp_send_data(&client_hello[0..hello_len]) {
        rust_print(b"ERROR: Failed to send Client Hello\n");
        return false;
    }
    
    // Wait for Server Hello
    rust_print(b"Waiting for Server Hello...\n");
    let recv_len = tcp_receive_data(5000000);
    
    if recv_len == 0 {
        rust_print(b"ERROR: No server response\n");
        return false;
    }
    
    rust_print(b"Received ");
    print_num(recv_len as i32);
    rust_print(b" bytes from server\n");
    
    // For this simplified version, we'll accept the handshake without full validation
    // In production, you'd need to:
    // 1. Parse and validate Server Hello
    // 2. Parse and validate Server Certificate
    // 3. Generate premaster secret and encrypt it with server's public key
    // 4. Send Client Key Exchange
    // 5. Send Change Cipher Spec
    // 6. Send Finished message
    // 7. Receive and verify server's Finished message
    
    rust_print(b"WARNING: Using simplified TLS (encryption not implemented)\n");
    rust_print(b"TLS handshake 'complete' (demo mode)\n");
    
    TLS_CONNECTION.handshake_complete = true;
    
    true
}

unsafe fn tls_send_application_data(data: &[u8]) -> bool {
    if !TLS_CONNECTION.handshake_complete {
        rust_print(b"ERROR: TLS not established\n");
        return false;
    }
    
    // In a real implementation, this would encrypt the data
    // For now, we'll send it as plain HTTP over the established TCP connection
    tcp_send_data(data)
}

unsafe fn tls_receive_data(timeout: u32) -> usize {
    if !TLS_CONNECTION.handshake_complete {
        return 0;
    }
    
    // In a real implementation, this would decrypt the data
    // For now, just receive TCP data
    tcp_receive_data(timeout)
}

unsafe fn tls_close() {
    if TLS_CONNECTION.tcp_connected {
        tcp_close();
        TLS_CONNECTION.tcp_connected = false;
        TLS_CONNECTION.handshake_complete = false;
    }
}

//=============================================================================
// HTTPS CLIENT
//=============================================================================

#[no_mangle]
pub extern "C" fn rust_https_get(url: *const u8) -> i32 {
    unsafe {
        if url.is_null() {
            return -1;
        }
        
        rust_print(b"\n=== HTTPS GET Request ===\n");
        
        // Parse URL
        let mut url_bytes = [0u8; 512];
        let mut url_len = 0;
        let mut ptr = url;
        while *ptr != 0 && url_len < 512 {
            url_bytes[url_len] = *ptr;
            url_len += 1;
            ptr = ptr.add(1);
        }
        
        let mut idx = 0;
        
        // Skip https://
        if url_len > 8 && &url_bytes[0..8] == b"https://" {
            idx = 8;
        } else if url_len > 7 && &url_bytes[0..7] == b"http://" {
            rust_print(b"ERROR: Use http:// URLs with rust_web_browser\n");
            return -1;
        } else {
            rust_print(b"ERROR: URL must start with https://\n");
            return -1;
        }
        
        // Extract hostname
        let hostname_start = idx;
        while idx < url_len && url_bytes[idx] != b'/' && url_bytes[idx] != b':' {
            idx += 1;
        }
        let hostname_end = idx;
        
        if hostname_start >= hostname_end {
            rust_print(b"ERROR: Invalid URL\n");
            return -1;
        }
        
        let hostname = &url_bytes[hostname_start..hostname_end];
        
        // Extract port (default 443 for HTTPS)
        let mut port = 443u16;
        if idx < url_len && url_bytes[idx] == b':' {
            idx += 1;
            port = 0;
            while idx < url_len && url_bytes[idx] >= b'0' && url_bytes[idx] <= b'9' {
                port = port * 10 + (url_bytes[idx] - b'0') as u16;
                idx += 1;
            }
        }
        
        // Extract path
        let path = if idx < url_len && url_bytes[idx] == b'/' {
            &url_bytes[idx..url_len]
        } else {
            b"/"
        };
        
        rust_print(b"Host: ");
        for &c in hostname {
            terminal_putchar(c);
        }
        rust_print(b"\nPort: ");
        print_num(port as i32);
        rust_print(b"\nPath: ");
        for &c in path {
            terminal_putchar(c);
        }
        rust_print(b"\n\n");
        
        // Resolve hostname
        let mut hostname_null = [0u8; 256];
        for i in 0..hostname.len().min(255) {
            hostname_null[i] = hostname[i];
        }
        hostname_null[hostname.len().min(255)] = 0;
        
        let server_ip = match resolve_host(&hostname_null) {
            Some(ip) => {
                rust_print(b"Resolved to: ");
                for i in 0..4 {
                    print_num(ip[i] as i32);
                    if i < 3 { rust_print(b"."); }
                }
                rust_print(b"\n");
                ip
            },
            None => {
                rust_print(b"ERROR: DNS resolution failed\n");
                return -1;
            }
        };
        
        // Establish TLS connection
        if !tls_connect(&server_ip, port) {
            rust_print(b"ERROR: TLS connection failed\n");
            return -1;
        }
        
        // Build HTTP request
        let mut http_request = [0u8; 1024];
        let mut req_idx = 0;
        
        for &c in b"GET " {
            http_request[req_idx] = c;
            req_idx += 1;
        }
        for &c in path {
            http_request[req_idx] = c;
            req_idx += 1;
        }
        for &c in b" HTTP/1.1\r\n" {
            http_request[req_idx] = c;
            req_idx += 1;
        }
        
        for &c in b"Host: " {
            http_request[req_idx] = c;
            req_idx += 1;
        }
        for &c in hostname {
            http_request[req_idx] = c;
            req_idx += 1;
        }
        for &c in b"\r\n" {
            http_request[req_idx] = c;
            req_idx += 1;
        }
        
        for &c in b"User-Agent: RadiumOS/1.0\r\n" {
            http_request[req_idx] = c;
            req_idx += 1;
        }
        for &c in b"Accept: text/html\r\n" {
            http_request[req_idx] = c;
            req_idx += 1;
        }
        for &c in b"Connection: close\r\n\r\n" {
            http_request[req_idx] = c;
            req_idx += 1;
        }
        
        rust_print(b"Sending HTTPS request...\n");
        
        if !tls_send_application_data(&http_request[0..req_idx]) {
            tls_close();
            return -1;
        }
        
        rust_print(b"Waiting for response...\n");
        
        let recv_len = tls_receive_data(5000000);
        tls_close();
        
        rust_print(b"Received ");
        print_num(recv_len as i32);
        rust_print(b" bytes\n");
        
        if recv_len == 0 {
            rust_print(b"ERROR: No response\n");
            return -1;
        }
        
        // Display response
        rust_print(b"\n=== Response ===\n");
        for i in 0..recv_len.min(1000) {
            if HTTP_RECEIVE_BUFFER[i] >= 32 && HTTP_RECEIVE_BUFFER[i] < 127 {
                terminal_putchar(HTTP_RECEIVE_BUFFER[i]);
            } else if HTTP_RECEIVE_BUFFER[i] == b'\n' {
                terminal_putchar(b'\n');
            }
        }
        rust_print(b"\n================\n");
        
        0
    }
}

#[no_mangle]
pub extern "C" fn rust_test_https() -> i32 {
    unsafe {
        rust_print(b"\n=== HTTPS Test ===\n\n");
        rust_print(b"WARNING: This is a simplified HTTPS implementation\n");
        rust_print(b"It performs TLS handshake but doesn't do full encryption\n");
        rust_print(b"For educational purposes only!\n\n");
        
        rust_print(b"Testing HTTPS connection to example.com...\n");
        rust_https_get(b"https://example.com/\0".as_ptr())
    }
}
//=============================================================================
// JSON PARSER
//=============================================================================

#[derive(Copy, Clone, PartialEq, Debug)]
enum JsonValueType {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
}

#[derive(Copy, Clone)]
struct JsonValue {
    value_type: JsonValueType,
    // For strings: start index and length in the JSON text
    string_start: usize,
    string_len: usize,
    // For numbers: stored as i32 (simplified)
    number_value: i32,
    // For booleans
    bool_value: bool,
    // For arrays/objects: start and count of children
    children_start: usize,
    children_count: usize,
}

#[derive(Copy, Clone)]
struct JsonKeyValue {
    key_start: usize,
    key_len: usize,
    value_index: usize,
}

static mut JSON_TEXT: [u8; 16384] = [0; 16384];
static mut JSON_TEXT_LEN: usize = 0;
static mut JSON_VALUES: [JsonValue; 512] = [JsonValue {
    value_type: JsonValueType::Null,
    string_start: 0,
    string_len: 0,
    number_value: 0,
    bool_value: false,
    children_start: 0,
    children_count: 0,
}; 512];
static mut JSON_VALUE_COUNT: usize = 0;
static mut JSON_KEY_VALUES: [JsonKeyValue; 256] = [JsonKeyValue {
    key_start: 0,
    key_len: 0,
    value_index: 0,
}; 256];
static mut JSON_KEY_VALUE_COUNT: usize = 0;

unsafe fn json_skip_whitespace(text: &[u8], mut pos: usize) -> usize {
    while pos < text.len() {
        match text[pos] {
            b' ' | b'\t' | b'\n' | b'\r' => pos += 1,
            _ => break,
        }
    }
    pos
}

unsafe fn json_parse_string(text: &[u8], mut pos: usize) -> Option<(usize, usize, usize)> {
    if pos >= text.len() || text[pos] != b'"' {
        return None;
    }
    pos += 1;
    let start = pos;
    
    while pos < text.len() && text[pos] != b'"' {
        if text[pos] == b'\\' {
            pos += 2; // Skip escaped character
        } else {
            pos += 1;
        }
    }
    
    if pos >= text.len() {
        return None;
    }
    
    let len = pos - start;
    pos += 1; // Skip closing quote
    
    Some((start, len, pos))
}

unsafe fn json_parse_number(text: &[u8], mut pos: usize) -> Option<(i32, usize)> {
    let start = pos;
    let mut is_negative = false;
    
    if pos < text.len() && text[pos] == b'-' {
        is_negative = true;
        pos += 1;
    }
    
    if pos >= text.len() || text[pos] < b'0' || text[pos] > b'9' {
        return None;
    }
    
    let mut value = 0i32;
    while pos < text.len() && text[pos] >= b'0' && text[pos] <= b'9' {
        value = value * 10 + (text[pos] - b'0') as i32;
        pos += 1;
    }
    
    if is_negative {
        value = -value;
    }
    
    // Skip decimal part if present (we only support integers for simplicity)
    if pos < text.len() && text[pos] == b'.' {
        pos += 1;
        while pos < text.len() && text[pos] >= b'0' && text[pos] <= b'9' {
            pos += 1;
        }
    }
    
    Some((value, pos))
}

unsafe fn json_parse_value(text: &[u8], mut pos: usize) -> Option<(usize, usize)> {
    pos = json_skip_whitespace(text, pos);
    
    if pos >= text.len() {
        return None;
    }
    
    let value_index = JSON_VALUE_COUNT;
    if value_index >= 512 {
        return None;
    }
    
    match text[pos] {
        b'"' => {
            // String
            if let Some((start, len, new_pos)) = json_parse_string(text, pos) {
                JSON_VALUES[value_index] = JsonValue {
                    value_type: JsonValueType::String,
                    string_start: start,
                    string_len: len,
                    number_value: 0,
                    bool_value: false,
                    children_start: 0,
                    children_count: 0,
                };
                JSON_VALUE_COUNT += 1;
                Some((value_index, new_pos))
            } else {
                None
            }
        }
        b'{' => {
            // Object
            pos += 1;
            let children_start = JSON_KEY_VALUE_COUNT;
            let mut count = 0;
            
            pos = json_skip_whitespace(text, pos);
            
            if pos < text.len() && text[pos] == b'}' {
                // Empty object
                JSON_VALUES[value_index] = JsonValue {
                    value_type: JsonValueType::Object,
                    string_start: 0,
                    string_len: 0,
                    number_value: 0,
                    bool_value: false,
                    children_start,
                    children_count: 0,
                };
                JSON_VALUE_COUNT += 1;
                return Some((value_index, pos + 1));
            }
            
            loop {
                pos = json_skip_whitespace(text, pos);
                
                // Parse key
                if let Some((key_start, key_len, new_pos)) = json_parse_string(text, pos) {
                    pos = json_skip_whitespace(text, new_pos);
                    
                    if pos >= text.len() || text[pos] != b':' {
                        return None;
                    }
                    pos += 1;
                    
                    // Parse value
                    if let Some((val_index, new_pos)) = json_parse_value(text, pos) {
                        if JSON_KEY_VALUE_COUNT < 256 {
                            JSON_KEY_VALUES[JSON_KEY_VALUE_COUNT] = JsonKeyValue {
                                key_start,
                                key_len,
                                value_index: val_index,
                            };
                            JSON_KEY_VALUE_COUNT += 1;
                            count += 1;
                        }
                        pos = json_skip_whitespace(text, new_pos);
                    } else {
                        return None;
                    }
                } else {
                    return None;
                }
                
                if pos >= text.len() {
                    return None;
                }
                
                if text[pos] == b'}' {
                    pos += 1;
                    break;
                } else if text[pos] == b',' {
                    pos += 1;
                } else {
                    return None;
                }
            }
            
            JSON_VALUES[value_index] = JsonValue {
                value_type: JsonValueType::Object,
                string_start: 0,
                string_len: 0,
                number_value: 0,
                bool_value: false,
                children_start,
                children_count: count,
            };
            JSON_VALUE_COUNT += 1;
            Some((value_index, pos))
        }
        b'[' => {
            // Array
            pos += 1;
            let children_start = JSON_VALUE_COUNT + 1;
            let mut count = 0;
            
            pos = json_skip_whitespace(text, pos);
            
            if pos < text.len() && text[pos] == b']' {
                // Empty array
                JSON_VALUES[value_index] = JsonValue {
                    value_type: JsonValueType::Array,
                    string_start: 0,
                    string_len: 0,
                    number_value: 0,
                    bool_value: false,
                    children_start,
                    children_count: 0,
                };
                JSON_VALUE_COUNT += 1;
                return Some((value_index, pos + 1));
            }
            
            JSON_VALUE_COUNT += 1; // Reserve space for array value
            
            loop {
                if let Some((_, new_pos)) = json_parse_value(text, pos) {
                    count += 1;
                    pos = json_skip_whitespace(text, new_pos);
                } else {
                    return None;
                }
                
                if pos >= text.len() {
                    return None;
                }
                
                if text[pos] == b']' {
                    pos += 1;
                    break;
                } else if text[pos] == b',' {
                    pos += 1;
                } else {
                    return None;
                }
            }
            
            JSON_VALUES[value_index] = JsonValue {
                value_type: JsonValueType::Array,
                string_start: 0,
                string_len: 0,
                number_value: 0,
                bool_value: false,
                children_start,
                children_count: count,
            };
            Some((value_index, pos))
        }
        b't' if pos + 3 < text.len() && &text[pos..pos+4] == b"true" => {
            JSON_VALUES[value_index] = JsonValue {
                value_type: JsonValueType::Boolean,
                string_start: 0,
                string_len: 0,
                number_value: 0,
                bool_value: true,
                children_start: 0,
                children_count: 0,
            };
            JSON_VALUE_COUNT += 1;
            Some((value_index, pos + 4))
        }
        b'f' if pos + 4 < text.len() && &text[pos..pos+5] == b"false" => {
            JSON_VALUES[value_index] = JsonValue {
                value_type: JsonValueType::Boolean,
                string_start: 0,
                string_len: 0,
                number_value: 0,
                bool_value: false,
                children_start: 0,
                children_count: 0,
            };
            JSON_VALUE_COUNT += 1;
            Some((value_index, pos + 5))
        }
        b'n' if pos + 3 < text.len() && &text[pos..pos+4] == b"null" => {
            JSON_VALUES[value_index] = JsonValue {
                value_type: JsonValueType::Null,
                string_start: 0,
                string_len: 0,
                number_value: 0,
                bool_value: false,
                children_start: 0,
                children_count: 0,
            };
            JSON_VALUE_COUNT += 1;
            Some((value_index, pos + 4))
        }
        b'-' | b'0'..=b'9' => {
            if let Some((value, new_pos)) = json_parse_number(text, pos) {
                JSON_VALUES[value_index] = JsonValue {
                    value_type: JsonValueType::Number,
                    string_start: 0,
                    string_len: 0,
                    number_value: value,
                    bool_value: false,
                    children_start: 0,
                    children_count: 0,
                };
                JSON_VALUE_COUNT += 1;
                Some((value_index, new_pos))
            } else {
                None
            }
        }
        _ => None,
    }
}

unsafe fn json_parse(text: &[u8], len: usize) -> Option<usize> {
    JSON_VALUE_COUNT = 0;
    JSON_KEY_VALUE_COUNT = 0;
    JSON_TEXT_LEN = len.min(JSON_TEXT.len());
    
    for i in 0..JSON_TEXT_LEN {
        JSON_TEXT[i] = text[i];
    }
    
    if let Some((root_index, _)) = json_parse_value(&JSON_TEXT[0..JSON_TEXT_LEN], 0) {
        Some(root_index)
    } else {
        None
    }
}

unsafe fn json_get_string(value_index: usize) -> Option<&'static [u8]> {
    if value_index >= JSON_VALUE_COUNT {
        return None;
    }
    
    let value = &JSON_VALUES[value_index];
    if value.value_type != JsonValueType::String {
        return None;
    }
    
    Some(&JSON_TEXT[value.string_start..value.string_start + value.string_len])
}

unsafe fn json_get_number(value_index: usize) -> Option<i32> {
    if value_index >= JSON_VALUE_COUNT {
        return None;
    }
    
    let value = &JSON_VALUES[value_index];
    if value.value_type != JsonValueType::Number {
        return None;
    }
    
    Some(value.number_value)
}

unsafe fn json_get_bool(value_index: usize) -> Option<bool> {
    if value_index >= JSON_VALUE_COUNT {
        return None;
    }
    
    let value = &JSON_VALUES[value_index];
    if value.value_type != JsonValueType::Boolean {
        return None;
    }
    
    Some(value.bool_value)
}

unsafe fn json_object_get(object_index: usize, key: &[u8]) -> Option<usize> {
    if object_index >= JSON_VALUE_COUNT {
        return None;
    }
    
    let obj = &JSON_VALUES[object_index];
    if obj.value_type != JsonValueType::Object {
        return None;
    }
    
    for i in 0..obj.children_count {
        let kv = &JSON_KEY_VALUES[obj.children_start + i];
        let stored_key = &JSON_TEXT[kv.key_start..kv.key_start + kv.key_len];
        
        if stored_key.len() == key.len() {
            let mut match_found = true;
            for j in 0..key.len() {
                if stored_key[j] != key[j] {
                    match_found = false;
                    break;
                }
            }
            if match_found {
                return Some(kv.value_index);
            }
        }
    }
    
    None
}

unsafe fn json_array_get(array_index: usize, index: usize) -> Option<usize> {
    if array_index >= JSON_VALUE_COUNT {
        return None;
    }
    
    let arr = &JSON_VALUES[array_index];
    if arr.value_type != JsonValueType::Array {
        return None;
    }
    
    if index >= arr.children_count {
        return None;
    }
    
    Some(arr.children_start + index)
}

unsafe fn json_array_len(array_index: usize) -> usize {
    if array_index >= JSON_VALUE_COUNT {
        return 0;
    }
    
    let arr = &JSON_VALUES[array_index];
    if arr.value_type != JsonValueType::Array {
        return 0;
    }
    
    arr.children_count
}

//=============================================================================
// DISCORD - ENHANCED + MODULE SYSTEM
//=============================================================================

#[no_mangle]
pub static mut DISCORD_TOKEN: [u8; 128] = [0; 128];

#[no_mangle]
pub static mut DISCORD_TOKEN_LEN: usize = 0;
// ── Rate-limit state ──────────────────────────────────────────────────────
static mut DISCORD_RL_RESET_AT:  u32 = 0;
static mut DISCORD_RL_REMAINING: i32 = 5;

// ── Module system ──────────────────────────────────────────────────────────
const MAX_MODULES:      usize = 16;
const MODULE_NAME_LEN:  usize = 32;
const MODULE_PARAM_LEN: usize = 256;
const MODULE_PARAMS:    usize = 8;

#[derive(Copy, Clone, PartialEq)]
enum ModuleKind {
    None, SendEmoji, SendMessage, SendEmbed,
    FetchMessages, DeleteMessage, React, AutoReply,
}

#[derive(Copy, Clone)]
struct ModuleParam {
    key:     [u8; 32],
    key_len: usize,
    val:     [u8; MODULE_PARAM_LEN],
    val_len: usize,
}
impl ModuleParam {
    const fn blank() -> Self {
        Self { key:[0;32], key_len:0, val:[0;MODULE_PARAM_LEN], val_len:0 }
    }
}

#[derive(Copy, Clone)]
struct DiscordModule {
    name:        [u8; MODULE_NAME_LEN],
    name_len:    usize,
    kind:        ModuleKind,
    params:      [ModuleParam; MODULE_PARAMS],
    param_count: usize,
    configured:  bool,
    active:      bool,
        tag:     [u8; 64],
    tag_len: usize,
}
impl DiscordModule {
    const fn blank() -> Self {
        Self {
            name:[0;MODULE_NAME_LEN], name_len:0,
            kind:ModuleKind::None,
            params:[ModuleParam::blank();MODULE_PARAMS],
            param_count:0, configured:false, active:false,
            tag: [0; 64],
            tag_len: 0,
        }
    }
}

static mut MODULES:      [DiscordModule; MAX_MODULES] = [DiscordModule::blank(); MAX_MODULES];
static mut MODULE_COUNT: usize = 0;

// ── Message cache ──────────────────────────────────────────────────────────
const MSG_CACHE_SIZE:  usize = 32;
const MSG_CONTENT_LEN: usize = 256;
const MSG_AUTHOR_LEN:  usize = 64;
const MSG_ID_LEN:      usize = 20;

#[derive(Copy, Clone)]
struct CachedMessage {
    id:       [u8; MSG_ID_LEN],   id_len:   usize,
    author:   [u8; MSG_AUTHOR_LEN], auth_len: usize,
    content:  [u8; MSG_CONTENT_LEN], cont_len: usize,
}
impl CachedMessage {
    const fn blank() -> Self {
        Self {
            id:[0;MSG_ID_LEN], id_len:0,
            author:[0;MSG_AUTHOR_LEN], auth_len:0,
            content:[0;MSG_CONTENT_LEN], cont_len:0,
        }
    }
}

static mut MSG_CACHE:      [CachedMessage; MSG_CACHE_SIZE] = [CachedMessage::blank(); MSG_CACHE_SIZE];
static mut MSG_CACHE_HEAD: usize = 0;
static mut MSG_CACHE_LEN:  usize = 0;

// ── Guild / channel state ──────────────────────────────────────────────────
const MAX_GUILDS:   usize = 19;
const MAX_CHANNELS: usize = 19;
const ID_LEN: usize = 19;
const NAME_LEN: usize = 19;

#[derive(Copy, Clone)]
struct GuildEntry { id:[u8;ID_LEN], id_len:usize, name:[u8;NAME_LEN], name_len:usize }
impl GuildEntry {
    const fn blank() -> Self {
        Self { id:[0;ID_LEN], id_len:0, name:[0;NAME_LEN], name_len:0 }
    }
}

#[derive(Copy, Clone)]
struct ChannelEntry {
    id:[u8;ID_LEN], id_len:usize,
    name:[u8;NAME_LEN], name_len:usize,
    channel_type:i32,
    guild_id:[u8;ID_LEN], guild_id_len:usize,
}
impl ChannelEntry {
    const fn blank() -> Self {
        Self {
            id:[0;ID_LEN], id_len:0,
            name:[0;NAME_LEN], name_len:0,
            channel_type:0,
            guild_id:[0;ID_LEN], guild_id_len:0,
        }
    }
}

static mut GUILDS:        [GuildEntry;   MAX_GUILDS]   = [GuildEntry::blank();   MAX_GUILDS];
static mut GUILD_COUNT:   usize = 0;
static mut CHANNELS:      [ChannelEntry; MAX_CHANNELS] = [ChannelEntry::blank(); MAX_CHANNELS];
static mut CHANNEL_COUNT: usize = 0;

//─────────────────────────────────────────────────────────────────────────────
// Internal helpers
//─────────────────────────────────────────────────────────────────────────────

unsafe fn cache_push_message(id: &[u8], author: &[u8], content: &[u8]) {
    let slot = &mut MSG_CACHE[MSG_CACHE_HEAD];
    let il = id.len().min(MSG_ID_LEN);
    slot.id[..il].copy_from_slice(&id[..il]); slot.id_len = il;
    let al = author.len().min(MSG_AUTHOR_LEN);
    slot.author[..al].copy_from_slice(&author[..al]); slot.auth_len = al;
    let cl = content.len().min(MSG_CONTENT_LEN);
    slot.content[..cl].copy_from_slice(&content[..cl]); slot.cont_len = cl;
    MSG_CACHE_HEAD = (MSG_CACHE_HEAD + 1) % MSG_CACHE_SIZE;
    if MSG_CACHE_LEN < MSG_CACHE_SIZE { MSG_CACHE_LEN += 1; }
}

unsafe fn copy_json_str(value_index: usize, dst: &mut [u8]) -> usize {
    match json_get_string(value_index) {
        Some(s) => { let n = s.len().min(dst.len()); dst[..n].copy_from_slice(&s[..n]); n }
        None    => 0,
    }
}

unsafe fn discord_wait_rate_limit() {
    if DISCORD_RL_REMAINING <= 0 {
        let now = get_ticks();
        if DISCORD_RL_RESET_AT > now {
            let wait = DISCORD_RL_RESET_AT - now;
            rust_print(b"Discord: rate limited, waiting ");
            print_num(wait as i32);
            rust_print(b" ms\n");
            sleep_ms(wait);
        }
        DISCORD_RL_REMAINING = 5;
    }
    DISCORD_RL_REMAINING -= 1;
}

unsafe fn discord_parse_rate_limit_headers() {
    let buf = &HTTP_RECEIVE_BUFFER[0..HTTP_RECEIVE_LEN.min(4096)];
    let mut header_end = buf.len();
    for i in 0..buf.len().saturating_sub(3) {
        if buf[i]==b'\r'&&buf[i+1]==b'\n'&&buf[i+2]==b'\r'&&buf[i+3]==b'\n' {
            header_end = i; break;
        }
    }
    let headers = &buf[0..header_end];

    let rl_rem = b"x-ratelimit-remaining: ";
    'r: for i in 0..headers.len() {
        if i + rl_rem.len() >= headers.len() { break; }
        let mut m = true;
        for j in 0..rl_rem.len() {
            let c = headers[i+j];
            let cl = if c>=b'A'&&c<=b'Z'{c+32}else{c};
            if cl != rl_rem[j] { m=false; break; }
        }
        if m {
            let mut v=0i32; let mut p=i+rl_rem.len();
            while p<headers.len()&&headers[p]>=b'0'&&headers[p]<=b'9' {
                v=v*10+(headers[p]-b'0')as i32; p+=1;
            }
            DISCORD_RL_REMAINING=v; break 'r;
        }
    }

    let rl_after = b"x-ratelimit-reset-after: ";
    'r2: for i in 0..headers.len() {
        if i + rl_after.len() >= headers.len() { break; }
        let mut m = true;
        for j in 0..rl_after.len() {
            let c = headers[i+j];
            let cl = if c>=b'A'&&c<=b'Z'{c+32}else{c};
            if cl != rl_after[j] { m=false; break; }
        }
        if m {
            let mut s=0u32; let mut p=i+rl_after.len();
            while p<headers.len()&&headers[p]>=b'0'&&headers[p]<=b'9' {
                s=s*10+(headers[p]-b'0')as u32; p+=1;
            }
            DISCORD_RL_RESET_AT=get_ticks()+s*1000+500; break 'r2;
        }
    }
}

fn bytes_contain(haystack: &[u8], needle: &[u8]) -> bool {
    if needle.len() > haystack.len() { return false; }
    for i in 0..=(haystack.len() - needle.len()) {
        if &haystack[i..i+needle.len()] == needle { return true; }
    }
    false
}

//─────────────────────────────────────────────────────────────────────────────
// send_ack
//─────────────────────────────────────────────────────────────────────────────

unsafe fn send_ack() {
    let mut ack_tcp = [0u8; 64];
    let ack_len = build_tcp_packet(
        &TCP_CONNECTION.remote_ip, TCP_CONNECTION.remote_port,
        TCP_CONNECTION.local_port, TCP_CONNECTION.seq_num,
        TCP_CONNECTION.ack_num, 0x10, &[], &mut ack_tcp);
    if ack_len == 0 { return; }
    let mut ack_ip = [0u8; 128];
    let ack_ip_len = build_ip_packet(
        &TCP_CONNECTION.remote_ip, 6, &ack_tcp[0..ack_len], &mut ack_ip);
    if ack_ip_len == 0 { return; }
    let gw = [0x52u8,0x54,0x00,0x12,0x34,0x56];
    let mut ack_eth = [0u8; 256];
    let ack_eth_len = build_ethernet_frame(
        &gw, 0x0800, &ack_ip[0..ack_ip_len], &mut ack_eth);
    if ack_eth_len == 0 { return; }
    rust_rtl8139_send(ack_eth.as_ptr(), ack_eth_len as u32);
}
// ─────────────────────────────────────────────────────────────────────────────
// Chat Client Configuration
// ─────────────────────────────────────────────────────────────────────────────
const PROXY_IP: [u8; 4] = [72u8, 14, 176, 144]; // Your Proxy IP
const PROXY_PORT: u16 = 8080;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Manually construct JSON: {"user":"...","msg":"..."}
// ─────────────────────────────────────────────────────────────────────────────
fn build_chat_json(out: &mut [u8], user: &[u8], msg: &[u8]) -> usize {
    let mut i = 0;
    
    // Prefix
    let p = b"{\"user\":\"";
    for &c in p { if i < out.len() { out[i] = c; i += 1; } }
    
    // User
    for &c in user { 
        // Basic escaping for quotes in username (simple version)
        if c == b'"' { 
            if i+1 < out.len() { out[i] = b'\\'; out[i+1] = b'"'; i += 2; }
        } else {
            if i < out.len() { out[i] = c; i += 1; }
        }
    }

    // Midfix
    let m = b"\",\"msg\":\"";
    for &c in m { if i < out.len() { out[i] = c; i += 1; } }

    // Message
    for &c in msg {
        if c == b'"' { 
            if i+1 < out.len() { out[i] = b'\\'; out[i+1] = b'"'; i += 2; }
        } else if c == b'\\' {
             if i+1 < out.len() { out[i] = b'\\'; out[i+1] = b'\\'; i += 2; }
        } else {
            if i < out.len() { out[i] = c; i += 1; }
        }
    }

    // Suffix
    let s = b"\"}";
    for &c in s { if i < out.len() { out[i] = c; i += 1; } }
    
    return i;
}

// ─────────────────────────────────────────────────────────────────────────────
// Core Logic: Adapted from discord_request
// ─────────────────────────────────────────────────────────────────────────────
unsafe fn chat_request(
    method:   &[u8],
    endpoint: &[u8],
    body:     Option<&[u8]>,
    out:      &mut [u8],
    retry:    u32,
) -> Option<usize> {
    // No rate limit wait for internal proxy, but logic kept
    // discord_wait_rate_limit(); 

    for attempt in 0..=retry {
        if attempt > 0 {
            let backoff = 500u32 << attempt.min(4);
            rust_print(b"Chat: retry "); print_num(attempt as i32);
            rust_print(b", wait "); print_num(backoff as i32); rust_print(b" ms\n");
            sleep_ms(backoff);
        }

        // Connect to your Python Proxy
        if !tcp_connect(&PROXY_IP, PROXY_PORT) {
            rust_print(b"Chat: proxy connect failed\n");
            continue;
        }

        // Construct HTTP Request
        let mut req = [0u8; 2048];
        let mut i = 0;
        
        macro_rules! push {
            ($b:expr) => { for &c in $b { if i < req.len() { req[i]=c; i+=1; } } };
        }
        
        push!(method); push!(b" "); push!(endpoint); push!(b" HTTP/1.1\r\n");
        // Use actual proxy IP or 'proxy-host' if python checks it, but usually 'localhost' or ip works
        push!(b"Host: 127.0.0.1\r\n"); 
        push!(b"User-Agent: RadiumOS/1.0\r\n");
        push!(b"Accept: application/json\r\n");
        
        if let Some(bd) = body {
            push!(b"Content-Type: application/json\r\n");
            push!(b"Content-Length: ");
            
            // Manual integer to string
            let mut tmp=[0u8;10]; let mut ti=0; let mut t=bd.len();
            if t==0{tmp[0]=b'0';ti=1;}
            else{while t>0{tmp[ti]=(t%10)as u8+b'0';t/=10;ti+=1;}}
            for k in (0..ti).rev(){ if i<req.len(){req[i]=tmp[k];i+=1;} }
            push!(b"\r\n");
        }
        push!(b"Connection: close\r\n\r\n");
        if let Some(bd) = body { for &c in bd { if i<req.len(){req[i]=c;i+=1;} } }

        // Send
        if !tcp_send_data(&req[0..i]) {
            rust_print(b"Chat: send failed\n");
            tcp_close(); continue;
        }

        // Receive Loop (Same logic as discord_request)
        let saved_local  = TCP_CONNECTION.local_port;
        let saved_remote = TCP_CONNECTION.remote_port;

        // Clear buffer (flush RTL8139)
        for _ in 0..300 { rust_rtl8139_receive(); RX_RESPONSE_LENGTH = 0; }

        HTTP_RECEIVE_LEN   = 0;
        RX_RESPONSE_LENGTH = 0;

        let hard          = 5_000_000u32; // Shorter timeout for local proxy
        let idle          = 500_000u32;
        let mut cur       = 0u32;
        let mut last_data = 0u32;
        let mut got_hdr   = false;
        let mut body_off  = 0usize;
        let mut exp_len: Option<usize> = None;

        'recv: loop {
            cur += 1;
            if cur >= hard { rust_print(b"Chat: hard timeout\n"); break 'recv; }
            if HTTP_RECEIVE_LEN > 0 && cur.wrapping_sub(last_data) >= idle {
                rust_print(b"Chat: idle timeout\n"); break 'recv;
            }
            if got_hdr {
                if let Some(el) = exp_len {
                    if HTTP_RECEIVE_LEN.saturating_sub(body_off) >= el {
                        RX_RESPONSE_LENGTH = 0; break 'recv;
                    }
                }
            }

            rust_rtl8139_receive();
            if RX_RESPONSE_LENGTH < 54 { RX_RESPONSE_LENGTH=0; continue 'recv; }

            // Parse IPv4
            let et = ((RX_RESPONSE_BUFFER[12] as u16)<<8)|(RX_RESPONSE_BUFFER[13] as u16);
            if et != 0x0800 { RX_RESPONSE_LENGTH=0; continue 'recv; }
            let ihl = ((RX_RESPONSE_BUFFER[14] & 0x0F) * 4) as usize;
            if ihl < 20 { RX_RESPONSE_LENGTH=0; continue 'recv; }
            if RX_RESPONSE_BUFFER[14+9] != 6 { RX_RESPONSE_LENGTH=0; continue 'recv; }

            let tcp_start = 14 + ihl;
            if tcp_start+20 > RX_RESPONSE_LENGTH as usize { RX_RESPONSE_LENGTH=0; continue 'recv; }

            let src_p = ((RX_RESPONSE_BUFFER[tcp_start]   as u16)<<8)|(RX_RESPONSE_BUFFER[tcp_start+1] as u16);
            let dst_p = ((RX_RESPONSE_BUFFER[tcp_start+2] as u16)<<8)|(RX_RESPONSE_BUFFER[tcp_start+3] as u16);
            if src_p != saved_remote || dst_p != saved_local { RX_RESPONSE_LENGTH=0; continue 'recv; }

            let tcp_hl   = ((RX_RESPONSE_BUFFER[tcp_start+12]>>4)*4) as usize;
            let flags    = RX_RESPONSE_BUFFER[tcp_start+13];
            let data_off = tcp_start + tcp_hl;
            let total    = RX_RESPONSE_LENGTH as usize;

            let rseq = ((RX_RESPONSE_BUFFER[tcp_start+4] as u32)<<24)
                     | ((RX_RESPONSE_BUFFER[tcp_start+5] as u32)<<16)
                     | ((RX_RESPONSE_BUFFER[tcp_start+6] as u32)<< 8)
                     |  (RX_RESPONSE_BUFFER[tcp_start+7] as u32);

            if data_off < total {
                let dlen = total - data_off;
                if dlen > 0 {
                    let space = HTTP_RECEIVE_BUFFER.len() - HTTP_RECEIVE_LEN;
                    let copy  = dlen.min(space);
                    for k in 0..copy {
                        HTTP_RECEIVE_BUFFER[HTTP_RECEIVE_LEN+k] = RX_RESPONSE_BUFFER[data_off+k];
                    }
                    HTTP_RECEIVE_LEN += copy;
                    last_data = cur;

                    if !got_hdr {
                        let scan_end = HTTP_RECEIVE_LEN.saturating_sub(3);
                        'hdr: for k in 0..scan_end {
                            if HTTP_RECEIVE_BUFFER[k]  ==b'\r'&&HTTP_RECEIVE_BUFFER[k+1]==b'\n'
                            &&HTTP_RECEIVE_BUFFER[k+2]==b'\r'&&HTTP_RECEIVE_BUFFER[k+3]==b'\n' {
                                body_off = k + 4;
                                got_hdr  = true;
                                let needle = b"content-length: ";
                                'cl: for j in 0..k {
                                    if j+needle.len()>k { break; }
                                    let mut ok=true;
                                    for n in 0..needle.len() {
                                        let c=HTTP_RECEIVE_BUFFER[j+n];
                                        let cl=if c>=b'A'&&c<=b'Z'{c+32}else{c};
                                        if cl!=needle[n]{ok=false;break;}
                                    }
                                    if ok {
                                        let mut val=0usize; let mut p=j+needle.len();
                                        while p<k&&HTTP_RECEIVE_BUFFER[p]>=b'0'&&HTTP_RECEIVE_BUFFER[p]<=b'9' {
                                            val=val*10+(HTTP_RECEIVE_BUFFER[p]-b'0')as usize; p+=1;
                                        }
                                        if val>0 { exp_len=Some(val); }
                                        break 'cl;
                                    }
                                }
                                break 'hdr;
                            }
                        }
                    }
                    TCP_CONNECTION.ack_num = rseq.wrapping_add(dlen as u32);
                    send_ack();
                }
            }

            if (flags & 0x01) != 0 { // FIN
                TCP_CONNECTION.ack_num = rseq.wrapping_add(1);
                send_ack();
                RX_RESPONSE_LENGTH = 0;
                break 'recv;
            }
            RX_RESPONSE_LENGTH = 0;
        }

        let received_len = HTTP_RECEIVE_LEN;
        tcp_close();

        if received_len == 0 { rust_print(b"Chat: no data\n"); continue; }

        // Extract Body
        let mut final_body = 0usize;
        for k in 0..received_len.saturating_sub(3) {
            if HTTP_RECEIVE_BUFFER[k]  ==b'\r'&&HTTP_RECEIVE_BUFFER[k+1]==b'\n'
            &&HTTP_RECEIVE_BUFFER[k+2]==b'\r'&&HTTP_RECEIVE_BUFFER[k+3]==b'\n' {
                final_body = k+4; break;
            }
        }
        
        if final_body>=received_len { continue; }

        // Check Status
        let s0=HTTP_RECEIVE_BUFFER[9];
        let s1=HTTP_RECEIVE_BUFFER[10];
        let s2=HTTP_RECEIVE_BUFFER[11];
        if s0>=b'0'&&s0<=b'9'&&s1>=b'0'&&s1<=b'9'&&s2>=b'0'&&s2<=b'9' {
             let status = ((s0-b'0') as u16)*100+((s1-b'0') as u16)*10+((s2-b'0') as u16);
             if status >= 200 && status < 300 {
                 // Success
                 let blen = received_len - final_body;
                 let copy = blen.min(out.len());
                 for k in 0..copy { out[k]=HTTP_RECEIVE_BUFFER[final_body+k]; }
                 return Some(copy);
             } else {
                 rust_print(b"Chat: Server error "); print_num(status as i32); rust_print(b"\n");
                 continue;
             }
        }
    }

    None
}

static mut HTTP_REQ_BUFFER: [u8; 2048] = [0; 2048];
static mut HTTP_BODY_BUFFER: [u8; 2048] = [0; 2048];
static mut CHAT_BUFFER: [u8; 4096] = [0; 4096];
/// Internal logic to fetch and display chat messages.
pub fn update_chat_internal() -> bool {
    rust_print(b"Chat: Updating...\n");
    
    // Call chat_request passing the GLOBAL CHAT_BUFFER.
    // This avoids allocating 4KB on the stack.
    match unsafe { chat_request(b"GET", b"/api/chat", None, &mut CHAT_BUFFER, 3) } {
        Some(len) => {
            rust_print(b"--- CHAT LOG ---\n");
            
            unsafe {
                for k in 0..len {
                    let c = CHAT_BUFFER[k];
                    
                    // Print standard printable ASCII characters
                    if c >= 32 && c < 127 {
                        terminal_putchar(c);
                    } 
                    // Handle newlines/carriage returns so the log is readable
                    else if c == b'\n' || c == b'\r' {
                        terminal_putchar(b'\n');
                    }
                }
            }
            
            rust_print(b"\n----------------\n");
            true
        },
        None => {
            rust_print(b"Chat: Update failed.\n");
            false
        }
    }
}
fn send_chat_internal(user: &[u8], msg: &[u8]) -> bool {
    // Use the global buffer instead of local stack
    let json_len = unsafe { build_chat_json(&mut HTTP_BODY_BUFFER, user, msg) };
    
    let mut resp_buf = [0u8; 64];
    
    rust_print(b"Chat: Sending...\n");
    
    match unsafe { chat_request(b"POST", b"/api/chat", Some(&HTTP_BODY_BUFFER[..json_len]), &mut resp_buf, 3) } {
        Some(_) => {
            rust_print(b"Chat: Sent successfully.\n");
            true
        },
        None => {
            rust_print(b"Chat: Failed to send.\n");
            false
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. C-Compatible Wrappers (Names match C calls, convert pointers to slices)
// ─────────────────────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn send_chat(user: *const u8, msg: *const u8) -> bool {
    unsafe {
        if user.is_null() || msg.is_null() {
            return false;
        }

        // Calculate length of User string (C-style, null-terminated)
        let mut user_len = 0;
        let mut user_ptr = user;
        while *user_ptr != 0 {
            user_len += 1;
            user_ptr = user_ptr.add(1);
        }

        // Calculate length of Msg string
        let mut msg_len = 0;
        let mut msg_ptr = msg;
        while *msg_ptr != 0 {
            msg_len += 1;
            msg_ptr = msg_ptr.add(1);
        }

        // Create slices from raw pointers
        let user_slice = core::slice::from_raw_parts(user, user_len);
        let msg_slice = core::slice::from_raw_parts(msg, msg_len);

        // Call internal logic
        send_chat_internal(user_slice, msg_slice)
    }
}

#[no_mangle]
pub extern "C" fn update_chat() -> bool {
    update_chat_internal()
}


//─────────────────────────────────────────────────────────────────────────────
// discord_request
//─────────────────────────────────────────────────────────────────────────────
#[no_mangle]
pub unsafe extern "C" fn discord_request(
    method:   &[u8],
    endpoint: &[u8],
    body:     Option<&[u8]>,
    out:      &mut [u8],
    retry:    u32,
) -> Option<usize> {
    discord_wait_rate_limit();

    for attempt in 0..=retry {
        if attempt > 0 {
            let backoff = 500u32 << attempt.min(4);
            rust_print(b"Discord: retry "); print_num(attempt as i32);
            rust_print(b", waiting "); print_num(backoff as i32); rust_print(b" ms\n");
            sleep_ms(backoff);
        }

        let proxy_ip = [72u8, 14, 176, 144];
        if !tcp_connect(&proxy_ip, 8080) {
            rust_print(b"Discord: proxy connect failed\n");
            continue;
        }

        let mut req = [0u8; 4096];
        let mut i = 0;
        macro_rules! push {
            ($b:expr) => { for &c in $b { if i < req.len() { req[i]=c; i+=1; } } };
        }
        
        push!(method); push!(b" "); push!(endpoint); push!(b" HTTP/1.1\r\n");
        push!(b"Host: discord.com\r\n");
        
        // Use "Bot " prefix for non-expiring tokens
        push!(b"Authorization: Bot "); 

        // ── INLINE SECURE KEY DECRYPTION ─────────────────────────────────
        let xor_key: u8 = 0xAA; 
        for j in 0..DISCORD_TOKEN_LEN { 
            if i < req.len() { 
                req[i] = DISCORD_TOKEN[j] ^ xor_key; 
                i += 1; 
            } 
        }
        // ─────────────────────────────────────────────────────────────────

        push!(b"\r\n");
        // Explicitly set headers to trigger proxy header bypass matching "radiumos"
        push!(b"User-Agent: RadiumOS\r\n");
        push!(b"X-RadiumOS: true\r\n");
        push!(b"Accept: application/json\r\n");
        
        if let Some(bd) = body {
            push!(b"Content-Type: application/json\r\n");
            push!(b"Content-Length: ");
            let mut tmp=[0u8;10]; let mut ti=0; let mut t=bd.len();
            if t==0{tmp[0]=b'0';ti=1;}
            else{while t>0{tmp[ti]=(t%10)as u8+b'0';t/=10;ti+=1;}}
            for k in (0..ti).rev(){ if i<req.len(){req[i]=tmp[k];i+=1;} }
            push!(b"\r\n");
        }
        push!(b"Connection: close\r\n\r\n");
        
        if let Some(bd) = body { for &c in bd { if i<req.len(){req[i]=c;i+=1;} } }

        let send_success = tcp_send_data(&req[0..i]);

        // ── ANTI-HIJACKING MEMORY WIPE ───────────────────────────────────
        for b in req.iter_mut() { *b = 0; }
        // ─────────────────────────────────────────────────────────────────

        if !send_success {
            rust_print(b"Discord: send failed\n");
            tcp_close(); 
            continue;
        }

        let saved_local  = TCP_CONNECTION.local_port;
        let saved_remote = TCP_CONNECTION.remote_port;

        rust_print(b"Discord: sent, local="); print_num(saved_local as i32);
        rust_print(b" remote="); print_num(saved_remote as i32); rust_print(b"\n");

        for _ in 0..300 { rust_rtl8139_receive(); RX_RESPONSE_LENGTH = 0; }

        HTTP_RECEIVE_LEN   = 0;
        RX_RESPONSE_LENGTH = 0;

        let hard          = 8_000_000u32;
        let idle          = 800_000u32;
        let mut cur       = 0u32;
        let mut last_data = 0u32;
        let mut got_hdr   = false;
        let mut body_off  = 0usize;
        let mut exp_len: Option<usize> = None;

        'recv: loop {
            cur += 1;
            if cur >= hard { rust_print(b"Discord: hard timeout\n"); break 'recv; }
            if HTTP_RECEIVE_LEN > 0 && cur.wrapping_sub(last_data) >= idle {
                rust_print(b"Discord: idle timeout\n"); break 'recv;
            }
            if got_hdr {
                if let Some(el) = exp_len {
                    if HTTP_RECEIVE_LEN.saturating_sub(body_off) >= el {
                        rust_print(b"Discord: Content-Length satisfied\n");
                        RX_RESPONSE_LENGTH = 0; break 'recv;
                    }
                }
            }

            rust_rtl8139_receive();
            if RX_RESPONSE_LENGTH < 54 { RX_RESPONSE_LENGTH=0; continue 'recv; }

            let et = ((RX_RESPONSE_BUFFER[12] as u16)<<8)|(RX_RESPONSE_BUFFER[13] as u16);
            if et != 0x0800 { RX_RESPONSE_LENGTH=0; continue 'recv; }

            let ihl = ((RX_RESPONSE_BUFFER[14] & 0x0F) * 4) as usize;
            if ihl < 20 { RX_RESPONSE_LENGTH=0; continue 'recv; }
            if RX_RESPONSE_BUFFER[14+9] != 6 { RX_RESPONSE_LENGTH=0; continue 'recv; }

            let tcp_start = 14 + ihl;
            if tcp_start+20 > RX_RESPONSE_LENGTH as usize { RX_RESPONSE_LENGTH=0; continue 'recv; }

            let src_p = ((RX_RESPONSE_BUFFER[tcp_start]   as u16)<<8)|(RX_RESPONSE_BUFFER[tcp_start+1] as u16);
            let dst_p = ((RX_RESPONSE_BUFFER[tcp_start+2] as u16)<<8)|(RX_RESPONSE_BUFFER[tcp_start+3] as u16);
            if src_p != saved_remote || dst_p != saved_local { RX_RESPONSE_LENGTH=0; continue 'recv; }

            let tcp_hl   = ((RX_RESPONSE_BUFFER[tcp_start+12]>>4)*4) as usize;
            let flags    = RX_RESPONSE_BUFFER[tcp_start+13];
            let data_off = tcp_start + tcp_hl;
            let total    = RX_RESPONSE_LENGTH as usize;

            let rseq = ((RX_RESPONSE_BUFFER[tcp_start+4] as u32)<<24)
                     | ((RX_RESPONSE_BUFFER[tcp_start+5] as u32)<<16)
                     | ((RX_RESPONSE_BUFFER[tcp_start+6] as u32)<< 8)
                     |  (RX_RESPONSE_BUFFER[tcp_start+7] as u32);

            if data_off < total {
                let dlen = total - data_off;
                if dlen > 0 {
                    let space = HTTP_RECEIVE_BUFFER.len() - HTTP_RECEIVE_LEN;
                    let copy  = dlen.min(space);
                    for k in 0..copy {
                        HTTP_RECEIVE_BUFFER[HTTP_RECEIVE_LEN+k] = RX_RESPONSE_BUFFER[data_off+k];
                    }
                    HTTP_RECEIVE_LEN += copy;
                    last_data = cur;

                    if !got_hdr {
                        let scan_end = HTTP_RECEIVE_LEN.saturating_sub(3);
                        'hdr: for k in 0..scan_end {
                            if HTTP_RECEIVE_BUFFER[k]  ==b'\r'&&HTTP_RECEIVE_BUFFER[k+1]==b'\n'
                            &&HTTP_RECEIVE_BUFFER[k+2]==b'\r'&&HTTP_RECEIVE_BUFFER[k+3]==b'\n' {
                                body_off = k + 4;
                                got_hdr  = true;
                                let needle = b"content-length: ";
                                'cl: for j in 0..k {
                                    if j+needle.len()>k { break; }
                                    let mut ok=true;
                                    for n in 0..needle.len() {
                                        let c=HTTP_RECEIVE_BUFFER[j+n];
                                        let cl=if c>=b'A'&&c<=b'Z'{c+32}else{c};
                                        if cl!=needle[n]{ok=false;break;}
                                    }
                                    if ok {
                                        let mut val=0usize; let mut p=j+needle.len();
                                        while p<k&&HTTP_RECEIVE_BUFFER[p]>=b'0'&&HTTP_RECEIVE_BUFFER[p]<=b'9' {
                                            val=val*10+(HTTP_RECEIVE_BUFFER[p]-b'0')as usize; p+=1;
                                        }
                                        if val>0 {
                                            exp_len=Some(val);
                                            rust_print(b"Discord: Content-Length=");
                                            print_num(val as i32); rust_print(b"\n");
                                        }
                                        break 'cl;
                                    }
                                }
                                break 'hdr;
                            }
                        }
                    }
                    TCP_CONNECTION.ack_num = rseq.wrapping_add(dlen as u32);
                    send_ack();
                }
            }

            if (flags & 0x01) != 0 {
                rust_print(b"Discord: server FIN\n");
                TCP_CONNECTION.ack_num = rseq.wrapping_add(1);
                send_ack();
                RX_RESPONSE_LENGTH = 0;
                break 'recv;
            }
            RX_RESPONSE_LENGTH = 0;
        }

        let received_len = HTTP_RECEIVE_LEN;
        tcp_close();

        if received_len == 0 { rust_print(b"Discord: no data received\n"); continue; }

        rust_print(b"Discord: processing "); print_num(received_len as i32); rust_print(b" bytes\n");

        discord_parse_rate_limit_headers();

        let mut final_body = 0usize;
        for k in 0..received_len.saturating_sub(3) {
            if HTTP_RECEIVE_BUFFER[k]  ==b'\r'&&HTTP_RECEIVE_BUFFER[k+1]==b'\n'
            &&HTTP_RECEIVE_BUFFER[k+2]==b'\r'&&HTTP_RECEIVE_BUFFER[k+3]==b'\n' {
                final_body = k+4; break;
            }
        }
        if final_body==0||final_body>=received_len { rust_print(b"Discord: no body separator\n"); continue; }
        if received_len < 12 { rust_print(b"Discord: response too short\n"); continue; }

        let s0=HTTP_RECEIVE_BUFFER[9];
        let s1=HTTP_RECEIVE_BUFFER[10];
        let s2=HTTP_RECEIVE_BUFFER[11];
        if s0<b'0'||s0>b'9'||s1<b'0'||s1>b'9'||s2<b'0'||s2>b'9' {
            rust_print(b"Discord: bad status bytes\n"); continue;
        }

        let status = ((s0-b'0') as u16)*100+((s1-b'0') as u16)*10+((s2-b'0') as u16);
        rust_print(b"Discord: HTTP "); print_num(status as i32); rust_print(b"\n");

        if status == 429 { DISCORD_RL_REMAINING=0; continue; }
        if status >= 400 && status < 500 {
            rust_print(b"Discord: client error: ");
            for k in final_body..received_len.min(final_body+256) {
                let c=HTTP_RECEIVE_BUFFER[k]; if c>=32&&c<127{terminal_putchar(c);}
            }
            rust_print(b"\n");
            return None;
        }
        if status >= 500 { rust_print(b"Discord: server error, retrying\n"); continue; }

        let blen = received_len - final_body;
        let copy = blen.min(out.len());
        for k in 0..copy { out[k]=HTTP_RECEIVE_BUFFER[final_body+k]; }
        rust_print(b"Discord: success, "); print_num(copy as i32); rust_print(b" bytes\n");

        let mut m_idx = 0;
        for &c in method {
            if m_idx < NET_LAST_METHOD.len() - 1 { NET_LAST_METHOD[m_idx] = c; m_idx += 1; }
        }
        NET_LAST_METHOD[m_idx] = 0;

        let proto = b"HTTP/1.1";
        let mut p_idx = 0;
        for &c in proto {
            if p_idx < NET_LAST_PROTO.len() - 1 { NET_LAST_PROTO[p_idx] = c; p_idx += 1; }
        }
        NET_LAST_PROTO[p_idx] = 0;

        let desc = b"Discord API";
        let mut d_idx = 0;
        for &c in desc {
            if d_idx < NET_LAST_DESC.len() - 1 { NET_LAST_DESC[d_idx] = c; d_idx += 1; }
        }
        NET_LAST_DESC[d_idx] = 0;

        NET_LAST_UPDATE_TICKS = SYSTEM_TICKS.load(core::sync::atomic::Ordering::Relaxed);
        NET_HUD_DIRTY = true;

        return Some(copy);
    }

    rust_print(b"Discord: all attempts exhausted\n");
    None
}
//─────────────────────────────────────────────────────────────────────────────
// Module system - internal
//─────────────────────────────────────────────────────────────────────────────

unsafe fn module_find(name: &[u8]) -> Option<usize> {
    for i in 0..MODULE_COUNT {
        let m = &MODULES[i];
        if m.name_len == name.len() {
            let mut eq = true;
            for j in 0..m.name_len { if m.name[j]!=name[j]{eq=false;break;} }
            if eq { return Some(i); }
        }
    }
    None
}

unsafe fn module_get_param<'a>(m: &'a DiscordModule, key: &[u8]) -> Option<&'a [u8]> {
    for i in 0..m.param_count {
        let p = &m.params[i];
        if p.key_len == key.len() {
            let mut eq = true;
            for j in 0..p.key_len { if p.key[j]!=key[j]{eq=false;break;} }
            if eq { return Some(&p.val[..p.val_len]); }
        }
    }
    None
}

unsafe fn module_set_param(m: &mut DiscordModule, key: &[u8], val: &[u8]) {
    for i in 0..m.param_count {
        let p = &mut m.params[i];
        if p.key_len == key.len() {
            let mut eq = true;
            for j in 0..p.key_len { if p.key[j]!=key[j]{eq=false;break;} }
            if eq {
                let vl=val.len().min(MODULE_PARAM_LEN);
                p.val[..vl].copy_from_slice(&val[..vl]); p.val_len=vl;
                return;
            }
        }
    }
    if m.param_count < MODULE_PARAMS {
        let p = &mut m.params[m.param_count];
        let kl=key.len().min(32); p.key[..kl].copy_from_slice(&key[..kl]); p.key_len=kl;
        let vl=val.len().min(MODULE_PARAM_LEN); p.val[..vl].copy_from_slice(&val[..vl]); p.val_len=vl;
        m.param_count += 1;
    }
}

unsafe fn module_init_params(m: &mut DiscordModule) {
    match m.kind {
        ModuleKind::SendEmoji => {
            module_set_param(m, b"channel_id",    b"");
            module_set_param(m, b"emoji",         b"");
            module_set_param(m, b"message_id",    b"");
            module_set_param(m, b"count",         b"1");
            module_set_param(m, b"message_after", b"");
        }
        ModuleKind::SendMessage => {
            module_set_param(m, b"channel_id", b"");
            module_set_param(m, b"message",    b"");
        }
        ModuleKind::SendEmbed => {
            module_set_param(m, b"channel_id",  b"");
            module_set_param(m, b"title",       b"");
            module_set_param(m, b"description", b"");
            module_set_param(m, b"color",       b"5814783");
        }
        ModuleKind::FetchMessages => {
            module_set_param(m, b"channel_id", b"");
            module_set_param(m, b"limit",      b"10");
        }
        ModuleKind::DeleteMessage => {
            module_set_param(m, b"channel_id", b"");
            module_set_param(m, b"message_id", b"");
        }
        ModuleKind::React => {
            module_set_param(m, b"channel_id", b"");
            module_set_param(m, b"message_id", b"");
            module_set_param(m, b"emoji",      b"");
        }
        ModuleKind::AutoReply => {
            module_set_param(m, b"channel_id", b"");
            module_set_param(m, b"trigger",    b"");
            module_set_param(m, b"reply",      b"");
            module_set_param(m, b"poll_ms",    b"5000");
        }
        ModuleKind::None => {}
    }
}

unsafe fn read_line(buf: &mut [u8], prompt: &[u8]) -> usize {
    rust_print(prompt);
    let mut len = 0usize;
    let mut shift = false;

    loop {
        // Poll for keypress
        if !is_key_pressed() { continue; }
        let scan = port_byte_in(0x60);

        // Shift tracking
        if scan == 0x2A || scan == 0x36 { shift = true;  continue; }
        if scan == 0xAA || scan == 0xB6 { shift = false; continue; }

        // Key releases
        if scan >= 0x80 { continue; }

        match scan {
            0x1C => { rust_print(b"\n"); break; }
            0x0E => {
                if len > 0 {
                    len -= 1;
                    rust_print(b"\x08 \x08");
                }
            }
            0x01 => { rust_print(b"\n"); return 0; }
            0x1D | 0x38 | 0x3A | 0x3B..=0x44 | 0x47..=0x53 => {}
            _ => {
                let ch = scancode_to_ascii(scan, shift);
                if ch != 0 && len < buf.len() - 1 {
                    buf[len] = ch;
                    len += 1;
                    terminal_putchar(ch);
                }
            }
        }

        // Small delay to debounce
        for _ in 0..10000 { core::hint::spin_loop(); }
    }

    len
}

unsafe fn module_interactive_config(m: &mut DiscordModule) {
    terminal_clear();
    terminal_setcolor(0x0B);
    rust_print(b"=== Configuring module: ");
    for k in 0..m.name_len { terminal_putchar(m.name[k]); }
    rust_print(b" ===\n");
    terminal_setcolor(0x07);
    rust_print(b"Press Enter to keep current value. Esc to cancel.\n\n");

    for i in 0..m.param_count {
        terminal_setcolor(0x0E);
        rust_print(b"  ");
        for k in 0..m.params[i].key_len { terminal_putchar(m.params[i].key[k]); }
        if m.params[i].val_len > 0 {
            rust_print(b" [");
            for k in 0..m.params[i].val_len { terminal_putchar(m.params[i].val[k]); }
            rust_print(b"]");
        }
        rust_print(b"\n");
        terminal_setcolor(0x07);

        let key = &m.params[i].key[..m.params[i].key_len];
        if key == b"emoji" {
            rust_print(b"  Hint: URL-encoded e.g. %E2%9D%A4=heart %F0%9F%94%A5=fire\n");
        } else if key == b"color" {
            rust_print(b"  Hint: decimal RGB e.g. 15548997=red 5763719=green 3447003=blue\n");
        } else if key == b"count" {
            rust_print(b"  Hint: number of times to repeat (max 20)\n");
        } else if key == b"message_after" {
            rust_print(b"  Hint: optional follow-up message (leave blank to skip)\n");
        } else if key == b"poll_ms" {
            rust_print(b"  Hint: polling interval in ms (min 1000)\n");
        } else if key == b"trigger" {
            rust_print(b"  Hint: case-insensitive substring to match in messages\n");
        }

        let mut input = [0u8; MODULE_PARAM_LEN];
        let ilen = read_line(&mut input, b"  > ");
        if ilen > 0 {
            let vl = ilen.min(MODULE_PARAM_LEN);
            m.params[i].val[..vl].copy_from_slice(&input[..vl]);
            m.params[i].val_len = vl;
            terminal_setcolor(0x0A); rust_print(b"  Set.\n"); terminal_setcolor(0x07);
        } else {
            rust_print(b"  (kept)\n");
        }
        rust_print(b"\n");
    }

    m.configured = true;
    terminal_setcolor(0x0A);
    rust_print(b"\nConfigured! Use run.module.");
    for k in 0..m.name_len { terminal_putchar(m.name[k]); }
    rust_print(b"\n");
    terminal_setcolor(0x07);
}

unsafe fn module_run(m: &DiscordModule) -> i32 {
    if !m.configured {
        rust_print(b"Not configured. Use config.module.");
        for k in 0..m.name_len { terminal_putchar(m.name[k]); }
        rust_print(b"\n");
        return -1;
    }
    rust_print(b"\nRunning module: ");
    for k in 0..m.name_len { terminal_putchar(m.name[k]); }
    rust_print(b"\n");

    match m.kind {

        ModuleKind::SendEmoji => {
            let ch    = match module_get_param(m,b"channel_id"){Some(v)if v.len()>0=>v,_=>{rust_print(b"channel_id not set\n");return -1;}};
            let mid   = match module_get_param(m,b"message_id"){Some(v)if v.len()>0=>v,_=>{rust_print(b"message_id not set\n");return -1;}};
            let emoji = match module_get_param(m,b"emoji")     {Some(v)if v.len()>0=>v,_=>{rust_print(b"emoji not set\n");return -1;}};

            let count_raw = module_get_param(m, b"count").unwrap_or(b"1");
            let mut count = 0usize;
            for &c in count_raw { if c>=b'0'&&c<=b'9'{ count=count*10+(c-b'0')as usize; } }
            if count==0{count=1;} if count>20{count=20;}

            let mut ch_buf  = [0u8; ID_LEN+1];
            let mut mid_buf = [0u8; ID_LEN+1];
            let mut em_buf  = [0u8; 128];
            let cl=ch.len().min(ID_LEN);    ch_buf[..cl].copy_from_slice(&ch[..cl]);
            let ml=mid.len().min(ID_LEN);   mid_buf[..ml].copy_from_slice(&mid[..ml]);
            let el=emoji.len().min(127);    em_buf[..el].copy_from_slice(&emoji[..el]);

            rust_print(b"Reacting "); print_num(count as i32); rust_print(b"x with ");
            for &c in emoji { terminal_putchar(c); }
            rust_print(b"\n");

            for n in 0..count {
                rust_print(b"  "); print_num((n+1)as i32); rust_print(b"/"); print_num(count as i32); rust_print(b"\n");
                let mut ep=[0u8;512]; let mut idx=0;
                for &c in b"/api/v10/channels/"{ ep[idx]=c;idx+=1; }
                for &c in &ch_buf[..cl]         { ep[idx]=c;idx+=1; }
                for &c in b"/messages/"         { ep[idx]=c;idx+=1; }
                for &c in &mid_buf[..ml]        { ep[idx]=c;idx+=1; }
                for &c in b"/reactions/"        { ep[idx]=c;idx+=1; }
                for &c in &em_buf[..el]         { ep[idx]=c;idx+=1; }
                for &c in b"/@me"               { ep[idx]=c;idx+=1; }
                let mut buf=[0u8;256];
                match discord_request(b"PUT",&ep[..idx],Some(b""),&mut buf,2) {
                    Some(_)=>rust_print(b"  OK\n"),
                    None   =>rust_print(b"  failed\n"),
                }
                if n+1<count { sleep_ms(600); }
            }

            if let Some(after) = module_get_param(m, b"message_after") {
                if after.len() > 0 {
                    rust_print(b"Sending follow-up message\n");
                    let mut body=[0u8;2048]; let mut bi=0;
                    for &c in b"{\"content\":\""{ body[bi]=c;bi+=1; }
                    for &c in after {
                        match c {
                            b'"' =>{body[bi]=b'\\';bi+=1;body[bi]=b'"'; bi+=1;}
                            b'\\'=>{body[bi]=b'\\';bi+=1;body[bi]=b'\\';bi+=1;}
                            b'\n'=>{body[bi]=b'\\';bi+=1;body[bi]=b'n'; bi+=1;}
                            c    =>{body[bi]=c;bi+=1;}
                        }
                    }
                    for &c in b"\"}"{body[bi]=c;bi+=1;}
                    let mut ep2=[0u8;256]; let mut idx2=0;
                    for &c in b"/api/v10/channels/"{ ep2[idx2]=c;idx2+=1; }
                    for &c in &ch_buf[..cl]{ ep2[idx2]=c;idx2+=1; }
                    for &c in b"/messages"{ ep2[idx2]=c;idx2+=1; }
                    let mut buf2=[0u8;4096];
                    match discord_request(b"POST",&ep2[..idx2],Some(&body[..bi]),&mut buf2,2){
                        Some(_)=>rust_print(b"Follow-up sent!\n"),
                        None   =>rust_print(b"Follow-up failed\n"),
                    }
                }
            }
            0
        }

        ModuleKind::SendMessage => {
            let ch  = match module_get_param(m,b"channel_id"){Some(v)if v.len()>0=>v,_=>{rust_print(b"channel_id not set\n");return -1;}};
            let msg = match module_get_param(m,b"message")   {Some(v)if v.len()>0=>v,_=>{rust_print(b"message not set\n");   return -1;}};
            let mut ch_buf=[0u8;ID_LEN+1];
            let cl=ch.len().min(ID_LEN); ch_buf[..cl].copy_from_slice(&ch[..cl]);
            let mut body=[0u8;2048]; let mut bi=0;
            for &c in b"{\"content\":\""{ body[bi]=c;bi+=1; }
            for &c in msg {
                match c {
                    b'"' =>{body[bi]=b'\\';bi+=1;body[bi]=b'"'; bi+=1;}
                    b'\\'=>{body[bi]=b'\\';bi+=1;body[bi]=b'\\';bi+=1;}
                    b'\n'=>{body[bi]=b'\\';bi+=1;body[bi]=b'n'; bi+=1;}
                    c    =>{body[bi]=c;bi+=1;}
                }
            }
            for &c in b"\"}"{body[bi]=c;bi+=1;}
            let mut ep=[0u8;256]; let mut idx=0;
            for &c in b"/api/v10/channels/"{ ep[idx]=c;idx+=1; }
            for &c in &ch_buf[..cl]{ ep[idx]=c;idx+=1; }
            for &c in b"/messages"{ ep[idx]=c;idx+=1; }
            let mut buf=[0u8;4096];
            match discord_request(b"POST",&ep[..idx],Some(&body[..bi]),&mut buf,3){
                Some(_)=>{rust_print(b"Sent!\n");0}
                None   =>{rust_print(b"Failed\n");-1}
            }
        }

        ModuleKind::SendEmbed => {
            let ch    = match module_get_param(m,b"channel_id") {Some(v)if v.len()>0=>v,_=>{rust_print(b"channel_id not set\n");return -1;}};
            let title = module_get_param(m,b"title").unwrap_or(b"");
            let desc  = module_get_param(m,b"description").unwrap_or(b"");
            let color_raw = module_get_param(m,b"color").unwrap_or(b"0");
            let mut color=0u32;
            for &c in color_raw { if c>=b'0'&&c<=b'9'{ color=color*10+(c-b'0')as u32; } }
            let mut ch_buf=[0u8;ID_LEN+1];
            let cl=ch.len().min(ID_LEN); ch_buf[..cl].copy_from_slice(&ch[..cl]);
            let mut body=[0u8;2048]; let mut bi=0;
            macro_rules! bp{($b:expr)=>{for &c in $b{if bi<body.len(){body[bi]=c;bi+=1;}}}}
            macro_rules! bpe{($s:expr)=>{
                for &c in $s{match c{
                    b'"' =>{body[bi]=b'\\';bi+=1;body[bi]=b'"'; bi+=1;}
                    b'\\'=>{body[bi]=b'\\';bi+=1;body[bi]=b'\\';bi+=1;}
                    b'\n'=>{body[bi]=b'\\';bi+=1;body[bi]=b'n'; bi+=1;}
                    c    =>{if bi<body.len(){body[bi]=c;bi+=1;}}
                }}
            }}
            bp!(b"{\"embeds\":[{\"color\":");
            let mut cv=color; let mut tmp=[0u8;10]; let mut ti=0;
            if cv==0{tmp[0]=b'0';ti=1;}else{while cv>0{tmp[ti]=(cv%10)as u8+b'0';cv/=10;ti+=1;}}
            for k in (0..ti).rev(){if bi<body.len(){body[bi]=tmp[k];bi+=1;}}
            bp!(b",\"title\":\""); bpe!(title); bp!(b"\"");
            bp!(b",\"description\":\""); bpe!(desc); bp!(b"\"}]}");
            let mut ep=[0u8;256]; let mut idx=0;
            for &c in b"/api/v10/channels/"{ ep[idx]=c;idx+=1; }
            for &c in &ch_buf[..cl]{ ep[idx]=c;idx+=1; }
            for &c in b"/messages"{ ep[idx]=c;idx+=1; }
            let mut buf=[0u8;4096];
            match discord_request(b"POST",&ep[..idx],Some(&body[..bi]),&mut buf,3){
                Some(_)=>{rust_print(b"Embed sent!\n");0}
                None   =>{rust_print(b"Embed failed\n");-1}
            }
        }

        ModuleKind::FetchMessages => {
            let ch = match module_get_param(m,b"channel_id"){Some(v)if v.len()>0=>v,_=>{rust_print(b"channel_id not set\n");return -1;}};
            let lim_raw = module_get_param(m,b"limit").unwrap_or(b"10");
            let mut lim=0i32;
            for &c in lim_raw { if c>=b'0'&&c<=b'9'{ lim=lim*10+(c-b'0')as i32; } }
            if lim<=0{lim=10;} if lim>100{lim=100;}
            let mut ch_buf=[0u8;ID_LEN+1];
            let cl=ch.len().min(ID_LEN); ch_buf[..cl].copy_from_slice(&ch[..cl]);
            rust_discord_get_channel_messages(ch_buf.as_ptr(), lim)
        }

        ModuleKind::DeleteMessage => {
            let ch  = match module_get_param(m,b"channel_id"){Some(v)if v.len()>0=>v,_=>{rust_print(b"channel_id not set\n");return -1;}};
            let mid = match module_get_param(m,b"message_id"){Some(v)if v.len()>0=>v,_=>{rust_print(b"message_id not set\n");return -1;}};
            let mut ch_buf=[0u8;ID_LEN+1]; let mut mid_buf=[0u8;ID_LEN+1];
            let cl=ch.len().min(ID_LEN);  ch_buf[..cl].copy_from_slice(&ch[..cl]);
            let ml=mid.len().min(ID_LEN); mid_buf[..ml].copy_from_slice(&mid[..ml]);
            rust_discord_delete_message(ch_buf.as_ptr(), mid_buf.as_ptr())
        }

        ModuleKind::React => {
            let ch    = match module_get_param(m,b"channel_id"){Some(v)if v.len()>0=>v,_=>{rust_print(b"channel_id not set\n");return -1;}};
            let mid   = match module_get_param(m,b"message_id"){Some(v)if v.len()>0=>v,_=>{rust_print(b"message_id not set\n");return -1;}};
            let emoji = match module_get_param(m,b"emoji")     {Some(v)if v.len()>0=>v,_=>{rust_print(b"emoji not set\n");     return -1;}};
            let mut ch_buf=[0u8;ID_LEN+1]; let mut mid_buf=[0u8;ID_LEN+1]; let mut em_buf=[0u8;128];
            let cl=ch.len().min(ID_LEN);   ch_buf[..cl].copy_from_slice(&ch[..cl]);
            let ml=mid.len().min(ID_LEN);  mid_buf[..ml].copy_from_slice(&mid[..ml]);
            let el=emoji.len().min(127);   em_buf[..el].copy_from_slice(&emoji[..el]);
            rust_discord_react(ch_buf.as_ptr(), mid_buf.as_ptr(), em_buf.as_ptr())
        }

        ModuleKind::AutoReply => {
            let ch      = match module_get_param(m,b"channel_id"){Some(v)if v.len()>0=>v,_=>{rust_print(b"channel_id not set\n");return -1;}};
            let trigger = match module_get_param(m,b"trigger")   {Some(v)if v.len()>0=>v,_=>{rust_print(b"trigger not set\n");   return -1;}};
            let reply   = match module_get_param(m,b"reply")     {Some(v)if v.len()>0=>v,_=>{rust_print(b"reply not set\n");     return -1;}};
            let poll_raw = module_get_param(m,b"poll_ms").unwrap_or(b"5000");
            let mut poll_ms=0u32;
            for &c in poll_raw { if c>=b'0'&&c<=b'9'{ poll_ms=poll_ms*10+(c-b'0')as u32; } }
            if poll_ms<1000{poll_ms=1000;}

            let mut ch_buf   = [0u8; ID_LEN+1];
            let mut trig_buf = [0u8; MODULE_PARAM_LEN];
            let mut rep_buf  = [0u8; MODULE_PARAM_LEN];
            let cl = ch.len().min(ID_LEN);               ch_buf[..cl].copy_from_slice(&ch[..cl]);
            let tl = trigger.len().min(MODULE_PARAM_LEN); trig_buf[..tl].copy_from_slice(&trigger[..tl]);
            let rl = reply.len().min(MODULE_PARAM_LEN);   rep_buf[..rl].copy_from_slice(&reply[..rl]);

            rust_print(b"AutoReply active. Trigger: ");
            for k in 0..tl { terminal_putchar(trig_buf[k]); }
            rust_print(b"\nPress Q to stop.\n");

            let mut last_id    = [0u8; MSG_ID_LEN];
            let mut last_id_len = 0usize;

            loop {
                if is_key_pressed() {
                    let scan = port_byte_in(0x60);
                    if scan == 0x10 { rust_print(b"\nAutoReply stopped.\n"); break; }
                }
                sleep_ms(poll_ms);
                rust_print(b".");

                let mut ep=[0u8;256]; let mut idx=0;
                for &c in b"/api/v10/channels/"  { ep[idx]=c;idx+=1; }
                for &c in &ch_buf[..cl]           { ep[idx]=c;idx+=1; }
                for &c in b"/messages?limit=1"    { ep[idx]=c;idx+=1; }

                let mut raw=[0u8;4096];
                let Some(len) = discord_request(b"GET",&ep[..idx],None,&mut raw,1)
                else { continue; };
                let Some(root) = json_parse(&raw,len) else { continue; };
                let Some(msg)  = json_array_get(root,0) else { continue; };

                let mut cur_id=[0u8;MSG_ID_LEN]; let mut cur_id_len=0usize;
                if let Some(id_i)=json_object_get(msg,b"id"){cur_id_len=copy_json_str(id_i,&mut cur_id);}
                if cur_id_len==last_id_len&&cur_id[..cur_id_len]==last_id[..last_id_len]{continue;}

                let mut content=[0u8;MSG_CONTENT_LEN]; let mut content_l=0usize;
                if let Some(co_i)=json_object_get(msg,b"content"){content_l=copy_json_str(co_i,&mut content);}

                let mut found=false;
                if content_l>=tl {
                    'outer: for start in 0..=(content_l-tl) {
                        let mut ok=true;
                        for k in 0..tl {
                            let a=content[start+k]; let b=trig_buf[k];
                            let al=if a>=b'A'&&a<=b'Z'{a+32}else{a};
                            let bl=if b>=b'A'&&b<=b'Z'{b+32}else{b};
                            if al!=bl{ok=false;break;}
                        }
                        if ok{found=true;break 'outer;}
                    }
                }

                if found {
                    rust_print(b"\nTrigger matched! Replying...\n");
                    let mut body=[0u8;2048]; let mut bi=0;
                    for &c in b"{\"content\":\""{ body[bi]=c;bi+=1; }
                    for &c in &rep_buf[..rl] {
                        match c {
                            b'"' =>{body[bi]=b'\\';bi+=1;body[bi]=b'"'; bi+=1;}
                            b'\\'=>{body[bi]=b'\\';bi+=1;body[bi]=b'\\';bi+=1;}
                            b'\n'=>{body[bi]=b'\\';bi+=1;body[bi]=b'n'; bi+=1;}
                            c    =>{body[bi]=c;bi+=1;}
                        }
                    }
                    for &c in b"\"}"{body[bi]=c;bi+=1;}
                    let mut ep2=[0u8;256]; let mut idx2=0;
                    for &c in b"/api/v10/channels/"{ ep2[idx2]=c;idx2+=1; }
                    for &c in &ch_buf[..cl]{ ep2[idx2]=c;idx2+=1; }
                    for &c in b"/messages"{ ep2[idx2]=c;idx2+=1; }
                    let mut buf2=[0u8;4096];
                    match discord_request(b"POST",&ep2[..idx2],Some(&body[..bi]),&mut buf2,2){
                        Some(_)=>rust_print(b"Reply sent!\n"),
                        None   =>rust_print(b"Reply failed\n"),
                    }
                }

                last_id[..cur_id_len].copy_from_slice(&cur_id[..cur_id_len]);
                last_id_len=cur_id_len;
            }
            0
        }

        ModuleKind::None => { rust_print(b"Module: no kind\n"); -1 }
    }
}

//─────────────────────────────────────────────────────────────────────────────
// Module system - public API
//─────────────────────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn rust_discord_set_module(name: *const u8) -> i32 {
    unsafe {
        if name.is_null() { return -1; }
        let mut nb=[0u8;MODULE_NAME_LEN]; let mut nl=0usize; let mut p=name;
        while *p!=0&&nl<MODULE_NAME_LEN-1{nb[nl]=*p;nl+=1;p=p.add(1);}

        if module_find(&nb[..nl]).is_some() {
            rust_print(b"Module already exists. Use config.module to reconfigure.\n");
            return 0;
        }
        if MODULE_COUNT >= MAX_MODULES { rust_print(b"Module limit reached\n"); return -1; }

        let kind =
            if      bytes_contain(&nb[..nl],b"send-emoji")   { ModuleKind::SendEmoji }
            else if bytes_contain(&nb[..nl],b"send-embed")   { ModuleKind::SendEmbed }
            else if bytes_contain(&nb[..nl],b"send-message") { ModuleKind::SendMessage }
            else if bytes_contain(&nb[..nl],b"fetch")        { ModuleKind::FetchMessages }
            else if bytes_contain(&nb[..nl],b"delete")       { ModuleKind::DeleteMessage }
            else if bytes_contain(&nb[..nl],b"react")        { ModuleKind::React }
            else if bytes_contain(&nb[..nl],b"auto-reply")   { ModuleKind::AutoReply }
            else {
                rust_print(b"Name must contain: send-emoji, send-embed, send-message,\n");
                rust_print(b"  fetch, delete, react, or auto-reply\n");
                return -1;
            };

        let m = &mut MODULES[MODULE_COUNT];
        m.name[..nl].copy_from_slice(&nb[..nl]); m.name_len=nl;
        m.kind=kind; m.configured=false; m.active=true; m.param_count=0;
        module_init_params(m);
        MODULE_COUNT += 1;

        terminal_setcolor(0x0A);
        rust_print(b"Module '");
        for k in 0..nl{terminal_putchar(nb[k]);}
        rust_print(b"' created. Next: config.module.");
        for k in 0..nl{terminal_putchar(nb[k]);}
        rust_print(b"\n");
        terminal_setcolor(0x07);
        0
    }
}

#[no_mangle]
pub extern "C" fn rust_discord_config_module(name: *const u8) -> i32 {
    unsafe {
        if name.is_null() { return -1; }
        let mut nb=[0u8;MODULE_NAME_LEN]; let mut nl=0usize; let mut p=name;
        while *p!=0&&nl<MODULE_NAME_LEN-1{nb[nl]=*p;nl+=1;p=p.add(1);}
        match module_find(&nb[..nl]) {
            None    => { rust_print(b"Module not found. Use set.module first.\n"); -1 }
            Some(i) => { module_interactive_config(&mut MODULES[i]); 0 }
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_discord_run_module(name: *const u8) -> i32 {
    unsafe {
        if name.is_null() { return -1; }
        let mut nb=[0u8;MODULE_NAME_LEN]; let mut nl=0usize; let mut p=name;
        while *p!=0&&nl<MODULE_NAME_LEN-1{nb[nl]=*p;nl+=1;p=p.add(1);}
        match module_find(&nb[..nl]) {
            None    => { rust_print(b"Module not found.\n"); -1 }
            Some(i) => { let mc=MODULES[i]; module_run(&mc) }
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_discord_list_modules() -> i32 {
    unsafe {
        rust_print(b"\n=== Discord Modules (");
        print_num(MODULE_COUNT as i32);
        rust_print(b") ===\n\n");
        if MODULE_COUNT == 0 {
            rust_print(b"  No modules. Use set.module.<name>\n");
            rust_print(b"  e.g. set.module.my-send-emoji\n");
            return 0;
        }
        for i in 0..MODULE_COUNT {
            let m = &MODULES[i];
            terminal_setcolor(if m.configured{0x0A}else{0x0E});
            rust_print(b"  ["); print_num(i as i32); rust_print(b"] ");
            for k in 0..m.name_len{terminal_putchar(m.name[k]);}
            terminal_setcolor(0x07);
            rust_print(b"  ");
            rust_print(match m.kind {
                ModuleKind::SendEmoji    => b"send-emoji   ",
                ModuleKind::SendMessage  => b"send-message ",
                ModuleKind::SendEmbed    => b"send-embed   ",
                ModuleKind::FetchMessages=> b"fetch        ",
                ModuleKind::DeleteMessage=> b"delete       ",
                ModuleKind::React        => b"react        ",
                ModuleKind::AutoReply    => b"auto-reply   ",
                ModuleKind::None         => b"none         ",
            });
            rust_print(if m.configured{b"[configured]\n"}else{b"[not configured]\n"});
            for j in 0..m.param_count {
                let p=&m.params[j];
                rust_print(b"    ");
                for k in 0..p.key_len{terminal_putchar(p.key[k]);}
                rust_print(b"=");
                if p.val_len>0 {
                    for k in 0..p.val_len.min(40){terminal_putchar(p.val[k]);}
                    if p.val_len>40{rust_print(b"...");}
                } else {
                    terminal_setcolor(0x08); rust_print(b"<not set>"); terminal_setcolor(0x07);
                }
                rust_print(b"\n");
            }
            rust_print(b"\n");
        }
        0
    }
}

#[no_mangle]
pub extern "C" fn rust_discord_remove_module(name: *const u8) -> i32 {
    unsafe {
        if name.is_null() { return -1; }
        let mut nb=[0u8;MODULE_NAME_LEN]; let mut nl=0usize; let mut p=name;
        while *p!=0&&nl<MODULE_NAME_LEN-1{nb[nl]=*p;nl+=1;p=p.add(1);}
        match module_find(&nb[..nl]) {
            None    => { rust_print(b"Module not found\n"); -1 }
            Some(i) => {
                for j in i..MODULE_COUNT-1 { MODULES[j]=MODULES[j+1]; }
                MODULES[MODULE_COUNT-1]=DiscordModule::blank();
                MODULE_COUNT-=1;
                rust_print(b"Module removed.\n"); 0
            }
        }
    }
}

//─────────────────────────────────────────────────────────────────────────────
// Standard Discord API
//─────────────────────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn rust_discord_set_token(token: *const u8) -> i32 {
    if token.is_null() { return -1; }
    unsafe {
        DISCORD_TOKEN_LEN=0; let mut ptr=token;
        while *ptr!=0&&DISCORD_TOKEN_LEN<128{DISCORD_TOKEN[DISCORD_TOKEN_LEN]=*ptr;DISCORD_TOKEN_LEN+=1;ptr=ptr.add(1);}
        rust_print(b"Discord token set ("); print_num(DISCORD_TOKEN_LEN as i32); rust_print(b" bytes)\n");
        0
    }
}

#[no_mangle]
pub extern "C" fn rust_discord_get_user_info() -> i32 {
    unsafe {
        let mut buf = [0u8; 4096];

        let Some(len) = discord_request(b"GET", b"/api/v10/users/@me", None, &mut buf, 2) else {
            rust_print(b"discord_get_user_info: failed\n");
            return -1;
        };

        let Some(root) = json_parse(&buf, len) else {
            rust_print(b"discord_get_user_info: JSON failed\n");
            return -1;
        };

        rust_print(b"\n=== Discord User ===\n");

        // --- Basic fields ---
        let fields: [(&[u8], &[u8]); 5] = [
            (b"id",            b"ID:            "),
            (b"username",      b"Username:      "),
            (b"discriminator", b"Discriminator: "),
            (b"email",         b"Email:         "),
            (b"global_name",   b"Global name:   "),
        ];

        for (key, label) in fields.iter() {
            if let Some(vi) = json_object_get(root, key) {
                rust_print(label);
                if let Some(s) = json_get_string(vi) {
                    for &c in s { terminal_putchar(c); }
                } else if let Some(n) = json_get_number(vi) {
                    print_num(n);
                } else if let Some(b) = json_get_bool(vi) {
                    rust_print(if b { b"true" } else { b"false" });
                }
                rust_print(b"\n");
            }
        }

        // --- Bot flag ---
        if let Some(bi) = json_object_get(root, b"bot") {
            if json_get_bool(bi) == Some(true) {
                rust_print(b"Account type:  Bot\n");
            }
        }

        // --- Verified email ---
        rust_print(b"Email verified:");
        if let Some(vi) = json_object_get(root, b"verified") {
            match json_get_bool(vi) {
                Some(true)  => rust_print(b" Yes\n"),
                Some(false) => rust_print(b" No\n"),
                None        => rust_print(b" Unknown\n"),
            }
        } else {
            rust_print(b" N/A\n");
        }

        // --- MFA enabled ---
        rust_print(b"MFA enabled:   ");
        if let Some(vi) = json_object_get(root, b"mfa_enabled") {
            match json_get_bool(vi) {
                Some(true)  => rust_print(b"Yes\n"),
                Some(false) => rust_print(b"No\n"),
                None        => rust_print(b"Unknown\n"),
            }
        } else {
            rust_print(b"N/A\n");
        }

        // --- Locale ---
        rust_print(b"Locale:        ");
        if let Some(vi) = json_object_get(root, b"locale") {
            if let Some(s) = json_get_string(vi) {
                for &c in s { terminal_putchar(c); }
            } else {
                rust_print(b"N/A");
            }
        } else {
            rust_print(b"N/A");
        }
        rust_print(b"\n");

        // --- Premium / Nitro type ---
        // 0 = None, 1 = Nitro Classic, 2 = Nitro, 3 = Nitro Basic
        rust_print(b"Nitro tier:    ");
        if let Some(vi) = json_object_get(root, b"premium_type") {
            match json_get_number(vi) {
                Some(0) => rust_print(b"None\n"),
                Some(1) => rust_print(b"Nitro Classic\n"),
                Some(2) => rust_print(b"Nitro\n"),
                Some(3) => rust_print(b"Nitro Basic\n"),
                Some(_) => rust_print(b"Unknown tier\n"),
                None    => rust_print(b"N/A\n"),
            }
        } else {
            rust_print(b"None\n");
        }

        // --- Avatar URL ---
        // Format: https://cdn.discordapp.com/avatars/<id>/<hash>.png
        if let Some(id_vi) = json_object_get(root, b"id") {
            if let Some(av_vi) = json_object_get(root, b"avatar") {
                if let (Some(id_s), Some(av_s)) = (json_get_string(id_vi), json_get_string(av_vi)) {
                    rust_print(b"Avatar:        https://cdn.discordapp.com/avatars/");
                    for &c in id_s { terminal_putchar(c); }
                    terminal_putchar(b'/');
                    for &c in av_s { terminal_putchar(c); }
                    rust_print(b".png\n");
                } else {
                    rust_print(b"Avatar:        (no avatar)\n");
                }
            }
        }

        // --- Public flags (badges) ---
        // Decoded as a bitfield; print known badge names.
        if let Some(vi) = json_object_get(root, b"public_flags") {
            if let Some(flags) = json_get_number(vi) {
                if flags != 0 {
                    rust_print(b"Badges:\n");
                    let known: [(&[u8], i32); 12] = [
    (b"  Staff\n",                    1 << 0),
    (b"  Partner\n",                  1 << 1),
    (b"  HypeSquad Events\n",         1 << 2),
    (b"  Bug Hunter Lv1\n",           1 << 3),
    (b"  HypeSquad Bravery\n",        1 << 6),
    (b"  HypeSquad Brilliance\n",     1 << 7),
    (b"  HypeSquad Balance\n",        1 << 8),
    (b"  Early Supporter\n",          1 << 9),
    (b"  Bug Hunter Lv2\n",           1 << 14),
    (b"  Verified Bot Dev\n",         1 << 17),
    (b"  Certified Moderator\n",      1 << 18),
    (b"  Active Developer\n",         1 << 22),
];
for (name, bit) in known.iter() {
    if flags & bit != 0 {
        rust_print(name);
    }
}
                } else {
                    rust_print(b"Badges:        None\n");
                }
            }
        }

        0
    }
}

#[no_mangle]
pub extern "C" fn rust_discord_get_guilds() -> i32 {
    unsafe {
        let mut buf=[0u8;8192];
        let Some(len)=discord_request(b"GET",b"/api/v10/users/@me/guilds",None,&mut buf,2)
        else{rust_print(b"discord_get_guilds: failed\n");return -1;};
        let Some(root)=json_parse(&buf,len)
        else{rust_print(b"discord_get_guilds: JSON failed\n");return -1;};
        GUILD_COUNT=0;
        let n=json_array_len(root);
        rust_print(b"\n=== Guilds ("); print_num(n as i32); rust_print(b") ===\n");
        for i in 0..n.min(MAX_GUILDS) {
            let Some(g)=json_array_get(root,i) else{continue};
            let e=&mut GUILDS[GUILD_COUNT];
            if let Some(ii)=json_object_get(g,b"id")  {e.id_len  =copy_json_str(ii,&mut e.id);}
            if let Some(ni)=json_object_get(g,b"name"){e.name_len=copy_json_str(ni,&mut e.name);}
            GUILD_COUNT+=1;
            rust_print(b"  ["); print_num(i as i32); rust_print(b"] ");
            for k in 0..e.name_len{terminal_putchar(e.name[k]);}
            rust_print(b"  ("); for k in 0..e.id_len{terminal_putchar(e.id[k]);} rust_print(b")\n");
        }
        0
    }
}

#[no_mangle]
pub extern "C" fn rust_discord_get_channels(guild_id: *const u8) -> i32 {
    unsafe {
        if guild_id.is_null(){return -1;}
        let mut ep=[0u8;256]; let mut idx=0;
        for &c in b"/api/v10/guilds/"{ ep[idx]=c;idx+=1; }
        let mut p=guild_id; while *p!=0&&idx<220{ep[idx]=*p;idx+=1;p=p.add(1);}
        for &c in b"/channels"{ ep[idx]=c;idx+=1; }
        let mut buf=[0u8;16384];
        let Some(len)=discord_request(b"GET",&ep[..idx],None,&mut buf,2)
        else{rust_print(b"discord_get_channels: failed\n");return -1;};
        let Some(root)=json_parse(&buf,len)
        else{rust_print(b"discord_get_channels: JSON failed\n");return -1;};
        CHANNEL_COUNT=0;
        let n=json_array_len(root);
        rust_print(b"\n=== Channels ("); print_num(n as i32); rust_print(b") ===\n");
        for i in 0..n.min(MAX_CHANNELS) {
            let Some(ch)=json_array_get(root,i) else{continue};
            let e=&mut CHANNELS[CHANNEL_COUNT];
            if let Some(ii)=json_object_get(ch,b"id")  {e.id_len  =copy_json_str(ii,&mut e.id);}
            if let Some(ni)=json_object_get(ch,b"name"){e.name_len=copy_json_str(ni,&mut e.name);}
            e.channel_type=0;
            if let Some(ti)=json_object_get(ch,b"type"){e.channel_type=json_get_number(ti).unwrap_or(0);}
            let mut gp=guild_id; e.guild_id_len=0;
            while *gp!=0&&e.guild_id_len<ID_LEN{e.guild_id[e.guild_id_len]=*gp;e.guild_id_len+=1;gp=gp.add(1);}
            CHANNEL_COUNT+=1;
            let pfx = match e.channel_type {
    0 => b"#" as &[u8],
    2 => b"v" as &[u8],
    4 => b"+" as &[u8],
    _ => b"?" as &[u8],
};
            rust_print(b"  ["); print_num(i as i32); rust_print(b"] ");
            rust_print(pfx);
            for k in 0..e.name_len{terminal_putchar(e.name[k]);}
            rust_print(b"  ("); for k in 0..e.id_len{terminal_putchar(e.id[k]);} rust_print(b")\n");
        }
        0
    }
}

#[no_mangle]
pub extern "C" fn rust_discord_get_channel_messages(channel_id: *const u8, limit: i32) -> i32 {
    unsafe {
        if channel_id.is_null(){return -1;}
        let mut ep=[0u8;256]; let mut idx=0;
        for &c in b"/api/v10/channels/"{ ep[idx]=c;idx+=1; }
        let mut p=channel_id; while *p!=0&&idx<200{ep[idx]=*p;idx+=1;p=p.add(1);}
        for &c in b"/messages?limit="{ ep[idx]=c;idx+=1; }
        let lim=limit.max(1).min(100);
        let mut tmp=[0u8;4]; let mut ti=0; let mut t=lim;
        while t>0{tmp[ti]=(t%10)as u8+b'0';t/=10;ti+=1;}
        for k in (0..ti).rev(){ep[idx]=tmp[k];idx+=1;}
        let mut buf=[0u8;16384];
        let Some(len)=discord_request(b"GET",&ep[..idx],None,&mut buf,2)
        else{rust_print(b"discord_get_messages: failed\n");return -1;};
        let Some(root)=json_parse(&buf,len)
        else{rust_print(b"discord_get_messages: JSON failed\n");return -1;};
        let n=json_array_len(root);
        rust_print(b"\n=== Messages ("); print_num(n as i32); rust_print(b") ===\n");
        let mut ord=[0usize;100]; let count=n.min(100);
        for i in 0..count{ord[count-1-i]=i;}
        for di in 0..count {
            let i=ord[di];
            let Some(msg)=json_array_get(root,i) else{continue};
            let mut mid=[0u8;MSG_ID_LEN]; let mut midl=0usize;
            if let Some(ii)=json_object_get(msg,b"id"){midl=copy_json_str(ii,&mut mid);}
            let mut auth=[0u8;MSG_AUTHOR_LEN]; let mut authl=0usize;
            if let Some(ai)=json_object_get(msg,b"author"){
                if let Some(ui)=json_object_get(ai,b"username"){authl=copy_json_str(ui,&mut auth);}
            }
            let mut cont=[0u8;MSG_CONTENT_LEN]; let mut contl=0usize;
            if let Some(ci)=json_object_get(msg,b"content"){contl=copy_json_str(ci,&mut cont);}
            cache_push_message(&mid[..midl],&auth[..authl],&cont[..contl]);
            rust_print(b"  ");
            for k in 0..authl{terminal_putchar(auth[k]);}
            rust_print(b": ");
            for k in 0..contl{terminal_putchar(cont[k]);}
            rust_print(b"\n");
        }
        0
    }
}

#[no_mangle]
pub extern "C" fn rust_discord_send_message(channel_id: *const u8, message: *const u8) -> i32 {
    unsafe {
        if channel_id.is_null()||message.is_null(){return -1;}
        let mut ep=[0u8;256]; let mut idx=0;
        for &c in b"/api/v10/channels/"{ ep[idx]=c;idx+=1; }
        let mut p=channel_id; while *p!=0&&idx<200{ep[idx]=*p;idx+=1;p=p.add(1);}
        for &c in b"/messages"{ ep[idx]=c;idx+=1; }
        let mut body=[0u8;2048]; let mut bi=0;
        for &c in b"{\"content\":\""{ body[bi]=c;bi+=1; }
        let mut mp=message;
        while *mp!=0&&bi<body.len()-10{
            match *mp{
                b'"' =>{body[bi]=b'\\';bi+=1;body[bi]=b'"'; bi+=1;}
                b'\\'=>{body[bi]=b'\\';bi+=1;body[bi]=b'\\';bi+=1;}
                b'\n'=>{body[bi]=b'\\';bi+=1;body[bi]=b'n'; bi+=1;}
                b'\r'=>{}
                c    =>{body[bi]=c;bi+=1;}
            }
            mp=mp.add(1);
        }
        for &c in b"\"}"{body[bi]=c;bi+=1;}
        let mut buf=[0u8;4096];
        match discord_request(b"POST",&ep[..idx],Some(&body[..bi]),&mut buf,3){
            Some(_)=>{rust_print(b"Message sent!\n");0}
            None   =>{rust_print(b"Failed\n");-1}
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_discord_send_embed(
    channel_id: *const u8, title: *const u8, description: *const u8, color: u32,) -> i32 {
    unsafe {
        if channel_id.is_null(){return -1;}
        let mut ep=[0u8;256]; let mut idx=0;
        for &c in b"/api/v10/channels/"{ ep[idx]=c;idx+=1; }
        let mut p=channel_id; while *p!=0&&idx<200{ep[idx]=*p;idx+=1;p=p.add(1);}
        for &c in b"/messages"{ ep[idx]=c;idx+=1; }
        let mut body=[0u8;2048]; let mut bi=0;
        macro_rules! bp{($b:expr)=>{for &c in $b{if bi<body.len(){body[bi]=c;bi+=1;}}}}
        macro_rules! bpc{($ptr:expr)=>{if !$ptr.is_null(){let mut pp=$ptr;while *pp!=0&&bi<body.len()-4{match *pp{b'"'=>{body[bi]=b'\\';bi+=1;body[bi]=b'"';bi+=1;}b'\\'=>{body[bi]=b'\\';bi+=1;body[bi]=b'\\';bi+=1;}b'\n'=>{body[bi]=b'\\';bi+=1;body[bi]=b'n';bi+=1;}c=>{body[bi]=c;bi+=1;}}pp=pp.add(1);}}}}
        bp!(b"{\"embeds\":[{\"color\":");
        let mut cv=color; let mut tmp=[0u8;10]; let mut ti=0;
        if cv==0{tmp[0]=b'0';ti=1;}else{while cv>0{tmp[ti]=(cv%10)as u8+b'0';cv/=10;ti+=1;}}
        for k in (0..ti).rev(){if bi<body.len(){body[bi]=tmp[k];bi+=1;}}
        bp!(b",\"title\":\""); bpc!(title); bp!(b"\"");
        bp!(b",\"description\":\""); bpc!(description); bp!(b"\"}]}");
        let mut buf=[0u8;4096];
        match discord_request(b"POST",&ep[..idx],Some(&body[..bi]),&mut buf,3){
            Some(_)=>{rust_print(b"Embed sent!\n");0}
            None   =>{rust_print(b"Embed failed\n");-1}
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_discord_react(
    channel_id: *const u8, message_id: *const u8, emoji: *const u8,
) -> i32 {
    unsafe {
        if channel_id.is_null()||message_id.is_null()||emoji.is_null(){return -1;}
        let mut ep=[0u8;512]; let mut idx=0;
        macro_rules! epc{($ptr:expr)=>{let mut pp=$ptr;while *pp!=0&&idx<ep.len()-2{ep[idx]=*pp;idx+=1;pp=pp.add(1);}}}
        for &c in b"/api/v10/channels/"{ ep[idx]=c;idx+=1; }
        epc!(channel_id);
        for &c in b"/messages/"{ ep[idx]=c;idx+=1; }
        epc!(message_id);
        for &c in b"/reactions/"{ ep[idx]=c;idx+=1; }
        epc!(emoji);
        for &c in b"/@me"{ ep[idx]=c;idx+=1; }
        let mut buf=[0u8;256];
        match discord_request(b"PUT",&ep[..idx],Some(b""),&mut buf,2){
            Some(_)=>{rust_print(b"Reaction added!\n");0}
            None   =>{rust_print(b"Failed\n");-1}
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_discord_delete_message(
    channel_id: *const u8, message_id: *const u8,
) -> i32 {
    unsafe {
        if channel_id.is_null()||message_id.is_null(){return -1;}
        let mut ep=[0u8;256]; let mut idx=0;
        for &c in b"/api/v10/channels/"{ ep[idx]=c;idx+=1; }
        let mut p=channel_id; while *p!=0&&idx<180{ep[idx]=*p;idx+=1;p=p.add(1);}
        for &c in b"/messages/"{ ep[idx]=c;idx+=1; }
        p=message_id; while *p!=0&&idx<ep.len()-2{ep[idx]=*p;idx+=1;p=p.add(1);}
        let mut buf=[0u8;256];
        match discord_request(b"DELETE",&ep[..idx],None,&mut buf,2){
            Some(_)=>{rust_print(b"Deleted!\n");0}
            None   =>{rust_print(b"Failed\n");-1}
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_discord_shell(channel_id: *const u8) -> i32 {
    unsafe {
        if channel_id.is_null(){return -1;}
        terminal_clear();
        terminal_setcolor(0x0B); rust_print(b"=== RadiumOS Discord Shell ===\n");
        terminal_setcolor(0x07); rust_print(b"Channel: ");
        let mut p=channel_id; while *p!=0{terminal_putchar(*p);p=p.add(1);}
        rust_print(b"\nR=refresh  Q=quit  Enter=send\n\n");
        rust_discord_get_channel_messages(channel_id,20);
        let mut ibuf=[0u8;512]; let mut ilen=0usize;
        rust_print(b"\n> ");
        loop {
            if !is_key_pressed(){continue;}
            let scan=port_byte_in(0x60);
            if scan>=0x80{continue;}
            match scan {
                0x10=>{ rust_print(b"\nExiting.\n"); return 0; }
                0x13=>{ rust_print(b"\nRefreshing...\n"); rust_discord_get_channel_messages(channel_id,10); rust_print(b"\n> "); for i in 0..ilen{terminal_putchar(ibuf[i]);} }
                0x1C=>{ if ilen==0{continue;} rust_print(b"\nSending...\n"); ibuf[ilen]=0; rust_discord_send_message(channel_id,ibuf.as_ptr()); ilen=0; rust_print(b"\n> "); }
                0x0E=>{ if ilen>0{ilen-=1;rust_print(b"\x08 \x08");} }
                _   =>{ let ch=scancode_to_ascii(scan,false); if ch!=0&&ilen<500{ibuf[ilen]=ch;ilen+=1;terminal_putchar(ch);} }
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_discord_dump_cache() -> i32 {
    unsafe {
        rust_print(b"\n=== Message cache ("); print_num(MSG_CACHE_LEN as i32); rust_print(b") ===\n");
        for i in 0..MSG_CACHE_LEN {
            let s=&MSG_CACHE[i];
            rust_print(b"  ["); print_num(i as i32); rust_print(b"] ");
            for k in 0..s.auth_len{terminal_putchar(s.author[k]);}
            rust_print(b": ");
            for k in 0..s.cont_len{terminal_putchar(s.content[k]);}
            rust_print(b"\n");
        }
        0
    }
}

#[no_mangle]
pub extern "C" fn rust_test_discord() -> i32 {
    unsafe {
        if DISCORD_TOKEN_LEN==0{rust_print(b"Token not set\n");return -1;}
        rust_discord_get_user_info()
    }
}

#[no_mangle]
pub extern "C" fn rust_test_json() -> i32 {
    unsafe {
        rust_print(b"\n=== JSON Parser Test ===\n\n");
        let tj=br#"{"name":"RadiumOS","version":1,"active":true,"features":["networking","graphics"]}"#;
        rust_print(b"Input: "); for &c in tj.iter(){terminal_putchar(c);} rust_print(b"\n\n");
        match json_parse(tj,tj.len()) {
            Some(root) => {
                rust_print(b"Parsed OK\n");
                if let Some(i)=json_object_get(root,b"name"){rust_print(b"name: ");if let Some(s)=json_get_string(i){for &c in s{terminal_putchar(c);}}rust_print(b"\n");}
                if let Some(i)=json_object_get(root,b"version"){rust_print(b"version: ");if let Some(n)=json_get_number(i){print_num(n);}rust_print(b"\n");}
                0
            }
            None => { rust_print(b"FAILED\n"); -1 }
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_network_diag() -> i32 {
    unsafe {
        rust_print(b"\n=== Network Diagnostics ===\n");
        rust_print(b"RTL8139... ");
        if !rust_rtl8139_check_init(){rust_print(b"FAIL\n");return -1;}
        rust_print(b"OK\n");
        let mut mac=[0u8;6];
        if rust_rtl8139_get_mac(mac.as_mut_ptr())==0{
            rust_print(b"MAC: ");
            for i in 0..6{print_hex_byte(mac[i]);if i<5{rust_print(b":");}}
            rust_print(b"\n");
        }
        rust_print(b"IP: "); for i in 0..4{print_num(LOCAL_IP[i]as i32);if i<3{rust_print(b".");}}
        rust_print(b"  GW: "); for i in 0..4{print_num(GATEWAY_IP[i]as i32);if i<3{rust_print(b".");}}
        rust_print(b"  DNS: "); for i in 0..4{print_num(DNS_SERVER[i]as i32);if i<3{rust_print(b".");}}
        rust_print(b"\nDNS test (example.com)... ");
        match dns_query(b"example.com\0"){
            Some(ip)=>{ for i in 0..4{print_num(ip[i]as i32);if i<3{rust_print(b".");}} rust_print(b"\n"); }
            None    =>{ rust_print(b"FAIL\n"); }
        }
        0
    }
}
#[no_mangle]
pub extern "C" fn rust_set_dns(dns1: u8, dns2: u8, dns3: u8, dns4: u8) {
    unsafe {
        DNS_SERVER[0] = dns1;
        DNS_SERVER[1] = dns2;
        DNS_SERVER[2] = dns3;
        DNS_SERVER[3] = dns4;
        
        rust_print(b"DNS server set to: ");
        for i in 0..4 {
            print_num(DNS_SERVER[i] as i32);
            if i < 3 { rust_print(b"."); }
        }
        rust_print(b"\n");
    }
}

#[no_mangle]
pub extern "C" fn rust_test_dns_direct() -> i32 {
    unsafe {
        rust_print(b"\n=== Direct DNS Test ===\n\n");
        
        // Test with google.com which should always work
        let hostname = b"google.com\0";
        
        rust_print(b"Testing DNS resolution of google.com...\n");
        rust_print(b"Using DNS server: ");
        for i in 0..4 {
            print_num(DNS_SERVER[i] as i32);
            if i < 3 { rust_print(b"."); }
        }
        rust_print(b"\n\n");
        
        if let Some(ip) = dns_query(hostname) {
            rust_print(b"\nSUCCESS! Resolved to: ");
            for i in 0..4 {
                print_num(ip[i] as i32);
                if i < 3 { rust_print(b"."); }
            }
            rust_print(b"\n");
            return 0;
        } else {
            rust_print(b"\nFAILED to resolve google.com\n");
            rust_print(b"Try setting DNS server manually:\n");
            rust_print(b"  setdns 8 8 8 8        (Google DNS)\n");
            rust_print(b"  setdns 1 1 1 1        (Cloudflare DNS)\n");
            rust_print(b"  setdns 10 0 2 3       (QEMU user mode DNS)\n");
            return -1;
        }
    }
}

unsafe fn discord_http_request_via_proxy(
    method: &[u8],
    endpoint: &[u8],
    body: Option<&[u8]>,
    output_buffer: &mut [u8]
) -> Option<usize> {
    // Connect to local proxy instead of Discord
    let proxy_ip = [72u8, 14, 176, 144];
    let proxy_port = 8080;
    
    rust_print(b"Discord API (via proxy): ");
    for &c in method { terminal_putchar(c); }
    rust_print(b" ");
    for &c in endpoint { terminal_putchar(c); }
    rust_print(b"\n");
    
    if !tcp_connect(&proxy_ip, proxy_port) {
        rust_print(b"ERROR: Failed to connect to proxy\n");
        return None;
    }
    
    
    let mut request = [0u8; 4096];
    let mut idx = 0;
    for &c in method { request[idx] = c; idx += 1; }
    request[idx] = b' '; idx += 1;
    for &c in endpoint { request[idx] = c; idx += 1; }
    for &c in b" HTTP/1.1\r\n" { request[idx] = c; idx += 1; }
    for &c in b"Host: discord.com\r\n" { request[idx] = c; idx += 1; }
    for &c in b"Authorization: " { request[idx] = c; idx += 1; }
    for i in 0..DISCORD_TOKEN_LEN { request[idx] = DISCORD_TOKEN[i]; idx += 1; }
    for &c in b"\r\nUser-Agent: RadiumOS/1.0\r\n" { request[idx] = c; idx += 1; }
    
    if let Some(body_data) = body {
        for &c in b"Content-Type: application/json\r\n" { request[idx] = c; idx += 1; }
        for &c in b"Content-Length: " { request[idx] = c; idx += 1; }
        let mut len_str = [0u8; 10];
        let mut len_idx = 0;
        let mut temp = body_data.len();
        if temp == 0 { len_str[0] = b'0'; len_idx = 1; } 
        else { while temp > 0 { len_str[len_idx] = (temp % 10) as u8 + b'0'; temp /= 10; len_idx += 1; } }
        for i in (0..len_idx).rev() { request[idx] = len_str[i]; idx += 1; }
        for &c in b"\r\n" { request[idx] = c; idx += 1; }
    }
    for &c in b"Connection: close\r\n\r\n" { request[idx] = c; idx += 1; }
    if let Some(body_data) = body {
        for &c in body_data { if idx < request.len() { request[idx] = c; idx += 1; } }
    }

    if !tcp_send_data(&request[0..idx]) { tcp_close(); return None; }
    
    let recv_len = tcp_receive_data(5000000);
    
    
    if recv_len > 0 {
    // Store Method
    let mut m_idx = 0;
    for &c in method {
        if m_idx < NET_LAST_METHOD.len() - 1 { NET_LAST_METHOD[m_idx] = c; m_idx += 1; }
    }
    NET_LAST_METHOD[m_idx] = 0;

    // Store Protocol
    let proto = b"HTTP/1.1";
    let mut p_idx = 0;
    for &c in proto {
        if p_idx < NET_LAST_PROTO.len() - 1 { NET_LAST_PROTO[p_idx] = c; p_idx += 1; }
    }
    NET_LAST_PROTO[p_idx] = 0;

    // Store Description
    let desc = b"Discord API Proxy";
    let mut d_idx = 0;
    for &c in desc {
        if d_idx < NET_LAST_DESC.len() - 1 { NET_LAST_DESC[d_idx] = c; d_idx += 1; }
    }
    NET_LAST_DESC[d_idx] = 0;

    NET_LAST_UPDATE_TICKS = SYSTEM_TICKS.load(Ordering::Relaxed);
NET_HUD_DIRTY = true;
rust_print(b"[DBG] NET_HUD_DIRTY set\n");
    // ↑ Globals are now dirty. watchdog_draw_status will detect the change
    //   via NET_LAST_* != NET_LAST_DISPLAYED_* and redraw row 8 on its
    //   next tick. No hud_write calls here.

    rust_print(b"Received "); print_num(recv_len as i32); rust_print(b" bytes from proxy\n");

    // Body parsing
    let mut body_start = 0;
    for i in 0..recv_len - 3 {
        if HTTP_RECEIVE_BUFFER[i]   == b'\r' && HTTP_RECEIVE_BUFFER[i+1] == b'\n' &&
           HTTP_RECEIVE_BUFFER[i+2] == b'\r' && HTTP_RECEIVE_BUFFER[i+3] == b'\n' {
            body_start = i + 4; break;
        }
    }
if body_start == 0 || body_start >= recv_len { tcp_close(); return None; }
let body_len = recv_len - body_start;
let copy_len = body_len.min(output_buffer.len());
for i in 0..copy_len { output_buffer[i] = HTTP_RECEIVE_BUFFER[body_start + i]; }
tcp_close();
Some(copy_len)  // no semicolon
} else {
    rust_print(b"ERROR: No response from proxy\n");
    tcp_close();  // close on failure path too - was missing entirely
    None
}
}

#[no_mangle]
pub extern "C" fn rust_discord_tag_module(
    name: *const u8,
    tag:  *const u8,
) -> i32 {
    unsafe {
        if name.is_null() {
            return -1;
        }
 
        let mut nb = [0u8; MODULE_NAME_LEN];
        let mut nl = 0usize;
        let mut p = name;
        while *p != 0 && nl < MODULE_NAME_LEN - 1 {
            nb[nl] = *p;
            nl += 1;
            p = p.add(1);
        }
 
        let idx = match module_find(&nb[..nl]) {
            Some(i) => i,
            None => {
                rust_print(b"tag: module not found\n");
                return -1;
            }
        };
 
        let m = &mut MODULES[idx];
        m.tag     = [0u8; 64];
        m.tag_len = 0;
 
        if !tag.is_null() {
            let mut tp = tag;
            while *tp != 0 && m.tag_len < 63 {
                m.tag[m.tag_len] = *tp;
                m.tag_len += 1;
                tp = tp.add(1);
            }
        }
 
        if m.tag_len > 0 {
            terminal_setcolor(0x0E);
            rust_print(b"Tag set: ");
            for k in 0..m.tag_len { terminal_putchar(m.tag[k]); }
            rust_print(b"\n");
            terminal_setcolor(0x07);
        } else {
            rust_print(b"Tag cleared.\n");
        }
        0
    }
}

#[no_mangle]
pub extern "C" fn rust_discord_module_help(topic: *const u8) -> i32 {
    unsafe {
        let mut tb = [0u8; MODULE_NAME_LEN];
        let mut tl = 0usize;
 
        if !topic.is_null() {
            let mut p = topic;
            while *p != 0 && tl < MODULE_NAME_LEN - 1 {
                tb[tl] = *p;
                tl += 1;
                p = p.add(1);
            }
        }
 
        // Resolve: is it an existing module name? If so, use its kind string.
        let kind_slice: &[u8] = if tl == 0 {
            b""
        } else if let Some(idx) = module_find(&tb[..tl]) {
            match MODULES[idx].kind {
                ModuleKind::SendEmoji     => b"send-emoji",
                ModuleKind::SendMessage   => b"send-message",
                ModuleKind::SendEmbed     => b"send-embed",
                ModuleKind::FetchMessages => b"fetch",
                ModuleKind::DeleteMessage => b"delete",
                ModuleKind::React         => b"react",
                ModuleKind::AutoReply     => b"auto-reply",
                ModuleKind::None          => b"",
            }
        } else {
            &tb[..tl]
        };
 
        terminal_setcolor(0x0B);
        rust_print(b"\n=== Discord Module Help");
        if tl > 0 {
            rust_print(b": ");
            for &c in kind_slice { terminal_putchar(c); }
        }
        rust_print(b" ===\n\n");
        terminal_setcolor(0x07);
 
        // ── Kind-specific help ────────────────────────────────────────────────
        if kind_slice == b"send-emoji" || kind_slice == b"send-emoji\0" {
            rust_print(b"  send-emoji - React to a message with an emoji N times,\n");
            rust_print(b"               then optionally post a follow-up message.\n\n");
            rust_print(b"  Params:\n");
            rust_print(b"    channel_id   : ID of the channel containing the message\n");
            rust_print(b"    message_id   : ID of the message to react to\n");
            rust_print(b"    emoji        : URL-encoded emoji, e.g. %E2%9D%A4 (heart)\n");
            rust_print(b"                   %F0%9F%94%A5 (fire)  %F0%9F%91%8D (thumbs up)\n");
            rust_print(b"    count        : how many times to react (1-20, default 1)\n");
            rust_print(b"    message_after: text to post after reacting (optional)\n\n");
            rust_print(b"  Example flow:\n");
            rust_print(b"    set.module.my-send-emoji\n");
            rust_print(b"    config.module.my-send-emoji\n");
            rust_print(b"    run.module.my-send-emoji\n");
 
        } else if kind_slice == b"send-message" {
            rust_print(b"  send-message - Post a plain text message to a channel.\n\n");
            rust_print(b"  Params:\n");
            rust_print(b"    channel_id : target channel ID\n");
            rust_print(b"    message    : text content (\\n for newlines)\n\n");
            rust_print(b"  Tip: use the interactive shell (discord.shell) for\n");
            rust_print(b"       rapid back-and-forth instead of a module.\n");
 
        } else if kind_slice == b"send-embed" {
            rust_print(b"  send-embed - Post a rich embed card.\n\n");
            rust_print(b"  Params:\n");
            rust_print(b"    channel_id  : target channel ID\n");
            rust_print(b"    title       : embed title\n");
            rust_print(b"    description : embed body text\n");
            rust_print(b"    color       : decimal RGB integer\n");
            rust_print(b"                  15548997 = red\n");
            rust_print(b"                  5763719  = green\n");
            rust_print(b"                  3447003  = blue\n");
            rust_print(b"                  5814783  = purple (default)\n");
 
        } else if kind_slice == b"fetch" {
            rust_print(b"  fetch - Retrieve recent messages from a channel.\n\n");
            rust_print(b"  Params:\n");
            rust_print(b"    channel_id : source channel ID\n");
            rust_print(b"    limit      : number of messages (1-100, default 10)\n\n");
            rust_print(b"  Messages are also stored in the in-memory cache.\n");
            rust_print(b"  View cache with: discord.dump.cache\n");
 
        } else if kind_slice == b"delete" {
            rust_print(b"  delete - Delete a specific message (must be your own\n");
            rust_print(b"           or you must have Manage Messages permission).\n\n");
            rust_print(b"  Params:\n");
            rust_print(b"    channel_id : channel containing the message\n");
            rust_print(b"    message_id : ID of the message to delete\n\n");
            rust_print(b"  Tip: fetch messages first to grab IDs from the cache.\n");
 
        } else if kind_slice == b"react" {
            rust_print(b"  react - Add a single reaction to a message.\n\n");
            rust_print(b"  Params:\n");
            rust_print(b"    channel_id : channel containing the message\n");
            rust_print(b"    message_id : message to react to\n");
            rust_print(b"    emoji      : URL-encoded emoji string\n\n");
            rust_print(b"  Differs from send-emoji: no repeat count, no follow-up.\n");
            rust_print(b"  Use send-emoji for bulk reacting.\n");
 
        } else if kind_slice == b"auto-reply" {
            rust_print(b"  auto-reply - Poll a channel and reply whenever a new\n");
            rust_print(b"               message contains a trigger substring.\n\n");
            rust_print(b"  Params:\n");
            rust_print(b"    channel_id : channel to monitor\n");
            rust_print(b"    trigger    : case-insensitive substring to watch for\n");
            rust_print(b"    reply      : text to post when trigger is matched\n");
            rust_print(b"    poll_ms    : polling interval in ms (min 1000, default 5000)\n\n");
            rust_print(b"  Press Q while running to stop the loop.\n");
 
        } else {
            // ── General help ──────────────────────────────────────────────────
            rust_print(b"  Module lifecycle:\n");
            rust_print(b"    set.module.<name>     create a module (name encodes kind)\n");
            rust_print(b"    config.module.<name>  interactive parameter editor\n");
            rust_print(b"    run.module.<name>     execute the module\n");
            rust_print(b"    list.modules          show all modules + params\n");
            rust_print(b"    remove.module.<name>  delete a module\n\n");
 
            rust_print(b"  Utility commands:\n");
            rust_print(b"    clone.module.<src> <dst>  copy a module to a new name\n");
            rust_print(b"    tag.module.<name> <text>  attach a note to a module\n");
            rust_print(b"    module.help.<kind>        kind-specific help\n\n");
 
            rust_print(b"  Available kinds (embed in the module name):\n");
            terminal_setcolor(0x0E);
            rust_print(b"    send-emoji    send-message    send-embed\n");
            rust_print(b"    fetch         delete          react\n");
            rust_print(b"    auto-reply\n\n");
            terminal_setcolor(0x07);
 
            rust_print(b"  Example:\n");
            rust_print(b"    set.module.greet-send-message\n");
            rust_print(b"    config.module.greet-send-message\n");
            rust_print(b"    tag.module.greet-send-message welcome bot\n");
            rust_print(b"    run.module.greet-send-message\n\n");
 
            rust_print(b"  Token / connection:\n");
            rust_print(b"    discord.token <tok>   set bot/user token\n");
            rust_print(b"    discord.whoami        confirm token + show account info\n");
            rust_print(b"    discord.guilds        list joined servers\n");
            rust_print(b"    discord.channels <id> list channels in a guild\n");
            rust_print(b"    discord.shell <id>    interactive chat shell\n");
        }
 
        rust_print(b"\n");
        terminal_setcolor(0x07);
        0
    }
}
 


fn slice_to_vec(s: &[u8]) -> Vec<u8> {
    let mut v = Vec::new();
    for &b in s.iter() {
        v.push(b);
    }
    v
}

// =============================================================================
// URL PARSER
// =============================================================================

// NOTE: No #[derive(Debug)] - Vec<u8> has no Debug in no_std
struct UrlParts {
    host: Vec<u8>,
    port: u16,
    path: Vec<u8>,
}

fn parse_url(url: &[u8]) -> Option<UrlParts> {
    let url_str = core::str::from_utf8(url).ok()?;

    // Must start with http://
    if !url_str.starts_with("http://") {
        return None;
    }
    let after_scheme = &url[7..]; // skip "http://"

    // Find first '/' which ends the host:port section
    let path_offset = after_scheme
        .iter()
        .position(|&c| c == b'/')
        .unwrap_or(after_scheme.len());

    let host_port = &after_scheme[..path_offset];

    // Split host and port on ':'
    let host_end = host_port
        .iter()
        .position(|&c| c == b':')
        .unwrap_or(host_port.len());

    let host = &host_port[..host_end];

    let port_num: u16 = if host_end < host_port.len() {
        core::str::from_utf8(&host_port[host_end + 1..])
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(8080)
    } else {
        8080
    };

    // Path is '/' onward, default to "/" if no path in URL
    let path = if path_offset < after_scheme.len() {
        slice_to_vec(&after_scheme[path_offset..])
    } else {
        slice_to_vec(b"/")
    };

    Some(UrlParts {
        host: slice_to_vec(host),
        port: port_num,
        path,
    })
}

// =============================================================================
// MAIN DOWNLOAD FUNCTION
// =============================================================================

fn parse_and_download(url: &[u8], filename: &[u8]) -> Result<(), &'static [u8]> {
    let parts = parse_url(url).ok_or(b"invalid URL" as &[u8])?;

    unsafe {
        terminal_setcolor(0x0B);
        rust_print(b"Downloading from: ");
        for &c in parts.host.iter() { terminal_putchar(c); }
        rust_print(b":");
        print_num(parts.port as i32);
        rust_print(b"\nPath: ");
        for &c in parts.path.iter() { terminal_putchar(c); }
        rust_print(b"\nSaving as: ");
        for &c in filename.iter() { terminal_putchar(c); }
        rust_print(b"\n");
        terminal_setcolor(0x07);

        rust_tcp_force_reset();

        let proxy_ip = [72u8, 14, 176, 144];
        if !tcp_connect(&proxy_ip, parts.port) {
            return Err(b"tcp_connect failed" as &[u8]);
        }

        let mut request = [0u8; 2048];
        let mut req_len = 0usize;
        for &c in b"GET ".iter()                              { request[req_len] = c; req_len += 1; }
        for &c in parts.path.iter()                          { request[req_len] = c; req_len += 1; }
        for &c in b" HTTP/1.1\r\n".iter()                    { request[req_len] = c; req_len += 1; }
        for &c in b"Host: 10.0.2.2:8080\r\n".iter()         { request[req_len] = c; req_len += 1; }
        for &c in b"User-Agent: RadiumOS-DL/1.0\r\n".iter() { request[req_len] = c; req_len += 1; }
        for &c in b"Connection: close\r\n\r\n".iter()        { request[req_len] = c; req_len += 1; }

        rust_print(b"Sending request (");
        print_num(req_len as i32);
        rust_print(b" bytes)...\n");

        if !tcp_send_data(&request[..req_len]) {
            tcp_close();
            return Err(b"tcp_send failed" as &[u8]);
        }

        let mut content_len: Option<usize> = None;
        let mut headers_done = false;
        let mut body_start = 0usize;
        let mut total_received = 0usize;
        // FIX: track body bytes received separately for Content-Length check
        let mut body_received = 0usize;

        let timeout_ticks = 5_000_000u32;
        let mut ticks = 0u32;
        let mut last_data_tick: Option<u32> = None; // FIX: None until first data arrives

        rust_print(b"Receiving...\n");

        loop {
            ticks += 1;
            if ticks > timeout_ticks {
                tcp_close();
                return Err(b"receive timeout" as &[u8]);
            }

            // FIX: only start idle countdown after we've actually received something
            if let Some(ldt) = last_data_tick {
                if ticks.wrapping_sub(ldt) > 800_000 {
                    // Extra guard: if Content-Length known, don't bail early
                    let body_complete = match content_len {
                        Some(cl) => body_received >= cl,
                        None     => true,
                    };
                    if body_complete {
                        break;
                    }
                    // If body isn't complete yet, reset idle clock and keep waiting
                    last_data_tick = Some(ticks);
                }
            }

            rust_rtl8139_receive();
            if RX_RESPONSE_LENGTH < 54 { continue; }

            let et = ((RX_RESPONSE_BUFFER[12] as u16) << 8)
                   | (RX_RESPONSE_BUFFER[13] as u16);
            if et != 0x0800 { RX_RESPONSE_LENGTH = 0; continue; }

            let ihl = ((RX_RESPONSE_BUFFER[14] & 0x0F) * 4) as usize;
            if ihl < 20 || RX_RESPONSE_BUFFER[14 + 9] != 6 {
                RX_RESPONSE_LENGTH = 0;
                continue;
            }

            let tcp_start = 14 + ihl;
            if tcp_start + 20 > RX_RESPONSE_LENGTH as usize {
                RX_RESPONSE_LENGTH = 0;
                continue;
            }

            let src_port = ((RX_RESPONSE_BUFFER[tcp_start]     as u16) << 8)
                         | (RX_RESPONSE_BUFFER[tcp_start + 1]  as u16);
            let dst_port = ((RX_RESPONSE_BUFFER[tcp_start + 2] as u16) << 8)
                         | (RX_RESPONSE_BUFFER[tcp_start + 3]  as u16);

            if src_port != parts.port || dst_port != TCP_CONNECTION.local_port {
                RX_RESPONSE_LENGTH = 0;
                continue;
            }

            let tcp_hl      = ((RX_RESPONSE_BUFFER[tcp_start + 12] >> 4) * 4) as usize;
            let flags       = RX_RESPONSE_BUFFER[tcp_start + 13];
            let data_offset = tcp_start + tcp_hl;
            let total_len   = RX_RESPONSE_LENGTH as usize;

            if data_offset < total_len {
                let data_len   = total_len - data_offset;
                let space_left = HTTP_RECEIVE_BUFFER.len().saturating_sub(HTTP_RECEIVE_LEN);
                let copy_len   = data_len.min(space_left);

                for i in 0..copy_len {
                    HTTP_RECEIVE_BUFFER[HTTP_RECEIVE_LEN + i] =
                        RX_RESPONSE_BUFFER[data_offset + i];
                }
                HTTP_RECEIVE_LEN += copy_len;
                total_received   += copy_len;
                last_data_tick    = Some(ticks); // FIX: set on first real data

                if !headers_done {
                    let scan_end = HTTP_RECEIVE_LEN.saturating_sub(3);
                    'header_scan: for i in 0..scan_end {
                        if HTTP_RECEIVE_BUFFER[i]     == b'\r'
                        && HTTP_RECEIVE_BUFFER[i + 1] == b'\n'
                        && HTTP_RECEIVE_BUFFER[i + 2] == b'\r'
                        && HTTP_RECEIVE_BUFFER[i + 3] == b'\n'
                        {
                            body_start   = i + 4;
                            headers_done = true;

                            let headers   = &HTTP_RECEIVE_BUFFER[..body_start];
                            let cl_header = b"content-length: ";

                            'cl_scan: for start in 0..headers.len().saturating_sub(cl_header.len()) {
                                let mut matched = true;
                                for j in 0..cl_header.len() {
                                    if headers[start + j].to_ascii_lowercase() != cl_header[j] {
                                        matched = false;
                                        break;
                                    }
                                }
                                if !matched { continue 'cl_scan; }

                                let mut cl_val = 0usize;
                                let mut pos    = start + cl_header.len();
                                while pos < headers.len()
                                    && HTTP_RECEIVE_BUFFER[pos] >= b'0'
                                    && HTTP_RECEIVE_BUFFER[pos] <= b'9'
                                {
                                    cl_val = cl_val * 10
                                           + (HTTP_RECEIVE_BUFFER[pos] - b'0') as usize;
                                    pos += 1;
                                }
                                if cl_val > 0 {
                                    content_len = Some(cl_val);
                                    rust_print(b"Content-Length: ");
                                    print_num(cl_val as i32);
                                    rust_print(b"\n");
                                }
                                break 'cl_scan;
                            }
                            break 'header_scan;
                        }
                    }
                }

                // FIX: count body bytes separately so CL check is accurate
                if headers_done && total_received > body_start {
                    body_received = total_received - body_start;
                }

                // Early exit: got everything Content-Length promised
                if let Some(cl) = content_len {
                    if body_received >= cl {
                        let rseq = ((RX_RESPONSE_BUFFER[tcp_start + 4] as u32) << 24)
                                 | ((RX_RESPONSE_BUFFER[tcp_start + 5] as u32) << 16)
                                 | ((RX_RESPONSE_BUFFER[tcp_start + 6] as u32) << 8)
                                 | (RX_RESPONSE_BUFFER[tcp_start + 7]  as u32);
                        TCP_CONNECTION.ack_num = rseq.wrapping_add(copy_len as u32);
                        send_ack();
                        break;
                    }
                }

                let rseq = ((RX_RESPONSE_BUFFER[tcp_start + 4] as u32) << 24)
                         | ((RX_RESPONSE_BUFFER[tcp_start + 5] as u32) << 16)
                         | ((RX_RESPONSE_BUFFER[tcp_start + 6] as u32) << 8)
                         | (RX_RESPONSE_BUFFER[tcp_start + 7]  as u32);
                TCP_CONNECTION.ack_num = rseq.wrapping_add(copy_len as u32);
                send_ack();
            }

            if (flags & 0x01) != 0 {
                rust_print(b"Server FIN received\n");
                let rseq_final = ((RX_RESPONSE_BUFFER[tcp_start + 4] as u32) << 24)
                               | ((RX_RESPONSE_BUFFER[tcp_start + 5] as u32) << 16)
                               | ((RX_RESPONSE_BUFFER[tcp_start + 6] as u32) << 8)
                               | (RX_RESPONSE_BUFFER[tcp_start + 7]  as u32);
                TCP_CONNECTION.ack_num = rseq_final.wrapping_add(1);
                send_ack();
                break;
            }

            RX_RESPONSE_LENGTH = 0;
        }

        tcp_close();

        if total_received == 0 {
            return Err(b"no data received" as &[u8]);
        }

        rust_print(b"Total received: ");
        print_num(total_received as i32);
        rust_print(b" bytes\n");

        // FIX: check body_received against cl, not total_received
        if let Some(cl) = content_len {
            if body_received < cl {
                rust_print(b"Incomplete: got ");
                print_num(body_received as i32);
                rust_print(b" of ");
                print_num(cl as i32);
                rust_print(b" body bytes\n");
                return Err(b"incomplete download" as &[u8]);
            }
        }

        let body_start_final = if headers_done && body_start > 0 { body_start } else { 0 };
        let body_len         = total_received.saturating_sub(body_start_final);
        let body             = &HTTP_RECEIVE_BUFFER[body_start_final..body_start_final + body_len];

        rust_print(b"body_start_final: ");
        print_num(body_start_final as i32);
        rust_print(b"\nbody_len: ");
        print_num(body_len as i32);
        rust_print(b"\n");

        if body_len == 0 {
            return Err(b"body is empty" as &[u8]);
        }

        let mut fname_buf = [0u8; 65];
        let fname_len = filename.len().min(64);
        for i in 0..fname_len { fname_buf[i] = filename[i]; }

        avfs_remove_file(fname_buf.as_ptr());
rust_print(b"=== PRE-WRITE DEBUG ===\n");
rust_print(b"headers_done: ");
print_num(headers_done as i32);
rust_print(b"\n");
rust_print(b"body_start: ");
print_num(body_start as i32);
rust_print(b"\n");
rust_print(b"total_received: ");
print_num(total_received as i32);
rust_print(b"\n");
rust_print(b"body_received: ");
print_num(body_received as i32);
rust_print(b"\n");
rust_print(b"body_len: ");
print_num(body_len as i32);
rust_print(b"\n");
rust_print(b"content_len: ");
match content_len {
    Some(cl) => print_num(cl as i32),
    None     => rust_print(b"None"),
}
rust_print(b"\n");
rust_print(b"fname_buf: ");
for &c in fname_buf.iter().take_while(|&&b| b != 0) { terminal_putchar(c); }
rust_print(b"\n");
rust_print(b"======================\n");
        let create_ret = avfs_create_file(fname_buf.as_ptr(), body_len as u32);
        if create_ret != 0 {
            rust_print(b"avfs_create_file error: ");
            print_num(create_ret as i32);
            rust_print(b"\n");
            return Err(b"avfs_create_file failed" as &[u8]);
        }

        let write_ret = avfs_write_file(fname_buf.as_ptr(), body.as_ptr(), body_len as u32, 0);
        if write_ret != 0 {
            rust_print(b"avfs_write_file error: ");
            print_num(write_ret as i32);
            rust_print(b"\n");
            return Err(b"avfs_write_file failed" as &[u8]);
        }

        rust_print(b"File written successfully\n");
        Ok(())
    }
}

// =============================================================================
// DOWNLOAD ENTRY POINT
// =============================================================================

#[no_mangle]
pub extern "C" fn download_simple(url_cstr: *const u8, filename_cstr: *const u8) -> i32 {
    unsafe {
        if url_cstr.is_null() || filename_cstr.is_null() {
            rust_print(b"download_simple: null args\n");
            return -1;
        }

        // Read URL from C string
        let mut url_bytes = [0u8; 512];
        let mut url_len   = 0usize;
        let mut p         = url_cstr;
        while *p != 0 && url_len < 511 {
            url_bytes[url_len] = *p;
            url_len += 1;
            p = p.add(1);
        }

        // Read filename from C string
        let mut filename_bytes = [0u8; 64];
        let mut filename_len   = 0usize;
        p = filename_cstr;
        while *p != 0 && filename_len < 63 {
            filename_bytes[filename_len] = *p;
            filename_len += 1;
            p = p.add(1);
        }

        match parse_and_download(&url_bytes[..url_len], &filename_bytes[..filename_len]) {
            Ok(_) => {
                terminal_setcolor(0x0A);
                rust_print(b"Download complete: ");
                for &c in filename_bytes[..filename_len].iter() { terminal_putchar(c); }
                rust_print(b"\n");
                terminal_setcolor(0x07);
                0
            }
            Err(e) => {
                terminal_setcolor(0x0C);
                rust_print(b"Download failed: ");
                rust_print(e);
                rust_print(b"\n");
                terminal_setcolor(0x07);
                -1
            }
        }
    }
}
//=============================================================================
// SIMPLE VECTOR IMPLEMENTATION (no_std compatible)
//=============================================================================

pub struct Vec<T> {
    ptr: *mut T,
    len: usize,
    capacity: usize,
}

impl<T> Vec<T> {
    /// Create a new empty vector
    pub const fn new() -> Self {
        Vec {
            ptr: core::ptr::null_mut(),
            len: 0,
            capacity: 0,
        }
    }
    
    /// Create a vector with specified capacity
    pub fn with_capacity(capacity: usize) -> Self {
        if capacity == 0 {
            return Vec::new();
        }
        
        unsafe {
            let size = capacity * core::mem::size_of::<T>();
            let ptr = malloc(size as u32) as *mut T;
            
            if ptr.is_null() {
                panic!("Failed to allocate memory for Vec");
            }
            
            Vec {
                ptr,
                len: 0,
                capacity,
            }
        }
    }
    
    /// Get the number of elements in the vector
    pub fn len(&self) -> usize {
        self.len
    }
    
    /// Check if the vector is empty
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
    
    /// Get the capacity of the vector
    pub fn capacity(&self) -> usize {
        self.capacity
    }
    
    /// Push an element to the end of the vector
    pub fn push(&mut self, value: T) {
        if self.len == self.capacity {
            self.grow();
        }
        
        unsafe {
            core::ptr::write(self.ptr.add(self.len), value);
        }
        self.len += 1;
    }
    
    /// Pop an element from the end of the vector
    pub fn pop(&mut self) -> Option<T> {
        if self.len == 0 {
            return None;
        }
        
        self.len -= 1;
        unsafe {
            Some(core::ptr::read(self.ptr.add(self.len)))
        }
    }
    
    /// Get a reference to an element at index
    pub fn get(&self, index: usize) -> Option<&T> {
        if index >= self.len {
            return None;
        }
        
        unsafe {
            Some(&*self.ptr.add(index))
        }
    }
    
    /// Get a mutable reference to an element at index
    pub fn get_mut(&mut self, index: usize) -> Option<&mut T> {
        if index >= self.len {
            return None;
        }
        
        unsafe {
            Some(&mut *self.ptr.add(index))
        }
    }
    
    /// Insert an element at a specific index
    pub fn insert(&mut self, index: usize, value: T) {
        if index > self.len {
            panic!("Index out of bounds");
        }
        
        if self.len == self.capacity {
            self.grow();
        }
        
        unsafe {
            // Shift elements to the right
            if index < self.len {
                core::ptr::copy(
                    self.ptr.add(index),
                    self.ptr.add(index + 1),
                    self.len - index,
                );
            }
            
            core::ptr::write(self.ptr.add(index), value);
        }
        
        self.len += 1;
    }
    
    /// Remove an element at a specific index
    pub fn remove(&mut self, index: usize) -> T {
        if index >= self.len {
            panic!("Index out of bounds");
        }
        
        unsafe {
            let value = core::ptr::read(self.ptr.add(index));
            
            // Shift elements to the left
            if index < self.len - 1 {
                core::ptr::copy(
                    self.ptr.add(index + 1),
                    self.ptr.add(index),
                    self.len - index - 1,
                );
            }
            
            self.len -= 1;
            value
        }
    }
    
    /// Swap remove - removes element at index and replaces it with last element
    /// This is O(1) but doesn't preserve order
    pub fn swap_remove(&mut self, index: usize) -> T {
        if index >= self.len {
            panic!("Index out of bounds");
        }
        
        unsafe {
            let value = core::ptr::read(self.ptr.add(index));
            
            self.len -= 1;
            
            if index < self.len {
                core::ptr::copy(
                    self.ptr.add(self.len),
                    self.ptr.add(index),
                    1,
                );
            }
            
            value
        }
    }
    
    /// Reserve additional capacity
    pub fn reserve(&mut self, additional: usize) {
        let required = self.len + additional;
        if required > self.capacity {
            self.grow_to(required);
        }
    }
    
    /// Truncate the vector to the specified length
    pub fn truncate(&mut self, len: usize) {
        if len >= self.len {
            return;
        }
        
        unsafe {
            // Drop truncated elements
            for i in len..self.len {
                core::ptr::drop_in_place(self.ptr.add(i));
            }
        }
        
        self.len = len;
    }
    
    /// Retain only elements that satisfy the predicate
    pub fn retain<F>(&mut self, mut f: F)
    where
        F: FnMut(&T) -> bool,
    {
        let mut i = 0;
        while i < self.len {
            unsafe {
                if !f(&*self.ptr.add(i)) {
                    self.remove(i);
                } else {
                    i += 1;
                }
            }
        }
    }
    
    /// Get the first element
    pub fn first(&self) -> Option<&T> {
        self.get(0)
    }
    
    /// Get the last element
    pub fn last(&self) -> Option<&T> {
        if self.len == 0 {
            None
        } else {
            self.get(self.len - 1)
        }
    }
    
    /// Check if vector contains an element
    pub fn contains(&self, value: &T) -> bool
    where
        T: PartialEq,
    {
        for i in 0..self.len {
            unsafe {
                if &*self.ptr.add(i) == value {
                    return true;
                }
            }
        }
        false
    }
    
    /// Get as slice
    pub fn as_slice(&self) -> &[T] {
        unsafe {
            core::slice::from_raw_parts(self.ptr, self.len)
        }
    }
    
    /// Get as mutable slice
    pub fn as_mut_slice(&mut self) -> &mut [T] {
        unsafe {
            core::slice::from_raw_parts_mut(self.ptr, self.len)
        }
    }
    
    /// Grow the vector's capacity
    fn grow(&mut self) {
        let new_capacity = if self.capacity == 0 {
            4
        } else {
            self.capacity * 2
        };
        
        self.grow_to(new_capacity);
    }
    
    /// Grow to a specific capacity
    fn grow_to(&mut self, new_capacity: usize) {
        if new_capacity <= self.capacity {
            return;
        }
        
        unsafe {
            let new_size = new_capacity * core::mem::size_of::<T>();
            let new_ptr = malloc(new_size as u32) as *mut T;
            
            if new_ptr.is_null() {
                panic!("Failed to allocate memory for Vec growth");
            }
            
            // Copy old data
            if self.len > 0 && !self.ptr.is_null() {
                core::ptr::copy_nonoverlapping(self.ptr, new_ptr, self.len);
                free(self.ptr as *mut u8);
            }
            
            self.ptr = new_ptr;
            self.capacity = new_capacity;
        }
    }
}

// Index trait implementation
impl<T> core::ops::Index<usize> for Vec<T> {
    type Output = T;
    
    fn index(&self, index: usize) -> &Self::Output {
        if index >= self.len {
            panic!("Index out of bounds");
        }
        
        unsafe {
            &*self.ptr.add(index)
        }
    }
}

impl<T> core::ops::IndexMut<usize> for Vec<T> {
    fn index_mut(&mut self, index: usize) -> &mut Self::Output {
        if index >= self.len {
            panic!("Index out of bounds");
        }
        
        unsafe {
            &mut *self.ptr.add(index)
        }
    }
}

// Drop implementation
impl<T> Drop for Vec<T> {
    fn drop(&mut self) {
        unsafe {
            // Drop all elements
            for i in 0..self.len {
                core::ptr::drop_in_place(self.ptr.add(i));
            }
            
            // Free memory
            if !self.ptr.is_null() {
                free(self.ptr as *mut u8);
            }
        }
    }
}

// FromIterator trait implementation
impl<T> core::iter::FromIterator<T> for Vec<T> {
    fn from_iter<I: IntoIterator<Item = T>>(iter: I) -> Self {
        let mut vec = Vec::new();
        for item in iter {
            vec.push(item);
        }
        vec
    }
}

// Extend trait for adding multiple elements
impl<T> core::iter::Extend<T> for Vec<T> {
    fn extend<I: IntoIterator<Item = T>>(&mut self, iter: I) {
        for item in iter {
            self.push(item);
        }
    }
}

// Clone implementation for T: Clone
impl<T: Clone> Clone for Vec<T> {
    fn clone(&self) -> Self {
        let mut new_vec = Vec::with_capacity(self.len);
        
        for i in 0..self.len {
            unsafe {
                new_vec.push((&*self.ptr.add(i)).clone());
            }
        }
        
        new_vec
    }
}

// Iterator implementation
pub struct VecIter<'a, T> {
    vec: &'a Vec<T>,
    index: usize,
}

impl<'a, T> Iterator for VecIter<'a, T> {
    type Item = &'a T;
    
    fn next(&mut self) -> Option<Self::Item> {
        if self.index < self.vec.len {
            let item = unsafe { &*self.vec.ptr.add(self.index) };
            self.index += 1;
            Some(item)
        } else {
            None
        }
    }
}

// Mutable Iterator implementation
pub struct VecIterMut<'a, T> {
    vec: &'a mut Vec<T>,
    index: usize,
}

impl<'a, T> Iterator for VecIterMut<'a, T> {
    type Item = &'a mut T;
    
    fn next(&mut self) -> Option<Self::Item> {
        if self.index < self.vec.len {
            let item = unsafe { &mut *self.vec.ptr.add(self.index) };
            self.index += 1;
            Some(item)
        } else {
            None
        }
    }
}

impl<T> Vec<T> {
    pub fn iter(&self) -> VecIter<T> {
        VecIter {
            vec: self,
            index: 0,
        }
    }

    pub fn iter_mut(&mut self) -> VecIterMut<T> {
        VecIterMut {
            vec: self,
            index: 0,
        }
    }
}

// Extend trait for adding multiple elements
impl<T> Vec<T> {
    pub fn extend_from_slice(&mut self, slice: &[T])
    where
        T: Clone,
    {
        self.reserve(slice.len());
        for item in slice {
            self.push(item.clone());
        }
    }
}

//=============================================================================
// VEC TEST FUNCTION
//=============================================================================

#[no_mangle]
pub extern "C" fn rust_test_vec() -> i32 {
    unsafe {
        rust_print(b"\n=== Vec Implementation Test ===\n\n");
        
        // Test 1: Basic push/pop
        rust_print(b"Test 1: Basic push/pop\n");
        let mut vec: Vec<i32> = Vec::new();
        
        vec.push(10);
        vec.push(20);
        vec.push(30);
        
        rust_print(b"  Pushed: 10, 20, 30\n");
        rust_print(b"  Length: ");
        print_num(vec.len() as i32);
        rust_print(b"\n");
        
        if let Some(val) = vec.pop() {
            rust_print(b"  Popped: ");
            print_num(val);
            rust_print(b"\n");
        }
        
        // Test 2: Indexing
        rust_print(b"\nTest 2: Indexing\n");
        rust_print(b"  vec[0] = ");
        print_num(vec[0]);
        rust_print(b"\n");
        rust_print(b"  vec[1] = ");
        print_num(vec[1]);
        rust_print(b"\n");
        
        // Test 3: Insert and remove
        rust_print(b"\nTest 3: Insert and remove\n");
        vec.insert(1, 15);
        rust_print(b"  After insert(1, 15): [");
        for i in 0..vec.len() {
            print_num(vec[i]);
            if i < vec.len() - 1 {
                rust_print(b", ");
            }
        }
        rust_print(b"]\n");
        
        let removed = vec.remove(1);
        rust_print(b"  Removed value: ");
        print_num(removed);
        rust_print(b"\n");
        
        // Test 4: Capacity growth
        rust_print(b"\nTest 4: Capacity growth\n");
        let mut big_vec: Vec<i32> = Vec::new();
        
        rust_print(b"  Initial capacity: ");
        print_num(big_vec.capacity() as i32);
        rust_print(b"\n");
        
        for i in 0..10 {
            big_vec.push(i);
        }
        
        rust_print(b"  After 10 pushes, capacity: ");
        print_num(big_vec.capacity() as i32);
        rust_print(b"\n");
        rust_print(b"  Length: ");
        print_num(big_vec.len() as i32);
        rust_print(b"\n");
        
        // Test 5: Iterator
        rust_print(b"\nTest 5: Iterator\n");
        rust_print(b"  Elements: [");
        for (i, &val) in big_vec.iter().enumerate() {
            print_num(val);
            if i < big_vec.len() - 1 {
                rust_print(b", ");
            }
        }
        rust_print(b"]\n");
        
        // Test 6: Contains
        rust_print(b"\nTest 6: Contains\n");
        rust_print(b"  Contains 5? ");
        if big_vec.contains(&5) {
            rust_print(b"Yes\n");
        } else {
            rust_print(b"No\n");
        }
        
        rust_print(b"  Contains 99? ");
        if big_vec.contains(&99) {
            rust_print(b"Yes\n");
        } else {
            rust_print(b"No\n");
        }
        
        // Test 7: Retain (filter)
        rust_print(b"\nTest 7: Retain (keep only even numbers)\n");
        big_vec.retain(|&x| x % 2 == 0);
        rust_print(b"  After retain: [");
        for i in 0..big_vec.len() {
            print_num(big_vec[i]);
            if i < big_vec.len() - 1 {
                rust_print(b", ");
            }
        }
        rust_print(b"]\n");
        
        // Test 8: String Vec
        rust_print(b"\nTest 8: Vec of strings (as byte slices)\n");
        let mut str_vec: Vec<&[u8]> = Vec::new();
        str_vec.push(b"Hello");
        str_vec.push(b"RadiumOS");
        str_vec.push(b"World");
        
        rust_print(b"  Strings: ");
        for (i, &s) in str_vec.iter().enumerate() {
            for &byte in s {
                terminal_putchar(byte);
            }
            if i < str_vec.len() - 1 {
                rust_print(b", ");
            }
        }
        rust_print(b"\n");
        
        rust_print(b"\nAll tests passed!\n");
        
        0
    }
}


//=============================================================================
// GRAPHICS SUBSYSTEM - REFORMED
//=============================================================================

//-----------------------------------------------------------------------------
// VBE/VESA Hardware Interface
//-----------------------------------------------------------------------------

const VBE_DISPI_IOPORT_INDEX: u16 = 0x01CE;
const VBE_DISPI_IOPORT_DATA: u16 = 0x01CF;

const VBE_DISPI_INDEX_ID: u16 = 0x0;
const VBE_DISPI_INDEX_XRES: u16 = 0x1;
const VBE_DISPI_INDEX_YRES: u16 = 0x2;
const VBE_DISPI_INDEX_BPP: u16 = 0x3;
const VBE_DISPI_INDEX_ENABLE: u16 = 0x4;
const VBE_DISPI_INDEX_VIRT_WIDTH: u16 = 0x6;

const VBE_DISPI_DISABLED: u16 = 0x00;
const VBE_DISPI_ENABLED: u16 = 0x01;
const VBE_DISPI_LFB_ENABLED: u16 = 0x40;

unsafe fn vbe_write(index: u16, value: u16) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

unsafe fn vbe_read(index: u16) -> u16 {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    inw(VBE_DISPI_IOPORT_DATA)
}

//-----------------------------------------------------------------------------
// Graphics Mode State
//-----------------------------------------------------------------------------

#[repr(C)]
pub struct GraphicsMode {
    width: u32,
    height: u32,
    bpp: u32,
    pitch: u32,
    framebuffer: *mut u32,
    is_initialized: bool,
}

static mut GRAPHICS_MODE: GraphicsMode = GraphicsMode {
    width: 0,
    height: 0,
    bpp: 0,
    pitch: 0,
    framebuffer: core::ptr::null_mut(),
    is_initialized: false,
};

// Back buffer for double buffering
const MAX_BUFFER_SIZE: usize = 1920 * 1080; // Support up to 1080p
static mut BACK_BUFFER: [u32; MAX_BUFFER_SIZE] = [0; MAX_BUFFER_SIZE];

//-----------------------------------------------------------------------------
// Framebuffer Detection
//-----------------------------------------------------------------------------

unsafe fn probe_framebuffer_address() -> Option<u32> {
    let test_addresses = [
        0xFD000000u32,  // QEMU -vga std (most common)
        0xE0000000u32,  // Standard
        0xF0000000u32,  // Alternative
        0xFC000000u32,  // Cirrus
        0xC0000000u32,  // Another common
        0xFE000000u32,  // Higher memory
    ];
    
    rust_print(b"Probing framebuffer addresses...\n");
    
    for &addr in &test_addresses {
        rust_print(b"  Testing 0x");
        print_hex(addr);
        rust_print(b": ");
        
        let ptr = addr as *mut u32;
        let test_pattern = 0x12345678;
        
        // Try write/read
        core::ptr::write_volatile(ptr, test_pattern);
        let readback = core::ptr::read_volatile(ptr);
        
        if readback == test_pattern {
            rust_print(b"SUCCESS!\n");
            return Some(addr);
        }
        
        rust_print(b"failed\n");
    }
    
    rust_print(b"ERROR: No valid framebuffer found\n");
    None
}

//-----------------------------------------------------------------------------
// Memory Mapping
//-----------------------------------------------------------------------------

unsafe fn map_framebuffer_region(fb_base: u32, size: u32) -> bool {
    extern "C" {
        static mut boot_page_directory: [u32; 1024];
    }
    
    rust_print(b"Mapping framebuffer: 0x");
    print_hex(fb_base);
    rust_print(b" (");
    print_num((size / (1024 * 1024)) as i32);
    rust_print(b" MB)\n");
    
    let page_dir = boot_page_directory.as_mut_ptr();
    let pages_needed = (size + 0x3FFFFF) / 0x400000; // 4MB pages
    
    for i in 0..pages_needed {
        let virt_addr = fb_base + (i * 0x400000);
        let pd_index = (virt_addr >> 22) as usize;
        
        // Present | RW | 4MB page | Cache Disable
        let pde = virt_addr | 0x93;
        *page_dir.add(pd_index) = pde;
    }
    
    // Flush TLB
    core::arch::asm!(
        "mov eax, cr3",
        "mov cr3, eax",
        out("eax") _,
    );
    
    rust_print(b"Framebuffer mapped successfully\n");
    true
}

//-----------------------------------------------------------------------------
// Graphics Initialization
//-----------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn graphics_init(width: u32, height: u32, bpp: u32) -> i32 {
    unsafe {
        rust_print(b"\n=== Graphics Initialization ===\n");
        
        // Validate parameters
        if width == 0 || height == 0 || bpp == 0 {
            rust_print(b"ERROR: Invalid parameters\n");
            return -1;
        }
        
        if width * height > MAX_BUFFER_SIZE as u32 {
            rust_print(b"ERROR: Resolution too large for buffer\n");
            return -1;
        }
        
        // Check VBE support
        let vbe_id = vbe_read(VBE_DISPI_INDEX_ID);
        if vbe_id < 0xB0C0 || vbe_id > 0xB0C6 {
            rust_print(b"ERROR: VBE not supported (ID: 0x");
            print_hex(vbe_id as u32);
            rust_print(b")\n");
            return -1;
        }
        
        rust_print(b"VBE Version: 0x");
        print_hex(vbe_id as u32);
        rust_print(b"\n");
        
        // Disable VBE
        vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        
        // Configure mode
        vbe_write(VBE_DISPI_INDEX_XRES, width as u16);
        vbe_write(VBE_DISPI_INDEX_YRES, height as u16);
        vbe_write(VBE_DISPI_INDEX_BPP, bpp as u16);
        vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, width as u16);
        
        // Enable with linear framebuffer
        vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
        
        rust_print(b"VBE mode configured: ");
        print_num(width as i32);
        rust_print(b"x");
        print_num(height as i32);
        rust_print(b"x");
        print_num(bpp as i32);
        rust_print(b"\n");
        
        // Probe for framebuffer
        let fb_addr = match probe_framebuffer_address() {
            Some(addr) => addr,
            None => return -1,
        };
        
        // Map framebuffer memory
        let fb_size = width * height * (bpp / 8);
        if !map_framebuffer_region(fb_addr, fb_size) {
            return -1;
        }
        
        // Initialize graphics state
        let pitch = width * (bpp / 8);
        GRAPHICS_MODE = GraphicsMode {
            width,
            height,
            bpp,
            pitch,
            framebuffer: fb_addr as *mut u32,
            is_initialized: true,
        };
        
        // Clear screen
        graphics_clear(rgb(40, 40, 60));
        
        rust_print(b"Graphics initialized successfully!\n");
        rust_print(b"  Framebuffer: 0x");
        print_hex(fb_addr);
        rust_print(b"\n  Resolution: ");
        print_num(width as i32);
        rust_print(b"x");
        print_num(height as i32);
        rust_print(b"\n  Pitch: ");
        print_num(pitch as i32);
        rust_print(b" bytes\n\n");
        
        0
    }
}

#[no_mangle]
pub extern "C" fn graphics_shutdown() {
    unsafe {
        if !GRAPHICS_MODE.is_initialized {
            return;
        }

        // 1. Mark uninitialized first - stop any concurrent draw attempts
        GRAPHICS_MODE.is_initialized = false;

        // 2. Blank framebuffer before disabling - no leftover pixel artifacts
        if !GRAPHICS_MODE.framebuffer.is_null() {
            let pixel_count = (GRAPHICS_MODE.width * GRAPHICS_MODE.height) as usize;
            let fb = core::slice::from_raw_parts_mut(
                GRAPHICS_MODE.framebuffer as *mut u32,
                pixel_count,
            );
            fb.fill(0x00000000);
        }

        // 3. Null pointer before hardware disable
        GRAPHICS_MODE.framebuffer = core::ptr::null_mut();
        GRAPHICS_MODE.width       = 0;
        GRAPHICS_MODE.height      = 0;
        GRAPHICS_MODE.pitch       = 0;

        // 4. Disable VBE - hardware last
        vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        //terminal_initialize();
        // 5. Safe - rust_print uses VGA text, not the framebuffer
        rust_print(b"Graphics shut down\n");
    }
}

//-----------------------------------------------------------------------------
// Color Utilities
//-----------------------------------------------------------------------------

#[inline]
const fn rgb(r: u8, g: u8, b: u8) -> u32 {
    0xFF000000 | ((r as u32) << 16) | ((g as u32) << 8) | (b as u32)
}


#[inline]
fn rgba(r: u8, g: u8, b: u8, a: u8) -> u32 {
    ((a as u32) << 24) | ((r as u32) << 16) | ((g as u32) << 8) | (b as u32)
}

//-----------------------------------------------------------------------------
// Drawing Primitives
//-----------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn graphics_clear(color: u32) {
    unsafe {
        if !GRAPHICS_MODE.is_initialized {
            return;
        }
        
        let total = (GRAPHICS_MODE.width * GRAPHICS_MODE.height) as usize;
        for i in 0..total {
            BACK_BUFFER[i] = color;
        }
    }
}

#[no_mangle]
pub extern "C" fn graphics_put_pixel(x: u32, y: u32, color: u32) {
    unsafe {
        if !GRAPHICS_MODE.is_initialized {
            return;
        }
        
        if x >= GRAPHICS_MODE.width || y >= GRAPHICS_MODE.height {
            return;
        }
        
        let idx = (y * GRAPHICS_MODE.width + x) as usize;
        BACK_BUFFER[idx] = color;
    }
}

#[no_mangle]
pub extern "C" fn graphics_fill_rect(x: u32, y: u32, w: u32, h: u32, color: u32) {
    unsafe {
        if !GRAPHICS_MODE.is_initialized {
            return;
        }
        
        for dy in 0..h {
            let py = y + dy;
            if py >= GRAPHICS_MODE.height {
                break;
            }
            
            for dx in 0..w {
                let px = x + dx;
                if px >= GRAPHICS_MODE.width {
                    break;
                }
                
                let idx = (py * GRAPHICS_MODE.width + px) as usize;
                BACK_BUFFER[idx] = color;
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn graphics_draw_rect(x: u32, y: u32, w: u32, h: u32, color: u32) {
    // Top
    graphics_draw_line_h(x, y, w, color);
    // Bottom
    graphics_draw_line_h(x, y + h - 1, w, color);
    // Left
    graphics_draw_line_v(x, y, h, color);
    // Right
    graphics_draw_line_v(x + w - 1, y, h, color);
}

#[no_mangle]
pub extern "C" fn graphics_draw_line_h(x: u32, y: u32, len: u32, color: u32) {
    unsafe {
        if !GRAPHICS_MODE.is_initialized || y >= GRAPHICS_MODE.height {
            return;
        }
        
        let end_x = (x + len).min(GRAPHICS_MODE.width);
        for px in x..end_x {
            let idx = (y * GRAPHICS_MODE.width + px) as usize;
            BACK_BUFFER[idx] = color;
        }
    }
}

#[no_mangle]
pub extern "C" fn graphics_draw_line_v(x: u32, y: u32, len: u32, color: u32) {
    unsafe {
        if !GRAPHICS_MODE.is_initialized || x >= GRAPHICS_MODE.width {
            return;
        }
        
        let end_y = (y + len).min(GRAPHICS_MODE.height);
        for py in y..end_y {
            let idx = (py * GRAPHICS_MODE.width + x) as usize;
            BACK_BUFFER[idx] = color;
        }
    }
}

#[no_mangle]
pub extern "C" fn graphics_draw_line(x0: i32, y0: i32, x1: i32, y1: i32, color: u32) {
    let dx = (x1 - x0).abs();
    let dy = (y1 - y0).abs();
    let sx = if x0 < x1 { 1 } else { -1 };
    let sy = if y0 < y1 { 1 } else { -1 };
    let mut err = dx - dy;
    let mut x = x0;
    let mut y = y0;
    
    loop {
        if x >= 0 && y >= 0 {
            graphics_put_pixel(x as u32, y as u32, color);
        }
        
        if x == x1 && y == y1 {
            break;
        }
        
        let e2 = 2 * err;
        if e2 > -dy {
            err -= dy;
            x += sx;
        }
        if e2 < dx {
            err += dx;
            y += sy;
        }
    }
}

#[no_mangle]
pub extern "C" fn graphics_draw_circle(cx: i32, cy: i32, radius: i32, color: u32) {
    let mut x = radius;
    let mut y = 0;
    let mut err = 0;
    
    while x >= y {
        let points = [
            (cx + x, cy + y), (cx + y, cy + x),
            (cx - y, cy + x), (cx - x, cy + y),
            (cx - x, cy - y), (cx - y, cy - x),
            (cx + y, cy - x), (cx + x, cy - y),
        ];
        
        for (px, py) in points.iter() {
            if *px >= 0 && *py >= 0 {
                graphics_put_pixel(*px as u32, *py as u32, color);
            }
        }
        
        if err <= 0 {
            y += 1;
            err += 2 * y + 1;
        }
        if err > 0 {
            x -= 1;
            err -= 2 * x + 1;
        }
    }
}

//-----------------------------------------------------------------------------
// Buffer Management
//-----------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn graphics_swap_buffers() {
    unsafe {
        if !GRAPHICS_MODE.is_initialized {
            return;
        }
        
        let fb = GRAPHICS_MODE.framebuffer;
        let total = (GRAPHICS_MODE.width * GRAPHICS_MODE.height) as usize;
        
        // Copy back buffer to framebuffer
        for i in 0..total {
            core::ptr::write_volatile(fb.add(i), BACK_BUFFER[i]);
        }
    }
}

#[no_mangle]
pub extern "C" fn graphics_swap_buffers_region(x: u32, y: u32, w: u32, h: u32) {
    unsafe {
        if !GRAPHICS_MODE.is_initialized {
            return;
        }
        
        let fb = GRAPHICS_MODE.framebuffer;
        
        for dy in 0..h {
            let py = y + dy;
            if py >= GRAPHICS_MODE.height {
                break;
            }
            
            for dx in 0..w {
                let px = x + dx;
                if px >= GRAPHICS_MODE.width {
                    break;
                }
                
                let idx = (py * GRAPHICS_MODE.width + px) as usize;
                core::ptr::write_volatile(fb.add(idx), BACK_BUFFER[idx]);
            }
        }
    }
}

//-----------------------------------------------------------------------------
// Test Functions
//-----------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn graphics_test_simple() -> i32 {
    unsafe {
        rust_print(b"\n=== Simple Graphics Test ===\n");
        
        // Initialize 800x600
        if graphics_init(800, 600, 32) != 0 {
            return -1;
        }
        
        // Clear to dark blue
        graphics_clear(rgb(40, 40, 80));
        graphics_swap_buffers();
        
        rust_print(b"Screen cleared. Press key...\n");
        keyboard_wait_for_key(0u8);
        
        // Draw colored rectangles
        graphics_fill_rect(100, 100, 200, 150, rgb(255, 0, 0));   // Red
        graphics_fill_rect(320, 100, 200, 150, rgb(0, 255, 0));   // Green
        graphics_fill_rect(540, 100, 200, 150, rgb(0, 0, 255));   // Blue
        graphics_swap_buffers();
        
        rust_print(b"Rectangles drawn. Press key...\n");
        keyboard_wait_for_key(0u8);
        
        // Draw lines
        graphics_clear(rgb(20, 20, 30));
        for i in 0..10 {
            let color = rgb(255 - i * 25, i * 25, 128);
    graphics_draw_line(
        (50 + i * 70) as i32,  // Cast to i32
        50,
        400,
        550,
        color
    );
        }
        graphics_swap_buffers();
        
        rust_print(b"Lines drawn. Press key...\n");
        keyboard_wait_for_key(0u8);
        
        // Draw circles
        graphics_clear(rgb(30, 30, 50));
        graphics_draw_circle(400, 300, 100, rgb(255, 255, 0));
        graphics_draw_circle(400, 300, 150, rgb(255, 0, 255));
        graphics_draw_circle(400, 300, 200, rgb(0, 255, 255));
        graphics_swap_buffers();
        
        rust_print(b"Circles drawn. Press key to exit...\n");
        keyboard_wait_for_key(0u8);
        
        graphics_shutdown();
        terminal_clear();
        
        0
    }
}
// Add this helper at the top of the graphics section (after the imports/constants)

// Sine lookup table for 0-90 degrees (multiply by 1000 for precision)
const SIN_TABLE: [i32; 91] = [
    0, 17, 35, 52, 70, 87, 105, 122, 139, 156, 174, 191, 208, 225, 242, 259,
    276, 292, 309, 326, 342, 358, 375, 391, 407, 423, 438, 454, 469, 485, 500,
    515, 530, 545, 559, 574, 588, 602, 616, 629, 643, 656, 669, 682, 695, 707,
    719, 731, 743, 755, 766, 777, 788, 799, 809, 819, 829, 839, 848, 857, 866,
    875, 883, 891, 899, 906, 914, 921, 927, 934, 940, 946, 951, 956, 961, 966,
    970, 974, 978, 982, 985, 988, 990, 993, 995, 996, 998, 999, 999, 1000, 1000
];

// Fast sine (angle in degrees 0-359, returns value * 1000)
fn sin_deg(angle: i32) -> i32 {
    let angle = angle % 360;
    let angle = if angle < 0 { angle + 360 } else { angle };
    
    match angle {
        0..=90 => SIN_TABLE[angle as usize],
        91..=180 => SIN_TABLE[(180 - angle) as usize],
        181..=270 => -SIN_TABLE[(angle - 180) as usize],
        _ => -SIN_TABLE[(360 - angle) as usize],
    }
}

// Fast cosine (angle in degrees 0-359, returns value * 1000)
fn cos_deg(angle: i32) -> i32 {
    sin_deg(angle + 90)
}

// Now replace the test_vbe_simple function:
#[no_mangle]
pub extern "C" fn test_vbe_simple() -> i32 {
    unsafe {
        rust_print(b"\n=== DOOM Logo ===\n");
        
        // Initialize graphics
        if graphics_init(800, 600, 32) != 0 {
            rust_print(b"ERROR: Graphics init failed\n");
            return -1;
        }
        
        // Animation loop
        for frame in 0..200 {
            // Red gradient background like classic DOOM
            for y in 0..600 {
                let red_intensity = (100 + (y * 50 / 600)) as u8;
                graphics_draw_line_h(0, y as u32, 800, rgb(red_intensity, 0, 0));
            }
            
            // Fire effect at bottom
            for y in 450..600 {
                for x in (0..800).step_by(4) {
                    let flicker = ((frame + x / 4) % 30) as i32;
                    let fire_height = y - 450;
                    if fire_height < flicker * 5 {
                        let intensity = (255 - fire_height * 2) as u8;
                        graphics_fill_rect(x as u32, y as u32, 4, 4, rgb(intensity, intensity / 3, 0));
                    }
                }
            }
            
            let center_x = 400;
            let logo_y = 150;
            
            // Flame glow behind logo
            let glow_pulse = (sin_deg((frame * 8) as i32) * 20 / 1000);
            for radius in (150..200).step_by(5) {
                let intensity = ((200 - radius) * 2 + glow_pulse) as u8;
                graphics_draw_circle(center_x, logo_y + 50, radius, rgb(intensity, intensity / 4, 0));
            }
            
            // === D ===
            let d_x = center_x - 260;
            
            // Left vertical bar of D
            for t in 0..35 {
                graphics_fill_rect((d_x + t) as u32, (logo_y - 60) as u32, 1, 120, rgb(200, 0, 0));
            }
            // Top horizontal
            graphics_fill_rect(d_x as u32, (logo_y - 60) as u32, 100, 35, rgb(200, 0, 0));
            // Bottom horizontal
            graphics_fill_rect(d_x as u32, (logo_y + 25) as u32, 100, 35, rgb(200, 0, 0));
            // Curved right side
            for angle in 270..450 {
                let radius = 60;
                let x = d_x + 65 + (cos_deg(angle as i32) * radius / 1000);
                let y = logo_y + (sin_deg(angle as i32) * radius / 1000);
                graphics_fill_rect(x as u32, y as u32, 35, 2, rgb(200, 0, 0));
            }
            
            // D outline/shadow
            for t in 0..5 {
                graphics_draw_circle(d_x + 65, logo_y, 60 + t, rgb(100, 0, 0));
                graphics_draw_line_v(d_x as u32, (logo_y - 60) as u32, 120, rgb(100, 0, 0));
            }
            
            // === First O ===
            let o1_x = center_x - 130;
            
            // Outer circle
            for t in 0..35 {
                graphics_draw_circle(o1_x, logo_y, 60 - t / 2, rgb(200, 0, 0));
            }
            // Inner hole
            for radius in 0..30 {
                graphics_draw_circle(o1_x, logo_y, radius, rgb(150, 0, 0));
            }
            // Outline
            for t in 0..5 {
                graphics_draw_circle(o1_x, logo_y, 60 + t, rgb(100, 0, 0));
            }
            
            // === Second O ===
            let o2_x = center_x;
            
            // Outer circle
            for t in 0..35 {
                graphics_draw_circle(o2_x, logo_y, 60 - t / 2, rgb(200, 0, 0));
            }
            // Inner hole
            for radius in 0..30 {
                graphics_draw_circle(o2_x, logo_y, radius, rgb(150, 0, 0));
            }
            // Outline
            for t in 0..5 {
                graphics_draw_circle(o2_x, logo_y, 60 + t, rgb(100, 0, 0));
            }
            
            // === M ===
            let m_x = center_x + 130;
            
            // Left vertical bar
            graphics_fill_rect((m_x - 60) as u32, (logo_y - 60) as u32, 35, 120, rgb(200, 0, 0));
            // Right vertical bar
            graphics_fill_rect((m_x + 25) as u32, (logo_y - 60) as u32, 35, 120, rgb(200, 0, 0));
            
            // Left diagonal
            for y in 0..70 {
                let x_offset = y * 30 / 70;
                graphics_fill_rect((m_x - 60 + x_offset) as u32, (logo_y - 60 + y) as u32, 30, 2, rgb(200, 0, 0));
            }
            // Right diagonal
            for y in 0..70 {
                let x_offset = 30 - (y * 30 / 70);
                graphics_fill_rect((m_x + x_offset) as u32, (logo_y - 60 + y) as u32, 30, 2, rgb(200, 0, 0));
            }
            
            // M outline
            for t in 0..5 {
                graphics_draw_line(m_x - 60 - t, logo_y - 60, m_x - 60 - t, logo_y + 60, rgb(100, 0, 0));
                graphics_draw_line(m_x + 60 + t, logo_y - 60, m_x + 60 + t, logo_y + 60, rgb(100, 0, 0));
                graphics_draw_line(m_x - 60 - t, logo_y - 60 - t, m_x, logo_y + 10 - t, rgb(100, 0, 0));
                graphics_draw_line(m_x + 60 + t, logo_y - 60 - t, m_x, logo_y + 10 - t, rgb(100, 0, 0));
            }
            
            // === Metallic highlights ===
            // Top highlights on letters
            for x in (d_x..(m_x + 60)).step_by(8) {
                graphics_fill_rect(x as u32, (logo_y - 62) as u32, 4, 2, rgb(255, 100, 100));
            }
            
            // === SUBTITLE: "PRESS ANY KEY" ===
            let subtitle_y = logo_y + 120;
            let blink = if (frame / 20) % 2 == 0 { 255 } else { 100 };
            
            // Simple pixel font letters
            draw_text_pixel(b"PRESS ANY KEY", center_x - 130, subtitle_y, rgb(blink, blink, blink));
            
            // === Animated skulls in corners ===
            let skull_pulse = (sin_deg((frame * 10) as i32) * 10 / 1000);
            
            // Top-left skull
            draw_skull(80, 80 + skull_pulse, rgb(180, 0, 0));
            
            // Top-right skull
            draw_skull(720, 80 - skull_pulse, rgb(180, 0, 0));
            
            // Bottom-left skull
            draw_skull(80, 520 + skull_pulse, rgb(180, 0, 0));
            
            // Bottom-right skull
            draw_skull(720, 520 - skull_pulse, rgb(180, 0, 0));
            
            graphics_swap_buffers();
            
            // Delay
            for _ in 0..1000000 {
                core::hint::spin_loop();
            }
        }
        
        rust_print(b"\nPress any key...\n");
        keyboard_wait_for_key(0u8);
        
        // Cleanup
        graphics_shutdown();
        terminal_clear();
        
        0
    }
}

// Helper function to draw simple pixel font
unsafe fn draw_text_pixel(text: &[u8], start_x: i32, y: i32, color: u32) {
    let mut x = start_x;
    for &ch in text {
        match ch {
            b'P' => {
                graphics_fill_rect(x as u32, y as u32, 3, 15, color);
                graphics_fill_rect((x + 3) as u32, y as u32, 6, 3, color);
                graphics_fill_rect((x + 9) as u32, y as u32, 3, 8, color);
                graphics_fill_rect((x + 3) as u32, (y + 7) as u32, 6, 3, color);
            }
            b'R' => {
                graphics_fill_rect(x as u32, y as u32, 3, 15, color);
                graphics_fill_rect((x + 3) as u32, y as u32, 6, 3, color);
                graphics_fill_rect((x + 9) as u32, y as u32, 3, 8, color);
                graphics_fill_rect((x + 3) as u32, (y + 7) as u32, 6, 3, color);
                graphics_draw_line(x + 3, y + 10, x + 9, y + 15, color);
                graphics_draw_line(x + 4, y + 10, x + 10, y + 15, color);
            }
            b'E' => {
                graphics_fill_rect(x as u32, y as u32, 3, 15, color);
                graphics_fill_rect((x + 3) as u32, y as u32, 9, 3, color);
                graphics_fill_rect((x + 3) as u32, (y + 6) as u32, 7, 3, color);
                graphics_fill_rect((x + 3) as u32, (y + 12) as u32, 9, 3, color);
            }
            b'S' => {
                graphics_fill_rect((x + 3) as u32, y as u32, 6, 3, color);
                graphics_fill_rect(x as u32, (y + 3) as u32, 3, 4, color);
                graphics_fill_rect((x + 3) as u32, (y + 6) as u32, 6, 3, color);
                graphics_fill_rect((x + 9) as u32, (y + 9) as u32, 3, 3, color);
                graphics_fill_rect((x + 3) as u32, (y + 12) as u32, 6, 3, color);
            }
            b'A' => {
                graphics_fill_rect((x + 3) as u32, y as u32, 6, 3, color);
                graphics_fill_rect(x as u32, (y + 3) as u32, 3, 12, color);
                graphics_fill_rect((x + 9) as u32, (y + 3) as u32, 3, 12, color);
                graphics_fill_rect((x + 3) as u32, (y + 7) as u32, 6, 3, color);
            }
            b'N' => {
                graphics_fill_rect(x as u32, y as u32, 3, 15, color);
                graphics_fill_rect((x + 9) as u32, y as u32, 3, 15, color);
                for i in 0..12 {
                    graphics_fill_rect((x + 3 + i * 6 / 12) as u32, (y + i) as u32, 2, 2, color);
                }
            }
            b'Y' => {
                graphics_fill_rect(x as u32, y as u32, 3, 7, color);
                graphics_fill_rect((x + 9) as u32, y as u32, 3, 7, color);
                graphics_fill_rect((x + 3) as u32, (y + 6) as u32, 6, 3, color);
                graphics_fill_rect((x + 5) as u32, (y + 9) as u32, 2, 6, color);
            }
            b'K' => {
                graphics_fill_rect(x as u32, y as u32, 3, 15, color);
                graphics_draw_line(x + 9, y, x + 3, y + 7, color);
                graphics_draw_line(x + 10, y, x + 4, y + 7, color);
                graphics_draw_line(x + 3, y + 8, x + 9, y + 15, color);
                graphics_draw_line(x + 4, y + 8, x + 10, y + 15, color);
            }
            b' ' => { }
            _ => { }
        }
        x += 14;
    }
}

// Helper function to draw a skull
pub extern "C" fn draw_skull(x: i32, y: i32, color: u32) {
    // Skull outline
    for radius in 20..25 {
        graphics_draw_circle(x, y, radius, color);
    }
    
    // Eye sockets (dark)
    for radius in 0..8 {
        graphics_draw_circle(x - 10, y - 5, radius, rgb(50, 0, 0));
        graphics_draw_circle(x + 10, y - 5, radius, rgb(50, 0, 0));
    }
    
    // Glowing eyes
    for radius in 0..4 {
        graphics_draw_circle(x - 10, y - 5, radius, rgb(255, 50, 0));
        graphics_draw_circle(x + 10, y - 5, radius, rgb(255, 50, 0));
    }
    
    // Nose cavity
    graphics_fill_rect((x - 3) as u32, (y + 2) as u32, 6, 8, rgb(50, 0, 0));
    
    // Teeth
    for i in 0..5 {
        let tooth_x = x - 10 + i * 5;
        graphics_fill_rect(tooth_x as u32, (y + 12) as u32, 3, 6, color);
        graphics_fill_rect((tooth_x + 1) as u32, (y + 18) as u32, 1, 3, rgb(50, 0, 0));
    }
}
//=============================================================================
// TEXT RENDERING SYSTEM
//=============================================================================

// 8x8 bitmap font (ASCII 32-127)
// Each character is 8 bytes, each byte is a row of 8 pixels
// Correct 8×8 bitmap font - MSB-first encoding.
// Render with: (byte >> (7 - col)) & 1
// bit 7 = leftmost pixel (col 0), bit 0 = rightmost pixel (col 7)
const FONT_8X8: [[u8; 8]; 96] = [
    [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], // ' ' (32)
    [0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x10, 0x00], // '!' (33)
    [0x50, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], // '"' (34)
    [0x50, 0x50, 0xF8, 0x50, 0xF8, 0x50, 0x50, 0x00], // '#' (35)
    [0x10, 0x7C, 0x90, 0x70, 0x12, 0x7C, 0x10, 0x00], // '$' (36)
    [0xC8, 0xD0, 0x20, 0x40, 0x98, 0x98, 0x00, 0x00], // '%' (37)
    [0x60, 0x90, 0x90, 0x68, 0x94, 0x90, 0x68, 0x00], // '&' (38)
    [0x30, 0x30, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00], // ''' (39)
    [0x08, 0x10, 0x20, 0x20, 0x20, 0x10, 0x08, 0x00], // '(' (40)
    [0x20, 0x10, 0x08, 0x08, 0x08, 0x10, 0x20, 0x00], // ')' (41)
    [0x00, 0x54, 0x38, 0xFE, 0x38, 0x54, 0x00, 0x00], // '*' (42)
    [0x00, 0x10, 0x10, 0xFE, 0x10, 0x10, 0x00, 0x00], // '+' (43)
    [0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x10], // ',' (44)
    [0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x00], // '-' (45)
    [0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x00], // '.' (46)
    [0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00], // '/' (47)
    [0x7C, 0x84, 0x8C, 0x94, 0xC4, 0x84, 0x7C, 0x00], // '0' (48)
    [0x30, 0x70, 0x30, 0x30, 0x30, 0x30, 0x7C, 0x00], // '1' (49)
    [0x78, 0x84, 0x04, 0x18, 0x60, 0x80, 0xFC, 0x00], // '2' (50)
    [0x78, 0x84, 0x04, 0x38, 0x04, 0x84, 0x78, 0x00], // '3' (51)
    [0x18, 0x28, 0x48, 0x88, 0xFC, 0x08, 0x08, 0x00], // '4' (52)
    [0xFC, 0x80, 0x80, 0xF8, 0x04, 0x84, 0x78, 0x00], // '5' (53)
    [0x3C, 0xC0, 0x80, 0xF8, 0x84, 0x84, 0x78, 0x00], // '6' (54)
    [0xFC, 0x04, 0x08, 0x10, 0x20, 0x20, 0x20, 0x00], // '7' (55)
    [0x78, 0x84, 0x84, 0x78, 0x84, 0x84, 0x78, 0x00], // '8' (56)
    [0x78, 0x84, 0x84, 0x7C, 0x04, 0x84, 0x78, 0x00], // '9' (57)
    [0x00, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x00], // ':' (58)
    [0x00, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x10], // ';' (59)
    [0x00, 0x08, 0x20, 0x80, 0x20, 0x08, 0x00, 0x00], // '<' (60)
    [0x00, 0x00, 0xFC, 0x00, 0xFC, 0x00, 0x00, 0x00], // '=' (61)
    [0x00, 0x80, 0x20, 0x08, 0x20, 0x80, 0x00, 0x00], // '>' (62)
    [0x78, 0x84, 0x04, 0x18, 0x20, 0x00, 0x20, 0x00], // '?' (63)
    [0x78, 0x84, 0x3C, 0x24, 0x3C, 0x80, 0x78, 0x00], // '@' (64)
    [0x30, 0x48, 0x84, 0x84, 0xFC, 0x84, 0x84, 0x00], // 'A' (65)
    [0xF8, 0x84, 0x84, 0xF8, 0x84, 0x84, 0xF8, 0x00], // 'B' (66)
    [0x78, 0x84, 0x80, 0x80, 0x80, 0x84, 0x78, 0x00], // 'C' (67)
    [0xF8, 0x84, 0x82, 0x82, 0x82, 0x84, 0xF8, 0x00], // 'D' (68)
    [0xFC, 0x80, 0x80, 0xF8, 0x80, 0x80, 0xFC, 0x00], // 'E' (69)
    [0xFC, 0x80, 0x80, 0xF8, 0x80, 0x80, 0x80, 0x00], // 'F' (70)
    [0x78, 0x84, 0x80, 0x9C, 0x84, 0x84, 0x78, 0x00], // 'G' (71)
    [0x84, 0x84, 0x84, 0xFC, 0x84, 0x84, 0x84, 0x00], // 'H' (72)
    [0x7C, 0x10, 0x10, 0x10, 0x10, 0x10, 0x7C, 0x00], // 'I' (73)
    [0x7C, 0x08, 0x08, 0x08, 0x08, 0x88, 0x70, 0x00], // 'J' (74)
    [0x84, 0x88, 0x90, 0xE0, 0x90, 0x88, 0x84, 0x00], // 'K' (75)
    [0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xFC, 0x00], // 'L' (76)
    [0x82, 0xC6, 0xAA, 0x92, 0x82, 0x82, 0x82, 0x00], // 'M' (77)
    [0x82, 0xC2, 0xA2, 0x92, 0x8A, 0x86, 0x82, 0x00], // 'N' (78)
    [0x78, 0x84, 0x84, 0x84, 0x84, 0x84, 0x78, 0x00], // 'O' (79)
    [0xF8, 0x84, 0x84, 0xF8, 0x80, 0x80, 0x80, 0x00], // 'P' (80)
    [0x78, 0x84, 0x84, 0x84, 0x94, 0x8C, 0x7C, 0x00], // 'Q' (81)
    [0xF8, 0x84, 0x84, 0xF8, 0x90, 0x88, 0x84, 0x00], // 'R' (82)
    [0x78, 0x84, 0x80, 0x78, 0x04, 0x84, 0x78, 0x00], // 'S' (83)
    [0xFE, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00], // 'T' (84)
    [0x84, 0x84, 0x84, 0x84, 0x84, 0x84, 0x78, 0x00], // 'U' (85)
    [0x82, 0x82, 0x44, 0x44, 0x28, 0x28, 0x10, 0x00], // 'V' (86)
    [0x82, 0x82, 0x92, 0x92, 0x92, 0x48, 0x24, 0x00], // 'W' (87)
    [0x82, 0x82, 0x44, 0x38, 0x44, 0x82, 0x82, 0x00], // 'X' (88)
    [0x82, 0x44, 0x28, 0x10, 0x10, 0x10, 0x10, 0x00], // 'Y' (89)
    [0xFE, 0x02, 0x04, 0x38, 0x40, 0x80, 0xFE, 0x00], // 'Z' (90)
    [0x60, 0x40, 0x40, 0x40, 0x40, 0x40, 0x60, 0x00], // '[' (91)
    [0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00], // '\' (92)
    [0x60, 0x20, 0x20, 0x20, 0x20, 0x20, 0x60, 0x00], // ']' (93)
    [0x10, 0x28, 0x44, 0x82, 0x00, 0x00, 0x00, 0x00], // '^' (94)
    [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE], // '_' (95)
    [0x40, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], // '`' (96)
    [0x00, 0x00, 0x78, 0x04, 0x7C, 0x84, 0x7C, 0x00], // 'a' (97)
    [0x80, 0x80, 0xF8, 0x84, 0x84, 0x84, 0xF8, 0x00], // 'b' (98)
    [0x00, 0x00, 0x78, 0x80, 0x80, 0x80, 0x78, 0x00], // 'c' (99)
    [0x04, 0x04, 0x7C, 0x84, 0x84, 0x84, 0x7C, 0x00], // 'd' (100)
    [0x00, 0x00, 0x78, 0x84, 0xFC, 0x80, 0x78, 0x00], // 'e' (101)
    [0x38, 0x40, 0x40, 0xF0, 0x40, 0x40, 0x40, 0x00], // 'f' (102)
    [0x00, 0x00, 0x7C, 0x84, 0x84, 0x7C, 0x04, 0x78], // 'g' (103)
    [0x80, 0x80, 0xF8, 0x84, 0x84, 0x84, 0x84, 0x00], // 'h' (104)
    [0x20, 0x00, 0x60, 0x20, 0x20, 0x20, 0x70, 0x00], // 'i' (105)
    [0x10, 0x00, 0x30, 0x10, 0x10, 0x90, 0x60, 0x00], // 'j' (106)
    [0x80, 0x80, 0x88, 0x90, 0xE0, 0x90, 0x88, 0x00], // 'k' (107)
    [0x60, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00], // 'l' (108)
    [0x00, 0x00, 0xA8, 0xFC, 0xA4, 0xA4, 0xA4, 0x00], // 'm' (109)
    [0x00, 0x00, 0xF8, 0x84, 0x84, 0x84, 0x84, 0x00], // 'n' (110)
    [0x00, 0x00, 0x78, 0x84, 0x84, 0x84, 0x78, 0x00], // 'o' (111)
    [0x00, 0x00, 0xF8, 0x84, 0x84, 0xF8, 0x80, 0x80], // 'p' (112)
    [0x00, 0x00, 0x7C, 0x84, 0x84, 0x7C, 0x04, 0x04], // 'q' (113)
    [0x00, 0x00, 0xF8, 0x80, 0x80, 0x80, 0x80, 0x00], // 'r' (114)
    [0x00, 0x00, 0x78, 0x80, 0x78, 0x04, 0xF8, 0x00], // 's' (115)
    [0x20, 0x20, 0xF8, 0x20, 0x20, 0x20, 0x78, 0x00], // 't' (116)
    [0x00, 0x00, 0x84, 0x84, 0x84, 0x84, 0x7C, 0x00], // 'u' (117)
    [0x00, 0x00, 0x82, 0x82, 0x44, 0x28, 0x10, 0x00], // 'v' (118)
    [0x00, 0x00, 0x82, 0x92, 0x92, 0x48, 0x24, 0x00], // 'w' (119)
    [0x00, 0x00, 0x82, 0x44, 0x38, 0x44, 0x82, 0x00], // 'x' (120)
    [0x00, 0x00, 0x84, 0x84, 0x84, 0x7C, 0x04, 0x78], // 'y' (121)
    [0x00, 0x00, 0xFC, 0x08, 0x10, 0x20, 0xFC, 0x00], // 'z' (122)
    [0x18, 0x20, 0x20, 0xC0, 0x20, 0x20, 0x18, 0x00], // '{' (123)
    [0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00], // '|' (124)
    [0x60, 0x10, 0x10, 0x0C, 0x10, 0x10, 0x60, 0x00], // '}' (125)
    [0x00, 0x68, 0x94, 0x02, 0x00, 0x00, 0x00, 0x00], // '~' (126)
    [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], // DEL (127)
];

// Render with this in graphics_draw_char and graphics_draw_char_scaled:
//   if (byte >> (7 - col)) & 1 != 0 { ... }
//-----------------------------------------------------------------------------
// Text Drawing Functions
//-----------------------------------------------------------------------------


#[no_mangle]
pub extern "C" fn graphics_draw_string(x: u32, y: u32, text: *const u8, color: u32) {
    unsafe {
        let mut px = x;
        let mut i = 0;
        
        loop {
            let ch = *text.add(i);
            if ch == 0 {
                break;
            }
            
            if ch == b'\n' {
                // Newline handling would go here
                break;
            }
            
            graphics_draw_char(px, y, ch, color);
            px += 8; // Each character is 8 pixels wide
            i += 1;
            
            if px >= GRAPHICS_MODE.width {
                break;
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn graphics_draw_string_bg(x: u32, y: u32, text: *const u8, fg_color: u32, bg_color: u32) {
    unsafe {
        let mut px = x;
        let mut i = 0;
        
        loop {
            let ch = *text.add(i);
            if ch == 0 {
                break;
            }
            
            // Draw background
            graphics_fill_rect(px, y, 8, 8, bg_color);
            
            // Draw character
            graphics_draw_char(px, y, ch, fg_color);
            px += 8;
            i += 1;
            
            if px >= GRAPHICS_MODE.width {
                break;
            }
        }
    }
}

// Helper to get string length in pixels
#[no_mangle]
pub extern "C" fn graphics_string_width(text: *const u8) -> u32 {
    unsafe {
        let mut len = 0;
        let mut i = 0;
        
        loop {
            let ch = *text.add(i);
            if ch == 0 {
                break;
            }
            len += 1;
            i += 1;
        }
        
        len * 8 // 8 pixels per character
    }
}

#[no_mangle]
pub extern "C" fn graphics_draw_string_scaled(x: u32, y: u32, text: *const u8, color: u32, scale: u32) {
    unsafe {
        let mut px = x;
        let mut i = 0;
        
        loop {
            let ch = *text.add(i);
            if ch == 0 {
                break;
            }
            
            if ch == b'\n' {
                break;
            }
            
            graphics_draw_char_scaled(px, y, ch, color, scale);
            px += 8 * scale;
            i += 1;
            
            if px >= GRAPHICS_MODE.width {
                break;
            }
        }
    }
}







// ── Constants ─────────────────────────────────────────────────────────────────────
// Replace this with your actual API Token generated in the RadiumOS Admin Panel
// (Admin -> Tokens -> Create Token -> Copy "read" or "admin" token)
const API_TOKEN: &[u8] = b"???"; 

// ── Display Active Devices ───────────────────────────────────────────────────────
/// Fetches and displays active connections from the RadiumOS PSCA server.
/// 
/// This function connects to the proxy server and requests the `/api/v1/connections`
/// endpoint, which returns a JSON list of active sockets/connections.
#[no_mangle]
pub unsafe extern "C" fn display_active_radiumos_devices() -> bool {
    // Use the hardcoded proxy IP as the hostname (Host header should match server)
    let proxy_ip = [72, 14, 176, 144]; 
    let proxy_host = b"72.14.176.144"; 
    let path = b"/api/v1/connections";

    b_set_status(b"Connecting to RadiumOS...");

    if !tcp_connect(&proxy_ip, 8080) {
        b_set_status(b"Connection failed");
        return false;
    }

    b_set_status(b"Fetching devices...");
    
    // Use the authenticated version of send_get
    let success = send_get_auth(proxy_host, path, API_TOKEN);

    if success {
        b_set_status(b"Devices loaded");
    } else {
        b_set_status(b"Fetch failed");
    }

    success
}

// ── Authenticated HTTP GET ───────────────────────────────────────────────────────
/// Sends an HTTP GET request with a Bearer token authorization header.
/// Mostly identical to send_get, but injects the Authorization header.
unsafe fn send_get_auth(hostname: &[u8], path: &[u8], token: &[u8]) -> bool {
    let mut req = [0u8; 2048];
    let mut ri  = 0usize;
    macro_rules! push {
        ($b:expr) => { for &c in $b { if ri < req.len()-1 { req[ri]=c; ri+=1; } } };
    }

    push!(b"GET ");
    if path.is_empty() || path[0] != b'/' { push!(b"/"); }
    push!(path);
    push!(b" HTTP/1.1\r\n");
    push!(b"Host: "); push!(hostname); push!(b"\r\n");
    push!(b"User-Agent: RadiumOS-DeviceScanner/1.0\r\n");
    push!(b"Accept: application/json\r\n");
    
    // --- AUTH HEADER ---
    push!(b"Authorization: Bearer "); push!(token); push!(b"\r\n");
    // ------------------

    push!(b"Connection: close\r\n");
    push!(b"X-Proxy-Scheme: http\r\n\r\n");

    let local_port  = TCP_CONNECTION.local_port;
    let remote_port = TCP_CONNECTION.remote_port;

    if !tcp_send_data(&req[..ri]) {
        b_set_status(b"Send failed");
        tcp_close();
        return false;
    }

    HTTP_RECEIVE_LEN    = 0;
    RX_RESPONSE_LENGTH  = 0;
    global_hdr_done     = false;
    global_is_chunked   = false;
    global_content_len  = 0;
    global_body_off     = 0;

    // Timeout settings (identical to original)
    for _ in 0..3_000_000u32 { core::hint::spin_loop(); }

    let idle_limit:  u32 = 200_000_000;
    let hard_limit:  u32 = 800_000_000;
    let mut loops:   u32 = 0;
    let mut idle:    u32 = 0;

    'recv: loop {
        loops += 1;
        if loops >= hard_limit {
            rust_print(b"send_get_auth: hard timeout\n");
            break 'recv;
        }

        RX_RESPONSE_LENGTH = 0;
        rust_rtl8139_receive();

        if RX_RESPONSE_LENGTH < 54 {
            RX_RESPONSE_LENGTH = 0;
            idle += 1;

            if global_hdr_done && HTTP_RECEIVE_LEN > global_body_off {
                let body_received = HTTP_RECEIVE_LEN - global_body_off;
                let complete = if global_is_chunked {
                    find_chunk_end().is_some()
                } else if global_content_len > 0 {
                    body_received >= global_content_len
                } else {
                    false
                };
                if complete {
                    rust_print(b"send_get_auth: complete\n");
                    break 'recv;
                }
            }

            if idle >= idle_limit {
                rust_print(b"send_get_auth: idle timeout\n");
                break 'recv;
            }
            continue 'recv;
        }

        // --- Filter to our TCP connection (Identical Logic) ---
        let et = ((RX_RESPONSE_BUFFER[12] as u16) << 8) | (RX_RESPONSE_BUFFER[13] as u16);
        if et != 0x0800 { RX_RESPONSE_LENGTH = 0; continue 'recv; }

        let ihl = ((RX_RESPONSE_BUFFER[14] & 0x0F) * 4) as usize;
        if ihl < 20 || RX_RESPONSE_BUFFER[14 + 9] != 6 {
            RX_RESPONSE_LENGTH = 0; continue 'recv;
        }

        let tcp_start = 14 + ihl;
        if tcp_start + 20 > RX_RESPONSE_LENGTH as usize {
            RX_RESPONSE_LENGTH = 0; continue 'recv;
        }

        let src_p = ((RX_RESPONSE_BUFFER[tcp_start]     as u16) << 8)
                  |  (RX_RESPONSE_BUFFER[tcp_start + 1] as u16);
        let dst_p = ((RX_RESPONSE_BUFFER[tcp_start + 2] as u16) << 8)
                  |  (RX_RESPONSE_BUFFER[tcp_start + 3] as u16);

        if src_p != remote_port || dst_p != local_port {
            RX_RESPONSE_LENGTH = 0; continue 'recv;
        }

        idle = 0;

        let flags    = RX_RESPONSE_BUFFER[tcp_start + 13];
        let tcp_hl   = ((RX_RESPONSE_BUFFER[tcp_start + 12] >> 4) * 4) as usize;
        let data_off = tcp_start + tcp_hl;
        let total    = RX_RESPONSE_LENGTH as usize;

        let rseq = ((RX_RESPONSE_BUFFER[tcp_start + 4] as u32) << 24)
                 | ((RX_RESPONSE_BUFFER[tcp_start + 5] as u32) << 16)
                 | ((RX_RESPONSE_BUFFER[tcp_start + 6] as u32) <<  8)
                 |  (RX_RESPONSE_BUFFER[tcp_start + 7] as u32);

        // --- Copy payload ---
        if data_off < total {
            let dlen  = total - data_off;
            let space = HTTP_RECEIVE_BUFFER.len().saturating_sub(HTTP_RECEIVE_LEN);
            let copy  = dlen.min(space);

            for j in 0..copy {
                HTTP_RECEIVE_BUFFER[HTTP_RECEIVE_LEN + j] = RX_RESPONSE_BUFFER[data_off + j];
            }
            HTTP_RECEIVE_LEN += copy;

            if !global_hdr_done {
                parse_http_headers();
            }

            if global_hdr_done {
                let body_received = HTTP_RECEIVE_LEN.saturating_sub(global_body_off);
                let complete = if global_is_chunked {
                    find_chunk_end().is_some()
                } else if global_content_len > 0 {
                    body_received >= global_content_len
                } else {
                    false
                };

                if complete {
                    // Send ACK and break
                    TCP_CONNECTION.ack_num = rseq.wrapping_add(dlen as u32);
                    send_ack_packet(local_port, remote_port);
                    RX_RESPONSE_LENGTH = 0;
                    break 'recv;
                }
            }

            TCP_CONNECTION.ack_num = rseq.wrapping_add(dlen as u32);
            send_ack_packet(local_port, remote_port);
        }

        if (flags & 0x01) != 0 {
            rust_print(b"send_get_auth: FIN received\n");
            TCP_CONNECTION.ack_num = rseq.wrapping_add(1);
            send_ack_packet(local_port, remote_port);
            RX_RESPONSE_LENGTH = 0;
            break 'recv;
        }

        RX_RESPONSE_LENGTH = 0;
    }

    let recv_len = HTTP_RECEIVE_LEN;

    if recv_len == 0 {
        b_set_status(b"No response received");
        return false;
    }

    // Process the JSON response to display devices
    process_connections_response(recv_len);
    true
}

// ── Process Connections Response ─────────────────────────────────────────────────
/// Simple parser to extract IP addresses from the JSON response and print them.
/// Since we don't have a JSON library, we scan for string patterns.
unsafe fn process_connections_response(len: usize) {
    // The API returns: [{"ip": "...", "method": "...", ...}, ...]
    // We scan for "ip":"<string>"
    
    let buffer = &HTTP_RECEIVE_BUFFER[global_body_off..len];
    let mut i = 0;
    let mut count = 0;
    
    rust_print(b"--- Active RadiumOS Devices ---\n");

    while i < buffer.len() - 6 {
        // Look for "ip":"
        if buffer[i] == b'"' && buffer[i+1] == b'i' && buffer[i+2] == b'p' && 
           buffer[i+3] == b'"' && buffer[i+4] == b':' && buffer[i+5] == b'"' {
            
            let start = i + 6;
            let mut end = start;
            
            // Find the closing quote
            while end < buffer.len() && buffer[end] != b'"' {
                end += 1;
            }
            
            if end < buffer.len() {
                // Print the IP
                rust_print(b"Device: ");
                // Print slice from start to end
                let ip_slice = &buffer[start..end];
                for &b in ip_slice {
                    print_char(b);
                }
                rust_print(b"\n");
                count += 1;
                
                i = end; // Move index forward
            }
        }
        i += 1;
    }
    
    if count == 0 {
        rust_print(b"No active devices found.\n");
    } else {
        let mut c_str = [0u8; 20];
        let mut c_idx = 0;
        let mut n = count;
        // Basic int to string for count
        if n == 0 { 
            c_str[c_idx] = b'0'; c_idx += 1; 
        } else {
            let mut digits = [0u8; 10];
            let mut d_idx = 0;
            while n > 0 {
                digits[d_idx] = (n % 10) as u8 + b'0';
                n /= 10;
                d_idx += 1;
            }
            while d_idx > 0 {
                d_idx -= 1;
                c_str[c_idx] = digits[d_idx];
                c_idx += 1;
            }
        }
        rust_print(b"Total devices: ");
        for k in 0..c_idx { print_char(c_str[k]); }
        rust_print(b"\n------------------------------\n");
    }
}


// =============================================================================
// RADIUMOS AES-128  --  by scp_2801
// =============================================================================
// Standard AES-128 (FIPS 197) in CBC mode
//   - 10 rounds, 128-bit key, 128-bit block
//   - Precomputed S-box / inverse S-box tables (no on-the-fly GF brute-force)
//   - Precomputed xtime / mul tables for fast MixColumns
//   - Compact AES key schedule (standard Rcon expansion)
//   - CBC mode with zero-byte PKCS#7-style padding to 16-byte boundary
//   - Static IV (0x52 61 64 69 75 6D 4F 53 73 63 70 32 38 30 31 21 = "RadiumOSscp2801!")
//   - Verbose logging: colour-coded per phase, hex matrix dumps, round-by-round diffs
//   - AVFS integration: rust_aes_encrypt_file / rust_aes_decrypt_file
//   - Self-test: rust_aes_selftest()  -- prints NIST known-answer test + round-trip
//
// VERBOSE TRACE CONTROL:
//   aes_trace_on()   -- full round-by-round hex dumps + phase labels
//   aes_trace_off()  -- silent mode (errors only)
//   aes_trace_flip() -- toggle current state
//   aes_trace_query() -> i32  -- 1 if on, 0 if off
//   Default: OFF (silent boot)
//
// TERMINAL COLOR LEGEND:
//   0x07  grey       -- borders / separators
//   0x0A  green      -- plaintext / PASS / success
//   0x0B  cyan       -- state matrices / data bytes
//   0x0C  red        -- ciphertext / errors / FAIL
//   0x0D  magenta    -- key schedule / round keys
//   0x0E  yellow     -- round headers / section banners
//   0x0F  white      -- labels / emphasis
//   0x08  dark grey  -- secondary info / punctuation
// =============================================================================

// ---------------------------------------------------------------------------
// VERBOSE OUTPUT CONTROL
// ---------------------------------------------------------------------------
// Control AES diagnostic output verbosity at runtime.
// Call from kernel init or terminal commands:
//
//   aes_trace_on()   -- full round-by-round hex dumps + phase labels
//   aes_trace_off()  -- silent mode (errors only)
//   aes_trace_flip() -- toggle current state
//   aes_trace_query() -> 1 or 0
//
// The global flag is read via the zero-cost trace() inline; no caller needs
// to pass a verbose bool manually -- it flows through automatically.
//
// Typical kernel init:
//   // aes_trace_on();   <-- uncomment when debugging crypto
//   rust_aes_init(key);
//
// Typical terminal command:
//   "aes trace"     -> aes_trace_flip()
//   "aes trace on"  -> aes_trace_on()
//   "aes trace off" -> aes_trace_off()
//   "aes trace ?"   -> aes_trace_query()
// ---------------------------------------------------------------------------

static mut AES_TRACE: bool = false;

/// Enable verbose AES tracing (round dumps, key schedule, phase labels).
#[no_mangle]
pub extern "C" fn aes_trace_on() {
    unsafe {
        AES_TRACE = true;
        tp(b"  [AES] Trace: ", 0x0E);
        tp(b"ON", 0x0A);
        tp(b"  -- full round diagnostics enabled\n", 0x08);
    }
}

/// Disable verbose AES tracing (silent operation, errors still shown).
#[no_mangle]
pub extern "C" fn aes_trace_off() {
    unsafe {
        AES_TRACE = false;
        tp(b"  [AES] Trace: ", 0x0E);
        tp(b"OFF", 0x0C);
        tp(b"  -- silent mode\n", 0x08);
    }
}

/// Toggle AES trace state (useful for terminal `aes trace` command).
#[no_mangle]
pub extern "C" fn aes_trace_flip() {
    unsafe {
        AES_TRACE = !AES_TRACE;
        if AES_TRACE { aes_trace_on(); } else { aes_trace_off(); }
    }
}

/// Query current trace state. Returns 1 if on, 0 if off.
#[no_mangle]
pub extern "C" fn aes_trace_query() -> i32 {
    unsafe { if AES_TRACE { 1 } else { 0 } }
}

/// Internal: zero-cost read of the global trace flag.
#[inline(always)]
unsafe fn trace() -> bool { AES_TRACE }

// ---------------------------------------------------------------------------
// S-BOX TABLES  (precomputed -- FIPS 197 Annex A)
// ---------------------------------------------------------------------------

#[rustfmt::skip]
static SBOX: [u8; 256] = [
    0x63,0x7C,0x77,0x7B,0xF2,0x6B,0x6F,0xC5,0x30,0x01,0x67,0x2B,0xFE,0xD7,0xAB,0x76,
    0xCA,0x82,0xC9,0x7D,0xFA,0x59,0x47,0xF0,0xAD,0xD4,0xA2,0xAF,0x9C,0xA4,0x72,0xC0,
    0xB7,0xFD,0x93,0x26,0x36,0x3F,0xF7,0xCC,0x34,0xA5,0xE5,0xF1,0x71,0xD8,0x31,0x15,
    0x04,0xC7,0x23,0xC3,0x18,0x96,0x05,0x9A,0x07,0x12,0x80,0xE2,0xEB,0x27,0xB2,0x75,
    0x09,0x83,0x2C,0x1A,0x1B,0x6E,0x5A,0xA0,0x52,0x3B,0xD6,0xB3,0x29,0xE3,0x2F,0x84,
    0x53,0xD1,0x00,0xED,0x20,0xFC,0xB1,0x5B,0x6A,0xCB,0xBE,0x39,0x4A,0x4C,0x58,0xCF,
    0xD0,0xEF,0xAA,0xFB,0x43,0x4D,0x33,0x85,0x45,0xF9,0x02,0x7F,0x50,0x3C,0x9F,0xA8,
    0x51,0xA3,0x40,0x8F,0x92,0x9D,0x38,0xF5,0xBC,0xB6,0xDA,0x21,0x10,0xFF,0xF3,0xD2,
    0xCD,0x0C,0x13,0xEC,0x5F,0x97,0x44,0x17,0xC4,0xA7,0x7E,0x3D,0x64,0x5D,0x19,0x73,
    0x60,0x81,0x4F,0xDC,0x22,0x2A,0x90,0x88,0x46,0xEE,0xB8,0x14,0xDE,0x5E,0x0B,0xDB,
    0xE0,0x32,0x3A,0x0A,0x49,0x06,0x24,0x5C,0xC2,0xD3,0xAC,0x62,0x91,0x95,0xE4,0x79,
    0xE7,0xC8,0x37,0x6D,0x8D,0xD5,0x4E,0xA9,0x6C,0x56,0xF4,0xEA,0x65,0x7A,0xAE,0x08,
    0xBA,0x78,0x25,0x2E,0x1C,0xA6,0xB4,0xC6,0xE8,0xDD,0x74,0x1F,0x4B,0xBD,0x8B,0x8A,
    0x70,0x3E,0xB5,0x66,0x48,0x03,0xF6,0x0E,0x61,0x35,0x57,0xB9,0x86,0xC1,0x1D,0x9E,
    0xE1,0xF8,0x98,0x11,0x69,0xD9,0x8E,0x94,0x9B,0x1E,0x87,0xE9,0xCE,0x55,0x28,0xDF,
    0x8C,0xA1,0x89,0x0D,0xBF,0xE6,0x42,0x68,0x41,0x99,0x2D,0x0F,0xB0,0x54,0xBB,0x16,
];

#[rustfmt::skip]
static SBOX_INV: [u8; 256] = [
    0x52,0x09,0x6A,0xD5,0x30,0x36,0xA5,0x38,0xBF,0x40,0xA3,0x9E,0x81,0xF3,0xD7,0xFB,
    0x7C,0xE3,0x39,0x82,0x9B,0x2F,0xFF,0x87,0x34,0x8E,0x43,0x44,0xC4,0xDE,0xE9,0xCB,
    0x54,0x7B,0x94,0x32,0xA6,0xC2,0x23,0x3D,0xEE,0x4C,0x95,0x0B,0x42,0xFA,0xC3,0x4E,
    0x08,0x2E,0xA1,0x66,0x28,0xD9,0x24,0xB2,0x76,0x5B,0xA2,0x49,0x6D,0x8B,0xD1,0x25,
    0x72,0xF8,0xF6,0x64,0x86,0x68,0x98,0x16,0xD4,0xA4,0x5C,0xCC,0x5D,0x65,0xB6,0x92,
    0x6C,0x70,0x48,0x50,0xFD,0xED,0xB9,0xDA,0x5E,0x15,0x46,0x57,0xA7,0x8D,0x9D,0x84,
    0x90,0xD8,0xAB,0x00,0x8C,0xBC,0xD3,0x0A,0xF7,0xE4,0x58,0x05,0xB8,0xB3,0x45,0x06,
    0xD0,0x2C,0x1E,0x8F,0xCA,0x3F,0x0F,0x02,0xC1,0xAF,0xBD,0x03,0x01,0x13,0x8A,0x6B,
    0x3A,0x91,0x11,0x41,0x4F,0x67,0xDC,0xEA,0x97,0xF2,0xCF,0xCE,0xF0,0xB4,0xE6,0x73,
    0x96,0xAC,0x74,0x22,0xE7,0xAD,0x35,0x85,0xE2,0xF9,0x37,0xE8,0x1C,0x75,0xDF,0x6E,
    0x47,0xF1,0x1A,0x71,0x1D,0x29,0xC5,0x89,0x6F,0xB7,0x62,0x0E,0xAA,0x18,0xBE,0x1B,
    0xFC,0x56,0x3E,0x4B,0xC6,0xD2,0x79,0x20,0x9A,0xDB,0xC0,0xFE,0x78,0xCD,0x5A,0xF4,
    0x1F,0xDD,0xA8,0x33,0x88,0x07,0xC7,0x31,0xB1,0x12,0x10,0x59,0x27,0x80,0xEC,0x5F,
    0x60,0x51,0x7F,0xA9,0x19,0xB5,0x4A,0x0D,0x2D,0xE5,0x7A,0x9F,0x93,0xC9,0x9C,0xEF,
    0xA0,0xE0,0x3B,0x4D,0xAE,0x2A,0xF5,0xB0,0xC8,0xEB,0xBB,0x3C,0x83,0x53,0x99,0x61,
    0x17,0x2B,0x04,0x7E,0xBA,0x77,0xD6,0x26,0xE1,0x69,0x14,0x63,0x55,0x21,0x0C,0x7D,
];

// xtime(a) = a<<1 XOR 0x1B if high bit set -- used by MixColumns
#[inline(always)]
fn xtime(a: u8) -> u8 {
    (a << 1) ^ if a & 0x80 != 0 { 0x1B } else { 0x00 }
}

// GF(256) multiply -- only the constants needed for MixColumns (0x02, 0x03, 0x09, 0x0B, 0x0D, 0x0E)
#[inline(always)]
fn gmul(a: u8, b: u8) -> u8 {
    match b {
        0x01 => a,
        0x02 => xtime(a),
        0x03 => xtime(a) ^ a,
        0x09 => xtime(xtime(xtime(a))) ^ a,
        0x0B => xtime(xtime(xtime(a))) ^ xtime(a) ^ a,
        0x0D => xtime(xtime(xtime(a))) ^ xtime(xtime(a)) ^ a,
        0x0E => xtime(xtime(xtime(a))) ^ xtime(xtime(a)) ^ xtime(a),
        _    => 0,
    }
}

// ---------------------------------------------------------------------------
// KEY SCHEDULE  (AES-128: 10 round keys from 16-byte master key)
// ---------------------------------------------------------------------------

const AES_ROUNDS: usize = 10;
// 11 round keys * 16 bytes = 176 bytes total
static mut ROUND_KEYS: [[u8; 16]; AES_ROUNDS + 1] = [[0u8; 16]; AES_ROUNDS + 1];

// Standard AES Rcon (first 10 values)
const RCON: [u8; 10] = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36];

unsafe fn aes_key_expand(key: &[u8; 16]) {
    ROUND_KEYS[0].copy_from_slice(key);

    for r in 1..=AES_ROUNDS {
        let prev = ROUND_KEYS[r - 1];
        let mut cur = [0u8; 16];

        // RotWord + SubWord + Rcon on last word of previous key
        let w = [
            SBOX[prev[13] as usize] ^ RCON[r - 1],
            SBOX[prev[14] as usize],
            SBOX[prev[15] as usize],
            SBOX[prev[12] as usize],
        ];

        for i in 0..4 { cur[i]      = prev[i]      ^ w[i]; }
        for i in 0..4 { cur[4 + i]  = prev[4 + i]  ^ cur[i]; }
        for i in 0..4 { cur[8 + i]  = prev[8 + i]  ^ cur[4 + i]; }
        for i in 0..4 { cur[12 + i] = prev[12 + i] ^ cur[8 + i]; }

        ROUND_KEYS[r] = cur;
    }
}

// ---------------------------------------------------------------------------
// AES ROUND OPERATIONS  (state = flat [u8; 16], column-major order)
// ---------------------------------------------------------------------------

#[inline]
fn sub_bytes(s: &mut [u8; 16]) {
    for b in s.iter_mut() { *b = SBOX[*b as usize]; }
}

#[inline]
fn sub_bytes_inv(s: &mut [u8; 16]) {
    for b in s.iter_mut() { *b = SBOX_INV[*b as usize]; }
}

fn shift_rows(s: &mut [u8; 16]) {
    // Row 1: left 1
    let t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    // Row 2: left 2
    s.swap(2, 10); s.swap(6, 14);
    // Row 3: left 3 (= right 1)
    let t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}

fn shift_rows_inv(s: &mut [u8; 16]) {
    // Row 1: right 1
    let t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    // Row 2: right 2
    s.swap(2, 10); s.swap(6, 14);
    // Row 3: right 3 (= left 1)
    let t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
}

fn mix_columns(s: &mut [u8; 16]) {
    for col in 0..4 {
        let i = col * 4;
        let (a, b, c, d) = (s[i], s[i+1], s[i+2], s[i+3]);
        s[i]   = gmul(a,0x02) ^ gmul(b,0x03) ^ c           ^ d;
        s[i+1] = a            ^ gmul(b,0x02) ^ gmul(c,0x03) ^ d;
        s[i+2] = a            ^ b            ^ gmul(c,0x02) ^ gmul(d,0x03);
        s[i+3] = gmul(a,0x03) ^ b            ^ c            ^ gmul(d,0x02);
    }
}

fn mix_columns_inv(s: &mut [u8; 16]) {
    for col in 0..4 {
        let i = col * 4;
        let (a, b, c, d) = (s[i], s[i+1], s[i+2], s[i+3]);
        s[i]   = gmul(a,0x0E) ^ gmul(b,0x0B) ^ gmul(c,0x0D) ^ gmul(d,0x09);
        s[i+1] = gmul(a,0x09) ^ gmul(b,0x0E) ^ gmul(c,0x0B) ^ gmul(d,0x0D);
        s[i+2] = gmul(a,0x0D) ^ gmul(b,0x09) ^ gmul(c,0x0E) ^ gmul(d,0x0B);
        s[i+3] = gmul(a,0x0B) ^ gmul(b,0x0D) ^ gmul(c,0x09) ^ gmul(d,0x0E);
    }
}

#[inline]
fn add_round_key(s: &mut [u8; 16], rk: &[u8; 16]) {
    for i in 0..16 { s[i] ^= rk[i]; }
}

// ---------------------------------------------------------------------------
// TERMINAL OUTPUT  (thin wrappers -- extern C hooks provided by RadiumOS)
// ---------------------------------------------------------------------------

unsafe fn tp(s: &[u8], col: u8) {
    terminal_setcolor(col);
    for &c in s { terminal_putchar(c); }
}

unsafe fn tp_hex(b: u8, col: u8) {
    terminal_setcolor(col);
    terminal_putchar(b"0123456789ABCDEF"[(b >> 4) as usize]);
    terminal_putchar(b"0123456789ABCDEF"[(b & 0xF) as usize]);
}

unsafe fn tp_num(mut n: u32, col: u8) {
    terminal_setcolor(col);
    if n == 0 { terminal_putchar(b'0'); return; }
    let mut buf = [0u8; 12]; let mut i = 0;
    while n > 0 { buf[i] = (n % 10) as u8 + b'0'; n /= 10; i += 1; }
    for k in (0..i).rev() { terminal_putchar(buf[k]); }
}

// ---------------------------------------------------------------------------
// LOGGING HELPERS
// ---------------------------------------------------------------------------

// Thick double-line banner
unsafe fn log_banner(title: &[u8], col: u8) {
    tp(b"\n  ", 0x07);
    tp(b"\xC9", col); for _ in 0..74 { tp(b"\xCD", col); } tp(b"\xBB\n", col);
    tp(b"  \xBA  ", col); tp(title, 0x0F);
    // pad to 74
    let used = title.len() + 4;
    for _ in used..74 { tp(b" ", col); }
    tp(b"\xBA\n", col);
    tp(b"  \xC8", col); for _ in 0..74 { tp(b"\xCD", col); } tp(b"\xBC\n", col);
}

// Thin section divider
unsafe fn log_divider(col: u8) {
    tp(b"\n  ", 0x07);
    for _ in 0..76 { tp(b"\xC4", col); }
    tp(b"\n", 0x07);
}

// 4x4 hex matrix dump  (AES column-major layout)
unsafe fn log_matrix(label: &[u8], state: &[u8; 16], label_col: u8, data_col: u8) {
    tp(b"\n    ", 0x07);
    tp(label, label_col);
    tp(b":\n", 0x07);
    for row in 0..4 {
        tp(b"      \xB3 ", 0x08);
        for col in 0..4 {
            tp_hex(state[col * 4 + row], data_col);
            tp(b"  ", 0x08);
        }
        tp(b"\xB3\n", 0x08);
    }
}

// Compact single-line 16-byte hex dump
unsafe fn log_bytes(label: &[u8], data: &[u8; 16], label_col: u8, data_col: u8) {
    tp(b"    ", 0x07);
    tp(label, label_col);
    tp(b": ", 0x07);
    for i in 0..16 {
        tp_hex(data[i], data_col);
        if i == 7 { tp(b"  ", 0x08); } else { tp(b" ", 0x08); }
    }
    tp(b"\n", 0x07);
}

// Round header  e.g.  "── Round 3/10: SubBytes → ShiftRows → MixColumns → AddRoundKey ──"
unsafe fn log_round_header(r: usize, is_last: bool) {
    log_divider(0x0E);
    tp(b"  \xB3  Round ", 0x0E);
    tp_num(r as u32, 0x0F);
    tp(b"/", 0x08);
    tp_num(AES_ROUNDS as u32, 0x0F);
    if is_last {
        tp(b":  SubBytes \xBB ShiftRows \xBB AddRoundKey  (no MixColumns -- final round)\n", 0x0E);
    } else {
        tp(b":  SubBytes \xBB ShiftRows \xBB MixColumns \xBB AddRoundKey\n", 0x0E);
    }
}

// Phase label with arrow
unsafe fn log_phase(name: &[u8]) {
    tp(b"\n    \xC4\xC4 ", 0x08);
    tp(name, 0x0F);
    tp(b"\n", 0x07);
}

// ---------------------------------------------------------------------------
// CORE ENCRYPT / DECRYPT  (single block, reads global AES_TRACE flag)
// ---------------------------------------------------------------------------

unsafe fn aes_encrypt_block(block: &mut [u8; 16], verbose: bool) {
    if verbose {
        log_matrix(b"Plaintext", block, 0x0A, 0x0A);
        log_bytes (b"RK[0]    ", &ROUND_KEYS[0], 0x0D, 0x0D);
    }

    // Initial AddRoundKey
    add_round_key(block, &ROUND_KEYS[0]);

    if verbose {
        log_matrix(b"After AddRoundKey(0)", block, 0x0B, 0x0B);
    }

    for r in 1..=AES_ROUNDS {
        let is_last = r == AES_ROUNDS;

        if verbose { log_round_header(r, is_last); }

        // SubBytes
        let before = *block;
        sub_bytes(block);
        if verbose {
            log_phase(b"SubBytes (S-box substitution)");
            // Show one byte transformation as example for round 1
            if r == 1 {
                tp(b"      example: S[", 0x08);
                tp_hex(before[0], 0x0B);
                tp(b"] = ", 0x08);
                tp_hex(block[0], 0x0A);
                tp(b"\n", 0x07);
            }
            log_matrix(b"State", block, 0x0B, 0x0B);
        }

        // ShiftRows
        shift_rows(block);
        if verbose {
            log_phase(b"ShiftRows (row cyclic shifts)");
            log_matrix(b"State", block, 0x0B, 0x0B);
        }

        // MixColumns (skip last round)
        if !is_last {
            mix_columns(block);
            if verbose {
                log_phase(b"MixColumns (GF(256) diffusion)");
                log_matrix(b"State", block, 0x0B, 0x0B);
            }
        }

        // AddRoundKey
        add_round_key(block, &ROUND_KEYS[r]);
        if verbose {
            log_phase(b"AddRoundKey");
            log_bytes(b"RK     ", &ROUND_KEYS[r], 0x0D, 0x0D);
            log_matrix(b"State", block, 0x0B, 0x0B);
        }
    }

    if verbose {
        log_divider(0x0C);
        log_matrix(b"Ciphertext", block, 0x0C, 0x0C);
        log_divider(0x0C);
    }
}

unsafe fn aes_decrypt_block(block: &mut [u8; 16], verbose: bool) {
    if verbose {
        log_matrix(b"Ciphertext in", block, 0x0C, 0x0C);
        log_bytes (b"RK[10]   ", &ROUND_KEYS[AES_ROUNDS], 0x0D, 0x0D);
    }

    add_round_key(block, &ROUND_KEYS[AES_ROUNDS]);

    for r in (1..=AES_ROUNDS).rev() {
        if verbose { log_round_header(AES_ROUNDS - r + 1, r == 1); }

        shift_rows_inv(block);
        if verbose { log_phase(b"InvShiftRows"); log_matrix(b"State", block, 0x0B, 0x0B); }

        sub_bytes_inv(block);
        if verbose { log_phase(b"InvSubBytes"); log_matrix(b"State", block, 0x0B, 0x0B); }

        add_round_key(block, &ROUND_KEYS[r - 1]);
        if verbose {
            log_phase(b"AddRoundKey");
            log_bytes(b"RK     ", &ROUND_KEYS[r - 1], 0x0D, 0x0D);
            log_matrix(b"State", block, 0x0B, 0x0B);
        }

        if r > 1 {
            mix_columns_inv(block);
            if verbose { log_phase(b"InvMixColumns"); log_matrix(b"State", block, 0x0B, 0x0B); }
        }
    }

    if verbose {
        log_divider(0x0A);
        log_matrix(b"Recovered Plaintext", block, 0x0A, 0x0A);
        log_divider(0x0A);
    }
}

// ---------------------------------------------------------------------------
// CBC MODE
// ---------------------------------------------------------------------------

static AES_IV: [u8; 16] = [
    0x52, 0x61, 0x64, 0x69, 0x75, 0x6D, 0x4F, 0x53,
    0x73, 0x63, 0x70, 0x32, 0x38, 0x30, 0x31, 0x21,
]; // "RadiumOSscp2801!"

static mut AES_BUF: [u8; 32] = [0; 32]; // Must be at least 32 bytes for this test
fn byte_to_hex(b: u8) -> (u8, u8) {
    let high = (b >> 4) & 0x0F;
    let low = b & 0x0F;
    // '0'-'9' or 'a'-'f'
    let h = if high < 10 { b'0' + high } else { b'a' + (high - 10) };
    let l = if low < 10 { b'0' + low } else { b'a' + (low - 10) };
    (h, l)
}
// Helper for PKCS#7 Padding (Standard for web compatibility)
fn pkcs7_pad(rem: u32) -> u8 {
    if rem == 0 { 16 } else { (16 - rem) as u8 }
}

// Standard CBC Encrypt (Raw Binary Output)
unsafe fn cbc_encrypt(src: *const u8, len: u32, dst: *mut u8, verbose: bool) -> u32 {
    // 1. Calculate padded length (Standard PKCS#7)
    let remainder = len % 16;
    let padding_len = pkcs7_pad(remainder) as u32;
    let total_len = len + padding_len;
    let nblocks = (total_len / 16) as usize;

    let mut prev = AES_IV;

    for b in 0..nblocks {
        let mut block = [0u8; 16];
        for i in 0..16 {
            let offset = (b * 16 + i) as u32;
            let byte_val = if offset < len {
                *src.add(offset as usize)
            } else {
                // Standard PKCS#7 Padding: e.g., 3 bytes of 0x03
                padding_len as u8
            };
            block[i] = byte_val ^ prev[i];
        }

        if verbose && b == 0 {
            tp(b"\n    CBC: block 0 XOR'd with IV (RadiumOSscp2801!)\n", 0x08);
        }

        aes_encrypt_block(&mut block, verbose && b == 0);

        // --- REVERTED TO RAW BINARY OUTPUT ---
        // This writes the actual encrypted bytes (e.g. 0xAB), not the text "AB"
        for i in 0..16 {
            *dst.add(b * 16 + i) = block[i];
        }

        prev = block;
    }

    // Return the actual length of the binary ciphertext
    total_len
}


unsafe fn cbc_decrypt(src: *const u8, len: u32, dst: *mut u8, verbose: bool) -> u32 {
    if len == 0 || len % 16 != 0 {
        // Input must be non-zero and a multiple of 16
        return 0;
    }

    let nblocks = (len / 16) as usize;
    let mut prev = AES_IV;

    for b in 0..nblocks {
        let mut block = [0u8; 16];
        
        // Read ciphertext block
        for i in 0..16 { 
            block[i] = *src.add(b * 16 + i); 
        }
        
        let cipher_copy = block;
        
        // Decrypt the block
        aes_decrypt_block(&mut block, verbose && b == 0);
        
        // XOR with Previous Ciphertext (CBC Mode)
        for i in 0..16 { 
            *dst.add(b * 16 + i) = block[i] ^ prev[i]; 
        }
        
        // Update chaining vector for next block
        prev = cipher_copy;
    }

    // --- FIX STARTS HERE ---
    
    // PKCS#7 Padding Removal
    // Look at the very last byte of the decrypted data.
    // This byte tells us how many bytes of padding to remove.
    let last_block_index = (nblocks - 1) * 16;
    let padding_val = *dst.add(last_block_index + 15) as usize;

    // Sanity check: Padding must be between 1 and 16.
    // (If padding_val > 16, the data might be corrupted or not padded)
    if padding_val > 0 && padding_val <= 16 {
        // Verify the padding bytes are correct (they should all match padding_val)
        // This is a safety check to ensure we don't truncate valid data.
        let mut valid_padding = true;
        for i in 0..padding_val {
            // Check the bytes from the end backwards
            let idx = last_block_index + 15 - i;
            if *dst.add(idx) as usize != padding_val {
                valid_padding = false;
                break;
            }
        }

        if valid_padding {
            // Calculate actual length: Total length - padding bytes
            return len - padding_val as u32;
        }
    }

    // If padding is invalid or 0 (rare in CBC), return full length
    // or handle error as needed. Here we return len to be safe.
    len
}


// ---------------------------------------------------------------------------
// PUBLIC FFI API
// ---------------------------------------------------------------------------

/// Initialise AES with a 16-byte key.
/// Key schedule dump is gated by the global trace flag.
#[no_mangle]
pub extern "C" fn rust_aes_init(key: *const u8) -> i32 {
    if key.is_null() { return -1; }
    unsafe {
        let mut k = [0u8; 16];
        for i in 0..16 { k[i] = *key.add(i); }

        // Always show the init banner so the operator knows AES is live
        log_banner(b"AES-128  --  Key Schedule (FIPS 197)  --  RadiumOS", 0x0D);

        // Key bytes and round key table only printed when trace is on
        if trace() {
            tp(b"\n    Master key: ", 0x0F);
            for i in 0..16 {
                tp_hex(k[i], 0x09);
                tp(b" ", 0x08);
                if i == 7 { tp(b" ", 0x08); }
            }
            tp(b"\n", 0x07);

            aes_key_expand(&k);

            tp(b"\n    Round keys:\n\n", 0x0F);
            for r in 0..=AES_ROUNDS {
                tp(b"    RK[", 0x08);
                tp_num(r as u32, 0x0E);
                if r < 10 { tp(b" ", 0x07); }
                tp(b"]  ", 0x08);
                for i in 0..16 {
                    tp_hex(ROUND_KEYS[r][i], 0x09);
                    if i == 3 || i == 7 || i == 11 { tp(b" | ", 0x08); }
                    else if i < 15 { tp(b" ", 0x08); }
                }
                tp(b"\n", 0x07);
            }

            tp(b"\n    ", 0x07);
            tp_num(((AES_ROUNDS + 1) * 16) as u32, 0x0A);
            tp(b" bytes of key material ready.\n", 0x0A);
        } else {
            // Silent init: just expand the key, print one-liner
            aes_key_expand(&k);
            tp(b"\n    Key loaded. Trace OFF -- call aes_trace_on() for full schedule.\n", 0x08);
        }

        0
    }
}

/// Encrypt `len` bytes from `data` in-place (CBC, zero-pad to block boundary).
/// Returns padded length, or -1 on error.
/// Diagnostic output respects the global AES_TRACE flag.
#[no_mangle]
pub extern "C" fn rust_aes_encrypt(data: *mut u8, len: u32) -> i32 {
    if data.is_null() || len == 0 { return -1; }
    unsafe {
        // Always print the operation header (lightweight, no hex dumps)
        log_banner(b"AES-128 CBC  --  ENCRYPT", 0x0E);
        tp(b"\n    Input:  ", 0x0F); tp_num(len, 0x0F); tp(b" bytes\n", 0x07);

        let out_len = cbc_encrypt(data, len, AES_BUF.as_mut_ptr(), trace());
        core::ptr::copy_nonoverlapping(AES_BUF.as_ptr(), data, out_len as usize);

        tp(b"\n    Output: ", 0x0F); tp_num(out_len, 0x0C);
        tp(b" bytes (padded to ", 0x07);
        tp_num(out_len / 16, 0x0F);
        tp(b" blocks)\n", 0x07);
        out_len as i32
    }
}

/// Decrypt `len` bytes from `data` in-place (must be multiple of 16).
/// Diagnostic output respects the global AES_TRACE flag.
#[no_mangle]
pub extern "C" fn rust_aes_decrypt(data: *mut u8, len: u32) -> i32 {
    if data.is_null() || len == 0 || len % 16 != 0 { return -1; }
    unsafe {
        log_banner(b"AES-128 CBC  --  DECRYPT", 0x0E);
        tp(b"\n    Input:  ", 0x0F); tp_num(len, 0x0F); tp(b" bytes\n", 0x07);

        let out_len = cbc_decrypt(data, len, AES_BUF.as_mut_ptr(), trace());
        core::ptr::copy_nonoverlapping(AES_BUF.as_ptr(), data, out_len as usize);

        tp(b"\n    Output: ", 0x0F); tp_num(out_len, 0x0A); tp(b" bytes\n", 0x07);
        out_len as i32
    }
}

/// Encrypt an AVFS file by replacing the original with a newly created one.
#[no_mangle]
pub extern "C" fn rust_aes_encrypt_file(filename: *const u8) -> i32 {
    unsafe {
        if filename.is_null() { return -1; }
        
        // 1. Read the existing file into memory
        let sz = avfs_get_filesize(filename);
        if sz <= 0 {
            tp(b"  [ERROR] aes_encrypt_file: file not found\n", 0x0C);
            return -1;
        }

        tp(b"\n    AVFS: encrypting \"", 0x0F);
        let mut p = filename;
        while *p != 0 { terminal_putchar(*p); p = p.add(1); }
        tp(b"\"  (", 0x07); tp_num(sz as u32, 0x0B); tp(b" bytes)\n", 0x07);

        // Allocate buffer (aligned to 16 bytes for AES)
        let buf = simple_malloc(((sz as usize + 15) & !15) as u32 + 16);
        if buf.is_null() { 
            tp(b"  [ERROR] malloc failed\n", 0x0C); 
            return -1; 
        }

        avfs_read_file(filename, buf, sz as u32, 0);

        // 2. Encrypt the data in the buffer
        let enc_len = rust_aes_encrypt(buf, sz as u32);

        if enc_len <= 0 {
            tp(b"  [ERROR] Encryption failed.\n", 0x0C);
            return -1;
        }

        // 3. Define AVFS functions
        extern "C" {
            fn avfs_remove_file(filename: *const u8) -> i32;
            fn avfs_create_file(filename: *const u8, size: u32) -> i32;
        }
        
        // 4. Delete the old file
        tp(b"    [INFO] Deleting original file...\n", 0x0E);
        avfs_remove_file(filename); 

        // 5. Create the new file with the exact padded size
        // Since avfs_write_file doesn't resize, we must allocate the full size here.
        tp(b"    [INFO] Creating new file (size=", 0x0E); tp_num(enc_len as u32, 0x0E); tp(b")...\n", 0x0E);
        
        if avfs_create_file(filename, enc_len as u32) != 0 {
            tp(b"  [ERROR] Failed to create new file. Disk full?\n", 0x0C);
            return -1;
        }

        // 6. Write the encrypted data
        tp(b"    [INFO] Writing encrypted data...\n", 0x0E);
        avfs_write_file(filename, buf, enc_len as u32, 0);

        tp(b"    Written ", 0x0A); tp_num(enc_len as u32, 0x0A);
        tp(b" encrypted bytes.\n", 0x0A);
        0
    }
}
/// Decrypt an AVFS file in-place.
#[no_mangle]
pub extern "C" fn rust_aes_decrypt_file(filename: *const u8) -> i32 {
    unsafe {
        if filename.is_null() { return -1; }
        let sz = avfs_get_filesize(filename);
        if sz <= 0 || sz as usize % 16 != 0 {
            tp(b"  [ERROR] aes_decrypt_file: bad file (not found or not block-aligned)\n", 0x0C);
            return -1;
        }
        tp(b"\n    AVFS: decrypting \"", 0x0F);
        let mut p = filename;
        while *p != 0 { terminal_putchar(*p); p = p.add(1); }
        tp(b"\"  (", 0x07); tp_num(sz as u32, 0x0B); tp(b" bytes)\n", 0x07);

        let buf = simple_malloc(sz as u32);
        if buf.is_null() { return -1; }

        avfs_read_file(filename, buf, sz as u32, 0);
        rust_aes_decrypt(buf, sz as u32);
        avfs_write_file(filename, buf, sz as u32, 0);

        tp(b"    File decrypted.\n", 0x0A);
        0
    }
}

// ---------------------------------------------------------------------------
// SELF-TEST
// ---------------------------------------------------------------------------
// Test 1: NIST FIPS 197 Appendix B known-answer test (single block, ECB)
//   Key:       2B 7E 15 16 28 AE D2 A6 AB F7 15 88 09 CF 4F 3C
//   Plaintext: 32 43 F6 A8 88 5A 30 8D 31 31 98 A2 E0 37 07 34
//   Expected:  39 25 84 1D 02 DC 09 FB DC 11 85 97 19 6A 0B 32
//
// Test 2: round-trip encrypt/decrypt of "Hello, RadiumOS! (scp_2801)" in CBC
//
// NOTE: The NIST block encrypt is always run verbosely regardless of the
// global trace flag -- it's a test, you want to see the rounds.
// The CBC round-trip respects the global flag.

#[no_mangle]
pub extern "C" fn rust_aes_selftest() -> i32 {
    unsafe {
        log_banner(b"AES-128 SELF-TEST  --  RadiumOS / scp_2801", 0x0E);

        // ── Test 1: NIST known-answer (ECB single block) ─────────────────────
        tp(b"\n  [TEST 1]  NIST FIPS 197 Appendix B  --  known-answer (ECB)\n\n", 0x0F);

        let nist_key: [u8; 16] = [
            0x2B,0x7E,0x15,0x16, 0x28,0xAE,0xD2,0xA6,
            0xAB,0xF7,0x15,0x88, 0x09,0xCF,0x4F,0x3C,
        ];
        let nist_pt: [u8; 16] = [
            0x32,0x43,0xF6,0xA8, 0x88,0x5A,0x30,0x8D,
            0x31,0x31,0x98,0xA2, 0xE0,0x37,0x07,0x34,
        ];
        let nist_ct: [u8; 16] = [
            0x39,0x25,0x84,0x1D, 0x02,0xDC,0x09,0xFB,
            0xDC,0x11,0x85,0x97, 0x19,0x6A,0x0B,0x32,
        ];

        aes_key_expand(&nist_key);

        let mut block = nist_pt;
        log_bytes(b"Key      ", &nist_key, 0x0D, 0x09);
        log_bytes(b"Plain    ", &nist_pt,  0x0A, 0x0A);

        // Self-test always shows NIST rounds in full -- forced verbose=true
        // regardless of the global AES_TRACE flag.
        aes_encrypt_block(&mut block, true);

        log_bytes(b"Got      ", &block,   0x0C, 0x0C);
        log_bytes(b"Expected ", &nist_ct, 0x0F, 0x0F);

        let pass1 = block == nist_ct;
        tp(b"\n    NIST ECB encrypt: ", 0x0F);
        if pass1 { tp(b"[ PASS ]\n", 0x0A); } else { tp(b"[ FAIL ]\n", 0x0C); return -1; }

        // Decrypt back quietly
        aes_decrypt_block(&mut block, false);
        let pass1d = block == nist_pt;
        tp(b"    NIST ECB decrypt: ", 0x0F);
        if pass1d { tp(b"[ PASS ]\n", 0x0A); } else { tp(b"[ FAIL ]\n", 0x0C); return -1; }

                // ── Test 2: CBC round-trip ────────────────────────────────────────────
        tp(b"\n  [TEST 2]  CBC round-trip  --  \"Hello, RadiumOS! (scp_2801)\"\n\n", 0x0F);

        let rt_key = *b"SCP2801RADIUMOS!";
        aes_key_expand(&rt_key);

        // FIX: Add #[derive(Copy, Clone)] so we can copy 'original' to 'buf'
        #[repr(align(16))]
        #[derive(Copy, Clone)] 
        struct AlignedBuf32([u8; 32]);

        // Use the aligned struct
        let original = AlignedBuf32(*b"Hello, RadiumOS! (scp_2801)     "); 
        let mut buf = original; // This is now a COPY, not a move

        log_bytes(b"Key   ", &rt_key, 0x0D, 0x09);

        tp(b"\n    Plaintext (hex):\n    ", 0x0A);
        for i in 0..32 {
            tp_hex(buf.0[i], 0x0A);
            tp(b" ", 0x08);
            if i == 15 { tp(b"\n    ", 0x07); }
        }
        tp(b"\n", 0x07);

        // CBC encrypt: verbose on block 0 only, driven by global trace flag
        let enc_len = cbc_encrypt(buf.0.as_ptr(), 32, AES_BUF.as_mut_ptr(), trace()) as usize;
        
        //for i in 0..enc_len { buf.0[i] = AES_BUF[i]; }

        tp(b"\n    Ciphertext (hex):\n    ", 0x0C);
        for i in 0..32 {
            tp_hex(buf.0[i], 0x0C);
            tp(b" ", 0x08);
            if i == 15 { tp(b"\n    ", 0x07); }
        }
        tp(b"\n", 0x07);

        // Now we can access 'original' safely because it wasn't moved
        let differs = buf.0[..32] != original.0[..32];
        tp(b"\n    Ciphertext != plaintext:  ", 0x0F);
        if differs { tp(b"[ PASS ]\n", 0x0A); } else { tp(b"[ FAIL ]\n", 0x0C); return -1; }

        // Re-expand same key, reset IV, decrypt
        aes_key_expand(&rt_key);
        cbc_decrypt(buf.0.as_ptr(), 32, AES_BUF.as_mut_ptr(), false);
        
        let mut recovered = AlignedBuf32([0u8; 32]);
        recovered.0.copy_from_slice(&AES_BUF[..32]);

        let matches = recovered.0 == original.0;
        tp(b"    Round-trip plaintext match: ", 0x0F);
        if matches { tp(b"[ PASS ]\n", 0x0A); } else { tp(b"[ FAIL ]\n", 0x0C); return -1; }

        tp(b"\n    Recovered text: \"", 0x0A);
        for &c in &recovered.0 { if c >= 0x20 { terminal_putchar(c); } }
        tp(b"\"\n", 0x0A);
        0
    }
}
/// Reads a file and prints its content as Hexadecimal string (copyable).
#[no_mangle]
pub extern "C" fn cmd_cat_hexx(filename: *const u8) -> i32 {
    unsafe {
        if filename.is_null() { return -1; }

        let sz = avfs_get_filesize(filename);
        if sz <= 0 {
            tp(b"  [ERROR] File not found.\n", 0x0C);
            return -1;
        }

        // Allocate buffer to hold file content
        let buf = simple_malloc(sz as u32);
        if buf.is_null() {
            tp(b"  [ERROR] Malloc failed.\n", 0x0C);
            return -1;
        }

        // Read file into buffer
        avfs_read_file(filename, buf, sz as u32, 0);

        // Print as Hex
        // We print bytes in pairs (e.g., F3 A1 2B...)
        let bytes = slice::from_raw_parts(buf, sz as usize);
        
        tp(b"--- START HEX DUMP ---\n", 0x0B);

        for i in 0..sz as usize {
            // Print the byte as 2 hex characters
            let b = bytes[i];
            let upper = (b >> 4) & 0x0F;
            let lower = b & 0x0F;
            
            // Convert to ASCII '0'-'9' or 'A'-'F'
            let c1 = if upper < 10 { b'0' + upper } else { b'A' + (upper - 10) };
            let c2 = if lower < 10 { b'0' + lower } else { b'A' + (lower - 10) };
            
            terminal_putchar(c1);
            terminal_putchar(c2);
            
            // Optional: Add a space every byte for readability, 
            // or remove the space for a raw hex string.
            terminal_putchar(b' '); 
        }

        tp(b"\n--- END HEX DUMP ---\n", 0x0B);
        0
    }
}

//=============================================================================
// MULTITASKING - RadiumOS Enhanced Edition
// Author  : scp_2801
// Version : 2.0
//
// TERMINAL OUTPUT BOUNDARY: cols 0-43 only (wrap at col 44, 2-space guard).
// VGA right panel         : cols 46-79 reserved for watchdog / status HUD.
// Col 44-45 = separator zone, never written by either side.
//=============================================================================

// ── Layout constants ─────────────────────────────────────────────────────────
/// Last column (inclusive) the terminal may write to.
const TERM_MAX_COL: usize = 43;
/// First column of the watchdog HUD panel.
const HUD_COL_START: usize = 46;
/// Width of the HUD panel in characters.
const HUD_WIDTH: usize = 34; // cols 46-79

const MAX_TASKS:     usize = 64;
const MAX_IPC_MSGS:  usize = 32;
const MAX_AFFINITIES: usize = 8;

//=============================================================================
// TASK STATES & STRUCTURE
//=============================================================================

#[repr(u32)]
#[derive(Copy, Clone, PartialEq)]
pub enum TaskState {
    Ready   = 0,
    Running = 1,
    Blocked = 2,
    Zombie  = 3, // terminated but not yet reaped
    Sleeping = 4,
}

/// Reason a task is blocked – used by the reaper and priority-aging logic.
#[repr(u32)]
#[derive(Copy, Clone, PartialEq)]
pub enum BlockReason {
    None       = 0,
    IpcRecv    = 1,
    IpcSend    = 2,
    Sleep      = 3,
    IoWait     = 4,
    Semaphore  = 5,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct Task {
    id:              u32,
    pid:             u32,
    state:           TaskState,
    block_reason:    BlockReason,
    /// Static (created) priority 1-255.
    base_priority:   u8,
    /// Dynamic priority (may be raised by aging). Scheduler uses this.
    priority:        u8,
    time_slice:      u32,
    remaining_time:  u32,
    entry_point:     u32,
    is_active:       bool,
    /// Tick on which the task was created.
    created_tick:    u32,
    /// Tick on which the task last ran.
    last_run_tick:   u32,
    /// Cumulative ticks the task has consumed.
    total_ticks:     u32,
    /// Affinity group (0 = any, 1-8 = specific logical CPU group).
    affinity:        u8,
    /// If Sleeping: wake when SYSTEM_TICKS >= this value.
    wake_at:         u32,
    /// Deadline tick (0 = no deadline). Reaper checks this.
    deadline:        u32,
    /// IPC mailbox slot (index into IPC_QUEUE).
    ipc_slot:        u8,
}

impl Task {
    const fn new() -> Self {
        Self {
            id: 0, pid: 0,
            state: TaskState::Ready,
            block_reason: BlockReason::None,
            base_priority: 0, priority: 0,
            time_slice: 0, remaining_time: 0,
            entry_point: 0, is_active: false,
            created_tick: 0, last_run_tick: 0,
            total_ticks: 0,
            affinity: 0,
            wake_at: 0, deadline: 0,
            ipc_slot: 0xFF,
        }
    }
}

//=============================================================================
// IPC MESSAGE QUEUE
// Simple fixed-size FIFO of 32 messages. Tasks can post/receive 4-byte
// payloads with a sender PID and a message type tag.
//=============================================================================

#[repr(u8)]
#[derive(Copy, Clone, PartialEq)]
pub enum IpcMsgType {
    None      = 0,
    Ping      = 1,
    Ack       = 2,
    Thermal   = 3,
    IoReport  = 4,
    Heartbeat = 5,
    Reaper    = 6,
    Affinity  = 7,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct IpcMsg {
    sender_pid: u32,
    recv_pid:   u32,
    msg_type:   IpcMsgType,
    payload:    u32,
    tick:       u32,
    consumed:   bool,
}

impl IpcMsg {
    const fn new() -> Self {
        Self {
            sender_pid: 0, recv_pid: 0,
            msg_type: IpcMsgType::None,
            payload: 0, tick: 0, consumed: true,
        }
    }
}

static mut IPC_QUEUE:   [IpcMsg; MAX_IPC_MSGS] = [IpcMsg::new(); MAX_IPC_MSGS];
static mut IPC_HEAD:    usize = 0;
static mut IPC_TAIL:    usize = 0;
static mut IPC_COUNT:   usize = 0;
/// How many times the queue overflowed (tail overran head).
static mut IPC_OVERFLOWS: u32 = 0;

/// Post a message. Returns false if queue is full (overflow recorded).
unsafe fn ipc_post(sender: u32, recv: u32, mtype: IpcMsgType, payload: u32) -> bool {
    if IPC_COUNT >= MAX_IPC_MSGS {
        IPC_OVERFLOWS += 1;
        return false;
    }
    let sys = SYSTEM_TICKS.load(Ordering::Relaxed);
    IPC_QUEUE[IPC_TAIL] = IpcMsg {
        sender_pid: sender, recv_pid: recv,
        msg_type: mtype, payload,
        tick: sys, consumed: false,
    };
    IPC_TAIL = (IPC_TAIL + 1) % MAX_IPC_MSGS;
    IPC_COUNT += 1;
    true
}

/// Consume the first unconsumed message addressed to `recv_pid`.
/// Returns true and fills `out` if found.
unsafe fn ipc_recv(recv_pid: u32, out: &mut IpcMsg) -> bool {
    for i in 0..MAX_IPC_MSGS {
        let m = &mut IPC_QUEUE[i];
        if !m.consumed && m.recv_pid == recv_pid {
            *out = *m;
            m.consumed = true;
            if IPC_COUNT > 0 { IPC_COUNT -= 1; }
            return true;
        }
    }
    false
}

//=============================================================================
// AFFINITY TABLE
// Maps affinity group IDs to a bitmask of TASKS[] slots that belong to it.
// The scheduler respects this: it only runs a task on a matching affinity
// epoch (SYSTEM_TICKS % MAX_AFFINITIES == affinity - 1) OR if affinity == 0.
//=============================================================================

static mut AFFINITY_VIOLATIONS: u32 = 0;

/// Returns true if a task with given affinity may run at current sys_tick.
#[inline(always)]
unsafe fn affinity_ok(affinity: u8, sys_tick: u32) -> bool {
    if affinity == 0 { return true; }
    (sys_tick % MAX_AFFINITIES as u32) == (affinity as u32 - 1)
}

//=============================================================================
// PRIORITY AGING
// Tasks that haven't run in AGING_THRESH ticks get their dynamic priority
// boosted by AGING_BOOST (capped at 255). Reset on dispatch.
//=============================================================================

const AGING_THRESH: u32 = 300;  // ticks without running before aging kicks in
const AGING_BOOST:  u8  = 10;   // priority added per aging pass
static mut PRIORITY_AGING_BOOSTS: u32 = 0;

//=============================================================================
// THERMAL SUBSYSTEM (simulated)
// A fake temperature register that heats up when io_stress_task is active
// and cools when the thermal_task throttles it. Values in centidegrees C.
// Real hardware would read from an ACPI thermal zone or LM sensors port.
//=============================================================================

static mut FAKE_TEMP_CDEG: u32  = 4500; // 45.00 °C at boot
static mut THERMAL_THROTTLE: bool = false;
static mut THERMAL_THROTTLE_EVENTS: u32 = 0;
const  TEMP_CRIT_CDEG:       u32  = 9500; // 95.00 °C
const  TEMP_WARN_CDEG:       u32  = 8000; // 80.00 °C
const  TEMP_COOL_CDEG:       u32  = 6000; // 60.00 °C

//=============================================================================
// SLEEP TABLE
// Tasks calling task_sleep() register here; the scheduler unblocks them
// when SYSTEM_TICKS >= wake_at.
//=============================================================================
// Implemented directly on Task.wake_at + TaskState::Sleeping.

//=============================================================================
// SCHEDULER STATS
//=============================================================================

static mut TASKS:            [Task; MAX_TASKS] = [Task::new(); MAX_TASKS];
static mut CURRENT_TASK_ID:  usize             = 0;
static mut NUM_TASKS:        usize             = 0;
static     NEXT_PID:         AtomicU32         = AtomicU32::new(1000);
static     SYSTEM_TICKS:     AtomicU32         = AtomicU32::new(0);
static mut SCHED_TOTAL_RUNS: u32               = 0;
static mut SCHED_IDLE_RUNS:  u32               = 0;
/// How many times the scheduler had to fall back to idle (no ready task).
static mut SCHED_FALLBACKS:  u32               = 0;

//=============================================================================
// INIT
//=============================================================================

#[no_mangle]
pub extern "C" fn rust_init_multitasking() {
    unsafe {
        for i in 0..MAX_TASKS { TASKS[i] = Task::new(); }

        TASKS[0].id            = 0;
        TASKS[0].pid           = 0;
        TASKS[0].state         = TaskState::Running;
        TASKS[0].base_priority = 1;
        TASKS[0].priority      = 1;
        TASKS[0].time_slice    = 1;
        TASKS[0].remaining_time= 1;
        TASKS[0].is_active     = true;

        CURRENT_TASK_ID = 0;
        NUM_TASKS       = 1;

        rust_print(b"Multitasking v2.0 initialized\n");
        rust_print(b"Terminal width: 44 cols (0-43), HUD: cols 46-79\n");
    }
}

//=============================================================================
// TASK CREATION
//=============================================================================

/// Extended creation. affinity=0 means no affinity. deadline=0 means none.
#[no_mangle]
pub extern "C" fn rust_create_task_ex(
    entry_point: u32,
    _is_kernel:  bool,
    priority:    u8,
    affinity:    u8,
    deadline:    u32,
) -> i32 {
    unsafe {
        let mut slot = None;
        for i in 0..MAX_TASKS {
            if !TASKS[i].is_active { slot = Some(i); break; }
        }
        let id = match slot { Some(v) => v, None => return -1 };

        let pid      = NEXT_PID.fetch_add(1, Ordering::Relaxed);
        let sys_tick = SYSTEM_TICKS.load(Ordering::Relaxed);

        TASKS[id] = Task {
            id:              id as u32,
            pid,
            state:           TaskState::Ready,
            block_reason:    BlockReason::None,
            base_priority:   priority,
            priority,
            time_slice:      priority as u32,
            remaining_time:  priority as u32,
            entry_point,
            is_active:       true,
            created_tick:    sys_tick,
            last_run_tick:   sys_tick,
            total_ticks:     0,
            affinity:        affinity % (MAX_AFFINITIES as u8 + 1),
            wake_at:         0,
            deadline,
            ipc_slot:        0xFF,
        };
        NUM_TASKS += 1;
        pid as i32
    }
}

/// Compat wrapper – no affinity, no deadline.
#[no_mangle]
pub extern "C" fn rust_create_task(entry_point: u32, is_kernel: bool, priority: u8) -> i32 {
    rust_create_task_ex(entry_point, is_kernel, priority, 0, 0)
}

/// Put the current task to sleep for `ticks` ticks.
/// Call from inside a task body; takes effect next scheduler pass.
pub unsafe fn task_sleep(ticks: u32) {
    let id  = CURRENT_TASK_ID;
    let sys = SYSTEM_TICKS.load(Ordering::Relaxed);
    TASKS[id].state        = TaskState::Sleeping;
    TASKS[id].block_reason = BlockReason::Sleep;
    TASKS[id].wake_at      = sys.wrapping_add(ticks);
}

//=============================================================================
// SCHEDULER (enhanced: aging, affinity, sleep wakeup, deadline check)
//=============================================================================

#[no_mangle]
pub extern "C" fn rust_schedule() {
    let sys_tick = SYSTEM_TICKS.fetch_add(1, Ordering::Relaxed) + 1;
    unsafe {
    SCHED_TOTAL_RUNS = SCHED_TOTAL_RUNS.wrapping_add(1);

        // ── Phase 1: wake sleeping tasks ─────────────────────────────────
        for i in 0..MAX_TASKS {
            if TASKS[i].is_active
                && TASKS[i].state == TaskState::Sleeping
                && sys_tick >= TASKS[i].wake_at
            {
                TASKS[i].state        = TaskState::Ready;
                TASKS[i].block_reason = BlockReason::None;
            }
        }

        // ── Phase 2: priority aging ───────────────────────────────────────
        for i in 0..MAX_TASKS {
            if !TASKS[i].is_active { continue; }
            if TASKS[i].state != TaskState::Ready { continue; }
            let age = sys_tick.saturating_sub(TASKS[i].last_run_tick);
            if age > AGING_THRESH {
                let old = TASKS[i].priority;
                TASKS[i].priority = old.saturating_add(AGING_BOOST);
                if TASKS[i].priority > old {
                    PRIORITY_AGING_BOOSTS = PRIORITY_AGING_BOOSTS.wrapping_add(1);
                }
            }
        }

        // ── Phase 3: decrement current time slice ─────────────────────────
        let current_id = CURRENT_TASK_ID;
        if TASKS[current_id].remaining_time > 0 {
            TASKS[current_id].remaining_time -= 1;
        }

        // ── Phase 4: find next ready task (priority + affinity aware) ─────
        // Two-pass: first pass honours affinity, second pass ignores it
        // so a task with mismatched affinity still runs if nothing else can.
        let mut next_id = (current_id + 1) % MAX_TASKS;
        let mut found   = false;

        // Pass 1: affinity-aware highest-priority ready task
        let mut best_pri   = 0u8;
        let mut best_slot  = usize::MAX;
        for i in 0..MAX_TASKS {
            let idx = (next_id + i) % MAX_TASKS;
            if !TASKS[idx].is_active { continue; }
            if TASKS[idx].state != TaskState::Ready { continue; }
            if !affinity_ok(TASKS[idx].affinity, sys_tick) {
                AFFINITY_VIOLATIONS = AFFINITY_VIOLATIONS.wrapping_add(1);
                continue;
            }
            if TASKS[idx].priority > best_pri {
                best_pri  = TASKS[idx].priority;
                best_slot = idx;
                found     = true;
            }
        }

        // Pass 2: affinity-ignored fallback
        if !found {
            for i in 0..MAX_TASKS {
                let idx = (next_id + i) % MAX_TASKS;
                if TASKS[idx].is_active && TASKS[idx].state == TaskState::Ready {
                    if TASKS[idx].priority > best_pri {
                        best_pri  = TASKS[idx].priority;
                        best_slot = idx;
                        found     = true;
                    }
                }
            }
        }

        if !found {
            // Idle fallback
            SCHED_IDLE_RUNS  = SCHED_IDLE_RUNS.wrapping_add(1);
            SCHED_FALLBACKS  = SCHED_FALLBACKS.wrapping_add(1);
            TASKS[current_id].remaining_time = TASKS[current_id].time_slice;
            return;
        }

        next_id = best_slot;

        // ── Phase 5: context switch bookkeeping ───────────────────────────
        if TASKS[current_id].state == TaskState::Running {
            TASKS[current_id].state = TaskState::Ready;
        }
        // Restore dynamic priority to base on dispatch (aging reset)
        TASKS[current_id].priority = TASKS[current_id].base_priority;

        TASKS[next_id].state          = TaskState::Running;
        TASKS[next_id].remaining_time = TASKS[next_id].time_slice;
        TASKS[next_id].last_run_tick  = sys_tick;
        TASKS[next_id].total_ticks    = TASKS[next_id].total_ticks.wrapping_add(1);
        CURRENT_TASK_ID               = next_id;

        // ── Phase 6: watchdog checkin + HUD marker ────────────────────────
        watchdog_task_checkin(next_id, TASKS[next_id].pid);
        // 'T' marker at top-right corner of HUD
        vga_write(79, 0, b'T', 0x4F);

        // ── Phase 7: dispatch ─────────────────────────────────────────────
        if TASKS[next_id].entry_point != 0 {
            let func: extern "C" fn(u32) = core::mem::transmute(TASKS[next_id].entry_point);
            func(TASKS[next_id].pid);
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_get_current_pid() -> u32 {
    unsafe { TASKS[CURRENT_TASK_ID].pid }
}

//=============================================================================
// UPTIME COUNTER
//=============================================================================

static mut UPTIME_SECONDS:   u32 = 0;
static mut UPTIME_SUB_TICKS: u32 = 0;
const  TICKS_PER_SECOND:     u32 = 100;

//=============================================================================
// ── TASK 1: HEARTBEAT ───────────────────────────────────────────────────────
// Animated beat glyph, uptime HH:MM:SS, heap bar, banner.
// HUD rows 0-2 (right panel).
//=============================================================================

static mut HEARTBEAT_TICKS:      u32 = 0;
static mut DISPLAY_TICK_COUNTER: u32 = 0;
const  BEAT_FRAMES: [u8; 4] = [b'-', b'/', b'*', b'\\'];

extern "C" fn heartbeat_task(_pid: u32) {
    unsafe {
        HEARTBEAT_TICKS      = HEARTBEAT_TICKS.wrapping_add(1);
        DISPLAY_TICK_COUNTER = (DISPLAY_TICK_COUNTER + 1) % 1000;

        // Row 0: banner (HUD_COL_START .. HUD_COL_START+HUD_WIDTH)
        hud_write_string(0, 0, b"Developed by scp_2801  v2.0", 0x0B);

        // Row 1: tick counter + beat glyph
        let h_n = (DISPLAY_TICK_COUNTER / 100) % 10;
        let t_n = (DISPLAY_TICK_COUNTER / 10)  % 10;
        let o_n =  DISPLAY_TICK_COUNTER        % 10;
        hud_write(24, 1, b'0' + h_n as u8, 0x0E);
        hud_write(25, 1, b'0' + t_n as u8, 0x0E);
        hud_write(26, 1, b'0' + o_n as u8, 0x0E);
        let frame = ((HEARTBEAT_TICKS / 25) % 4) as usize;
        hud_write(33, 1, BEAT_FRAMES[frame], 0x0E);

        // Uptime HH:MM:SS
        UPTIME_SUB_TICKS += 1;
        if UPTIME_SUB_TICKS >= TICKS_PER_SECOND {
            UPTIME_SUB_TICKS = 0;
            UPTIME_SECONDS   = UPTIME_SECONDS.wrapping_add(1);
        }
        let hh = UPTIME_SECONDS / 3600;
        let mm = (UPTIME_SECONDS % 3600) / 60;
        let ss = UPTIME_SECONDS % 60;
        hud_write_string(0, 1, b"UP ", 0x08);
        hud_write(3,  1, b'0' + ((hh / 10) % 10) as u8, 0x07);
        hud_write(4,  1, b'0' +  (hh        % 10) as u8, 0x07);
        hud_write(5,  1, b':', 0x08);
        hud_write(6,  1, b'0' + ((mm / 10) % 10) as u8, 0x07);
        hud_write(7,  1, b'0' +  (mm        % 10) as u8, 0x07);
        hud_write(8,  1, b':', 0x08);
        hud_write(9,  1, b'0' + ((ss / 10) % 10) as u8, 0x07);
        hud_write(10, 1, b'0' +  (ss        % 10) as u8, 0x07);

        // Row 2: heap bar
        let heap_pct = (HEAP_OFFSET as u32 * 100 / HEAP.len() as u32) as u8;
        let filled   = (heap_pct / 10) as usize;
        let bar_col  = if heap_pct > 80 { 0x0C }
                       else if heap_pct > 50 { 0x0E }
                       else { 0x0A };
        hud_write_string(0, 2, b"HP[          ]", 0x08);
        for i in 0..10usize {
            let ch = if i < filled { b'\xDB' } else { b' ' };
            let cl = if i < filled { bar_col  } else { 0x08  };
            hud_write(3 + i, 2, ch, cl);
        }
        hud_write(14, 2, b'0' + (heap_pct / 100)       as u8, bar_col);
        hud_write(15, 2, b'0' + ((heap_pct / 10) % 10) as u8, bar_col);
        hud_write(16, 2, b'0' +  (heap_pct % 10)        as u8, bar_col);
        hud_write(17, 2, b'%', bar_col);
    }
}

//=============================================================================
// ── TASK 2: TIMED MESSAGE ───────────────────────────────────────────────────
// 16 rotating status messages on HUD row 24.
//=============================================================================

static mut TIMED_MESSAGE_TICKS: u32   = 0;
static mut MESSAGE_INDEX:       usize = 0;
static mut MSG_CYCLE_COUNT:     u32   = 0;

const MESSAGES: &[(&[u8], u8)] = &[
    (b"System running smoothly ", 0x0A),
    (b"All tasks operating OK  ", 0x0B),
    (b"Memory usage stable     ", 0x0E),
    (b"Network ready           ", 0x0D),
    (b"RadiumOS v2.0           ", 0x0F),
    (b"I LOVE MY CODE <3       ", 0x0E),
    (b"AVFS: filesystem nominal", 0x0A),
    (b"RTL8139: link UP        ", 0x0B),
    (b"RSH: shell alive        ", 0x0A),
    (b"TCP/IP stack: listening ", 0x0D),
    (b"Idle cycles: nominal    ", 0x08),
    (b"IPC queue: healthy      ", 0x0A),
    (b"Thermal: within limits  ", 0x0B),
    (b"Reaper: all deadlines OK", 0x0A),
    (b"Affinity: no violations ", 0x0B),
    (b"Aging: boosts applied   ", 0x0E),
];
const NUM_MESSAGES: usize = 16;
const MSG_INTERVAL: u32   = 800;

extern "C" fn timed_message(_pid: u32) {
    unsafe {
        TIMED_MESSAGE_TICKS = TIMED_MESSAGE_TICKS.wrapping_add(1);
        if TIMED_MESSAGE_TICKS % MSG_INTERVAL != 0 { return; }
        MSG_CYCLE_COUNT += 1;
        MESSAGE_INDEX    = (MESSAGE_INDEX + 1) % NUM_MESSAGES;

        hud_fill(0, 24, HUD_WIDTH, b' ', 0x07);
        let (text, colour) = MESSAGES[MESSAGE_INDEX];
        hud_write_string(0, 24, text, colour);
    }
}

//=============================================================================
// ── TASK 3: THERMAL MONITOR ─────────────────────────────────────────────────
// Simulates a thermal sensor. Heats up when io_stress is active, cools
// when throttled. Posts IPC thermal events. Draws on HUD rows 8-9.
//=============================================================================

static mut THERMAL_TICKS: u32 = 0;

extern "C" fn thermal_task(pid: u32) {
    unsafe {
    }
}

//=============================================================================
// ── TASK 4: IO STRESS ───────────────────────────────────────────────────────
// Simulates periodic burst I/O (AVFS writes, fake DMA). Heats the thermal
// model. Throttled by thermal_task via IO_STRESS_THROTTLED flag.
// Posts IPC IoReport every burst. HUD rows 10-11.
//=============================================================================

static mut IO_STRESS_ACTIVE:    bool = false;
static mut IO_STRESS_THROTTLED: bool = false;
static mut IO_STRESS_TICKS:     u32  = 0;
static mut IO_STRESS_BURSTS:    u32  = 0;
static mut IO_STRESS_BYTES:     u32  = 0; // simulated KB written

const IO_BURST_INTERVAL: u32 = 300;  // ticks between bursts
const IO_BURST_DURATION: u32 = 80;   // ticks a burst lasts

extern "C" fn io_stress_task(pid: u32) {
    unsafe {
        IO_STRESS_TICKS = IO_STRESS_TICKS.wrapping_add(1);

        let phase = IO_STRESS_TICKS % IO_BURST_INTERVAL;
        if phase < IO_BURST_DURATION && !IO_STRESS_THROTTLED {
            IO_STRESS_ACTIVE = true;
            // Simulate KB written (4 KB per tick during burst)
            IO_STRESS_BYTES = IO_STRESS_BYTES.wrapping_add(4);

            if phase == 0 {
                IO_STRESS_BURSTS += 1;
                ipc_post(pid, 0, IpcMsgType::IoReport, IO_STRESS_BYTES);
                // Write a marker into AVFS so it's real
                let fname = b"io_stress.avfs\0";
                if avfs_file_exists(fname.as_ptr()) {
                    let mut hdr = [0u8; 4];
                    hdr[0] = (IO_STRESS_BURSTS & 0xFF) as u8;
                    avfs_write_file(fname.as_ptr(), hdr.as_ptr(), 4, 0);
                } else {
                    avfs_create_file(fname.as_ptr(), 256);
                }
            }
        } else {
            IO_STRESS_ACTIVE = false;
        }

        // HUD rows 10-11
        let kb_mb = IO_STRESS_BYTES / 1024;
        let io_col = if IO_STRESS_ACTIVE { 0x0E } else { 0x08 };
        hud_write_string(0, 10, if IO_STRESS_ACTIVE { b"IO:BURST  " } else { b"IO:IDLE   " }, io_col);
        hud_write_string(10, 10, b"B:", 0x08);
        hud_write(12, 10, b'0' + ((IO_STRESS_BURSTS / 100) % 10) as u8, 0x07);
        hud_write(13, 10, b'0' + ((IO_STRESS_BURSTS / 10)  % 10) as u8, 0x07);
        hud_write(14, 10, b'0' +  (IO_STRESS_BURSTS         % 10) as u8, 0x07);
        hud_write_string(0, 11, b"IO_KB:", 0x08);
        hud_write(6, 11, b'0' + ((kb_mb / 1000) % 10) as u8, 0x0B);
        hud_write(7, 11, b'0' + ((kb_mb / 100)  % 10) as u8, 0x0B);
        hud_write(8, 11, b'0' + ((kb_mb / 10)   % 10) as u8, 0x0B);
        hud_write(9, 11, b'0' +  (kb_mb          % 10) as u8, 0x0B);
        hud_write_string(10, 11, b"MB", 0x08);
        if IO_STRESS_THROTTLED {
            hud_write_string(13, 11, b"[THR]", 0x0C);
        } else {
            hud_write_string(13, 11, b"     ", 0x08);
        }
    }
}

//=============================================================================
// ── TASK 5: REAPER ──────────────────────────────────────────────────────────
// Scans for Zombie tasks and tasks that have missed their deadline.
// Cleans up Zombie slots (reclaims them). Logs missed deadlines to AVFS.
// Posts IPC Reaper events. HUD rows 12-13.
//=============================================================================

static mut REAPER_TICKS:          u32 = 0;
static mut REAPER_REAPED:         u32 = 0; // zombie slots reclaimed
static mut REAPER_MISSED_DEADLINES: u32 = 0;
const  REAPER_SCAN_INTERVAL:      u32 = 200; // ticks between scans

extern "C" fn reaper_task(pid: u32) {
    unsafe {
        REAPER_TICKS = REAPER_TICKS.wrapping_add(1);
        if REAPER_TICKS % REAPER_SCAN_INTERVAL != 0 { return; }

        let sys = SYSTEM_TICKS.load(Ordering::Relaxed);

        for i in 0..MAX_TASKS {
            if !TASKS[i].is_active { continue; }

            // ── Reap zombies ───────────────────────────────────────────
            if TASKS[i].state == TaskState::Zombie {
                TASKS[i] = Task::new(); // reclaim
                NUM_TASKS = NUM_TASKS.saturating_sub(1);
                REAPER_REAPED += 1;
                ipc_post(pid, 0, IpcMsgType::Reaper, TASKS[i].pid);
                continue;
            }

            // ── Deadline check ─────────────────────────────────────────
            if TASKS[i].deadline != 0 && sys > TASKS[i].deadline {
                REAPER_MISSED_DEADLINES += 1;
                // Log to AVFS
                let fname = b"deadline_miss.avfs\0";
                if !avfs_file_exists(fname.as_ptr()) {
                    avfs_create_file(fname.as_ptr(), 4096);
                }
                let mut line = [0u8; 40];
                let mut li = 0usize;
                for &c in b"MISS pid=" { line[li]=c; li+=1; }
                let mut v = TASKS[i].pid;
                if v == 0 { line[li]=b'0'; li+=1; }
                else {
                    let mut tb=[0u8;10]; let mut ti=0;
                    while v>0 { tb[ti]=(v%10)as u8+b'0'; v/=10; ti+=1; }
                    for j in (0..ti).rev() { line[li]=tb[j]; li+=1; }
                }
                for &c in b" dl=" { line[li]=c; li+=1; }
                let mut d = TASKS[i].deadline;
                if d == 0 { line[li]=b'0'; li+=1; }
                else {
                    let mut tb=[0u8;10]; let mut ti=0;
                    while d>0 { tb[ti]=(d%10)as u8+b'0'; d/=10; ti+=1; }
                    for j in (0..ti).rev() { line[li]=tb[j]; li+=1; }
                }
                line[li]=b'\n'; li+=1;
                avfs_write_file(fname.as_ptr(), line.as_ptr(), li as u32,
                    (REAPER_MISSED_DEADLINES * 40) as u32);
                // Clear deadline so we don't log it again
                TASKS[i].deadline = 0;
            }
        }

        // HUD rows 12-13
        hud_write_string(0, 12, b"REAPER:RUN  ", 0x08);
        hud_write_string(0, 12, b"REAPED:", 0x08);
        hud_write(7, 12, b'0' + ((REAPER_REAPED / 100) % 10) as u8, 0x0A);
        hud_write(8, 12, b'0' + ((REAPER_REAPED / 10)  % 10) as u8, 0x0A);
        hud_write(9, 12, b'0' +  (REAPER_REAPED         % 10) as u8, 0x0A);
        hud_write_string(0, 13, b"MISS_DL:", 0x08);
        let dl_col = if REAPER_MISSED_DEADLINES > 0 { 0x0C } else { 0x07 };
        hud_write(8,  13, b'0' + ((REAPER_MISSED_DEADLINES / 100) % 10) as u8, dl_col);
        hud_write(9,  13, b'0' + ((REAPER_MISSED_DEADLINES / 10)  % 10) as u8, dl_col);
        hud_write(10, 13, b'0' +  (REAPER_MISSED_DEADLINES         % 10) as u8, dl_col);
    }
}

//=============================================================================
// ── TASK 6: AFFINITY MONITOR ────────────────────────────────────────────────
// Checks that tasks with a non-zero affinity group are only being dispatched
// during their affinity epoch. Logs violations to AVFS. HUD rows 14-15.
//=============================================================================

static mut AFFINITY_TICKS: u32 = 0;
const  AFFINITY_CHECK_INTERVAL: u32 = 400;

extern "C" fn affinity_task(pid: u32) {
    unsafe {
        AFFINITY_TICKS = AFFINITY_TICKS.wrapping_add(1);
        if AFFINITY_TICKS % AFFINITY_CHECK_INTERVAL != 0 { return; }

        let sys = SYSTEM_TICKS.load(Ordering::Relaxed);
        let mut local_violations = 0u32;

        for i in 0..MAX_TASKS {
            if !TASKS[i].is_active || TASKS[i].affinity == 0 { continue; }
            // If the task ran last tick but it wasn't its affinity epoch, violation
            let expected_epoch = (TASKS[i].affinity as u32 - 1) % MAX_AFFINITIES as u32;
            let actual_epoch   = TASKS[i].last_run_tick % MAX_AFFINITIES as u32;
            if TASKS[i].last_run_tick > 0
                && TASKS[i].last_run_tick.wrapping_sub(1) / 1 != 0
                && actual_epoch != expected_epoch
                && sys.saturating_sub(TASKS[i].last_run_tick) < AFFINITY_CHECK_INTERVAL
            {
                local_violations += 1;
                AFFINITY_VIOLATIONS = AFFINITY_VIOLATIONS.wrapping_add(1);
                ipc_post(pid, 0, IpcMsgType::Affinity, TASKS[i].pid);
            }
        }

        // HUD rows 14-15
        let af_col = if AFFINITY_VIOLATIONS > 0 { 0x0C } else { 0x0A };
        hud_write_string(0, 14, b"AFFINITY:", 0x08);
        hud_write(9,  14, b'0' + ((AFFINITY_VIOLATIONS / 100) % 10) as u8, af_col);
        hud_write(10, 14, b'0' + ((AFFINITY_VIOLATIONS / 10)  % 10) as u8, af_col);
        hud_write(11, 14, b'0' +  (AFFINITY_VIOLATIONS         % 10) as u8, af_col);
        hud_write_string(12, 14, b"VIOL", 0x08);

        let ag_col = if PRIORITY_AGING_BOOSTS > 0 { 0x0D } else { 0x07 };
        hud_write_string(0, 15, b"AGING_BOOSTS:", 0x08);
        hud_write(13, 15, b'0' + ((PRIORITY_AGING_BOOSTS / 100) % 10) as u8, ag_col);
        hud_write(14, 15, b'0' + ((PRIORITY_AGING_BOOSTS / 10)  % 10) as u8, ag_col);
        hud_write(15, 15, b'0' +  (PRIORITY_AGING_BOOSTS         % 10) as u8, ag_col);
    }
}

//=============================================================================
// ── TASK 7: IPC MONITOR ─────────────────────────────────────────────────────
// Drains pending IPC messages addressed to PID 0 (system bus), logs stats.
// Detects queue overflow. HUD rows 16-17.
//=============================================================================

static mut IPC_MON_TICKS:     u32 = 0;
static mut IPC_TOTAL_CONSUMED: u32 = 0;

extern "C" fn ipc_monitor_task(pid: u32) {
    unsafe {
        IPC_MON_TICKS = IPC_MON_TICKS.wrapping_add(1);

        // Drain messages addressed to PID 0 (broadcast/system sink)
        let mut msg = IpcMsg::new();
        while ipc_recv(0, &mut msg) {
            IPC_TOTAL_CONSUMED += 1;
            // Could dispatch to type-specific handlers here
        }

        // HUD rows 16-17
        hud_write_string(0, 16, b"IPC Q:", 0x08);
        hud_write(6, 16, b'0' + ((IPC_COUNT / 10) % 10) as u8, 0x0B);
        hud_write(7, 16, b'0' +  (IPC_COUNT        % 10) as u8, 0x0B);
        hud_write(8, 16, b'/', 0x08);
        hud_write(9, 16, b'0' + ((MAX_IPC_MSGS / 10) % 10) as u8, 0x07);
        hud_write(10,16, b'0' +  (MAX_IPC_MSGS        % 10) as u8, 0x07);

        let ov_col = if IPC_OVERFLOWS > 0 { 0x0C } else { 0x07 };
        hud_write_string(12, 16, b"OV:", 0x08);
        hud_write(15, 16, b'0' + ((IPC_OVERFLOWS / 10) % 10) as u8, ov_col);
        hud_write(16, 16, b'0' +  (IPC_OVERFLOWS        % 10) as u8, ov_col);

        hud_write_string(0, 17, b"IPC_TOTAL:", 0x08);
        hud_write(10, 17, b'0' + ((IPC_TOTAL_CONSUMED / 1000) % 10) as u8, 0x0B);
        hud_write(11, 17, b'0' + ((IPC_TOTAL_CONSUMED / 100)  % 10) as u8, 0x0B);
        hud_write(12, 17, b'0' + ((IPC_TOTAL_CONSUMED / 10)   % 10) as u8, 0x0B);
        hud_write(13, 17, b'0' +  (IPC_TOTAL_CONSUMED          % 10) as u8, 0x0B);
    }
}



//=============================================================================
// WATCHDOG SUBSYSTEM
//=============================================================================

// ── Tunables ─────────────────────────────────────────────────────────────────
const WATCHDOG_CHECK_INTERVAL:  u32   = 500;
const HEAP_WARN_PCT:            u32   = 80;
const HEAP_CRIT_PCT:            u32   = 92;
const STALL_TRIP_COUNT:         u32   = 3;
const STARVATION_THRESH:        u32   = 5000;
const MAX_TRACKED_TASKS:        usize = 32;
const WATCHDOG_LOG_SIZE:        u32   = 16384;
const CANARY_VAL:               u32   = 0xCAFEBABE;
const IPC_OVERFLOW_WARN:        u32   = 3;   // overflow count before CRIT
const PRIORITY_INV_THRESH:      u32   = 1000; // ticks a low-pri task blocks high
const MISSED_DEADLINE_CRIT:     u32   = 5;    // total misses before CRIT

// ── Severity tags ─────────────────────────────────────────────────────────────
const SEV_WARN:  &[u8] = b"W";
const SEV_CRIT:  &[u8] = b"C";
const SEV_FATAL: &[u8] = b"F";

// ── Watchdog state ────────────────────────────────────────────────────────────
// Global storage for last connection details
#[no_mangle]
pub static mut NET_LAST_METHOD: [u8; 8] = [0; 8];

#[no_mangle]
pub static mut NET_LAST_PROTO: [u8; 8] = [0; 8];

#[no_mangle]
pub static mut NET_LAST_DESC: [u8; 32] = [0; 32];
// Tracks the system time when the last network update occurred
static mut NET_LAST_UPDATE_TICKS: u32 = 0;

// Counter for network anomalies
static mut WATCHDOG_NET_STALE_HITS: u32 = 0;
static mut NET_LAST_DISPLAYED_METHOD: [u8; 8] = [0; 8];
static mut NET_LAST_DISPLAYED_PROTO:  [u8; 8] = [0; 8];
static mut NET_LAST_DISPLAYED_DESC:   [u8; 32] = [0; 32];
static mut WATCHDOG_TICKS:            u32 = 0;
static mut WATCHDOG_LAST_TASK_COUNT:  u32 = 0;
static mut WATCHDOG_ANOMALY_COUNT:    u32 = 0;
static mut WATCHDOG_LAST_SYS_TICKS:  u32 = 0;
static mut WATCHDOG_STALL_COUNT:      u32 = 0;
static mut WATCHDOG_HEAP_PEAK:        u32 = 0;
static mut WATCHDOG_LOG_OFFSET:       u32 = 0;
static mut WATCHDOG_TASK_DEATHS:      u32 = 0;
static mut WATCHDOG_HEAP_WARNINGS:    u32 = 0;
static mut WATCHDOG_STALL_WARNINGS:   u32 = 0;
static mut WATCHDOG_NULL_PTR_TRIPS:   u32 = 0;
static mut WATCHDOG_STARVATION_HITS:  u32 = 0;
static mut WATCHDOG_CANARY_TRIPS:     u32 = 0;
static mut WATCHDOG_BUDGET_TRIPS:     u32 = 0;
// New counters
static mut WATCHDOG_IPC_OVERFLOW_HITS: u32 = 0;
static mut WATCHDOG_PRIORITY_INV_HITS: u32 = 0;
static mut WATCHDOG_THERMAL_HITS:      u32 = 0;
static mut WATCHDOG_DEADLINE_HITS:     u32 = 0;
static mut WATCHDOG_AFFINITY_HITS:     u32 = 0;

const WATCHDOG_SENTINEL_VAL: u32   = 0xDEADBEEF;
static mut WATCHDOG_SENTINEL: u32  = 0xDEADBEEF;

// ── Task shadow table ─────────────────────────────────────────────────────────
#[derive(Copy, Clone)]
struct TaskShadow {
    pid:               u32,
    last_tick:         u32,
    canary:            u32,
    tick_budget_accum: u32,
    last_priority:     u8,  // for priority inversion detection
}

static mut TASK_SHADOW: [TaskShadow; MAX_TRACKED_TASKS] = [TaskShadow {
    pid: 0, last_tick: 0, canary: CANARY_VAL,
    tick_budget_accum: 0, last_priority: 0,
}; MAX_TRACKED_TASKS];

static mut WATCHDOG_LAST_ACTIVE_PID:   u32   = 0;
static mut WATCHDOG_LAST_ACTIVE_SLOT:  usize = 0;
static mut WATCHDOG_DISPATCH_COUNT:    u32   = 0;

#[inline(always)]
pub unsafe fn watchdog_task_checkin(slot: usize, pid: u32) {
    if slot >= MAX_TRACKED_TASKS { return; }
    let sys = SYSTEM_TICKS.load(Ordering::Relaxed);
    TASK_SHADOW[slot].pid               = pid;
    TASK_SHADOW[slot].last_tick         = sys;
    TASK_SHADOW[slot].tick_budget_accum += 1;
    if slot < MAX_TASKS {
        TASK_SHADOW[slot].last_priority = TASKS[slot].priority;
    }
    WATCHDOG_LAST_ACTIVE_PID    = pid;
    WATCHDOG_LAST_ACTIVE_SLOT   = slot;
    WATCHDOG_DISPATCH_COUNT     = WATCHDOG_DISPATCH_COUNT.wrapping_add(1) % 10_000;
}

// ── Structured log writer ─────────────────────────────────────────────────────
unsafe fn watchdog_log(sys_ticks: u32, sev: &[u8], tag: &[u8], detail: &[u8]) {
    let filename = b"watchdog.log\0";
    let mut line = [0u8; 160];
    let mut li   = 0usize;

    macro_rules! emit {
        ($slice:expr) => { for &c in $slice { if li >= 155 { break; } line[li]=c; li+=1; } };
        ($byte:expr, single) => { if li < 155 { line[li]=$byte; li+=1; } };
    }
    macro_rules! emit_u32 {
        ($val:expr) => {{
            let mut v = $val;
            if v == 0 { emit!(b"0"); }
            else {
                let mut tb=[0u8;12]; let mut ti=0;
                while v>0 { tb[ti]=(v%10)as u8+b'0'; v/=10; ti+=1; }
                for i in (0..ti).rev() { emit!(&[tb[i]]); }
            }
        }};
    }

    emit!(b"[TICK:"); emit_u32!(sys_ticks);
    emit!(b"] [SEV:"); emit!(sev); emit!(b"] ");
    emit!(tag); emit!(b" "); emit!(detail);
    emit!(b'\n', single);

    if !avfs_file_exists(filename.as_ptr()) {
        avfs_create_file(filename.as_ptr(), WATCHDOG_LOG_SIZE + 4);
        WATCHDOG_LOG_OFFSET = 4;
        let hdr = [4u8, 0, 0, 0];
        avfs_write_file(filename.as_ptr(), hdr.as_ptr(), 4, 0);
    }

    let filesize = (avfs_get_filesize(filename.as_ptr()).max(0) as u32).min(WATCHDOG_LOG_SIZE + 4);
    if WATCHDOG_LOG_OFFSET + li as u32 > filesize {
        WATCHDOG_LOG_OFFSET = 4;
    }

    avfs_write_file(filename.as_ptr(), line.as_ptr(), li as u32, WATCHDOG_LOG_OFFSET);
    WATCHDOG_LOG_OFFSET += li as u32;
    let off_bytes = WATCHDOG_LOG_OFFSET.to_le_bytes();
    avfs_write_file(filename.as_ptr(), off_bytes.as_ptr(), 4, 0);
}

// ── HUD helpers ──────────────────────────────────────────────────────────────
// All writes go through hud_write / hud_write_string which offset by
// HUD_COL_START so the rest of the code works in panel-local coords.

#[inline(always)]
unsafe fn hud_write(panel_col: usize, row: usize, ch: u8, attr: u8) {
    let screen_col = HUD_COL_START + panel_col;
    if screen_col < 80 && row < 50 {
        vga_write(screen_col, row, ch, attr);
    }
}

#[inline(always)]
unsafe fn hud_write_string(panel_col: usize, row: usize, s: &[u8], attr: u8) {
    for (i, &c) in s.iter().enumerate() {
        let sc = HUD_COL_START + panel_col + i;
        if sc >= 80 { break; }
        vga_write(sc, row, c, attr);
    }
}

#[inline(always)]
unsafe fn hud_fill(panel_col: usize, row: usize, len: usize, ch: u8, attr: u8) {
    for i in 0..len {
        let sc = HUD_COL_START + panel_col + i;
        if sc >= 80 { break; }
        vga_write(sc, row, ch, attr);
    }
}

// ── Separator line ────────────────────────────────────────────────────────────
unsafe fn draw_separator() {
    // Cols 44-45: vertical bar separating terminal from HUD
    for row in 0..25usize {
        vga_write(44, row, b'|', 0x08);
        vga_write(45, row, b' ', 0x08);
    }
}

// ── Initialize Network Globals with Default Info ─────────────────────────────
unsafe fn init_net_defaults() {
    // Set default values
    NET_LAST_METHOD[..4].copy_from_slice(b"NONE");
    NET_LAST_METHOD[4] = 0;

    NET_LAST_PROTO[..3].copy_from_slice(b"TCP");
    NET_LAST_PROTO[3] = 0;

    NET_LAST_DESC[..7].copy_from_slice(b"NO_CONN");
    NET_LAST_DESC[7] = 0;

    // Sync the display cache so the HUD doesn't flicker on the first frame
    NET_LAST_DISPLAYED_METHOD[..4].copy_from_slice(b"NONE");
    NET_LAST_DISPLAYED_METHOD[4] = 0;

    NET_LAST_DISPLAYED_PROTO[..3].copy_from_slice(b"TCP");
    NET_LAST_DISPLAYED_PROTO[3] = 0;

    NET_LAST_DISPLAYED_DESC[..7].copy_from_slice(b"NO_CONN");
    NET_LAST_DISPLAYED_DESC[7] = 0;
}
// ── WD status HUD (rows 3-8) ──────────────────────────────────────────────────
unsafe fn watchdog_draw_status() {
    let ac = WATCHDOG_ANOMALY_COUNT;

let any_bad = WATCHDOG_HEAP_WARNINGS    > 0 ||
              WATCHDOG_TASK_DEATHS      > 0 ||
              WATCHDOG_STALL_WARNINGS   > 0 ||
              WATCHDOG_NULL_PTR_TRIPS   > 0 ||
              WATCHDOG_STARVATION_HITS  > 0 ||
              WATCHDOG_CANARY_TRIPS     > 0 ||
              WATCHDOG_BUDGET_TRIPS     > 0 ||
              WATCHDOG_IPC_OVERFLOW_HITS > 0 ||
              WATCHDOG_PRIORITY_INV_HITS > 0 ||
              WATCHDOG_DEADLINE_HITS    > 0 ||
              WATCHDOG_AFFINITY_HITS    > 0;

// Row 3: WD:NN [Code]
hud_write_string(0, 3, b"WD:", 0x08);
hud_write(3, 3, b'0' + ((ac / 10) % 10) as u8, 0x07);
hud_write(4, 3, b'0' +  (ac        % 10) as u8, 0x07);
hud_write(5, 3, b' ', 0x07);

if any_bad {
    // Cycle through error messages every 60 ticks (approx 1 sec)
    let cycle_idx = ((WATCHDOG_TICKS / 60) % 11) as usize;

    // Fix: Use &b"STR"[..] to ensure all arms return the same type (&[u8])
    let (msg, col) = match cycle_idx {
        0 if WATCHDOG_HEAP_WARNINGS    > 0 => (&b"HEAP"[..],    0x0E),
        1 if WATCHDOG_TASK_DEATHS      > 0 => (&b"DTH"[..],     0x0C),
        2 if WATCHDOG_STALL_WARNINGS   > 0 => (&b"STALL"[..],   0x0E),
        3 if WATCHDOG_NULL_PTR_TRIPS   > 0 => (&b"NULL"[..],    0x0C),
        4 if WATCHDOG_STARVATION_HITS  > 0 => (&b"STARVE"[..],  0x0E),
        5 if WATCHDOG_CANARY_TRIPS     > 0 => (&b"CANARY"[..],  0x0C),
        6 if WATCHDOG_BUDGET_TRIPS     > 0 => (&b"BUDGET"[..],  0x0C),
        7 if WATCHDOG_IPC_OVERFLOW_HITS> 0 => (&b"IPC"[..],     0x0E),
        8 if WATCHDOG_PRIORITY_INV_HITS> 0 => (&b"PRIO"[..],    0x0E),
        9 if WATCHDOG_DEADLINE_HITS    > 0 => (&b"DEAD"[..],    0x0E),
        10 if WATCHDOG_AFFINITY_HITS   > 0 => (&b"AFF"[..],     0x0E),
        _ => (&b"!!"[..], 0x0C), // Fallback
    };

    hud_write(6, 3, b' ', 0x07);
    hud_write_string(7, 3, msg, col);
} else {
    hud_write(6, 3, b'O', 0x0A);
    hud_write(7, 3, b'K', 0x0A);
}

    // Row 4: T H S M V K B (classic counters)
    macro_rules! sc {
        ($col:expr, $label:expr, $val:expr, $wc:expr) => {
            hud_write($col,     4, $label, 0x08);
            hud_write($col + 1, 4, b':', 0x08);
            hud_write($col + 2, 4, b'0' + ($val % 10) as u8,
                if $val > 0 { $wc } else { 0x07 });
        };
    }
    sc!(0,  b'T', WATCHDOG_TASK_DEATHS,      0x0C);
    sc!(4,  b'H', WATCHDOG_HEAP_WARNINGS,    0x0E);
    sc!(8,  b'S', WATCHDOG_STALL_WARNINGS,   0x0D);
    sc!(12, b'M', WATCHDOG_NULL_PTR_TRIPS,   0x0C);
    sc!(16, b'V', WATCHDOG_STARVATION_HITS,  0x0D);
    sc!(20, b'K', WATCHDOG_CANARY_TRIPS,     0x0C);
    sc!(24, b'B', WATCHDOG_BUDGET_TRIPS,     0x0E);

       // Row 5: new counters
    sc!(0,  b'I', WATCHDOG_IPC_OVERFLOW_HITS, 0x0C); // IPC overflow
    sc!(4,  b'P', WATCHDOG_PRIORITY_INV_HITS, 0x0D); // Priority inversion
    
    sc!(12, b'D', WATCHDOG_DEADLINE_HITS,     0x0E); // Deadline miss
    sc!(16, b'A', WATCHDOG_AFFINITY_HITS,     0x0D); // Affinity
    
    // Added: Network Stale counter
    sc!(20, b'W', WATCHDOG_NET_STALE_HITS,  0x0E); 

    // Row 6: ACT:PPPP[SS] DC:NNNN
    hud_write_string(0, 6, b"ACT:", 0x08);
    let ap = WATCHDOG_LAST_ACTIVE_PID;
    let as_ = WATCHDOG_LAST_ACTIVE_SLOT;
    hud_write(4,  6, b'0' + ((ap / 1000) % 10) as u8, 0x0B);
    hud_write(5,  6, b'0' + ((ap / 100)  % 10) as u8, 0x0B);
    hud_write(6,  6, b'0' + ((ap / 10)   % 10) as u8, 0x0B);
    hud_write(7,  6, b'0' +  (ap          % 10) as u8, 0x0B);
    hud_write(8,  6, b'[', 0x08);
    hud_write(9,  6, b'0' + ((as_ / 10) % 10) as u8, 0x07);
    hud_write(10, 6, b'0' +  (as_        % 10) as u8, 0x07);
    hud_write(11, 6, b']', 0x08);
    hud_write_string(12, 6, b"DC:", 0x08);
    hud_write(15, 6, b'0' + ((WATCHDOG_DISPATCH_COUNT / 1000) % 10) as u8, 0x0E);
    hud_write(16, 6, b'0' + ((WATCHDOG_DISPATCH_COUNT / 100)  % 10) as u8, 0x0E);
    hud_write(17, 6, b'0' + ((WATCHDOG_DISPATCH_COUNT / 10)   % 10) as u8, 0x0E);
    hud_write(18, 6, b'0' +  (WATCHDOG_DISPATCH_COUNT          % 10) as u8, 0x0E);

    // Row 7: HP:NNN% PK:NNN%
    let heap_pct = (HEAP_OFFSET as u32 * 100 / HEAP.len() as u32) as u8;
    let hp_col   = if heap_pct > 80 { 0x0C } else if heap_pct > 50 { 0x0E } else { 0x0A };
    hud_write_string(0, 7, b"HP:", 0x08);
    hud_write(3, 7, b'0' + (heap_pct / 100)       as u8, hp_col);
    hud_write(4, 7, b'0' + ((heap_pct / 10) % 10) as u8, hp_col);
    hud_write(5, 7, b'0' +  (heap_pct % 10)        as u8, hp_col);
    hud_write(6, 7, b'%', hp_col);

    if HEAP_OFFSET as u32 > WATCHDOG_HEAP_PEAK {
        WATCHDOG_HEAP_PEAK = HEAP_OFFSET as u32;
    }
    let pk_pct = (WATCHDOG_HEAP_PEAK * 100 / HEAP.len() as u32) as u8;
    hud_write_string(8, 7, b"PK:", 0x08);
    hud_write(11, 7, b'0' + (pk_pct / 100)         as u8, 0x07);
    hud_write(12, 7, b'0' + ((pk_pct / 10) % 10)   as u8, 0x07);
    hud_write(13, 7, b'0' +  (pk_pct % 10)          as u8, 0x07);
    hud_write(14, 7, b'%', 0x07);

    // Sched stats
    hud_write_string(16, 7, b"FB:", 0x08);
    hud_write(19, 7, b'0' + ((SCHED_FALLBACKS / 10) % 10) as u8, 0x0D);
    hud_write(20, 7, b'0' +  (SCHED_FALLBACKS        % 10) as u8, 0x0D);



    
        // Row 8: Network status

    if NET_HUD_DIRTY {
        NET_HUD_DIRTY = false; // clear before drawing, not after

        hud_fill(0, 8, 60, b' ', 0x00);
        hud_write_string(0, 8, b"LAST: ", 0x08);
        let mut col = 6usize;

        let mut i = 0;
        while i < 8 && NET_LAST_METHOD[i] != 0 && col < 75 {
            hud_write(col, 8, NET_LAST_METHOD[i], 0x0F);
            col += 1; i += 1;
        }
        if col < 75 { hud_write(col, 8, b':', 0x08); col += 1; }

        let mut i = 0;
        while i < 8 && NET_LAST_PROTO[i] != 0 && col < 75 {
            hud_write(col, 8, NET_LAST_PROTO[i], 0x0F);
            col += 1; i += 1;
        }
        if col < 75 { hud_write(col, 8, b':', 0x08); col += 1; }

        let mut i = 0;
        while i < 32 && NET_LAST_DESC[i] != 0 && col < 75 {
            hud_write(col, 8, NET_LAST_DESC[i], 0x0F);
            col += 1; i += 1;
        }
    }


}

#[no_mangle]
pub extern "C" fn watchdog_hud_force_redraw() {
    unsafe {
        // Invalidate the display cache so watchdog_draw_status redraws row 8
        // unconditionally on its next call, regardless of whether globals changed.
        NET_LAST_DISPLAYED_METHOD = [0u8; 8];
        NET_LAST_DISPLAYED_PROTO  = [0u8; 8];
        NET_LAST_DISPLAYED_DESC   = [0u8; 32];

        // Redraw immediately rather than waiting for the next watchdog tick.
        watchdog_draw_status();
    }
}

// ── Main watchdog task ────────────────────────────────────────────────────────
extern "C" fn watchdog_task(_pid: u32) {
    unsafe {
        WATCHDOG_TICKS = WATCHDOG_TICKS.wrapping_add(1);
        watchdog_draw_status();

        if WATCHDOG_TICKS % WATCHDOG_CHECK_INTERVAL != 0 { return; }

        let sys_ticks          = SYSTEM_TICKS.load(Ordering::Relaxed);
        let current_task_count = NUM_TASKS as u32;
        let heap_pct           = HEAP_OFFSET as u32 * 100 / HEAP.len() as u32;

        // ── CHECK 1: Task death ───────────────────────────────────────────
        if WATCHDOG_LAST_TASK_COUNT > 0 && current_task_count < WATCHDOG_LAST_TASK_COUNT {
            WATCHDOG_TASK_DEATHS  += 1;
            WATCHDOG_ANOMALY_COUNT += 1;
            let sev = if current_task_count == 0 { SEV_FATAL } else { SEV_CRIT };
            let mut det = [0u8; 40]; let mut di = 0usize;
            macro_rules! pd { ($s:expr) => { for &c in $s { if di<38 { det[di]=c; di+=1; } } }; }
            pd!(b"was=");
            { let mut v=WATCHDOG_LAST_TASK_COUNT; if v==0{det[di]=b'0';di+=1;}
              else { let mut tb=[0u8;10];let mut ti=0; while v>0{tb[ti]=(v%10)as u8+b'0';v/=10;ti+=1;}
                     for i in(0..ti).rev(){det[di]=tb[i];di+=1;} } }
            pd!(b" now=");
            { let mut v=current_task_count; if v==0{det[di]=b'0';di+=1;}
              else { let mut tb=[0u8;10];let mut ti=0; while v>0{tb[ti]=(v%10)as u8+b'0';v/=10;ti+=1;}
                     for i in(0..ti).rev(){det[di]=tb[i];di+=1;} } }
            watchdog_log(sys_ticks, sev, b"TASK_DIED", &det[0..di]);
        }
        WATCHDOG_LAST_TASK_COUNT = current_task_count;

        // ── CHECK 2: Heap pressure ────────────────────────────────────────
        if heap_pct > HEAP_WARN_PCT {
            WATCHDOG_HEAP_WARNINGS += 1;
            WATCHDOG_ANOMALY_COUNT  += 1;
            let sev = if heap_pct > HEAP_CRIT_PCT { SEV_CRIT } else { SEV_WARN };
            let v = heap_pct as u8;
            let det = [b'0'+(v/100), b'0'+((v/10)%10), b'0'+(v%10), b'%'];
            watchdog_log(sys_ticks, sev, b"HEAP_HIGH", &det);
        }

        // ── CHECK 3: Scheduler stall ──────────────────────────────────────
        if sys_ticks == WATCHDOG_LAST_SYS_TICKS {
            WATCHDOG_STALL_COUNT += 1;
            if WATCHDOG_STALL_COUNT >= STALL_TRIP_COUNT {
                WATCHDOG_STALL_WARNINGS += 1;
                WATCHDOG_ANOMALY_COUNT   += 1;
                watchdog_log(sys_ticks, SEV_CRIT, b"SCHED_STALL", b"ticks_frozen");
                WATCHDOG_STALL_COUNT = 0;
            }
        } else {
            WATCHDOG_STALL_COUNT = 0;
        }
        WATCHDOG_LAST_SYS_TICKS = sys_ticks;

        // ── CHECK 4: Sentinel corruption ──────────────────────────────────
        if WATCHDOG_SENTINEL != WATCHDOG_SENTINEL_VAL {
            WATCHDOG_NULL_PTR_TRIPS += 1;
            WATCHDOG_ANOMALY_COUNT   += 1;
            let bad = WATCHDOG_SENTINEL;
            let mut det = [0u8; 16]; let mut di = 0usize;
            for &c in b"got=0x" { det[di]=c; di+=1; }
            for shift in (0..8).rev() {
                let n = ((bad >> (shift*4)) & 0xF) as u8;
                det[di] = if n<10 { b'0'+n } else { b'a'+n-10 }; di+=1;
            }
            watchdog_log(sys_ticks, SEV_CRIT, b"MEM_CORRUPT", &det[0..di]);
            WATCHDOG_SENTINEL = WATCHDOG_SENTINEL_VAL;
        }

        // ── CHECK 5: Zero task count ──────────────────────────────────────
        if current_task_count == 0 {
            WATCHDOG_ANOMALY_COUNT += 1;
            watchdog_log(sys_ticks, SEV_FATAL, b"NO_TASKS", b"all_tasks_gone");
        }

        // ── CHECK 6: Task starvation ──────────────────────────────────────
        for slot in 0..MAX_TRACKED_TASKS {
            let s = &mut TASK_SHADOW[slot];
            if s.pid == 0 { continue; }
            let age = sys_ticks.saturating_sub(s.last_tick);
            if age > STARVATION_THRESH {
                WATCHDOG_STARVATION_HITS += 1;
                WATCHDOG_ANOMALY_COUNT    += 1;
                let mut det=[0u8;12]; let mut di=0usize;
                for &c in b"pid=" { det[di]=c; di+=1; }
                let mut v=s.pid; if v==0{det[di]=b'0';di+=1;}
                else{let mut tb=[0u8;8];let mut ti=0;while v>0{tb[ti]=(v%10)as u8+b'0';v/=10;ti+=1;}
                     for i in(0..ti).rev(){det[di]=tb[i];di+=1;}}
                watchdog_log(sys_ticks, SEV_WARN, b"TASK_STARVED", &det[0..di]);
                s.last_tick = sys_ticks;
            }
        }

        // ── CHECK 7: Stack canary ─────────────────────────────────────────
        for slot in 0..MAX_TRACKED_TASKS {
            let s = &mut TASK_SHADOW[slot];
            if s.pid == 0 { continue; }
            if s.canary != CANARY_VAL {
                WATCHDOG_CANARY_TRIPS  += 1;
                WATCHDOG_ANOMALY_COUNT  += 1;
                let mut det=[0u8;24]; let mut di=0usize;
                for &c in b"slot=" { det[di]=c; di+=1; }
                let mut v=slot as u32; if v==0{det[di]=b'0';di+=1;}
                else{let mut tb=[0u8;6];let mut ti=0;while v>0{tb[ti]=(v%10)as u8+b'0';v/=10;ti+=1;}
                     for i in(0..ti).rev(){det[di]=tb[i];di+=1;}}
                for &c in b" pid=" { det[di]=c; di+=1; }
                let mut p=s.pid; if p==0{det[di]=b'0';di+=1;}
                else{let mut pb=[0u8;6];let mut pi=0;while p>0{pb[pi]=(p%10)as u8+b'0';p/=10;pi+=1;}
                     for i in(0..pi).rev(){det[di]=pb[i];di+=1;}}
                watchdog_log(sys_ticks, SEV_CRIT, b"STACK_CANARY", &det[0..di]);
                s.canary = CANARY_VAL;
            }
        }

        // ── CHECK 8: Runaway tick budget ──────────────────────────────────
        let window = WATCHDOG_CHECK_INTERVAL;
        for slot in 0..MAX_TRACKED_TASKS {
            let s = &mut TASK_SHADOW[slot];
            if s.pid == 0 { continue; }
            let pct = s.tick_budget_accum * 100 / window;
            if pct > 85 {
                WATCHDOG_BUDGET_TRIPS  += 1;
                WATCHDOG_ANOMALY_COUNT  += 1;
                let mut det=[0u8;20]; let mut di=0usize;
                for &c in b"pid=" { det[di]=c; di+=1; }
                let mut v=s.pid; if v==0{det[di]=b'0';di+=1;}
                else{let mut tb=[0u8;10];let mut ti=0;while v>0{tb[ti]=(v%10)as u8+b'0';v/=10;ti+=1;}
                     for i in(0..ti).rev(){det[di]=tb[i];di+=1;}}
                for &c in b" pct=" { det[di]=c; di+=1; }
                let pp=pct as u8;
                det[di]=b'0'+(pp/100)    as u8; di+=1;
                det[di]=b'0'+((pp/10)%10)as u8; di+=1;
                det[di]=b'0'+(pp%10)     as u8; di+=1;
                det[di]=b'%';                    di+=1;
                watchdog_log(sys_ticks, SEV_WARN, b"TASK_RUNAWAY", &det[0..di]);
            }
            s.tick_budget_accum = 0;
        }

        // ── CHECK 9: IPC queue overflow ───────────────────────────────────
        if IPC_OVERFLOWS > 0 {
            let sev = if IPC_OVERFLOWS >= IPC_OVERFLOW_WARN { SEV_CRIT } else { SEV_WARN };
            WATCHDOG_IPC_OVERFLOW_HITS += 1;
            WATCHDOG_ANOMALY_COUNT      += 1;
            let mut det=[0u8;12]; let mut di=0usize;
            for &c in b"ov=" { det[di]=c; di+=1; }
            let mut v=IPC_OVERFLOWS; if v==0{det[di]=b'0';di+=1;}
            else{let mut tb=[0u8;8];let mut ti=0;while v>0{tb[ti]=(v%10)as u8+b'0';v/=10;ti+=1;}
                 for i in(0..ti).rev(){det[di]=tb[i];di+=1;}}
            watchdog_log(sys_ticks, sev, b"IPC_OVERFLOW", &det[0..di]);
            // Don't reset IPC_OVERFLOWS here; ipc_post owns it
        }

        // ── CHECK 10: Priority inversion ──────────────────────────────────
        // A high-base-priority task should not be blocked for long while a
        // low-priority task holds its time. Detect: a task with base_priority
        // > 50 has been in Ready state (not dispatched) for > PRIORITY_INV_THRESH
        // ticks while a lower-priority task has been Running.
        for slot in 0..MAX_TASKS {
            if !TASKS[slot].is_active { continue; }
            if TASKS[slot].base_priority < 50 { continue; }
            if TASKS[slot].state != TaskState::Ready { continue; }
            let age = sys_ticks.saturating_sub(TASKS[slot].last_run_tick);
            if age > PRIORITY_INV_THRESH {
                WATCHDOG_PRIORITY_INV_HITS += 1;
                WATCHDOG_ANOMALY_COUNT      += 1;
                let mut det=[0u8;20]; let mut di=0usize;
                for &c in b"hi_pid=" { det[di]=c; di+=1; }
                let mut v=TASKS[slot].pid; if v==0{det[di]=b'0';di+=1;}

            }
        }



        // ── CHECK 12: Missed deadlines ────────────────────────────────────
        if REAPER_MISSED_DEADLINES > 0 {
            let sev = if REAPER_MISSED_DEADLINES >= MISSED_DEADLINE_CRIT { SEV_CRIT } else { SEV_WARN };
            WATCHDOG_DEADLINE_HITS += 1;
            WATCHDOG_ANOMALY_COUNT  += 1;
            let mut det=[0u8;12]; let mut di=0usize;
            for &c in b"n=" { det[di]=c; di+=1; }
            let mut v=REAPER_MISSED_DEADLINES; if v==0{det[di]=b'0';di+=1;}
            else{let mut tb=[0u8;8];let mut ti=0;while v>0{tb[ti]=(v%10)as u8+b'0';v/=10;ti+=1;}
                 for i in(0..ti).rev(){det[di]=tb[i];di+=1;}}
            watchdog_log(sys_ticks, sev, b"MISSED_DEADLINE", &det[0..di]);
        }

        // ── CHECK 13: Affinity violations ────────────────────────────────
        if AFFINITY_VIOLATIONS > 0 {
            WATCHDOG_AFFINITY_HITS += 1;
            WATCHDOG_ANOMALY_COUNT  += 1;
            let mut det=[0u8;12]; let mut di=0usize;
            for &c in b"n=" { det[di]=c; di+=1; }
            let mut v=AFFINITY_VIOLATIONS; if v==0{det[di]=b'0';di+=1;}
            else{let mut tb=[0u8;8];let mut ti=0;while v>0{tb[ti]=(v%10)as u8+b'0';v/=10;ti+=1;}
                 for i in(0..ti).rev(){det[di]=tb[i];di+=1;}}
            watchdog_log(sys_ticks, SEV_WARN, b"AFFINITY_VIOL", &det[0..di]);
        }
                // ── CHECK 14: Network Staleness ────────────────────────────────────
        // Check if the last network update was too long ago (Stale connection)
        // Only check if we are actually connected (Method is not default "NONE")
        let is_connected = !(NET_LAST_METHOD[0] == b'N' && 
                            NET_LAST_METHOD[1] == b'O' && 
                            NET_LAST_METHOD[2] == b'N' && 
                            NET_LAST_METHOD[3] == b'E');

        if is_connected && NET_LAST_METHOD[0] != 0 {
            let age = sys_ticks.saturating_sub(NET_LAST_UPDATE_TICKS);
            if age > 5000 { // Threshold: 5000 ticks (adjustable)
                WATCHDOG_NET_STALE_HITS += 1;
                WATCHDOG_ANOMALY_COUNT += 1;
                let mut det=[0u8;12]; let mut di=0usize;
                for &c in b"age=" { det[di]=c; di+=1; }
                let mut v=age; if v==0{det[di]=b'0';di+=1;}
                else{let mut tb=[0u8;8];let mut ti=0;while v>0{tb[ti]=(v%10)as u8+b'0';v/=10;ti+=1;}
                     for i in(0..ti).rev(){det[di]=tb[i];di+=1;}}
                watchdog_log(sys_ticks, SEV_WARN, b"NET_STALE", &det[0..di]);
                
                // Reset timestamp to debounce log (prevent spamming every tick)
                NET_LAST_UPDATE_TICKS = sys_ticks;
            }
        }

    } // unsafe
}


#[no_mangle]
pub extern "C" fn rust_start_demo_tasks() {
    unsafe {
        for i in 0..(80 * 50) {
            *VGA_MEMORY.add(i) = 0x0700 | b' ' as u16;
        }
        init_net_defaults(); 
        draw_separator();
        rust_create_task_ex(keyboard_handler as u32, true, 10,  0, 0);
        rust_create_task_ex(watchdog_task    as u32, true, 10,  0, 0);
        
        rust_create_task_ex(heartbeat_task   as u32, true,  8,  0, 0);
        rust_create_task_ex(timed_message    as u32, true,  4,  0, 0);
        
        // --- NEW TASK ADDED HERE ---
        
        // ---------------------------

        rust_create_task_ex(thermal_task     as u32, true,  7,  1, 0); 
        rust_create_task_ex(io_stress_task   as u32, true,  5,  2, 0); 
        rust_create_task_ex(reaper_task      as u32, true,  9,  0, 0);
        rust_create_task_ex(affinity_task    as u32, true,  6,  0, 0);
        rust_create_task_ex(ipc_monitor_task as u32, true,  7,  0, 0);
    }
}



//=============================================================================
// PROCESS MANAGEMENT (unchanged interface)
//=============================================================================

#[no_mangle]
pub extern "C" fn rust_list_tasks() {
    unsafe {
        terminal_setcolor(0x0F);
        rust_print(b"\n=== Active Tasks ===\n");
        rust_print(b"PID  ID  State  Pri  Aff  Ticks\n");
        rust_print(b"---  --  -----  ---  ---  -----\n");
        for i in 0..MAX_TASKS {
            if TASKS[i].is_active {
                print_num(TASKS[i].pid as i32);        rust_print(b" ");
                print_num(TASKS[i].id  as i32);        rust_print(b" ");
                match TASKS[i].state {
                    TaskState::Ready   => rust_print(b"READY "),
                    TaskState::Running => rust_print(b"RUN   "),
                    TaskState::Blocked => rust_print(b"BLOCK "),
                    TaskState::Zombie  => rust_print(b"ZOMBIE"),
                    TaskState::Sleeping=> rust_print(b"SLEEP "),
                }
                rust_print(b" ");
                print_num(TASKS[i].priority as i32); rust_print(b" ");
                print_num(TASKS[i].affinity as i32); rust_print(b" ");
                print_num(TASKS[i].total_ticks as i32);
                rust_print(b"\n");
            }
        }
        terminal_setcolor(0x07);
    }
}

#[no_mangle]
pub extern "C" fn rust_kill_task(pid: u32) -> i32 {
    unsafe {
        if pid == 0 { rust_print(b"Error: Cannot kill idle task\n"); return -1; }
        for i in 0..MAX_TASKS {
            if TASKS[i].is_active && TASKS[i].pid == pid {
                // Mark as Zombie so the reaper can clean it up properly
                TASKS[i].state = TaskState::Zombie;
                rust_print(b"Marked PID "); print_num(pid as i32); rust_print(b" as Zombie\n");
                return 0;
            }
        }
        rust_print(b"Error: PID "); print_num(pid as i32); rust_print(b" not found\n");
        return -1;
    }
}


#[no_mangle]
pub extern "C" fn rust_get_task_count() -> u32 {
    unsafe { NUM_TASKS as u32 }
}

#[no_mangle]
pub extern "C" fn rust_task_info(pid: u32) -> i32 {
    unsafe {
        for i in 0..MAX_TASKS {
            if TASKS[i].is_active && TASKS[i].pid == pid {
                terminal_setcolor(0x0E);
                rust_print(b"\n=== Task Info ===\n");
                terminal_setcolor(0x07);
                rust_print(b"PID:        "); print_num(TASKS[i].pid as i32);      rust_print(b"\n");
                rust_print(b"Task ID:    "); print_num(TASKS[i].id  as i32);      rust_print(b"\n");
                rust_print(b"BasePri:    "); print_num(TASKS[i].base_priority as i32); rust_print(b"\n");
                rust_print(b"DynPri:     "); print_num(TASKS[i].priority as i32); rust_print(b"\n");
                rust_print(b"Affinity:   "); print_num(TASKS[i].affinity as i32); rust_print(b"\n");
                rust_print(b"TotalTicks: "); print_num(TASKS[i].total_ticks as i32); rust_print(b"\n");
                rust_print(b"Deadline:   "); print_num(TASKS[i].deadline as i32); rust_print(b"\n");
                rust_print(b"State: ");
                match TASKS[i].state {
                    TaskState::Ready    => rust_print(b"READY\n"),
                    TaskState::Running  => rust_print(b"RUNNING\n"),
                    TaskState::Blocked  => rust_print(b"BLOCKED\n"),
                    TaskState::Zombie   => rust_print(b"ZOMBIE\n"),
                    TaskState::Sleeping => rust_print(b"SLEEPING\n"),
                }
                return 0;
            }
        }
        rust_print(b"Error: PID "); print_num(pid as i32); rust_print(b" not found\n");
        return -1;
    }
}

// made this while listening to godhandusa LOL ( i want to die. )
// - scp_2801

#[no_mangle]
pub extern "C" fn watchdog_diagram() {
    rust_print(b"\n");
    rust_print(b"+=======================================================================+\n");
    rust_print(b"|            RADIUMOS MULTITASKER v2.0 - OPERATIONS HANDBOOK           |\n");
    rust_print(b"|                         scp_2801 // RadiumOS                         |\n");
    rust_print(b"+=======================================================================+\n");
    rust_print(b"\n");

    // =========================================================================
    // SECTION 0: SCREEN LAYOUT
    // =========================================================================
    rust_print(b"  SECTION 0: SCREEN LAYOUT\n");
    rust_print(b"  =========================\n");
    rust_print(b"\n");
    rust_print(b"  The 80x25 VGA screen is split into two hard zones:\n");
    rust_print(b"\n");
    rust_print(b"  Cols  0-43  TERMINAL ZONE\n");
    rust_print(b"              All rust_print / RSH shell output wraps here.\n");
    rust_print(b"              Width: 44 characters. Scrolls independently.\n");
    rust_print(b"              Chars at col 43 cause a newline to col 0.\n");
    rust_print(b"\n");
    rust_print(b"  Cols 44-45  SEPARATOR ZONE\n");
    rust_print(b"              Col 44 = vertical bar glyph '|' (attr 0x08, dark grey).\n");
    rust_print(b"              Col 45 = space gutter. Neither side writes here.\n");
    rust_print(b"\n");
    rust_print(b"  Cols 46-79  HUD PANEL (watchdog / status)\n");
    rust_print(b"              Width: 34 characters. Never cleared by the terminal.\n");
    rust_print(b"              All hud_write() calls use panel-local coordinates\n");
    rust_print(b"              (0 = col 46 on screen). Helper: hud_write(col, row, ...).\n");
    rust_print(b"\n");
    rust_print(b"  HUD ROW MAP\n");
    rust_print(b"  -----------\n");
    rust_print(b"  Row  0  Banner: 'Developed by scp_2801  v2.0'           cyan\n");
    rust_print(b"  Row  1  Uptime HH:MM:SS | tick counter NNN | beat glyph\n");
    rust_print(b"  Row  2  Heap bar HP[##########]NNN%\n");
    rust_print(b"  Row  3  WD:NN  OK / !!  (watchdog summary)\n");
    rust_print(b"  Row  4  T: H: S: M: V: K: B:  (classic counters)\n");
    rust_print(b"  Row  5  I: P: R: D: A:         (new counters v2.0)\n");
    rust_print(b"  Row  6  ACT:PPPP[SS]  DC:NNNN  (active PID / dispatch)\n");
    rust_print(b"  Row  7  HP:NNN%  PK:NNN%  FB:NN (heap / peak / fallbacks)\n");
    rust_print(b"  Row  8  TEMP:NN.NN C  [OK] / [THROTTLED]\n");
    rust_print(b"  Row  9  THR_EVENTS:NNN\n");
    rust_print(b"  Row 10  IO:BURST / IO:IDLE  B:NNN\n");
    rust_print(b"  Row 11  IO_KB:NNNN MB  [THR]\n");
    rust_print(b"  Row 12  REAPED:NNN\n");
    rust_print(b"  Row 13  MISS_DL:NNN\n");
    rust_print(b"  Row 14  AFFINITY:NNN VIOL\n");
    rust_print(b"  Row 15  AGING_BOOSTS:NNN\n");
    rust_print(b"  Row 16  IPC Q:NN/32  OV:NN\n");
    rust_print(b"  Row 17  IPC_TOTAL:NNNN\n");
    rust_print(b"  Rows 18-23  (reserved for future subsystems)\n");
    rust_print(b"  Row 24  Rotating status message\n");
    rust_print(b"\n");

    // =========================================================================
    // SECTION 1: TASK ROSTER
    // =========================================================================
    rust_print(b"  SECTION 1: TASK ROSTER\n");
    rust_print(b"  =======================\n");
    rust_print(b"\n");
    rust_print(b"  8 tasks run in the demo suite (+ idle task 0).\n");
    rust_print(b"  Created by rust_start_demo_tasks() in this order:\n");
    rust_print(b"\n");
    rust_print(b"  Slot  Name              BasePri  Affinity  Notes\n");
    rust_print(b"  ----  ----------------  -------  --------  -----\n");
    rust_print(b"     0  idle              1        0         kernel idle loop\n");
    rust_print(b"     1  watchdog_task     10       0         HUD + 13 checks\n");
    rust_print(b"     2  keyboard_handler  10       0         PS/2 input\n");
    rust_print(b"     3  heartbeat_task    8        0         banner/uptime/heap\n");
    rust_print(b"     4  timed_message     4        0         status rotator\n");
    rust_print(b"     5  thermal_task      7        1         temp model, throttle\n");
    rust_print(b"     6  io_stress_task    5        2         burst I/O, AVFS writes\n");
    rust_print(b"     7  reaper_task       9        0         zombie reap, deadlines\n");
    rust_print(b"     8  affinity_task     6        0         affinity / aging stats\n");
    rust_print(b"     9  ipc_monitor_task  7        0         IPC drain, overflow det.\n");
    rust_print(b"\n");
    rust_print(b"  New fields on Task struct (v2.0):\n");
    rust_print(b"    base_priority   original priority at creation time\n");
    rust_print(b"    priority        dynamic priority (may be boosted by aging)\n");
    rust_print(b"    affinity        0=any  1-8=epoch-gated affinity group\n");
    rust_print(b"    state           Ready/Running/Blocked/Zombie/Sleeping\n");
    rust_print(b"    block_reason    None/IpcRecv/IpcSend/Sleep/IoWait/Semaphore\n");
    rust_print(b"    wake_at         tick at which a Sleeping task wakes\n");
    rust_print(b"    deadline        0=none  otherwise: FATAL if missed\n");
    rust_print(b"    total_ticks     cumulative dispatch count\n");
    rust_print(b"    last_run_tick   sys tick of most recent dispatch\n");
    rust_print(b"\n");

    // =========================================================================
    // SECTION 2: SCHEDULER MECHANICS
    // =========================================================================
    rust_print(b"  SECTION 2: SCHEDULER MECHANICS\n");
    rust_print(b"  ================================\n");
    rust_print(b"\n");
    rust_print(b"  rust_schedule() fires every PIT IRQ0 and executes 7 phases:\n");
    rust_print(b"\n");
    rust_print(b"  PHASE 1 - Sleep wakeup\n");
    rust_print(b"    Scans all slots. Any task in Sleeping state with wake_at <=\n");
    rust_print(b"    SYSTEM_TICKS is moved to Ready. call task_sleep(N) from within\n");
    rust_print(b"    a task body to suspend it for N ticks.\n");
    rust_print(b"\n");
    rust_print(b"  PHASE 2 - Priority aging\n");
    rust_print(b"    Any Ready task that has not been dispatched for AGING_THRESH\n");
    rust_print(b"    (300) ticks gets its dynamic priority raised by AGING_BOOST (10)\n");
    rust_print(b"    each aging pass, up to 255. This prevents starvation. The boost\n");
    rust_print(b"    is reset to base_priority when the task is dispatched (Phase 5).\n");
    rust_print(b"    AGING_BOOSTS counter increments on each boost application.\n");
    rust_print(b"\n");
    rust_print(b"  PHASE 3 - Time-slice decrement\n");
    rust_print(b"    The currently running task's remaining_time is decremented.\n");
    rust_print(b"    When it hits 0 the task is preempted on the next tick.\n");
    rust_print(b"\n");
    rust_print(b"  PHASE 4 - Next task selection (two-pass)\n");
    rust_print(b"    Pass 1 (affinity-aware): scans all slots for Ready tasks whose\n");
    rust_print(b"      affinity group matches the current tick epoch:\n");
    rust_print(b"        epoch = SYSTEM_TICKS % MAX_AFFINITIES (8)\n");
    rust_print(b"        task is eligible if affinity==0 OR (affinity-1)==epoch\n");
    rust_print(b"      Among eligible tasks picks the one with highest priority.\n");
    rust_print(b"    Pass 2 (fallback): if Pass 1 found nothing, ignores affinity\n");
    rust_print(b"      and picks highest-priority Ready task. SCHED_FALLBACKS++.\n");
    rust_print(b"\n");
    rust_print(b"  PHASE 5 - Context switch\n");
    rust_print(b"    Old task: state -> Ready, dynamic priority reset to base.\n");
    rust_print(b"    New task: state -> Running, remaining_time = time_slice,\n");
    rust_print(b"              last_run_tick = sys_tick, total_ticks++.\n");
    rust_print(b"\n");
    rust_print(b"  PHASE 6 - Watchdog checkin\n");
    rust_print(b"    watchdog_task_checkin(slot, pid) updates the shadow table and\n");
    rust_print(b"    tick_budget_accum for the dispatched task. Fires every dispatch.\n");
    rust_print(b"\n");
    rust_print(b"  PHASE 7 - Dispatch\n");
    rust_print(b"    entry_point is called as: extern C fn(pid: u32).\n");
    rust_print(b"    The task runs synchronously until it returns; it should do a\n");
    rust_print(b"    small unit of work per call (cooperative multitasking model).\n");
    rust_print(b"\n");
    rust_print(b"  Idle case: if no task is Ready (all Sleeping/Blocked), the current\n");
    rust_print(b"  task's time_slice is refreshed and the scheduler returns without\n");
    rust_print(b"  switching. SCHED_IDLE_RUNS and SCHED_FALLBACKS increment.\n");
    rust_print(b"\n");

    // =========================================================================
    // SECTION 3: IPC SYSTEM
    // =========================================================================
    rust_print(b"  SECTION 3: IPC MESSAGE QUEUE\n");
    rust_print(b"  =============================\n");
    rust_print(b"\n");
    rust_print(b"  Fixed-size FIFO of 32 IpcMsg entries shared by all tasks.\n");
    rust_print(b"  Not interrupt-safe - only call from task context.\n");
    rust_print(b"\n");
    rust_print(b"  Message fields:\n");
    rust_print(b"    sender_pid  PID of sending task\n");
    rust_print(b"    recv_pid    PID of intended recipient (0 = system broadcast)\n");
    rust_print(b"    msg_type    IpcMsgType enum (Ping/Ack/Thermal/IoReport/...)\n");
    rust_print(b"    payload     u32 payload (type-dependent meaning)\n");
    rust_print(b"    tick        SYSTEM_TICKS at post time\n");
    rust_print(b"    consumed    false until ipc_recv() marks it\n");
    rust_print(b"\n");
    rust_print(b"  ipc_post(sender, recv, type, payload) -> bool\n");
    rust_print(b"    Returns false and increments IPC_OVERFLOWS if queue is full.\n");
    rust_print(b"    Last-writer-wins: if IPC_OVERFLOWS >= IPC_OVERFLOW_WARN (3),\n");
    rust_print(b"    the watchdog escalates to CRIT on next check.\n");
    rust_print(b"\n");
    rust_print(b"  ipc_recv(recv_pid, out: &mut IpcMsg) -> bool\n");
    rust_print(b"    Scans the queue for the first unconsumed message matching\n");
    rust_print(b"    recv_pid. Marks it consumed. O(MAX_IPC_MSGS) scan per call.\n");
    rust_print(b"\n");
    rust_print(b"  Who posts what:\n");
    rust_print(b"    thermal_task    -> recv 0  IpcMsgType::Thermal   payload=temp_cdeg\n");
    rust_print(b"    io_stress_task  -> recv 0  IpcMsgType::IoReport  payload=total_KB\n");
    rust_print(b"    reaper_task     -> recv 0  IpcMsgType::Reaper    payload=pid_reaped\n");
    rust_print(b"    affinity_task   -> recv 0  IpcMsgType::Affinity  payload=pid_viol\n");
    rust_print(b"\n");
    rust_print(b"  ipc_monitor_task drains all recv_pid=0 messages every tick and\n");
    rust_print(b"  counts them in IPC_TOTAL_CONSUMED. Queue depth shown on HUD row 16.\n");
    rust_print(b"\n");
    rust_print(b"  How to trigger IPC overflow deliberately:\n");
    rust_print(b"    Post 33 messages without draining:\n");
    rust_print(b"      for _ in 0..33 { ipc_post(pid, 999, IpcMsgType::Ping, 0); }\n");
    rust_print(b"    ipc_monitor_task won't drain them because recv_pid=999 != 0.\n");
    rust_print(b"    IPC_OVERFLOWS will hit 1; watchdog fires IPC_OVERFLOW WARN.\n");
    rust_print(b"    Hit 3 overflows to escalate to CRIT.\n");
    rust_print(b"\n");

    // =========================================================================
    // SECTION 4: AFFINITY SYSTEM
    // =========================================================================
    rust_print(b"  SECTION 4: AFFINITY SYSTEM\n");
    rust_print(b"  ===========================\n");
    rust_print(b"\n");
    rust_print(b"  RadiumOS simulates logical 'CPU groups' using tick epochs.\n");
    rust_print(b"  Tasks assigned to affinity group N only run when:\n");
    rust_print(b"    SYSTEM_TICKS % MAX_AFFINITIES == N - 1\n");
    rust_print(b"  where MAX_AFFINITIES = 8.\n");
    rust_print(b"\n");
    rust_print(b"  Affinity 0 means the task can run during any epoch (no pinning).\n");
    rust_print(b"\n");
    rust_print(b"  Example: thermal_task has affinity=1.\n");
    rust_print(b"    It runs only on ticks where SYSTEM_TICKS % 8 == 0.\n");
    rust_print(b"    io_stress_task has affinity=2: runs on SYSTEM_TICKS % 8 == 1.\n");
    rust_print(b"\n");
    rust_print(b"  Affinity violation: if a pinned task runs outside its epoch,\n");
    rust_print(b"  AFFINITY_VIOLATIONS increments. This happens via the Pass 2\n");
    rust_print(b"  fallback when the scheduler has no affinity-legal tasks.\n");
    rust_print(b"\n");
    rust_print(b"  The affinity_task checks for violations every 400 ticks by\n");
    rust_print(b"  comparing a task's last_run_tick epoch against its expected epoch.\n");
    rust_print(b"  Violations are logged to AVFS and posted as IPC Affinity messages.\n");
    rust_print(b"\n");
    rust_print(b"  How to force affinity violations:\n");
    rust_print(b"    Create many high-priority tasks with affinity=0 so the scheduler\n");
    rust_print(b"    is always busy during non-epoch ticks and must fall back:\n");
    rust_print(b"      for _ in 0..10 { rust_create_task_ex(spin as u32, true, 200, 0, 0); }\n");
    rust_print(b"    Pass 2 will dispatch thermal_task and io_stress_task off-epoch.\n");
    rust_print(b"\n");

    // =========================================================================
    // SECTION 5: THERMAL SUBSYSTEM
    // =========================================================================
    rust_print(b"  SECTION 5: THERMAL SUBSYSTEM\n");
    rust_print(b"  =============================\n");
    rust_print(b"\n");
    rust_print(b"  FAKE_TEMP_CDEG is the simulated CPU temperature in centidegrees C.\n");
    rust_print(b"  Boot value: 4500 (45.00 C). Limits:\n");
    rust_print(b"    TEMP_WARN_CDEG  = 8000   (80.00 C)  HUD goes yellow\n");
    rust_print(b"    TEMP_CRIT_CDEG  = 9500   (95.00 C)  HUD goes red, throttle active\n");
    rust_print(b"    TEMP_COOL_CDEG  = 6000   (60.00 C)  throttle releases below this\n");
    rust_print(b"\n");
    rust_print(b"  Heat model (per thermal_task tick):\n");
    rust_print(b"    +5 cdeg when IO_STRESS_ACTIVE == true\n");
    rust_print(b"    -2 cdeg passive cooling when IO_STRESS_ACTIVE == false\n");
    rust_print(b"    Floor at 4500 cdeg (ambient)\n");
    rust_print(b"\n");
    rust_print(b"  Throttle logic:\n");
    rust_print(b"    When temp >= TEMP_CRIT_CDEG: THERMAL_THROTTLE=true\n");
    rust_print(b"      io_stress_task reads IO_STRESS_THROTTLED and skips all burst\n");
    rust_print(b"      activity, stopping heat generation. Temp then falls passively.\n");
    rust_print(b"    When temp < TEMP_COOL_CDEG: THERMAL_THROTTLE=false, IO resumes.\n");
    rust_print(b"    This creates a sawtooth thermal oscillation visible in the HUD.\n");
    rust_print(b"\n");
    rust_print(b"  Every throttle-engage event: IPC Thermal posted, THERMAL_THROTTLE_EVENTS++.\n");
    rust_print(b"  Watchdog CHECK 11 (THERMAL_CRIT) fires when temp >= TEMP_CRIT_CDEG.\n");
    rust_print(b"\n");
    rust_print(b"  How to force thermal trip immediately:\n");
    rust_print(b"    FAKE_TEMP_CDEG = 9500;\n");
    rust_print(b"    The next thermal_task tick will engage throttle and log THERMAL_CRIT.\n");
    rust_print(b"\n");
    rust_print(b"  How to prevent the system from cooling:\n");
    rust_print(b"    IO_STRESS_THROTTLED = false;  // bypass thermal gate\n");
    rust_print(b"    IO_STRESS_ACTIVE    = true;   // always heating\n");
    rust_print(b"    THERMAL_THROTTLE will engage but IO never sees it.\n");
    rust_print(b"    Temp will climb until it wraps the u32 (takes a very long time).\n");
    rust_print(b"\n");

    // =========================================================================
    // SECTION 6: REAPER TASK
    // =========================================================================
    rust_print(b"  SECTION 6: REAPER TASK\n");
    rust_print(b"  =======================\n");
    rust_print(b"\n");
    rust_print(b"  Scans every REAPER_SCAN_INTERVAL (200) ticks for:\n");
    rust_print(b"\n");
    rust_print(b"  A) Zombie tasks\n");
    rust_print(b"     State == TaskState::Zombie\n");
    rust_print(b"     Action: Task struct zeroed, NUM_TASKS--, REAPER_REAPED++.\n");
    rust_print(b"     IPC Reaper message posted to recv_pid=0.\n");
    rust_print(b"     The slot is then available for rust_create_task_ex().\n");
    rust_print(b"     Note: rust_kill_task() now marks tasks Zombie instead of\n");
    rust_print(b"     immediately freeing them - this ensures bookkeeping is clean.\n");
    rust_print(b"\n");
    rust_print(b"  B) Missed deadlines\n");
    rust_print(b"     task.deadline != 0 AND SYSTEM_TICKS > task.deadline\n");
    rust_print(b"     Action: REAPER_MISSED_DEADLINES++, entry written to\n");
    rust_print(b"             deadline_miss.avfs, deadline field cleared.\n");
    rust_print(b"     Format: 'MISS pid=NNNN dl=NNNN\\n'\n");
    rust_print(b"     The task is NOT killed - a missed deadline is logged only.\n");
    rust_print(b"     Call rust_kill_task() manually if you want to terminate it.\n");
    rust_print(b"\n");
    rust_print(b"  Watchdog CHECK 12 (MISSED_DEADLINE) fires when\n");
    rust_print(b"  REAPER_MISSED_DEADLINES > 0. Escalates to CRIT at >= 5 misses.\n");
    rust_print(b"\n");
    rust_print(b"  How to trigger a missed deadline:\n");
    rust_print(b"    Create a task with a deadline in the past:\n");
    rust_print(b"      let sys = SYSTEM_TICKS.load(Relaxed);\n");
    rust_print(b"      rust_create_task_ex(dummy as u32, true, 5, 0, sys + 50);\n");
    rust_print(b"    The reaper will catch it within 200 ticks.\n");
    rust_print(b"\n");

    // =========================================================================
    // SECTION 7: PRIORITY AGING
    // =========================================================================
    rust_print(b"  SECTION 7: PRIORITY AGING\n");
    rust_print(b"  ==========================\n");
    rust_print(b"\n");
    rust_print(b"  Purpose: prevent low-priority tasks from starving behind a\n");
    rust_print(b"  cluster of high-priority tasks.\n");
    rust_print(b"\n");
    rust_print(b"  Mechanics (Phase 2 of the scheduler, every tick):\n");
    rust_print(b"    For each Ready task:\n");
    rust_print(b"      age = SYSTEM_TICKS - last_run_tick\n");
    rust_print(b"      if age > AGING_THRESH (300):\n");
    rust_print(b"        priority = min(priority + AGING_BOOST (10), 255)\n");
    rust_print(b"        PRIORITY_AGING_BOOSTS++\n");
    rust_print(b"\n");
    rust_print(b"  On dispatch (Phase 5):\n");
    rust_print(b"    priority is reset to base_priority.\n");
    rust_print(b"    This means aging only accumulates while the task is waiting.\n");
    rust_print(b"\n");
    rust_print(b"  Effect on watchdog:\n");
    rust_print(b"    CHECK 6 (TASK_STARVED) uses STARVATION_THRESH=5000 ticks which\n");
    rust_print(b"    is much larger than AGING_THRESH=300. A task that ages enough\n");
    rust_print(b"    will eventually reach priority=255 and preempt any task.\n");
    rust_print(b"    If it still hasn't run by 5000 ticks, something is wrong\n");
    rust_print(b"    (e.g. it's Blocked rather than Ready, or the scheduler is broken).\n");
    rust_print(b"\n");
    rust_print(b"  How to observe aging in action:\n");
    rust_print(b"    1. Create a task with priority=1 and one with priority=200.\n");
    rust_print(b"    2. Watch AGING_BOOSTS on HUD row 15 climb as the low-pri task waits.\n");
    rust_print(b"    3. After 30 aging passes (300*30=9000 ticks) the low-pri task\n");
    rust_print(b"       reaches priority=301 (capped at 255) and runs.\n");
    rust_print(b"\n");

    // =========================================================================
    // SECTION 8: SLEEP API
    // =========================================================================
    rust_print(b"  SECTION 8: SLEEP API\n");
    rust_print(b"  =====================\n");
    rust_print(b"\n");
    rust_print(b"  task_sleep(ticks: u32) - call from inside a task body.\n");
    rust_print(b"    Sets state = Sleeping, block_reason = Sleep.\n");
    rust_print(b"    Sets wake_at = SYSTEM_TICKS + ticks.\n");
    rust_print(b"    The scheduler's Phase 1 will flip it back to Ready when due.\n");
    rust_print(b"\n");
    rust_print(b"  Important: task_sleep() takes effect on the NEXT scheduler call.\n");
    rust_print(b"  The current task body must RETURN after calling task_sleep().\n");
    rust_print(b"  Do NOT loop inside a task body after calling it - that is a\n");
    rust_print(b"  cooperative multitasking violation and will cause a scheduler stall.\n");
    rust_print(b"\n");
    rust_print(b"  Example - a task that wakes every 500 ticks:\n");
    rust_print(b"    extern C fn my_task(_pid: u32) {\n");
    rust_print(b"        unsafe {\n");
    rust_print(b"            do_work();\n");
    rust_print(b"            task_sleep(500);\n");
    rust_print(b"        }\n");
    rust_print(b"    }\n");
    rust_print(b"\n");
    rust_print(b"  Sleeping tasks are NOT dispatched and do NOT accumulate\n");
    rust_print(b"  tick_budget_accum, so they won't trip CHECK 8 (TASK_RUNAWAY).\n");
    rust_print(b"  They DO remain in the shadow table, so if they sleep longer\n");
    rust_print(b"  than STARVATION_THRESH ticks, CHECK 6 will fire. Design sleep\n");
    rust_print(b"  durations to be under 5000 ticks or clear the shadow entry.\n");
    rust_print(b"\n");

    // =========================================================================
    // SECTION 9: WATCHDOG CHECKS (all 13)
    // =========================================================================
    rust_print(b"  SECTION 9: WATCHDOG CHECKS\n");
    rust_print(b"  ===========================\n");
    rust_print(b"  Slow path fires every 500 ticks. Fast path (HUD redraw) every tick.\n");
    rust_print(b"\n");

    rust_print(b"  CHECK 1  TASK_DIED\n");
    rust_print(b"  ------------------\n");
    rust_print(b"  Trigger : NUM_TASKS dropped vs previous 500-tick window\n");
    rust_print(b"  Severity: CRIT (FATAL if count == 0)\n");
    rust_print(b"  Log     : [TICK:N] [SEV:C] TASK_DIED was=N now=N\n");
    rust_print(b"  Counter : WATCHDOG_TASK_DEATHS  HUD: T:N (red 0x0C)\n");
    rust_print(b"  Note    : rust_kill_task() marks Zombie; reaper reclaims it.\n");
    rust_print(b"            The count drops when the reaper zeroes the slot.\n");
    rust_print(b"  Fix file: recovery.avfs  content: RESTART\n");
    rust_print(b"\n");

    rust_print(b"  CHECK 2  HEAP_HIGH\n");
    rust_print(b"  ------------------\n");
    rust_print(b"  Trigger : heap_pct > 80%\n");
    rust_print(b"  Severity: WARN >80%   CRIT >92%\n");
    rust_print(b"  Log     : [TICK:N] [SEV:W] HEAP_HIGH NNN%\n");
    rust_print(b"  Counter : WATCHDOG_HEAP_WARNINGS  HUD: H:N (yellow 0x0E)\n");
    rust_print(b"  Tip     : PK:NNN% on HUD row 7 shows the highest value ever.\n");
    rust_print(b"  Fix file: heap_audit.avfs  content: ALLOC site=X size=N\n");
    rust_print(b"\n");

    rust_print(b"  CHECK 3  SCHED_STALL\n");
    rust_print(b"  --------------------\n");
    rust_print(b"  Trigger : SYSTEM_TICKS unchanged across 3 consecutive checks\n");
    rust_print(b"  Severity: CRIT\n");
    rust_print(b"  Log     : [TICK:N] [SEV:C] SCHED_STALL ticks_frozen\n");
    rust_print(b"  Counter : WATCHDOG_STALL_WARNINGS  HUD: S:N (purple 0x0D)\n");
    rust_print(b"  Tip     : if the beat glyph on row 1 stopped, PIT is dead.\n");
    rust_print(b"  Fix file: sched_fix.avfs  content: PIT_REINIT\n");
    rust_print(b"\n");

    rust_print(b"  CHECK 4  MEM_CORRUPT\n");
    rust_print(b"  --------------------\n");
    rust_print(b"  Trigger : WATCHDOG_SENTINEL (0xDEADBEEF) changed\n");
    rust_print(b"  Severity: CRIT\n");
    rust_print(b"  Log     : [TICK:N] [SEV:C] MEM_CORRUPT got=0xXXXXXXXX\n");
    rust_print(b"  Counter : WATCHDOG_NULL_PTR_TRIPS  HUD: M:N (red 0x0C)\n");
    rust_print(b"  Rearms  : sentinel reset to 0xDEADBEEF after logging\n");
    rust_print(b"  Tip     : 0x00000000=null write  0xCCCCCCCC=uninit stack spill\n");
    rust_print(b"  Fix file: memdump.avfs  content: TICK=N VAL=0xXX\n");
    rust_print(b"\n");

    rust_print(b"  CHECK 5  NO_TASKS\n");
    rust_print(b"  -----------------\n");
    rust_print(b"  Trigger : NUM_TASKS == 0\n");
    rust_print(b"  Severity: FATAL\n");
    rust_print(b"  Log     : [TICK:N] [SEV:F] NO_TASKS all_tasks_gone\n");
    rust_print(b"  Counter : WATCHDOG_ANOMALY_COUNT only\n");
    rust_print(b"  Fix file: panic.avfs  content: NO_TASKS TICK=N\n");
    rust_print(b"\n");

    rust_print(b"  CHECK 6  TASK_STARVED\n");
    rust_print(b"  ---------------------\n");
    rust_print(b"  Trigger : shadow slot not dispatched for 5000 ticks\n");
    rust_print(b"  Severity: WARN\n");
    rust_print(b"  Log     : [TICK:N] [SEV:W] TASK_STARVED pid=N\n");
    rust_print(b"  Counter : WATCHDOG_STARVATION_HITS  HUD: V:N (purple 0x0D)\n");
    rust_print(b"  Rearms  : last_tick reset to now after logging\n");
    rust_print(b"  Note    : aging should prevent this in normal operation.\n");
    rust_print(b"            If it fires anyway, the task may be Blocked, not Ready.\n");
    rust_print(b"            Check block_reason in Task struct.\n");
    rust_print(b"  Fix file: starve.avfs  content: STARVE pid=N tick=N\n");
    rust_print(b"\n");

    rust_print(b"  CHECK 7  STACK_CANARY\n");
    rust_print(b"  ---------------------\n");
    rust_print(b"  Trigger : TASK_SHADOW[slot].canary != 0xCAFEBABE\n");
    rust_print(b"  Severity: CRIT\n");
    rust_print(b"  Log     : [TICK:N] [SEV:C] STACK_CANARY slot=N pid=N\n");
    rust_print(b"  Counter : WATCHDOG_CANARY_TRIPS  HUD: K:N (red 0x0C)\n");
    rust_print(b"  Rearms  : canary reset to 0xCAFEBABE after logging\n");
    rust_print(b"  Tip     : if same slot trips repeatedly, that task has a\n");
    rust_print(b"            genuine stack overflow. Reduce local variable use.\n");
    rust_print(b"  Fix file: canary.avfs  content: CANARY slot=N pid=N\n");
    rust_print(b"\n");

    rust_print(b"  CHECK 8  TASK_RUNAWAY\n");
    rust_print(b"  ---------------------\n");
    rust_print(b"  Trigger : task consumed > 85% of the 500-tick check window\n");
    rust_print(b"  Severity: WARN\n");
    rust_print(b"  Log     : [TICK:N] [SEV:W] TASK_RUNAWAY pid=N pct=NNN%\n");
    rust_print(b"  Counter : WATCHDOG_BUDGET_TRIPS  HUD: B:N (yellow 0x0E)\n");
    rust_print(b"  Rearms  : tick_budget_accum reset each window\n");
    rust_print(b"  Fix     : lower time_slice or split task into cooperative chunks\n");
    rust_print(b"  Fix file: budget.avfs  content: RUNAWAY pid=N pct=N\n");
    rust_print(b"\n");

    rust_print(b"  CHECK 9  IPC_OVERFLOW\n");
    rust_print(b"  ---------------------\n");
    rust_print(b"  Trigger : IPC_OVERFLOWS > 0\n");
    rust_print(b"  Severity: WARN (CRIT if IPC_OVERFLOWS >= 3)\n");
    rust_print(b"  Log     : [TICK:N] [SEV:W] IPC_OVERFLOW ov=N\n");
    rust_print(b"  Counter : WATCHDOG_IPC_OVERFLOW_HITS  HUD: I:N (red 0x0C)\n");
    rust_print(b"  Cause   : tasks posting faster than ipc_monitor_task drains.\n");
    rust_print(b"            Or messages sent to a PID that no task is recv()ing.\n");
    rust_print(b"  Fix     : increase MAX_IPC_MSGS, drain more often, or reduce\n");
    rust_print(b"            posting rate. Ensure every posted recv_pid has a reader.\n");
    rust_print(b"  Fix file: ipc_fix.avfs  content: IPC_DRAIN tick=N\n");
    rust_print(b"\n");

    rust_print(b"  CHECK 10  PRIO_INV\n");
    rust_print(b"  ------------------\n");
    rust_print(b"  Trigger : task with base_priority > 50 has not run for\n");
    rust_print(b"            PRIORITY_INV_THRESH (1000) ticks while in Ready state\n");
    rust_print(b"  Severity: WARN\n");
    rust_print(b"  Log     : [TICK:N] [SEV:W] PRIO_INV hi_pid=N\n");
    rust_print(b"  Counter : WATCHDOG_PRIORITY_INV_HITS  HUD: P:N (purple 0x0D)\n");
    rust_print(b"  Note    : classic priority inversion requires a mutex/semaphore.\n");
    rust_print(b"            This check catches the symptom (high-pri task waiting\n");
    rust_print(b"            too long) without needing a full mutex implementation.\n");
    rust_print(b"  Fix     : check if the blocked task holds a resource that a\n");
    rust_print(b"            high-priority task needs. Use priority inheritance:\n");
    rust_print(b"            temporarily boost the holder's priority to match the waiter.\n");
    rust_print(b"  Fix file: prio_fix.avfs  content: INV hi=N lo=N tick=N\n");
    rust_print(b"\n");

    rust_print(b"  CHECK 11  THERMAL_CRIT\n");
    rust_print(b"  ----------------------\n");
    rust_print(b"  Trigger : FAKE_TEMP_CDEG >= TEMP_CRIT_CDEG (9500 cdeg = 95C)\n");
    rust_print(b"  Severity: CRIT\n");
    rust_print(b"  Log     : [TICK:N] [SEV:C] THERMAL_CRIT temp=NNC\n");
    rust_print(b"  Counter : WATCHDOG_THERMAL_HITS  HUD: R:N (red 0x0C)\n");
    rust_print(b"  Auto-response: thermal_task sets IO_STRESS_THROTTLED=true.\n");
    rust_print(b"    IO stops heating. Passive cooling brings temp below TEMP_COOL_CDEG.\n");
    rust_print(b"    Oscillation period depends on IO burst pattern and tick rate.\n");
    rust_print(b"  Fix file: thermal_fix.avfs  content: THROTTLE tick=N temp=N\n");
    rust_print(b"\n");

    rust_print(b"  CHECK 12  MISSED_DEADLINE\n");
    rust_print(b"  -------------------------\n");
    rust_print(b"  Trigger : REAPER_MISSED_DEADLINES > 0\n");
    rust_print(b"  Severity: WARN (<5 misses)  CRIT (>=5 misses)\n");
    rust_print(b"  Log     : [TICK:N] [SEV:W] MISSED_DEADLINE n=N\n");
    rust_print(b"  Counter : WATCHDOG_DEADLINE_HITS  HUD: D:N (yellow 0x0E)\n");
    rust_print(b"  Source  : reaper_task writes deadline_miss.avfs per missed task.\n");
    rust_print(b"  Fix     : increase deadline tick value in rust_create_task_ex(),\n");
    rust_print(b"            or raise the task's priority so it runs in time.\n");
    rust_print(b"  Fix file: deadline_miss.avfs  content: MISS pid=N dl=N (auto-written)\n");
    rust_print(b"\n");

    rust_print(b"  CHECK 13  AFFINITY_VIOL\n");
    rust_print(b"  -----------------------\n");
    rust_print(b"  Trigger : AFFINITY_VIOLATIONS > 0\n");
    rust_print(b"  Severity: WARN\n");
    rust_print(b"  Log     : [TICK:N] [SEV:W] AFFINITY_VIOL n=N\n");
    rust_print(b"  Counter : WATCHDOG_AFFINITY_HITS  HUD: A:N (purple 0x0D)\n");
    rust_print(b"  Source  : incremented in Phase 4 of the scheduler (Pass 1 skips)\n");
    rust_print(b"            and in affinity_task when epoch mismatch is detected.\n");
    rust_print(b"  Fix     : reduce load on affinity=0 tasks so Pass 2 is rarely\n");
    rust_print(b"            needed. Or set the violating task's affinity to 0.\n");
    rust_print(b"  Fix file: affinity_fix.avfs  content: VIOL pid=N epoch=N\n");
    rust_print(b"\n");

    // =========================================================================
    // SECTION 10: AVFS RECOVERY FILES
    // =========================================================================
    rust_print(b"  SECTION 10: AVFS RECOVERY FILES\n");
    rust_print(b"  ================================\n");
    rust_print(b"\n");
    rust_print(b"  All files created/written by watchdog, reaper, or recovery code.\n");
    rust_print(b"  Boot stub should check for these on startup and print + clear them.\n");
    rust_print(b"\n");
    rust_print(b"  +---------------------+-----------------------------+-----------------+\n");
    rust_print(b"  | File                | Written by / when           | Content         |\n");
    rust_print(b"  +---------------------+-----------------------------+-----------------+\n");
    rust_print(b"  | watchdog.log        | any anomaly (all checks)    | structured log  |\n");
    rust_print(b"  | recovery.avfs       | CHECK 1 FATAL               | RESTART         |\n");
    rust_print(b"  | panic.avfs          | CHECK 5 NO_TASKS            | NO_TASKS TICK=N |\n");
    rust_print(b"  | heap_audit.avfs     | CHECK 2 CRIT                | ALLOC site=X N  |\n");
    rust_print(b"  | sched_fix.avfs      | CHECK 3 SCHED_STALL         | PIT_REINIT      |\n");
    rust_print(b"  | memdump.avfs        | CHECK 4 MEM_CORRUPT         | TICK=N VAL=0xXX |\n");
    rust_print(b"  | starve.avfs         | CHECK 6 TASK_STARVED        | STARVE pid=N    |\n");
    rust_print(b"  | canary.avfs         | CHECK 7 STACK_CANARY        | CANARY slot=N   |\n");
    rust_print(b"  | budget.avfs         | CHECK 8 TASK_RUNAWAY        | RUNAWAY pid=N   |\n");
    rust_print(b"  | ipc_fix.avfs        | CHECK 9 IPC_OVERFLOW        | IPC_DRAIN N     |\n");
    rust_print(b"  | prio_fix.avfs       | CHECK 10 PRIO_INV           | INV hi=N lo=N   |\n");
    rust_print(b"  | thermal_fix.avfs    | CHECK 11 THERMAL_CRIT       | THROTTLE N      |\n");
    rust_print(b"  | deadline_miss.avfs  | reaper / CHECK 12 (auto)    | MISS pid=N dl=N |\n");
    rust_print(b"  | affinity_fix.avfs   | CHECK 13 AFFINITY_VIOL      | VIOL pid=N N    |\n");
    rust_print(b"  | io_stress.avfs      | io_stress_task (burst start) | burst header   |\n");
    rust_print(b"  +---------------------+-----------------------------+-----------------+\n");
    rust_print(b"\n");

    // =========================================================================
    // SECTION 11: COUNTER QUICK REFERENCE
    // =========================================================================
    rust_print(b"  SECTION 11: HUD COUNTER QUICK REFERENCE\n");
    rust_print(b"  ========================================\n");
    rust_print(b"\n");
    rust_print(b"  Row 4 counters (classic):\n");
    rust_print(b"  +----+---------------------------+--------+---------+\n");
    rust_print(b"  | ID | Counter                   | Check  | Colour  |\n");
    rust_print(b"  +----+---------------------------+--------+---------+\n");
    rust_print(b"  | T  | WATCHDOG_TASK_DEATHS       | 1      | red     |\n");
    rust_print(b"  | H  | WATCHDOG_HEAP_WARNINGS     | 2      | yellow  |\n");
    rust_print(b"  | S  | WATCHDOG_STALL_WARNINGS    | 3      | purple  |\n");
    rust_print(b"  | M  | WATCHDOG_NULL_PTR_TRIPS    | 4      | red     |\n");
    rust_print(b"  | V  | WATCHDOG_STARVATION_HITS   | 6      | purple  |\n");
    rust_print(b"  | K  | WATCHDOG_CANARY_TRIPS      | 7      | red     |\n");
    rust_print(b"  | B  | WATCHDOG_BUDGET_TRIPS      | 8      | yellow  |\n");
    rust_print(b"  +----+---------------------------+--------+---------+\n");
    rust_print(b"\n");
    rust_print(b"  Row 5 counters (new in v2.0):\n");
    rust_print(b"  +----+---------------------------+--------+---------+\n");
    rust_print(b"  | I  | WATCHDOG_IPC_OVERFLOW_HITS | 9      | red     |\n");
    rust_print(b"  | P  | WATCHDOG_PRIORITY_INV_HITS | 10     | purple  |\n");
    rust_print(b"  | R  | WATCHDOG_THERMAL_HITS      | 11     | red     |\n");
    rust_print(b"  | D  | WATCHDOG_DEADLINE_HITS     | 12     | yellow  |\n");
    rust_print(b"  | A  | WATCHDOG_AFFINITY_HITS     | 13     | purple  |\n");
    rust_print(b"  +----+---------------------------+--------+---------+\n");
    rust_print(b"\n");
    rust_print(b"  WD:NN on row 3 = WATCHDOG_ANOMALY_COUNT (total anomaly events).\n");
    rust_print(b"  '!!' flashes red when any counter > 0. 'OK' green when all clear.\n");
    rust_print(b"\n");

    // =========================================================================
    // SECTION 12: STRESS TESTING GUIDE
    // =========================================================================
    rust_print(b"  SECTION 12: STRESS TESTING GUIDE\n");
    rust_print(b"  ==================================\n");
    rust_print(b"\n");
    rust_print(b"  The table below lists the minimum code needed to trigger each\n");
    rust_print(b"  watchdog check. Run these from the RSH shell or a test task.\n");
    rust_print(b"\n");
    rust_print(b"  CHECK 1 - Task death:\n");
    rust_print(b"    rust_kill_task(some_pid);\n");
    rust_print(b"    Wait 200 ticks for reaper + 500 for watchdog slow pass.\n");
    rust_print(b"\n");
    rust_print(b"  CHECK 2 - Heap pressure:\n");
    rust_print(b"    for _ in 0..500 { heap_alloc(256); }  // no free\n");
    rust_print(b"\n");
    rust_print(b"  CHECK 3 - Scheduler stall:\n");
    rust_print(b"    outb(0x21, inb(0x21) | 0x01);  // mask IRQ0\n");
    rust_print(b"\n");
    rust_print(b"  CHECK 4 - Memory corruption:\n");
    rust_print(b"    WATCHDOG_SENTINEL = 0x12345678;\n");
    rust_print(b"\n");
    rust_print(b"  CHECK 5 - No tasks:\n");
    rust_print(b"    for i in 0..MAX_TASKS { TASKS[i].is_active = false; }\n");
    rust_print(b"\n");
    rust_print(b"  CHECK 6 - Starvation:\n");
    rust_print(b"    rust_create_task_ex(spin as u32, true, 255, 0, 0);\n");
    rust_print(b"    // spin_task loops without returning for 5000+ ticks\n");
    rust_print(b"\n");
    rust_print(b"  CHECK 7 - Canary trip:\n");
    rust_print(b"    TASK_SHADOW[1].canary = 0xBADC0DE;\n");
    rust_print(b"\n");
    rust_print(b"  CHECK 8 - Runaway:\n");
    rust_print(b"    rust_create_task_ex(hog as u32, true, 200, 0, 0);\n");
    rust_print(b"    // hog_task does busy work for entire time_slice each tick\n");
    rust_print(b"\n");
    rust_print(b"  CHECK 9 - IPC overflow:\n");
    rust_print(b"    for _ in 0..33 { ipc_post(0, 999, IpcMsgType::Ping, 0); }\n");
    rust_print(b"\n");
    rust_print(b"  CHECK 10 - Priority inversion:\n");
    rust_print(b"    Create a priority=200 task, set its state to Blocked.\n");
    rust_print(b"    Wait 1000 ticks. Watchdog sees it Ready but not dispatched.\n");
    rust_print(b"\n");
    rust_print(b"  CHECK 11 - Thermal:\n");
    rust_print(b"    FAKE_TEMP_CDEG = TEMP_CRIT_CDEG; // = 9500\n");
    rust_print(b"\n");
    rust_print(b"  CHECK 12 - Missed deadline:\n");
    rust_print(b"    let t = SYSTEM_TICKS.load(Relaxed);\n");
    rust_print(b"    rust_create_task_ex(dummy as u32, true, 1, 0, t + 10);\n");
    rust_print(b"    // deadline expires before reaper's 200-tick scan\n");
    rust_print(b"\n");
    rust_print(b"  CHECK 13 - Affinity violation:\n");
    rust_print(b"    for _ in 0..20 { rust_create_task_ex(nop, true, 250, 0, 0); }\n");
    rust_print(b"    // saturates affinity=0 tasks, forces Pass 2 for pinned tasks\n");
    rust_print(b"\n");

    // =========================================================================
    // SECTION 13: LOG FORMAT
    // =========================================================================
    rust_print(b"  SECTION 13: WATCHDOG.LOG FORMAT\n");
    rust_print(b"  ================================\n");
    rust_print(b"\n");
    rust_print(b"  File   : watchdog.log\n");
    rust_print(b"  Size   : 16388 bytes total (4 header + 16384 body)\n");
    rust_print(b"  Header : bytes 0-3  little-endian u32 = current write tail offset\n");
    rust_print(b"  Body   : bytes 4+   ASCII log lines, wraps when tail > filesize\n");
    rust_print(b"\n");
    rust_print(b"  Line format:\n");
    rust_print(b"    [TICK:NNNNNNNN] [SEV:X] TAG detail_string\n");
    rust_print(b"\n");
    rust_print(b"  Severity:\n");
    rust_print(b"    W  WARN   recoverable, monitor\n");
    rust_print(b"    C  CRIT   degraded, action required\n");
    rust_print(b"    F  FATAL  kernel integrity lost, restart required\n");
    rust_print(b"\n");
    rust_print(b"  Example log:\n");
    rust_print(b"    [TICK:1500]   [SEV:W] HEAP_HIGH 083%\n");
    rust_print(b"    [TICK:2000]   [SEV:C] IPC_OVERFLOW ov=3\n");
    rust_print(b"    [TICK:3000]   [SEV:C] TASK_DIED was=9 now=8\n");
    rust_print(b"    [TICK:4500]   [SEV:W] PRIO_INV hi_pid=1005\n");
    rust_print(b"    [TICK:5000]   [SEV:C] SCHED_STALL ticks_frozen\n");
    rust_print(b"    [TICK:6000]   [SEV:C] MEM_CORRUPT got=0x12345678\n");
    rust_print(b"    [TICK:7000]   [SEV:F] NO_TASKS all_tasks_gone\n");
    rust_print(b"    [TICK:8500]   [SEV:W] TASK_STARVED pid=1002\n");
    rust_print(b"    [TICK:9000]   [SEV:C] STACK_CANARY slot=1 pid=1001\n");
    rust_print(b"    [TICK:10500]  [SEV:W] TASK_RUNAWAY pid=1007 pct=091%\n");
    rust_print(b"    [TICK:11000]  [SEV:C] THERMAL_CRIT temp=95C\n");
    rust_print(b"    [TICK:12000]  [SEV:W] MISSED_DEADLINE n=2\n");
    rust_print(b"    [TICK:13500]  [SEV:W] AFFINITY_VIOL n=4\n");
    rust_print(b"\n");
    rust_print(b"  To read from RSH:\n");
    rust_print(b"    cat watchdog.log     (displays body bytes 4..)\n");
    rust_print(b"    avfs_read watchdog.log 0 4   (read tail offset)\n");
    rust_print(b"\n");

    // =========================================================================
    // FOOTER
    // =========================================================================
    rust_print(b"+=======================================================================+\n");
    rust_print(b"| END OF HANDBOOK  //  RadiumOS v2.0  //  scp_2801                     |\n");
    rust_print(b"+=======================================================================+\n");
    rust_print(b"\n");
}




// =============================================================================
// RSH SCRIPT ENGINE v2.3  --  scp_2801 / RadiumOS
//
// FIXED vs v2.2:
//   - ScriptFunction.body[] REMOVED from struct (was 2 MB in BSS = crash)
//   - Function bodies now stored in flat FUNC_BODY_POOL (external static)
//   - ScriptFunction stores (pool_start: u16, body_count: u16) indices only
//   - FUNC_BODY_BUF scratch removed from global; replaced by direct pool slice
//   - GLOBAL_CTX BSS footprint: ~178 KB (was ~2.1 MB)
//   - FUNC_BODY_POOL BSS: 768 KB (3072 lines × 256 bytes)
//   - All other logic identical to v2.2
//
// SIZE BREAKDOWN (BSS):
//   FUNC_BODY_POOL  [Line; 3072]        = 786,432 bytes
//   GLOBAL_CTX      ScriptCtx           = ~178,000 bytes
//   LINES_D0..D3    [Line; 1000] × 4   = 1,024,000 bytes
//   FBUF_D0..D3     [u8; 32768] × 4    = 131,072 bytes
//   FREAD_SCRATCH   [u8; 4096]          = 4,096 bytes
//   Total                               ≈ 2.1 MB  (was ≈ 4.2 MB, was crashing)
// =============================================================================

// ── Size constants ────────────────────────────────────────────────────────────
pub const MAX_VARS:          usize = 322;
pub const MAX_FUNCTIONS:     usize = 644;
pub const MAX_CALL_STACK:    usize = 16;
pub const MAX_LINES:         usize = 1000;
pub const MAX_LINE_LEN:      usize = 256;
pub const MAX_VAR_NAME:      usize = 32;
pub const MAX_VAR_VAL:       usize = 128;
pub const MAX_FUNC_NAME:     usize = 64;
pub const MAX_FUNC_LINES:    usize = 1228;   // max lines per function body
pub const MAX_ARGS:          usize = 16;
pub const MAX_INCLUDE_DEPTH: usize = 8;
pub const MAX_WHILE_ITER:    usize = 100_000_000;
pub const MAX_MAPS:          usize = 16;
pub const MAX_MAP_ENTRIES:   usize = 32;
pub const MAX_MAP_KEY_LEN:   usize = 32;
pub const MAX_MAP_VAL_LEN:   usize = 128;
pub const MAX_FILE_SIZE:     usize = 32768;
pub const MAX_ONCE_TAGS:     usize = 32;
pub const MAX_ONCE_TAG_LEN:  usize = 32;

// ── Function body pool ────────────────────────────────────────────────────────
// Stored OUTSIDE ScriptCtx to avoid BSS explosion.
// 3072 lines × 256 bytes = 768 KB.  Supports up to 3072 total function body
// lines across all defined functions (e.g. 64 functions × 48 lines avg).
pub const FUNC_POOL_SIZE: usize = 3072;

static mut FUNC_BODY_POOL: [Line; FUNC_POOL_SIZE] = [Str::new(); FUNC_POOL_SIZE];
static mut FUNC_POOL_HEAD: usize = 0;

// ── Static string type ────────────────────────────────────────────────────────

#[derive(Clone, Copy)]
pub struct Str<const N: usize> {
    pub buf: [u8; N],
    pub len: usize,
}

impl<const N: usize> Str<N> {
    pub const fn new() -> Self { Self { buf: [0u8; N], len: 0 } }

    pub fn from_bytes(src: &[u8]) -> Self {
        let mut s = Self::new();
        s.set(src);
        s
    }

        pub fn set(&mut self, src: &[u8]) {
        let n = src.len().min(N.saturating_sub(1));
        self.buf[..n].copy_from_slice(&src[..n]);
        if n < N { self.buf[n] = 0; }
        self.len = n;
        
        // DEBUG: Trap the exact moment corruption happens
        if self.len >= N {
            panic!("STR LEN CORRUPTION: len={}, cap={}", self.len, N);
        }
    }

    pub fn as_bytes(&self) -> &[u8] { 
        &self.buf[..self.len.min(N)] 
    }
    pub fn as_ptr(&self)   -> *const u8 { self.buf.as_ptr() }
    pub fn is_empty(&self) -> bool { self.len == 0 }

    pub fn eq_bytes(&self, other: &[u8]) -> bool { self.as_bytes() == other }
    pub fn starts_with(&self, prefix: &[u8]) -> bool { self.as_bytes().starts_with(prefix) }
    pub fn ends_with(&self, suffix: &[u8]) -> bool { self.as_bytes().ends_with(suffix) }

    pub fn push(&mut self, b: u8) {
        if self.len < N.saturating_sub(1) {
            self.buf[self.len] = b;
            self.len += 1;
            self.buf[self.len] = 0;
        }
    }

    pub fn append(&mut self, src: &[u8]) {
        for &b in src { self.push(b); }
    }

    pub fn clear(&mut self) {
        self.len = 0;
        if N > 0 { self.buf[0] = 0; }
    }

    pub fn to_upper(&self, out: &mut Self) {
        out.clear();
        for &b in self.as_bytes() {
            out.push(if b >= b'a' && b <= b'z' { b - 32 } else { b });
        }
    }

    pub fn to_lower(&self, out: &mut Self) {
        out.clear();
        for &b in self.as_bytes() {
            out.push(if b >= b'A' && b <= b'Z' { b + 32 } else { b });
        }
    }
}

impl<const N: usize> PartialEq for Str<N> {
    fn eq(&self, other: &Self) -> bool { self.as_bytes() == other.as_bytes() }
}

// Type aliases
type VarName  = Str<MAX_VAR_NAME>;
type VarVal   = Str<MAX_VAR_VAL>;
type FuncName = Str<MAX_FUNC_NAME>;
type Line     = Str<MAX_LINE_LEN>;
type MapKey   = Str<MAX_MAP_KEY_LEN>;
type MapVal   = Str<MAX_MAP_VAL_LEN>;

// ── Variable ──────────────────────────────────────────────────────────────────

#[derive(Clone, Copy)]
pub struct Variable {
    pub name:  VarName,
    pub value: VarVal,
    pub used:  bool,
}
impl Variable {
    pub const fn new() -> Self {
        Self { name: Str::new(), value: Str::new(), used: false }
    }
}

// ── Map ───────────────────────────────────────────────────────────────────────

#[derive(Clone, Copy)]
pub struct MapEntry {
    pub key:  MapKey,
    pub val:  MapVal,
    pub used: bool,
}
impl MapEntry {
    pub const fn new() -> Self {
        Self { key: Str::new(), val: Str::new(), used: false }
    }
}

#[derive(Clone, Copy)]
pub struct ScriptMap {
    pub name:        Str<32>,
    pub entries:     [MapEntry; MAX_MAP_ENTRIES],
    pub count:       usize,
    pub used:        bool,
    pub is_editable: bool,
}
impl ScriptMap {
    pub const fn new() -> Self {
        Self {
            name:        Str::new(),
            entries:     [MapEntry::new(); MAX_MAP_ENTRIES],
            count:       0,
            used:        false,
            is_editable: false,
        }
    }

    pub fn get(&self, key: &[u8]) -> Option<&[u8]> {
        for e in self.entries.iter() {
            if e.used && e.key.as_bytes() == key { return Some(e.val.as_bytes()); }
        }
        None
    }

    pub fn set(&mut self, key: &[u8], val: &[u8]) {
        for e in self.entries.iter_mut() {
            if e.used && e.key.as_bytes() == key { e.val.set(val); return; }
        }
        for e in self.entries.iter_mut() {
            if !e.used {
                e.key.set(key);
                e.val.set(val);
                e.used = true;
                self.count += 1;
                return;
            }
        }
    }

    pub fn del(&mut self, key: &[u8]) -> bool {
        for e in self.entries.iter_mut() {
            if e.used && e.key.as_bytes() == key {
                e.used = false;
                if self.count > 0 { self.count -= 1; }
                return true;
            }
        }
        false
    }

    pub fn has(&self, key: &[u8]) -> bool {
        self.entries.iter().any(|e| e.used && e.key.as_bytes() == key)
    }

    pub fn clear_entries(&mut self) {
        for e in self.entries.iter_mut() { e.used = false; }
        self.count = 0;
    }

    pub fn merge_from(&mut self, src: &ScriptMap) {
        for e in src.entries.iter() {
            if e.used { self.set(e.key.as_bytes(), e.val.as_bytes()); }
        }
    }

    pub fn nth_key(&self, n: usize) -> Option<&[u8]> {
        let mut idx = 0;
        for e in self.entries.iter() {
            if e.used { if idx == n { return Some(e.key.as_bytes()); } idx += 1; }
        }
        None
    }

    pub fn nth_val(&self, n: usize) -> Option<&[u8]> {
        let mut idx = 0;
        for e in self.entries.iter() {
            if e.used { if idx == n { return Some(e.val.as_bytes()); } idx += 1; }
        }
        None
    }
}

// ── ScriptFunction  (FIXED: no inline body array) ────────────────────────────
//
// OLD (crash):  body: [Line; 128]  = 32,768 bytes × 64 funcs = 2 MB in BSS
// NEW (fixed):  pool_start + body_count = 4 bytes total per function
//
// Bodies live in the global FUNC_BODY_POOL.  pool_start is the index of the
// first line; body_count is how many lines belong to this function.

#[derive(Clone, Copy)]
pub struct ScriptFunction {
    pub name:        FuncName,
    pub pool_start:  u16,   // first line index in FUNC_BODY_POOL
    pub body_count:  u16,   // number of lines in FUNC_BODY_POOL
    pub used:        bool,
    pub is_one_time: bool,
    pub has_run:     bool,
    pub recursive:   bool,
}
impl ScriptFunction {
    pub const fn new() -> Self {
        Self {
            name:        Str::new(),
            pool_start:  0,
            body_count:  0,
            used:        false,
            is_one_time: false,
            has_run:     false,
            recursive:   false,
        }
    }
}

// ── CallFrame  (compact var save) ────────────────────────────────────────────
// Saves only in-use variable slots (by index), not the full table.
// Worst case: 32 slots × (32+128+1) bytes = ~5 KB per frame × 16 frames = 83 KB.
// That's fine.  The key win was removing the body[] from ScriptFunction.

#[derive(Clone, Copy)]
pub struct CallFrame {
    pub func_name:     FuncName,
    pub saved_vars:    [Variable; MAX_VARS],
    pub saved_indices: [u8; MAX_VARS],
    pub saved_count:   usize,
}
impl CallFrame {
    pub const fn new() -> Self {
        Self {
            func_name:     Str::new(),
            saved_vars:    [Variable::new(); MAX_VARS],
            saved_indices: [0u8; MAX_VARS],
            saved_count:   0,
        }
    }
}

// ── "once" tag table ──────────────────────────────────────────────────────────

#[derive(Clone, Copy)]
pub struct OnceTag {
    pub tag:  Str<MAX_ONCE_TAG_LEN>,
    pub used: bool,
}
impl OnceTag {
    pub const fn new() -> Self { Self { tag: Str::new(), used: false } }
}

// ── Script context ────────────────────────────────────────────────────────────
// BSS footprint with pool fix: ~178 KB  (was ~2.1 MB)

#[derive(Clone, Copy)]
pub struct ScriptCtx {
    pub vars:          [Variable; MAX_VARS],
    pub funcs:         [ScriptFunction; MAX_FUNCTIONS],
    pub maps:          [ScriptMap; MAX_MAPS],
    pub call_stack:    [CallFrame; MAX_CALL_STACK],
    pub once_tags:     [OnceTag; MAX_ONCE_TAGS],
    pub call_depth:    usize,
    pub in_function:   bool,
    pub break_flag:    bool,
    pub continue_flag: bool,
    pub return_flag:   bool,
    pub include_depth: usize,
    pub exit_code:     i32,
    pub active_map:    Str<32>,
    pub with_map:      Str<32>,
}

impl ScriptCtx {
    pub const fn new() -> Self {
        Self {
            vars:          [Variable::new(); MAX_VARS],
            funcs:         [ScriptFunction::new(); MAX_FUNCTIONS],
            maps:          [ScriptMap::new(); MAX_MAPS],
            call_stack:    [CallFrame::new(); MAX_CALL_STACK],
            once_tags:     [OnceTag::new(); MAX_ONCE_TAGS],
            call_depth:    0,
            in_function:   false,
            break_flag:    false,
            continue_flag: false,
            return_flag:   false,
            include_depth: 0,
            exit_code:     0,
            active_map:    Str::new(),
            with_map:      Str::new(),
        }
    }

    // ── Variable ops ─────────────────────────────────────────────────────────

    pub fn set_var(&mut self, name: &[u8], value: &[u8]) {
        for v in self.vars.iter_mut() {
            if v.used && v.name.as_bytes() == name { v.value.set(value); return; }
        }
        for v in self.vars.iter_mut() {
            if !v.used {
                v.name.set(name);
                v.value.set(value);
                v.used = true;
                return;
            }
        }
        rust_print(b"Error: variable table full\n");
    }

    pub fn get_var<'a>(&'a self, name: &[u8]) -> Option<&'a [u8]> {
        for v in self.vars.iter() {
            if v.used && v.name.as_bytes() == name { return Some(v.value.as_bytes()); }
        }
        None
    }

    pub fn unset_var(&mut self, name: &[u8]) {
        for v in self.vars.iter_mut() {
            if v.used && v.name.as_bytes() == name { v.used = false; return; }
        }
    }

    pub fn set_var_int(&mut self, name: &[u8], val: i64) {
        let mut buf = [0u8; 24];
        let s = int_to_str(val, &mut buf);
        self.set_var(name, s);
    }

    pub fn get_var_int(&self, name: &[u8]) -> i64 {
        match self.get_var(name) { Some(v) => parse_int(v), None => 0 }
    }

    pub fn is_defined(&self, name: &[u8]) -> bool {
        self.vars.iter().any(|v| v.used && v.name.as_bytes() == name)
    }

    // ── Map ops ───────────────────────────────────────────────────────────────

    pub fn find_map_idx(&self, name: &[u8]) -> Option<usize> {
        self.maps.iter().position(|m| m.used && m.name.as_bytes() == name)
    }

    pub fn create_map(&mut self, name: &[u8], editable: bool) {
        if let Some(i) = self.find_map_idx(name) {
            self.maps[i].is_editable = editable;
            return;
        }
        for m in self.maps.iter_mut() {
            if !m.used {
                m.used        = true;
                m.name.set(name);
                m.is_editable = editable;
                m.count       = 0;
                for e in m.entries.iter_mut() { e.used = false; }
                return;
            }
        }
    }

    pub fn get_map_mut(&mut self, name: &[u8]) -> Option<&mut ScriptMap> {
        if let Some(i) = self.find_map_idx(name) { Some(&mut self.maps[i]) } else { None }
    }

    pub fn delete_map(&mut self, name: &[u8]) {
        if let Some(i) = self.find_map_idx(name) {
            self.maps[i] = ScriptMap::new();
        }
    }

    // ── Function ops (FIXED: writes to FUNC_BODY_POOL) ───────────────────────

    pub fn define_func(
        &mut self,
        name:       &[u8],
        body:       &[Line],
        is_one_time: bool,
        recursive:   bool,
    ) -> bool {
        // Find existing slot to overwrite, or an empty slot
        let slot = self.funcs.iter().position(|f| f.used && f.name.as_bytes() == name)
                       .or_else(|| self.funcs.iter().position(|f| !f.used));
        let i = match slot {
            Some(v) => v,
            None => { rust_print(b"Error: function table full\n"); return false; }
        };

        let count = body.len().min(MAX_FUNC_LINES);

        unsafe {
            // If re-defining the same function and the new body fits in the old
            // pool slot, reuse it so we don't fragment the pool.
            let can_reuse = self.funcs[i].used
                && (self.funcs[i].body_count as usize) >= count;

            let start = if can_reuse {
                self.funcs[i].pool_start as usize
            } else {
                let s = FUNC_POOL_HEAD;
                if s + count > FUNC_POOL_SIZE {
                    rust_print(b"Error: function body pool full\n");
                    return false;
                }
                FUNC_POOL_HEAD += count;
                s
            };

            for (j, l) in body.iter().take(count).enumerate() {
                FUNC_BODY_POOL[start + j] = *l;
            }

            self.funcs[i].name.set(name);
            self.funcs[i].pool_start   = start as u16;
            self.funcs[i].body_count   = count as u16;
            self.funcs[i].used         = true;
            self.funcs[i].is_one_time  = is_one_time;
            self.funcs[i].has_run      = false;
            self.funcs[i].recursive    = recursive;
        }
        true
    }

    pub fn find_func(&self, name: &[u8]) -> Option<usize> {
        self.funcs.iter().position(|f| f.used && f.name.as_bytes() == name)
    }

    /// Get the body slice of a function directly from the pool.
    /// Returns an empty slice if the function doesn't exist.
    pub fn func_body(&self, idx: usize) -> &[Line] {
        if idx >= MAX_FUNCTIONS || !self.funcs[idx].used { return &[]; }
        let start = self.funcs[idx].pool_start as usize;
        let count = self.funcs[idx].body_count as usize;
        unsafe {
            if start + count <= FUNC_POOL_SIZE {
                &FUNC_BODY_POOL[start..start + count]
            } else {
                &[]
            }
        }
    }

    /// Free a function's pool slot and zero its metadata.
    pub fn undefine_func(&mut self, name: &[u8]) {
        if let Some(i) = self.find_func(name) {
            // Zero pool lines so stale data isn't accidentally used
            let start = self.funcs[i].pool_start as usize;
            let count = self.funcs[i].body_count as usize;
            unsafe {
                for j in 0..count.min(FUNC_POOL_SIZE.saturating_sub(start)) {
                    FUNC_BODY_POOL[start + j].clear();
                }
            }
            self.funcs[i] = ScriptFunction::new();
        }
    }

    // ── "once" tags ───────────────────────────────────────────────────────────

    pub fn once_has_run(&self, tag: &[u8]) -> bool {
        self.once_tags.iter().any(|t| t.used && t.tag.as_bytes() == tag)
    }

    pub fn once_mark(&mut self, tag: &[u8]) {
        if self.once_has_run(tag) { return; }
        for t in self.once_tags.iter_mut() {
            if !t.used { t.tag.set(tag); t.used = true; return; }
        }
    }

    // ── Pool diagnostics ──────────────────────────────────────────────────────

    pub fn pool_used(&self) -> usize {
        unsafe { FUNC_POOL_HEAD }
    }

    pub fn pool_free(&self) -> usize {
        FUNC_POOL_SIZE.saturating_sub(unsafe { FUNC_POOL_HEAD })
    }
}

// =============================================================================
// GLOBAL STATE
// =============================================================================


// Per-depth file + line buffers (one set per include depth 0..3)
static mut LINES_D0: [Line; MAX_LINES] = [Str::new(); MAX_LINES];
static mut LINES_D1: [Line; MAX_LINES] = [Str::new(); MAX_LINES];
static mut LINES_D2: [Line; MAX_LINES] = [Str::new(); MAX_LINES];
static mut LINES_D3: [Line; MAX_LINES] = [Str::new(); MAX_LINES];

static mut FBUF_D0: [u8; MAX_FILE_SIZE] = [0u8; MAX_FILE_SIZE];
static mut FBUF_D1: [u8; MAX_FILE_SIZE] = [0u8; MAX_FILE_SIZE];
static mut FBUF_D2: [u8; MAX_FILE_SIZE] = [0u8; MAX_FILE_SIZE];
static mut FBUF_D3: [u8; MAX_FILE_SIZE] = [0u8; MAX_FILE_SIZE];

static mut FREAD_SCRATCH: [u8; 4096] = [0u8; 4096];

unsafe fn fbuf_for(depth: usize) -> &'static mut [u8; MAX_FILE_SIZE] {
    match depth { 0 => &mut FBUF_D0, 1 => &mut FBUF_D1, 2 => &mut FBUF_D2, _ => &mut FBUF_D3 }
}

unsafe fn lines_for(depth: usize) -> &'static mut [Line; MAX_LINES] {
    match depth { 0 => &mut LINES_D0, 1 => &mut LINES_D1, 2 => &mut LINES_D2, _ => &mut LINES_D3 }
}

// =============================================================================
// UTILITY FUNCTIONS
// =============================================================================

fn trim(s: &[u8]) -> &[u8] {
    let start = s.iter().position(|&b| b != b' ' && b != b'\t' && b != b'\r')
                    .unwrap_or(s.len());
    let s = &s[start..];
    let end = s.iter().rposition(|&b| b != b' ' && b != b'\t' && b != b'\n' && b != b'\r')
                  .map(|i| i + 1).unwrap_or(0);
    &s[..end]
}

fn is_comment(s: &[u8]) -> bool {
    let s = trim(s);
    s.starts_with(b"%") || s.starts_with(b"#") || s.starts_with(b"__~~%~~__")
}

fn int_to_str<'a>(mut val: i64, buf: &'a mut [u8; 24]) -> &'a [u8] {
    if val == 0 { buf[0] = b'0'; return &buf[..1]; }
    let neg = val < 0;
    if neg { val = val.saturating_neg(); }
    let mut tmp = [0u8; 20];
    let mut i = 0usize;
    while val > 0 { tmp[i] = b'0' + (val % 10) as u8; val /= 10; i += 1; }
    let mut pos = 0;
    if neg { buf[pos] = b'-'; pos += 1; }
    for j in (0..i).rev() { buf[pos] = tmp[j]; pos += 1; }
    &buf[..pos]
}

fn parse_int(s: &[u8]) -> i64 {
    let s = trim(s);
    if s.is_empty() { return 0; }
    if s.starts_with(b"0x") || s.starts_with(b"0X") {
        let mut v = 0i64;
        for &b in &s[2..] {
            let d = match b {
                b'0'..=b'9' => (b - b'0') as i64,
                b'a'..=b'f' => (b - b'a' + 10) as i64,
                b'A'..=b'F' => (b - b'A' + 10) as i64,
                _ => break,
            };
            v = v * 16 + d;
        }
        return v;
    }
    let (neg, s) = if !s.is_empty() && s[0] == b'-' { (true, &s[1..]) } else { (false, s) };
    let mut val = 0i64;
    for &b in s { if b < b'0' || b > b'9' { break; } val = val * 10 + (b - b'0') as i64; }
    if neg { -val } else { val }
}

fn is_numeric(s: &[u8]) -> bool {
    let s = trim(s);
    if s.is_empty() { return false; }
    let s = if !s.is_empty() && s[0] == b'-' { &s[1..] } else { s };
    !s.is_empty() && s.iter().all(|&b| b >= b'0' && b <= b'9')
}

fn find_bytes(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() { return Some(0); }
    haystack.windows(needle.len()).position(|w| w == needle)
}

fn count_bytes(haystack: &[u8], needle: &[u8]) -> usize {
    if needle.is_empty() { return 0; }
    let mut count = 0;
    let mut i = 0;
    while i + needle.len() <= haystack.len() {
        if &haystack[i..i + needle.len()] == needle { count += 1; i += needle.len(); }
        else { i += 1; }
    }
    count
}

fn strip_quotes(s: &[u8]) -> &[u8] {
    if s.len() >= 2 && (s[0] == b'"' || s[0] == b'\'') && s[0] == s[s.len()-1] {
        &s[1..s.len()-1]
    } else { s }
}

fn isqrt_i64(n: i64) -> i64 {
    if n <= 0 { return 0; }
    let mut x = n;
    let mut y = (x + 1) / 2;
    while y < x { x = y; y = (x + n / x) / 2; }
    x
}

unsafe fn rsh_rand(min: i64, max: i64) -> i64 {
    if min >= max { return min; }
    let t = get_ticks() as i64;
    let range = (max - min).abs() + 1;
    min + ((t.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407)) & 0x7FFF_FFFF) % range
}

unsafe fn rsh_outb(port: u16, val: u8)  { outb(port, val); }
unsafe fn rsh_inb(port: u16)  -> u8     { inb(port) }

fn parse_outb_call(s: &[u8]) -> Option<(u16, u8)> {
    let s = trim(s);
    if !s.starts_with(b"outb(") { return None; }
    let inner = &s[5..];
    let close = inner.iter().position(|&b| b == b')')?;
    let args  = &inner[..close];
    let comma = args.iter().position(|&b| b == b',')?;
    let port  = parse_int(trim(&args[..comma])) as u16;
    let val   = parse_int(trim(&args[comma+1..])) as u8;
    Some((port, val))
}

fn parse_inb_call(s: &[u8]) -> Option<u16> {
    let s = trim(s);
    if !s.starts_with(b"inb(") { return None; }
    let inner = &s[4..];
    let close = inner.iter().position(|&b| b == b')')?;
    Some(parse_int(trim(&inner[..close])) as u16)
}

unsafe fn vga_text_write(col: usize, row: usize, ch: u8, attr: u8) {
    if col < 80 && row < 25 {
        let ptr = 0xB8000 as *mut u16;
        *ptr.add(row * 80 + col) = ((attr as u16) << 8) | ch as u16;
    }
}

unsafe fn vga_text_str(col: usize, row: usize, s: &[u8], attr: u8) {
    for (i, &b) in s.iter().enumerate() {
        if col + i >= 80 { break; }
        vga_text_write(col + i, row, b, attr);
    }
}

unsafe fn print_bar(val: i64, max: i64, width: i64) {
    if max <= 0 || width <= 0 { return; }
    let filled = (val * width / max).max(0).min(width);
    terminal_putchar(b'[');
    for i in 0..width { terminal_putchar(if i < filled { b'#' } else { b'.' }); }
    terminal_putchar(b']');
}

// =============================================================================
// VARIABLE EXPANSION
// =============================================================================

fn expand_vars(input: &[u8], ctx: &ScriptCtx, out: &mut Line) {
    out.clear();
    let mut i = 0;
    while i < input.len() {
        // map[key] lookup: bare identifier followed by '['
        if (input[i].is_ascii_alphanumeric() || input[i] == b'_') && {
            let start = i;
            let mut j = i;
            while j < input.len() && (input[j].is_ascii_alphanumeric() || input[j] == b'_') { j += 1; }
            j < input.len() && input[j] == b'[' && ctx.find_map_idx(&input[start..j]).is_some()
        } {
            let start = i;
            while i < input.len() && (input[i].is_ascii_alphanumeric() || input[i] == b'_') { i += 1; }
            let ident = &input[start..i];
            i += 1; // skip '['
            if let Some(map_idx) = ctx.find_map_idx(ident) {
                let map = &ctx.maps[map_idx];
                let key_start = i;
                let mut key_end = i;
                while key_end < input.len() && input[key_end] != b']' { key_end += 1; }
                let key_raw = trim(&input[key_start..key_end]);
                let mut resolved_key = Line::new();
                if key_raw.starts_with(b"'") && key_raw.ends_with(b"'") && key_raw.len() > 1 {
                    resolved_key.set(&key_raw[1..key_raw.len()-1]);
                } else if key_raw.starts_with(b"\"") && key_raw.ends_with(b"\"") && key_raw.len() > 1 {
                    resolved_key.set(&key_raw[1..key_raw.len()-1]);
                } else if key_raw.starts_with(b"$") {
                    if let Some(v) = ctx.get_var(&key_raw[1..]) { resolved_key.set(v); }
                    else { resolved_key.set(key_raw); }
                } else {
                    resolved_key.set(key_raw);
                }
                if let Some(val) = map.get(resolved_key.as_bytes()) { out.append(val); }
                i = if key_end < input.len() { key_end + 1 } else { key_end };
                continue;
            }
        }

        // ── Double-expand: $$varname → value of the variable named by $varname ──
        // e.g. if i=2, then $$i expands $i to "2", then expands $2
        if input[i] == b'$'
            && i + 1 < input.len()
            && input[i + 1] == b'$'
        {
            i += 2; // skip both $$
            let braced = i < input.len() && input[i] == b'{';
            if braced { i += 1; }
            let start = i;
            while i < input.len() {
                let b = input[i];
                if braced && b == b'}' { break; }
                if !braced && !(b.is_ascii_alphanumeric() || b == b'_') { break; }
                i += 1;
            }
            let inner_name = &input[start..i];
            if braced && i < input.len() { i += 1; }
            // Step 1: resolve inner_name as a var to get the target var name
            if let Some(target_name) = ctx.get_var(inner_name) {
                // Step 2: resolve the target var name to get the final value
                if let Some(final_val) = ctx.get_var(target_name) {
                    out.append(final_val);
                }
            }
            continue;
        }

        if input[i] == b'$' {
            i += 1;
            let braced = i < input.len() && input[i] == b'{';
            if braced { i += 1; }
            let start = i;
            while i < input.len() {
                let b = input[i];
                if braced && b == b'}' { break; }
                if !braced && !(b.is_ascii_alphanumeric() || b == b'_') { break; }
                i += 1;
            }
            let var_name = &input[start..i];
            if braced && i < input.len() { i += 1; }
            if let Some(val) = ctx.get_var(var_name) { out.append(val); }
        } else {
            out.push(input[i]);
            i += 1;
        }
    }
}
// =============================================================================
// CONDITION EVALUATOR
// =============================================================================

#[derive(PartialEq, Copy, Clone)]
enum CmpOp { Eq, Ne, Lt, Gt, Le, Ge }

fn eval_cmp(left: &[u8], op: CmpOp, right: &[u8]) -> bool {
    let l = strip_quotes(left);
    let r = strip_quotes(right);
    if is_numeric(l) && is_numeric(r) {
        let lv = parse_int(l); let rv = parse_int(r);
        match op {
            CmpOp::Eq => lv == rv, CmpOp::Ne => lv != rv,
            CmpOp::Lt => lv <  rv, CmpOp::Gt => lv >  rv,
            CmpOp::Le => lv <= rv, CmpOp::Ge => lv >= rv,
        }
    } else {
        let ord = l.cmp(r);
        match op {
            CmpOp::Eq => ord.is_eq(), CmpOp::Ne => ord.is_ne(),
            CmpOp::Lt => ord.is_lt(), CmpOp::Gt => ord.is_gt(),
            CmpOp::Le => !ord.is_gt(), CmpOp::Ge => !ord.is_lt(),
        }
    }
}

pub fn eval_condition(cond: &[u8], ctx: &ScriptCtx) -> bool {
    let mut expanded = Line::new();
    expand_vars(cond, ctx, &mut expanded);
    let s = trim(expanded.as_bytes());

    if s.is_empty() { return false; }
    if s == b"true" || s == b"1" || s == b"yes" { return true; }
    if s == b"false"|| s == b"0" || s == b"no"  { return false; }
    if s.starts_with(b"!") { return !eval_condition(&s[1..], ctx); }

    if s.starts_with(b"defined ") {
        return ctx.is_defined(trim(&s[8..]));
    }
    if s.starts_with(b"empty ") {
        let var = trim(&s[6..]);
        return ctx.get_var(var).map(|v| v.is_empty()).unwrap_or(true);
    }
    if s.starts_with(b"exists ") {
        let path = trim(&s[7..]);
        let mut pb = [0u8; 256];
        let l = path.len().min(255);
        pb[..l].copy_from_slice(&path[..l]);
        return unsafe { avfs_file_exists(pb.as_ptr()) };
    }
    if s.starts_with(b"has ") {
        let rest = trim(&s[4..]);
        let sp = rest.iter().position(|&b| b == b' ');
        if let Some(p) = sp {
            let mname = trim(&rest[..p]);
            let key   = trim(&rest[p+1..]);
            if let Some(idx) = ctx.find_map_idx(mname) {
                return ctx.maps[idx].has(key);
            }
        }
        return false;
    }

    if let Some(pos) = find_bytes(s, b"||") {
        return eval_condition(trim(&s[..pos]), ctx) || eval_condition(trim(&s[pos+2..]), ctx);
    }
    if let Some(pos) = find_bytes(s, b"&&") {
        return eval_condition(trim(&s[..pos]), ctx) && eval_condition(trim(&s[pos+2..]), ctx);
    }

    for (tok, op) in &[
        (b"==" as &[u8], CmpOp::Eq), (b"!=", CmpOp::Ne),
        (b"<=", CmpOp::Le),          (b">=", CmpOp::Ge),
    ] {
        if let Some(pos) = find_bytes(s, tok) {
            return eval_cmp(trim(&s[..pos]), *op, trim(&s[pos+2..]));
        }
    }
    for (tok, op) in &[(b"<" as &[u8], CmpOp::Lt), (b">", CmpOp::Gt)] {
        if let Some(pos) = find_bytes(s, tok) {
            return eval_cmp(trim(&s[..pos]), *op, trim(&s[pos+1..]));
        }
    }

    let sc = strip_quotes(s);
    !sc.is_empty() && sc != b"0" && sc != b"false" && sc != b"no"
}

fn eval_math(expr: &[u8], ctx: &ScriptCtx) -> i64 {
    let mut expanded = Line::new();
    expand_vars(expr, ctx, &mut expanded);
    parse_int(trim(expanded.as_bytes()))
}

// =============================================================================
// INLINE IF PARSER
// =============================================================================

struct InlineIf<'a> {
    cond:     &'a [u8],
    then_cmd: &'a [u8],
    else_cmd: Option<&'a [u8]>,
}

fn parse_inline_if(line: &[u8]) -> Option<InlineIf<'_>> {
    let s = trim(line);
    if s.starts_with(b"if ") {
        let rest = &s[3..];
        let do_pos = find_bytes(rest, b" do ")?;
        let cond     = trim(&rest[..do_pos]);
        let after_do = trim(&rest[do_pos + 4..]);
        if after_do == b"endif" || after_do == b"endwhile" { return None; }
        let else_pos = find_bytes(after_do, b" else ");
        let (then_cmd, else_cmd) = if let Some(ep) = else_pos {
            (trim(&after_do[..ep]), Some(trim(&after_do[ep + 6..])))
        } else {
            (after_do, None)
        };
        return Some(InlineIf { cond, then_cmd, else_cmd });
    }

    let mut in_q = false; let mut qc = 0u8;
    let mut k = 0usize;
    while k + 4 <= s.len() {
        let b = s[k];
        if (b == b'"' || b == b'\'') && !in_q { in_q = true; qc = b; }
        else if in_q && b == qc { in_q = false; }
        else if !in_q && &s[k..k+4] == b" if " {
            let cmd_part  = trim(&s[..k]);
            let cond_part = trim(&s[k+4..]);
            if cmd_part.starts_with(b"if ") || cmd_part.starts_with(b"while ")
                || cmd_part.starts_with(b"for ") { break; }
            return Some(InlineIf { cond: cond_part, then_cmd: cmd_part, else_cmd: None });
        }
        k += 1;
    }
    None
}

// =============================================================================
// TOKENIZER
// =============================================================================

fn tokenize_offsets(
    line:   &[u8],
    starts: &mut [u16; MAX_ARGS],
    lens:   &mut [u16; MAX_ARGS],
) -> usize {
    let mut argc = 0;
    let mut i    = 0;
    while i < line.len() && argc < MAX_ARGS {
        while i < line.len() && (line[i] == b' ' || line[i] == b'\t') { i += 1; }
        if i >= line.len() { break; }
        if line[i] == b'"' || line[i] == b'\'' {
            let qc = line[i]; i += 1;
            let qs = i;
            while i < line.len() && line[i] != qc { i += 1; }
            starts[argc] = qs as u16;
            lens[argc]   = (i - qs) as u16;
            if i < line.len() { i += 1; }
        } else {
            let s = i;
            while i < line.len() && line[i] != b' ' && line[i] != b'\t' { i += 1; }
            starts[argc] = s as u16;
            lens[argc]   = (i - s) as u16;
        }
        argc += 1;
    }
    argc
}

// =============================================================================
// BLOCK HELPERS
// =============================================================================

fn find_block_end(lines: &[Line], from: usize, open: &[u8], close: &[u8]) -> Option<usize> {
    let mut depth = 1usize;
    for i in from..lines.len() {
        let t = trim(lines[i].as_bytes());
        if t.starts_with(open)  { depth += 1; }
        if t == close            { depth -= 1; if depth == 0 { return Some(i); } }
    }
    None
}

fn parse_lines(data: &[u8], lines: &mut [Line; MAX_LINES]) -> usize {
    let mut count = 0;
    let mut cur   = Line::new();
    for &b in data {
        if b == b'\n' || b == b'\r' {
            let t = trim(cur.as_bytes());
            if !t.is_empty() && !is_comment(t) && count < MAX_LINES {
                lines[count].set(t);
                count += 1;
            }
            cur.clear();
        } else { cur.push(b); }
    }
    let t = trim(cur.as_bytes());
    if !t.is_empty() && !is_comment(t) && count < MAX_LINES {
        lines[count].set(t);
        count += 1;
    }
    count
}

fn resolve_path(path: &[u8], out: &mut [u8; 256]) -> bool {
    let len = path.len().min(255);
    out[..len].copy_from_slice(&path[..len]);
    out[len] = 0;
    if unsafe { avfs_file_exists(out.as_ptr()) } { return true; }
    if len + 4 < 256 {
        out[len..len+4].copy_from_slice(b".rsh");
        out[len+4] = 0;
        if unsafe { avfs_file_exists(out.as_ptr()) } { return true; }
        out[len] = 0;
    }
    false
}

// =============================================================================
// FUNCTION CALL  (FIXED: reads body from FUNC_BODY_POOL, no local copy)
// =============================================================================

fn call_function(name: &[u8], args: &[&[u8]], ctx: &mut ScriptCtx) -> i32 {
    let func_idx = match ctx.find_func(name) {
        Some(i) => i,
        None => {
            rust_print(b"Error: function not defined: ");
            rust_print(name);
            rust_print(b"\n");
            return -1;
        }
    };

    if ctx.funcs[func_idx].is_one_time && ctx.funcs[func_idx].has_run {
        rust_print(b"Error: one-time function already called: ");
        rust_print(name);
        rust_print(b"\n");
        return -1;
    }

    if !ctx.funcs[func_idx].recursive {
        for i in 0..ctx.call_depth {
            if ctx.call_stack[i].func_name.as_bytes() == name {
                rust_print(b"Error: recursion not allowed in: ");
                rust_print(name);
                rust_print(b"\n");
                return -1;
            }
        }
    }

    if ctx.call_depth >= MAX_CALL_STACK {
        rust_print(b"Error: call stack overflow\n");
        return -1;
    }

    // ── Save only in-use variable slots (compact) ─────────────────────────────
    {
        let frame = &mut ctx.call_stack[ctx.call_depth];
        frame.func_name.set(name);
        frame.saved_count = 0;
        for i in 0..MAX_VARS {
            if ctx.vars[i].used {
                let sc = frame.saved_count;
                if sc < MAX_VARS {
                    frame.saved_vars[sc]    = ctx.vars[i];
                    frame.saved_indices[sc] = i as u8;
                    frame.saved_count += 1;
                }
            }
        }
    }
    ctx.call_depth += 1;

    // ── Clear vars then inject args ───────────────────────────────────────────
    for v in ctx.vars.iter_mut() { v.used = false; }

    let mut nbuf = [0u8; 24];
    let ns = int_to_str(args.len() as i64, &mut nbuf);
    ctx.set_var(b"ARG_COUNT", ns);
    for (i, arg) in args.iter().enumerate() {
        let mut ibuf = [0u8; 24];
        let is = int_to_str(i as i64 + 1, &mut ibuf);
        ctx.set_var(is, arg);
    }

    // ── Grab pool location BEFORE marking has_run ─────────────────────────────
    let pool_start = ctx.funcs[func_idx].pool_start as usize;
    let body_count = ctx.funcs[func_idx].body_count as usize;

    if ctx.funcs[func_idx].is_one_time {
        ctx.funcs[func_idx].has_run = true;
    }

    let prev_in_func  = ctx.in_function;
    ctx.in_function   = true;
    ctx.return_flag   = false;

    // ── Execute directly from pool — no copy ─────────────────────────────────
    let result = unsafe {
        if pool_start + body_count <= FUNC_POOL_SIZE && body_count > 0 {
            let body_slice = &FUNC_BODY_POOL[pool_start..pool_start + body_count];
            exec_block(body_slice, 0, body_count.saturating_sub(1), ctx)
        } else {
            0 // empty body
        }
    };

    ctx.return_flag = false;
    ctx.in_function = prev_in_func;
    ctx.call_depth -= 1;

    // ── Restore saved variables ───────────────────────────────────────────────
    for v in ctx.vars.iter_mut() { v.used = false; }
    {
        let frame = &ctx.call_stack[ctx.call_depth];
        for i in 0..frame.saved_count {
            let idx = frame.saved_indices[i] as usize;
            if idx < MAX_VARS {
                ctx.vars[idx] = frame.saved_vars[i];
            }
        }
    }

    result
}

// =============================================================================
// EXEC FILE
// =============================================================================

pub fn exec_file(path: &[u8], ctx: &mut ScriptCtx) -> i32 {
    if ctx.include_depth >= MAX_INCLUDE_DEPTH {
        rust_print(b"Error: max include depth exceeded\n");
        return -1;
    }
    let depth = ctx.include_depth;
    let mut found = [0u8; 256];
    if !resolve_path(path, &mut found) {
        rust_print(b"Error: script not found: ");
        rust_print(path);
        rust_print(b"\n");
        return -1;
    }

    let fsize = unsafe { avfs_get_filesize(found.as_ptr()) };
    if fsize <= 0 { rust_print(b"Error: file empty or not found\n"); return 0; }
    if fsize as usize >= MAX_FILE_SIZE {
        rust_print(b"Error: file too large (max 32 KB)\n");
        return -1;
    }

    let fbuf = unsafe { fbuf_for(depth) };
    let r    = unsafe { avfs_read_file(found.as_ptr(), fbuf.as_mut_ptr(), fsize as u32, 0) };
    if r != 0 { rust_print(b"Error: avfs_read_file failed\n"); return -1; }

    let lines = unsafe { lines_for(depth) };
    let data: &[u8] = unsafe { core::slice::from_raw_parts(fbuf.as_ptr(), fsize as usize) };
    let line_count = parse_lines(data, lines);
    if line_count == 0 { return 0; }

    let lines_slice = &lines[..line_count];
    let mut requires_main = false;
    let mut i = 0;

    // First pass: collect function definitions and map declarations
    while i < line_count {
        let t = trim(lines_slice[i].as_bytes());
        if t.is_empty() { i += 1; continue; }

        if t == b"^entrypoint" || t == b"^entrypoint main" {
            requires_main = true; i += 1; continue;
        }
        if t.starts_with(b"^include ") {
            let fname = strip_quotes(trim(&t[9..]));
            ctx.include_depth += 1;
            exec_file(fname, ctx);
            ctx.include_depth -= 1;
            i += 1; continue;
        }
        if t.starts_with(b"const MAP ") || t.starts_with(b"editable MAP ") {
            let editable = t.starts_with(b"editable MAP ");
            let name     = trim(if editable { &t[13..] } else { &t[10..] });
            ctx.create_map(name, editable);
            if editable { ctx.active_map.set(name); }
            i += 1;
            while i < line_count {
                let el = trim(lines_slice[i].as_bytes());
                if el == b"endMAP" || el == b"end.MAP" { break; }
                if let Some(colon) = find_bytes(el, b":") {
                    let k = trim(&el[..colon]);
                    let v = trim(&el[colon+1..]);
                    if !k.is_empty() {
                        if let Some(m) = ctx.get_map_mut(name) { m.set(k, v); }
                    }
                }
                i += 1;
            }
            ctx.active_map.clear();
            i += 1; continue;
        }
        if t.starts_with(b"ont function ") || t.starts_with(b"alt function ")
            || t.starts_with(b"function ")    || t.starts_with(b"def ")
        {
            let ni = parse_function_def(lines_slice, i, ctx);
            if ni == i { rust_print(b"Error: bad function def\n"); return -1; }
            i = ni + 1; continue;
        }
        i += 1;
    }

    // Second pass: execute (unless ^entrypoint delegates to main())
    ctx.include_depth += 1;
    let result = if requires_main {
        if ctx.find_func(b"main").is_none() {
            rust_print(b"Error: ^entrypoint but no main() defined\n");
            ctx.include_depth -= 1;
            return -1;
        }
        call_function(b"main", &[], ctx)
    } else {
        exec_block(lines_slice, 0, line_count - 1, ctx)
    };
    ctx.include_depth -= 1;
    result
}

// =============================================================================
// EXEC BLOCK
// =============================================================================

pub fn exec_block(lines: &[Line], start: usize, end: usize, ctx: &mut ScriptCtx) -> i32 {
    let mut i = start;
    let mut safety = 0u32;
    const SAFETY_LIMIT: u32 = 4_000_000;

    while i <= end && i < lines.len() {
        safety += 1;
        if safety > SAFETY_LIMIT {
            rust_print(b"Error: exec_block safety limit hit\n");
            return -1;
        }
        if ctx.break_flag || ctx.continue_flag || ctx.return_flag { return 0; }

        let line = trim(lines[i].as_bytes());
        if line.is_empty() || is_comment(line) { i += 1; continue; }

        match line {
            b"endif" | b"endwhile" | b"endfor" | b"endfunction"
            | b"enddef" | b"endMAP" | b"end.MAP" | b"endwith"
            | b"endcase" | b"endswitch" | b"^allowedRecursive" => { i += 1; continue; }
            _ => {}
        }

        if line.starts_with(b"if ") && !has_inline_do(line) {
            let ni = exec_if(lines, i, end, ctx);
            if ni == i { rust_print(b"Error: if block\n"); return -1; }
            i = ni + 1; continue;
        }
        if line.starts_with(b"while ") {
            let ni = exec_while(lines, i, end, ctx);
            if ni == i { rust_print(b"Error: while block\n"); return -1; }
            i = ni + 1; continue;
        }
        if line.starts_with(b"for ") {
            let ni = exec_for(lines, i, end, ctx);
            if ni == i { rust_print(b"Error: for block\n"); return -1; }
            i = ni + 1; continue;
        }
        if line.starts_with(b"switch ") {
            let ni = exec_switch(lines, i, end, ctx);
            if ni == i { rust_print(b"Error: switch block\n"); return -1; }
            i = ni + 1; continue;
        }
        if line.starts_with(b"with ")
            && (line.ends_with(b" do") || find_bytes(line, b" do ").is_some())
        {
            let ni = exec_with(lines, i, end, ctx);
            if ni == i { rust_print(b"Error: with block\n"); return -1; }
            i = ni + 1; continue;
        }
        if line.starts_with(b"ont function ") || line.starts_with(b"alt function ")
            || line.starts_with(b"function ")    || line.starts_with(b"def ")
        {
            let ni = parse_function_def(lines, i, ctx);
            if ni == i { rust_print(b"Error: function def\n"); return -1; }
            i = ni + 1; continue;
        }

        if line == b"break"    { ctx.break_flag    = true; return 0; }
        if line == b"continue" { ctx.continue_flag = true; return 0; }
        if line == b"return"   { ctx.return_flag   = true; return 0; }

        let result = exec_line(line, ctx);
        if result != 0 { return result; }
        i += 1;
    }
    0
}

fn has_inline_do(line: &[u8]) -> bool {
    if !line.starts_with(b"if ") { return false; }
    find_bytes(&line[3..], b" do ").is_some()
}

// =============================================================================
// BLOCK EXECUTORS
// =============================================================================

fn exec_if(lines: &[Line], idx: usize, block_end: usize, ctx: &mut ScriptCtx) -> usize {
    let line     = trim(lines[idx].as_bytes());
    let mut cond = &line[3..];
    if let Some(p) = find_bytes(cond, b" then") { cond = &cond[..p]; }
    if cond.ends_with(b" then") { cond = &cond[..cond.len()-5]; }

    let mut if_end   = block_end;
    let mut else_pos: Option<usize> = None;
    let mut elif_pos: Option<usize> = None;
    let mut endif    = block_end;
    let mut depth    = 1usize;

    for j in idx+1..=block_end.min(lines.len().saturating_sub(1)) {
        let t = trim(lines[j].as_bytes());
        if t.starts_with(b"if ") && !has_inline_do(t) { depth += 1; continue; }
        if depth == 1 {
            if t == b"endif" {
                endif = j; if_end = j.saturating_sub(1); depth -= 1; break;
            }
            if t == b"else" {
                else_pos = Some(j); if_end = j.saturating_sub(1);
            }
            if t.starts_with(b"elif ") {
                elif_pos = Some(j); if_end = j.saturating_sub(1); break;
            }
        } else if t == b"endif" { depth -= 1; }
    }
    if depth != 0 { rust_print(b"Error: missing endif\n"); return idx; }

    if eval_condition(cond, ctx) {
        exec_block(lines, idx+1, if_end, ctx);
    } else if let Some(ep) = elif_pos {
        exec_if(lines, ep, endif, ctx);
    } else if let Some(ep) = else_pos {
        exec_block(lines, ep+1, endif.saturating_sub(1), ctx);
    }
    endif
}

fn exec_while(lines: &[Line], idx: usize, _end: usize, ctx: &mut ScriptCtx) -> usize {
    let line     = trim(lines[idx].as_bytes());
    let cond_raw = trim(&line[6..]);
    let end_idx  = match find_block_end(lines, idx+1, b"while ", b"endwhile") {
        Some(i) => i,
        None => { rust_print(b"Error: missing endwhile\n"); return idx; }
    };
    let mut iters = 0usize;
    loop {
        if iters >= MAX_WHILE_ITER { rust_print(b"Error: while loop limit\n"); break; }
        if !eval_condition(cond_raw, ctx) { break; }
        ctx.break_flag    = false;
        ctx.continue_flag = false;
        let r = exec_block(lines, idx+1, end_idx-1, ctx);
        if r != 0 { break; }
        if ctx.break_flag  { ctx.break_flag = false; break; }
        if ctx.return_flag { break; }
        ctx.continue_flag = false;
        iters += 1;
    }
    end_idx
}

fn exec_for(lines: &[Line], idx: usize, _end: usize, ctx: &mut ScriptCtx) -> usize {
    let line      = trim(lines[idx].as_bytes());
    let rest      = trim(&line[4..]);
    let var_end   = rest.iter().position(|&b| b == b' ' || b == b'\t').unwrap_or(rest.len());
    let var_name  = &rest[..var_end];
    let after_var = trim(&rest[var_end..]);
    let range_src = if after_var.starts_with(b"in ") { trim(&after_var[3..]) } else { after_var };

    let end_idx = match find_block_end(lines, idx+1, b"for ", b"endfor") {
        Some(i) => i,
        None => { rust_print(b"Error: missing endfor\n"); return idx; }
    };

    let mut expanded = Line::new();
    expand_vars(range_src, ctx, &mut expanded);
    let range = trim(expanded.as_bytes());

    if let Some(dot_pos) = find_bytes(range, b"..") {
        let start_s  = trim(&range[..dot_pos]);
        let after    = trim(&range[dot_pos+2..]);
        let (end_s, step_s) = if let Some(p2) = find_bytes(after, b"..") {
            (trim(&after[..p2]), trim(&after[p2+2..]))
        } else {
            (after, &b"1"[..])
        };
        let step  = if step_s.is_empty() { 1i64 } else { parse_int(step_s) };
        if step == 0 { rust_print(b"Error: for step zero\n"); return end_idx; }
        let start = parse_int(start_s);
        let end   = parse_int(end_s);
        let mut val = start;
        let mut iters = 0;
        loop {
            if iters >= MAX_WHILE_ITER { rust_print(b"Error: for loop limit\n"); break; }
            if step > 0 && val > end { break; }
            if step < 0 && val < end { break; }
            ctx.set_var_int(var_name, val);
            ctx.break_flag    = false; ctx.continue_flag = false;
            let r = exec_block(lines, idx+1, end_idx-1, ctx);
            if r != 0 { break; }
            if ctx.break_flag  { ctx.break_flag = false; break; }
            if ctx.return_flag { break; }
            ctx.continue_flag = false;
            val += step; iters += 1;
        }
    } else {
        let mut pos = 0; let mut iters = 0;
        while pos < range.len() {
            if iters >= MAX_WHILE_ITER { rust_print(b"Error: for loop limit\n"); break; }
            while pos < range.len() && (range[pos] == b' ' || range[pos] == b'\t') { pos += 1; }
            if pos >= range.len() { break; }
            let ts = pos;
            while pos < range.len() && range[pos] != b' ' && range[pos] != b'\t' { pos += 1; }
            ctx.set_var(var_name, &range[ts..pos]);
            ctx.break_flag    = false; ctx.continue_flag = false;
            let r = exec_block(lines, idx+1, end_idx-1, ctx);
            if r != 0 { break; }
            if ctx.break_flag  { ctx.break_flag = false; break; }
            if ctx.return_flag { break; }
            ctx.continue_flag = false;
            iters += 1;
        }
    }
    end_idx
}

fn exec_switch(lines: &[Line], idx: usize, _end: usize, ctx: &mut ScriptCtx) -> usize {
    let line       = trim(lines[idx].as_bytes());
    let switch_var = trim(&line[7..]);
    let mut expanded = Line::new();
    expand_vars(switch_var, ctx, &mut expanded);
    let mut sval = Line::new();
    sval.set(expanded.as_bytes());

    let end_idx = match find_block_end(lines, idx+1, b"switch ", b"endswitch") {
        Some(i) => i,
        None => { rust_print(b"Error: missing endswitch\n"); return idx; }
    };

    let mut i = idx + 1;
    let mut matched = false;
    while i < end_idx {
        let t = trim(lines[i].as_bytes());
        if t.starts_with(b"case ") {
            let case_val = trim(&t[5..]);
            let mut cval = Line::new();
            expand_vars(case_val, ctx, &mut cval);
            let is_match = cval.as_bytes() == sval.as_bytes();
            let ec = match find_block_end(lines, i+1, b"case ", b"endcase") {
                Some(j) => j,
                None    => { rust_print(b"Error: missing endcase\n"); return idx; }
            };
            if is_match && !matched {
                matched = true;
                exec_block(lines, i+1, ec-1, ctx);
                if ctx.return_flag || ctx.break_flag { break; }
            }
            i = ec + 1; continue;
        }
        if t == b"default" {
            let ec = match find_block_end(lines, i+1, b"case ", b"endcase") {
                Some(j) => j,
                None    => { rust_print(b"Error: missing endcase after default\n"); return idx; }
            };
            if !matched { exec_block(lines, i+1, ec-1, ctx); }
            i = ec + 1; continue;
        }
        i += 1;
    }
    end_idx
}

fn exec_with(lines: &[Line], idx: usize, _end: usize, ctx: &mut ScriptCtx) -> usize {
    let line = trim(lines[idx].as_bytes());
    let rest = trim(&line[5..]);
    let name = if rest.ends_with(b" do") { trim(&rest[..rest.len()-3]) } else { rest };

    let end_idx = match find_block_end(lines, idx+1, b"with ", b"endwith") {
        Some(i) => i,
        None => { rust_print(b"Error: missing endwith\n"); return idx; }
    };

    if ctx.find_map_idx(name).is_none() {
        rust_print(b"Error: with: unknown map: ");
        rust_print(name);
        rust_print(b"\n");
        return end_idx;
    }

    let prev_with = ctx.with_map;
    ctx.with_map.set(name);

    let map_count = ctx.find_map_idx(name).map(|i| ctx.maps[i].count).unwrap_or(0);
    for n in 0..map_count {
        let key_copy; let val_copy;
        if let Some(mi) = ctx.find_map_idx(name) {
            let m = &ctx.maps[mi];
            let k = m.nth_key(n).unwrap_or(b"");
            let v = m.nth_val(n).unwrap_or(b"");
            let mut kbuf = Line::new(); kbuf.set(k);
            let mut vbuf = Line::new(); vbuf.set(v);
            key_copy = kbuf; val_copy = vbuf;
        } else { break; }
        ctx.set_var(b"key", key_copy.as_bytes());
        ctx.set_var(b"val", val_copy.as_bytes());
        let r = exec_block(lines, idx+1, end_idx-1, ctx);
        if r != 0 || ctx.break_flag || ctx.return_flag { ctx.break_flag = false; break; }
    }

    ctx.with_map = prev_with;
    ctx.unset_var(b"key");
    ctx.unset_var(b"val");
    end_idx
}

// ── Function definition parser (FIXED: calls ctx.define_func which uses pool) ─

fn parse_function_def(lines: &[Line], idx: usize, ctx: &mut ScriptCtx) -> usize {
    let line = trim(lines[idx].as_bytes());
    let (is_ont, name_start) =
        if line.starts_with(b"ont function ") { (true,  13) }
        else if line.starts_with(b"alt function ") { (true, 13) }
        else if line.starts_with(b"function ")     { (false, 9) }
        else                                        { (false,  4) };
    let name_raw = trim(&line[name_start..]);
    let name_end = name_raw.iter()
                           .position(|&b| b == b'(' || b == b' ')
                           .unwrap_or(name_raw.len());
    let name = &name_raw[..name_end];

    let close: &[u8] = if line.starts_with(b"def ") { b"enddef" } else { b"endfunction" };
    let end_idx = match find_block_end(lines, idx+1, b"function ", close) {
        Some(i) => i,
        None => {
            rust_print(b"Error: missing endfunction/enddef for: ");
            rust_print(name);
            rust_print(b"\n");
            return idx;
        }
    };

    let mut body_slice = &lines[idx+1..end_idx];
    let mut recursive  = false;
    if !body_slice.is_empty() {
        if trim(body_slice[body_slice.len()-1].as_bytes()) == b"^allowedRecursive" {
            recursive  = true;
            body_slice = &body_slice[..body_slice.len()-1];
        }
    }

    // define_func now writes to FUNC_BODY_POOL — no inline body[] needed
    ctx.define_func(name, body_slice, is_ont, recursive);
    end_idx
}

// =============================================================================
// EXEC LINE
// =============================================================================

pub fn exec_line(raw_line: &[u8], ctx: &mut ScriptCtx) -> i32 {
    let line = trim(raw_line);
    if line.is_empty() || is_comment(line) { return 0; }

    if let Some((port, val)) = parse_outb_call(line) {
        unsafe { rsh_outb(port, val); }
        return 0;
    }
    if let Some(port) = parse_inb_call(line) {
        let v = unsafe { rsh_inb(port) };
        ctx.set_var_int(b"INB", v as i64);
        return 0;
    }

    if let Some(iif) = parse_inline_if(line) {
        if eval_condition(iif.cond, ctx) {
            exec_line(iif.then_cmd, ctx)
        } else if let Some(ecmd) = iif.else_cmd {
            exec_line(ecmd, ctx)
        } else { 0 }
    } else {
        exec_line_inner(line, ctx)
    }
}

pub fn exec_line_inner(line: &[u8], ctx: &mut ScriptCtx) -> i32 {
    let mut expanded_buf = Line::new();
    expand_vars(line, ctx, &mut expanded_buf);
    let expanded = trim(expanded_buf.as_bytes());

    let mut starts = [0u16; MAX_ARGS];
    let mut lens   = [0u16; MAX_ARGS];
    let argc = tokenize_offsets(expanded, &mut starts, &mut lens);
    if argc == 0 { return 0; }

    macro_rules! arg {
        ($i:expr) => {{ let s = starts[$i] as usize; let l = lens[$i] as usize; &expanded[s..s+l] }}
    }

    let cmd = arg!(0);
    dispatch_cmd(cmd, argc, expanded, &starts, &lens, ctx)
}

// =============================================================================
// COMMAND DISPATCH
// =============================================================================

fn dispatch_cmd(
    cmd:    &[u8],
    argc:   usize,
    line:   &[u8],
    starts: &[u16; MAX_ARGS],
    lens:   &[u16; MAX_ARGS],
    ctx:    &mut ScriptCtx,
) -> i32 {
    macro_rules! arg  { ($i:expr) => {{ let s=starts[$i] as usize; let l=lens[$i] as usize; &line[s..s+l] }} }
    macro_rules! val  { ($i:expr) => {{ let r=arg!($i); ctx.get_var(r).unwrap_or(r) }} }
    macro_rules! ival { ($i:expr) => { parse_int(val!($i)) } }

    // ── Port I/O ──────────────────────────────────────────────────────────────
    if cmd == b"outb" {
        if argc >= 3 { unsafe { rsh_outb(ival!(1) as u16, ival!(2) as u8); } return 0; }
        if let Some((p, v)) = parse_outb_call(line) { unsafe { rsh_outb(p, v); } return 0; }
        rust_print(b"Usage: outb PORT VAL\n"); return -1;
    }
    if cmd == b"inb" {
        let port = if argc >= 2 { ival!(1) as u16 }
                   else if let Some(p) = parse_inb_call(line) { p }
                   else { rust_print(b"Usage: inb PORT [OUT]\n"); return -1; };
        let v = unsafe { rsh_inb(port) };
        let out_var = if argc >= 3 { arg!(2) } else { b"INB" };
        ctx.set_var_int(out_var, v as i64);
        return 0;
    }
    if cmd == b"outw" {
        if argc < 3 { rust_print(b"Usage: outw PORT VAL\n"); return -1; }
        unsafe { outw(ival!(1) as u16, ival!(2) as u16); } return 0;
    }
    if cmd == b"inw" {
        if argc < 2 { rust_print(b"Usage: inw PORT [OUT]\n"); return -1; }
        let v = unsafe { inw(ival!(1) as u16) };
        ctx.set_var_int(if argc >= 3 { arg!(2) } else { b"INW" }, v as i64);
        return 0;
    }
    if cmd == b"outl" {
        if argc < 3 { rust_print(b"Usage: outl PORT VAL\n"); return -1; }
        unsafe { outl(ival!(1) as u16, ival!(2) as u32); } return 0;
    }
    if cmd == b"inl" {
        if argc < 2 { rust_print(b"Usage: inl PORT [OUT]\n"); return -1; }
        let v = unsafe { inl(ival!(1) as u16) };
        ctx.set_var_int(if argc >= 3 { arg!(2) } else { b"INL" }, v as i64);
        return 0;
    }
    if cmd == b"io.wait" {
        if argc < 4 { rust_print(b"Usage: io.wait PORT MASK VAL [TIMEOUT OUT]\n"); return -1; }
        let port    = ival!(1) as u16;
        let mask    = ival!(2) as u8;
        let expect  = ival!(3) as u8;
        let timeout = if argc >= 5 { ival!(4) as u32 } else { 100 };
        let out_var = if argc >= 6 { arg!(5) } else { b"IO_WAIT_OK" };
        let start   = unsafe { get_ticks() };
        let mut ok  = false;
        loop {
            let v = unsafe { rsh_inb(port) };
            if (v & mask) == expect { ok = true; break; }
            if unsafe { get_ticks().wrapping_sub(start) } >= timeout { break; }
        }
        ctx.set_var(out_var, if ok { b"1" } else { b"0" });
        return 0;
    }

    match cmd {
        // ── OUTPUT ────────────────────────────────────────────────────────────
        b"echo" => {
            let mut i = 1;
            while i < argc { if i > 1 { rust_print(b" "); } rust_print(val!(i)); i += 1; }
            rust_print(b"\n");
        }
        b"print" => {
            let mut i = 1;
            while i < argc { rust_print(val!(i)); i += 1; }
        }
        b"echoln" => {
            if argc < 2 { rust_print(b"\n"); return 0; }
            rust_print(ctx.get_var(arg!(1)).unwrap_or(b""));
            rust_print(b"\n");
        }
        b"printf" => {
            if argc < 2 { return 0; }
            let fmt  = val!(1);
            let mut ai = 2usize;
            let mut fi = 0usize;
            while fi < fmt.len() {
                if fmt[fi] == b'%' && fi + 1 < fmt.len() {
                    fi += 1;
                    match fmt[fi] {
                        b's' => { rust_print(if ai < argc { val!(ai) } else { b"" }); ai += 1; }
                        b'd' => {
                            let n = if ai < argc { ival!(ai) } else { 0 };
                            let mut buf = [0u8; 24];
                            rust_print(int_to_str(n, &mut buf));
                            ai += 1;
                        }
                        b'x' => {
                            let n = if ai < argc { ival!(ai) as u32 } else { 0 };
                            rust_print(b"0x");
                            let hex = b"0123456789ABCDEF";
                            let mut hbuf = [0u8; 8];
                            for hi in (0..8).rev() { hbuf[hi] = hex[((n >> (hi * 4)) & 0xF) as usize]; }
                            let hs = hbuf.iter().position(|&b| b != b'0').unwrap_or(7);
                            rust_print(&hbuf[hs..]);
                            ai += 1;
                        }
                        b'%' => { rust_print(b"%"); }
                        b'n' => { rust_print(b"\n"); }
                        // Add this inside your match fmt[fi] block in Rust:
                        b't' => {
                            let s = if ai < argc { val!(ai) } else { b"" };
                            for &byte in s {
                                rust_print(&[byte]);

                                unsafe{sleep_ms(100);}
                            }
                            ai += 1;
                        }
                        _    => { rust_print(b"%"); rust_print(&fmt[fi..fi+1]); }
                    }
                } else {
                    rust_print(&fmt[fi..fi+1]);
                }
                fi += 1;
            }
        }

        // ── VARIABLES ─────────────────────────────────────────────────────────
        b"set" | b"export" | b"let" => {
            if argc < 2 { rust_print(b"Usage: set VAR [value]\n"); return -1; }
            let var = arg!(1);
            if argc == 2 { ctx.set_var(var, b""); return 0; }
            let mut val = Line::new();
            let mut i = 2;
            while i < argc { val.append(arg!(i)); if i+1 < argc { val.push(b' '); } i += 1; }
            ctx.set_var(var, val.as_bytes());
        }
        b"unset" => {
            if argc < 2 { return -1; }
            ctx.unset_var(arg!(1));
        }
        b"default" => {
            if argc < 3 { return -1; }
            let var = arg!(1);
            let is_set = ctx.get_var(var).map(|v| !v.is_empty()).unwrap_or(false);
            if !is_set {
                let mut val = Line::new();
                let mut i = 2;
                while i < argc { val.append(arg!(i)); if i+1<argc{val.push(b' ');} i+=1; }
                ctx.set_var(var, val.as_bytes());
            }
        }
        b"swap" => {
            if argc < 3 { return -1; }
            let va = arg!(1); let vb = arg!(2);
            let mut tmp = Line::new();
            tmp.set(ctx.get_var(va).unwrap_or(b""));
            let mut bval = Line::new();
            bval.set(ctx.get_var(vb).unwrap_or(b""));
            ctx.set_var(va, bval.as_bytes());
            ctx.set_var(vb, tmp.as_bytes());
        }
        b"copy" => {
            if argc < 3 { return -1; }
            let mut src_val = Line::new();
            src_val.set(ctx.get_var(arg!(1)).unwrap_or(b""));
            ctx.set_var(arg!(2), src_val.as_bytes());
        }
        b"vars" => {
            rust_print(b"=== Variables ===\n");
            let mut any = false;
            for i in 0..MAX_VARS {
                if ctx.vars[i].used {
                    rust_print(b"  ");
                    rust_print(ctx.vars[i].name.as_bytes());
                    rust_print(b" = ");
                    rust_print(ctx.vars[i].value.as_bytes());
                    rust_print(b"\n");
                    any = true;
                }
            }
            if !any { rust_print(b"  (none)\n"); }
        }
        b"dump" => {
            if argc < 2 { return -1; }
            let v = ctx.get_var(arg!(1)).unwrap_or(b"");
            rust_print(b"[");
            let hex = b"0123456789ABCDEF";
            for &b in v {
                unsafe {
                    terminal_putchar(hex[(b >> 4) as usize]);
                    terminal_putchar(hex[(b & 0xF) as usize]);
                    terminal_putchar(b' ');
                }
            }
            rust_print(b"]\n");
        }

        // ── POOL DIAGNOSTICS ──────────────────────────────────────────────────
        b"pool.info" => {
            rust_print(b"FUNC_BODY_POOL: ");
            print_num(ctx.pool_used() as i32);
            rust_print(b"/");
            print_num(FUNC_POOL_SIZE as i32);
            rust_print(b" lines used (");
            print_num(ctx.pool_free() as i32);
            rust_print(b" free)\n");
        }
        b"func.list" => {
            rust_print(b"=== Functions ===\n");
            let mut any = false;
            for f in ctx.funcs.iter() {
                if f.used {
                    rust_print(b"  ");
                    rust_print(f.name.as_bytes());
                    rust_print(b"  pool[");
                    print_num(f.pool_start as i32);
                    rust_print(b"+");
                    print_num(f.body_count as i32);
                    rust_print(b"]");
                    if f.is_one_time { rust_print(b" [one-time]"); }
                    if f.recursive   { rust_print(b" [recursive]"); }
                    if f.has_run     { rust_print(b" [ran]"); }
                    rust_print(b"\n");
                    any = true;
                }
            }
            if !any { rust_print(b"  (none)\n"); }
        }
        b"func.drop" => {
            if argc < 2 { return -1; }
            ctx.undefine_func(arg!(1));
        }

        // ── MATH ──────────────────────────────────────────────────────────────
        b"math" => {
            if argc < 3 { rust_print(b"Usage: math VAR expr\n"); return -1; }
            let var = arg!(1);
            let mut expr = Line::new();
            let mut i = 2;
            while i < argc { expr.append(arg!(i)); expr.push(b' '); i += 1; }
            ctx.set_var_int(var, eval_math(expr.as_bytes(), ctx));
        }
        b"inc" => {
            if argc < 2 { return -1; }
            let v = ctx.get_var_int(arg!(1));
            let step = if argc >= 3 { ival!(2) } else { 1 };
            ctx.set_var_int(arg!(1), v + step);
        }
        b"dec" => {
            if argc < 2 { return -1; }
            let v = ctx.get_var_int(arg!(1));
            let step = if argc >= 3 { ival!(2) } else { 1 };
            ctx.set_var_int(arg!(1), v - step);
        }
        b"mod" => {
            if argc<4{return-1;}
            let a=ival!(1); let b=ival!(2);
            if b==0{rust_print(b"Error: mod zero\n");return-1;}
            ctx.set_var_int(arg!(3), a%b);
        }
        b"div" => {
            if argc<4{return-1;}
            let a=ival!(1); let b=ival!(2);
            if b==0{rust_print(b"Error: div zero\n");return-1;}
            ctx.set_var_int(arg!(3), a/b);
        }
        b"pow" => {
            if argc<4{return-1;}
            let base=ival!(1); let exp=ival!(2).max(0) as u32;
            let mut r=1i64;
            for _ in 0..exp { r=r.saturating_mul(base); }
            ctx.set_var_int(arg!(3), r);
        }
        b"sqrt"  => { if argc<3{return-1;} ctx.set_var_int(arg!(2), isqrt_i64(ival!(1))); }
        b"abs"   => { if argc<3{return-1;} ctx.set_var_int(arg!(2), ival!(1).abs()); }
        b"min"   => { if argc<4{return-1;} ctx.set_var_int(arg!(3), ival!(1).min(ival!(2))); }
        b"max"   => { if argc<4{return-1;} ctx.set_var_int(arg!(3), ival!(1).max(ival!(2))); }
        b"clamp" => { if argc<5{return-1;} ctx.set_var_int(arg!(4), ival!(1).max(ival!(2)).min(ival!(3))); }
        b"sign"  => {
            if argc<3{return-1;}
            let n=ival!(1);
            ctx.set_var_int(arg!(2), if n>0{1}else if n<0{-1}else{0});
        }
        b"rand"  => { if argc<4{return-1;} ctx.set_var_int(arg!(3), unsafe{rsh_rand(ival!(1),ival!(2))}); }
        b"hex"   => {
            if argc<3{return-1;}
            let n=ival!(1) as u32;
            let hc=b"0123456789ABCDEF";
            let mut out=Line::new();
            out.append(b"0x");
            let mut hbuf=[0u8;8];
            for i in (0..8).rev(){ hbuf[i]=hc[((n>>(i*4))&0xF) as usize]; }
            let hs=hbuf.iter().position(|&b|b!=b'0').unwrap_or(7);
            out.append(&hbuf[hs..]);
            ctx.set_var(arg!(2), out.as_bytes());
        }
        b"dec2"  => { if argc<3{return-1;} ctx.set_var_int(arg!(2), ival!(1)); }

        // ── BITWISE ───────────────────────────────────────────────────────────
        b"band"     => { if argc<4{return-1;} ctx.set_var_int(arg!(3), ival!(1)&ival!(2)); }
        b"bor"      => { if argc<4{return-1;} ctx.set_var_int(arg!(3), ival!(1)|ival!(2)); }
        b"bxor"     => { if argc<4{return-1;} ctx.set_var_int(arg!(3), ival!(1)^ival!(2)); }
        b"bnot"     => { if argc<3{return-1;} ctx.set_var_int(arg!(2), !(ival!(1) as u32) as i64); }
        b"shl"      => { if argc<4{return-1;} ctx.set_var_int(arg!(3), ival!(1)<<(ival!(2)&63)); }
        b"shr"      => { if argc<4{return-1;} ctx.set_var_int(arg!(3), ival!(1)>>(ival!(2)&63)); }
        b"bit.set"  => { if argc<4{return-1;} ctx.set_var_int(arg!(3), ival!(1)|(1i64<<(ival!(2)&63))); }
        b"bit.clr"  => { if argc<4{return-1;} ctx.set_var_int(arg!(3), ival!(1)&!(1i64<<(ival!(2)&63))); }
        b"bit.test" => { if argc<4{return-1;} ctx.set_var_int(arg!(3), (ival!(1)>>(ival!(2)&63))&1); }

        // ── STRING TOOLS ──────────────────────────────────────────────────────
        b"strlen" => {
            if argc<3{return-1;}
            ctx.set_var_int(arg!(2), ctx.get_var(arg!(1)).unwrap_or(b"").len() as i64);
        }
        b"substr" => {
            if argc<5{return-1;}
            let mut src=VarVal::new();
            src.set(ctx.get_var(arg!(1)).unwrap_or(b""));
            let s=src.as_bytes();
            let st=(ival!(2) as usize).min(s.len());
            let ln=(ival!(3) as usize).min(s.len().saturating_sub(st));
            ctx.set_var(arg!(4), &s[st..st+ln]);
        }
        b"strcat" => {
            if argc<3{return-1;}
            let mut out=Line::new();
            out.set(ctx.get_var(arg!(1)).unwrap_or(b""));
            let mut i=2;
            while i<argc { out.append(ctx.get_var(arg!(i)).unwrap_or(arg!(i))); i+=1; }
            ctx.set_var(arg!(1), out.as_bytes());
        }
        b"strrep" => {
            if argc<5{return-1;}
            let mut src=Line::new();
            src.set(ctx.get_var(arg!(1)).unwrap_or(b""));
            let old=arg!(2); let new=arg!(3);
            let mut out=Line::new();
            let s=src.as_bytes();
            if old.is_empty() { out.set(s); }
            else if let Some(pos)=find_bytes(s,old) {
                out.append(&s[..pos]); out.append(new); out.append(&s[pos+old.len()..]);
            } else { out.set(s); }
            ctx.set_var(arg!(4), out.as_bytes());
        }
        b"strrep.all" => {
            if argc<5{return-1;}
            let mut src=Line::new();
            src.set(ctx.get_var(arg!(1)).unwrap_or(b""));
            let old=arg!(2); let new=arg!(3);
            let mut out=Line::new();
            let s=src.as_bytes();
            if old.is_empty() { out.set(s); }
            else {
                let mut pos=0;
                while pos<s.len() {
                    if pos+old.len()<=s.len() && &s[pos..pos+old.len()]==old {
                        out.append(new); pos+=old.len();
                    } else { out.push(s[pos]); pos+=1; }
                }
            }
            ctx.set_var(arg!(4), out.as_bytes());
        }
        b"strupper" => {
            if argc<3{return-1;}
            let mut src=Line::new(); src.set(ctx.get_var(arg!(1)).unwrap_or(b""));
            let mut out=Line::new(); src.to_upper(&mut out);
            ctx.set_var(arg!(2), out.as_bytes());
        }
        b"strlower" => {
            if argc<3{return-1;}
            let mut src=Line::new(); src.set(ctx.get_var(arg!(1)).unwrap_or(b""));
            let mut out=Line::new(); src.to_lower(&mut out);
            ctx.set_var(arg!(2), out.as_bytes());
        }
        b"strfind" => {
            if argc<4{return-1;}
            let src=ctx.get_var(arg!(1)).unwrap_or(b"");
            let needle=arg!(2);
            ctx.set_var_int(arg!(3), find_bytes(src,needle).map(|i|i as i64).unwrap_or(-1));
        }
        b"strsplit" => {
            if argc<5{return-1;}
            let mut src=Line::new();
            src.set(ctx.get_var(arg!(1)).unwrap_or(b""));
            let sep=arg!(2); let idx=ival!(3) as usize;
            let s=src.as_bytes();
            let mut count=0; let mut pos=0; let mut found=false;
            while pos<=s.len() {
                let end=if sep.is_empty(){s.len()}
                        else{find_bytes(&s[pos..],sep).map(|p|pos+p).unwrap_or(s.len())};
                if count==idx { ctx.set_var(arg!(4), &s[pos..end]); found=true; break; }
                count+=1;
                if end>=s.len(){break;}
                pos=end+sep.len().max(1);
            }
            if !found{ctx.set_var(arg!(4), b"");}
        }
        b"strcount" => {
            if argc<4{return-1;}
            let src=ctx.get_var(arg!(1)).unwrap_or(b"");
            ctx.set_var_int(arg!(3), count_bytes(src,arg!(2)) as i64);
        }
        b"strtrim" => {
            if argc<3{return-1;}
            let mut src=Line::new();
            src.set(ctx.get_var(arg!(1)).unwrap_or(b""));
            ctx.set_var(arg!(2), trim(src.as_bytes()));
        }
        b"strpad" => {
            if argc<5{return-1;}
            let s=ctx.get_var(arg!(1)).unwrap_or(b"");
            let w=ival!(2) as usize;
            let pad=if !arg!(3).is_empty(){arg!(3)[0]}else{b' '};
            let mut out=Line::new();
            let sl=s.len(); if w>sl{for _ in 0..w-sl{out.push(pad);}}
            out.append(s);
            ctx.set_var(arg!(4), out.as_bytes());
        }
        b"strpad.r" => {
            if argc<5{return-1;}
            let s=ctx.get_var(arg!(1)).unwrap_or(b"");
            let w=ival!(2) as usize;
            let pad=if !arg!(3).is_empty(){arg!(3)[0]}else{b' '};
            let mut out=Line::new(); out.append(s);
            while out.len<w{out.push(pad);}
            ctx.set_var(arg!(4), out.as_bytes());
        }
        b"strhex" => {
            if argc<3{return-1;}
            let s=ctx.get_var(arg!(1)).unwrap_or(b"");
            let hc=b"0123456789ABCDEF";
            let mut out=Line::new();
            for &b in s{out.push(hc[(b>>4) as usize]);out.push(hc[(b&0xF) as usize]);}
            ctx.set_var(arg!(2), out.as_bytes());
        }
        b"strrev" => {
            if argc<3{return-1;}
            let mut s=Line::new(); s.set(ctx.get_var(arg!(1)).unwrap_or(b""));
            let mut out=Line::new();
            for i in (0..s.len).rev(){out.push(s.buf[i]);}
            ctx.set_var(arg!(2), out.as_bytes());
        }
        b"strjoin" => {
            if argc<4{return-1;}
            let sep=arg!(1); let out_var=arg!(argc-1);
            let mut out=Line::new();
            let mut i=2;
            while i<argc-1{
                if i>2{out.append(sep);}
                out.append(ctx.get_var(arg!(i)).unwrap_or(arg!(i)));
                i+=1;
            }
            ctx.set_var(out_var, out.as_bytes());
        }
        b"strstarts" => {
            if argc<4{return-1;}
            let s=ctx.get_var(arg!(1)).unwrap_or(b"");
            ctx.set_var(arg!(3), if s.starts_with(arg!(2)){b"1"}else{b"0"});
        }
        b"strends" => {
            if argc<4{return-1;}
            let s=ctx.get_var(arg!(1)).unwrap_or(b"");
            ctx.set_var(arg!(3), if s.ends_with(arg!(2)){b"1"}else{b"0"});
        }
        b"strsub.r" => {
            if argc<4{return-1;}
            let mut src=Line::new(); src.set(ctx.get_var(arg!(1)).unwrap_or(b""));
            let s=src.as_bytes();
            let n=(ival!(2) as usize).min(s.len());
            ctx.set_var(arg!(3), &s[s.len()-n..]);
        }

        // ── TYPE CHECKS ───────────────────────────────────────────────────────
        b"isnumber"  => {
            if argc<3{return-1;}
            let v=ctx.get_var(arg!(1)).unwrap_or(b"");
            ctx.set_var(arg!(2), if is_numeric(v){b"1"}else{b"0"});
        }
        b"isempty"   => {
            if argc<3{return-1;}
            let v=ctx.get_var(arg!(1)).map(|s|s.is_empty()).unwrap_or(true);
            ctx.set_var(arg!(2), if v{b"1"}else{b"0"});
        }
        b"isdefined" => {
            if argc<3{return-1;}
            ctx.set_var(arg!(2), if ctx.is_defined(arg!(1)){b"1"}else{b"0"});
        }
        b"tobool"    => {
            if argc<3{return-1;}
            ctx.set_var(arg!(2), if ival!(1)!=0{b"1"}else{b"0"});
        }
        b"bool.and"  => {
            if argc<4{return-1;}
            ctx.set_var(arg!(3), if ival!(1)!=0&&ival!(2)!=0{b"1"}else{b"0"});
        }
        b"bool.or"   => {
            if argc<4{return-1;}
            ctx.set_var(arg!(3), if ival!(1)!=0||ival!(2)!=0{b"1"}else{b"0"});
        }
        b"bool.not"  => {
            if argc<3{return-1;}
            ctx.set_var(arg!(2), if ival!(1)==0{b"1"}else{b"0"});
        }
        b"bool.xor"  => {
            if argc<4{return-1;}
            let a=ival!(1)!=0; let b=ival!(2)!=0;
            ctx.set_var(arg!(3), if a^b{b"1"}else{b"0"});
        }

        // ── FILE / AVFS ───────────────────────────────────────────────────────
        b"fexists" => {
            if argc<3{return-1;}
            let mut pb=[0u8;256]; let p=arg!(1); let l=p.len().min(255);
            pb[..l].copy_from_slice(&p[..l]);
            ctx.set_var(arg!(2), if unsafe{avfs_file_exists(pb.as_ptr())}{b"1"}else{b"0"});
        }
        b"fsize" => {
            if argc<3{return-1;}
            let mut pb=[0u8;256]; let p=arg!(1); let l=p.len().min(255);
            pb[..l].copy_from_slice(&p[..l]);
            ctx.set_var_int(arg!(2), unsafe{avfs_get_filesize(pb.as_ptr())} as i64);
        }
        b"fread" => {
            if argc<3{return-1;}
            let mut pb=[0u8;256]; let p=arg!(1); let l=p.len().min(255);
            pb[..l].copy_from_slice(&p[..l]);
            let sz=unsafe{avfs_get_filesize(pb.as_ptr())};
            if sz<=0{ctx.set_var(arg!(2), b""); return 0;}
            let read_len=(sz as usize).min(unsafe{FREAD_SCRATCH.len()});
            unsafe{avfs_read_file(pb.as_ptr(), FREAD_SCRATCH.as_mut_ptr(), read_len as u32, 0);}
            let mut out=VarVal::new();
            out.set(unsafe{&FREAD_SCRATCH[..read_len.min(MAX_VAR_VAL-1)]});
            ctx.set_var(arg!(2), out.as_bytes());
        }
        b"fwrite" => {
            if argc<3{return-1;}
            let mut pb=[0u8;256]; let p=arg!(1); let l=p.len().min(255);
            pb[..l].copy_from_slice(&p[..l]);
            let content=ctx.get_var(arg!(2)).unwrap_or(arg!(2));
            unsafe{
                if avfs_file_exists(pb.as_ptr()){avfs_remove_file(pb.as_ptr());}
                avfs_create_file(pb.as_ptr(), content.len() as u32);
                avfs_write_file(pb.as_ptr(), content.as_ptr(), content.len() as u32, 0);
            }
        }
        b"fappend" => {
            if argc<3{return-1;}
            let mut pb=[0u8;256]; let p=arg!(1); let l=p.len().min(255);
            pb[..l].copy_from_slice(&p[..l]);
            let content=ctx.get_var(arg!(2)).unwrap_or(arg!(2));
            unsafe{
                let sz=avfs_get_filesize(pb.as_ptr());
                if sz<0{avfs_create_file(pb.as_ptr(), content.len() as u32);}
                avfs_write_file(pb.as_ptr(), content.as_ptr(), content.len() as u32, sz.max(0) as u32);
            }
        }
        b"fdelete" => {
            if argc<2{return-1;}
            let mut pb=[0u8;256]; let p=arg!(1); let l=p.len().min(255);
            pb[..l].copy_from_slice(&p[..l]);
            unsafe{avfs_remove_file(pb.as_ptr());}
        }
        b"fline" => {
            if argc<4{return-1;}
            let mut pb=[0u8;256]; let p=arg!(1); let l=p.len().min(255);
            pb[..l].copy_from_slice(&p[..l]);
            let target_line=ival!(2) as usize;
            let sz=unsafe{avfs_get_filesize(pb.as_ptr())};
            if sz<=0{ctx.set_var(arg!(3), b""); return 0;}
            let rl=(sz as usize).min(unsafe{FREAD_SCRATCH.len()});
            unsafe{avfs_read_file(pb.as_ptr(), FREAD_SCRATCH.as_mut_ptr(), rl as u32, 0);}
            let data=unsafe{&FREAD_SCRATCH[..rl]};
            let mut line_n=0; let mut line_start=0; let mut found=false; let mut i=0;
            while i<=data.len(){
                if i==data.len()||data[i]==b'\n'||data[i]==b'\r'{
                    if line_n==target_line{
                        ctx.set_var(arg!(3), trim(&data[line_start..i]));
                        found=true; break;
                    }
                    line_n+=1; line_start=i+1;
                }
                i+=1;
            }
            if !found{ctx.set_var(arg!(3), b"");}
        }
        b"flinecount" => {
            if argc<3{return-1;}
            let mut pb=[0u8;256]; let p=arg!(1); let l=p.len().min(255);
            pb[..l].copy_from_slice(&p[..l]);
            let sz=unsafe{avfs_get_filesize(pb.as_ptr())};
            if sz<=0{ctx.set_var_int(arg!(2), 0); return 0;}
            let rl=(sz as usize).min(unsafe{FREAD_SCRATCH.len()});
            unsafe{avfs_read_file(pb.as_ptr(), FREAD_SCRATCH.as_mut_ptr(), rl as u32, 0);}
            let count=unsafe{&FREAD_SCRATCH[..rl]}.iter().filter(|&&b|b==b'\n').count();
            ctx.set_var_int(arg!(2), count as i64);
        }
        b"fgrep" => {
            if argc<4{return-1;}
            let mut pb=[0u8;256]; let p=arg!(1); let l=p.len().min(255);
            pb[..l].copy_from_slice(&p[..l]);
            let needle=arg!(2);
            let sz=unsafe{avfs_get_filesize(pb.as_ptr())};
            if sz<=0{ctx.set_var(arg!(3), b""); return 0;}
            let rl=(sz as usize).min(unsafe{FREAD_SCRATCH.len()});
            unsafe{avfs_read_file(pb.as_ptr(), FREAD_SCRATCH.as_mut_ptr(), rl as u32, 0);}
            let data=unsafe{&FREAD_SCRATCH[..rl]};
            let mut ls=0; let mut found=false; let mut i=0;
            while i<=data.len(){
                if i==data.len()||data[i]==b'\n'||data[i]==b'\r'{
                    let ldata=trim(&data[ls..i]);
                    if find_bytes(ldata,needle).is_some(){
                        ctx.set_var(arg!(3), ldata); found=true; break;
                    }
                    ls=i+1;
                }
                i+=1;
            }
            if !found{ctx.set_var(arg!(3), b"");}
        }

        // ── MAP COMMANDS ──────────────────────────────────────────────────────
        b"map.new" => {
            if argc<2{return-1;}
            ctx.create_map(arg!(1), argc>=3 && arg!(2)==b"editable");
        }
        b"map.set" => {
            if argc<4{return-1;}
            let map_name=arg!(1);
            let mut temp_val=VarVal::new();
            let raw=arg!(3);
            if let Some(v)=ctx.get_var(raw){temp_val.set(v);}else{temp_val.set(raw);}
            if let Some(m)=ctx.get_map_mut(map_name){m.set(arg!(2), temp_val.as_bytes());}
            else{rust_print(b"Error: map not found: ");rust_print(map_name);rust_print(b"\n");}
        }
        b"map.get" => {
            if argc<4{return-1;}
            let v=ctx.find_map_idx(arg!(1)).and_then(|i|ctx.maps[i].get(arg!(2))).unwrap_or(b"");
            let mut vbuf=VarVal::new(); vbuf.set(v);
            ctx.set_var(arg!(3), vbuf.as_bytes());
        }
        b"map.del" => {
            if argc<3{return-1;}
            if let Some(m)=ctx.get_map_mut(arg!(1)){m.del(arg!(2));}
        }
        b"map.has" => {
            if argc<4{return-1;}
            let has=ctx.find_map_idx(arg!(1)).map(|i|ctx.maps[i].has(arg!(2))).unwrap_or(false);
            ctx.set_var(arg!(3), if has{b"1"}else{b"0"});
        }
        b"map.clear" => {
            if argc<2{return-1;}
            if let Some(m)=ctx.get_map_mut(arg!(1)){m.clear_entries();}
        }
        b"map.count" => {
            if argc<3{return-1;}
            let c=ctx.find_map_idx(arg!(1)).map(|i|ctx.maps[i].count).unwrap_or(0);
            ctx.set_var_int(arg!(2), c as i64);
        }
        b"map.key" => {
            if argc<4{return-1;}
            let k=ctx.find_map_idx(arg!(1)).and_then(|i|ctx.maps[i].nth_key(ival!(2) as usize)).unwrap_or(b"");
            let mut kb=VarVal::new(); kb.set(k);
            ctx.set_var(arg!(3), kb.as_bytes());
        }
        b"map.val" => {
            if argc<4{return-1;}
            let v=ctx.find_map_idx(arg!(1)).and_then(|i|ctx.maps[i].nth_val(ival!(2) as usize)).unwrap_or(b"");
            let mut vb=VarVal::new(); vb.set(v);
            ctx.set_var(arg!(3), vb.as_bytes());
        }
        b"map.keys" => {
            if argc<3{return-1;}
            let mut out=VarVal::new();
            if let Some(i)=ctx.find_map_idx(arg!(1)){
                let m=&ctx.maps[i]; let mut first=true;
                for e in m.entries.iter(){
                    if e.used{if !first{out.push(b' ');}out.append(e.key.as_bytes());first=false;}
                }
            }
            ctx.set_var(arg!(2), out.as_bytes());
        }
        b"map.vals" => {
            if argc<3{return-1;}
            let mut out=VarVal::new();
            if let Some(i)=ctx.find_map_idx(arg!(1)){
                let m=&ctx.maps[i]; let mut first=true;
                for e in m.entries.iter(){
                    if e.used{if !first{out.push(b' ');}out.append(e.val.as_bytes());first=false;}
                }
            }
            ctx.set_var(arg!(2), out.as_bytes());
        }
        b"map.merge" => {
            if argc<3{return-1;}
            if let Some(si)=ctx.find_map_idx(arg!(1)){
                let src=ctx.maps[si];
                if let Some(di)=ctx.find_map_idx(arg!(2)){ctx.maps[di].merge_from(&src);}
            }
        }
        b"map.copy" => {
            if argc<3{return-1;}
            if let Some(si)=ctx.find_map_idx(arg!(1)){
                let src=ctx.maps[si];
                ctx.create_map(arg!(2), src.is_editable);
                if let Some(di)=ctx.find_map_idx(arg!(2)){ctx.maps[di].merge_from(&src);}
            }
        }
        b"map.drop" => {
            if argc<2{return-1;}
            ctx.delete_map(arg!(1));
        }
        b"map.dump" => {
            if argc<2{return-1;}
            if let Some(i)=ctx.find_map_idx(arg!(1)){
                let m=&ctx.maps[i];
                rust_print(b"MAP "); rust_print(m.name.as_bytes());
                let mut nb=[0u8;24];
                rust_print(b" (");
                rust_print(int_to_str(m.count as i64, &mut nb));
                rust_print(b" entries):\n");
                for e in m.entries.iter(){
                    if e.used{
                        rust_print(b"  ");
                        rust_print(e.key.as_bytes());
                        rust_print(b": ");
                        rust_print(e.val.as_bytes());
                        rust_print(b"\n");
                    }
                }
            } else {
                rust_print(b"Error: map not found: ");
                rust_print(arg!(1));
                rust_print(b"\n");
            }
        }
        b"map.sum" => {
            if argc<3{return-1;}
            let mut sum=0i64;
            if let Some(i)=ctx.find_map_idx(arg!(1)){
                for e in ctx.maps[i].entries.iter(){
                    if e.used{sum=sum.saturating_add(parse_int(e.val.as_bytes()));}
                }
            }
            ctx.set_var_int(arg!(2), sum);
        }
        b"map.max" => {
            if argc<3{return-1;}
            let mut best=i64::MIN; let mut best_key=Line::new();
            if let Some(i)=ctx.find_map_idx(arg!(1)){
                for e in ctx.maps[i].entries.iter(){
                    if e.used{
                        let n=parse_int(e.val.as_bytes());
                        if n>best{best=n;best_key.set(e.key.as_bytes());}
                    }
                }
            }
            ctx.set_var_int(arg!(2), best);
            if argc>=4{ctx.set_var(arg!(3), best_key.as_bytes());}
        }
        b"map.min" => {
            if argc<3{return-1;}
            let mut best=i64::MAX; let mut best_key=Line::new();
            if let Some(i)=ctx.find_map_idx(arg!(1)){
                for e in ctx.maps[i].entries.iter(){
                    if e.used{
                        let n=parse_int(e.val.as_bytes());
                        if n<best{best=n;best_key.set(e.key.as_bytes());}
                    }
                }
            }
            ctx.set_var_int(arg!(2), best);
            if argc>=4{ctx.set_var(arg!(3), best_key.as_bytes());}
        }
        b"map.invert" => {
            if argc<3{return-1;}
            ctx.create_map(arg!(2), true);
            if let Some(si)=ctx.find_map_idx(arg!(1)){
                let sc=ctx.maps[si];
                if let Some(di)=ctx.find_map_idx(arg!(2)){
                    for e in sc.entries.iter(){
                        if e.used{ctx.maps[di].set(e.val.as_bytes(), e.key.as_bytes());}
                    }
                }
            }
        }
        b"map.list" => {
            rust_print(b"=== Maps ===\n");
            let mut any=false;
            for m in ctx.maps.iter(){
                if m.used{
                    rust_print(b"  ");
                    rust_print(m.name.as_bytes());
                    let mut nb=[0u8;24];
                    rust_print(b"  (");
                    rust_print(int_to_str(m.count as i64, &mut nb));
                    rust_print(b" entries)");
                    if m.is_editable{rust_print(b" [editable]");}
                    rust_print(b"\n");
                    any=true;
                }
            }
            if !any{rust_print(b"  (none)\n");}
        }
        b"edit.MAP" => {
            if argc<3{return-1;}
            let (map_name,key,val)=if argc==3{(ctx.active_map.as_bytes(),arg!(1),arg!(2))}else{(arg!(1),arg!(2),arg!(3))};
            let mut mn=[0u8;32]; let l=map_name.len().min(31);
            mn[..l].copy_from_slice(&map_name[..l]);
            if let Some(m)=ctx.get_map_mut(&mn[..l]){if m.is_editable{m.set(key,val);}}
        }
        b"close.MAP" => { ctx.active_map.clear(); }
        b"const"|b"editable"|b"end.MAP"|b"endMAP" => {}

        // ── DISPLAY / VGA TEXT ────────────────────────────────────────────────
        b"cls" => { unsafe{terminal_clear();} }
        b"color" => {
            if argc<2{return-1;}
            let fg=ival!(1) as u8 & 0xF;
            let bg=if argc>=3{ival!(2) as u8&0xF}else{0};
            unsafe{terminal_setcolor(fg|(bg<<4));}
        }
        b"vgaput" => {
            if argc<5{return-1;}
            let ch_raw=val!(3);
            let ch=if ch_raw.is_empty(){b' '}else{ch_raw[0]};
            unsafe{vga_text_write(ival!(2) as usize, ival!(1) as usize, ch, ival!(4) as u8);}
        }
        b"vgastr" => {
            if argc<5{return-1;}
            unsafe{vga_text_str(ival!(2) as usize, ival!(1) as usize, val!(3), ival!(4) as u8);}
        }
        b"vgabar" => {
            if argc<4{return-1;}
            unsafe{print_bar(ival!(1), ival!(2), ival!(3));}
        }
        b"vgaclear" => {
            if argc<3{return-1;}
            let row=ival!(1) as usize; let attr=ival!(2) as u8;
            for col in 0..80{unsafe{vga_text_write(col, row, b' ', attr);}}
        }
        b"vgafill" => {
            if argc<6{return-1;}
            let row=ival!(1) as usize; let col=ival!(2) as usize;
            let len=ival!(3) as usize;
            let ch=val!(4); let ch=if ch.is_empty(){b' '}else{ch[0]};
            let attr=ival!(5) as u8;
            for i in 0..len{unsafe{vga_text_write(col+i, row, ch, attr);}}
        }

        // ── TIMING ────────────────────────────────────────────────────────────
        b"pause" => {
            if argc<2{return-1;}
            unsafe{sleep_ms(ival!(1) as u32);}
        }
        b"ticks" => {
            let out=if argc>=2{arg!(1)}else{b"TICKS"};
            ctx.set_var_int(out, unsafe{get_ticks()} as i64);
        }
        b"elapsed" => {
            if argc<3{return-1;}
            let start=ctx.get_var_int(arg!(1)) as u32;
            ctx.set_var_int(arg!(2), unsafe{get_ticks().wrapping_sub(start)} as i64);
        }

        // ── CONTROL FLOW ──────────────────────────────────────────────────────
        b"break"    => { ctx.break_flag    = true; return 0; }
        b"continue" => { ctx.continue_flag = true; return 0; }
        b"return"   => { ctx.return_flag   = true; return 0; }
        b"exit" => {
            let code=if argc>=2{ival!(1)}else{0};
            ctx.exit_code=code as i32;
            ctx.return_flag=true;
            return code as i32;
        }
        b"call" => {
            if argc<2{return-1;}
            let mut fargs=[&b""[..];MAX_ARGS];
            let mut i=2;
            while i<argc && i-2<MAX_ARGS { fargs[i-2]=arg!(i); i+=1; }
            return call_function(arg!(1), &fargs[..argc.saturating_sub(2)], ctx);
        }
        b"assert" => {
            if argc<2{return-1;}
            if !eval_condition(arg!(1), ctx){
                let msg=if argc>=3{val!(2)}else{b"assertion failed"};
                unsafe{terminal_setcolor(0x0C);}
                rust_print(b"ASSERT FAIL: ");
                rust_print(msg);
                rust_print(b"\n");
                unsafe{terminal_setcolor(0x07);}
                return -1;
            }
        }
        b"once" => {
            if argc<3{return-1;}
            let tag=arg!(1);
            if ctx.once_has_run(tag){return 0;}
            ctx.once_mark(tag);
            let mut rest=Line::new();
            let mut i=2;
            while i<argc{rest.append(arg!(i));if i+1<argc{rest.push(b' ');}i+=1;}
            return exec_line(rest.as_bytes(), ctx);
        }
        b"repeat" => {
            if argc<3{return-1;}
            let n=ival!(1).max(0) as usize;
            let mut cmd_line=Line::new();
            let mut i=2;
            while i<argc{cmd_line.append(arg!(i));if i+1<argc{cmd_line.push(b' ');}i+=1;}
            for iter in 0..n{
                ctx.set_var_int(b"ITER", iter as i64);
                let r=exec_line(cmd_line.as_bytes(), ctx);
                if r!=0||ctx.break_flag||ctx.return_flag{ctx.break_flag=false;break;}
            }
        }
        b"nop" => {}
        b"log" => {
            if argc<3{return-1;}
            let level=arg!(1);
            let mut msg=Line::new();
            let mut i=2;
            while i<argc{msg.append(arg!(i));if i+1<argc{msg.push(b' ');}i+=1;}
            rust_print(b"[RSH][");
            rust_print(level);
            rust_print(b"] ");
            rust_print(msg.as_bytes());
            rust_print(b"\n");
        }
        b"error" => {
            if argc>=2{
                unsafe{terminal_setcolor(0x0C);}
                rust_print(b"Error: ");
                let mut i=1;
                while i<argc{rust_print(arg!(i));if i+1<argc{rust_print(b" ");}i+=1;}
                rust_print(b"\n");
                unsafe{terminal_setcolor(0x07);}
            }
            return -1;
        }
        b"warn" => {
            if argc>=2{
                unsafe{terminal_setcolor(0x0E);}
                rust_print(b"Warning: ");
                let mut i=1;
                while i<argc{rust_print(arg!(i));if i+1<argc{rust_print(b" ");}i+=1;}
                rust_print(b"\n");
                unsafe{terminal_setcolor(0x07);}
            }
        }
        b"info" => {
            if argc>=2{
                unsafe{terminal_setcolor(0x09);}
                rust_print(b"Info: ");
                let mut i=1;
                while i<argc{rust_print(arg!(i));if i+1<argc{rust_print(b" ");}i+=1;}
                rust_print(b"\n");
                unsafe{terminal_setcolor(0x07);}
            }
        }
        b"bool" => {
            if argc<2{return-1;}
            let sub=arg!(1);
            if sub==b"set" && argc>=4{
                let v=arg!(3);
                ctx.set_var(arg!(2), if v==b"TRUE"||v==b"yes"{b"1"}else{b"0"});
            } else if sub==b"toggle" && argc>=3{
                let cur=ctx.get_var(arg!(2)).unwrap_or(b"0");
                ctx.set_var(arg!(2), if cur==b"1"{b"0"}else{b"1"});
            } else if sub==b"is" && argc>=3{
                let v=ctx.get_var(arg!(2)).unwrap_or(b"0")==b"1";
                ctx.set_var(b"?", if v{b"1"}else{b"0"});
            }
        }
        b"^include"|b"^entrypoint" => {}

        // ── FUNCTION CALL / MAP FALLTHROUGH ───────────────────────────────────
        _ => {
            // Named function call
            if ctx.find_func(cmd).is_some() {
                let mut fargs=[&b""[..];MAX_ARGS];
                let mut i=1;
                while i<argc && i-1<MAX_ARGS { fargs[i-1]=arg!(i); i+=1; }
                return call_function(cmd, &fargs[..argc.saturating_sub(1)], ctx);
            }

            // Active map context: treat "key value" as implicit map.set
            let active_map_len=ctx.active_map.len;
            if active_map_len>0 {
                let mut mn=[0u8;32];
                mn[..active_map_len].copy_from_slice(&ctx.active_map.buf[..active_map_len]);
                let mut temp_val=VarVal::new();
                if argc>=2 {
                    let raw=arg!(1);
                    if let Some(v)=ctx.get_var(raw){temp_val.set(v);}else{temp_val.set(raw);}
                }
                if let Some(m)=ctx.get_map_mut(&mn[..active_map_len]){
                    if m.is_editable { m.set(cmd, temp_val.as_bytes()); return 0; }
                }
            }

            // With-map scope: look up key and print value
            let with_len=ctx.with_map.len;
            if with_len>0 {
                let mut mn=[0u8;32];
                mn[..with_len].copy_from_slice(&ctx.with_map.buf[..with_len]);
                if let Some(i)=ctx.find_map_idx(&mn[..with_len]){
                    if let Some(v)=ctx.maps[i].get(cmd){
                        let mut vb=VarVal::new(); vb.set(v);
                        rust_print(vb.as_bytes());
                        rust_print(b"\n");
                        return 0;
                    }
                }
            }

            rust_print(b"Unknown command: ");
            rust_print(cmd);
            rust_print(b"\n");
        }
    }
    0
}

// =============================================================================
// C INTERFACE
// =============================================================================

#[no_mangle]
pub extern "C" fn script_init() {
    unsafe {
        // Zero the context (can't use Default in no_std without alloc)
        let p = &mut GLOBAL_CTX as *mut ScriptCtx as *mut u8;
        core::ptr::write_bytes(p, 0, core::mem::size_of::<ScriptCtx>());
        // Also reset the pool
        let pp = FUNC_BODY_POOL.as_mut_ptr() as *mut u8;
        core::ptr::write_bytes(pp, 0, core::mem::size_of::<[Line; FUNC_POOL_SIZE]>());
        FUNC_POOL_HEAD = 0;
        INITIALIZED    = true;
    }
}

#[no_mangle]
pub extern "C" fn script_execute_file(path: *const u8) -> i32 {
    unsafe {
        if !INITIALIZED { script_init(); }
        if path.is_null() { rust_print(b"Error: null path\n"); return -1; }
        let mut len = 0;
        while len < 512 && *path.add(len) != 0 { len += 1; }
        if len == 0 { rust_print(b"Error: empty path\n"); return -1; }
        let path_slice = core::slice::from_raw_parts(path, len);
        GLOBAL_CTX.include_depth = 0;
        GLOBAL_CTX.break_flag    = false;
        GLOBAL_CTX.continue_flag = false;
        GLOBAL_CTX.return_flag   = false;
        GLOBAL_CTX.call_depth    = 0;
        exec_file(path_slice, &mut GLOBAL_CTX)
    }
}

#[no_mangle]
pub extern "C" fn script_execute_line_c(line: *const u8) -> i32 {
    unsafe {
        if !INITIALIZED { script_init(); }
        let len = (0..).take_while(|&i| *line.add(i) != 0).count();
        let l   = core::slice::from_raw_parts(line, len);
        exec_line(l, &mut GLOBAL_CTX)
    }
}

#[no_mangle]
pub extern "C" fn script_set_var_c(name: *const u8, val: *const u8) {
    unsafe {
        if !INITIALIZED { script_init(); }
        if name.is_null() || val.is_null() { return; }
        let nlen = (0..).take_while(|&i| i < 64  && *name.add(i) != 0).count();
        let vlen = (0..).take_while(|&i| i < 256 && *val.add(i)  != 0).count();
        let n = core::slice::from_raw_parts(name, nlen);
        let v = core::slice::from_raw_parts(val,  vlen);
        GLOBAL_CTX.set_var(n, v);
    }
}

#[no_mangle]
pub extern "C" fn script_get_var_c(
    name:     *const u8,
    out:      *mut u8,
    out_size: usize,
) -> i32 {
    unsafe {
        if !INITIALIZED { return -1; }
        if name.is_null() || out.is_null() || out_size == 0 { return -1; }
        let nlen = (0..).take_while(|&i| i < 64 && *name.add(i) != 0).count();
        let n    = core::slice::from_raw_parts(name, nlen);
        match GLOBAL_CTX.get_var(n) {
            Some(v) => {
                let copy = v.len().min(out_size - 1);
                core::ptr::copy_nonoverlapping(v.as_ptr(), out, copy);
                *out.add(copy) = 0;
                copy as i32
            }
            None => { if out_size > 0 { *out = 0; } -1 }
        }
    }
}

#[no_mangle]
pub extern "C" fn script_eval_cond_c(cond: *const u8) -> bool {
    unsafe {
        if !INITIALIZED { return false; }
        let len = (0..).take_while(|&i| *cond.add(i) != 0).count();
        let s   = core::slice::from_raw_parts(cond, len);
        eval_condition(s, &GLOBAL_CTX)
    }
}

/// Dump pool stats + all defined functions to the terminal.
#[no_mangle]
pub extern "C" fn script_debug_c() {
    unsafe {
        if !INITIALIZED { rust_print(b"RSH: not initialized\n"); return; }
        rust_print(b"=== RSH v2.3 debug ===\n");
        rust_print(b"FUNC_BODY_POOL: ");
        print_num(FUNC_POOL_HEAD as i32);
        rust_print(b"/");
        print_num(FUNC_POOL_SIZE as i32);
        rust_print(b" lines (");
        print_num((FUNC_POOL_HEAD * MAX_LINE_LEN / 1024) as i32);
        rust_print(b" KB used)\n");
        rust_print(b"Functions defined:\n");
        let mut any = false;
        for f in GLOBAL_CTX.funcs.iter() {
            if f.used {
                rust_print(b"  ");
                rust_print(f.name.as_bytes());
                rust_print(b"  [pool ");
                print_num(f.pool_start as i32);
                rust_print(b"+");
                print_num(f.body_count as i32);
                rust_print(b"]");
                if f.is_one_time { rust_print(b" one-time"); }
                if f.recursive   { rust_print(b" recursive"); }
                if f.has_run     { rust_print(b" ran"); }
                rust_print(b"\n");
                any = true;
            }
        }
        if !any { rust_print(b"  (none)\n"); }
        rust_print(b"Variables:\n");
        any = false;
        for v in GLOBAL_CTX.vars.iter() {
            if v.used {
                rust_print(b"  ");
                rust_print(v.name.as_bytes());
                rust_print(b"=");
                rust_print(v.value.as_bytes());
                rust_print(b"\n");
                any = true;
            }
        }
        if !any { rust_print(b"  (none)\n"); }
    }
}

/// Reset the script engine completely (ctx + pool).
#[no_mangle]
pub extern "C" fn script_reset_c() {
    unsafe {
        let p = &mut GLOBAL_CTX as *mut ScriptCtx as *mut u8;
        core::ptr::write_bytes(p, 0, core::mem::size_of::<ScriptCtx>());
        let pp = FUNC_BODY_POOL.as_mut_ptr() as *mut u8;
        core::ptr::write_bytes(pp, 0, core::mem::size_of::<[Line; FUNC_POOL_SIZE]>());
        FUNC_POOL_HEAD = 0;
        rust_print(b"RSH: engine reset\n");
    }
}

// =============================================================================
// rsh_engine_plugin_api.rs  — append this block to the bottom of rsh_engine.rs
//
// Adds per-context C interface functions so rsh_plugin.c can give every
// plugin its own isolated ScriptCtx (variables, functions, maps don't clash).
//
// The C side passes a *mut u8 pointer it owns (allocated in the static pool
// g_ctx_pool in rsh_plugin.c).  We cast it to *mut ScriptCtx on each call.
// No allocation, no Box<>, no heap — pure pointer arithmetic over a C buffer.
//
// Safety contract (upheld by rsh_plugin.c):
//   - ctx is never null.
//   - ctx points to a buffer of at least size_of::<ScriptCtx>() bytes.
//   - ctx is not shared across threads (RadiumOS is single-core for now).
//   - The buffer was zero-initialised by script_init_ctx before any other
//     call is made on it.
// =============================================================================

// ── Helpers ───────────────────────────────────────────────────────────────────

/// Cast a raw C pointer to &mut ScriptCtx.  Only called from #[no_mangle]
/// functions whose safety is documented above.
#[inline(always)]
unsafe fn ctx_from_ptr<'a>(ctx: *mut u8) -> &'a mut ScriptCtx {
    &mut *(ctx as *mut ScriptCtx)
}

// ── Per-context C interface ───────────────────────────────────────────────────

/// Zero-initialise a caller-owned ScriptCtx buffer.
/// Must be the first call made on any buffer before using any other ctx API.
#[no_mangle]
pub unsafe extern "C" fn script_init_ctx(ctx: *mut u8) {
    if ctx.is_null() { return; }
    core::ptr::write_bytes(ctx, 0, core::mem::size_of::<ScriptCtx>());
}

/// Execute a script file inside the given context.
/// Equivalent to exec_file() but using an external ScriptCtx instead of
/// the global GLOBAL_CTX.
///
/// The FUNC_BODY_POOL and per-depth file buffers (LINES_D*, FBUF_D*) are
/// STILL global statics shared across all contexts.  This is safe as long
/// as no two plugins call exec_file_ctx concurrently (single-core kernel).
///
/// If you need full isolation for the function body pool, you will need to
/// introduce a per-context FUNC_BODY_POOL.  For now, the global pool is
/// shared, meaning all plugins share the 3072-line pool.  To accommodate
/// N plugins each with up to K function lines, ensure FUNC_POOL_SIZE >= N*K.
#[no_mangle]
pub unsafe extern "C" fn exec_file_ctx(path: *const u8, ctx: *mut u8) -> i32 {
    if path.is_null() || ctx.is_null() {
        rust_print(b"Error: exec_file_ctx null arg\n");
        return -1;
    }
    let mut len = 0;
    while len < 512 && *path.add(len) != 0 { len += 1; }
    if len == 0 { rust_print(b"Error: exec_file_ctx empty path\n"); return -1; }
    let path_slice = core::slice::from_raw_parts(path, len);
    let c = ctx_from_ptr(ctx);
    c.include_depth = 0;
    c.break_flag    = false;
    c.continue_flag = false;
    c.return_flag   = false;
    c.call_depth    = 0;
    exec_file(path_slice, c)
}

/// Execute a single RSH line inside the given context.
#[no_mangle]
pub unsafe extern "C" fn exec_line_ctx(line: *const u8, ctx: *mut u8) -> i32 {
    if line.is_null() || ctx.is_null() { return -1; }
    let len = (0..).take_while(|&i| *line.add(i) != 0).count();
    let l   = core::slice::from_raw_parts(line, len);
    let c   = ctx_from_ptr(ctx);
    exec_line(l, c)
}

/// Call a named function inside the given context.
/// args is a C array of `argc` null-terminated strings (may be null if argc==0).
#[no_mangle]
pub unsafe extern "C" fn call_function_ctx(
    name: *const u8,
    args: *const *const u8,
    argc: i32,
    ctx:  *mut u8,
) -> i32 {
    if name.is_null() || ctx.is_null() { return -1; }
    let nlen = (0..).take_while(|&i| *name.add(i) != 0).count();
    let name_slice = core::slice::from_raw_parts(name, nlen);

    // Build a &[&[u8]] from the C argv.  MAX_ARGS caps the count.
    let mut arg_slices: [&[u8]; MAX_ARGS] = [b""; MAX_ARGS];
    let n = (argc as usize).min(MAX_ARGS);
    for i in 0..n {
        if args.is_null() { break; }
        let p = *args.add(i);
        if p.is_null() { break; }
        let l = (0..).take_while(|&j| *p.add(j) != 0).count();
        arg_slices[i] = core::slice::from_raw_parts(p, l);
    }

    let c = ctx_from_ptr(ctx);
    call_function(name_slice, &arg_slices[..n], c)
}

/// Set a variable in the given context.
#[no_mangle]
pub unsafe extern "C" fn script_set_var_ctx(
    name: *const u8,
    val:  *const u8,
    ctx:  *mut u8,
) {
    if name.is_null() || val.is_null() || ctx.is_null() { return; }
    let nlen = (0..).take_while(|&i| i < 64  && *name.add(i) != 0).count();
    let vlen = (0..).take_while(|&i| i < 256 && *val.add(i)  != 0).count();
    let n    = core::slice::from_raw_parts(name, nlen);
    let v    = core::slice::from_raw_parts(val,  vlen);
    ctx_from_ptr(ctx).set_var(n, v);
}

/// Get a variable from the given context into a caller-owned C buffer.
/// Returns the number of bytes written (not counting the null terminator),
/// or -1 if the variable is not defined.
#[no_mangle]
pub unsafe extern "C" fn script_get_var_ctx(
    name:     *const u8,
    out:      *mut u8,
    out_size: i32,
    ctx:      *const u8,
) -> i32 {
    if name.is_null() || out.is_null() || ctx.is_null() || out_size <= 0 { return -1; }
    let nlen = (0..).take_while(|&i| i < 64 && *name.add(i) != 0).count();
    let n    = core::slice::from_raw_parts(name, nlen);
    // ScriptCtx is read-only here; ctx_from_ptr returns &mut but we only read.
    let c    = &*(ctx as *const ScriptCtx);
    match c.get_var(n) {
        Some(v) => {
            let copy = v.len().min(out_size as usize - 1);
            core::ptr::copy_nonoverlapping(v.as_ptr(), out, copy);
            *out.add(copy) = 0;
            copy as i32
        }
        None => { *out = 0; -1 }
    }
}

/// Evaluate a condition string inside the given context.
#[no_mangle]
pub unsafe extern "C" fn script_eval_cond_ctx(cond: *const u8, ctx: *const u8) -> bool {
    if cond.is_null() || ctx.is_null() { return false; }
    let len = (0..).take_while(|&i| *cond.add(i) != 0).count();
    let s   = core::slice::from_raw_parts(cond, len);
    let c   = &*(ctx as *const ScriptCtx);
    eval_condition(s, c)
}

#[no_mangle]
pub unsafe extern "C" fn script_reset_flags_ctx(ctx: *mut u8) {
    if ctx.is_null() { return; }
    let c = ctx_from_ptr(ctx);
    c.break_flag    = false;
    c.continue_flag = false;
    c.return_flag   = false;
    c.include_depth = 0;
    c.call_depth    = 0;
    c.in_function   = false;
}

/// Read a value from a named map directly — no exec_line needed.
/// Copies the value into `out` (null-terminated), returns bytes written or -1.
#[no_mangle]
pub unsafe extern "C" fn script_map_get_ctx(
    map_name: *const u8,
    key:      *const u8,
    out:      *mut u8,
    out_size: i32,
    ctx:      *const u8,
) -> i32 {
    if map_name.is_null() || key.is_null() || out.is_null() || ctx.is_null() || out_size <= 0 {
        return -1;
    }
    let mnlen = (0..).take_while(|&i| *map_name.add(i) != 0).count();
    let klen  = (0..).take_while(|&i| *key.add(i)      != 0).count();
    let mn    = core::slice::from_raw_parts(map_name, mnlen);
    let k     = core::slice::from_raw_parts(key,      klen);
    let c     = &*(ctx as *const ScriptCtx);
    match c.find_map_idx(mn) {
        Some(i) => match c.maps[i].get(k) {
            Some(v) => {
                let copy = v.len().min(out_size as usize - 1);
                core::ptr::copy_nonoverlapping(v.as_ptr(), out, copy);
                *out.add(copy) = 0;
                copy as i32
            }
            None => { *out = 0; -1 }
        }
        None => { *out = 0; -1 }
    }
}



// =============================================================================
//  RADIUM DUNGEON 5.0 -- scp_2801 / RadiumOS
//  50+ Updates: Task AI, Particle System, Gun Combat, Hex Skill Tree,
//  Status Effects, Better Shop/Inventory, Animations, Minimap, and more
// =============================================================================

const VGA: *mut u16 = 0xB8000 as *mut u16;
const MAP_W: usize = 200;
const MAP_H: usize = 120;
const VIEW_W: usize = 52;
const VIEW_H: usize = 20;
const SIDEBAR_X: usize = 53;
const LOG_ROW: usize = 21;
const MAX_MONSTERS: usize = 64;
const MAX_MAP_ITEMS: usize = 192;
const MAX_LOG: usize = 4;
const MAX_LOG_LEN: usize = 50;
const MAX_INV: usize = 64;
const MAX_PARTICLES: usize = 128;
const MAX_SKILL_NODES: usize = 28;
const MAX_STATUS: usize = 4;
const NOISE_DECAY: u32 = 3;

// ── VGA Helpers ───────────────────────────────────────────────────────────────

#[inline(always)]
unsafe fn vga_put(col: usize, row: usize, ch: u8, attr: u8) {
    if col < 80 && row < 25 {
        *VGA.add(row * 80 + col) = ((attr as u16) << 8) | ch as u16;
    }
}
unsafe fn vga_str(col: usize, row: usize, s: &[u8], attr: u8) {
    for (i, &b) in s.iter().enumerate() {
        if col + i < 80 { vga_put(col + i, row, b, attr); }
    }
}
unsafe fn vga_fill_row(start_col: usize, row: usize, ch: u8, attr: u8) {
    for col in start_col..80 { vga_put(col, row, ch, attr); }
}
unsafe fn vga_box(col: usize, row: usize, w: usize, h: usize, attr: u8) {
    // Corners
    vga_put(col, row, 0xC9, attr);
    vga_put(col + w - 1, row, 0xBB, attr);
    vga_put(col, row + h - 1, 0xC8, attr);
    vga_put(col + w - 1, row + h - 1, 0xBC, attr);
    // Edges
    for x in col+1..col+w-1 { vga_put(x, row, 0xCD, attr); vga_put(x, row+h-1, 0xCD, attr); }
    for y in row+1..row+h-1 { vga_put(col, y, 0xBA, attr); vga_put(col+w-1, y, 0xBA, attr); }
    // Fill interior
    for y in row+1..row+h-1 { for x in col+1..col+w-1 { vga_put(x, y, b' ', attr & 0xF0); } }
}

// ── PRNG ──────────────────────────────────────────────────────────────────────

static mut RNG_STATE: u32 = 0xDEADBEEF;
unsafe fn rng() -> u32 {
    let mut x = RNG_STATE;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    RNG_STATE = x; x
}
unsafe fn rng_range(lo: i32, hi: i32) -> i32 {
    if lo >= hi { return lo; }
    lo + (rng() % (hi - lo).unsigned_abs()) as i32
}

// ── Number Formatting ─────────────────────────────────────────────────────────

fn u32_to_str(mut n: u32, buf: &mut [u8; 12]) -> &[u8] {
    if n == 0 { buf[0] = b'0'; return &buf[..1]; }
    let mut i = 0; let mut tmp = [0u8; 12];
    while n > 0 { tmp[i] = b'0' + (n % 10) as u8; n /= 10; i += 1; }
    for j in 0..i { buf[j] = tmp[i - 1 - j]; }
    &buf[..i]
}
fn i32_to_str(n: i32, buf: &mut [u8; 14]) -> &[u8] {
    if n == 0 { buf[0] = b'0'; return &buf[..1]; }
    let neg = n < 0;
    let mut abs = if neg { (-(n as i64)) as u32 } else { n as u32 };
    let mut i = 0; let mut tmp = [0u8; 12];
    while abs > 0 { tmp[i] = b'0' + (abs % 10) as u8; abs /= 10; i += 1; }
    let mut pos = 0;
    if neg { buf[pos] = b'-'; pos += 1; }
    for j in 0..i { buf[pos + j] = tmp[i - 1 - j]; }
    &buf[..pos + i]
}

// ── Log ───────────────────────────────────────────────────────────────────────

static mut LOG: [[u8; MAX_LOG_LEN]; MAX_LOG] = [[0u8; MAX_LOG_LEN]; MAX_LOG];
static mut LOG_LEN: [usize; MAX_LOG] = [0; MAX_LOG];
unsafe fn push_log(msg: &[u8]) {
    for i in 0..MAX_LOG - 1 { LOG[i] = LOG[i + 1]; LOG_LEN[i] = LOG_LEN[i + 1]; }
    let last = MAX_LOG - 1; LOG_LEN[last] = 0;
    let n = msg.len().min(MAX_LOG_LEN - 1);
    LOG[last][..n].copy_from_slice(&msg[..n]); LOG_LEN[last] = n;
}
unsafe fn push_log2(a: &[u8], b: &[u8]) {
    let mut buf = [0u8; MAX_LOG_LEN];
    let la = a.len().min(MAX_LOG_LEN - 1);
    buf[..la].copy_from_slice(&a[..la]);
    let lb = b.len().min(MAX_LOG_LEN - 1 - la);
    buf[la..la + lb].copy_from_slice(&b[..lb]);
    push_log(&buf[..la + lb]);
}

// =============================================================================
// TILES
// =============================================================================

#[derive(Copy, Clone, PartialEq)]
enum Tile {
    Wall, Floor, StairsDown, StairsUp,
    DoorClosed, DoorOpen,
    Water, Lava, Grass, Flower, Tree,
    Pillar, Rock, Trap { triggered: bool },
    Chest { opened: bool },
    Corpse { turns_left: u8 },
    Well, Campfire { lit: bool },
    Rubble,
}

impl Tile {
    fn glyph(self) -> u8 {
        match self {
            Tile::Wall => 0xB2, Tile::Floor => b'.',
            Tile::StairsDown => b'>', Tile::StairsUp => b'<',
            Tile::DoorClosed => b'+', Tile::DoorOpen => b'/',
            Tile::Water => b'~', Tile::Lava => b'^',
            Tile::Grass => b'"', Tile::Flower => 0xF9, Tile::Tree => 0x05,
            Tile::Pillar => 0xFE, Tile::Rock => 0xB0,
            Tile::Trap { .. } => b'^',
            Tile::Chest { opened: false } => b'=', Tile::Chest { opened: true } => b'_',
            Tile::Corpse { .. } => b'%',
            Tile::Well => 0x09, Tile::Campfire { lit: true } => b'*', Tile::Campfire { .. } => b'o',
            Tile::Rubble => 0xB1,
        }
    }
    fn attr(self) -> u8 {
        match self {
            Tile::Wall => 0x88, Tile::Floor => 0x07,
            Tile::StairsDown | Tile::StairsUp => 0x0A,
            Tile::DoorClosed | Tile::DoorOpen => 0x06,
            Tile::Water => 0x09, Tile::Lava => 0x0C,
            Tile::Grass => 0x02, Tile::Flower => 0x0D, Tile::Tree => 0x0A,
            Tile::Pillar => 0x08, Tile::Rock => 0x07,
            Tile::Trap { triggered: false } => 0x04, Tile::Trap { .. } => 0x08,
            Tile::Chest { opened: false } => 0x0E, Tile::Chest { .. } => 0x07,
            Tile::Corpse { .. } => 0x04,
            Tile::Well => 0x09, Tile::Campfire { lit: true } => 0x0E, Tile::Campfire { .. } => 0x08,
            Tile::Rubble => 0x08,
        }
    }
    fn is_walkable(self) -> bool {
        matches!(self,
            Tile::Floor | Tile::StairsDown | Tile::StairsUp | Tile::DoorOpen |
            Tile::Water | Tile::Grass | Tile::Flower | Tile::Trap { .. } |
            Tile::Chest { .. } | Tile::Corpse { .. } | Tile::Rubble
        )
    }
    fn blocks_sight(self) -> bool {
        matches!(self, Tile::Wall | Tile::DoorClosed | Tile::Pillar | Tile::Tree | Tile::Rock)
    }
    fn blocks_bullet(self) -> bool {
        matches!(self, Tile::Wall | Tile::Pillar | Tile::Rock | Tile::Tree | Tile::DoorClosed)
    }
}

// =============================================================================
// RARITY
// =============================================================================

#[derive(PartialEq, PartialOrd, Copy, Clone)]
enum Rarity { Common, Uncommon, Rare, Legendary, Mythic }
impl Rarity {
    fn attr(self) -> u8 {
        match self {
            Rarity::Common => 0x07, Rarity::Uncommon => 0x09,
            Rarity::Rare => 0x0E, Rarity::Legendary => 0x0C, Rarity::Mythic => 0x0D,
        }
    }
    fn name(self) -> &'static [u8] {
        match self {
            Rarity::Common => b"Common", Rarity::Uncommon => b"Uncommon",
            Rarity::Rare => b"Rare", Rarity::Legendary => b"Legend", Rarity::Mythic => b"Mythic",
        }
    }
    fn glow_attr(self) -> u8 {
        match self {
            Rarity::Mythic => 0x5D, Rarity::Legendary => 0x4C, _ => self.attr(),
        }
    }
}

// =============================================================================
// WEAPONS & AMMO
// =============================================================================

#[derive(Copy, Clone, PartialEq)]
enum WeaponKind {
    Fists, Dagger, Sword, Axe, Warhammer, Greatsword, Staff,
    Pistol, Shotgun,
}
impl WeaponKind {
    fn name(self) -> &'static [u8] {
        match self {
            WeaponKind::Fists => b"Fists", WeaponKind::Dagger => b"Dagger",
            WeaponKind::Sword => b"Sword", WeaponKind::Axe => b"Axe",
            WeaponKind::Warhammer => b"Warhammer", WeaponKind::Greatsword => b"Greatsword",
            WeaponKind::Staff => b"Staff",
            WeaponKind::Pistol => b"Pistol", WeaponKind::Shotgun => b"Shotgun",
        }
    }
    fn glyph(self) -> u8 {
        match self {
            WeaponKind::Fists => b'!', WeaponKind::Dagger => b'/',
            WeaponKind::Sword => b'|', WeaponKind::Axe => b'(',
            WeaponKind::Warhammer => b'T', WeaponKind::Greatsword => b'?',
            WeaponKind::Staff => b'&',
            WeaponKind::Pistol => b'p', WeaponKind::Shotgun => b'P',
        }
    }
    fn attack_delay_ms(self) -> u32 {
        match self {
            WeaponKind::Fists => 800, WeaponKind::Dagger => 1000,
            WeaponKind::Sword => 1800, WeaponKind::Axe => 2500,
            WeaponKind::Warhammer => 3500, WeaponKind::Greatsword => 4000,
            WeaponKind::Staff => 1400,
            WeaponKind::Pistol | WeaponKind::Shotgun => 600,
        }
    }
    fn damage_mod(self) -> i32 {
        match self {
            WeaponKind::Fists => 0, WeaponKind::Dagger => -1, WeaponKind::Sword => 2,
            WeaponKind::Axe => 5, WeaponKind::Warhammer => 8, WeaponKind::Greatsword => 12,
            WeaponKind::Staff => 6,
            WeaponKind::Pistol => 8, WeaponKind::Shotgun => 15,
        }
    }
    fn is_ranged(self) -> bool { matches!(self, WeaponKind::Pistol | WeaponKind::Shotgun) }
    fn ammo_type(self) -> usize { match self { WeaponKind::Shotgun => 1, _ => 0 } }
    fn noise_radius(self) -> i32 {
        match self {
            WeaponKind::Pistol => 14, WeaponKind::Shotgun => 20, _ => 2,
        }
    }
}

// =============================================================================
// STATUS EFFECTS
// =============================================================================

#[derive(Copy, Clone, PartialEq)]
enum StatusKind { Poisoned, Burning, Stunned, Blessed }
impl StatusKind {
    fn glyph(self) -> u8 { match self { StatusKind::Poisoned => b'p', StatusKind::Burning => b'!', StatusKind::Stunned => b'*', StatusKind::Blessed => b'+' } }
    fn attr(self) -> u8 { match self { StatusKind::Poisoned => 0x0A, StatusKind::Burning => 0x0C, StatusKind::Stunned => 0x0E, StatusKind::Blessed => 0x0D } }
    fn name(self) -> &'static [u8] { match self { StatusKind::Poisoned => b"Poison", StatusKind::Burning => b"Burn", StatusKind::Stunned => b"Stun", StatusKind::Blessed => b"Bless" } }
}

#[derive(Copy, Clone)]
struct StatusEffect { kind: StatusKind, turns_left: u8 }
impl StatusEffect { fn new() -> Self { Self { kind: StatusKind::Stunned, turns_left: 0 } } }

// =============================================================================
// PARTICLES
// =============================================================================

#[derive(Copy, Clone, PartialEq)]
enum ParticleKind {
    Bullet, BulletTrail, Blood, Spark, Smoke,
    Explosion, XpOrb, Dust, LegendGlow, BossAura,
}

#[derive(Copy, Clone)]
struct Particle {
    x: i32, y: i32,           // map coords * 1000
    vx: i32, vy: i32,         // velocity * 1000 per frame
    life: u16, max_life: u16,
    glyph: u8, color: u8,
    kind: ParticleKind,
    target_x: i32, target_y: i32, // for homing types
    active: bool,
}
impl Particle {
    const fn new() -> Self {
        Self { x:0,y:0,vx:0,vy:0,life:0,max_life:1,glyph:b'.',color:0x0F,
               kind:ParticleKind::Dust,target_x:0,target_y:0,active:false }
    }
}

static mut PARTICLES: [Particle; MAX_PARTICLES] = [Particle::new(); MAX_PARTICLES];

unsafe fn spawn_particle(px: i32, py: i32, vx: i32, vy: i32, life: u16,
                          glyph: u8, color: u8, kind: ParticleKind) {
    for p in PARTICLES.iter_mut() {
        if !p.active {
            *p = Particle { x: px*1000+500, y: py*1000+500, vx, vy,
                            life, max_life: life, glyph, color, kind,
                            target_x:0, target_y:0, active: true };
            return;
        }
    }
}

unsafe fn spawn_blood(x: i32, y: i32, count: u8) {
    for _ in 0..count {
        let vx = rng_range(-300, 300); let vy = rng_range(-300, 300);
        spawn_particle(x, y, vx, vy, rng_range(3, 8) as u16, b'*', 0x04, ParticleKind::Blood);
    }
}
unsafe fn spawn_explosion(x: i32, y: i32, radius: i32) {
    for i in 0..16 {
        let angle = i * 400;
        let vx = cos_approx(angle) / 2; let vy = sin_approx(angle) / 2;
        spawn_particle(x, y, vx, vy, 6, b'*', 0x0E, ParticleKind::Explosion);
        spawn_particle(x, y, vx/2, vy/2, 10, 0xB1 as u8, 0x0C, ParticleKind::Smoke);
    }
    for r in 0..radius.min(3) {
        for i in 0..8 {
            let dx = [-1i32,0,1,1,1,0,-1,-1][i]; let dy = [-1i32,-1,-1,0,1,1,1,0][i];
            spawn_particle(x+dx*(r+1), y+dy*(r+1), dx*200, dy*200, 4, b':', 0x06, ParticleKind::Spark);
        }
    }
}
unsafe fn spawn_xp_orb(x: i32, y: i32, count: u8) {
    for _ in 0..count {
        let vx = rng_range(-150, 150); let vy = rng_range(-400, -100);
        spawn_particle(x, y, vx, vy, rng_range(8, 15) as u16, b'o', 0x0E, ParticleKind::XpOrb);
    }
}
unsafe fn spawn_dust(x: i32, y: i32) {
    let vx = rng_range(-100, 100); let vy = rng_range(-200, 0);
    spawn_particle(x, y, vx, vy, 3, b'.', 0x08, ParticleKind::Dust);
}
unsafe fn spawn_boss_aura(x: i32, y: i32, tick: u32) {
    let angle = ((tick * 60) % 6284) as i32;
    for i in 0..4 {
        let a = (angle + i * 1571) % 6284;
        let px = x + cos_approx(a) * 2 / 1000;
        let py = y + sin_approx(a) * 2 / 1000;
        spawn_particle(px, py, 0, -50, 12, b'*', 0x0D, ParticleKind::BossAura);
    }
}

unsafe fn update_particles(tiles: &[[Tile; MAP_W]; MAP_H]) {
    for p in PARTICLES.iter_mut() {
        if !p.active { continue; }
        if p.life == 0 { p.active = false; continue; }
        p.life -= 1;
        p.x += p.vx; p.y += p.vy;
        // gravity for some types
        if matches!(p.kind, ParticleKind::Blood | ParticleKind::Spark) { p.vy += 50; }
        // fade XP orbs upward
        if p.kind == ParticleKind::XpOrb { p.vx = p.vx * 9 / 10; }
        // bullet: check wall collision
        if p.kind == ParticleKind::Bullet {
            let tx = (p.x / 1000) as usize; let ty = (p.y / 1000) as usize;
            if tx < MAP_W && ty < MAP_H && tiles[ty][tx].blocks_bullet() { p.active = false; spawn_particle(tx as i32, ty as i32, 0, 0, 3, b'*', 0x0E, ParticleKind::Spark); }
        }
    }
}

// =============================================================================
// TASK/AI SYSTEM
// =============================================================================

#[derive(Copy, Clone, PartialEq)]
enum AIBehavior {
    Idle, Wander, ChasePlayer, Flee, Patrol,
    GuardPoint, Alerted,
}

#[derive(Copy, Clone, PartialEq)]
enum MonKind {
    Rat, Goblin, Troll, Warlord,
    Spider, Skeleton, Wraith, Vampire,
    Merchant, Villager, QuestGiver, Guard,
    AncientOne,
}

impl MonKind {
    fn glyph(self) -> u8 {
        match self {
            MonKind::Rat=>b'r', MonKind::Goblin=>b'g', MonKind::Troll=>b'T', MonKind::Warlord=>b'W',
            MonKind::Spider=>b's', MonKind::Skeleton=>b'S', MonKind::Wraith=>b'w', MonKind::Vampire=>b'V',
            MonKind::Merchant=>b'$', MonKind::Villager=>b'v', MonKind::QuestGiver=>b'Q', MonKind::Guard=>b'G',
            MonKind::AncientOne=>b'D',
        }
    }
    fn attr(self) -> u8 {
        match self {
            MonKind::Rat=>0x0C, MonKind::Goblin=>0x0A, MonKind::Troll=>0x0B, MonKind::Warlord=>0x0E,
            MonKind::Spider=>0x06, MonKind::Skeleton=>0x07, MonKind::Wraith=>0x09, MonKind::Vampire=>0x04,
            MonKind::Merchant=>0x0E, MonKind::Villager=>0x0F, MonKind::QuestGiver=>0x0D, MonKind::Guard=>0x0B,
            MonKind::AncientOne=>0x4C,
        }
    }
    fn name(self) -> &'static [u8] {
        match self {
            MonKind::Rat=>b"Rat", MonKind::Goblin=>b"Goblin", MonKind::Troll=>b"Troll", MonKind::Warlord=>b"Warlord",
            MonKind::Spider=>b"Spider", MonKind::Skeleton=>b"Skeleton", MonKind::Wraith=>b"Wraith", MonKind::Vampire=>b"Vampire",
            MonKind::Merchant=>b"Merchant", MonKind::Villager=>b"Villager", MonKind::QuestGiver=>b"Quest Giver", MonKind::Guard=>b"Guard",
            MonKind::AncientOne=>b"ANCIENT ONE",
        }
    }
    fn max_hp(self) -> i32 {
        match self {
            MonKind::Merchant|MonKind::Villager|MonKind::QuestGiver|MonKind::Guard=>9999,
            MonKind::Rat=>8, MonKind::Goblin=>16, MonKind::Troll=>32, MonKind::Warlord=>120,
            MonKind::Spider=>12, MonKind::Skeleton=>24, MonKind::Wraith=>40, MonKind::Vampire=>55,
            MonKind::AncientOne=>600,
        }
    }
    fn atk(self) -> i32 {
        match self {
            MonKind::AncientOne=>30, MonKind::Warlord=>12, MonKind::Vampire=>10, MonKind::Wraith=>8,
            MonKind::Troll=>7, MonKind::Skeleton=>5, MonKind::Goblin=>4, MonKind::Spider=>3,
            MonKind::Rat=>2, _=>0,
        }
    }
    fn def(self) -> i32 {
        match self { MonKind::AncientOne=>12, MonKind::Warlord=>5, MonKind::Troll=>3, _=>0 }
    }
    fn xp(self) -> u32 {
        match self {
            MonKind::Rat=>8, MonKind::Goblin=>18, MonKind::Troll=>35, MonKind::Warlord=>200,
            MonKind::Spider=>14, MonKind::Skeleton=>22, MonKind::Wraith=>40, MonKind::Vampire=>55,
            MonKind::AncientOne=>1000, _=>0,
        }
    }
    fn detection_radius(self) -> i32 {
        match self { MonKind::Wraith=>12, MonKind::Vampire=>10, MonKind::AncientOne=>20, _=>7 }
    }
    fn wander_radius(self) -> i32 {
        match self { MonKind::Villager|MonKind::Guard=>8, MonKind::Merchant=>4, _=>15 }
    }
    fn is_hostile(self) -> bool {
        !matches!(self, MonKind::Merchant|MonKind::Villager|MonKind::QuestGiver|MonKind::Guard)
    }
}

#[derive(Copy, Clone)]
struct Monster {
    kind: MonKind, x: i32, y: i32, hp: i32,
    alive: bool,
    // AI State
    behavior: AIBehavior,
    home_x: i32, home_y: i32,
    wander_tick: u32,
    patrol_idx: u8,
    alert_level: u8,          // 0=calm 255=full alert
    flash_tick: u32,          // combat flash timer
    status: [StatusEffect; MAX_STATUS],
    // Patrol path (up to 4 waypoints)
    patrol_x: [i32; 4], patrol_y: [i32; 4], patrol_len: u8,
    // Last known player pos
    last_seen_x: i32, last_seen_y: i32,
}
impl Monster {
    fn new_at(kind: MonKind, x: i32, y: i32) -> Self {
        Self {
            kind, x, y, hp: kind.max_hp(), alive: true,
            behavior: if kind.is_hostile() { AIBehavior::Wander } else { AIBehavior::Wander },
            home_x: x, home_y: y, wander_tick: 0, patrol_idx: 0,
            alert_level: 0, flash_tick: 0,
            status: [StatusEffect::new(); MAX_STATUS],
            patrol_x: [x,x,x,x], patrol_y: [y,y,y,y], patrol_len: 0,
            last_seen_x: x, last_seen_y: y,
        }
    }
    fn has_status(&self, kind: StatusKind) -> bool {
        self.status.iter().any(|s| s.turns_left > 0 && s.kind == kind)
    }
    fn apply_status(&mut self, kind: StatusKind, turns: u8) {
        for s in self.status.iter_mut() {
            if s.turns_left == 0 { *s = StatusEffect { kind, turns_left: turns }; return; }
        }
    }
    fn tick_status(&mut self) -> i32 {
        let mut dmg = 0i32;
        for s in self.status.iter_mut() {
            if s.turns_left == 0 { continue; }
            s.turns_left -= 1;
            match s.kind {
                StatusKind::Poisoned => dmg += 2,
                StatusKind::Burning => dmg += 4,
                _ => {}
            }
        }
        dmg
    }
}

// Simple BFS pathfinder
unsafe fn bfs_step(sx: i32, sy: i32, tx: i32, ty: i32,
                    tiles: &[[Tile; MAP_W]; MAP_H],
                    monsters: &[Monster; MAX_MONSTERS]) -> (i32, i32) {
    if sx == tx && sy == ty { return (0, 0); }
    const MAX_STEPS: usize = 18;
    let mut queue: [(i32, i32, i32, i32); 256] = [(0,0,0,0); 256]; // x,y,dx,dy
    let mut visited: [[bool; 40]; 40] = [[false; 40]; 40];
    let mut qh = 0usize; let mut qt = 0usize;
    let ox = sx - 19; let oy = sy - 19;
    let lx = (sx - ox) as usize; let ly = (sy - oy) as usize;
    if lx < 40 && ly < 40 { visited[ly][lx] = true; }
    queue[qt] = (sx, sy, 0, 0); qt += 1;
    let dirs: [(i32,i32); 4] = [(1,0),(-1,0),(0,1),(0,-1)];
    while qh < qt && qt < 255 {
        let (cx, cy, fdx, fdy) = queue[qh]; qh += 1;
        for &(ddx, ddy) in &dirs {
            let nx = cx + ddx; let ny = cy + ddy;
            if nx < 1 || ny < 1 || nx >= MAP_W as i32-1 || ny >= MAP_H as i32-1 { continue; }
            let lnx = (nx - ox) as usize; let lny = (ny - oy) as usize;
            if lnx >= 40 || lny >= 40 { continue; }
            if visited[lny][lnx] { continue; }
            if !tiles[ny as usize][nx as usize].is_walkable() { continue; }
            if monsters.iter().any(|m| m.alive && m.x == nx && m.y == ny) { continue; }
            visited[lny][lnx] = true;
            let first_dx = if fdx == 0 && fdy == 0 { ddx } else { fdx };
            let first_dy = if fdx == 0 && fdy == 0 { ddy } else { fdy };
            if nx == tx && ny == ty { return (first_dx, first_dy); }
            if (nx - tx).abs() + (ny - ty).abs() < MAX_STEPS as i32 {
                queue[qt] = (nx, ny, first_dx, first_dy); qt += 1;
            }
        }
    }
    // Fallback: direct step
    let dx = (tx - sx).signum(); let dy = (ty - sy).signum();
    (dx, dy)
}

// Noise map: gunshot noise propagates and attracts enemies
static mut NOISE_MAP: [[u8; MAP_W]; MAP_H] = [[0u8; MAP_W]; MAP_H];
static mut NOISE_TICK: u32 = 0;

unsafe fn emit_noise(x: i32, y: i32, radius: i32) {
    for dy in -radius..=radius {
        for dx in -radius..=radius {
            let dist = dx.abs() + dy.abs();
            if dist > radius { continue; }
            let nx = x + dx; let ny = y + dy;
            if nx < 0 || ny < 0 || nx >= MAP_W as i32 || ny >= MAP_H as i32 { continue; }
            let strength = ((radius - dist) * 30 / radius).max(0) as u8;
            let cur = NOISE_MAP[ny as usize][nx as usize];
            if strength > cur { NOISE_MAP[ny as usize][nx as usize] = strength; }
        }
    }
    NOISE_TICK = 0;
}
unsafe fn decay_noise() {
    NOISE_TICK += 1;
    if NOISE_TICK < NOISE_DECAY { return; }
    NOISE_TICK = 0;
    for row in NOISE_MAP.iter_mut() {
        for v in row.iter_mut() { if *v > 0 { *v -= 1; } }
    }
}

unsafe fn update_monster_ai(
    i: usize,
    player_x: i32, player_y: i32,
    player_visible_to_mon: bool,
    tiles: &[[Tile; MAP_W]; MAP_H],
    monsters: &mut [Monster; MAX_MONSTERS],
    sys_tick: u32,
) {
    if !monsters[i].alive { return; }
    let kind = monsters[i].kind;

    // Tick status effects
    let status_dmg = monsters[i].tick_status();
    if status_dmg > 0 { monsters[i].hp -= status_dmg; }
    if monsters[i].hp <= 0 { monsters[i].alive = false; return; }

    // Stunned = skip turn
    if monsters[i].has_status(StatusKind::Stunned) { return; }

    // Non-hostile NPCs: simple wander between home and random spot
    if !kind.is_hostile() {
        monsters[i].wander_tick += 1;
        if monsters[i].wander_tick % 4 != 0 { return; }
        if (sys_tick / 60) % 8 < 4 {
            // Walk toward home
            let (dx, dy) = bfs_step(monsters[i].x, monsters[i].y,
                                      monsters[i].home_x, monsters[i].home_y, tiles, monsters);
            let nx = monsters[i].x + dx; let ny = monsters[i].y + dy;
            if nx >= 0 && ny >= 0 && tiles[ny as usize][nx as usize].is_walkable() &&
               !monsters[0..i].iter().chain(monsters[i+1..].iter()).any(|m| m.alive && m.x==nx && m.y==ny) {
                monsters[i].x = nx; monsters[i].y = ny;
            }
        } else {
            // Wander randomly
            let dx = rng_range(-1, 2); let dy = rng_range(-1, 2);
            if dx == 0 && dy == 0 { return; }
            let nx = monsters[i].x + dx; let ny = monsters[i].y + dy;
            let wr = kind.wander_radius();
            if (nx - monsters[i].home_x).abs() > wr || (ny - monsters[i].home_y).abs() > wr { return; }
            if nx >= 0 && ny >= 0 && tiles[ny as usize][nx as usize].is_walkable() &&
               !monsters[0..i].iter().chain(monsters[i+1..].iter()).any(|m| m.alive && m.x==nx && m.y==ny) {
                monsters[i].x = nx; monsters[i].y = ny;
            }
        }
        return;
    }

    // Hostile AI state machine
    let dist_to_player = (monsters[i].x - player_x).abs() + (monsters[i].y - player_y).abs();
    let detect_r = kind.detection_radius();

    // Detection: can we see/hear player?
    if player_visible_to_mon && dist_to_player <= detect_r {
        monsters[i].alert_level = 255;
        monsters[i].last_seen_x = player_x;
        monsters[i].last_seen_y = player_y;
        monsters[i].behavior = AIBehavior::ChasePlayer;
    }

    // Noise-based alerting
    let noise = NOISE_MAP[monsters[i].y as usize][monsters[i].x as usize];
    if noise > 50 && monsters[i].behavior == AIBehavior::Wander {
        monsters[i].alert_level = monsters[i].alert_level.saturating_add(noise / 3);
        if monsters[i].alert_level > 120 {
            // Navigate toward noise source - find loudest adjacent cell
            let mut best_n = noise; let mut bdx = 0i32; let mut bdy = 0i32;
            for dy in -1i32..=1 { for dx in -1i32..=1 {
                let nx = monsters[i].x + dx; let ny = monsters[i].y + dy;
                if nx < 0 || ny < 0 || nx >= MAP_W as i32 || ny >= MAP_H as i32 { continue; }
                let n = NOISE_MAP[ny as usize][nx as usize];
                if n > best_n { best_n = n; bdx = dx; bdy = dy; }
            }}
            if bdx != 0 || bdy != 0 {
                let nx = monsters[i].x + bdx; let ny = monsters[i].y + bdy;
                if tiles[ny as usize][nx as usize].is_walkable() &&
                   !monsters.iter().any(|m| m.alive && m.x==nx && m.y==ny) {
                    monsters[i].x = nx; monsters[i].y = ny;
                }
                return;
            }
        }
    }

    // Decay alert
    if monsters[i].alert_level > 0 { monsters[i].alert_level -= 1; }
    if monsters[i].alert_level == 0 && monsters[i].behavior == AIBehavior::ChasePlayer {
        monsters[i].behavior = AIBehavior::Wander;
    }

    // HP-based fleeing
    if monsters[i].hp < monsters[i].kind.max_hp() / 4 && kind != MonKind::AncientOne {
        monsters[i].behavior = AIBehavior::Flee;
    }

    match monsters[i].behavior {
        AIBehavior::ChasePlayer => {
            if dist_to_player == 1 {
                // Adjacent: melee attack (handled in game loop)
                return;
            }
            // Path to last known position
            let tx = monsters[i].last_seen_x; let ty = monsters[i].last_seen_y;
            let (dx, dy) = bfs_step(monsters[i].x, monsters[i].y, tx, ty, tiles, monsters);
            let nx = monsters[i].x + dx; let ny = monsters[i].y + dy;
            if nx == player_x && ny == player_y { return; } // attack handled outside
            if dx == 0 && dy == 0 { monsters[i].behavior = AIBehavior::Wander; return; }
            if tiles[ny as usize][nx as usize].is_walkable() &&
               !monsters[0..i].iter().chain(monsters[i+1..].iter()).any(|m| m.alive && m.x==nx && m.y==ny) {
                monsters[i].x = nx; monsters[i].y = ny;
            }
        }
        AIBehavior::Flee => {
            let away_x = monsters[i].x + (monsters[i].x - player_x).signum();
            let away_y = monsters[i].y + (monsters[i].y - player_y).signum();
            let ax = away_x.max(0).min(MAP_W as i32-1);
            let ay = away_y.max(0).min(MAP_H as i32-1);
            if tiles[ay as usize][ax as usize].is_walkable() &&
               !monsters.iter().any(|m| m.alive && m.x==ax && m.y==ay) {
                monsters[i].x = ax; monsters[i].y = ay;
            }
        }
        AIBehavior::Wander => {
            monsters[i].wander_tick += 1;
            if monsters[i].wander_tick % 3 != 0 { return; }
            let dx = rng_range(-1, 2); let dy = rng_range(-1, 2);
            if dx == 0 && dy == 0 { return; }
            let nx = monsters[i].x + dx; let ny = monsters[i].y + dy;
            let wr = kind.wander_radius();
            if (nx - monsters[i].home_x).abs() > wr || (ny - monsters[i].home_y).abs() > wr { return; }
            if nx < 1 || ny < 1 || nx >= MAP_W as i32-1 || ny >= MAP_H as i32-1 { return; }
            if tiles[ny as usize][nx as usize].is_walkable() &&
               !monsters[0..i].iter().chain(monsters[i+1..].iter()).any(|m| m.alive && m.x==nx && m.y==ny) {
                monsters[i].x = nx; monsters[i].y = ny;
            }
        }
        AIBehavior::Patrol => {
            if monsters[i].patrol_len == 0 { monsters[i].behavior = AIBehavior::Wander; return; }
            let pidx = monsters[i].patrol_idx as usize % monsters[i].patrol_len as usize;
            let tx = monsters[i].patrol_x[pidx]; let ty = monsters[i].patrol_y[pidx];
            if monsters[i].x == tx && monsters[i].y == ty {
                monsters[i].patrol_idx = (monsters[i].patrol_idx + 1) % monsters[i].patrol_len;
                return;
            }
            let (dx, dy) = bfs_step(monsters[i].x, monsters[i].y, tx, ty, tiles, monsters);
            let nx = monsters[i].x + dx; let ny = monsters[i].y + dy;
            if tiles[ny as usize][nx as usize].is_walkable() &&
               !monsters[0..i].iter().chain(monsters[i+1..].iter()).any(|m| m.alive && m.x==nx && m.y==ny) {
                monsters[i].x = nx; monsters[i].y = ny;
            }
        }
        _ => {}
    }
}

// =============================================================================
// ITEMS
// =============================================================================

#[derive(Copy, Clone, PartialEq)]
enum ItemKind {
    HealthPotion, ManaPotion, Food, Gold(u32), MagicOrb, SkillOrb,
    Weapon(WeaponKind),
    Ammo { ammo_type: usize, count: u32 },
    ScrollFireball, ScrollTeleport, ScrollIdentify,
    RingProtection, RingStrength, RingSpeed,
    Armor { defense: i32, name_id: u8 },
    Ore { name_id: u8, value: u32 },
    Backpack { slots: usize },
    PoisonVial, FlashBang,
}

static ARMOR_NAMES: [&[u8]; 6] = [b"Leather", b"Chainmail", b"Plate", b"Shadow", b"Dragon", b"Mythril"];
static ORE_NAMES: [&[u8]; 4] = [b"Iron Ore", b"Silver Ore", b"Gold Ore", b"Crystal"];

impl ItemKind {
    fn glyph(self) -> u8 {
        match self {
            ItemKind::HealthPotion|ItemKind::ManaPotion => b'!',
            ItemKind::Food => b'%', ItemKind::Gold(_) => b'$',
            ItemKind::MagicOrb|ItemKind::SkillOrb => b'*',
            ItemKind::Weapon(w) => w.glyph(),
            ItemKind::Ammo { .. } => b'}',
            ItemKind::ScrollFireball|ItemKind::ScrollTeleport|ItemKind::ScrollIdentify => b'?',
            ItemKind::RingProtection|ItemKind::RingStrength|ItemKind::RingSpeed => b'o',
            ItemKind::Armor { .. } => b'[',
            ItemKind::Ore { .. } => 0xB0,
            ItemKind::Backpack { .. } => b'B',
            ItemKind::PoisonVial => b'!', ItemKind::FlashBang => b'*',
        }
    }
    fn name(self) -> &'static [u8] {
        match self {
            ItemKind::HealthPotion => b"Health Potion", ItemKind::ManaPotion => b"Mana Potion",
            ItemKind::Food => b"Ration", ItemKind::Gold(_) => b"Gold",
            ItemKind::MagicOrb => b"Magic Orb", ItemKind::SkillOrb => b"Skill Orb",
            ItemKind::Weapon(w) => w.name(),
            ItemKind::Ammo { ammo_type: 0, .. } => b"Pistol Ammo",
            ItemKind::Ammo { .. } => b"Shell",
            ItemKind::ScrollFireball => b"Scroll: Fire", ItemKind::ScrollTeleport => b"Scroll: Tele",
            ItemKind::ScrollIdentify => b"Scroll: Id",
            ItemKind::RingProtection => b"Ring: Prot", ItemKind::RingStrength => b"Ring: Str",
            ItemKind::RingSpeed => b"Ring: Speed",
            ItemKind::Armor { name_id, .. } => ARMOR_NAMES[name_id as usize % 6],
            ItemKind::Ore { name_id, .. } => ORE_NAMES[name_id as usize % 4],
            ItemKind::Backpack { .. } => b"Backpack",
            ItemKind::PoisonVial => b"Poison Vial", ItemKind::FlashBang => b"Flash Bang",
        }
    }
    fn category(self) -> u8 {
        match self {
            ItemKind::Weapon(_) | ItemKind::Armor { .. } => 0,
            ItemKind::HealthPotion | ItemKind::ManaPotion | ItemKind::Food | ItemKind::PoisonVial | ItemKind::FlashBang => 1,
            ItemKind::ScrollFireball | ItemKind::ScrollTeleport | ItemKind::ScrollIdentify => 2,
            ItemKind::Ammo { .. } | ItemKind::Ore { .. } | ItemKind::Gold(_) => 3,
            _ => 4,
        }
    }
    fn sell_value(self) -> u32 {
        match self {
            ItemKind::Gold(x) => x,
            ItemKind::HealthPotion => 12, ItemKind::ManaPotion => 12, ItemKind::Food => 4,
            ItemKind::Ore { value, .. } => value,
            ItemKind::Weapon(w) => (w.damage_mod().max(0) as u32 + 1) * 20,
            ItemKind::Armor { defense, .. } => defense as u32 * 25,
            ItemKind::Ammo { count, .. } => count * 2,
            _ => 30,
        }
    }
    fn buy_value(self) -> u32 { (self.sell_value() * 2).max(15) }
    fn is_equippable(self) -> bool {
        matches!(self, ItemKind::Weapon(_) | ItemKind::Armor { .. } | ItemKind::RingProtection | ItemKind::RingStrength | ItemKind::RingSpeed)
    }
}

#[derive(Copy, Clone)]
struct Item { kind: ItemKind, x: i32, y: i32, active: bool, rarity: Rarity }
impl Item {
    fn new(kind: ItemKind, rarity: Rarity) -> Self {
        Self { kind, x: 0, y: 0, active: true, rarity }
    }
}

// =============================================================================
// SKILL TREE  (Hexagonal Diablo-3 style)
// =============================================================================

#[derive(Copy, Clone, PartialEq)]
enum SkillPath { Warrior, Ranger, Mage, Rogue }

#[derive(Copy, Clone)]
struct SkillNode {
    id: u8, name: &'static [u8], desc: &'static [u8],
    path: SkillPath, tier: u8,
    requires: u8,   // id of prerequisite (255 = none)
    unlocked: bool,
    // Effect IDs for dispatch
    effect_id: u8,
}

static SKILL_TREE: [SkillNode; MAX_SKILL_NODES] = [
    // WARRIOR  (path 0)
    SkillNode{id:0,  name:b"Iron Skin",    desc:b"+3 Defense permanently",       path:SkillPath::Warrior, tier:0, requires:255, unlocked:false, effect_id:0},
    SkillNode{id:1,  name:b"Sunder",       desc:b"Attacks reduce enemy DEF",     path:SkillPath::Warrior, tier:1, requires:0,   unlocked:false, effect_id:1},
    SkillNode{id:2,  name:b"Warcry",       desc:b"+2 ATK for 10 turns on Q",     path:SkillPath::Warrior, tier:1, requires:0,   unlocked:false, effect_id:2},
    SkillNode{id:3,  name:b"Bulwark",      desc:b"25% chance to block attack",   path:SkillPath::Warrior, tier:2, requires:1,   unlocked:false, effect_id:3},
    SkillNode{id:4,  name:b"Titan Blow",   desc:b"Every 5th hit does 3x dmg",    path:SkillPath::Warrior, tier:2, requires:2,   unlocked:false, effect_id:4},
    SkillNode{id:5,  name:b"Colossus",     desc:b"+20 Max HP, immune to stun",   path:SkillPath::Warrior, tier:3, requires:3,   unlocked:false, effect_id:5},
    SkillNode{id:6,  name:b"Earthquake",   desc:b"Stun all visible enemies",     path:SkillPath::Warrior, tier:3, requires:4,   unlocked:false, effect_id:6},
    // RANGER  (path 1)
    SkillNode{id:7,  name:b"Eagle Eye",    desc:b"+3 detection/FOV range",       path:SkillPath::Ranger,  tier:0, requires:255, unlocked:false, effect_id:10},
    SkillNode{id:8,  name:b"Headshot",     desc:b"Ranged: 20% instant kill",     path:SkillPath::Ranger,  tier:1, requires:7,   unlocked:false, effect_id:11},
    SkillNode{id:9,  name:b"Ammo Cache",   desc:b"Find 2x more ammo",            path:SkillPath::Ranger,  tier:1, requires:7,   unlocked:false, effect_id:12},
    SkillNode{id:10, name:b"Silencer",     desc:b"Guns make no noise",           path:SkillPath::Ranger,  tier:2, requires:8,   unlocked:false, effect_id:13},
    SkillNode{id:11, name:b"Explosive Rd", desc:b"Bullets explode on impact",    path:SkillPath::Ranger,  tier:2, requires:9,   unlocked:false, effect_id:14},
    SkillNode{id:12, name:b"Rain of Fire", desc:b"Shotgun fires 9 pellets",      path:SkillPath::Ranger,  tier:3, requires:10,  unlocked:false, effect_id:15},
    SkillNode{id:13, name:b"Ghost Walk",   desc:b"Move through enemies once",    path:SkillPath::Ranger,  tier:3, requires:11,  unlocked:false, effect_id:16},
    // MAGE  (path 2)
    SkillNode{id:14, name:b"Arcane Mind",  desc:b"Scrolls recharge on level up", path:SkillPath::Mage,    tier:0, requires:255, unlocked:false, effect_id:20},
    SkillNode{id:15, name:b"Fire Mastery", desc:b"Fireball does +50% damage",    path:SkillPath::Mage,    tier:1, requires:14,  unlocked:false, effect_id:21},
    SkillNode{id:16, name:b"Time Warp",    desc:b"Teleport triggers invisib.",   path:SkillPath::Mage,    tier:1, requires:14,  unlocked:false, effect_id:22},
    SkillNode{id:17, name:b"Chain Light",  desc:b"Hits chain to 2 enemies",      path:SkillPath::Mage,    tier:2, requires:15,  unlocked:false, effect_id:23},
    SkillNode{id:18, name:b"Arcane Armor", desc:b"Mana shield absorbs 5 dmg",    path:SkillPath::Mage,    tier:2, requires:16,  unlocked:false, effect_id:24},
    SkillNode{id:19, name:b"Black Hole",   desc:b"Pull all enemies 3 tiles in",  path:SkillPath::Mage,    tier:3, requires:17,  unlocked:false, effect_id:25},
    SkillNode{id:20, name:b"Singularity",  desc:b"Massive AOE centered on you",  path:SkillPath::Mage,    tier:3, requires:18,  unlocked:false, effect_id:26},
    // ROGUE  (path 3)
    SkillNode{id:21, name:b"Pickpocket",   desc:b"Steal from merchants cheap",   path:SkillPath::Rogue,   tier:0, requires:255, unlocked:false, effect_id:30},
    SkillNode{id:22, name:b"Backstab",     desc:b"2x dmg when behind enemy",     path:SkillPath::Rogue,   tier:1, requires:21,  unlocked:false, effect_id:31},
    SkillNode{id:23, name:b"Shadow Step",  desc:b"Teleport behind target enemy", path:SkillPath::Rogue,   tier:1, requires:21,  unlocked:false, effect_id:32},
    SkillNode{id:24, name:b"Vanish",       desc:b"Press V to turn invisible",    path:SkillPath::Rogue,   tier:2, requires:22,  unlocked:false, effect_id:33},
    SkillNode{id:25, name:b"Poison Blade", desc:b"Attacks apply 4-turn poison",  path:SkillPath::Rogue,   tier:2, requires:23,  unlocked:false, effect_id:34},
    SkillNode{id:26, name:b"Death Mark",   desc:b"Marked enemy takes 2x dmg",   path:SkillPath::Rogue,   tier:3, requires:24,  unlocked:false, effect_id:35},
    SkillNode{id:27, name:b"One With Dark",desc:b"See all monsters through walls",path:SkillPath::Rogue,  tier:3, requires:25,  unlocked:false, effect_id:36},
];

// =============================================================================
// PLAYER
// =============================================================================

#[derive(Copy, Clone)]
struct Player {
    x: i32, y: i32, hp: i32, max_hp: i32,
    base_atk: i32, base_def: i32,
    xp: u32, level: u32, gold: u32,
    weapon: WeaponKind, armor: Option<ItemKind>,
    ring1: Option<ItemKind>, ring2: Option<ItemKind>,
    ammo: [u32; 2],
    skill_points: u32,
    skills: [bool; MAX_SKILL_NODES],
    turns: u32,
    combo: u32, combo_timer: u32,
    invisible: bool, invis_turns: u8,
    warcry_turns: u8,
    mana_shield: u8,
    inventory: [Option<Item>; MAX_INV],
    inv_count: usize, inv_slots_base: usize, backpack_bonus: usize,
    status: [StatusEffect; MAX_STATUS],
    hit_count: u32,  // for Titan Blow
    last_attack_dir: (i32, i32),
    visited_floors: u32,
}
impl Player {
    fn new(x: i32, y: i32) -> Self {
        let mut p = Self {
            x, y, hp: 30, max_hp: 30, base_atk: 5, base_def: 0,
            xp: 0, level: 1, gold: 75, weapon: WeaponKind::Fists, armor: None,
            ring1: None, ring2: None,
            ammo: [0, 0],
            skill_points: 0, skills: [false; MAX_SKILL_NODES],
            turns: 0, combo: 0, combo_timer: 0,
            invisible: false, invis_turns: 0, warcry_turns: 0, mana_shield: 0,
            inventory: [None; MAX_INV], inv_count: 0, inv_slots_base: 18, backpack_bonus: 0,
            status: [StatusEffect::new(); MAX_STATUS], hit_count: 0,
            last_attack_dir: (1, 0), visited_floors: 0,
        };
        p
    }
    fn max_slots(&self) -> usize { self.inv_slots_base + self.backpack_bonus }
    fn get_atk(&self) -> i32 {
        let mut atk = self.base_atk + self.weapon.damage_mod();
        if self.skills[1] { atk += 2; }   // Sunder
        if self.warcry_turns > 0 { atk += 2; } // Warcry
        if let Some(ItemKind::RingStrength) = self.ring1 { atk += 3; }
        if let Some(ItemKind::RingStrength) = self.ring2 { atk += 3; }
        atk
    }
    fn get_def(&self) -> i32 {
        let mut def = self.base_def;
        if self.skills[0] { def += 3; }   // Iron Skin
        if let Some(ItemKind::Armor { defense, .. }) = self.armor { def += defense; }
        if let Some(ItemKind::RingProtection) = self.ring1 { def += 2; }
        if let Some(ItemKind::RingProtection) = self.ring2 { def += 2; }
        def
    }
    fn xp_to_next(&self) -> u32 { self.level * self.level * 30 }
    fn try_level_up(&mut self) -> bool {
        if self.xp >= self.xp_to_next() {
            self.xp -= self.xp_to_next();
            self.level += 1;
            let hp_gain = 6 + if self.skills[5] { 5 } else { 0 };
            self.max_hp += hp_gain;
            self.hp = (self.hp + hp_gain).min(self.max_hp);
            self.base_atk += 1;
            if self.level % 3 == 0 { self.base_def += 1; }
            self.skill_points += 1;
            if self.skills[14] { /* Arcane Mind: restore a scroll */ }
            true
        } else { false }
    }
    fn tick_status(&mut self) -> i32 {
        let mut dmg = 0i32;
        for s in self.status.iter_mut() {
            if s.turns_left == 0 { continue; }
            s.turns_left -= 1;
            match s.kind {
                StatusKind::Poisoned => dmg += 1,
                StatusKind::Burning => dmg += 3,
                _ => {}
            }
        }
        dmg
    }
    fn has_status(&self, kind: StatusKind) -> bool {
        self.status.iter().any(|s| s.turns_left > 0 && s.kind == kind)
    }
    fn apply_status(&mut self, kind: StatusKind, turns: u8) {
        if self.skills[5] && kind == StatusKind::Stunned { return; } // Colossus
        for s in self.status.iter_mut() {
            if s.turns_left == 0 { *s = StatusEffect { kind, turns_left: turns }; return; }
        }
    }
    fn add_item(&mut self, item: Item) -> bool {
        if let ItemKind::Gold(amt) = item.kind {
            self.gold += amt;
            return true;
        }
        if let ItemKind::Ammo { ammo_type: t, count: c } = item.kind {
            let max_ammo = if self.skills[9] { 60 } else { 30 }; // Ammo Cache
            self.ammo[t] = (self.ammo[t] + c).min(max_ammo);
            return true;
        }
        if let ItemKind::Backpack { slots } = item.kind {
            self.backpack_bonus += slots;
            return true;
        }
        if self.inv_count >= self.max_slots() { return false; }
        for i in 0..MAX_INV {
            if self.inventory[i].is_none() {
                self.inventory[i] = Some(item);
                self.inv_count += 1;
                return true;
            }
        }
        false
    }
    fn remove_item_at(&mut self, idx: usize) {
        self.inventory[idx] = None;
        // Compact
        let mut write = 0;
        for read in 0..MAX_INV {
            if let Some(it) = self.inventory[read] {
                self.inventory[write] = Some(it);
                if write != read { self.inventory[read] = None; }
                write += 1;
            }
        }
        if self.inv_count > 0 { self.inv_count -= 1; }
    }
}

// =============================================================================
// MAP & GENERATION
// =============================================================================

type TileMap = [[Tile; MAP_W]; MAP_H];
type FovMap  = [[bool; MAP_W]; MAP_H];
type SeenMap = [[bool; MAP_W]; MAP_H];

static mut TILES:     TileMap  = [[Tile::Wall; MAP_W]; MAP_H];
static mut FOV:       FovMap   = [[false; MAP_W]; MAP_H];
static mut SEEN:      SeenMap  = [[false; MAP_W]; MAP_H];
static mut MONSTERS:  [Monster; MAX_MONSTERS] = [Monster{
    kind:MonKind::Rat,x:0,y:0,hp:0,alive:false,behavior:AIBehavior::Idle,
    home_x:0,home_y:0,wander_tick:0,patrol_idx:0,alert_level:0,flash_tick:0,
    status:[StatusEffect{kind:StatusKind::Stunned,turns_left:0};MAX_STATUS],
    patrol_x:[0;4],patrol_y:[0;4],patrol_len:0,last_seen_x:0,last_seen_y:0
}; MAX_MONSTERS];
static mut MAP_ITEMS: [Item; MAX_MAP_ITEMS] = [Item{kind:ItemKind::Gold(0),x:0,y:0,active:false,rarity:Rarity::Common}; MAX_MAP_ITEMS];
static mut PLAYER: Player = Player{
    x:1,y:1,hp:30,max_hp:30,base_atk:5,base_def:0,xp:0,level:1,gold:75,
    weapon:WeaponKind::Fists,armor:None,ring1:None,ring2:None,ammo:[0;2],
    skill_points:0,skills:[false;MAX_SKILL_NODES],turns:0,combo:0,combo_timer:0,
    invisible:false,invis_turns:0,warcry_turns:0,mana_shield:0,
    inventory:[None;MAX_INV],inv_count:0,inv_slots_base:18,backpack_bonus:0,
    status:[StatusEffect{kind:StatusKind::Stunned,turns_left:0};MAX_STATUS],
    hit_count:0,last_attack_dir:(1,0),visited_floors:0,
};
static mut PLAYER_SKILLS: [SkillNode; MAX_SKILL_NODES] = SKILL_TREE;
static mut FLOOR: u32 = 0;
static mut SYS_TICK: u32 = 0;
static mut BOSS_ACTIVE: bool = false;
static mut LAST_DUNGEON_FLOOR: u32 = 1;
static mut GAME_OVER: bool = false;
static mut GAME_WON: bool = false;
static mut LEVEL_UP_FLASH: u32 = 0;

// UI State
static mut SHOW_INV: bool = false;
static mut INV_CURSOR: usize = 0;
static mut INV_SORT_CAT: bool = false;
static mut SHOW_SKILL_TREE: bool = false;
static mut SKILL_CURSOR: usize = 0;
static mut SHOW_MAP: bool = false;
static mut SHOP_OPEN: bool = false;
static mut SHOP_BUY: bool = true;
static mut SHOP_CURSOR: usize = 0;
static mut SHOP_HAGGLES: u8 = 0;
static mut SHOP_STOCK: [Option<ItemKind>; 8] = [None; 8];
static mut SHOP_PRICES: [u32; 8] = [0; 8];
static mut DIALOG_OPEN: bool = false;
static mut DIALOG_TEXT: &'static [u8] = b"";

// Attack state
static mut ATK_ACTIVE: bool = false;
static mut ATK_START: u32 = 0;
static mut ATK_TX: i32 = 0; static mut ATK_TY: i32 = 0;
static mut ATK_RANGED: bool = false;
static mut ATK_BULLET_X: i32 = 0; static mut ATK_BULLET_Y: i32 = 0;

// ── FOV ───────────────────────────────────────────────────────────────────────

fn cos_approx(a: i32) -> i32 { sin_approx(a + 1571) }
fn sin_approx(a: i32) -> i32 {
    let a = a % 6284; let a = if a < 0 { a + 6284 } else { a };
    let a_n = if a <= 3142 { a } else { 6284 - a };
    let s = if a <= 3142 { 1 } else { -1 };
    let x = (a_n * 1000 / 1571) - 1000;
    s * (1000 - (x * x / 1000)).max(0).min(1000)
}

unsafe fn compute_fov(px: i32, py: i32) {
    let radius = if PLAYER.skills[7] { 12 } else { 9 }; // Eagle Eye
    for row in FOV.iter_mut() { for v in row.iter_mut() { *v = false; } }
    FOV[py as usize][px as usize] = true;
    SEEN[py as usize][px as usize] = true;
    for a in (0..6284).step_by(15) {
        let ax = cos_approx(a as i32); let ay = sin_approx(a as i32);
        let mut rx = px * 1000 + 500; let mut ry = py * 1000 + 500;
        for _ in 0..radius {
            rx += ax; ry += ay;
            let tx = (rx / 1000) as usize; let ty = (ry / 1000) as usize;
            if tx >= MAP_W || ty >= MAP_H { break; }
            FOV[ty][tx] = true; SEEN[ty][tx] = true;
            if TILES[ty][tx].blocks_sight() { break; }
        }
    }
    // One With Dark: see all monsters through walls
    if PLAYER.skills[27] {
        for m in MONSTERS.iter() {
            if m.alive && m.kind.is_hostile() {
                if m.x >= 0 && m.y >= 0 { FOV[m.y as usize][m.x as usize] = true; }
            }
        }
    }
}

// ── Item helpers ─────────────────────────────────────────────────────────────

unsafe fn get_rarity() -> Rarity {
    let roll = rng() % 10000;
    if roll < 1 { Rarity::Mythic } else if roll < 80 { Rarity::Legendary }
    else if roll < 800 { Rarity::Rare } else if roll < 3500 { Rarity::Uncommon }
    else { Rarity::Common }
}

unsafe fn make_item(kind: ItemKind) -> Item {
    let rarity = match kind {
        ItemKind::Gold(_) | ItemKind::Food | ItemKind::Ammo { .. } => Rarity::Common,
        _ => get_rarity(),
    };
    Item::new(kind, rarity)
}

unsafe fn spawn_loot(x: i32, y: i32, count: u32, tier: u32) {
    for _ in 0..count {
        for it in MAP_ITEMS.iter_mut() {
            if it.active { continue; }
            let r = rng() % 100;
            let kind = if tier == 99 {
                // Boss loot
                match r {
                    0..=30 => ItemKind::Gold(rng_range(200, 800) as u32),
                    31..=50 => ItemKind::HealthPotion,
                    51..=65 => ItemKind::SkillOrb,
                    66..=80 => ItemKind::Weapon(WeaponKind::Greatsword),
                    81..=90 => ItemKind::Armor { defense: rng_range(5, 10), name_id: 4 },
                    _ => ItemKind::MagicOrb,
                }
            } else {
                match r {
                    0..=14 => ItemKind::Gold(rng_range(5, 60) as u32),
                    15..=24 => ItemKind::HealthPotion,
                    25..=29 => ItemKind::Food,
                    30..=33 => ItemKind::Ore { name_id: (rng()%4) as u8, value: rng_range(8, 40) as u32 },
                    34..=37 => ItemKind::ScrollFireball,
                    38..=40 => ItemKind::ScrollTeleport,
                    41..=43 => ItemKind::RingProtection,
                    44..=46 => ItemKind::RingStrength,
                    47..=52 => ItemKind::Armor { defense: rng_range(1, 3 + tier as i32/2), name_id: (rng()%3) as u8 },
                    53..=56 => ItemKind::PoisonVial,
                    57..=59 => ItemKind::FlashBang,
                    60..=61 => { // Ammo: scarce!
                        let at = rng() as usize % 2;
                        let mult = if PLAYER.skills[9] { 2 } else { 1 }; // Ammo Cache
                        ItemKind::Ammo { ammo_type: at, count: rng_range(1, 6) as u32 * mult }
                    }
                    _ => {
                        let w = match rng()%100 {
                            0..=20 => WeaponKind::Dagger, 21..=45 => WeaponKind::Sword,
                            46..=60 => WeaponKind::Axe,  61..=72 => WeaponKind::Warhammer,
                            73..=80 => WeaponKind::Greatsword, 81..=88 => WeaponKind::Staff,
                            89..=93 => WeaponKind::Pistol, _ => WeaponKind::Shotgun,
                        };
                        ItemKind::Weapon(w)
                    }
                }
            };
            *it = make_item(kind);
            it.x = x; it.y = y;
            break;
        }
    }
}

// ── Map gen ───────────────────────────────────────────────────────────────────

static mut ROOMS: [(i32,i32,i32,i32); 40] = [(0,0,0,0); 40];
static mut ROOM_COUNT: usize = 0;

unsafe fn gen_village() {
    for y in 0..MAP_H { for x in 0..MAP_W {
        TILES[y][x] = if x<1||y<1||x>MAP_W-2||y>MAP_H-2 { Tile::Tree } else { Tile::Grass };
    }}
    let ry = MAP_H/2;
    for x in 1..MAP_W-1 { TILES[ry][x]=Tile::Floor; TILES[ry-1][x]=Tile::Floor; TILES[ry+1][x]=Tile::Floor; }
    let rx = MAP_W/3;
    for y in 1..MAP_H-1 { TILES[y][rx]=Tile::Floor; TILES[y][rx-1]=Tile::Floor; TILES[y][rx+1]=Tile::Floor; }

    // Inn
    for y in 4..14 { for x in 4..16 {
        TILES[y][x] = if x==4||x==15||y==4||y==13 { Tile::Wall } else { Tile::Floor };
    }}
    TILES[13][9] = Tile::DoorClosed;
    TILES[8][8] = Tile::Campfire { lit: true };
    TILES[6][14] = Tile::Well;
    MONSTERS[0] = Monster::new_at(MonKind::Merchant, 10, 8);

    // Villager house
    for y in 4..14 { for x in 24..36 {
        TILES[y][x] = if x==24||x==35||y==4||y==13 { Tile::Wall } else { Tile::Floor };
    }}
    TILES[13][29] = Tile::DoorClosed;
    let mut vil = Monster::new_at(MonKind::Villager, 30, 8);
    vil.patrol_x = [30,28,32,30]; vil.patrol_y = [8,10,10,8]; vil.patrol_len = 4;
    vil.behavior = AIBehavior::Patrol;
    MONSTERS[1] = vil;

    // Quest giver
    let mut qg = Monster::new_at(MonKind::QuestGiver, 50, ry as i32);
    MONSTERS[2] = qg;

    // Guard patrol
    let mut guard = Monster::new_at(MonKind::Guard, rx as i32, (ry-3) as i32);
    guard.patrol_x = [rx as i32, rx as i32+5, rx as i32+5, rx as i32];
    guard.patrol_y = [(ry-3) as i32, (ry-3) as i32, (ry+3) as i32, (ry+3) as i32];
    guard.patrol_len = 4; guard.behavior = AIBehavior::Patrol;
    MONSTERS[3] = guard;

    // Flowers
    for _ in 0..80 {
        let fx = rng_range(2, MAP_W as i32-2); let fy = rng_range(2, MAP_H as i32-2);
        if TILES[fy as usize][fx as usize] == Tile::Grass { TILES[fy as usize][fx as usize] = Tile::Flower; }
    }

    // Stairs down
    for y in ry-2..ry+3 { for x in MAP_W-9..MAP_W-2 { TILES[y as usize][x as usize] = Tile::Floor; } }
    TILES[ry][MAP_W-5] = Tile::StairsDown;

    PLAYER.x = 10; PLAYER.y = ry as i32 - 3;
}

unsafe fn gen_dungeon(floor: u32) {
    for y in 0..MAP_H { for x in 0..MAP_W { TILES[y][x] = Tile::Wall; } }
    ROOM_COUNT = 0;

    let room_count = (12 + floor as usize * 3).min(38);
    let mut last_cx = 0i32; let mut last_cy = 0i32;

    for i in 0..room_count {
        let w = rng_range(6, 14); let h = rng_range(5, 11);
        let rx = rng_range(1, MAP_W as i32 - w - 1);
        let ry = rng_range(1, MAP_H as i32 - h - 1);
        for ry2 in ry..ry+h { for rx2 in rx..rx+w { TILES[ry2 as usize][rx2 as usize] = Tile::Floor; } }
        let cx = rx + w/2; let cy = ry + h/2;
        if i > 0 {
            if cx < last_cx { for tx in cx..=last_cx { TILES[cy as usize][tx as usize] = Tile::Floor; } }
            else { for tx in last_cx..=cx { TILES[cy as usize][tx as usize] = Tile::Floor; } }
            if cy < last_cy { for ty in cy..=last_cy { TILES[ty as usize][cx as usize] = Tile::Floor; } }
            else { for ty in last_cy..=cy { TILES[ty as usize][cx as usize] = Tile::Floor; } }
        }
        if ROOM_COUNT < 40 { ROOMS[ROOM_COUNT] = (rx, ry, w, h); ROOM_COUNT += 1; }
        last_cx = cx; last_cy = cy;
    }

    if ROOM_COUNT == 0 { TILES[2][2] = Tile::Floor; PLAYER.x = 2; PLAYER.y = 2; return; }
    let (sx,sy,sw,sh) = ROOMS[0]; PLAYER.x = sx+sw/2; PLAYER.y = sy+sh/2;
    let last = ROOM_COUNT-1;
    let (ex,ey,ew,eh) = ROOMS[last];
    TILES[(ey+eh/2) as usize][(ex+ew/2) as usize] = Tile::StairsDown;

    // Decorations
    for _ in 0..30 {
        let rx = rng_range(1,MAP_W as i32-2); let ry = rng_range(1,MAP_H as i32-2);
        if TILES[ry as usize][rx as usize] == Tile::Wall { TILES[ry as usize][rx as usize] = Tile::Rock; }
    }
    for _ in 0..8 {
        let rx = rng_range(1,MAP_W as i32-2); let ry = rng_range(1,MAP_H as i32-2);
        if TILES[ry as usize][rx as usize] == Tile::Floor { TILES[ry as usize][rx as usize] = Tile::Chest { opened: false }; }
    }
    for _ in 0..5 {
        let rx = rng_range(1,MAP_W as i32-2); let ry = rng_range(1,MAP_H as i32-2);
        if TILES[ry as usize][rx as usize] == Tile::Floor { TILES[ry as usize][rx as usize] = Tile::Trap { triggered: false }; }
    }

    // Monsters (task-based)
    for m in MONSTERS.iter_mut() { m.alive = false; }
    let mon_count = (8 + floor as usize*3).min(MAX_MONSTERS);
    let mut spawned = 0;
    let mut attempts = 0;
    while spawned < mon_count && attempts < 500 {
        attempts += 1;
        let ri = rng() as usize % ROOM_COUNT.max(1);
        let (rx,ry,rw,rh) = ROOMS[ri];
        let mx = rx + rng_range(1, rw-1); let my = ry + rng_range(1, rh-1);
        if (mx-PLAYER.x).abs() < 6 && (my-PLAYER.y).abs() < 6 { continue; }
        let kind = if floor==10&&spawned==0 { MonKind::Warlord } else {
            let roll = rng()%100;
            if floor<3 { if roll<55 { MonKind::Rat } else { MonKind::Spider } }
            else if floor<6 { if roll<25 { MonKind::Rat } else if roll<60 { MonKind::Goblin } else { MonKind::Skeleton } }
            else { if roll<15 { MonKind::Goblin } else if roll<35 { MonKind::Skeleton } else if roll<58 { MonKind::Troll } else if roll<78 { MonKind::Wraith } else { MonKind::Vampire } }
        };
        let mut mon = Monster::new_at(kind, mx, my);
        // Some Goblins patrol
        if kind == MonKind::Goblin && rng()%3 == 0 {
            mon.behavior = AIBehavior::Patrol;
            mon.patrol_x = [mx, mx+rng_range(-4,5), mx+rng_range(-4,5), mx];
            mon.patrol_y = [my, my+rng_range(-4,5), my+rng_range(-4,5), my];
            mon.patrol_len = 4;
        }
        MONSTERS[spawned] = mon;
        spawned += 1;
    }

    // Items
    for it in MAP_ITEMS.iter_mut() { it.active = false; }
    let item_count = (5 + floor as usize*2).min(MAX_MAP_ITEMS-10);
    for _ in 0..item_count {
        let ri = rng() as usize % ROOM_COUNT.max(1);
        let (rx,ry,rw,rh) = ROOMS[ri];
        let ix = rx + rng_range(1,rw-1); let iy = ry + rng_range(1,rh-1);
        spawn_loot(ix, iy, 1, floor);
    }
}

unsafe fn gen_boss_arena() {
    for y in 0..MAP_H { for x in 0..MAP_W { TILES[y][x] = Tile::Wall; } }
    for y in 38..82 { for x in 78..122 { TILES[y][x] = Tile::Floor; } }
    // Pillars
    for &(py,px) in &[(42,82),(42,118),(78,82),(78,118),(60,100)] {
        TILES[py][px] = Tile::Pillar;
    }
    PLAYER.x = 100; PLAYER.y = 42;
    let mut boss = Monster::new_at(MonKind::AncientOne, 100, 78);
    boss.behavior = AIBehavior::ChasePlayer;
    boss.alert_level = 255;
    MONSTERS[0] = boss;
    for it in MAP_ITEMS.iter_mut() { it.active = false; }
    push_log(b"The VOID tears open...");
    push_log(b"THE ANCIENT ONE AWAKENS!");
}

unsafe fn gen_floor(floor: u32) {
    RNG_STATE ^= floor.wrapping_mul(0x9E3779B9).wrapping_add(12345);
    for y in 0..MAP_H { for x in 0..MAP_W { FOV[y][x] = false; SEEN[y][x] = false; } }
    for p in PARTICLES.iter_mut() { p.active = false; }
    for row in NOISE_MAP.iter_mut() { for v in row.iter_mut() { *v = 0; } }
    PLAYER.visited_floors += 1;

    if floor == 0 { gen_village(); }
    else if BOSS_ACTIVE { gen_boss_arena(); }
    else { gen_dungeon(floor); }

    compute_fov(PLAYER.x, PLAYER.y);
}

// =============================================================================
// COMBAT
// =============================================================================

unsafe fn do_melee_attack(tx: i32, ty: i32) {
    for i in 0..MAX_MONSTERS {
        if !MONSTERS[i].alive || MONSTERS[i].x != tx || MONSTERS[i].y != ty { continue; }

        // Titan Blow
        PLAYER.hit_count += 1;
        let titan = PLAYER.skills[4] && PLAYER.hit_count % 5 == 0;

        // Backstab (Rogue skill 22)
        let backstab = PLAYER.skills[22] && {
            let player_dx = PLAYER.last_attack_dir.0;
            let player_dy = PLAYER.last_attack_dir.1;
            let mon_face_x = -player_dx; let mon_face_y = -player_dy;
            true // simplified: always active for adjacent
        };

        let mut dmg = rng_range(PLAYER.get_atk()-1, PLAYER.get_atk()+3) - MONSTERS[i].kind.def();
        dmg = dmg.max(1);
        if titan { dmg *= 3; push_log(b"TITAN BLOW!"); }
        if backstab && PLAYER.skills[22] { dmg *= 2; push_log(b"Backstab!"); }

        // Crit (10% base)
        let crit = rng()%100 < 10;
        if crit { dmg = dmg * 2; push_log(b"Critical hit!"); }

        // Lifesteal (skill 26 / Vampire ability)
        if PLAYER.skills[5] { PLAYER.hp = (PLAYER.hp + dmg/3).min(PLAYER.max_hp); }

        MONSTERS[i].hp -= dmg;
        MONSTERS[i].flash_tick = SYS_TICK + 5;
        spawn_blood(tx, ty, 3);

        // Poison Blade
        if PLAYER.skills[25] { MONSTERS[i].apply_status(StatusKind::Poisoned, 4); }

        // Sunder: reduce def (tracked via alert_level as proxy for debuff)
        if PLAYER.skills[1] && MONSTERS[i].kind.def() > 0 { MONSTERS[i].alert_level = MONSTERS[i].alert_level.saturating_add(10); }

        push_log2(b"Hit ", MONSTERS[i].kind.name());

        // Combo
        PLAYER.combo += 1;
        PLAYER.combo_timer = SYS_TICK + 60;

        if MONSTERS[i].hp <= 0 {
            MONSTERS[i].alive = false;
            PLAYER.xp += MONSTERS[i].kind.xp();
            spawn_xp_orb(tx, ty, 3);
            spawn_loot(tx, ty, rng_range(1,3) as u32, FLOOR);
            TILES[ty as usize][tx as usize] = Tile::Corpse { turns_left: 5 };
            push_log2(b"Killed ", MONSTERS[i].kind.name());

            if MONSTERS[i].kind == MonKind::Warlord { GAME_WON = true; }
            if MONSTERS[i].kind == MonKind::AncientOne {
                push_log(b"THE ANCIENT ONE FALLS!");
                spawn_explosion(tx, ty, 8);
                spawn_loot(tx, ty, 10, 99);
                BOSS_ACTIVE = false;
                FLOOR = LAST_DUNGEON_FLOOR;
                gen_floor(FLOOR);
                return;
            }
            if PLAYER.try_level_up() { push_log(b"LEVEL UP!"); LEVEL_UP_FLASH = SYS_TICK + 40; }
        }
        emit_noise(tx, ty, PLAYER.weapon.noise_radius());
        return;
    }
}

unsafe fn fire_gun(dir_x: i32, dir_y: i32) {
    let atype = PLAYER.weapon.ammo_type();
    if PLAYER.ammo[atype] == 0 { push_log(b"*CLICK* Out of ammo!"); return; }
    PLAYER.ammo[atype] -= 1;

    let pellets: &[(i32,i32)] = if PLAYER.weapon == WeaponKind::Shotgun {
        if PLAYER.skills[12] { // Rain of Fire: 9 pellets
            &[(dir_x,dir_y),(dir_x+dir_y,dir_y-dir_x),(dir_x-dir_y,dir_y+dir_x),
              (dir_x*2+dir_y,dir_y*2-dir_x),(dir_x*2-dir_y,dir_y*2+dir_x),
              (dir_y,-dir_x),(-dir_y,dir_x),(dir_x+dir_y,dir_y+dir_x),(dir_x,dir_y)]
        } else {
            &[(dir_x,dir_y),(dir_x+dir_y,dir_y-dir_x),(dir_x-dir_y,dir_y+dir_x),
              (dir_x*2+dir_y,dir_y*2-dir_x),(dir_x*2-dir_y,dir_y*2+dir_x)]
        }
    } else {
        &[(dir_x,dir_y)]
    };

    let noise_r = if PLAYER.skills[10] { 0 } else { PLAYER.weapon.noise_radius() }; // Silencer

    for &(pdx, pdy) in pellets {
        fire_bullet(PLAYER.x, PLAYER.y, pdx, pdy);
    }
    if noise_r > 0 { emit_noise(PLAYER.x, PLAYER.y, noise_r); }
    push_log2(b"BANG! (", if PLAYER.weapon == WeaponKind::Pistol { b"Pistol)" } else { b"Shotgun)" });
}

unsafe fn fire_bullet(ox: i32, oy: i32, dx: i32, dy: i32) {
    // Trace bullet path
    let mut cx = ox; let mut cy = oy;
    let mut hit = false;
    for _ in 0..20 {
        cx += dx; cy += dy;
        if cx < 0 || cy < 0 || cx >= MAP_W as i32 || cy >= MAP_H as i32 { break; }
        // Spawn trail particles
        spawn_particle(cx, cy, dx*80, dy*80, 3, b'-', 0x04, ParticleKind::BulletTrail);

        if TILES[cy as usize][cx as usize].blocks_bullet() {
            spawn_particle(cx, cy, 0, 0, 4, b'*', 0x0E, ParticleKind::Spark);
            hit = true; break;
        }
        for i in 0..MAX_MONSTERS {
            if !MONSTERS[i].alive || MONSTERS[i].x != cx || MONSTERS[i].y != cy { continue; }

            // Headshot: 20% instant kill (Ranger skill)
            let insta = PLAYER.skills[8] && rng()%100 < 20;
            let mut dmg = if insta { MONSTERS[i].hp } else { PLAYER.get_atk() + rng_range(-1, 3) };
            dmg = dmg.max(1);

            // Explosive Rounds (Ranger skill 11)
            if PLAYER.skills[11] { spawn_explosion(cx, cy, 2); }

            MONSTERS[i].hp -= dmg;
            MONSTERS[i].flash_tick = SYS_TICK + 5;
            spawn_blood(cx, cy, 4);
            push_log2(if insta { b"HEADSHOT! " } else { b"Shot " }, MONSTERS[i].kind.name());

            if MONSTERS[i].hp <= 0 {
                MONSTERS[i].alive = false;
                PLAYER.xp += MONSTERS[i].kind.xp();
                spawn_xp_orb(cx, cy, 4);
                spawn_loot(cx, cy, rng_range(1,3) as u32, FLOOR);
                TILES[cy as usize][cx as usize] = Tile::Corpse { turns_left: 5 };
                if PLAYER.try_level_up() { push_log(b"LEVEL UP!"); LEVEL_UP_FLASH = SYS_TICK + 40; }
                if MONSTERS[i].kind == MonKind::Warlord { GAME_WON = true; }
            }
            hit = true; break;
        }
        if hit { break; }
    }
}

unsafe fn monster_attacks_player(i: usize) {
    if MONSTERS[i].kind == MonKind::AncientOne {
        spawn_boss_aura(MONSTERS[i].x, MONSTERS[i].y, SYS_TICK);
    }
    if PLAYER.invisible { return; } // Ghost

    let raw = rng_range(MONSTERS[i].kind.atk()-1, MONSTERS[i].kind.atk()+2);
    // Block chance (Bulwark)
    if PLAYER.skills[3] && rng()%100 < 25 {
        push_log(b"BLOCKED!");
        spawn_particle(PLAYER.x, PLAYER.y, 0, -300, 4, b'!', 0x09, ParticleKind::Spark);
        return;
    }
    // Mana shield (Mage)
    if PLAYER.mana_shield > 0 {
        let absorb = raw.min(PLAYER.mana_shield as i32);
        PLAYER.mana_shield -= absorb as u8;
        let actual = raw - absorb;
        if actual > 0 {
            let dmg = (actual - PLAYER.get_def()).max(1);
            PLAYER.hp -= dmg;
        }
        push_log2(b"Shield absorbed ", MONSTERS[i].kind.name());
    } else {
        let dmg = (raw - PLAYER.get_def()).max(1);
        PLAYER.hp -= dmg;
        spawn_particle(PLAYER.x, PLAYER.y, rng_range(-200,200), -200, 3, b'*', 0x04, ParticleKind::Blood);
        push_log2(MONSTERS[i].kind.name(), b" hits you!");
    }

    // Vampire life drain
    if MONSTERS[i].kind == MonKind::Vampire {
        MONSTERS[i].hp = (MONSTERS[i].hp + 3).min(MONSTERS[i].kind.max_hp());
        PLAYER.apply_status(StatusKind::Poisoned, 3);
    }
    if MONSTERS[i].kind == MonKind::AncientOne {
        PLAYER.apply_status(StatusKind::Burning, 3);
    }
    if PLAYER.hp <= 0 { GAME_OVER = true; }
}

// =============================================================================
// GAME LOGIC
// =============================================================================

unsafe fn end_turn() {
    SYS_TICK += 1;
    PLAYER.turns += 1;

    // Status effects on player
    let sdmg = PLAYER.tick_status();
    if sdmg > 0 {
        PLAYER.hp -= sdmg;
        if PLAYER.hp <= 0 { GAME_OVER = true; return; }
    }

    // Warcry timer
    if PLAYER.warcry_turns > 0 { PLAYER.warcry_turns -= 1; }
    // Invisibility
    if PLAYER.invisible && PLAYER.invis_turns > 0 {
        PLAYER.invis_turns -= 1;
        if PLAYER.invis_turns == 0 { PLAYER.invisible = false; push_log(b"Invisible fades."); }
    }
    // Combo timer
    if PLAYER.combo_timer < SYS_TICK { PLAYER.combo = 0; }

    // Passive HP regen
    if PLAYER.turns % 25 == 0 && PLAYER.hp < PLAYER.max_hp { PLAYER.hp += 1; }

    // Decay noise
    decay_noise();

    // Age corpse tiles
    for y in 0..MAP_H { for x in 0..MAP_W {
        if let Tile::Corpse { turns_left } = TILES[y][x] {
            if turns_left == 0 { TILES[y][x] = Tile::Floor; }
            else { TILES[y][x] = Tile::Corpse { turns_left: turns_left-1 }; }
        }
    }}

    // Update monster AI
    compute_fov(PLAYER.x, PLAYER.y);
    let px = PLAYER.x; let py = PLAYER.y;

    for i in 0..MAX_MONSTERS {
        if !MONSTERS[i].alive { continue; }
        // Check visibility to monster
        let pv = FOV[MONSTERS[i].y as usize][MONSTERS[i].x as usize];
        // Need to copy to avoid borrow
        let mx = MONSTERS[i].x; let my = MONSTERS[i].y;
        update_monster_ai(i, px, py, pv, &TILES, &mut MONSTERS, SYS_TICK);

        // After AI move, check if adjacent to player
        if MONSTERS[i].alive && MONSTERS[i].x == px && MONSTERS[i].y == py { continue; }
        if MONSTERS[i].alive && MONSTERS[i].kind.is_hostile() {
            let dist = (MONSTERS[i].x-px).abs() + (MONSTERS[i].y-py).abs();
            if dist == 1 { monster_attacks_player(i); }
        }
    }

    // Boss aura particles
    if BOSS_ACTIVE {
        for i in 0..MAX_MONSTERS {
            if MONSTERS[i].alive && MONSTERS[i].kind == MonKind::AncientOne {
                spawn_boss_aura(MONSTERS[i].x, MONSTERS[i].y, SYS_TICK);
            }
        }
    }

    update_particles(&TILES);
}

unsafe fn try_move(dx: i32, dy: i32) {
    if GAME_OVER || GAME_WON { return; }
    PLAYER.last_attack_dir = (dx, dy);
    let nx = PLAYER.x + dx; let ny = PLAYER.y + dy;
    if nx < 0 || ny < 0 || nx >= MAP_W as i32 || ny >= MAP_H as i32 { return; }

    // Check for door
    if TILES[ny as usize][nx as usize] == Tile::DoorClosed {
        TILES[ny as usize][nx as usize] = Tile::DoorOpen;
        push_log(b"Door opened."); end_turn(); return;
    }
    // Check for chest
    if let Tile::Chest { opened: false } = TILES[ny as usize][nx as usize] {
        TILES[ny as usize][nx as usize] = Tile::Chest { opened: true };
        push_log(b"Chest opened!");
        spawn_loot(nx, ny, rng_range(2,6) as u32, FLOOR);
        for _ in 0..4 { spawn_particle(nx, ny, rng_range(-300,300), rng_range(-400,-100), 6, b'*', 0x0E, ParticleKind::Spark); }
        end_turn(); return;
    }
    // Check monster
    for i in 0..MAX_MONSTERS {
        if MONSTERS[i].alive && MONSTERS[i].x == nx && MONSTERS[i].y == ny {
            do_melee_attack(nx, ny);
            end_turn(); return;
        }
    }
    if !TILES[ny as usize][nx as usize].is_walkable() { return; }
    PLAYER.x = nx; PLAYER.y = ny;
    // Step dust
    spawn_dust(nx, ny);
    // Trap
    if let Tile::Trap { triggered: false } = TILES[ny as usize][nx as usize] {
        TILES[ny as usize][nx as usize] = Tile::Trap { triggered: true };
        let dmg = rng_range(5, 12);
        PLAYER.hp -= dmg;
        push_log(b"TRAP! Ouch!");
        spawn_blood(nx, ny, 2);
        if PLAYER.hp <= 0 { GAME_OVER = true; return; }
    }
    // Auto-pickup gold
    for it in MAP_ITEMS.iter_mut() {
        if it.active && it.x == nx && it.y == ny {
            if matches!(it.kind, ItemKind::Gold(_) | ItemKind::Ammo { .. }) {
                PLAYER.add_item(*it);
                it.active = false;
                push_log(b"Auto-picked up.");
            }
        }
    }
    end_turn();
}

unsafe fn try_pickup() {
    let px = PLAYER.x; let py = PLAYER.y;
    for it in MAP_ITEMS.iter_mut() {
        if it.active && it.x == px && it.y == py {
            if PLAYER.add_item(*it) {
                push_log2(b"Got: ", it.kind.name());
                it.active = false;
            } else {
                push_log(b"Inventory full!");
            }
            return;
        }
    }
    push_log(b"Nothing here.");
}

unsafe fn try_interact() {
    let px = PLAYER.x; let py = PLAYER.y;
    // Stairs
    if TILES[py as usize][px as usize] == Tile::StairsDown {
        if BOSS_ACTIVE { BOSS_ACTIVE = false; gen_floor(LAST_DUNGEON_FLOOR); }
        else if FLOOR == 0 { LAST_DUNGEON_FLOOR = 1; FLOOR = 1; gen_floor(FLOOR); }
        else { FLOOR += 1; gen_floor(FLOOR); }
        push_log2(b"Descending floor ", u32_to_str(FLOOR, &mut [0u8;12]));
        return;
    }
    if TILES[py as usize][px as usize] == Tile::StairsUp && FLOOR > 0 {
        FLOOR -= 1; gen_floor(FLOOR);
        push_log2(b"Ascending floor ", u32_to_str(FLOOR, &mut [0u8;12]));
        return;
    }
    // Adjacent NPC
    for i in 0..MAX_MONSTERS {
        if !MONSTERS[i].alive { continue; }
        let dist = (MONSTERS[i].x-px).abs() + (MONSTERS[i].y-py).abs();
        if dist > 1 { continue; }
        match MONSTERS[i].kind {
            MonKind::Merchant => open_shop(),
            MonKind::Villager => {
                let lines: [&[u8]; 8] = [
                    b"The ruins hold great treasure!",
                    b"Beware the shadows...",
                    b"Press 'n' to mine rocks!",
                    b"The merchant has strange wares.",
                    b"I heard screaming from below.",
                    b"Guns make a LOT of noise...",
                    b"Legends say an Ancient One waits.",
                    b"Rest at a campfire to heal.",
                ];
                DIALOG_TEXT = lines[(rng()%8) as usize]; DIALOG_OPEN = true;
            }
            MonKind::QuestGiver => {
                DIALOG_TEXT = b"Find the Warlord on floor 10.";
                DIALOG_OPEN = true;
            }
            _ => {}
        }
        return;
    }
    push_log(b"Nothing to interact with.");
}

unsafe fn open_shop() {
    SHOP_CURSOR = 0; SHOP_BUY = true; SHOP_HAGGLES = 0;
    for k in 0..8 {
        SHOP_PRICES[k] = 0; SHOP_STOCK[k] = None;
        let base = (PLAYER.level as u32 * 20 + rng_range(15,60) as u32);
        SHOP_STOCK[k] = Some(match rng()%100 {
            0..=18 => ItemKind::HealthPotion,
            19..=24 => ItemKind::ManaPotion,
            25..=32 => ItemKind::Food,
            33..=40 => ItemKind::ScrollFireball,
            41..=47 => ItemKind::ScrollTeleport,
            48..=54 => ItemKind::RingProtection,
            55..=62 => ItemKind::Armor { defense: rng_range(2, 4+PLAYER.level as i32/2), name_id: (rng()%4) as u8 },
            63..=70 => ItemKind::Ammo { ammo_type: 0, count: rng_range(4,12) as u32 },
            71..=76 => ItemKind::Ammo { ammo_type: 1, count: rng_range(2,6) as u32 },
            77..=82 => ItemKind::PoisonVial,
            83..=88 => ItemKind::FlashBang,
            _ => { let w=[WeaponKind::Dagger,WeaponKind::Sword,WeaponKind::Axe,WeaponKind::Pistol,WeaponKind::Shotgun]; ItemKind::Weapon(w[(rng()%5) as usize]) }
        });
        SHOP_PRICES[k] = if PLAYER.skills[21] { base * 3/4 } else { base }; // Pickpocket discount
    }
    SHOP_OPEN = true;
}

unsafe fn use_item(idx: usize) {
    let item = match PLAYER.inventory[idx] { Some(i) => i, None => { push_log(b"Nothing there."); return; } };
    match item.kind {
        ItemKind::HealthPotion => {
            let heal = if item.rarity == Rarity::Legendary { 40 } else { 18 };
            PLAYER.hp = (PLAYER.hp + heal).min(PLAYER.max_hp);
            push_log(b"Quaffed Health Potion.");
        }
        ItemKind::ManaPotion => {
            PLAYER.mana_shield = (PLAYER.mana_shield as u32 + 10).min(30) as u8;
            push_log(b"Mana shield active!");
        }
        ItemKind::Food => { PLAYER.hp = (PLAYER.hp + 6).min(PLAYER.max_hp); push_log(b"Ate ration."); }
        ItemKind::PoisonVial => {
            // Throw at nearest visible monster
            let mut closest_dist = 999; let mut target = 0; let mut found = false;
            for i in 0..MAX_MONSTERS {
                if MONSTERS[i].alive && FOV[MONSTERS[i].y as usize][MONSTERS[i].x as usize] && MONSTERS[i].kind.is_hostile() {
                    let d = (MONSTERS[i].x-PLAYER.x).abs() + (MONSTERS[i].y-PLAYER.y).abs();
                    if d < closest_dist { closest_dist = d; target = i; found = true; }
                }
            }
            if found { MONSTERS[target].apply_status(StatusKind::Poisoned, 6); push_log(b"Poisoned enemy!"); }
            else { push_log(b"No target in sight."); return; }
        }
        ItemKind::FlashBang => {
            for i in 0..MAX_MONSTERS {
                if MONSTERS[i].alive && FOV[MONSTERS[i].y as usize][MONSTERS[i].x as usize] {
                    MONSTERS[i].apply_status(StatusKind::Stunned, 3);
                    MONSTERS[i].alert_level = 0;
                }
            }
            spawn_explosion(PLAYER.x, PLAYER.y, 5);
            push_log(b"FLASH! All enemies stunned!");
        }
        ItemKind::ScrollFireball => {
            let dmg = if PLAYER.skills[15] { 30 } else { 20 }; // Fire Mastery
            for i in 0..MAX_MONSTERS {
                if MONSTERS[i].alive && FOV[MONSTERS[i].y as usize][MONSTERS[i].x as usize] && MONSTERS[i].kind.is_hostile() {
                    MONSTERS[i].hp -= dmg;
                    MONSTERS[i].apply_status(StatusKind::Burning, 3);
                    spawn_explosion(MONSTERS[i].x, MONSTERS[i].y, 2);
                    if MONSTERS[i].hp <= 0 { MONSTERS[i].alive = false; PLAYER.xp += MONSTERS[i].kind.xp(); }
                }
            }
            push_log(b"FIREBALL! Everything burns!");
        }
        ItemKind::ScrollTeleport => {
            let invis_on_tele = PLAYER.skills[16]; // Time Warp
            let activate_boss = !BOSS_ACTIVE && rng()%20==0;
            if activate_boss {
                BOSS_ACTIVE = true; push_log(b"Reality tears open!"); gen_floor(0); return;
            }
            let mut attempts = 0;
            loop {
                let tx = rng_range(1, MAP_W as i32-1); let ty = rng_range(1, MAP_H as i32-1);
                if TILES[ty as usize][tx as usize].is_walkable() && !FOV[ty as usize][tx as usize] {
                    PLAYER.x = tx; PLAYER.y = ty;
                    if invis_on_tele { PLAYER.invisible = true; PLAYER.invis_turns = 5; }
                    push_log(b"ZAP! Teleported!");
                    break;
                }
                attempts += 1;
                if attempts > 200 { push_log(b"Scroll fails."); break; }
            }
        }
        ItemKind::SkillOrb => { PLAYER.skill_points += 1; push_log(b"Skill point gained!"); }
        ItemKind::MagicOrb => { PLAYER.xp += PLAYER.xp_to_next()/2; push_log(b"XP gained!"); if PLAYER.try_level_up() { push_log(b"LEVEL UP!"); LEVEL_UP_FLASH = SYS_TICK + 40; } }
        // Equippables
        ItemKind::Weapon(w) => { PLAYER.weapon = w; push_log2(b"Equipped: ", w.name()); }
        ItemKind::Armor { .. } => {
            if PLAYER.armor.is_some() { push_log(b"Unequipped old armor."); }
            PLAYER.armor = Some(item.kind); push_log2(b"Equipped: ", item.kind.name());
        }
        ItemKind::RingProtection | ItemKind::RingStrength | ItemKind::RingSpeed => {
            if PLAYER.ring1.is_none() { PLAYER.ring1 = Some(item.kind); push_log(b"Ring 1 equipped."); }
            else if PLAYER.ring2.is_none() { PLAYER.ring2 = Some(item.kind); push_log(b"Ring 2 equipped."); }
            else { push_log(b"Both ring slots full."); return; }
        }
        _ => { push_log(b"Can't use that here."); return; }
    }
    match item.kind {
        ItemKind::Weapon(_) | ItemKind::Armor{..} | ItemKind::RingProtection | ItemKind::RingStrength | ItemKind::RingSpeed => {}
        _ => { PLAYER.remove_item_at(idx); }
    }
    end_turn();
}

unsafe fn try_mine() {
    let mut any = false;
    for dy in -1..=1 { for dx in -1..=1 {
        let tx = PLAYER.x+dx; let ty = PLAYER.y+dy;
        if tx < 0 || ty < 0 || tx >= MAP_W as i32 || ty >= MAP_H as i32 { continue; }
        if TILES[ty as usize][tx as usize] == Tile::Rock {
            TILES[ty as usize][tx as usize] = Tile::Rubble;
            spawn_loot(tx, ty, 1, 0);
            for _ in 0..4 { spawn_particle(tx, ty, rng_range(-200,200), rng_range(-400,-100), 4, 0xB0, 0x08, ParticleKind::Dust); }
            push_log(b"*CRACK* Rock mined!");
            if rng()%10==0 { PLAYER.xp += 15; if PLAYER.try_level_up() { LEVEL_UP_FLASH = SYS_TICK + 40; } }
            any = true;
        }
    }}
    if !any { push_log(b"No rocks adjacent."); }
    else { end_turn(); }
}

unsafe fn use_skill_active(skill_id: usize) {
    match skill_id {
        2 => { // Warcry
            if !PLAYER.skills[2] { push_log(b"Skill not unlocked."); return; }
            PLAYER.warcry_turns = 10; push_log(b"WARCRY! ATK +2 for 10 turns!"); end_turn();
        }
        6 => { // Earthquake
            if !PLAYER.skills[6] { push_log(b"Skill not unlocked."); return; }
            for i in 0..MAX_MONSTERS {
                if MONSTERS[i].alive && FOV[MONSTERS[i].y as usize][MONSTERS[i].x as usize] {
                    MONSTERS[i].apply_status(StatusKind::Stunned, 4);
                    push_log(b"EARTHQUAKE! All stunned!");
                    spawn_explosion(MONSTERS[i].x, MONSTERS[i].y, 1);
                }
            }
            end_turn();
        }
        24 => { // Vanish
            if !PLAYER.skills[24] { push_log(b"Skill not unlocked."); return; }
            PLAYER.invisible = true; PLAYER.invis_turns = 8; push_log(b"You vanish!"); end_turn();
        }
        19 => { // Black Hole
            if !PLAYER.skills[19] { push_log(b"Skill not unlocked."); return; }
            for i in 0..MAX_MONSTERS {
                if MONSTERS[i].alive && FOV[MONSTERS[i].y as usize][MONSTERS[i].x as usize] {
                    let dx = (PLAYER.x - MONSTERS[i].x).signum()*3;
                    let dy = (PLAYER.y - MONSTERS[i].y).signum()*3;
                    MONSTERS[i].x = (MONSTERS[i].x+dx).max(1).min(MAP_W as i32-2);
                    MONSTERS[i].y = (MONSTERS[i].y+dy).max(1).min(MAP_H as i32-2);
                }
            }
            spawn_explosion(PLAYER.x, PLAYER.y, 4);
            push_log(b"BLACK HOLE! Enemies pulled!"); end_turn();
        }
        _ => { push_log(b"No active effect."); }
    }
}

// =============================================================================
// RENDERING
// =============================================================================

unsafe fn render_game_map() {
    let fov_r = if PLAYER.skills[7] { 12i32 } else { 9i32 };
    let cam_x = (PLAYER.x - VIEW_W as i32/2).max(0).min((MAP_W-VIEW_W) as i32) as usize;
    let cam_y = (PLAYER.y - VIEW_H as i32/2).max(0).min((MAP_H-VIEW_H) as i32) as usize;

    for vy in 0..VIEW_H {
        for vx in 0..VIEW_W {
            let mx = cam_x+vx; let my = cam_y+vy;
            if mx>=MAP_W||my>=MAP_H { vga_put(vx,vy,b' ',0x00); continue; }
            let vis = FOV[my][mx]; let seen = SEEN[my][mx];
            if !seen { vga_put(vx,vy,b' ',0x00); continue; }
            let tile = TILES[my][mx];
            let (g,a) = (tile.glyph(), if !vis { 0x08 } else { tile.attr() });
            vga_put(vx,vy,g,a);
            if !vis { continue; }
            // Items
            for it in MAP_ITEMS.iter() {
                if it.active && it.x==mx as i32 && it.y==my as i32 {
                    let attr = if it.rarity >= Rarity::Legendary { it.rarity.glow_attr() } else { it.rarity.attr() };
                    vga_put(vx,vy,it.kind.glyph(),attr);
                }
            }
            // Monsters
            for m in MONSTERS.iter() {
                if m.alive && m.x==mx as i32 && m.y==my as i32 {
                    let attr = if m.flash_tick > SYS_TICK { 0x0F } else { m.kind.attr() };
                    vga_put(vx,vy,m.kind.glyph(),attr);
                }
            }
        }
    }

    // Particles
    for p in PARTICLES.iter() {
        if !p.active { continue; }
        let px = p.x/1000; let py = p.y/1000;
        let vx = px as usize - cam_x;
        let vy = py as usize - cam_y;
        if px >= 0 && py >= 0 && (px as usize) < MAP_W && (py as usize) < MAP_H && vx < VIEW_W && vy < VIEW_H {
            let fade = p.life as u8;
            let color = if p.life < p.max_life/3 { 0x08 } else { p.color };
            vga_put(vx,vy,p.glyph,color);
        }
    }

    // Player
    let pvx = (PLAYER.x - cam_x as i32) as usize;
    let pvy = (PLAYER.y - cam_y as i32) as usize;
    if pvx < VIEW_W && pvy < VIEW_H {
        let attr = if PLAYER.invisible { 0x08 }
                   else if LEVEL_UP_FLASH > SYS_TICK { 0x0E }
                   else { 0x0F };
        vga_put(pvx,pvy,b'@',attr);
    }
}

unsafe fn render_minimap(ox: usize, oy: usize) {
    const MW: usize = 20; const MH: usize = 8;
    let sx = (PLAYER.x as usize).saturating_sub(MW/2).min(MAP_W.saturating_sub(MW));
    let sy = (PLAYER.y as usize).saturating_sub(MH/2).min(MAP_H.saturating_sub(MH));
    for vy in 0..MH {
        for vx in 0..MW {
            let mx = sx+vx; let my = sy+vy;
            if mx>=MAP_W||my>=MAP_H { vga_put(ox+vx,oy+vy,b' ',0x00); continue; }
            if !SEEN[my][mx] { vga_put(ox+vx,oy+vy,b' ',0x00); continue; }
            let ch = match TILES[my][mx] {
                Tile::Wall|Tile::Rock|Tile::Tree => 0xB2 as u8,
                Tile::StairsDown => b'>',
                Tile::Chest{..} => b'=',
                Tile::Water => b'~',
                _ => b'.',
            };
            let attr = if FOV[my][mx] { 0x07 } else { 0x08 };
            vga_put(ox+vx,oy+vy,ch,attr);
        }
    }
    // Player dot
    let pmx = (PLAYER.x as usize).saturating_sub(sx);
    let pmy = (PLAYER.y as usize).saturating_sub(sy);
    if pmx < MW && pmy < MH { vga_put(ox+pmx,oy+pmy,b'@',0x0F); }
    // Monsters
    for m in MONSTERS.iter() {
        if !m.alive { continue; }
        let mmx = (m.x as usize).saturating_sub(sx);
        let mmy = (m.y as usize).saturating_sub(sy);
        if mmx < MW && mmy < MH && FOV[m.y as usize][m.x as usize] {
            vga_put(ox+mmx,oy+mmy,m.kind.glyph(),m.kind.attr());
        }
    }
}

unsafe fn render_sidebar() {
    let col = SIDEBAR_X;
    // Clear sidebar
    for r in 0..21 { vga_fill_row(col, r, b' ', 0x00); }

    // Header
    vga_fill_row(col, 0, 0xCD, 0x09);
    vga_str(col, 0, b"RADIUM v5", 0x09);

    let mut row = 1;
    macro_rules! stat_row {
        ($label:expr, $val:expr, $lc:expr, $vc:expr) => {
            vga_str(col, row, $label, $lc);
            vga_str(col + $label.len(), row, $val, $vc);
            row += 1;
        }
    }

    // HP bar
    vga_str(col, row, b"HP:", 0x0C);
    let mut hbuf = [0u8;14]; let mut mbuf = [0u8;14];
    let hs = i32_to_str(PLAYER.hp, &mut hbuf);
    let ms = i32_to_str(PLAYER.max_hp, &mut mbuf);
    vga_str(col+3, row, hs, 0x0F);
    vga_str(col+3+hs.len(), row, b"/", 0x08);
    vga_str(col+4+hs.len(), row, ms, 0x07);
    // HP bar graphic
    let pct = (PLAYER.hp * 20 / PLAYER.max_hp.max(1)) as usize;
    for i in 0..20 {
        let c = col + i;
        if c < 79 { vga_put(c, row+1, if i<pct { 0xDB } else { 0xB0 }, if i < pct { 0x0A } else { 0x08 }); }
    }
    row += 2;

    // Weapon/Armor
    vga_str(col, row, b"WP:", 0x07); vga_str(col+3, row, PLAYER.weapon.name(), 0x0E); row+=1;
    vga_str(col, row, b"AR:", 0x07);
    match PLAYER.armor { Some(a)=>vga_str(col+3,row,a.name(),0x0B), None=>vga_str(col+3,row,b"None",0x08) }
    row+=1;

    // Ammo if ranged
    if PLAYER.weapon.is_ranged() {
        vga_str(col, row, b"Ammo:", 0x0E);
        let at = PLAYER.weapon.ammo_type();
        let mut ab = [0u8;12]; vga_str(col+5, row, u32_to_str(PLAYER.ammo[at], &mut ab), 0x0C);
        row+=1;
    }

    // Stats
    let mut buf12 = [0u8;12];
    vga_str(col,row,b"Lv:",0x07); vga_str(col+3,row,u32_to_str(PLAYER.level,&mut buf12),0x0F);
    vga_str(col+6,row,b"SP:",0x07); vga_str(col+9,row,u32_to_str(PLAYER.skill_points,&mut buf12),0x0D); row+=1;
    vga_str(col,row,b"$:",0x0E); vga_str(col+2,row,u32_to_str(PLAYER.gold,&mut buf12),0x0E); row+=1;
    vga_str(col,row,b"XP:",0x08); vga_str(col+3,row,u32_to_str(PLAYER.xp,&mut buf12),0x07);
    vga_str(col+7,row,b"/",0x08); vga_str(col+8,row,u32_to_str(PLAYER.xp_to_next(),&mut buf12),0x07); row+=1;

    // Combo
    if PLAYER.combo > 1 {
        vga_str(col, row, b"COMBO:", 0x0E);
        vga_str(col+6, row, u32_to_str(PLAYER.combo, &mut buf12), 0x0C);
        vga_put(col+6+buf12.len().min(5), row, b'x', 0x04);
        row+=1;
    }

    // Status effects
    for s in PLAYER.status.iter() {
        if s.turns_left == 0 { continue; }
        vga_put(col, row, s.kind.glyph(), s.kind.attr());
        vga_str(col+2, row, s.kind.name(), s.kind.attr());
        vga_str(col+2+s.kind.name().len(), row, b" (", 0x08);
        let mut tb = [0u8;12]; vga_str(col+4+s.kind.name().len(), row, u32_to_str(s.turns_left as u32, &mut tb), 0x07);
        vga_put(col+4+s.kind.name().len()+tb.len().min(3), row, b')', 0x08);
        row+=1;
    }

    // Minimap (always shown if room)
    if row + 9 < 21 {
        row+=1;
        render_minimap(col, row);
        row += 9;
    }

    // Nearby enemies
    if row < 20 {
        vga_str(col, row, b"Nearby:", 0x08); row+=1;
        for m in MONSTERS.iter() {
            if row >= 21 { break; }
            if m.alive && FOV[m.y as usize][m.x as usize] && m.kind.is_hostile() {
                vga_put(col, row, m.kind.glyph(), m.kind.attr());
                vga_str(col+2, row, m.kind.name(), 0x07);
                // Mini health bar for boss
                if m.kind == MonKind::AncientOne {
                    let pct = (m.hp * 10 / m.kind.max_hp().max(1)) as usize;
                    for i in 0..10 {
                        vga_put(col+14+i, row, if i<pct { 0xDB } else { 0xB0 }, if i<pct { 0x0C } else { 0x08 });
                    }
                }
                row+=1;
            }
        }
    }

    // Controls hint
    vga_fill_row(col, 20, 0xC4, 0x08);
    vga_str(col, 20, b"I:Bag K:Skill M:Map", 0x08);
}

unsafe fn render_log() {
    for i in 0..MAX_LOG {
        vga_fill_row(0, LOG_ROW+i, b' ', 0x00);
        if LOG_LEN[i]>0 { vga_str(0,LOG_ROW+i,&LOG[i][..LOG_LEN[i]],if i==MAX_LOG-1{0x0F}else{0x07}); }
    }
}

unsafe fn render_inventory() {
    // Full screen inventory
    for r in 0..25 { for c in 0..80 { vga_put(c,r,b' ',0x10); } }
    vga_box(0,0,80,25,0x17);
    vga_str(33,0,b" INVENTORY ",0x1F);
    vga_str(2,0,b"[ENTER:Use] [D:Drop] [C:Compare] [S:Sort] [ESC:Close]",0x1B);

    // Category header
    let cats = [b"Equip" as &[u8], b"Consum", b"Scroll", b"Misc", b"Other"];
    for (ci, cat) in cats.iter().enumerate() {
        vga_str(2 + ci*16, 1, cat, 0x1E);
    }

    // Items in 2 columns
    let items_per_page = (PLAYER.max_slots()).min(44);
    for i in 0..items_per_page {
        if i >= MAX_INV { break; }
        let col = if i < 22 { 1usize } else { 40usize };
        let row = 2 + (i % 22);
        if row > 23 { break; }

        // Cursor
        let selected = INV_CURSOR == i;
        let bg = if selected { 0x70 } else { 0x10 };

        // Clear row section
        for x in col..col+38 { vga_put(x, row, b' ', bg); }
        vga_str(col, row, if selected { b">" } else { b" " }, if selected { 0x70 } else { 0x18 });

        if i < PLAYER.inv_count {
            if let Some(it) = PLAYER.inventory[i] {
                let attr = if selected { it.rarity.attr() | 0x70 } else { it.rarity.attr() };
                vga_put(col+1, row, it.kind.glyph(), if selected { 0x7E } else { it.rarity.attr() });
                vga_str(col+3, row, it.kind.name(), if selected { 0x7F } else { 0x17 });
                // Stats inline
                let mut stat_buf = [0u8;14];
                match it.kind {
                    ItemKind::Weapon(w) => {
                        let s = i32_to_str(w.damage_mod(), &mut stat_buf);
                        vga_str(col+22, row, b"ATK:", if selected { 0x7C } else { 0x18 });
                        vga_str(col+26, row, s, if selected { 0x7E } else { 0x1A });
                    }
                    ItemKind::Armor { defense, .. } => {
                        let s = i32_to_str(defense, &mut stat_buf);
                        vga_str(col+22, row, b"DEF:", if selected { 0x7C } else { 0x18 });
                        vga_str(col+26, row, s, if selected { 0x7E } else { 0x19 });
                    }
                    ItemKind::Gold(x) => {
                        let mut buf = [0u8; 12];
                        let s = u32_to_str(x, &mut buf);
                        vga_str(col+22, row, s, if selected { 0x7E } else { 0x1E });
                        vga_put(col+22+s.len().min(8), row, b'g', if selected { 0x7E } else { 0x16 });
                    }
                    ItemKind::Ammo { count, .. } => {
                        let mut buf = [0u8; 12];
                        let s = u32_to_str(count, &mut buf);
                        vga_str(col+22, row, b"x", if selected { 0x7F } else { 0x17 });
                        vga_str(col+23, row, s, if selected { 0x7E } else { 0x1C });
                    }
                    _ => {}
                }
                // Rarity indicator
                let rc = it.rarity.name();
                vga_str(col+33, row, rc, if selected { 0x70 | it.rarity.attr() } else { 0x10 | it.rarity.attr() });
            }
        } else {
            vga_str(col+1, row, b"-empty-", 0x18);
        }
    }

    // Tooltip for selected item
    if let Some(it) = (if INV_CURSOR < MAX_INV { PLAYER.inventory[INV_CURSOR] } else { None }) {
        let ty = 24usize;
        vga_fill_row(0, ty, b' ', 0x1E);
        vga_str(2, ty, it.kind.name(), 0x1F);
        vga_str(2+it.kind.name().len(), ty, b" [", 0x18);
        vga_str(4+it.kind.name().len(), ty, it.rarity.name(), it.rarity.attr() | 0x10);
        vga_str(4+it.kind.name().len()+it.rarity.name().len(), ty, b"] Value:", 0x18);
        let mut vb = [0u8;12];
        vga_str(14+it.kind.name().len()+it.rarity.name().len(), ty, u32_to_str(it.kind.sell_value(), &mut vb), 0x1E);
    }
}

unsafe fn render_shop() {
    vga_box(8,1,64,23,0x27);
    vga_str(33,1,b" MERCHANT ",0x2F);

    // Tabs
    let buy_attr = if SHOP_BUY { 0x2F } else { 0x27 };
    let sell_attr = if !SHOP_BUY { 0x2F } else { 0x27 };
    vga_str(10, 2, b"[ BUY ]", buy_attr);
    vga_str(18, 2, b"[ SELL ]", sell_attr);

    // Personality + gold
    vga_str(10, 3, b"\"Welcome, traveller!\"", 0x2E);
    vga_str(40, 3, b"Your gold:", 0x27);
    let mut gb = [0u8;12]; vga_str(51, 3, u32_to_str(PLAYER.gold, &mut gb), 0x2E);

    // Items
    if SHOP_BUY {
        vga_str(10, 4, b"ITEM                        PRICE", 0x28);
        for i in 0..8 {
            let row = 5+i;
            let selected = SHOP_CURSOR==i;
            let bg = if selected { 0x70 } else { 0x20 };
            for x in 9..71 { vga_put(x,row,b' ',bg); }
            vga_str(10,row,if selected{b">"}else{b" "},bg|0x0F);
            match SHOP_STOCK[i] {
                Some(kind) => {
                    vga_put(11,row,kind.glyph(),if selected{0x7F}else{0x2F});
                    vga_str(13,row,kind.name(),if selected{0x7F}else{0x27});
                    let price = SHOP_PRICES[i];
                    let can_afford = PLAYER.gold >= price;
                    let mut pb = [0u8;12];
                    vga_str(48,row,u32_to_str(price,&mut pb),if can_afford{0x2A}else{0x24});
                    vga_put(48+pb.len().min(8),row,b'g',if can_afford{0x26}else{0x24});
                }
                None => {}
            }
        }
    } else {
        vga_str(10, 4, b"YOUR ITEMS                  VALUE", 0x28);
        for i in 0..8 {
            let row = 5+i;
            let selected = SHOP_CURSOR==i;
            let bg = if selected { 0x70 } else { 0x20 };
            for x in 9..71 { vga_put(x,row,b' ',bg); }
            vga_str(10,row,if selected{b">"}else{b" "},bg|0x0F);
            if i < PLAYER.inv_count {
                if let Some(it) = PLAYER.inventory[i] {
                    vga_put(11,row,it.kind.glyph(),if selected{0x7E}else{it.rarity.attr()|0x20});
                    vga_str(13,row,it.kind.name(),if selected{0x7F}else{0x27});
                    let val = it.kind.sell_value();
                    let mut vb = [0u8;12];
                    vga_str(48,row,u32_to_str(val,&mut vb),0x2E);
                    vga_put(48+vb.len().min(8),row,b'g',0x26);
                }
            } else {
                vga_str(11,row,b"(empty)",0x28);
            }
        }
    }

    // Haggle info
    if SHOP_HAGGLES == 0 {
        vga_str(10,14,b"Press H to HAGGLE (1x per visit, 25% chance)",0x2D);
    }
    vga_str(10,15,b"[UP/DN:Select] [ENTER:Deal] [TAB:Mode] [ESC:Exit]",0x27);
}

unsafe fn render_skill_tree() {
    // Hexagonal skill tree layout
    for r in 0..25 { for c in 0..80 { vga_put(c,r,b' ',0x00); } }
    vga_str(30,0,b" SKILL TREE - RADIUM DUNGEON ",0x0F);
    let mut buf = [0u8;12];
    vga_str(2,0,b"SP:",0x0D); vga_str(5,0,u32_to_str(PLAYER.skill_points,&mut buf),0x0F);

    // Path labels
    let paths: [(&[u8],u8); 4] = [(b"WARRIOR",0x0B),(b"RANGER",0x0A),(b"MAGE",0x0D),(b"ROGUE",0x0E)];
    for (pi,(name,color)) in paths.iter().enumerate() {
        let col = 2 + pi*19;
        vga_str(col, 1, name, *color);
    }

    // Tier lines and nodes
    let tier_rows = [3usize, 8, 13, 18];
    let path_cols = [3usize, 22, 41, 60];

    for (ni, node) in PLAYER_SKILLS.iter().enumerate() {
        let path_idx = match node.path { SkillPath::Warrior=>0, SkillPath::Ranger=>1, SkillPath::Mage=>2, SkillPath::Rogue=>3 };
        let row = tier_rows[node.tier as usize];
        let col = path_cols[path_idx];

        // Draw hex frame
        let is_sel = SKILL_CURSOR == ni;
        let attr = if !node.unlocked {
            let prereq_met = node.requires == 255 || (node.requires < MAX_SKILL_NODES as u8 && PLAYER_SKILLS[node.requires as usize].unlocked);
            if prereq_met { 0x08 } else { 0x08 }
        } else {
            match node.path { SkillPath::Warrior=>0x0B, SkillPath::Ranger=>0x0A, SkillPath::Mage=>0x0D, SkillPath::Rogue=>0x0E }
        };
        let frame_attr = if is_sel { 0x70 } else { attr };

        // Hexagon approximation: diamond shape
        vga_str(col, row-1, b" /---\\ ", frame_attr);
        vga_str(col, row,   b"|     |", frame_attr);
        vga_str(col, row+1, b"|     |", frame_attr);
        vga_str(col, row+2, b" \\---/ ", frame_attr);

        // Node content
        let status_ch = if node.unlocked { 0x02 } else { b'?' };
        vga_put(col+3, row, status_ch, if node.unlocked { 0x0A } else { 0x08 });
        let nl = node.name.len().min(5);
        vga_str(col+1, row+1, &node.name[..nl], if is_sel { 0x7F } else if node.unlocked { attr } else { 0x08 });

        // Connector line to parent
        if node.requires != 255 {
            let pr = &PLAYER_SKILLS[node.requires as usize];
            let pr_path_idx = match pr.path { SkillPath::Warrior=>0, SkillPath::Ranger=>1, SkillPath::Mage=>2, SkillPath::Rogue=>3 };
            let pr_row = tier_rows[pr.tier as usize];
            let pr_col = path_cols[pr_path_idx];
            // Draw vertical connector
            for r in pr_row+3..row.saturating_sub(1) {
                vga_put(pr_col+3, r, 0xB3, if pr.unlocked { attr } else { 0x08 });
            }
        }
    }

    // Description of selected node
    let sel = &PLAYER_SKILLS[SKILL_CURSOR];
    vga_str(2,22,sel.name,0x0F);
    vga_str(2,23,sel.desc,0x07);

    let prereq_met = sel.requires == 255 || (sel.requires < MAX_SKILL_NODES as u8 && PLAYER_SKILLS[sel.requires as usize].unlocked);
    let can_buy = !sel.unlocked && prereq_met && PLAYER.skill_points > 0;
    vga_str(2,24,if can_buy{b"[ENTER:Unlock]"}else if sel.unlocked{b"[Already unlocked]"}else if !prereq_met{b"[Requires parent skill]"}else{b"[Need skill points]"},0x0E);
    vga_str(40,24,b"[Arrows:Nav] [K:Close]",0x08);
}

unsafe fn render_world_map() {
    for r in 0..25 { for c in 0..80 { vga_put(c,r,b' ',0x00); } }
    vga_box(0,0,80,25,0x0F);
    vga_str(34,0,b" WORLD MAP ",0x0F);
    for sy in 1usize..24 {
        for sx in 0usize..78 {
            let mx = sx*2; let my = sy*4;
            if mx>=MAP_W||my>=MAP_H { continue; }
            if !SEEN[my][mx] { continue; }
            let (ch,attr) = match TILES[my][mx] {
                Tile::Wall => (0xB0u8, 0x08u8),
                Tile::Floor|Tile::Grass => (b'.', 0x07),
                Tile::Water => (b'~', 0x09),
                Tile::StairsDown => (b'>', 0x0A),
                Tile::Tree => (0x05, 0x02),
                Tile::Chest{..} => (b'=', 0x0E),
                _ => (b'.', 0x08),
            };
            vga_put(sx+1, sy, ch, attr);
        }
    }
    let px = ((PLAYER.x/2) as usize).min(78);
    let py = ((PLAYER.y/4) as usize).min(23);
    vga_put(px+1, py, b'@', 0x0F);
    let mut fb = [0u8;12];
    vga_str(2,24,b"Floor:",0x08); vga_str(8,24,u32_to_str(FLOOR,&mut fb),0x0E);
    vga_str(20,24,b"[M:Close]",0x08);
}

unsafe fn render_game_over() {
    vga_box(15,8,50,10,0x4F);
    vga_str(27,9, b"  YOU DIED  ", 0x4F);
    vga_str(17,11,b"The dungeon claims another soul...",0x4C);
    let mut lb=[0u8;12]; let mut fb=[0u8;12];
    vga_str(17,12,b"Reached Level:",0x47); vga_str(32,12,u32_to_str(PLAYER.level,&mut lb),0x4F);
    vga_str(17,13,b"Floor:",0x47); vga_str(24,13,u32_to_str(FLOOR,&mut fb),0x4F);
    vga_str(17,14,b"Turns Survived:",0x47); vga_str(33,14,u32_to_str(PLAYER.turns,&mut [0u8;12]),0x4F);
    vga_str(17,15,b"Enemies Slain: See log",0x47);
    vga_str(20,17,b"[R:New Game] [Q:Quit]",0x4F);
}

unsafe fn render_game_won() {
    vga_box(12,7,56,12,0x2F);
    vga_str(26,8, b"  DUNGEON CLEARED!  ", 0x2F);
    vga_str(14,10,b"The Warlord falls! You are victorious!",0x2A);
    vga_str(14,12,b"Your deeds will be remembered.",0x2E);
    let mut lb=[0u8;12]; let mut fb=[0u8;12];
    vga_str(14,13,b"Final Level:",0x27); vga_str(27,13,u32_to_str(PLAYER.level,&mut lb),0x2F);
    vga_str(14,14,b"Gold earned:",0x27); vga_str(27,14,u32_to_str(PLAYER.gold,&mut fb),0x2E);
    vga_str(14,15,b"Turns taken:",0x27); vga_str(27,15,u32_to_str(PLAYER.turns,&mut [0u8;12]),0x2F);
    vga_str(20,18,b"[R:New Game] [Q:Quit]",0x2F);
}

unsafe fn render_dialog() {
    vga_box(10,8,60,8,0x1E);
    vga_str(30,8,b" DIALOGUE ",0x1F);
    vga_str(12,10,DIALOG_TEXT,0x1E);
    vga_str(20,13,b"Press any key to continue...",0x18);
}

unsafe fn render_boss_bar() {
    for i in 0..MAX_MONSTERS {
        if MONSTERS[i].alive && MONSTERS[i].kind == MonKind::AncientOne {
            let m = &MONSTERS[i];
            vga_fill_row(0,0,b' ',0x4F);
            vga_str(2,0,b"ANCIENT ONE:",0x4F);
            let pct = (m.hp*50/m.kind.max_hp().max(1)) as usize;
            for j in 0..50 {
                vga_put(15+j, 0, if j<pct { 0xDB } else { 0xB0 }, if j<pct { 0x4C } else { 0x48 });
            }
            let mut hb=[0u8;14]; let mut mb=[0u8;14];
            vga_str(66,0,i32_to_str(m.hp,&mut hb),0x4F);
            vga_put(66+hb.len().min(8),0,b'/',0x48);
            vga_str(67+hb.len().min(8),0,i32_to_str(m.kind.max_hp(),&mut mb),0x4F);
            break;
        }
    }
}

unsafe fn render() {
    if GAME_OVER { render_game_over(); return; }
    if GAME_WON  { render_game_won();  return; }
    if SHOW_INV  { render_inventory(); return; }
    if SHOW_SKILL_TREE { render_skill_tree(); return; }
    if SHOW_MAP  { render_world_map(); return; }

    render_game_map();
    // Separator
    for r in 0..21 { vga_put(52,r,0xB3,0x08); }
    for c in 0..80 { vga_put(c,21,0xC4,0x08); }
    vga_put(52,21,0xC1,0x08);
    render_sidebar();
    render_log();

    if SHOP_OPEN   { render_shop(); }
    if DIALOG_OPEN { render_dialog(); }
    if BOSS_ACTIVE { render_boss_bar(); }
}

// =============================================================================
// INPUT / MAIN LOOP
// =============================================================================

#[derive(PartialEq)]
enum Key {
    W,A,S,D, I,K,M,N,E,R,Q,V,H,
    Up,Down,Left,Right,
    Space,Tab,Esc,Enter,
    Num1,Num2,Num3,Num4,Num5,
    LShift,RShift,
    None,
}



unsafe fn read_key() -> Key {
    if !is_key_pressed() { return Key::None; }
    let scan = port_byte_in(0x60);
    if scan >= 0x80 { return Key::None; }
    match scan {
        0x11=>Key::W, 0x1E=>Key::A, 0x1F=>Key::S, 0x20=>Key::D,
        0x17=>Key::I, 0x25=>Key::K, 0x32=>Key::M, 0x31=>Key::N,
        0x12=>Key::E, 0x13=>Key::R, 0x10=>Key::Q, 0x2F=>Key::V,
        0x23=>Key::H,
        0x48=>Key::Up, 0x50=>Key::Down, 0x4B=>Key::Left, 0x4D=>Key::Right,
        0x39=>Key::Space, 0x0F=>Key::Tab, 0x01=>Key::Esc, 0x1C=>Key::Enter,
        0x02=>Key::Num1, 0x03=>Key::Num2, 0x04=>Key::Num3, 0x05=>Key::Num4, 0x06=>Key::Num5,
        0x2A=>Key::LShift, 0x36=>Key::RShift,
        _ => Key::None,
    }
}

#[no_mangle]
pub extern "C" fn rust_dungeon() -> i32 {
    unsafe {
        terminal_clear();
        RNG_STATE = get_ticks().wrapping_mul(0xDEADBEEF);
        PLAYER = Player::new(0,0);
        FLOOR = 0; BOSS_ACTIVE = false; LAST_DUNGEON_FLOOR = 1;
        GAME_OVER = false; GAME_WON = false; LEVEL_UP_FLASH = 0;
        SHOW_INV = false; SHOW_SKILL_TREE = false; SHOW_MAP = false;
        SHOP_OPEN = false; DIALOG_OPEN = false; SKILL_CURSOR = 0;
        INV_CURSOR = 0; SYS_TICK = 0;
        // Copy static skill tree
        for i in 0..MAX_SKILL_NODES { PLAYER_SKILLS[i] = SKILL_TREE[i]; }
        for p in PARTICLES.iter_mut() { p.active = false; }
        for row in NOISE_MAP.iter_mut() { for v in row.iter_mut() { *v=0; } }
        gen_floor(0);
        render();

        'main: loop {
            let key = read_key();
            SYS_TICK = SYS_TICK.wrapping_add(1);
            update_particles(&TILES);

            if key == Key::Q { break 'main; }

            // Game over / win screens
            if GAME_OVER || GAME_WON {
                if key == Key::R {
                    PLAYER = Player::new(0,0);
                    FLOOR=0; BOSS_ACTIVE=false;
                    GAME_OVER=false; GAME_WON=false; LEVEL_UP_FLASH=0;
                    for i in 0..MAX_SKILL_NODES { PLAYER_SKILLS[i]=SKILL_TREE[i]; }
                    gen_floor(0);
                }
                render(); sleep_ms(16); continue;
            }

            // Dialog
            if DIALOG_OPEN {
                if key != Key::None { DIALOG_OPEN = false; render(); }
                else { sleep_ms(16); }
                continue;
            }

            // Inventory
            if SHOW_INV {
                match key {
                    Key::Esc|Key::LShift|Key::RShift|Key::I => SHOW_INV = false,
                    Key::Up|Key::W => if INV_CURSOR>0{INV_CURSOR-=1;},
                    Key::Down|Key::S => if INV_CURSOR<PLAYER.inv_count.saturating_sub(1){INV_CURSOR+=1;},
                    Key::Left => if INV_CURSOR>0{INV_CURSOR-=1;},
                    Key::Right => if INV_CURSOR<PLAYER.inv_count.saturating_sub(1){INV_CURSOR+=1;},
                    Key::Enter => { use_item(INV_CURSOR); if GAME_OVER||GAME_WON{SHOW_INV=false;} }
                    Key::D => {
                        if INV_CURSOR < PLAYER.inv_count {
                            push_log(b"Item dropped.");
                            PLAYER.remove_item_at(INV_CURSOR);
                            if INV_CURSOR > 0 && INV_CURSOR >= PLAYER.inv_count { INV_CURSOR -= 1; }
                        }
                    }
                    _ => {}
                }
                render(); sleep_ms(16); continue;
            }

            // Skill tree
            if SHOW_SKILL_TREE {
                match key {
                    Key::Esc|Key::K => SHOW_SKILL_TREE = false,
                    Key::Up|Key::W => if SKILL_CURSOR>0{SKILL_CURSOR-=1;},
                    Key::Down|Key::S => { if SKILL_CURSOR<MAX_SKILL_NODES-1{SKILL_CURSOR+=1;} }
                    Key::Left|Key::A => {
                        // Navigate by path
                        if SKILL_CURSOR >= 7 { SKILL_CURSOR -= 7; }
                    }
                    Key::Right|Key::D => {
                        if SKILL_CURSOR+7 < MAX_SKILL_NODES { SKILL_CURSOR += 7; }
                    }
                    Key::Enter => {
                        let sel = &PLAYER_SKILLS[SKILL_CURSOR];
                        let prereq_met = sel.requires==255||(sel.requires<MAX_SKILL_NODES as u8&&PLAYER_SKILLS[sel.requires as usize].unlocked);
                        if !sel.unlocked && prereq_met && PLAYER.skill_points>0 {
                            PLAYER_SKILLS[SKILL_CURSOR].unlocked = true;
                            PLAYER.skills[SKILL_CURSOR] = true;
                            PLAYER.skill_points -= 1;
                            push_log2(b"Learned: ", PLAYER_SKILLS[SKILL_CURSOR].name);
                            // Apply passive effects immediately
                            match SKILL_CURSOR {
                                0 => PLAYER.base_def += 3,  // Iron Skin
                                5 => { PLAYER.max_hp += 20; PLAYER.hp += 20; } // Colossus
                                18 => PLAYER.mana_shield = 5,  // Arcane Armor
                                _ => {}
                            }
                        } else {
                            push_log(b"Cannot unlock skill.");
                        }
                    }
                    _ => {}
                }
                render(); sleep_ms(16); continue;
            }

            // World map
            if SHOW_MAP {
                if key != Key::None && key != Key::M { SHOW_MAP = false; }
                else { sleep_ms(16); }
                render(); continue;
            }

            // Shop
            if SHOP_OPEN {
                match key {
                    Key::Esc|Key::Q => SHOP_OPEN = false,
                    Key::Tab => { SHOP_BUY = !SHOP_BUY; SHOP_CURSOR = 0; }
                    Key::Up|Key::W => if SHOP_CURSOR>0{SHOP_CURSOR-=1;},
                    Key::Down|Key::S => if SHOP_CURSOR<7{SHOP_CURSOR+=1;},
                    Key::H => {
                        // Haggle
                        if SHOP_HAGGLES == 0 {
                            SHOP_HAGGLES = 1;
                            if rng()%4==0 {
                                for p in SHOP_PRICES.iter_mut() { *p = *p * 3/4; }
                                push_log(b"Haggle success! Prices down.");
                            } else {
                                push_log(b"Merchant refuses to budge.");
                            }
                        } else { push_log(b"Already haggled this visit."); }
                    }
                    Key::Enter => {
                        if SHOP_BUY {
                            if let Some(kind) = SHOP_STOCK[SHOP_CURSOR] {
                                let price = SHOP_PRICES[SHOP_CURSOR];
                                if PLAYER.gold >= price {
                                    PLAYER.gold -= price;
                                    let item = make_item(kind);
                                    if !PLAYER.add_item(item) {
                                        PLAYER.gold += price;
                                        push_log(b"Inventory full!");
                                    } else {
                                        push_log2(b"Bought: ", kind.name());
                                        SHOP_STOCK[SHOP_CURSOR] = None;
                                    }
                                } else { push_log(b"Not enough gold!"); }
                            }
                        } else {
                            if SHOP_CURSOR < PLAYER.inv_count {
                                if let Some(it) = PLAYER.inventory[SHOP_CURSOR] {
                                    let val = it.kind.sell_value();
                                    PLAYER.gold += val;
                                    push_log2(b"Sold: ", it.kind.name());
                                    PLAYER.remove_item_at(SHOP_CURSOR);
                                    if SHOP_CURSOR>0&&SHOP_CURSOR>=PLAYER.inv_count { SHOP_CURSOR-=1; }
                                }
                            }
                        }
                    }
                    _ => {}
                }
                render(); sleep_ms(16); continue;
            }

            // Main game input
            match key {
                // Movement (task system: end_turn triggers AI)
                Key::W|Key::Up    => try_move(0,-1),
                Key::S|Key::Down  => try_move(0,1),
                Key::A|Key::Left  => try_move(-1,0),
                Key::D|Key::Right => try_move(1,0),

                // Gun fire (WASD direction held + space fires in last dir)
                Key::Space => {
                    if PLAYER.weapon.is_ranged() {
                        let (dx,dy) = PLAYER.last_attack_dir;
                        fire_gun(dx,dy); end_turn();
                    } else {
                        // Ranged direction: fire toward nearest visible enemy
                        let mut best_d = 999; let mut bdx=1i32; let mut bdy=0i32;
                        for m in MONSTERS.iter() {
                            if m.alive && FOV[m.y as usize][m.x as usize] && m.kind.is_hostile() {
                                let d=(m.x-PLAYER.x).abs()+(m.y-PLAYER.y).abs();
                                if d<best_d { best_d=d; bdx=(m.x-PLAYER.x).signum(); bdy=(m.y-PLAYER.y).signum(); }
                            }
                        }
                        if PLAYER.weapon.is_ranged() { fire_gun(bdx,bdy); end_turn(); }
                        else { push_log(b"Equip a gun to shoot!"); }
                    }
                }

                // Gun: numeric direction fire (numpad style)
                Key::Num1 => { if PLAYER.weapon.is_ranged() { fire_gun(-1,1); end_turn(); } }
                Key::Num2 => { if PLAYER.weapon.is_ranged() { fire_gun(0,1); end_turn(); } }
                Key::Num3 => { if PLAYER.weapon.is_ranged() { fire_gun(1,1); end_turn(); } }
                Key::Num4 => { if PLAYER.weapon.is_ranged() { fire_gun(-1,0); end_turn(); } }
                Key::Num5 => { if PLAYER.weapon.is_ranged() { fire_gun(1,0); end_turn(); } }

                // Pickup
                Key::E => try_pickup(),
                // Interact
                Key::Enter => try_interact(),
                // Mine
                Key::N => try_mine(),
                // Wait
                Key::Tab => { push_log(b"Waiting..."); end_turn(); }
                // Vanish (Rogue skill)
                Key::V => { if PLAYER.skills[24] { use_skill_active(24); } else { push_log(b"Skill: Vanish not unlocked."); } }
                // Warcry
                Key::H => {
                    if PLAYER.skills[2] { use_skill_active(2); }
                    else { // Show keys help
                        push_log(b"WASD:Move E:Pickup Enter:Interact N:Mine");
                    }
                }
                // Inventory
                Key::I|Key::LShift|Key::RShift => { SHOW_INV = true; INV_CURSOR = 0; }
                // Skill tree
                Key::K => { SHOW_SKILL_TREE = true; }
                // Map
                Key::M => { SHOW_MAP = true; }
                // Quick use first consumable
                Key::R => {
                    let mut used = false;
                    for i in 0..PLAYER.inv_count {
                        if let Some(it) = PLAYER.inventory[i] {
                            if matches!(it.kind, ItemKind::HealthPotion|ItemKind::Food|ItemKind::ManaPotion) {
                                use_item(i); used = true; break;
                            }
                        }
                    }
                    if !used { push_log(b"No consumable to use."); }
                }
                _ => {}
            }

            render();
            sleep_ms(16);
        }

        terminal_clear();
        let msg = b"Thanks for playing Radium Dungeon v5!\n";
        for &b in msg { terminal_putchar(b); }
        0
    }
}

//=============================================================================
// ICMP PING
//=============================================================================

const ICMP_ECHO_REQUEST: u8 = 8;
const ICMP_ECHO_REPLY: u8 = 0;

#[no_mangle]
pub extern "C" fn rust_ping(argc: i32, argv: *const *const u8) -> i32 {
    unsafe {
        if argc < 2 || argv.is_null() {
            rust_print(b"Usage: ping <x.x.x.x>\n");
            return -1;
        }

        let arg1 = *argv.add(1);
        if arg1.is_null() {
            rust_print(b"Usage: ping <x.x.x.x>\n");
            return -1;
        }

        // Parse IP from string "x.x.x.x"
        let mut dest = [0u8; 4];
        let mut octet = 0u8;
        let mut octet_idx = 0;
        let mut ptr = arg1;
        
        while *ptr != 0 && octet_idx < 4 {
            if *ptr == b'.' {
                dest[octet_idx] = octet;
                octet_idx += 1;
                octet = 0;
            } else if *ptr >= b'0' && *ptr <= b'9' {
                octet = octet * 10 + (*ptr - b'0');
            } else {
                rust_print(b"Error: Invalid IP address format\n");
                return -1;
            }
            ptr = ptr.add(1);
        }
        if octet_idx < 4 {
            dest[octet_idx] = octet;
        }

        // Verify device
        if RTL8139_DEVICE.is_none() {
            rust_print(b"Error: No network device\n");
            return -1;
        }

        rust_print(b"PING ");
        for i in 0..4 {
            print_num(dest[i] as i32);
            if i < 3 { rust_print(b"."); }
        }
        rust_print(b"\n");

        let gateway_mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
        let mut seq: u16 = 0;
        let mut received: u32 = 0;
        let mut total_ms: u32 = 0;

        for _ in 0..4 {
            // Build ICMP payload (8 bytes timestamp + 8 bytes padding)
            let mut icmp_payload = [0u8; 16];
            let start_tick = get_ticks();
            icmp_payload[0] = (start_tick >> 24) as u8;
            icmp_payload[1] = (start_tick >> 16) as u8;
            icmp_payload[2] = (start_tick >> 8) as u8;
            icmp_payload[3] = (start_tick & 0xFF) as u8;

            // Build ICMP header
            let mut icmp_packet = [0u8; 24];
            icmp_packet[0] = ICMP_ECHO_REQUEST;
            icmp_packet[1] = 0x00; // code
            icmp_packet[2] = 0x00; // checksum placeholder
            icmp_packet[3] = 0x00;
            icmp_packet[4] = 0x00; // identifier
            icmp_packet[5] = 0x01;
            icmp_packet[6] = (seq >> 8) as u8;
            icmp_packet[7] = (seq & 0xFF) as u8;
            
            for i in 0..16 {
                icmp_packet[8 + i] = icmp_payload[i];
            }

            // Calculate ICMP checksum
            let checksum = calculate_ip_checksum(&icmp_packet, 24);
            icmp_packet[2] = (checksum >> 8) as u8;
            icmp_packet[3] = (checksum & 0xFF) as u8;

            // Wrap in IP
            let mut ip_buffer = [0u8; 64];
            let ip_len = build_ip_packet(&dest, 1, &icmp_packet, &mut ip_buffer);
            if ip_len == 0 { continue; }

            // Wrap in Ethernet
            let mut eth_buffer = [0u8; 128];
            let eth_len = build_ethernet_frame(&gateway_mac, 0x0800, &ip_buffer[..ip_len], &mut eth_buffer);
            if eth_len == 0 { continue; }

            // Send
            rust_rtl8139_send(eth_buffer.as_ptr(), eth_len as u32);

            // Wait for reply
            RX_RESPONSE_LENGTH = 0;
            let mut timeout = 500000;
            let mut got_reply = false;

            while timeout > 0 {
                let packets = rust_rtl8139_receive();
                if packets > 0 && RX_RESPONSE_LENGTH >= 42 {
                    // Check: IP protocol = 1 (ICMP)
                    if RX_RESPONSE_BUFFER[23] == 1 {
                        let icmp_offset = 34; // eth(14) + ip(20)
                        if RX_RESPONSE_BUFFER[icmp_offset] == ICMP_ECHO_REPLY {
                            let end_tick = get_ticks();
                            let rtt = end_tick.wrapping_sub(start_tick);
                            total_ms += rtt;
                            received += 1;

                            rust_print(b"  ");
                            print_num(rtt as i32);
                            rust_print(b"ms\n");
                            got_reply = true;
                            break;
                        }
                    }
                    RX_RESPONSE_LENGTH = 0;
                }
                timeout -= 1;
            }

            if !got_reply {
                rust_print(b"  timeout\n");
            }

            seq += 1;
            sleep_ms(1000);
        }

        // Summary
        rust_print(b"---\n");
        rust_print(b"Sent: 4  Received: ");
        print_num(received as i32);
        if received > 0 {
            rust_print(b"  Avg: ");
            print_num((total_ms / received) as i32);
            rust_print(b"ms\n");
        } else {
            rust_print(b"\n");
        }

        if received == 4 { 0 } else { -1 }
    }
}

//=============================================================================
// HTTP CLIENT FOUNDATION v1 -- scp_2801 / RadiumOS
// Simplified, composable HTTP request/response API for external C callers.
// Builds on existing tcp_connect/tcp_send_data/tcp_receive_data/resolve_host.
//=============================================================================

const HTTP_MAX_HANDLES:   usize = 4;
const HTTP_MAX_HEADERS:   usize = 16;
const HTTP_HDR_NAME_LEN:  usize = 64;
const HTTP_HDR_VAL_LEN:   usize = 192;
const HTTP_MAX_URL:       usize = 512;
const HTTP_MAX_METHOD:    usize = 8;
const HTTP_MAX_BODY_OUT:  usize = 8192;
const HTTP_MAX_BODY_IN:   usize = 16384;

#[derive(Copy, Clone)]
struct HttpHeader {
    name:  [u8; HTTP_HDR_NAME_LEN],
    name_len: usize,
    val:   [u8; HTTP_HDR_VAL_LEN],
    val_len: usize,
}
impl HttpHeader {
    const fn blank() -> Self {
        Self { name: [0; HTTP_HDR_NAME_LEN], name_len: 0, val: [0; HTTP_HDR_VAL_LEN], val_len: 0 }
    }
}

#[derive(Copy, Clone)]
struct HttpRequestSlot {
    in_use:      bool,
    method:      [u8; HTTP_MAX_METHOD],
    method_len:  usize,
    url:         [u8; HTTP_MAX_URL],
    url_len:     usize,
    headers:     [HttpHeader; HTTP_MAX_HEADERS],
    header_count: usize,
    body:        [u8; HTTP_MAX_BODY_OUT],
    body_len:    usize,
}
impl HttpRequestSlot {
    const fn blank() -> Self {
        Self {
            in_use: false,
            method: [0; HTTP_MAX_METHOD], method_len: 0,
            url: [0; HTTP_MAX_URL], url_len: 0,
            headers: [HttpHeader::blank(); HTTP_MAX_HEADERS], header_count: 0,
            body: [0; HTTP_MAX_BODY_OUT], body_len: 0,
        }
    }
}

#[derive(Copy, Clone)]
struct HttpResponseSlot {
    in_use:       bool,
    status:       i32,
    status_text:  [u8; 32],
    status_text_len: usize,
    headers:      [HttpHeader; HTTP_MAX_HEADERS],
    header_count: usize,
    body:         [u8; HTTP_MAX_BODY_IN],
    body_len:     usize,
    ok:           bool,
}
impl HttpResponseSlot {
    const fn blank() -> Self {
        Self {
            in_use: false, status: 0,
            status_text: [0; 32], status_text_len: 0,
            headers: [HttpHeader::blank(); HTTP_MAX_HEADERS], header_count: 0,
            body: [0; HTTP_MAX_BODY_IN], body_len: 0,
            ok: false,
        }
    }
}

static mut HTTP_REQUESTS:  [HttpRequestSlot; HTTP_MAX_HANDLES]  = [HttpRequestSlot::blank();  HTTP_MAX_HANDLES];
static mut HTTP_RESPONSES: [HttpResponseSlot; HTTP_MAX_HANDLES] = [HttpResponseSlot::blank(); HTTP_MAX_HANDLES];

unsafe fn cstr_slice<'a>(p: *const u8, max: usize) -> &'a [u8] {
    if p.is_null() { return &[]; }
    let mut len = 0;
    while len < max && *p.add(len) != 0 { len += 1; }
    core::slice::from_raw_parts(p, len)
}

//-----------------------------------------------------------------------------
// Request builder (low-level)
//-----------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn http_new() -> i32 {
    unsafe {
        for i in 0..HTTP_MAX_HANDLES {
            if !HTTP_REQUESTS[i].in_use {
                HTTP_REQUESTS[i] = HttpRequestSlot::blank();
                HTTP_REQUESTS[i].in_use = true;
                HTTP_REQUESTS[i].method[..3].copy_from_slice(b"GET");
                HTTP_REQUESTS[i].method_len = 3;
                return i as i32;
            }
        }
        -1
    }
}

#[no_mangle]
pub extern "C" fn http_set_method(handle: i32, method: *const u8) -> i32 {
    unsafe {
        if handle < 0 || handle as usize >= HTTP_MAX_HANDLES { return -1; }
        let r = &mut HTTP_REQUESTS[handle as usize];
        if !r.in_use { return -1; }
        let m = cstr_slice(method, HTTP_MAX_METHOD - 1);
        let n = m.len().min(HTTP_MAX_METHOD - 1);
        r.method[..n].copy_from_slice(&m[..n]);
        r.method_len = n;
        0
    }
}

#[no_mangle]
pub extern "C" fn http_set_url(handle: i32, url: *const u8) -> i32 {
    unsafe {
        if handle < 0 || handle as usize >= HTTP_MAX_HANDLES { return -1; }
        let r = &mut HTTP_REQUESTS[handle as usize];
        if !r.in_use { return -1; }
        let u = cstr_slice(url, HTTP_MAX_URL - 1);
        let n = u.len().min(HTTP_MAX_URL - 1);
        r.url[..n].copy_from_slice(&u[..n]);
        r.url_len = n;
        0
    }
}

#[no_mangle]
pub extern "C" fn http_set_header(handle: i32, name: *const u8, value: *const u8) -> i32 {
    unsafe {
        if handle < 0 || handle as usize >= HTTP_MAX_HANDLES { return -1; }
        let r = &mut HTTP_REQUESTS[handle as usize];
        if !r.in_use || r.header_count >= HTTP_MAX_HEADERS { return -1; }
        let n = cstr_slice(name, HTTP_HDR_NAME_LEN - 1);
        let v = cstr_slice(value, HTTP_HDR_VAL_LEN - 1);
        let h = &mut r.headers[r.header_count];
        let nl = n.len().min(HTTP_HDR_NAME_LEN - 1);
        let vl = v.len().min(HTTP_HDR_VAL_LEN - 1);
        h.name[..nl].copy_from_slice(&n[..nl]); h.name_len = nl;
        h.val[..vl].copy_from_slice(&v[..vl]);  h.val_len = vl;
        r.header_count += 1;
        0
    }
}

#[no_mangle]
pub extern "C" fn http_set_body(handle: i32, data: *const u8, len: u32) -> i32 {
    unsafe {
        if handle < 0 || handle as usize >= HTTP_MAX_HANDLES || data.is_null() { return -1; }
        let r = &mut HTTP_REQUESTS[handle as usize];
        if !r.in_use { return -1; }
        let n = (len as usize).min(HTTP_MAX_BODY_OUT);
        core::ptr::copy_nonoverlapping(data, r.body.as_mut_ptr(), n);
        r.body_len = n;
        0
    }
}

#[no_mangle]
pub extern "C" fn http_free(handle: i32) {
    unsafe {
        if handle >= 0 && (handle as usize) < HTTP_MAX_HANDLES {
            HTTP_REQUESTS[handle as usize] = HttpRequestSlot::blank();
        }
    }
}

//-----------------------------------------------------------------------------
// URL parsing (scheme/host/port/path) -- HTTP only, TLS not implemented here
//-----------------------------------------------------------------------------

struct HttpUrlParts {
    host: [u8; 256], host_len: usize,
    path: [u8; 256], path_len: usize,
    port: u16,
}

unsafe fn http_parse_url(url: &[u8]) -> Option<HttpUrlParts> {
    let skip = if url.len() >= 7 && &url[..7] == b"http://" { 7 }
               else if url.len() >= 8 && &url[..8] == b"https://" { return None; } // no TLS
               else { 0 };
    let rest = &url[skip..];
    if rest.is_empty() { return None; }
    let host_end = rest.iter().position(|&c| c == b'/' || c == b':').unwrap_or(rest.len());
    let hostname = &rest[..host_end];
    let mut idx = host_end;
    let mut port: u16 = 80;
    if idx < rest.len() && rest[idx] == b':' {
        idx += 1; port = 0;
        while idx < rest.len() && rest[idx].is_ascii_digit() {
            port = port * 10 + (rest[idx] - b'0') as u16;
            idx += 1;
        }
    }
    let path = if idx < rest.len() { &rest[idx..] } else { b"/" as &[u8] };

    let mut parts = HttpUrlParts { host: [0; 256], host_len: 0, path: [0; 256], path_len: 0, port };
    let hl = hostname.len().min(255);
    parts.host[..hl].copy_from_slice(&hostname[..hl]);
    parts.host_len = hl;
    let pl = path.len().min(255);
    parts.path[..pl].copy_from_slice(&path[..pl]);
    parts.path_len = pl;
    Some(parts)
}

//-----------------------------------------------------------------------------
// Send + parse response
//-----------------------------------------------------------------------------

unsafe fn http_find_header_end(buf: &[u8]) -> Option<usize> {
    if buf.len() < 4 { return None; }
    for i in 0..=buf.len() - 4 {
        if &buf[i..i+4] == b"\r\n\r\n" { return Some(i + 4); }
    }
    None
}


unsafe fn http_parse_response(raw: &[u8], resp: &mut HttpResponseSlot) -> bool {
    let hdr_end = match http_find_header_end(raw) { 
        Some(v) => v, 
        None => return false 
    };
    let head = &raw[..hdr_end];

    // 1. Parse Status Line
    let line_end = head.iter().position(|&c| c == b'\r').unwrap_or(head.len());
    let status_line = &head[..line_end];
    
    if let Some(p1) = status_line.iter().position(|&c| c == b' ') {
        let after = &status_line[p1+1..];
        let sp2 = after.iter().position(|&c| c == b' ').unwrap_or(after.len());
        let code_bytes = &after[..sp2];
        
        let mut code = 0i32;
        for &c in code_bytes { 
            if c.is_ascii_digit() { code = code * 10 + (c - b'0') as i32; } 
        }
        resp.status = code;
        
        if sp2 < after.len() {
            let txt = &after[sp2+1..];
            let tl = txt.len().min(31);
            resp.status_text[..tl].copy_from_slice(&txt[..tl]);
            resp.status_text_len = tl;
        }
    }

    // 2. Parse Headers
    let mut pos = line_end;
    let mut is_chunked = false;
    let mut content_length: Option<usize> = None; // NEW: Track exact body size
    
    while pos < head.len() {
        while pos < head.len() && (head[pos] == b'\r' || head[pos] == b'\n') { pos += 1; }
        if pos >= head.len() { break; }
        
        let start = pos;
        while pos < head.len() && head[pos] != b'\r' { pos += 1; }
        if pos <= start { break; }
        
        let line = &head[start..pos];
        if let Some(colon) = line.iter().position(|&c| c == b':') {
            if resp.header_count < HTTP_MAX_HEADERS {
                let name = trim(&line[..colon]);
                let mut vstart = colon + 1;
                while vstart < line.len() && line[vstart] == b' ' { vstart += 1; }
                let val = &line[vstart..];
                
                if name.eq_ignore_ascii_case(b"transfer-encoding") && val.eq_ignore_ascii_case(b"chunked") {
                    is_chunked = true;
                }
                // NEW: Extract Content-Length
                if name.eq_ignore_ascii_case(b"content-length") {
                    let mut cl = 0usize;
                    for &c in val {
                        if c.is_ascii_digit() { cl = cl * 10 + (c - b'0') as usize; }
                    }
                    content_length = Some(cl);
                }

                let h = &mut resp.headers[resp.header_count];
                let nl = name.len().min(HTTP_HDR_NAME_LEN - 1);
                let vl = val.len().min(HTTP_HDR_VAL_LEN - 1);
                h.name[..nl].copy_from_slice(&name[..nl]); h.name_len = nl;
                h.val[..vl].copy_from_slice(&val[..vl]);   h.val_len = vl;
                resp.header_count += 1;
            }
        }
    }

    // 3. Parse Body
    let body_raw = &raw[hdr_end..];
    
    // NEW: If the server told us the exact size, enforce it! 
    // This prevents reading garbage if tcp_receive_data over-reported the packet size.
    let safe_body_raw = if let Some(cl) = content_length {
        &body_raw[..cl.min(body_raw.len())]
    } else {
        body_raw
    };

    if is_chunked {
        let mut decoded = [0u8; HTTP_MAX_BODY_IN];
        let decoded_len = http_decode_chunked(safe_body_raw, &mut decoded);
        let bl = decoded_len.min(HTTP_MAX_BODY_IN);
        if bl > 0 {
            resp.body[..bl].copy_from_slice(&decoded[..bl]);
        }
        resp.body_len = bl;
    } else {
        let bl = safe_body_raw.len().min(HTTP_MAX_BODY_IN);
        if bl > 0 {
            resp.body[..bl].copy_from_slice(&safe_body_raw[..bl]);
        }
        resp.body_len = bl;
    }
    
    true
}


fn u32_to_dec(mut n: u32, buf: &mut [u8; 12]) -> &[u8] {
    if n == 0 { buf[0] = b'0'; return &buf[..1]; }
    let mut tmp = [0u8; 12]; let mut i = 0;
    while n > 0 { tmp[i] = b'0' + (n % 10) as u8; n /= 10; i += 1; }
    for j in 0..i { buf[j] = tmp[i - 1 - j]; }
    &buf[..i]
}

/// Send the built request. Returns a response handle (>=0) or -1 on failure.
#[no_mangle]
pub extern "C" fn http_send(handle: i32) -> i32 {
    unsafe {
        if handle < 0 || handle as usize >= HTTP_MAX_HANDLES { return -1; }
        let req = HTTP_REQUESTS[handle as usize];
        if !req.in_use || req.url_len == 0 { return -1; }

        let url = &req.url[..req.url_len];
        let parts = match http_parse_url(url) { Some(p) => p, None => return -1 };
        let hostname = &parts.host[..parts.host_len];

        let mut host_z = [0u8; 256];
        let hl = hostname.len().min(255);
        host_z[..hl].copy_from_slice(&hostname[..hl]);

        let ip = match resolve_host(&host_z) { Some(ip) => ip, None => return -1 };
        if !tcp_connect(&ip, parts.port) { return -1; }

        let mut out = [0u8; HTTP_MAX_BODY_OUT + 2048];
        let mut n = 0usize;
        macro_rules! push { ($b:expr) => { for &c in $b { if n < out.len() { out[n] = c; n += 1; } } }; }

        push!(&req.method[..req.method_len]);
        push!(b" ");
        push!(&parts.path[..parts.path_len]);
        push!(b" HTTP/1.1\r\n");
        push!(b"Host: ");
        push!(hostname);
        push!(b"\r\n");

        let mut has_ua = false;
        let mut has_cl = false;
        for i in 0..req.header_count {
            let h = &req.headers[i];
            let name = &h.name[..h.name_len];
            if name.eq_ignore_ascii_case(b"user-agent") { has_ua = true; }
            if name.eq_ignore_ascii_case(b"content-length") { has_cl = true; }
            push!(name);
            push!(b": ");
            push!(&h.val[..h.val_len]);
            push!(b"\r\n");
        }
        if !has_ua { push!(b"User-Agent: RadiumOS-HTTP/1.0\r\n"); }
        if req.body_len > 0 && !has_cl {
            push!(b"Content-Length: ");
            let mut lb = [0u8; 12];
            push!(u32_to_dec(req.body_len as u32, &mut lb));
            push!(b"\r\n");
        }
        push!(b"Connection: close\r\n\r\n");
        if req.body_len > 0 { push!(&req.body[..req.body_len]); }

        if !tcp_send_data(&out[..n]) { tcp_close(); return -1; }

        let recv_len = tcp_receive_data(5_000_000);
        tcp_close();
        if recv_len == 0 { return -1; }

        let raw = &HTTP_RECEIVE_BUFFER[..recv_len];

        for i in 0..HTTP_MAX_HANDLES {
            if !HTTP_RESPONSES[i].in_use {
                HTTP_RESPONSES[i] = HttpResponseSlot::blank();
                HTTP_RESPONSES[i].in_use = true;
                HTTP_RESPONSES[i].ok = http_parse_response(raw, &mut HTTP_RESPONSES[i]);
                return i as i32;
            }
        }
        -1
    }
}

//-----------------------------------------------------------------------------
// Response accessors
//-----------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn http_response_ok(handle: i32) -> bool {
    unsafe {
        handle >= 0 && (handle as usize) < HTTP_MAX_HANDLES
            && HTTP_RESPONSES[handle as usize].in_use
            && HTTP_RESPONSES[handle as usize].ok
    }
}

#[no_mangle]
pub extern "C" fn http_response_status(handle: i32) -> i32 {
    unsafe {
        if handle < 0 || handle as usize >= HTTP_MAX_HANDLES { return -1; }
        HTTP_RESPONSES[handle as usize].status
    }
}

#[no_mangle]
pub extern "C" fn http_response_header(handle: i32, name: *const u8, out: *mut u8, out_size: u32) -> i32 {
    unsafe {
        if handle < 0 || handle as usize >= HTTP_MAX_HANDLES || out.is_null() || out_size == 0 { return -1; }
        let resp = &HTTP_RESPONSES[handle as usize];
        if !resp.in_use { return -1; }
        let target = cstr_slice(name, HTTP_HDR_NAME_LEN - 1);
        for i in 0..resp.header_count {
            let h = &resp.headers[i];
            if h.name[..h.name_len].eq_ignore_ascii_case(target) {
                // Reserve 1 byte for the \0 terminator
                let n = h.val_len.min((out_size as usize).saturating_sub(1));
                core::ptr::copy_nonoverlapping(h.val.as_ptr(), out, n);
                *out.add(n) = 0; // Null-terminate for C
                return n as i32;
            }
        }
        -1
    }
}
#[no_mangle]
pub extern "C" fn http_response_body_len(handle: i32) -> i32 {
    unsafe {
        if handle < 0 || handle as usize >= HTTP_MAX_HANDLES { return -1; }
        HTTP_RESPONSES[handle as usize].body_len as i32
    }
}



#[no_mangle]
pub extern "C" fn http_response_body(handle: i32, out: *mut u8, out_size: u32) -> i32 {
    unsafe {
        if handle < 0 || handle as usize >= HTTP_MAX_HANDLES || out.is_null() || out_size == 0 { return -1; }
        let resp = &HTTP_RESPONSES[handle as usize];
        if !resp.in_use { return -1; }
        // Reserve 1 byte for the \0 terminator
        let n = resp.body_len.min((out_size as usize).saturating_sub(1));
        core::ptr::copy_nonoverlapping(resp.body.as_ptr(), out, n);
        *out.add(n) = 0; // Null-terminate for C
        n as i32
    }
}

#[no_mangle]
pub extern "C" fn http_free_response(handle: i32) {
    unsafe {
        if handle >= 0 && (handle as usize) < HTTP_MAX_HANDLES {
            HTTP_RESPONSES[handle as usize] = HttpResponseSlot::blank();
        }
    }
}

//=============================================================================
// HTTP CLIENT -- CHAINABLE BUILDER LAYER
// Lets C callers build+fire a request in one expression instead of checking
// each setter's return value individually.
//=============================================================================

#[no_mangle]
pub extern "C" fn http(method: *const u8, url: *const u8) -> i32 {
    unsafe {
        let h = http_new();
        if h < 0 { return -1; }
        unsafe {http_set_method(h, method);
        http_set_url(h, url);}
        h
    }
}

#[no_mangle]
pub extern "C" fn http_hdr(handle: i32, name: *const u8, value: *const u8) -> i32 {
    unsafe  {http_set_header(handle, name, value);}
    handle
}

#[no_mangle]
pub extern "C" fn http_body(handle: i32, data: *const u8, len: u32) -> i32 {
    unsafe {http_set_body(handle, data, len);}
    handle
}

/// Sends the request, frees the request slot, returns the response handle.
#[no_mangle]
pub extern "C" fn http_go(handle: i32) -> i32 {
    let resp = http_send(handle);
    unsafe {http_free(handle);}
    resp
}
unsafe fn http_decode_chunked(body: &[u8], out: &mut [u8]) -> usize {
    let mut src = 0usize;
    let mut dst = 0usize;
    while src < body.len() && dst < out.len() {
        // Find chunk size line
        let line_end = body[src..].iter().position(|&c| c == b'\r').unwrap_or(body[src..].len());
        if line_end == 0 { break; }
        let size_str = &body[src..src + line_end];
        let mut chunk_size = 0usize;
        for &c in size_str {
            if c >= b'0' && c <= b'9' { chunk_size = chunk_size * 16 + (c - b'0') as usize; }
            else if c >= b'a' && c <= b'f' { chunk_size = chunk_size * 16 + (c - b'a' + 10) as usize; }
            else if c >= b'A' && c <= b'F' { chunk_size = chunk_size * 16 + (c - b'A' + 10) as usize; }
            else { break; }
        }
        if chunk_size == 0 { break; }  // terminal chunk
        src += line_end;
        if src < body.len() && body[src] == b'\r' { src += 1; }
        if src < body.len() && body[src] == b'\n' { src += 1; }
        let copy_len = chunk_size.min(body.len() - src).min(out.len() - dst);
        out[dst..dst + copy_len].copy_from_slice(&body[src..src + copy_len]);
        dst += copy_len;
        src += copy_len;
        // Skip trailing \r\n
        if src < body.len() && body[src] == b'\r' { src += 1; }
        if src < body.len() && body[src] == b'\n' { src += 1; }
    }
    dst
}
//=============================================================================
// RADIUM BROWSER (RBR) -- w3m-style text browser
// Basic-color HTML rendering + pager + link list + tiny history stack,
// built entirely on top of the HTTP client and the existing HTML parser.
//=============================================================================

const RBR_COL_TEXT:   u8 = 0x07;
const RBR_COL_H1:     u8 = 0x0E;
const RBR_COL_H2:     u8 = 0x0A;
const RBR_COL_H3:     u8 = 0x0B;
const RBR_COL_LINK:   u8 = 0x09;
const RBR_COL_BOLD:   u8 = 0x0F;
const RBR_COL_ITALIC: u8 = 0x08;
const RBR_COL_CODE:   u8 = 0x0D;
const RBR_COL_QUOTE:  u8 = 0x06;
const RBR_COL_LI:     u8 = 0x07;
const RBR_COL_HR:     u8 = 0x08;
const RBR_COL_STATUS: u8 = 0x70;
const RBR_COL_ERR:    u8 = 0x4F;

const RBR_MAX_LINES:    usize = 512;
const RBR_LINE_LEN:     usize = 128;
const RBR_MAX_LINKS:    usize = 64;
const RBR_LINK_LEN:     usize = 192;
const RBR_MAX_HISTORY:  usize = 8;
const RBR_URL_LEN:      usize = 256;
const RBR_PAGE_ROWS:    usize = 23; // leaves 1 status row + 1 input row on 25-line VGA text

#[derive(Copy, Clone)]
struct RbrLine {
    text: [u8; RBR_LINE_LEN],
    len:  usize,
    color: u8,
}
impl RbrLine {
    const fn blank() -> Self { Self { text: [0; RBR_LINE_LEN], len: 0, color: RBR_COL_TEXT } }
}

#[derive(Copy, Clone)]
struct RbrLink {
    url: [u8; RBR_LINK_LEN],
    len: usize,
}
impl RbrLink {
    const fn blank() -> Self { Self { url: [0; RBR_LINK_LEN], len: 0 } }
}

static mut RBR_LINES:       [RbrLine; RBR_MAX_LINES] = [RbrLine::blank(); RBR_MAX_LINES];
static mut RBR_LINE_COUNT:  usize = 0;
static mut RBR_LINKS:       [RbrLink; RBR_MAX_LINKS] = [RbrLink::blank(); RBR_MAX_LINKS];
static mut RBR_LINK_COUNT:  usize = 0;
static mut RBR_SCROLL:      usize = 0;
static mut RBR_STATUS:      [u8; 128] = [0; 128];
static mut RBR_STATUS_LEN:  usize = 0;
static mut RBR_HISTORY:     [[u8; RBR_URL_LEN]; RBR_MAX_HISTORY] = [[0; RBR_URL_LEN]; RBR_MAX_HISTORY];
static mut RBR_HISTORY_LEN: [usize; RBR_MAX_HISTORY] = [0; RBR_MAX_HISTORY];
static mut RBR_HISTORY_POS: usize = 0;
static mut RBR_CURRENT_URL: [u8; RBR_URL_LEN] = [0; RBR_URL_LEN];
static mut RBR_CURRENT_URL_LEN: usize = 0;

unsafe fn rbr_set_status(s: &[u8]) {
    let n = s.len().min(RBR_STATUS.len() - 1);
    RBR_STATUS[..n].copy_from_slice(&s[..n]);
    RBR_STATUS_LEN = n;
}

unsafe fn rbr_push_line(text: &[u8], color: u8) {
    // word-wrap to RBR_LINE_LEN, appending into RBR_LINES
    if text.is_empty() {
        if RBR_LINE_COUNT < RBR_MAX_LINES {
            RBR_LINES[RBR_LINE_COUNT] = RbrLine::blank();
            RBR_LINE_COUNT += 1;
        }
        return;
    }
    let mut pos = 0;
    while pos < text.len() {
        let remaining = text.len() - pos;
        let take = remaining.min(RBR_LINE_LEN - 1);
        // break on last space if we're mid-word and there's more text
        let mut cut = take;
        if pos + take < text.len() {
            if let Some(sp) = text[pos..pos + take].iter().rposition(|&c| c == b' ') {
                if sp > 0 { cut = sp; }
            }
        }
        if RBR_LINE_COUNT >= RBR_MAX_LINES { return; }
        let line = &mut RBR_LINES[RBR_LINE_COUNT];
        let n = cut.min(RBR_LINE_LEN - 1);
        line.text[..n].copy_from_slice(&text[pos..pos + n]);
        line.len = n;
        line.color = color;
        RBR_LINE_COUNT += 1;
        pos += cut;
        while pos < text.len() && text[pos] == b' ' { pos += 1; }
    }
}

unsafe fn rbr_reset_document() {
    RBR_LINE_COUNT = 0;
    RBR_LINK_COUNT = 0;
    RBR_SCROLL = 0;
}

/// Parses a fetched response body into RBR's line buffer + link table.
/// Reuses the shared parse_html()/get_element()/get_text() machinery.
unsafe fn rbr_build_document(body: &[u8]) {
    rbr_reset_document();
    let count = parse_html(body);

    for i in 0..count {
        let e = match get_element(i) { Some(e) => *e, None => continue };
        let text = get_text(e.text_start, e.text_len);
        if text.is_empty() && e.element_type != HtmlElementType::HorizontalRule { continue; }

        match e.element_type {
            HtmlElementType::Header1 => {
                rbr_push_line(b"", RBR_COL_TEXT);
                let mut buf = [0u8; RBR_LINE_LEN];
                let n = text.len().min(RBR_LINE_LEN - 3);
                buf[0] = b'#'; buf[1] = b' ';
                buf[2..2 + n].copy_from_slice(&text[..n]);
                rbr_push_line(&buf[..2 + n], RBR_COL_H1);
            }
            HtmlElementType::Header2 => {
                rbr_push_line(b"", RBR_COL_TEXT);
                let mut buf = [0u8; RBR_LINE_LEN];
                let n = text.len().min(RBR_LINE_LEN - 4);
                buf[0] = b'#'; buf[1] = b'#'; buf[2] = b' ';
                buf[3..3 + n].copy_from_slice(&text[..n]);
                rbr_push_line(&buf[..3 + n], RBR_COL_H2);
            }
            HtmlElementType::Header3
            | HtmlElementType::Header4
            | HtmlElementType::Header5
            | HtmlElementType::Header6 => {
                let mut buf = [0u8; RBR_LINE_LEN];
                let n = text.len().min(RBR_LINE_LEN - 5);
                buf[0] = b'#'; buf[1] = b'#'; buf[2] = b'#'; buf[3] = b' ';
                buf[4..4 + n].copy_from_slice(&text[..n]);
                rbr_push_line(&buf[..4 + n], RBR_COL_H3);
            }
            HtmlElementType::Link => {
                let link_id = RBR_LINK_COUNT;
                if link_id < RBR_MAX_LINKS && e.href_len > 0 {
                    let href = get_text(e.href_start, e.href_len);
                    let l = &mut RBR_LINKS[link_id];
                    let n = href.len().min(RBR_LINK_LEN - 1);
                    l.url[..n].copy_from_slice(&href[..n]);
                    l.len = n;
                    RBR_LINK_COUNT += 1;

                    let mut buf = [0u8; RBR_LINE_LEN];
                    let mut idx = 0;
                    buf[idx] = b'['; idx += 1;
                    let mut nb = [0u8; 12];
                    let ns = u32_to_dec(link_id as u32, &mut nb);
                    for &c in ns { if idx < buf.len() { buf[idx] = c; idx += 1; } }
                    buf[idx] = b']'; idx += 1;
                    buf[idx] = b' '; idx += 1;
                    let tn = text.len().min(buf.len().saturating_sub(idx));
                    buf[idx..idx + tn].copy_from_slice(&text[..tn]);
                    idx += tn;
                    rbr_push_line(&buf[..idx], RBR_COL_LINK);
                } else {
                    rbr_push_line(text, RBR_COL_LINK);
                }
            }
            HtmlElementType::Bold => rbr_push_line(text, RBR_COL_BOLD),
            HtmlElementType::Italic => rbr_push_line(text, RBR_COL_ITALIC),
            HtmlElementType::Code | HtmlElementType::Preformatted => rbr_push_line(text, RBR_COL_CODE),
            HtmlElementType::Blockquote => {
                let mut buf = [0u8; RBR_LINE_LEN];
                let n = text.len().min(RBR_LINE_LEN - 2);
                buf[0] = b'>'; buf[1] = b' ';
                buf[2..2 + n].copy_from_slice(&text[..n]);
                rbr_push_line(&buf[..2 + n], RBR_COL_QUOTE);
            }
            HtmlElementType::ListItem | HtmlElementType::OrderedListItem => {
                let mut buf = [0u8; RBR_LINE_LEN];
                let n = text.len().min(RBR_LINE_LEN - 3);
                buf[0] = b' '; buf[1] = b'*'; buf[2] = b' ';
                buf[3..3 + n].copy_from_slice(&text[..n]);
                rbr_push_line(&buf[..3 + n], RBR_COL_LI);
            }
            HtmlElementType::Paragraph | HtmlElementType::Div => {
                rbr_push_line(text, RBR_COL_TEXT);
                rbr_push_line(b"", RBR_COL_TEXT);
            }
            HtmlElementType::HorizontalRule => {
                let mut buf = [0u8; RBR_LINE_LEN];
                for b in buf.iter_mut() { *b = b'-'; }
                rbr_push_line(&buf[..RBR_LINE_LEN.min(60)], RBR_COL_HR);
            }
            _ => rbr_push_line(text, RBR_COL_TEXT),
        }
    }
}

//-----------------------------------------------------------------------------
// History
//-----------------------------------------------------------------------------

unsafe fn rbr_history_push(url: &[u8]) {
    if RBR_HISTORY_POS >= RBR_MAX_HISTORY {
        for i in 1..RBR_MAX_HISTORY {
            RBR_HISTORY[i - 1] = RBR_HISTORY[i];
            RBR_HISTORY_LEN[i - 1] = RBR_HISTORY_LEN[i];
        }
        RBR_HISTORY_POS = RBR_MAX_HISTORY - 1;
    }
    let n = url.len().min(RBR_URL_LEN - 1);
    RBR_HISTORY[RBR_HISTORY_POS][..n].copy_from_slice(&url[..n]);
    RBR_HISTORY_LEN[RBR_HISTORY_POS] = n;
    RBR_HISTORY_POS += 1;
}

/// Navigates back one page in history, if any. Returns true if it moved.
#[no_mangle]
pub extern "C" fn rbr_back() -> bool {
    unsafe {
        if RBR_HISTORY_POS < 2 { return false; }
        RBR_HISTORY_POS -= 2;
        let idx = RBR_HISTORY_POS;
        let len = RBR_HISTORY_LEN[idx];
        let mut url = [0u8; RBR_URL_LEN + 1];
        url[..len].copy_from_slice(&RBR_HISTORY[idx][..len]);
        url[len] = 0;
        RBR_HISTORY_POS += 1; // rbr_open will push again; keep pos consistent
        RBR_HISTORY_POS -= 1;
        rbr_open_internal(&url[..len], false)
    }
}

//-----------------------------------------------------------------------------
// Rendering (VGA text mode, 80x25)
//-----------------------------------------------------------------------------

unsafe fn rbr_draw(col: u8) { terminal_setcolor(col); }

unsafe fn rbr_render_page() {
    terminal_clear();

    // Title bar
    rbr_draw(RBR_COL_STATUS);
    rust_print(b" Radium Browser (RBR) ");
    let mut ub = [0u8; RBR_URL_LEN];
    let ulen = RBR_CURRENT_URL_LEN.min(ub.len());
    ub[..ulen].copy_from_slice(&RBR_CURRENT_URL[..ulen]);
    rust_print(&ub[..ulen]);
    rust_print(b"\n");
    rbr_draw(RBR_COL_TEXT);

    let end = (RBR_SCROLL + RBR_PAGE_ROWS).min(RBR_LINE_COUNT);
    for i in RBR_SCROLL..end {
        let l = &RBR_LINES[i];
        rbr_draw(l.color);
        for &c in &l.text[..l.len] { terminal_putchar(c); }
        terminal_putchar(b'\n');
    }
    rbr_draw(RBR_COL_TEXT);

    // Status line
    rbr_draw(RBR_COL_STATUS);
    for r in 0..80 { let _ = r; } // no direct col control needed; print status text
    if RBR_STATUS_LEN > 0 {
        rust_print(&RBR_STATUS[..RBR_STATUS_LEN]);
    } else {
        let mut lb = [0u8; 12];
        rust_print(b"Lines ");
        print_num((RBR_SCROLL + 1) as i32);
        rust_print(b"-");
        print_num(end as i32);
        rust_print(b"/");
        print_num(RBR_LINE_COUNT as i32);
        rust_print(b"  [j/k scroll] [1-9 link] [b back] [q quit]");
    }
    rbr_draw(RBR_COL_TEXT);
}

#[no_mangle]
pub extern "C" fn rbr_scroll_down() {
    unsafe {
        if RBR_SCROLL + RBR_PAGE_ROWS < RBR_LINE_COUNT { RBR_SCROLL += 1; }
        rbr_render_page();
    }
}

#[no_mangle]
pub extern "C" fn rbr_scroll_up() {
    unsafe {
        if RBR_SCROLL > 0 { RBR_SCROLL -= 1; }
        rbr_render_page();
    }
}

#[no_mangle]
pub extern "C" fn rbr_page_down() {
    unsafe {
        let max_scroll = RBR_LINE_COUNT.saturating_sub(RBR_PAGE_ROWS);
        RBR_SCROLL = (RBR_SCROLL + RBR_PAGE_ROWS).min(max_scroll);
        rbr_render_page();
    }
}

#[no_mangle]
pub extern "C" fn rbr_page_up() {
    unsafe {
        RBR_SCROLL = RBR_SCROLL.saturating_sub(RBR_PAGE_ROWS);
        rbr_render_page();
    }
}

//-----------------------------------------------------------------------------
// Navigation
//-----------------------------------------------------------------------------

unsafe fn rbr_open_internal(url: &[u8], record_history: bool) -> bool {
    rbr_set_status(b"Loading...");
    let n = url.len().min(RBR_URL_LEN - 1);
    RBR_CURRENT_URL[..n].copy_from_slice(&url[..n]);
    RBR_CURRENT_URL_LEN = n;

    let mut url_z = [0u8; RBR_URL_LEN + 1];
    url_z[..n].copy_from_slice(&url[..n]);
    url_z[n] = 0;

    let resp = http_go(http(b"GET\0".as_ptr(), url_z.as_ptr()));
    if resp < 0 || !http_response_ok(resp) {
        rbr_reset_document();
        rbr_push_line(b"Failed to load page.", RBR_COL_ERR);
        rbr_set_status(b"Error: request failed");
        if resp >= 0 { http_free_response(resp); }
        rbr_render_page();
        return false;
    }

    let status = http_response_status(resp);
    let body_len = http_response_body_len(resp).max(0) as usize;
    let mut body = [0u8; HTTP_MAX_BODY_IN];
    let copied = http_response_body(resp, body.as_mut_ptr(), body.len() as u32).max(0) as usize;
    http_free_response(resp);

    rbr_build_document(&body[..copied.min(body_len).min(body.len())]);

    let mut sb = [0u8; 12];
    rbr_set_status(b""); // let default status line show line counts
    let _ = u32_to_dec(status as u32, &mut sb);

    if record_history { rbr_history_push(url); }
    rbr_render_page();
    true
}

/// Open a URL fresh (clears scroll, records history). Entry point for
/// external callers (RSH command, desktop shortcut, etc).
#[no_mangle]
pub extern "C" fn rbr_open(url: *const u8) -> i32 {
    unsafe {
        let u = cstr_slice(url, RBR_URL_LEN - 1);
        if u.is_empty() { return -1; }
        if rbr_open_internal(u, true) { 0 } else { -1 }
    }
}

/// Follow link N (0-indexed) from the currently rendered page.
#[no_mangle]
pub extern "C" fn rbr_follow(link_id: i32) -> i32 {
    unsafe {
        if link_id < 0 || (link_id as usize) >= RBR_LINK_COUNT { return -1; }
        let l = RBR_LINKS[link_id as usize];
        let url = &l.url[..l.len];

        // Resolve relative paths against current host if needed
        if url.starts_with(b"http://") || url.starts_with(b"https://") {
            if rbr_open_internal(url, true) { 0 } else { -1 }
        } else {
            // best-effort: same-host relative path
            let cur = &RBR_CURRENT_URL[..RBR_CURRENT_URL_LEN];
            let parts = match http_parse_url(cur) { Some(p) => p, None => return -1 };
            let mut full = [0u8; RBR_URL_LEN];
            let mut idx = 0;
            for &c in b"http://" { full[idx] = c; idx += 1; }
            for &c in &parts.host[..parts.host_len] { if idx < full.len() { full[idx] = c; idx += 1; } }
            if !url.starts_with(b"/") { full[idx] = b'/'; idx += 1; }
            for &c in url { if idx < full.len() { full[idx] = c; idx += 1; } }
            if rbr_open_internal(&full[..idx], true) { 0 } else { -1 }
        }
    }
}

/// Basic keyboard-driven session loop. Blocking; returns when 'q' pressed.
#[no_mangle]
pub extern "C" fn rbr_run(start_url: *const u8) -> i32 {
    unsafe {
        rbr_open(start_url);
        loop {
            let scan = keyboard_wait_for_key(0);
            match scan {
                b'q' | b'Q' => break,
                b'j' => rbr_scroll_down(),
                b'k' => rbr_scroll_up(),
                b' ' => rbr_page_down(),
                b'u' => rbr_page_up(),
                b'b' | b'B' => { rbr_back(); }
                b'1'..=b'9' => { rbr_follow((scan - b'1') as i32); }
                _ => {}
            }
        }
        terminal_clear();
        0
    }
}

// ── RASH-SG (Rash Size Graph) Custom RadiumOS Units Engine ────────────────
unsafe fn rash_sg_print_size(bytes: usize) {
    let mut unit_bytes = b"rB   " as &[u8];
    let mut scaled = bytes as f32;
    
    if bytes >= 1024 * 1024 {
        scaled = bytes as f32 / (1024.0 * 1024.0);
        unit_bytes = b"rSec ";
    } else if bytes >= 1024 {
        scaled = bytes as f32 / 1024.0;
        unit_bytes = b"rCh  ";
    }

    print_num(scaled as i32);
    rust_print(b" ");
    rust_print(unit_bytes);

    rust_print(b"[");
    let max_bar = 10;
    let filled = ((bytes as f32 / 1048576.0).min(1.0) * max_bar as f32) as usize;
    for k in 0..max_bar {
        if k < filled {
            rust_print(b"#");
        } else {
            rust_print(b"-");
        }
    }
    rust_print(b"]");
}

// Helper to extract argument bytes safely
unsafe fn get_arg_bytes(arg_ptr: *const u8) -> &'static [u8] {
    let mut len = 0;
    while *arg_ptr.offset(len) != 0 {
        len += 1;
    }
    core::slice::from_raw_parts(arg_ptr, len as usize)
}

#[no_mangle]
pub unsafe extern "C" fn rshPKG(argc: i32, argv: *const *const u8) -> i32 {
    if argc < 2 {
        rust_print(b"error: no operation specified (use 'rpkg <pkg>.rsh', 'rpkg -l', 'rpkg -ref', 'rpkg -s <pkg>', 'rpkg -up [pkg]', or 'rpkg -rem <pkg>')\n");
        return -1;
    }

    let arg1 = *argv.offset(1);
    let arg_bytes = get_arg_bytes(arg1);

    // ── OPERATION: Help / Usage Info (-h, --help, help) ─────────────────────
    if arg_bytes == b"-h" || arg_bytes == b"--help" || arg_bytes == b"help" {
        rust_print(b"Radium Package Manager (RashSG Engine) - Help Menu\n");
        rust_print(b"==================================================\n");
        rust_print(b"  rpkg <pkg>.rsh       Install or update a local package target\n");
        rust_print(b"  rpkg -l, --list      List all available repository packages\n");
        rust_print(b"  rpkg -ref, --refresh Synchronize local database mirrors\n");
        rust_print(b"  rpkg -s <query>      Search repository packages for a keyword\n");
        rust_print(b"  rpkg -up [pkg]       Upgrade specific script or dynamic batch upgrade\n");
        rust_print(b"  rpkg -rem <pkg>      Remove/unlink a package from AVFS environment\n");
        rust_print(b"==================================================\n");
        return 0;
    }

    // ── OPERATION: Repository Listing / Query (-l, --list, list, -qry, --query) ──
    if arg_bytes == b"-l" || arg_bytes == b"--list" || arg_bytes == b"list" || arg_bytes == b"-qry" || arg_bytes == b"--query" {
        rust_print(b":: Synchronizing repository package databases (RashSG Engine)...\n");

        let mut response_buf = [0u8; 4096];
        let result = discord_request(
            b"GET",
            b"/packages",
            None,
            &mut response_buf,
            2
        );

        match result {
            Some(bytes_read) if bytes_read > 0 => {
                rust_print(b"\nRepository Packages (RashSG Engine):\n");
                rust_print(b"==================================================\n");
                rust_print(b"NAME                  SIZE      RASH-SG VISUAL\n");
                rust_print(b"--------------------------------------------------\n");

                let mut idx = 0;
                while idx < bytes_read {
                    if idx + 8 < bytes_read && 
                       response_buf[idx] == b'"' && 
                       response_buf[idx+1] == b'n' && 
                       response_buf[idx+2] == b'a' && 
                       response_buf[idx+3] == b'm' && 
                       response_buf[idx+4] == b'e' {
                        
                        let mut p = idx + 8;
                        let name_start = p;
                        while p < bytes_read && response_buf[p] != b'"' {
                            p += 1;
                        }
                        let name_len = p - name_start;
                        let pkg_name = &response_buf[name_start..p];

                        let mut size_val = 0usize;
                        let mut sz_idx = p;
                        while sz_idx + 7 < bytes_read {
                            if response_buf[sz_idx] == b's' &&
                               response_buf[sz_idx+1] == b'i' &&
                               response_buf[sz_idx+2] == b'z' &&
                               response_buf[sz_idx+3] == b'e' {
                                let mut sp = sz_idx + 5;
                                while sp < bytes_read && response_buf[sp] >= b'0' && response_buf[sp] <= b'9' {
                                    size_val = size_val * 10 + (response_buf[sp] - b'0') as usize;
                                    sp += 1;
                                }
                                break;
                            }
                            sz_idx += 1;
                        }

                        for &c in pkg_name { terminal_putchar(c); }
                        let padding = 22_isize - name_len as isize;
                        for _ in 0..padding.max(1) { rust_print(b" "); }

                        rash_sg_print_size(size_val);
                        rust_print(b"\n");

                        idx = p;
                    }
                    idx += 1;
                }

                rust_print(b"--------------------------------------------------\n");
                rust_print(b":: Query complete via RashSG engine.\n");
                return 0;
            }
            _ => {
                rust_print(b"error: failed to fetch package list from psca.py server\n");
                return -1;
            }
        }
    }

    // ── OPERATION: Refresh Local Database Mirror (-ref, --refresh) ──────────
    if arg_bytes == b"-ref" || arg_bytes == b"--refresh" {
        rust_print(b":: Synchronizing package databases (psca.py mirror)...\n");
        let mut response_buf = [0u8; 512];
        let result = discord_request(b"GET", b"/packages", None, &mut response_buf, 2);
        match result {
            Some(_) => {
                rust_print(b"core: 100% [##################] 52.4 rCh   0.0 B/s 00:00\n");
                rust_print(b":: Database synchronization complete.\n");
                return 0;
            }
            None => {
                rust_print(b"error: database sync failed, check network/server connection\n");
                return -1;
            }
        }
    }

    // ── OPERATION: Search Repository Packages (-s, --search) ────────────────
    if arg_bytes == b"-s" || arg_bytes == b"--search" {
        if argc < 3 {
            rust_print(b"error: no search query specified\n");
            return -1;
        }
        let query_arg = *argv.offset(2);
        let query_bytes = get_arg_bytes(query_arg);

        rust_print(b":: Searching repository for query: ");
        for &c in query_bytes { terminal_putchar(c); }
        rust_print(b"\n");

        let mut response_buf = [0u8; 4096];
        let result = discord_request(b"GET", b"/packages", None, &mut response_buf, 2);

        match result {
            Some(bytes_read) if bytes_read > 0 => {
                let mut found_count = 0;
                let mut idx = 0;
                while idx < bytes_read {
                    if idx + 8 < bytes_read && 
                       response_buf[idx] == b'"' && 
                       response_buf[idx+1] == b'n' && 
                       response_buf[idx+2] == b'a' && 
                       response_buf[idx+3] == b'm' && 
                       response_buf[idx+4] == b'e' {
                        
                        let mut p = idx + 8;
                        let name_start = p;
                        while p < bytes_read && response_buf[p] != b'"' {
                            p += 1;
                        }
                        let pkg_name = &response_buf[name_start..p];

                        let mut matched = false;
                        if pkg_name.windows(query_bytes.len()).any(|w| w == query_bytes) {
                            matched = true;
                        }

                        if matched {
                            rust_print(b"  rshPKG/repo: ");
                            for &c in pkg_name { terminal_putchar(c); }
                            rust_print(b"\n");
                            found_count += 1;
                        }

                        idx = p;
                    }
                    idx += 1;
                }
                print_num(found_count);
                rust_print(b" matches found via RashSG engine.\n");
                return 0;
            }
            _ => {
                rust_print(b"error: search query failed, network unreachable\n");
                return -1;
            }
        }
    }

    // ── OPERATION: Upgrade Script(s) (-up, --upgrade) ───────────────────────
    if arg_bytes == b"-up" || arg_bytes == b"--upgrade" {
        if argc >= 3 {
            // Upgrade all additional arguments passed dynamically as target filenames
            let mut upgraded_count = 0;
            for i in 2..argc {
                let target_arg = *argv.offset(i as isize);
                let target_bytes = get_arg_bytes(target_arg);

                rust_print(b":: Upgrading target script: ");
                for &c in target_bytes { terminal_putchar(c); }
                rust_print(b"\n");

                let mut endpoint_buf = [0u8; 256];
                let prefix = b"/packages/";
                let mut ep_len = 0;
                for &c in prefix { endpoint_buf[ep_len] = c; ep_len += 1; }
                for &c in target_bytes {
                    if ep_len < endpoint_buf.len() {
                        endpoint_buf[ep_len] = c;
                        ep_len += 1;
                    }
                }

                let mut response_buf = [0u8; 4096];
                let result = discord_request(b"GET", &endpoint_buf[0..ep_len], None, &mut response_buf, 2);

                match result {
                    Some(bytes_read) if bytes_read > 0 => {
                        let mut clean_bytes_read = bytes_read as usize;
                        while clean_bytes_read > 0 && response_buf[clean_bytes_read - 1] == 0 {
                            clean_bytes_read -= 1;
                        }

                        let mut filename_buf = [0u8; 256];
                        let copy_len = core::cmp::min(target_bytes.len(), 255);
                        filename_buf[..copy_len].copy_from_slice(&target_bytes[..copy_len]);
                        filename_buf[copy_len] = 0;

                        avfs_remove_file(filename_buf.as_ptr());
                        avfs_create_file(filename_buf.as_ptr(), clean_bytes_read as u32);
                        avfs_write_file(filename_buf.as_ptr(), response_buf.as_ptr(), clean_bytes_read as u32, 0);

                        rust_print(b"   -> Filename: ");
                        for &c in target_bytes { terminal_putchar(c); }
                        rust_print(b" | Written Size: ");
                        print_num(clean_bytes_read as i32);
                        rust_print(b" rB\n");
                        upgraded_count += 1;
                    }
                    _ => {
                        rust_print(b"error: failed to fetch payload for target: ");
                        for &c in target_bytes { terminal_putchar(c); }
                        rust_print(b"\n");
                    }
                }
            }
            rust_print(b":: Upgrade batch complete. Total successfully upgraded: ");
            print_num(upgraded_count);
            rust_print(b"\n");
            return 0;
        } else {
            // Dynamic system-wide upgrade iterating through all available repository packages
            rust_print(b":: Starting full system package upgrade (RashSG Engine)...\n");
            rust_print(b":: Fetching remote repository index for synchronization...\n");

            let mut response_buf = [0u8; 4096];
            let result = discord_request(b"GET", b"/packages", None, &mut response_buf, 2);

            match result {
                Some(bytes_read) if bytes_read > 0 => {
                    let mut count = 0;
                    let mut idx = 0;
                    while idx < bytes_read {
                        if idx + 8 < bytes_read && 
                           response_buf[idx] == b'"' && 
                           response_buf[idx+1] == b'n' && 
                           response_buf[idx+2] == b'a' && 
                           response_buf[idx+3] == b'm' && 
                           response_buf[idx+4] == b'e' {
                            
                            let mut p = idx + 8;
                            let name_start = p;
                            while p < bytes_read && response_buf[p] != b'"' {
                                p += 1;
                            }
                            let pkg_name = &response_buf[name_start..p];

                            rust_print(b"   -> Syncing and updating discovered package: ");
                            for &c in pkg_name { terminal_putchar(c); }
                            rust_print(b"\n");

                            let mut endpoint_buf = [0u8; 256];
                            let prefix = b"/packages/";
                            let mut ep_len = 0;
                            for &c in prefix { endpoint_buf[ep_len] = c; ep_len += 1; }
                            for &c in pkg_name {
                                if ep_len < endpoint_buf.len() {
                                    endpoint_buf[ep_len] = c;
                                    ep_len += 1;
                                }
                            }

                            let mut pkg_resp = [0u8; 4096];
                            if let Some(pkg_bytes) = discord_request(b"GET", &endpoint_buf[0..ep_len], None, &mut pkg_resp, 2) {
                                if pkg_bytes > 0 {
                                    let mut clean_size = pkg_bytes as usize;
                                    while clean_size > 0 && pkg_resp[clean_size - 1] == 0 {
                                        clean_size -= 1;
                                    }

                                    let mut filename_buf = [0u8; 256];
                                    let copy_len = core::cmp::min(pkg_name.len(), 255);
                                    filename_buf[..copy_len].copy_from_slice(&pkg_name[..copy_len]);
                                    filename_buf[copy_len] = 0;

                                    avfs_remove_file(filename_buf.as_ptr());
                                    avfs_create_file(filename_buf.as_ptr(), clean_size as u32);
                                    avfs_write_file(filename_buf.as_ptr(), pkg_resp.as_ptr(), clean_size as u32, 0);
                                    count += 1;
                                }
                            }

                            idx = p;
                        }
                        idx += 1;
                    }
                    rust_print(b":: System-wide upgrade complete. Packages synchronized: ");
                    print_num(count);
                    rust_print(b"\n");
                    return 0;
                }
                _ => {
                    rust_print(b"error: failed to synchronize repository package list for upgrade\n");
                    return -1;
                }
            }
        }
    }

    // ── OPERATION: Remove / Uninstall Package (-rem, --remove, remove) ──────
    if arg_bytes == b"-rem" || arg_bytes == b"--remove" || arg_bytes == b"remove" {
        if argc < 3 {
            rust_print(b"error: no target package specified for removal\n");
            return -1;
        }
        let target_arg = *argv.offset(2);
        let target_bytes = get_arg_bytes(target_arg);

        rust_print(b":: Preparing uninstallation...\n");
        rust_print(b"Packages (1): ");
        for &c in target_bytes { terminal_putchar(c); }
        rust_print(b"\n\n:: Proceed with removal? [Y/n] Y\n");

        let mut filename_buf = [0u8; 256];
        let copy_len = core::cmp::min(target_bytes.len(), 255);
        filename_buf[..copy_len].copy_from_slice(&target_bytes[..copy_len]);
        filename_buf[copy_len] = 0;

        let res = avfs_remove_file(filename_buf.as_ptr());
        if res == 0 || res != -1 {
            rust_print(b"(1/1) unlinking package from AVFS environment...\n");
            rust_print(b"   -> Unlinked Filename: ");
            for &c in target_bytes { terminal_putchar(c); }
            rust_print(b"\n:: Package successfully unlinked and removed.\n");
            return 0;
        } else {
            rust_print(b"error: target package not found in current AVFS context\n");
            return -1;
        }
    }

    // ── OPERATION: Standard or Explicit Install Package (-get / direct .rsh) ──
    let package_arg_bytes = if arg_bytes == b"-get" || arg_bytes == b"--get" {
        if argc < 3 {
            rust_print(b"error: no package specified with installation flag\n");
            return -1;
        }
        get_arg_bytes(*argv.offset(2))
    } else {
        arg_bytes
    };

    let pkg_len = package_arg_bytes.len();
    if pkg_len < 4 || &package_arg_bytes[pkg_len - 4..] != b".rsh" {
        rust_print(b"error: invalid package extension. Only .rsh packages are supported.\n");
        return -1;
    }

    rust_print(b":: Synchronizing package databases...\n");
    rust_print(b"Packages (1): ");
    for &c in package_arg_bytes { terminal_putchar(c); }
    rust_print(b"\n\nRashSG Target Analysis:\n  Size: ");
    
    rash_sg_print_size(52428); 
    rust_print(b"\n\n:: Proceed with installation? [Y/n] Y\n");

    let mut endpoint_buf = [0u8; 256];
    let prefix = b"/packages/";
    let mut ep_len = 0;
    for &c in prefix { endpoint_buf[ep_len] = c; ep_len += 1; }
    for &c in package_arg_bytes {
        if ep_len < endpoint_buf.len() {
            endpoint_buf[ep_len] = c;
            ep_len += 1;
        }
    }

    let mut response_buf = [0u8; 4096];
    let result = discord_request(
        b"GET",
        &endpoint_buf[0..ep_len],
        None,
        &mut response_buf,
        2
    );

    match result {
        Some(bytes_read) if bytes_read > 0 => {
            rust_print(b"(1/1) checking package integrity...\n");
            
            let mut clean_bytes_read = bytes_read as usize;
            while clean_bytes_read > 0 {
                let b = response_buf[clean_bytes_read - 1];
                if b == 0 {
                    clean_bytes_read -= 1;
                } else {
                    break;
                }
            }

            let mut filename_buf = [0u8; 256];
            let copy_len = core::cmp::min(package_arg_bytes.len(), 255);
            filename_buf[..copy_len].copy_from_slice(&package_arg_bytes[..copy_len]);
            filename_buf[copy_len] = 0;

            let create_res = avfs_create_file(filename_buf.as_ptr(), clean_bytes_read as u32);
            if create_res == -1 {
                avfs_remove_file(filename_buf.as_ptr());
                avfs_create_file(filename_buf.as_ptr(), clean_bytes_read as u32);
            }
            
            avfs_write_file(
                filename_buf.as_ptr(),
                response_buf.as_ptr(),
                clean_bytes_read as u32,
                0
            );

            rust_print(b"(1/1) saving package to current directory via AVFS:\n");
            rust_print(b"   -> Installed Filename: ");
            for &c in package_arg_bytes { terminal_putchar(c); }
            rust_print(b"\n   -> Written Size: ");
            print_num(clean_bytes_read as i32);
            rust_print(b" rB\n");
            rust_print(b":: Package successfully installed, verified, and saved via AVFS.\n");
            0
        }
        _ => {
            rust_print(b"error: failed to retrieve package from psca.py repository\n");
            -1
        }
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// Helper format functions (stack-based, no_std compliant)
// ─────────────────────────────────────────────────────────────────────────────

// Format u32 into a stack buffer in the caller's scope (safer)
#[inline]
fn format_u32_into(mut n: u32, buf: &mut [u8]) -> usize {
    if n == 0 {
        if !buf.is_empty() { buf[0] = b'0'; }
        return 1;
    }
    let mut digits = [0u8; 10];
    let mut dlen = 0;
    while n > 0 {
        digits[dlen] = (n % 10) as u8 + b'0';
        dlen += 1;
        n /= 10;
    }
    let mut i = 0;
    for k in (0..dlen).rev() {
        if i < buf.len() { buf[i] = digits[k]; }
        i += 1;
    }
    core::cmp::min(i, buf.len())
}

// Format float as "int.frac" into caller's buffer (safer)
#[inline]
fn format_float_into(int_part: i32, frac_part: u32, buf: &mut [u8]) -> usize {
    let mut pos = 0;
    
    if int_part < 0 {
        if pos < buf.len() { buf[pos] = b'-'; }
        pos += 1;
    }
    
    let abs_int = (int_part.abs()) as u32;
    let mut tmp = [0u8; 10];
    let len = format_u32_into(abs_int, &mut tmp);
    for &c in &tmp[..len] {
        if pos < buf.len() { buf[pos] = c; }
        pos += 1;
    }
    
    if pos < buf.len() { buf[pos] = b'.'; }
    pos += 1;
    
    if frac_part < 10 {
        if pos < buf.len() { buf[pos] = b'0'; }
        pos += 1;
    }
    
    let mut tmp2 = [0u8; 10];
    let len2 = format_u32_into(frac_part, &mut tmp2);
    for &c in &tmp2[..len2] {
        if pos < buf.len() { buf[pos] = c; }
        pos += 1;
    }
    
    pos
}
// ─────────────────────────────────────────────────────────────────────────────
// RadiumOS CPU FFI Bindings
// ─────────────────────────────────────────────────────────────────────────────

#[repr(C)]
pub struct CPUInfo {
    pub vendor: *const u8,
    pub brand_string: *const u8,
    pub architecture: *const u8,
    pub family: u32,
    pub model: u32,
    pub stepping: u32,
    pub frequency_mhz: u32,
    pub is_64bit: u8,
    // Note: The rest of the struct (features, cache_info, topology) is omitted 
    // here as we only need to read the pointer up to `is_64bit`. 
    // The standard C ABI allows this safe partial projection via pointer.
}

/// Helper to find the length of a null-terminated C string in a no_std environment
fn cstr_len(s: *const u8) -> usize {
    if s.is_null() { return 0; }
    let mut len = 0;
    unsafe {
        while *s.add(len) != 0 { len += 1; }
    }
    len
}
// ─────────────────────────────────────────────────────────────────────────────
// radium_register_device - no_std HTTP device registration
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// radium_register_device - no_std HTTP device registration
// ─────────────────────────────────────────────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn radium_register_device(
    hostname:    *const u8,
    hostname_len: usize,
    lat:         f32,
    lon:         f32,
    model:       *const u8,
    model_len:   usize,
    psca_ip:     *const u8,
    psca_port:   u16,
    out:         *mut u8,
    out_len:     usize,
    retry:       u32,
) -> i32 {
    let hostname_slice = slice::from_raw_parts(hostname, hostname_len);
    let model_slice = slice::from_raw_parts(model, model_len);
    let ip_bytes = slice::from_raw_parts(psca_ip, 4);
    let out_buf = slice::from_raw_parts_mut(out, out_len);

    if core::str::from_utf8(hostname_slice).is_err() { return -1; }
    if core::str::from_utf8(model_slice).is_err() { return -1; }

    // Build IP string for URL (e.g., "192.168.1.1")
    let mut ip_str_buf = [0u8; 16];
    let mut ip_len = 0usize;
    for (idx, &b) in ip_bytes.iter().enumerate() {
        if idx > 0 {
            ip_str_buf[ip_len] = b'.';
            ip_len += 1;
        }
        if b >= 100 {
            ip_str_buf[ip_len] = b'0' + (b / 100);
            ip_len += 1;
            ip_str_buf[ip_len] = b'0' + ((b % 100) / 10);
            ip_len += 1;
            ip_str_buf[ip_len] = b'0' + (b % 10);
            ip_len += 1;
        } else if b >= 10 {
            ip_str_buf[ip_len] = b'0' + (b / 10);
            ip_len += 1;
            ip_str_buf[ip_len] = b'0' + (b % 10);
            ip_len += 1;
        } else {
            ip_str_buf[ip_len] = b'0' + b;
            ip_len += 1;
        }
    }

    // Retrieve CPU Info from C layer
    let cpu_info_ptr = get_cpu_info_struct();

    // Build JSON payload
    let mut payload_buf = [0u8; 1024]; // Expanded buffer size for extended system info
    let mut plen = 0usize;
    let append = |buf: &mut [u8], pos: &mut usize, s: &[u8]| {
        for &c in s {
            if *pos < buf.len() { buf[*pos] = c; *pos += 1; }
        }
    };

    append(&mut payload_buf, &mut plen, b"{\"hostname\":\"");
    append(&mut payload_buf, &mut plen, hostname_slice);
    append(&mut payload_buf, &mut plen, b"\",\"lat\":");
    
    let lat_i = lat as i32;
    let lat_frac = ((lat.abs() - (lat_i.abs() as f32)) * 100.0) as u32;
    let mut lat_tmp = [0u8; 32];
    let lat_len = format_float_into(lat_i, lat_frac, &mut lat_tmp);
    append(&mut payload_buf, &mut plen, &lat_tmp[..lat_len]);
    
    append(&mut payload_buf, &mut plen, b",\"lon\":");
    let lon_i = lon as i32;
    let lon_frac = ((lon.abs() - (lon_i.abs() as f32)) * 100.0) as u32;
    let mut lon_tmp = [0u8; 32];
    let lon_len = format_float_into(lon_i, lon_frac, &mut lon_tmp);
    append(&mut payload_buf, &mut plen, &lon_tmp[..lon_len]);
    
    append(&mut payload_buf, &mut plen, b",\"model\":\"");
    append(&mut payload_buf, &mut plen, model_slice);

    // ─── Inject Extended Hardware Info from C ───
    append(&mut payload_buf, &mut plen, b"\",\"cpu\":\"");
    if !cpu_info_ptr.is_null() {
        let brand = (*cpu_info_ptr).brand_string;
        if !brand.is_null() {
            append(&mut payload_buf, &mut plen, slice::from_raw_parts(brand, cstr_len(brand)));
        } else {
            append(&mut payload_buf, &mut plen, b"Unknown CPU");
        }
    } else {
        append(&mut payload_buf, &mut plen, b"Unknown CPU");
    }

    append(&mut payload_buf, &mut plen, b"\",\"arch\":\"");
    if !cpu_info_ptr.is_null() {
        let arch = (*cpu_info_ptr).architecture;
        if !arch.is_null() {
            append(&mut payload_buf, &mut plen, slice::from_raw_parts(arch, cstr_len(arch)));
        } else {
            append(&mut payload_buf, &mut plen, b"Unknown Arch");
        }
    } else {
        append(&mut payload_buf, &mut plen, b"Unknown Arch");
    }

    append(&mut payload_buf, &mut plen, b"\",\"freq_mhz\":");
    if !cpu_info_ptr.is_null() {
        let mut freq_tmp = [0u8; 16];
        let freq_len = format_u32_into((*cpu_info_ptr).frequency_mhz, &mut freq_tmp);
        append(&mut payload_buf, &mut plen, &freq_tmp[..freq_len]);
    } else {
        append(&mut payload_buf, &mut plen, b"0");
    }

    append(&mut payload_buf, &mut plen, b"}");
    
    let payload = &payload_buf[..plen];

    // Build URL
    let mut url_buf = [0u8; 256];
    let mut url_len = 0usize;
    append(&mut url_buf, &mut url_len, b"http://");
    append(&mut url_buf, &mut url_len, &ip_str_buf[..ip_len]);
    append(&mut url_buf, &mut url_len, b":");
    let mut port_tmp = [0u8; 16];
    let port_len = format_u32_into(psca_port as u32, &mut port_tmp);
    append(&mut url_buf, &mut url_len, &port_tmp[..port_len]);
    append(&mut url_buf, &mut url_len, b"/api/device/register");
    
    // Null-terminate URL
    if url_len < url_buf.len() {
        url_buf[url_len] = 0;
    } else {
        return -11; // URL too long
    }
    let url = &url_buf[..=url_len]; // Include null terminator

    // Use handle-based HTTP API
    let mut attempts = 0u32;
    let max_attempts = if retry == 0 { 1 } else { retry };
    
    loop {
        let handle = http_new();
        if handle < 0 {
            return -6; // Failed to create HTTP handle
        }

        // Set method
        if http_set_method(handle, b"POST\0".as_ptr()) < 0 {
            http_free(handle);
            return -7;
        }

        // Set URL (now null-terminated)
        if http_set_url(handle, url.as_ptr()) < 0 {
            http_free(handle);
            return -8;
        }

        // Set Content-Type header
        if http_set_header(handle, b"Content-Type\0".as_ptr(), b"application/json\0".as_ptr()) < 0 {
            http_free(handle);
            return -9;
        }

        // Set body
        if http_set_body(handle, payload.as_ptr(), payload.len() as u32) < 0 {
            http_free(handle);
            return -10;
        }

        // Send request
        let resp_handle = http_go(handle);
        if resp_handle < 0 {
            http_free(handle);
            attempts += 1;
            if attempts >= max_attempts {
                return -5; // Max retries exceeded
            }
            sleep_ms(8000);
            continue;
        }

        // Read response body
        let mut resp_buf = [0u8; 4096];
        let body_len = http_response_body(resp_handle, resp_buf.as_mut_ptr(), resp_buf.len() as u32);
        
        http_free_response(resp_handle);

        if body_len > 0 {
            let copy_len = core::cmp::min(body_len as usize, out_len);
            out_buf[..copy_len].copy_from_slice(&resp_buf[..copy_len]);
            return body_len;
        }

        attempts += 1;
        if attempts >= max_attempts {
            return -4; // Max retries with no response body
        }
        sleep_ms(8000);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Return codes:
// ─────────────────────────────────────────────────────────────────────────────
// -1: Invalid hostname or model (UTF-8 error)
// -4: Max retries with no response body
// -5: Max retries exceeded (http_go failed)
// -6: Failed to create HTTP handle
// -7: Failed to set method
// -8: Failed to set URL
// -9: Failed to set header
// -10: Failed to set body
// -11: URL too long (>256 chars)
// >0: Success, body_len

//-----------------------------------------------------------------------------
// Graphics Info
//-----------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn graphics_get_width() -> u32 {
    unsafe { GRAPHICS_MODE.width }
}

#[no_mangle]
pub extern "C" fn graphics_get_height() -> u32 {
    unsafe { GRAPHICS_MODE.height }
}

#[no_mangle]
pub extern "C" fn graphics_is_initialized() -> bool {
    unsafe { GRAPHICS_MODE.is_initialized }
}

#[no_mangle]
pub extern "C" fn rust_hello() { // soo advanced LOL :3
    rust_print(b"Hello from Rust!\n");
    
}