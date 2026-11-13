#![allow(
    clippy::arithmetic_side_effects,
    clippy::as_conversions,
    clippy::indexing_slicing,
    clippy::unwrap_used
)]

use super::*;

const MACHINE: u16 = 62;
const LIMIT: u64 = 0x0000_8000_0000_0000;
const TARGET: Target = Target { machine: MACHINE, user_limit: LIMIT };
const PIE_BASE: u64 = 0x0000_5500_0000_0000;

#[derive(Clone, Copy)]
struct Ph {
    kind: u32,
    flags: u32,
    offset: u64,
    vaddr: u64,
    filesz: u64,
    memsz: u64,
    align: u64,
}

fn load(offset: u64, vaddr: u64, filesz: u64, memsz: u64, flags: u32) -> Ph {
    Ph { kind: PT_LOAD, flags, offset, vaddr, filesz, memsz, align: 0x1000 }
}

fn interp(offset: u64, filesz: u64) -> Ph {
    Ph { kind: PT_INTERP, flags: 4, offset, vaddr: 0, filesz, memsz: filesz, align: 1 }
}

struct Elf {
    kind: u16,
    entry: u64,
    phoff: u64,
    programs: Vec<Ph>,
    size: usize,
}

impl Elf {
    fn exec(programs: Vec<Ph>, entry: u64) -> Self {
        Elf { kind: ET_EXEC, entry, phoff: 64, programs, size: 0x4000 }
    }

    fn bytes(&self) -> Vec<u8> {
        let mut out = vec![0u8; self.size.max(64 + 56 * self.programs.len())];
        out[..4].copy_from_slice(b"\x7fELF");
        out[4] = 2;
        out[5] = 1;
        out[6] = 1;
        out[16..18].copy_from_slice(&self.kind.to_le_bytes());
        out[18..20].copy_from_slice(&MACHINE.to_le_bytes());
        out[20..24].copy_from_slice(&1u32.to_le_bytes());
        out[24..32].copy_from_slice(&self.entry.to_le_bytes());
        out[32..40].copy_from_slice(&self.phoff.to_le_bytes());
        out[52..54].copy_from_slice(&64u16.to_le_bytes());
        out[54..56].copy_from_slice(&56u16.to_le_bytes());
        out[56..58].copy_from_slice(&(self.programs.len() as u16).to_le_bytes());
        for (index, ph) in self.programs.iter().enumerate() {
            let Some(at) =
                usize::try_from(self.phoff).ok().and_then(|at| at.checked_add(index * 56))
            else {
                break;
            };
            if at.saturating_add(56) > out.len() {
                break;
            }
            let entry = &mut out[at..at + 56];
            entry[0..4].copy_from_slice(&ph.kind.to_le_bytes());
            entry[4..8].copy_from_slice(&ph.flags.to_le_bytes());
            entry[8..16].copy_from_slice(&ph.offset.to_le_bytes());
            entry[16..24].copy_from_slice(&ph.vaddr.to_le_bytes());
            entry[24..32].copy_from_slice(&ph.vaddr.to_le_bytes());
            entry[32..40].copy_from_slice(&ph.filesz.to_le_bytes());
            entry[40..48].copy_from_slice(&ph.memsz.to_le_bytes());
            entry[48..56].copy_from_slice(&ph.align.to_le_bytes());
        }
        out
    }
}

fn static_exec() -> Elf {
    Elf::exec(
        vec![
            load(0, 0x400000, 0x1000, 0x1000, PF_X | 4),
            load(0x1000, 0x401000, 0x800, 0x3000, PF_W | 4),
        ],
        0x400100,
    )
}

fn pie_with_interp() -> Elf {
    let mut bytes_needed = static_exec();
    bytes_needed.kind = ET_DYN;
    bytes_needed.programs = vec![
        load(0, 0, 0x1000, 0x1000, PF_X | 4),
        interp(0x200, 28),
        load(0x1000, 0x1000, 0x100, 0x2000, PF_W | 4),
    ];
    bytes_needed.entry = 0x300;
    bytes_needed
}

