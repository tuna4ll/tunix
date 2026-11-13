use std::env;
use std::fs;
use std::path::{Path, PathBuf};

use tunix_elf::{
    LOWEST_ADDRESS, MAX_HEADER_SPAN, PAGE_SIZE, PT_INTERP, Plan, Segment, Target, header_span,
    interp_path_valid, plan,
};
use tunix_elf_check::{old_header_span, old_interp_valid, old_plan, same_plan, same_segment};

const USER_LIMIT: u64 = 0x0000_8000_0000_0000;
const MAIN_PIE_BASE: u64 = 0x0000_5500_0000_0000;
const INTERP_BASE: u64 = 0x0000_7F00_0000_0000;
const MACHINES: [u16; 2] = [62, 183];
const SAMPLE: usize = 128 * 1024;

#[derive(Default)]
struct Tally {
    compared: u64,
    accepted: u64,
    wrapped: u64,
}

fn check_invariants(bytes: &[u8], file_size: u64, plan: &Plan, segments: &[Segment]) {
    assert!(plan.image_start <= plan.image_end && plan.image_end <= USER_LIMIT);
    assert!(plan.entry >= LOWEST_ADDRESS && plan.entry < USER_LIMIT);
    assert_eq!(segments.len(), usize::from(plan.load_count));
    assert!(segments.iter().any(|s| (s.page_start..s.page_end).contains(&plan.entry)));
    for segment in segments {
        assert!(segment.address >= LOWEST_ADDRESS && segment.address < USER_LIMIT);
        assert_eq!(segment.page_start % PAGE_SIZE, 0);
        assert_eq!(segment.page_end % PAGE_SIZE, 0);
        assert!(segment.page_start <= segment.address && segment.page_end <= USER_LIMIT);
        assert!(segment.address + segment.memory_size <= segment.page_end);
        assert!(segment.file_size <= segment.memory_size);
        if segment.file_size != 0 {
            assert!(segment.file_offset + segment.file_size <= file_size);
        }
        assert!(plan.image_start <= segment.page_start && segment.page_end <= plan.image_end);
    }
    if plan.phdr != 0 {
        let table = u64::from(plan.phnum) * 56;
        assert!(
            segments
                .iter()
                .any(|s| s.address <= plan.phdr && plan.phdr + table <= s.address + s.file_size)
        );
    }
    if plan.interp_size != 0 {
        assert!((2..=4096).contains(&plan.interp_size));
        assert!(plan.interp_offset + plan.interp_size <= file_size);
        assert_ne!(plan.phdr, 0);
    }
    assert!(
        header_span(bytes, file_size, u16::from_le_bytes([bytes[18], bytes[19]])).unwrap()
            <= MAX_HEADER_SPAN
    );
}

fn compare(bytes: &[u8], file_size: u64, base: u64, machine: u16, tally: &mut Tally) {
    let target = Target { machine, user_limit: USER_LIMIT };
    let new_span = header_span(bytes, file_size, machine).ok();
    assert_eq!(new_span, old_header_span(bytes, file_size, machine), "header span differs");
    let new = plan(bytes, file_size, base, target);
    let old = old_plan(bytes, file_size, base, target);
    tally.compared += 1;
    match (new, old) {
        (Ok(image), Some((old, old_segments))) => {
            let segments: Vec<Segment> = image.segments().collect();
            assert!(same_plan(image.plan(), &old), "plans differ: {:x?} vs {old:x?}", image.plan());
            assert_eq!(segments.len(), old_segments.len());
            for (new, old) in segments.iter().zip(&old_segments) {
                assert!(same_segment(new, old), "segments differ: {new:x?} vs {old:x?}");
            }
            check_invariants(bytes, file_size, image.plan(), &segments);
            tally.accepted += 1;
        }
        (Err(error), Some((old, _))) => {
            assert!(old.entry_wrapped != 0, "only the old loader accepts ({error:?}): {old:x?}");
            tally.wrapped += 1;
        }
        (Ok(image), None) => panic!("only the new loader accepts: {:x?}", image.plan()),
        (Err(_), None) => {}
    }
}

fn elf_files(directory: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(directory) else { return };
    for entry in entries.flatten() {
        let Ok(kind) = entry.file_type() else { continue };
        let path = entry.path();
        if kind.is_dir() {
            elf_files(&path, out);
        } else if kind.is_file() {
            let mut magic = [0u8; 4];
            if fs::File::open(&path)
                .and_then(|mut file| std::io::Read::read_exact(&mut file, &mut magic))
                .is_ok()
                && magic == *b"\x7fELF"
            {
                out.push(path);
            }
        }
    }
}

