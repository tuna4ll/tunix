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

#[cfg(test)]
mod tests;

pub const HASH_LOG: u32 = 12;
pub const TABLE_ENTRIES: usize = 1 << HASH_LOG;
pub const MAX_INPUT: usize = 65535;

const MIN_MATCH: usize = 4;
const LAST_LITERALS: usize = 5;
const MF_LIMIT: usize = 12;
const MAX_OFFSET: usize = 65535;
const RUN_MASK: usize = 15;
const SKIP_TRIGGER: u32 = 6;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Malformed,
    OutputSize,
}

pub const fn bound(length: usize) -> usize {
    length.saturating_add(length / 255).saturating_add(16)
}

fn copy(target: &mut [u8], source: &[u8]) {
    for (to, from) in target.iter_mut().zip(source) {
        *to = *from;
    }
}

fn read_u32(data: &[u8], at: usize) -> Option<u32> {
    let end = at.checked_add(4)?;
    let bytes: [u8; 4] = data.get(at..end)?.try_into().ok()?;
    Some(u32::from_le_bytes(bytes))
}

fn hash(sequence: u32) -> usize {
    let mixed = sequence.wrapping_mul(2_654_435_761) >> (32 - HASH_LOG);
    usize::try_from(mixed).unwrap_or(0) & (TABLE_ENTRIES - 1)
}

struct Writer<'a> {
    buffer: &'a mut [u8],
    at: usize,
}

impl Writer<'_> {
    fn byte(&mut self, value: u8) -> Option<()> {
        *self.buffer.get_mut(self.at)? = value;
        self.at = self.at.checked_add(1)?;
        Some(())
    }

    fn bytes(&mut self, data: &[u8]) -> Option<()> {
        let end = self.at.checked_add(data.len())?;
        copy(self.buffer.get_mut(self.at..end)?, data);
        self.at = end;
        Some(())
    }

    fn length(&mut self, mut rest: usize) -> Option<()> {
        while rest >= 255 {
            self.byte(255)?;
            rest = rest.checked_sub(255)?;
        }
        self.byte(u8::try_from(rest).ok()?)
    }

    fn sequence(&mut self, literals: &[u8], offset: usize, match_length: usize) -> Option<()> {
        let literal_length = literals.len();
        let extra = match_length.checked_sub(MIN_MATCH)?;
        let high = u8::try_from(literal_length.min(RUN_MASK)).ok()?;
        let low = u8::try_from(extra.min(RUN_MASK)).ok()?;
        self.byte((high << 4) | low)?;
        if literal_length >= RUN_MASK {
            self.length(literal_length.checked_sub(RUN_MASK)?)?;
        }
        self.bytes(literals)?;
        let offset = u16::try_from(offset).ok()?;
        self.bytes(&offset.to_le_bytes())?;
        if extra >= RUN_MASK {
            self.length(extra.checked_sub(RUN_MASK)?)?;
        }
        Some(())
    }

    fn last(&mut self, literals: &[u8]) -> Option<()> {
        let literal_length = literals.len();
        let high = u8::try_from(literal_length.min(RUN_MASK)).ok()?;
        self.byte(high << 4)?;
        if literal_length >= RUN_MASK {
            self.length(literal_length.checked_sub(RUN_MASK)?)?;
        }
        self.bytes(literals)
    }
}

fn common_length(source: &[u8], first: usize, second: usize, limit: usize) -> usize {
    let mut length = 0usize;
    loop {
        let (Some(a), Some(b)) = (first.checked_add(length), second.checked_add(length)) else {
            return length;
        };
        if b >= limit {
            return length;
        }
        match (source.get(a), source.get(b)) {
            (Some(x), Some(y)) if x == y => length = length.saturating_add(1),
            _ => return length,
        }
    }
}