fn planned(elf: &Elf) -> Result<Plan, Error> {
    let bytes = elf.bytes();
    plan(&bytes, bytes.len() as u64, PIE_BASE, TARGET).map(|image| *image.plan())
}

fn rejected(elf: &Elf) -> Error {
    planned(elf).unwrap_err()
}

#[test]
fn a_static_executable_is_planned_in_place() {
    let elf = static_exec();
    let bytes = elf.bytes();
    let image = plan(&bytes, bytes.len() as u64, PIE_BASE, TARGET).unwrap();
    let plan = image.plan();
    assert_eq!(plan.kind, ET_EXEC);
    assert_eq!(plan.load_bias, 0);
    assert_eq!(plan.entry, 0x400100);
    assert_eq!(plan.phdr, 0x400040);
    assert_eq!(plan.image_start, 0x400000);
    assert_eq!(plan.image_end, 0x404000);
    assert_eq!(plan.load_count, 2);
    assert_eq!(plan.interp_size, 0);
    let segments: Vec<_> = image.segments().collect();
    assert_eq!(segments.len(), 2);
    assert_eq!(segments[0].page_start, 0x400000);
    assert_eq!(segments[0].page_end, 0x401000);
    assert!(segments[0].executable && !segments[0].writable);
    assert_eq!(segments[1].address, 0x401000);
    assert_eq!(segments[1].file_offset, 0x1000);
    assert_eq!(segments[1].file_size, 0x800);
    assert_eq!(segments[1].memory_size, 0x3000);
    assert_eq!(segments[1].page_end, 0x404000);
    assert!(segments[1].writable && !segments[1].executable);
}

#[test]
fn a_position_independent_executable_moves_to_the_preferred_base() {
    let plan = planned(&pie_with_interp()).unwrap();
    assert_eq!(plan.load_bias, PIE_BASE);
    assert_eq!(plan.entry, PIE_BASE + 0x300);
    assert_eq!(plan.phdr, PIE_BASE + 0x40);
    assert_eq!(plan.image_end, PIE_BASE + 0x3000);
    assert_eq!((plan.interp_offset, plan.interp_size), (0x200, 28));
}

#[test]
fn the_header_must_describe_this_machine_and_layout() {
    let good = static_exec().bytes();
    let size = good.len() as u64;
    for (at, value, error) in [
        (0usize, 0x7eu8, Error::Ident),
        (4, 1, Error::Ident),
        (5, 2, Error::Ident),
        (6, 0, Error::Ident),
        (16, 1, Error::Kind),
        (18, 0x28, Error::Machine),
        (20, 2, Error::Ident),
        (52, 63, Error::Layout),
        (54, 55, Error::Layout),
    ] {
        let mut bytes = good.clone();
        bytes[at] = value;
        assert_eq!(header_span(&bytes, size, MACHINE).unwrap_err(), error, "byte {at}");
    }
    let mut no_programs = good.clone();
    no_programs[56] = 0;
    no_programs[57] = 0;
    assert_eq!(header_span(&no_programs, size, MACHINE).unwrap_err(), Error::Layout);
    assert_eq!(header_span(&good[..63], size, MACHINE).unwrap_err(), Error::Truncated);
    assert_eq!(header_span(&good, 63, MACHINE).unwrap_err(), Error::Truncated);
}

#[test]
fn the_program_table_must_fit_the_file_and_the_header_budget() {
    let mut elf = static_exec();
    elf.phoff = 0x3fe0;
    assert_eq!(header_span(&elf.bytes(), 0x4000, MACHINE).unwrap_err(), Error::Layout);
    elf.phoff = u64::MAX - 8;
    assert_eq!(header_span(&elf.bytes(), 0x4000, MACHINE).unwrap_err(), Error::Layout);
    let mut wide = static_exec();
    wide.phoff = 64 * 1024 - 100;
    wide.size = 0x20000;
    assert_eq!(header_span(&wide.bytes(), 0x20000, MACHINE).unwrap_err(), Error::Layout);
    wide.phoff = 64 * 1024 - 112;
    assert_eq!(header_span(&wide.bytes(), 0x20000, MACHINE).unwrap(), 64 * 1024);
}

