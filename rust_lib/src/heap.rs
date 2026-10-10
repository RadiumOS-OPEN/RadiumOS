use core::alloc::{GlobalAlloc, Layout};
use core::ptr::{self, NonNull};
use linked_list_allocator::LockedHeap;

struct KernelHeap;

#[global_allocator]
static HEAP: KernelHeap = KernelHeap;
static ARENA: LockedHeap = LockedHeap::empty();

unsafe impl GlobalAlloc for KernelHeap {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
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
        ARENA
            .lock()
            .deallocate(NonNull::new_unchecked(pointer), layout);
    }
}
