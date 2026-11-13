#![allow(
    clippy::arithmetic_side_effects,
    clippy::as_conversions,
    clippy::expect_used,
    clippy::indexing_slicing,
    clippy::unwrap_used
)]

use crate::{Error, MAX_INPUT, TABLE_ENTRIES, bound, compress, decompress};

fn round_trip(data: &[u8]) -> usize {
    let mut table = [0u16; TABLE_ENTRIES];
    let mut packed = vec![0u8; bound(data.len())];
    let size = compress(data, &mut packed, &mut table).expect("fits in the bound");
    let mut unpacked = vec![0u8; data.len()];
    decompress(&packed[..size], &mut unpacked).expect("own output decodes");
    assert_eq!(unpacked, data);
    size
}

fn pseudo_random(seed: u64, length: usize) -> Vec<u8> {
    let mut state = seed | 1;
    (0..length)
        .map(|_| {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            (state >> 24) as u8
        })
        .collect()
}

#[test]
fn zero_page_shrinks() {
    assert!(round_trip(&[0u8; 4096]) < 64);
}

#[test]
fn random_page_survives() {
    let data = pseudo_random(7, 4096);
    assert!(round_trip(&data) <= bound(4096));
}

#[test]
fn short_inputs() {
    for length in 0..64 {
        let data: Vec<u8> = (0..length).map(|i| (i % 3) as u8).collect();
        round_trip(&data);
    }
}

#[test]
fn text_and_runs() {
    let mut data = Vec::new();
    while data.len() < 4096 {
        data.extend_from_slice(b"the quick brown fox jumps over the lazy dog 0123456789 ");
    }
    data.truncate(4096);
    assert!(round_trip(&data) < 300);
    let mut runs = vec![0u8; 4096];
    for (index, byte) in runs.iter_mut().enumerate() {
        *byte = (index / 300) as u8;
    }
    round_trip(&runs);
}

#[test]
fn largest_input() {
    let data = pseudo_random(3, MAX_INPUT);
    round_trip(&data);
    let mut table = [0u16; TABLE_ENTRIES];
    let mut packed = vec![0u8; bound(MAX_INPUT + 1)];
    assert_eq!(compress(&vec![0u8; MAX_INPUT + 1], &mut packed, &mut table), None);
}

#[test]
fn too_small_output_is_refused() {
    let data = pseudo_random(9, 4096);
    let mut table = [0u16; TABLE_ENTRIES];
    let mut packed = vec![0u8; 4000];
    assert_eq!(compress(&data, &mut packed, &mut table), None);
}

#[test]
fn malformed_streams() {
    let mut output = [0u8; 64];
    assert_eq!(decompress(&[], &mut output), Err(Error::Malformed));
    assert_eq!(decompress(&[0xF0], &mut output), Err(Error::Malformed));
    assert_eq!(decompress(&[0x10, b'a'], &mut output), Err(Error::OutputSize));
    let mut one = [0u8; 1];
    assert_eq!(decompress(&[0x10, b'a'], &mut one), Ok(()));
    let bad_offset = [0x14, b'a', 0x05, 0x00, 0x50, 1, 2, 3, 4, 5];
    assert_eq!(decompress(&bad_offset, &mut output), Err(Error::Malformed));
}