#[test]
fn a_short_buffer_is_refused_rather_than_read_past() {
    let bytes = static_exec().bytes();
    assert_eq!(
        plan(&bytes[..100], bytes.len() as u64, PIE_BASE, TARGET).map(|_| ()).unwrap_err(),
        Error::Truncated
    );
}

#[test]
fn loadable_segments_are_checked_against_the_file() {
    let cases: [(Ph, Error); 6] = [
        (load(0, 0x400000, 0x2000, 0x1000, 4), Error::Program),
        (load(0x3000, 0x403000, 0x2000, 0x2000, 4), Error::Program),
        (load(u64::MAX, 0x400fff, 1, 1, 4), Error::Program),
        (Ph { align: 0x1800, ..load(0, 0x400000, 0x100, 0x100, 4) }, Error::Program),
        (load(0x10, 0x400000, 0x100, 0x100, 4), Error::Program),
        (load(0, 0xffff_ffff_ffff_f000, 0, 0x2000, 4), Error::Program),
    ];
    for (bad, error) in cases {
        let elf = Elf::exec(vec![load(0, 0x400000, 0x1000, 0x1000, 5), bad], 0x400100);
        assert_eq!(rejected(&elf), error, "{:x?}", (bad.offset, bad.vaddr, bad.filesz));
    }
    let wraps_when_rounded = Elf::exec(
        vec![load(0, 0x400000, 0x1000, 0x1000, 5), load(0, u64::MAX - 0xfff, 0, 0xff, 4)],
        0x400100,
    );
    assert_eq!(rejected(&wraps_when_rounded), Error::Range);
}

#[test]
fn an_empty_tail_segment_is_allowed_past_the_end_of_the_file() {
    let elf = Elf::exec(
        vec![load(0, 0x400000, 0x1000, 0x1000, 5), load(0x9000, 0x409000, 0, 0x1000, 6)],
        0x400100,
    );
    let plan = planned(&elf).unwrap();
    assert_eq!(plan.image_end, 0x40a000);
}

#[test]
fn something_must_be_loaded() {
    let elf = Elf::exec(vec![interp(0x200, 10)], 0x400100);
    assert_eq!(rejected(&elf), Error::Program);
    let empty = Elf::exec(vec![load(0, 0x400000, 0, 0, 4)], 0x400000);
    assert_eq!(rejected(&empty), Error::Program);
}

#[test]
fn addresses_stay_inside_user_space() {
    let low = Elf::exec(vec![load(0, 0x1000, 0x1000, 0x1000, 5)], 0x1100);
    assert_eq!(rejected(&low), Error::Range);
    let high = Elf::exec(vec![load(0, LIMIT - 0x1000, 0x1000, 0x2000, 5)], LIMIT - 0x1000);
    assert_eq!(rejected(&high), Error::Range);
    let mut far = pie_with_interp();
    far.programs[0].vaddr = PIE_BASE + 0x1000;
    far.programs[0].offset = 0;
    far.programs[0].vaddr &= !0xfff;
    assert_eq!(rejected(&far), Error::Range);
    let mut huge_pie = pie_with_interp();
    huge_pie.programs[2].memsz = LIMIT;
    assert_eq!(rejected(&huge_pie), Error::Range);
}

#[test]
fn the_entry_point_must_be_loaded() {
    let outside = Elf::exec(vec![load(0, 0x400000, 0x1000, 0x1000, 5)], 0x500000);
    assert_eq!(rejected(&outside), Error::Entry);
    let mut wraps = pie_with_interp();
    wraps.entry = u64::MAX - 0x10;
    assert_eq!(rejected(&wraps), Error::Entry);
    let tail = Elf::exec(vec![load(0, 0x400000, 0x10, 0x10, 5)], 0x400fff);
    assert_eq!(planned(&tail).unwrap().entry, 0x400fff);
}