fn corpus() -> Vec<(Vec<u8>, u64)> {
    let mut paths = Vec::new();
    for directory in env::var("TUNIX_ELF_CORPUS").unwrap_or_default().split(':') {
        if !directory.is_empty() {
            elf_files(Path::new(directory), &mut paths);
        }
    }
    paths.sort();
    paths
        .iter()
        .filter_map(|path| {
            let file = fs::File::open(path).ok()?;
            let size = file.metadata().ok()?.len();
            let mut bytes = Vec::new();
            std::io::Read::read_to_end(&mut std::io::Read::take(file, SAMPLE as u64), &mut bytes)
                .ok()?;
            Some((bytes, size))
        })
        .collect()
}

#[test]
fn real_binaries_get_the_same_plan_from_both_loaders() {
    let files = corpus();
    let minimum: usize =
        env::var("TUNIX_ELF_CORPUS_MINIMUM").ok().and_then(|v| v.parse().ok()).unwrap_or(0);
    assert!(files.len() >= minimum, "corpus has {} files, wanted {minimum}", files.len());
    let mut tally = Tally::default();
    let mut planned = [0u64; 2];
    for (bytes, size) in &files {
        for (index, machine) in MACHINES.iter().enumerate() {
            for base in [MAIN_PIE_BASE, INTERP_BASE] {
                compare(bytes, *size, base, *machine, &mut tally);
            }
            let target = Target { machine: *machine, user_limit: USER_LIMIT };
            if plan(bytes, *size, MAIN_PIE_BASE, target).is_ok() {
                planned[index] += 1;
            }
        }
    }
    eprintln!(
        "corpus: {} files, {} comparisons, {} accepted, x86_64 {} aarch64 {}",
        files.len(),
        tally.compared,
        tally.accepted,
        planned[0],
        planned[1]
    );
    assert_eq!(tally.wrapped, 0, "a real binary relied on a wrapping entry point");
    if minimum > 0 {
        assert!(
            planned[0] > 0 && planned[1] > 0,
            "the corpus must hold binaries for both machines"
        );
    }
}

struct Random(u64);

impl Random {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }

    fn below(&mut self, bound: u64) -> u64 {
        if bound == 0 { 0 } else { self.next() % bound }
    }

    fn interesting(&mut self, current: u64) -> u64 {
        const VALUES: [u64; 22] = [
            0,
            1,
            2,
            55,
            56,
            63,
            64,
            0xfff,
            0x1000,
            0x1001,
            0xffff,
            0x10000,
            0x40_0000,
            0x0000_5500_0000_0000,
            0x0000_7f00_0000_0000,
            0x0000_7fff_ffff_f000,
            0x0000_8000_0000_0000,
            0x8000_0000_0000_0000,
            u64::MAX - 0x1000,
            u64::MAX - 0xfff,
            u64::MAX - 1,
            u64::MAX,
        ];
        match self.below(6) {
            0 => current.wrapping_add(self.below(0x2000)),
            1 => current.wrapping_sub(self.below(0x2000)),
            2 => current ^ (1 << self.below(64)),
            _ => VALUES[self.below(VALUES.len() as u64) as usize],
        }
    }
}

fn put(bytes: &mut [u8], at: usize, width: usize, value: u64) {
    if at + width <= bytes.len() {
        bytes[at..at + width].copy_from_slice(&value.to_le_bytes()[..width]);
    }
}

fn get(bytes: &[u8], at: usize, width: usize) -> u64 {
    let mut out = [0u8; 8];
    if at + width <= bytes.len() {
        out[..width].copy_from_slice(&bytes[at..at + width]);
    }
    u64::from_le_bytes(out)
}

fn mutate(random: &mut Random, seed: &[u8], size: u64) -> (Vec<u8>, u64) {
    let mut bytes = seed.to_vec();
    let mut file_size = size;
    let phoff = get(&bytes, 32, 8) as usize;
    let phnum = get(&bytes, 56, 2) as usize;
    for _ in 0..1 + random.below(4) {
        match random.below(10) {
            0 if !bytes.is_empty() => {
                let at = random.below(64.min(bytes.len() as u64)) as usize;
                bytes[at] = random.next() as u8;
            }
            1 => {
                let field = [(16usize, 2usize), (24, 8), (32, 8), (54, 2), (56, 2)]
                    [random.below(5) as usize];
                let value = random.interesting(get(&bytes, field.0, field.1));
                put(&mut bytes, field.0, field.1, value);
            }
            2..=7 if phnum > 0 => {
                let index = random.below(phnum as u64) as usize;
                let at = phoff.saturating_add(index * 56);
                let (offset, width) =
                    [(0usize, 4usize), (4, 4), (8, 8), (16, 8), (32, 8), (40, 8), (48, 8)]
                        [random.below(7) as usize];
                let value = if offset == 0 && random.below(2) == 0 {
                    [1u64, 3, 2, 6, 0x6474e551][random.below(5) as usize]
                } else {
                    random.interesting(get(&bytes, at + offset, width))
                };
                put(&mut bytes, at + offset, width, value);
                if offset == 16 && random.below(2) == 0 {
                    let low = get(&bytes, at + 16, 8) & 0xfff;
                    let file_offset = (get(&bytes, at + 8, 8) & !0xfff) | low;
                    put(&mut bytes, at + 8, 8, file_offset);
                }
            }
            8 => {
                let keep = random.below(bytes.len() as u64 + 1) as usize;
                bytes.truncate(keep);
            }
            _ => {
                file_size = match random.below(3) {
                    0 => random.interesting(file_size),
                    1 => bytes.len() as u64,
                    _ => file_size.saturating_add(random.below(0x10000)),
                };
            }
        }
    }
    (bytes, file_size)
}

