use tunix_elf::{Plan, Segment, Target};

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ReferencePlan {
    pub kind: u16,
    pub phnum: u16,
    pub load_count: u16,
    pub entry_wrapped: u16,
    pub load_bias: u64,
    pub entry: u64,
    pub phdr: u64,
    pub image_start: u64,
    pub image_end: u64,
    pub interp_offset: u64,
    pub interp_size: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ReferenceSegment {
    pub page_start: u64,
    pub page_end: u64,
    pub address: u64,
    pub writable: u32,
    pub executable: u32,
}

unsafe extern "C" {
    fn reference_header_span(
        bytes: *const u8,
        length: usize,
        file_size: u64,
        machine: u16,
        span: *mut u64,
    ) -> i32;
    fn reference_plan(
        bytes: *const u8,
        length: usize,
        file_size: u64,
        machine: u16,
        preferred_base: u64,
        user_limit: u64,
        plan: *mut ReferencePlan,
        segments: *mut ReferenceSegment,
        capacity: usize,
    ) -> i32;
    fn reference_interp_valid(path: *const u8, length: usize) -> i32;
}

pub fn old_header_span(bytes: &[u8], file_size: u64, machine: u16) -> Option<u64> {
    let mut span = 0;
    let status = unsafe {
        reference_header_span(bytes.as_ptr(), bytes.len(), file_size, machine, &mut span)
    };
    (status == 0).then_some(span)
}

pub fn old_plan(
    bytes: &[u8],
    file_size: u64,
    preferred_base: u64,
    target: Target,
) -> Option<(ReferencePlan, Vec<ReferenceSegment>)> {
    let mut plan = ReferencePlan::default();
    let mut segments = vec![ReferenceSegment::default(); 2048];
    let status = unsafe {
        reference_plan(
            bytes.as_ptr(),
            bytes.len(),
            file_size,
            target.machine,
            preferred_base,
            target.user_limit,
            &mut plan,
            segments.as_mut_ptr(),
            segments.len(),
        )
    };
    if status != 0 {
        return None;
    }
    segments.truncate(usize::from(plan.load_count));
    Some((plan, segments))
}

pub fn old_interp_valid(path: &[u8]) -> bool {
    unsafe { reference_interp_valid(path.as_ptr(), path.len()) != 0 }
}

pub fn same_plan(new: &Plan, old: &ReferencePlan) -> bool {
    new.kind == old.kind
        && new.phnum == old.phnum
        && new.load_count == old.load_count
        && new.load_bias == old.load_bias
        && new.entry == old.entry
        && new.phdr == old.phdr
        && new.image_start == old.image_start
        && new.image_end == old.image_end
        && new.interp_offset == old.interp_offset
        && new.interp_size == old.interp_size
}

pub fn same_segment(new: &Segment, old: &ReferenceSegment) -> bool {
    new.page_start == old.page_start
        && new.page_end == old.page_end
        && new.address == old.address
        && u32::from(new.writable) == old.writable
        && u32::from(new.executable) == old.executable
}
