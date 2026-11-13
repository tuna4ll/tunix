use crate::{Plan, Segment, Target, header_span, interp_path_valid, plan};

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TunixElfPlan {
    pub kind: u16,
    pub phnum: u16,
    pub load_count: u16,
    pub reserved: u16,
    pub load_bias: u64,
    pub entry: u64,
    pub phdr: u64,
    pub image_start: u64,
    pub image_end: u64,
    pub interp_offset: u64,
    pub interp_size: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TunixElfSegment {
    pub address: u64,
    pub file_offset: u64,
    pub file_size: u64,
    pub memory_size: u64,
    pub page_start: u64,
    pub page_end: u64,
    pub writable: u8,
    pub executable: u8,
    pub reserved: [u8; 6],
}

const _: () = assert!(core::mem::size_of::<TunixElfPlan>() == 64);
const _: () = assert!(core::mem::size_of::<TunixElfSegment>() == 56);

impl From<&Plan> for TunixElfPlan {
    fn from(plan: &Plan) -> Self {
        Self {
            kind: plan.kind,
            phnum: plan.phnum,
            load_count: plan.load_count,
            reserved: 0,
            load_bias: plan.load_bias,
            entry: plan.entry,
            phdr: plan.phdr,
            image_start: plan.image_start,
            image_end: plan.image_end,
            interp_offset: plan.interp_offset,
            interp_size: plan.interp_size,
        }
    }
}

impl From<Segment> for TunixElfSegment {
    fn from(segment: Segment) -> Self {
        Self {
            address: segment.address,
            file_offset: segment.file_offset,
            file_size: segment.file_size,
            memory_size: segment.memory_size,
            page_start: segment.page_start,
            page_end: segment.page_end,
            writable: u8::from(segment.writable),
            executable: u8::from(segment.executable),
            reserved: [0; 6],
        }
    }
}

unsafe fn bytes<'a>(pointer: *const u8, length: usize) -> &'a [u8] {
    if pointer.is_null() || length == 0 || isize::try_from(length).is_err() {
        return &[];
    }
    unsafe { core::slice::from_raw_parts(pointer, length) }
}

/// # Safety
/// `header` must point to `length` readable bytes, or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tunix_elf_header_span(
    header: *const u8,
    length: usize,
    file_size: u64,
    machine: u16,
) -> i64 {
    let header = unsafe { bytes(header, length) };
    header_span(header, file_size, machine)
        .ok()
        .and_then(|span| i64::try_from(span).ok())
        .unwrap_or(-1)
}

/// # Safety
/// `headers` must point to `length` readable bytes, or be null. `out` must point to a writable
/// plan. `segments` must point to `capacity` writable segments, or be null when `capacity` is 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tunix_elf_plan(
    headers: *const u8,
    length: usize,
    file_size: u64,
    machine: u16,
    preferred_base: u64,
    user_limit: u64,
    out: *mut TunixElfPlan,
    segments: *mut TunixElfSegment,
    capacity: usize,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    let headers = unsafe { bytes(headers, length) };
    let Ok(image) = plan(headers, file_size, preferred_base, Target { machine, user_limit }) else {
        return -1;
    };
    let summary = TunixElfPlan::from(image.plan());
    unsafe { out.write(summary) };
    if segments.is_null() || capacity < usize::from(summary.load_count) {
        return 0;
    }
    let mut written = 0usize;
    for segment in image.segments() {
        if written >= capacity {
            return -1;
        }
        unsafe { segments.add(written).write(TunixElfSegment::from(segment)) };
        written = written.saturating_add(1);
    }
    if written == usize::from(summary.load_count) { 0 } else { -1 }
}

/// # Safety
/// `path` must point to `length` readable bytes, or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tunix_elf_interp_valid(path: *const u8, length: usize) -> i32 {
    i32::from(interp_path_valid(unsafe { bytes(path, length) }))
}
