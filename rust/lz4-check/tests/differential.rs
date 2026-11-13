use std::env;
use std::fs;
use std::path::Path;
use tunix_lz4::{TABLE_ENTRIES, bound, compress, decompress};
use tunix_lz4_check::{reference_compress, reference_decompress};

const PAGE: usize = 4096;

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }

    fn below(&mut self, limit: usize) -> usize {
        (self.next() % limit as u64) as usize
    }
}

fn zero_offset_before_end(stream: &[u8]) -> bool {
    let mut at = 0usize;
    let length = |at: &mut usize, mut value: usize| -> Option<usize> {
        loop {
            let byte = *stream.get(*at)?;
            *at += 1;
            value += usize::from(byte);
            if byte != 255 {
                return Some(value);
            }
        }
    };
    loop {
        let Some(&token) = stream.get(at) else { return false };
        at += 1;
        let mut literals = usize::from(token >> 4);
        if literals == 15 {
            let Some(value) = length(&mut at, literals) else { return false };
            literals = value;
        }
        at += literals;
        if at >= stream.len() {
            return false;
        }
        let (Some(&low), Some(&high)) = (stream.get(at), stream.get(at + 1)) else { return false };
        if low == 0 && high == 0 {
            return true;
        }
        at += 2;
        if token & 15 == 15 && length(&mut at, 15).is_none() {
            return false;
        }
    }
}

fn ours(data: &[u8]) -> Vec<u8> {
    let mut table = vec![0u16; TABLE_ENTRIES];
    let mut out = vec![0u8; bound(data.len())];
    let size = compress(data, &mut out, &mut table).expect("bound always fits");
    out.truncate(size);
    out
}

fn check_page(data: &[u8], stats: &mut (usize, usize, usize)) {
    let packed = ours(data);
    assert_eq!(reference_decompress(&packed, data.len()).as_deref(), Some(data), "liblz4 decode");
    let mut back = vec![0u8; data.len()];
    decompress(&packed, &mut back).expect("own decode");
    assert_eq!(back, data);
    let theirs = reference_compress(data);
    let mut back = vec![0u8; data.len()];
    decompress(&theirs, &mut back).expect("decode liblz4 output");
    assert_eq!(back, data);
    stats.0 += 1;
    stats.1 += packed.len();
    stats.2 += theirs.len();
}

fn corpus_pages(limit: usize) -> Vec<Vec<u8>> {
    let roots = env::var("TUNIX_LZ4_CORPUS").unwrap_or_else(|_| "/usr/lib:/usr/bin".into());
    let mut pages = Vec::new();
    let mut stack: Vec<std::path::PathBuf> = roots.split(':').map(Into::into).collect();
    while let Some(path) = stack.pop() {
        if pages.len() >= limit {
            break;
        }
        let Ok(meta) = fs::symlink_metadata(&path) else { continue };
        if meta.is_dir() {
            if let Ok(entries) = fs::read_dir(&path) {
                for entry in entries.flatten() {
                    stack.push(entry.path());
                }
            }
        } else if meta.is_file() && meta.len() > 0 && meta.len() < (8 << 20) {
            let Ok(bytes) = fs::read(Path::new(&path)) else { continue };
            for chunk in bytes.chunks(PAGE).take(64) {
                pages.push(chunk.to_vec());
            }
        }
    }
    pages
}

#[test]
fn real_pages_match_liblz4() {
    let minimum: usize =
        env::var("TUNIX_LZ4_CORPUS_MINIMUM").ok().and_then(|v| v.parse().ok()).unwrap_or(1000);
    let pages = corpus_pages(200_000);
    assert!(pages.len() >= minimum, "corpus too small: {} pages", pages.len());
    let mut stats = (0, 0, 0);
    for page in &pages {
        check_page(page, &mut stats);
    }
    println!(
        "lz4: {} pages, ours {} bytes ({:.3}), liblz4 {} bytes ({:.3})",
        stats.0,
        stats.1,
        stats.1 as f64 / (stats.0 * PAGE) as f64,
        stats.2,
        stats.2 as f64 / (stats.0 * PAGE) as f64
    );
}

#[test]
fn synthetic_pages_match_liblz4() {
    let mut rng = Rng(0x9E37_79B9_7F4A_7C15);
    let mut stats = (0, 0, 0);
    for round in 0..20_000 {
        let length = if round % 4 == 0 { rng.below(PAGE + 1) } else { PAGE };
        let alphabet = 1 + rng.below(255);
        let mut data: Vec<u8> = (0..length).map(|_| rng.below(alphabet) as u8).collect();
        for _ in 0..rng.below(8) {
            if data.len() < 32 {
                break;
            }
            let from = rng.below(data.len() - 16);
            let to = rng.below(data.len() - 16);
            let span = 1 + rng.below((data.len() - from.max(to)).min(600));
            let copy = data[from..from + span].to_vec();
            data[to..to + span].copy_from_slice(&copy);
        }
        check_page(&data, &mut stats);
    }
}

#[test]
fn mutated_streams_agree_with_liblz4() {
    let iterations: usize =
        env::var("TUNIX_LZ4_FUZZ_ITERATIONS").ok().and_then(|v| v.parse().ok()).unwrap_or(200_000);
    let seed: u64 = env::var("TUNIX_LZ4_FUZZ_SEED").ok().and_then(|v| v.parse().ok()).unwrap_or(1);
    let mut rng = Rng(seed.wrapping_mul(0x2545_F491_4F6C_DD1D) | 1);
    let seeds: Vec<Vec<u8>> = (0..64)
        .map(|index| {
            let mut page = vec![0u8; PAGE];
            for (at, byte) in page.iter_mut().enumerate() {
                *byte = match index % 4 {
                    0 => (at % 7) as u8,
                    1 => (rng.next() % 4) as u8,
                    2 => b"lorem ipsum dolor sit amet "[at % 27],
                    _ => (rng.next() >> 8) as u8,
                };
            }
            page
        })
        .collect();
    let mut divergent = 0usize;
    let mut zero_offsets = 0usize;
    for _ in 0..iterations {
        let page = &seeds[rng.below(seeds.len())];
        let size = if rng.below(4) == 0 { rng.below(PAGE + 64) } else { PAGE };
        let mut stream = if rng.below(2) == 0 {
            ours(&page[..size.min(PAGE)])
        } else {
            reference_compress(page)
        };
        for _ in 0..1 + rng.below(6) {
            if stream.is_empty() {
                break;
            }
            let at = rng.below(stream.len());
            match rng.below(4) {
                0 => stream[at] ^= 1 << rng.below(8),
                1 => stream[at] = rng.next() as u8,
                2 => stream.truncate(at),
                _ => stream.insert(at, rng.next() as u8),
            }
        }
        let theirs = reference_decompress(&stream, size);
        let mut mine = vec![0u8; size];
        let accepted = decompress(&stream, &mut mine).is_ok();
        if !accepted && theirs.is_some() && zero_offset_before_end(&stream) {
            zero_offsets += 1;
            continue;
        }
        if accepted != theirs.is_some() || (accepted && theirs.as_deref() != Some(&mine[..])) {
            divergent += 1;
            if divergent < 5 {
                eprintln!(
                    "diverged: size {size} ours {accepted} liblz4 {} len {} stream {:02x?}",
                    theirs.is_some(),
                    stream.len(),
                    &stream[stream.len().saturating_sub(24)..]
                );
            }
        }
    }
    println!(
        "lz4 fuzz: {iterations} streams, {zero_offsets} with a zero offset that liblz4 accepts"
    );
    assert_eq!(divergent, 0, "{divergent} of {iterations} streams diverged from liblz4");
}