pub fn compress(source: &[u8], output: &mut [u8], table: &mut [u16]) -> Option<usize> {
    if source.len() > MAX_INPUT || table.len() < TABLE_ENTRIES {
        return None;
    }
    let mut writer = Writer { buffer: output, at: 0 };
    let length = source.len();
    if length <= MF_LIMIT {
        writer.last(source)?;
        return Some(writer.at);
    }
    for entry in table.iter_mut().take(TABLE_ENTRIES) {
        *entry = 0;
    }
    let match_limit = length.checked_sub(LAST_LITERALS)?;
    let start_limit = length.checked_sub(MF_LIMIT)?;
    let mut anchor = 0usize;
    let mut position = 0usize;
    let mut misses = 0u32;
    while position <= start_limit {
        let sequence = read_u32(source, position)?;
        let slot = table.get_mut(hash(sequence))?;
        let candidate = usize::from(*slot);
        *slot = u16::try_from(position).ok()?;
        let distance = position.wrapping_sub(candidate);
        if candidate < position
            && distance <= MAX_OFFSET
            && read_u32(source, candidate) == Some(sequence)
        {
            let mut begin = position;
            let mut origin = candidate;
            while begin > anchor && origin > 0 {
                let (Some(previous), Some(earlier)) = (begin.checked_sub(1), origin.checked_sub(1))
                else {
                    break;
                };
                if source.get(previous) != source.get(earlier) {
                    break;
                }
                begin = previous;
                origin = earlier;
            }
            let found = MIN_MATCH.saturating_add(common_length(
                source,
                origin.checked_add(MIN_MATCH)?,
                begin.checked_add(MIN_MATCH)?,
                match_limit,
            ));
            writer.sequence(source.get(anchor..begin)?, begin.checked_sub(origin)?, found)?;
            position = begin.checked_add(found)?;
            anchor = position;
            misses = 0;
            if let Some(back) = position.checked_sub(2)
                && back <= start_limit
                && let Some(value) = read_u32(source, back)
                && let Some(entry) = table.get_mut(hash(value))
            {
                *entry = u16::try_from(back).ok()?;
            }
            continue;
        }
        misses = misses.saturating_add(1);
        let step = 1usize.saturating_add(usize::try_from(misses >> SKIP_TRIGGER).ok()?);
        position = position.checked_add(step)?;
    }
    writer.last(source.get(anchor..)?)?;
    Some(writer.at)
}

fn read_length(input: &[u8], at: &mut usize, mut length: usize) -> Result<usize, Error> {
    loop {
        let byte = *input.get(*at).ok_or(Error::Malformed)?;
        *at = at.checked_add(1).ok_or(Error::Malformed)?;
        length = length.checked_add(usize::from(byte)).ok_or(Error::Malformed)?;
        if byte != 255 {
            return Ok(length);
        }
    }
}

pub fn decompress(input: &[u8], output: &mut [u8]) -> Result<(), Error> {
    let end = output.len();
    let mut at = 0usize;
    let mut out = 0usize;
    if input.is_empty() {
        return Err(Error::Malformed);
    }
    if end == 0 {
        return if input == [0] { Ok(()) } else { Err(Error::Malformed) };
    }
    loop {
        let token = *input.get(at).ok_or(Error::Malformed)?;
        at = at.checked_add(1).ok_or(Error::Malformed)?;
        let mut literals = usize::from(token >> 4);
        if literals == RUN_MASK {
            literals = read_length(input, &mut at, literals)?;
        }
        let literal_end = at.checked_add(literals).ok_or(Error::Malformed)?;
        let out_end = out.checked_add(literals).ok_or(Error::Malformed)?;
        let source = input.get(at..literal_end).ok_or(Error::Malformed)?;
        copy(output.get_mut(out..out_end).ok_or(Error::OutputSize)?, source);
        at = literal_end;
        out = out_end;
        if at == input.len() {
            return if out == end { Ok(()) } else { Err(Error::OutputSize) };
        }
        if out > end.saturating_sub(MF_LIMIT)
            || at > input.len().saturating_sub(3usize.saturating_add(LAST_LITERALS))
        {
            return Err(Error::Malformed);
        }
        let offset_end = at.checked_add(2).ok_or(Error::Malformed)?;
        let offset_bytes: [u8; 2] = input
            .get(at..offset_end)
            .ok_or(Error::Malformed)?
            .try_into()
            .map_err(|_| Error::Malformed)?;
        at = offset_end;
        let offset = usize::from(u16::from_le_bytes(offset_bytes));
        if offset == 0 || offset > out {
            return Err(Error::Malformed);
        }
        let mut matched = usize::from(token & 15);
        if matched == RUN_MASK {
            matched = read_length(input, &mut at, matched)?;
        }
        matched = matched.checked_add(MIN_MATCH).ok_or(Error::Malformed)?;
        let match_end = out.checked_add(matched).ok_or(Error::Malformed)?;
        if match_end > end.saturating_sub(LAST_LITERALS) {
            return Err(Error::Malformed);
        }
        let mut from = out.checked_sub(offset).ok_or(Error::Malformed)?;
        if offset >= matched {
            let (head, tail) = output.split_at_mut_checked(out).ok_or(Error::Malformed)?;
            let source = head.get(from..from.saturating_add(matched)).ok_or(Error::Malformed)?;
            let target = tail.get_mut(..matched).ok_or(Error::Malformed)?;
            copy(target, source);
        } else {
            for index in out..match_end {
                let value = *output.get(from).ok_or(Error::Malformed)?;
                *output.get_mut(index).ok_or(Error::Malformed)? = value;
                from = from.saturating_add(1);
            }
        }
        out = match_end;
    }
}
