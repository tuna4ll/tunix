#![cfg_attr(target_os = "none", no_std)]
#![deny(
    unsafe_op_in_unsafe_fn,
    clippy::arithmetic_side_effects,
    clippy::as_conversions,
    clippy::expect_used,
    clippy::indexing_slicing,
    clippy::panic,
    clippy::todo,
    clippy::unimplemented,
    clippy::unreachable,
    clippy::unwrap_used
)]

pub mod ffi;

pub const HEADER_SIZE: usize = 64;
pub const PROGRAM_SIZE: usize = 56;
const PROGRAM_SIZE_BYTES: u64 = 56;
pub const PAGE_SIZE: u64 = 4096;
pub const LOWEST_ADDRESS: u64 = 0x10000;
pub const MAX_HEADER_SPAN: u64 = 64 * 1024;
pub const MAX_INTERP_SIZE: u64 = 4096;

pub const ET_EXEC: u16 = 2;
pub const ET_DYN: u16 = 3;
pub const PT_LOAD: u32 = 1;
pub const PT_INTERP: u32 = 3;
pub const PF_X: u32 = 1;
pub const PF_W: u32 = 2;

const ELFCLASS64: u8 = 2;
const ELFDATA2LSB: u8 = 1;
const EV_CURRENT: u8 = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Truncated,
    Ident,
    Kind,
    Machine,
    Layout,
    Program,
    Interp,
    Range,
    Entry,
    Phdr,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Header {
    pub kind: u16,
    pub entry: u64,
    pub phoff: u64,
    pub phnum: u16,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Program {
    pub kind: u32,
    pub flags: u32,
    pub offset: u64,
    pub vaddr: u64,
    pub filesz: u64,
    pub memsz: u64,
    pub align: u64,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Plan {
    pub kind: u16,
    pub phnum: u16,
    pub load_count: u16,
    pub load_bias: u64,
    pub entry: u64,
    pub phdr: u64,
    pub image_start: u64,
    pub image_end: u64,
    pub interp_offset: u64,
    pub interp_size: u64,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Segment {
    pub address: u64,
    pub file_offset: u64,
    pub file_size: u64,
    pub memory_size: u64,
    pub page_start: u64,
    pub page_end: u64,
    pub writable: bool,
    pub executable: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Target {
    pub machine: u16,
    pub user_limit: u64,
}

fn field<const N: usize>(bytes: &[u8], at: usize) -> Option<[u8; N]> {
    bytes.get(at..at.checked_add(N)?)?.try_into().ok()
}

fn read_u16(bytes: &[u8], at: usize) -> Option<u16> {
    field(bytes, at).map(u16::from_le_bytes)
}

fn read_u32(bytes: &[u8], at: usize) -> Option<u32> {
    field(bytes, at).map(u32::from_le_bytes)
}

fn read_u64(bytes: &[u8], at: usize) -> Option<u64> {
    field(bytes, at).map(u64::from_le_bytes)
}

fn page_down(value: u64) -> u64 {
    value & !(PAGE_SIZE - 1)
}

fn page_up(value: u64) -> Option<u64> {
    Some(page_down(value.checked_add(PAGE_SIZE - 1)?))
}

fn table_size(phnum: u16) -> u64 {
    u64::from(phnum).saturating_mul(PROGRAM_SIZE_BYTES)
}

pub fn parse_header(bytes: &[u8], file_size: u64, machine: u16) -> Result<Header, Error> {
    if file_size < 64 || bytes.len() < HEADER_SIZE {
        return Err(Error::Truncated);
    }
    let ident: [u8; 16] = field(bytes, 0).ok_or(Error::Truncated)?;
    if ident[..4] != *b"\x7fELF" {
        return Err(Error::Ident);
    }
    if ident[4] != ELFCLASS64 || ident[5] != ELFDATA2LSB || ident[6] != EV_CURRENT {
        return Err(Error::Ident);
    }
    let read = |at| read_u16(bytes, at).ok_or(Error::Truncated);
    let kind = read(16)?;
    if kind != ET_EXEC && kind != ET_DYN {
        return Err(Error::Kind);
    }
    if read(18)? != machine {
        return Err(Error::Machine);
    }
    if read_u32(bytes, 20).ok_or(Error::Truncated)? != u32::from(EV_CURRENT) {
        return Err(Error::Ident);
    }
    let entry = read_u64(bytes, 24).ok_or(Error::Truncated)?;
    let phoff = read_u64(bytes, 32).ok_or(Error::Truncated)?;
    let ehsize = read(52)?;
    let phentsize = read(54)?;
    let phnum = read(56)?;
    if usize::from(ehsize) != HEADER_SIZE || usize::from(phentsize) != PROGRAM_SIZE || phnum == 0 {
        return Err(Error::Layout);
    }
    let room = file_size.checked_sub(phoff).ok_or(Error::Layout)?;
    if table_size(phnum) > room {
        return Err(Error::Layout);
    }
    Ok(Header { kind, entry, phoff, phnum })
}

pub fn header_span(bytes: &[u8], file_size: u64, machine: u16) -> Result<u64, Error> {
    let header = parse_header(bytes, file_size, machine)?;
    let span = header.phoff.checked_add(table_size(header.phnum)).ok_or(Error::Layout)?;
    if span > file_size || span > MAX_HEADER_SPAN {
        return Err(Error::Layout);
    }
    Ok(span)
}

fn program(bytes: &[u8], header: &Header, index: u16) -> Option<Program> {
    let start = usize::try_from(header.phoff).ok()?;
    let at = start.checked_add(usize::from(index).checked_mul(PROGRAM_SIZE)?)?;
    let entry = bytes.get(at..at.checked_add(PROGRAM_SIZE)?)?;
    Some(Program {
        kind: read_u32(entry, 0)?,
        flags: read_u32(entry, 4)?,
        offset: read_u64(entry, 8)?,
        vaddr: read_u64(entry, 16)?,
        filesz: read_u64(entry, 32)?,
        memsz: read_u64(entry, 40)?,
        align: read_u64(entry, 48)?,
    })
}

fn within_file(offset: u64, size: u64, file_size: u64) -> bool {
    file_size.checked_sub(offset).is_some_and(|room| size <= room)
}

fn check_program(program: &Program, file_size: u64) -> Result<(), Error> {
    if program.memsz < program.filesz {
        return Err(Error::Program);
    }
    if program.filesz != 0 && !within_file(program.offset, program.filesz, file_size) {
        return Err(Error::Program);
    }
    if program.align > 1 && !program.align.is_power_of_two() {
        return Err(Error::Program);
    }
    if program.vaddr & (PAGE_SIZE - 1) != program.offset & (PAGE_SIZE - 1) {
        return Err(Error::Program);
    }
    let end = program.vaddr.checked_add(program.memsz).ok_or(Error::Program)?;
    page_up(end).ok_or(Error::Range)?;
    Ok(())
}

fn segment(program: &Program, load_bias: u64, target: &Target) -> Result<Segment, Error> {
    let address = program.vaddr.checked_add(load_bias).ok_or(Error::Range)?;
    let end = address.checked_add(program.memsz).ok_or(Error::Range)?;
    if address < LOWEST_ADDRESS || address >= target.user_limit {
        return Err(Error::Range);
    }
    if program.memsz > target.user_limit.saturating_sub(address) {
        return Err(Error::Range);
    }
    Ok(Segment {
        address,
        file_offset: program.offset,
        file_size: program.filesz,
        memory_size: program.memsz,
        page_start: page_down(address),
        page_end: page_up(end).ok_or(Error::Range)?,
        writable: program.flags & PF_W != 0,
        executable: program.flags & PF_X != 0,
    })
}

pub struct Image<'a> {
    bytes: &'a [u8],
    header: Header,
    plan: Plan,
    target: Target,
}

impl<'a> Image<'a> {
    pub fn plan(&self) -> &Plan {
        &self.plan
    }

    pub fn segments(&self) -> impl Iterator<Item = Segment> + '_ {
        (0..self.header.phnum)
            .filter_map(|index| program(self.bytes, &self.header, index))
            .filter(|program| program.kind == PT_LOAD)
            .filter_map(|program| segment(&program, self.plan.load_bias, &self.target).ok())
    }
}

pub fn plan(
    bytes: &[u8],
    file_size: u64,
    preferred_base: u64,
    target: Target,
) -> Result<Image<'_>, Error> {
    let header = parse_header(bytes, file_size, target.machine)?;
    let span = header_span(bytes, file_size, target.machine)?;
    if u64::try_from(bytes.len()).map_or(true, |length| length < span) {
        return Err(Error::Truncated);
    }

    let mut lowest = u64::MAX;
    let mut highest = 0u64;
    let mut load_count = 0u16;
    let mut interp = None;
    for index in 0..header.phnum {
        let program = program(bytes, &header, index).ok_or(Error::Truncated)?;
        if program.kind == PT_INTERP {
            if program.filesz < 2
                || program.filesz > MAX_INTERP_SIZE
                || !within_file(program.offset, program.filesz, file_size)
            {
                return Err(Error::Interp);
            }
            if interp.replace((program.offset, program.filesz)).is_some() {
                return Err(Error::Interp);
            }
            continue;
        }
        if program.kind != PT_LOAD {
            continue;
        }
        check_program(&program, file_size)?;
        load_count = load_count.checked_add(1).ok_or(Error::Program)?;
        let end = program.vaddr.checked_add(program.memsz).and_then(page_up).ok_or(Error::Range)?;
        lowest = lowest.min(page_down(program.vaddr));
        highest = highest.max(end);
    }
    if load_count == 0 || lowest == u64::MAX || highest <= lowest {
        return Err(Error::Program);
    }

    if preferred_base & (PAGE_SIZE - 1) != 0 {
        return Err(Error::Range);
    }
    let load_bias = if header.kind == ET_EXEC {
        if lowest < LOWEST_ADDRESS {
            return Err(Error::Range);
        }
        0
    } else {
        preferred_base.checked_sub(lowest).ok_or(Error::Range)?
    };
    let image_end = highest.checked_add(load_bias).ok_or(Error::Range)?;
    if image_end > target.user_limit {
        return Err(Error::Range);
    }

    let image_start = lowest.checked_add(load_bias).ok_or(Error::Range)?;

    let table = table_size(header.phnum);
    let mut phdr = 0u64;
    let mut entry_mapped = false;
    let entry = header.entry.checked_add(load_bias).ok_or(Error::Entry)?;
    for index in 0..header.phnum {
        let program = program(bytes, &header, index).ok_or(Error::Truncated)?;
        if program.kind != PT_LOAD {
            continue;
        }
        let placed = segment(&program, load_bias, &target)?;
        if (placed.page_start..placed.page_end).contains(&entry) {
            entry_mapped = true;
        }
        if phdr == 0
            && let Some(relative) = header.phoff.checked_sub(program.offset)
            && relative <= program.filesz
            && table <= program.filesz.saturating_sub(relative)
        {
            phdr = placed.address.checked_add(relative).ok_or(Error::Range)?;
        }
    }
    if entry < LOWEST_ADDRESS || entry >= target.user_limit || !entry_mapped {
        return Err(Error::Entry);
    }
    if interp.is_some() && phdr == 0 {
        return Err(Error::Phdr);
    }

    let (interp_offset, interp_size) = interp.unwrap_or((0, 0));
    Ok(Image {
        bytes,
        header,
        plan: Plan {
            kind: header.kind,
            phnum: header.phnum,
            load_count,
            load_bias,
            entry,
            phdr,
            image_start,
            image_end,
            interp_offset,
            interp_size,
        },
        target,
    })
}

pub fn interp_path_valid(path: &[u8]) -> bool {
    match path.split_last() {
        Some((0, text)) => text.first() == Some(&b'/') && text.iter().all(|&byte| byte != 0),
        _ => false,
    }
}

#[cfg(test)]
mod tests;