#[test]
fn an_interpreter_needs_one_sane_entry_and_a_visible_program_table() {
    let mut twice = pie_with_interp();
    twice.programs.push(interp(0x200, 28));
    assert_eq!(rejected(&twice), Error::Interp);
    for size in [1, MAX_INTERP_SIZE + 1] {
        let mut odd = pie_with_interp();
        odd.programs[1].filesz = size;
        assert_eq!(rejected(&odd), Error::Interp);
    }
    let mut outside = pie_with_interp();
    outside.programs[1].offset = 0x3ff0;
    assert_eq!(rejected(&outside), Error::Interp);
    let mut hidden = pie_with_interp();
    hidden.phoff = 0x3000;
    assert_eq!(rejected(&hidden), Error::Phdr);
    let mut static_hidden = static_exec();
    static_hidden.phoff = 0x3000;
    assert_eq!(planned(&static_hidden).unwrap().phdr, 0);
}

#[test]
fn a_shared_object_needs_room_below_the_preferred_base() {
    let mut elf = pie_with_interp();
    for program in &mut elf.programs {
        if program.kind == PT_LOAD {
            program.vaddr += PIE_BASE;
        }
    }
    elf.programs[0].vaddr += 0x1000;
    elf.programs[0].offset += 0x1000;
    elf.programs[2].offset = 0x2000;
    elf.programs[2].vaddr = PIE_BASE + 0x2000;
    elf.entry += PIE_BASE + 0x1000;
    assert_eq!(rejected(&elf), Error::Range);
}

#[test]
fn the_preferred_base_must_be_page_aligned() {
    let bytes = pie_with_interp().bytes();
    let result = plan(&bytes, bytes.len() as u64, PIE_BASE + 0x10, TARGET);
    assert_eq!(result.map(|_| ()).unwrap_err(), Error::Range);
}

#[test]
fn an_empty_unaligned_segment_still_maps_its_page() {
    let elf = Elf::exec(
        vec![load(0, 0x400000, 0x1000, 0x1000, 5), load(0x2010, 0x402010, 0, 0, 6)],
        0x402010,
    );
    let bytes = elf.bytes();
    let image = plan(&bytes, bytes.len() as u64, PIE_BASE, TARGET).unwrap();
    let segments: Vec<_> = image.segments().collect();
    assert_eq!((segments[1].page_start, segments[1].page_end), (0x402000, 0x403000));
    assert_eq!(image.plan().entry, 0x402010);
}

#[test]
fn interpreter_paths_are_absolute_and_terminated_once() {
    assert!(interp_path_valid(b"/lib/ld-linux-x86-64.so.2\0"));
    assert!(interp_path_valid(b"/\0"));
    assert!(!interp_path_valid(b""));
    assert!(!interp_path_valid(b"\0"));
    assert!(!interp_path_valid(b"lib/ld.so\0"));
    assert!(!interp_path_valid(b"/lib/ld.so"));
    assert!(!interp_path_valid(b"/lib\0/ld.so\0"));
}

#[test]
fn random_bytes_never_panic() {
    let mut state = 0x9e37_79b9_7f4a_7c15u64;
    let mut next = || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };
    let template = pie_with_interp().bytes();
    for round in 0..20_000 {
        let mut bytes = template.clone();
        for _ in 0..(1 + next() % 8) {
            let at = (next() % 300) as usize;
            bytes[at] = next() as u8;
        }
        let length = if round % 7 == 0 { (next() % 400) as usize } else { bytes.len() };
        let size = if round % 11 == 0 { next() } else { bytes.len() as u64 };
        let _ = header_span(&bytes[..length], size, MACHINE);
        if let Ok(image) = plan(&bytes[..length], size, PIE_BASE, TARGET) {
            assert_eq!(image.segments().count(), usize::from(image.plan().load_count));
        }
    }
}
