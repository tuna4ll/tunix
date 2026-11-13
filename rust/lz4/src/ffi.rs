use crate::{TABLE_ENTRIES, compress, decompress};

unsafe fn bytes<'a>(pointer: *const u8, length: usize) -> Option<&'a [u8]> {
    if pointer.is_null() || isize::try_from(length).is_err() {
        return None;
    }
    Some(unsafe { core::slice::from_raw_parts(pointer, length) })
}

unsafe fn bytes_mut<'a>(pointer: *mut u8, length: usize) -> Option<&'a mut [u8]> {
    if pointer.is_null() || isize::try_from(length).is_err() {
        return None;
    }
    Some(unsafe { core::slice::from_raw_parts_mut(pointer, length) })
}

/// # Safety
/// `source` must point to `length` readable bytes, `output` to `capacity` writable bytes and
/// `table` to `TUNIX_LZ4_TABLE_ENTRIES` writable 16-bit entries; none of them may overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tunix_lz4_compress(
    source: *const u8,
    length: usize,
    output: *mut u8,
    capacity: usize,
    table: *mut u16,
) -> usize {
    let (Some(source), Some(output)) =
        (unsafe { bytes(source, length) }, unsafe { bytes_mut(output, capacity) })
    else {
        return 0;
    };
    if table.is_null() {
        return 0;
    }
    let table = unsafe { core::slice::from_raw_parts_mut(table, TABLE_ENTRIES) };
    compress(source, output, table).unwrap_or(0)
}

/// # Safety
/// `input` must point to `length` readable bytes and `output` to `size` writable bytes that do
/// not overlap them.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tunix_lz4_decompress(
    input: *const u8,
    length: usize,
    output: *mut u8,
    size: usize,
) -> i32 {
    let (Some(input), Some(output)) =
        (unsafe { bytes(input, length) }, unsafe { bytes_mut(output, size) })
    else {
        return -1;
    };
    match decompress(input, output) {
        Ok(()) => 0,
        Err(_) => -1,
    }
}
