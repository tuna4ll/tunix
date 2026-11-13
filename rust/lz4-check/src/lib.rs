use std::os::raw::{c_char, c_int};

unsafe extern "C" {
    fn LZ4_compress_default(
        source: *const c_char,
        dest: *mut c_char,
        size: c_int,
        capacity: c_int,
    ) -> c_int;
    fn LZ4_decompress_safe(
        source: *const c_char,
        dest: *mut c_char,
        compressed: c_int,
        capacity: c_int,
    ) -> c_int;
}

pub fn reference_compress(data: &[u8]) -> Vec<u8> {
    let capacity = tunix_lz4::bound(data.len()) + 64;
    let mut out = vec![0u8; capacity];
    let size = unsafe {
        LZ4_compress_default(
            data.as_ptr().cast(),
            out.as_mut_ptr().cast(),
            c_int::try_from(data.len()).expect("small input"),
            c_int::try_from(capacity).expect("small output"),
        )
    };
    assert!(size > 0, "liblz4 refused to compress");
    out.truncate(usize::try_from(size).expect("positive"));
    out
}

pub fn reference_decompress(packed: &[u8], size: usize) -> Option<Vec<u8>> {
    let mut out = vec![0u8; size];
    let produced = unsafe {
        LZ4_decompress_safe(
            packed.as_ptr().cast(),
            out.as_mut_ptr().cast(),
            c_int::try_from(packed.len()).ok()?,
            c_int::try_from(size).ok()?,
        )
    };
    (usize::try_from(produced).ok()? == size).then_some(out)
}
