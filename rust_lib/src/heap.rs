use core::alloc::{GlobalAlloc, Layout};
use core::ptr::{self, NonNull};
use linked_list_allocator::LockedHeap;

struct KernelHeap;

#[global_allocator]
static HEAP: KernelHeap = KernelHeap;
static ARENA: LockedHeap = LockedHeap::empty();

// The tick that handles Ctrl+C must never land while the allocator lock is held, or the lock would stay taken.
struct NoIrq(u32);

impl NoIrq {
    #[inline(always)]
    unsafe fn new() -> Self {
        let flags: u32;
        core::arch::asm!("pushfd", "pop {}", "cli", out(reg) flags);
        NoIrq(flags)
    }
}

impl Drop for NoIrq {
    #[inline(always)]
    fn drop(&mut self) {
        if self.0 & (1 << 9) != 0 {
            unsafe { core::arch::asm!("sti") };
        }
    }
}

unsafe impl GlobalAlloc for KernelHeap {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let _irq = NoIrq::new();
        let mut heap = ARENA.lock();
        if heap.size() == 0 {
            // This region is reserved by linker.ld and loaded with the kernel.
            let start = ptr::addr_of!(crate::_heap_start) as *mut u8;
            let end = ptr::addr_of!(crate::_heap_end) as usize;
            heap.init(start, end - start as usize);
        }
        heap.allocate_first_fit(layout)
            .map_or(ptr::null_mut(), NonNull::as_ptr)
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        let _irq = NoIrq::new();
        ARENA
            .lock()
            .deallocate(NonNull::new_unchecked(pointer), layout);
    }
}

// malloc / free / size for C code (the Doom port): blocks carry a 16-byte header holding their total size.
#[no_mangle]
pub unsafe extern "C" fn rust_c_alloc(size: usize) -> *mut u8 {
    let Some(total) = size.checked_add(16) else { return ptr::null_mut() };
    let Ok(layout) = Layout::from_size_align(total, 16) else { return ptr::null_mut() };
    let p = HEAP.alloc(layout);
    if p.is_null() { return p; }
    *(p as *mut usize) = total;
    p.add(16)
}

#[no_mangle]
pub unsafe extern "C" fn rust_c_free(p: *mut u8) {
    if p.is_null() { return; }
    let base = p.sub(16);
    let total = *(base as *mut usize);
    HEAP.dealloc(base, Layout::from_size_align_unchecked(total, 16));
}

#[no_mangle]
pub unsafe extern "C" fn rust_c_size(p: *mut u8) -> usize {
    *(p.sub(16) as *mut usize) - 16
}
