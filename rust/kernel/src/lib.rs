#![cfg_attr(target_os = "none", no_std)]

pub use tunix_elf::ffi as elf;
pub use tunix_lz4::ffi as lz4;

#[cfg(target_os = "none")]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    unsafe extern "C" {
        #[link_name = "panic"]
        fn kernel_panic(message: *const core::ffi::c_char) -> !;
    }
    unsafe { kernel_panic(c"rust: kernel code panicked".as_ptr()) }
}