#[test]
fn mutated_headers_never_split_the_loaders() {
    let iterations: u64 =
        env::var("TUNIX_ELF_FUZZ_ITERATIONS").ok().and_then(|v| v.parse().ok()).unwrap_or(200_000);
    let seed: u64 = env::var("TUNIX_ELF_FUZZ_SEED").ok().and_then(|v| v.parse().ok()).unwrap_or(1);
    let mut seeds: Vec<(Vec<u8>, u64)> = corpus()
        .into_iter()
        .filter_map(|(bytes, size)| {
            let machine = u16::from_le_bytes([*bytes.get(18)?, *bytes.get(19)?]);
            let span = header_span(&bytes, size, machine).ok()? as usize;
            let keep = bytes.len().min(span.max(4096));
            Some((bytes[..keep].to_vec(), size))
        })
        .collect();
    seeds.dedup();
    if seeds.is_empty() {
        seeds.push((synthetic(), 0x4000));
    }
    let mut random = Random(seed.wrapping_mul(0x9e37_79b9_7f4a_7c15) | 1);
    let mut tally = Tally::default();
    for _ in 0..iterations {
        let (bytes, size) = &seeds[random.below(seeds.len() as u64) as usize];
        let (bytes, size) = mutate(&mut random, bytes, *size);
        let machine = if random.below(16) == 0 {
            MACHINES[random.below(2) as usize]
        } else {
            u16::from_le_bytes([*bytes.get(18).unwrap_or(&0), *bytes.get(19).unwrap_or(&0)])
        };
        let base = [MAIN_PIE_BASE, INTERP_BASE, 0, random.interesting(0) & !(PAGE_SIZE - 1)]
            [random.below(4) as usize];
        compare(&bytes, size, base, machine, &mut tally);
    }
    eprintln!(
        "fuzz: seed {seed}, {} comparisons, {} accepted, {} accepted only by the old loader (wrapping entry)",
        tally.compared, tally.accepted, tally.wrapped
    );
    assert!(tally.accepted > iterations / 100, "mutations rarely produced loadable images");
}

fn synthetic() -> Vec<u8> {
    let mut bytes = vec![0u8; 0x4000];
    bytes[..4].copy_from_slice(b"\x7fELF");
    bytes[4] = 2;
    bytes[5] = 1;
    bytes[6] = 1;
    put(&mut bytes, 16, 2, 3);
    put(&mut bytes, 18, 2, 62);
    put(&mut bytes, 20, 4, 1);
    put(&mut bytes, 24, 8, 0x100);
    put(&mut bytes, 32, 8, 64);
    put(&mut bytes, 52, 2, 64);
    put(&mut bytes, 54, 2, 56);
    put(&mut bytes, 56, 2, 3);
    let programs = [
        (1u64, 5u64, 0u64, 0u64, 0x1000u64, 0x1000u64),
        (u64::from(PT_INTERP), 4, 0x200, 0, 28, 28),
        (1, 6, 0x1000, 0x1000, 0x800, 0x3000),
    ];
    for (index, (kind, flags, offset, vaddr, filesz, memsz)) in programs.into_iter().enumerate() {
        let at = 64 + index * 56;
        put(&mut bytes, at, 4, kind);
        put(&mut bytes, at + 4, 4, flags);
        put(&mut bytes, at + 8, 8, offset);
        put(&mut bytes, at + 16, 8, vaddr);
        put(&mut bytes, at + 32, 8, filesz);
        put(&mut bytes, at + 40, 8, memsz);
        put(&mut bytes, at + 48, 8, 0x1000);
    }
    bytes[0x200..0x21c].copy_from_slice(b"/lib/ld-linux-x86-64.so.2\0\0\0");
    bytes
}

#[test]
fn interpreter_paths_are_judged_alike() {
    let mut random = Random(7);
    let alphabet = [0u8, b'/', b'a', b'.'];
    for _ in 0..200_000 {
        let length = 2 + random.below(40) as usize;
        let mut path: Vec<u8> = (0..length).map(|_| alphabet[random.below(4) as usize]).collect();
        if random.below(2) == 0 {
            path[0] = b'/';
        }
        if random.below(2) == 0 {
            path[length - 1] = 0;
        }
        assert_eq!(interp_path_valid(&path), old_interp_valid(&path), "{path:?}");
    }
}
