use alloc::{boxed::Box, collections::VecDeque, vec, vec::Vec};

const MAX_TRACKS: usize = 16;
const MAX_SAMPLES: usize = 500_000;

#[derive(Clone, Copy)]
struct Atom {
    kind: [u8; 4],
    payload: usize,
    end: usize,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum TrackKind {
    Video,
    Audio,
}

pub(crate) struct Sample {
    pub offset: usize,
    pub size: usize,
    pub pts: i64,
    pub duration: u32,
    pub keyframe: bool,
}

pub(crate) struct Track {
    pub kind: TrackKind,
    pub codec: [u8; 4],
    pub timescale: u32,
    pub width: u16,
    pub height: u16,
    pub nal_length_size: u8,
    pub decoder_config: Vec<u8>,
    pub samples: Vec<Sample>,
}

pub(crate) struct Mp4File {
    pub video: Option<Track>,
    pub audio: Option<Track>,
}

fn be_u16(data: &[u8], offset: usize) -> Option<u16> {
    Some(u16::from_be_bytes(data.get(offset..offset + 2)?.try_into().ok()?))
}

fn be_u32(data: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_be_bytes(data.get(offset..offset + 4)?.try_into().ok()?))
}

fn be_u64(data: &[u8], offset: usize) -> Option<u64> {
    Some(u64::from_be_bytes(data.get(offset..offset + 8)?.try_into().ok()?))
}

fn version(data: &[u8], atom: Atom) -> Option<u8> {
    data.get(atom.payload).copied()
}

fn atom_at(data: &[u8], offset: usize, parent_end: usize) -> Option<Atom> {
    if offset.checked_add(8)? > parent_end || parent_end > data.len() {
        return None;
    }
    let size32 = be_u32(data, offset)? as u64;
    let kind = data.get(offset + 4..offset + 8)?.try_into().ok()?;
    let (header_size, size) = match size32 {
        0 => (8usize, (parent_end - offset) as u64),
        1 => (16usize, be_u64(data, offset + 8)?),
        n => (8usize, n),
    };
    let size = usize::try_from(size).ok()?;
    let end = offset.checked_add(size)?;
    if size < header_size || end > parent_end {
        return None;
    }
    Some(Atom { kind, payload: offset + header_size, end })
}

fn find_child(data: &[u8], start: usize, end: usize, wanted: &[u8; 4]) -> Option<Atom> {
    let mut offset = start;
    while let Some(atom) = atom_at(data, offset, end) {
        if &atom.kind == wanted { return Some(atom); }
        offset = atom.end;
    }
    None
}

fn children(data: &[u8], start: usize, end: usize) -> Vec<Atom> {
    let mut out = Vec::new();
    let mut offset = start;
    while let Some(atom) = atom_at(data, offset, end) {
        out.push(atom);
        offset = atom.end;
    }
    out
}

fn expand_timing(data: &[u8], stbl_start: usize, stbl_end: usize, kind: &[u8; 4], signed: bool) -> Option<Vec<i64>> {
    let atom = find_child(data, stbl_start, stbl_end, kind)?;
    let count = be_u32(data, atom.payload + 4)? as usize;
    if count > MAX_SAMPLES || atom.payload + 8 + count * 8 > atom.end { return None; }
    let mut out = Vec::new();
    for index in 0..count {
        let offset = atom.payload + 8 + index * 8;
        let repeats = be_u32(data, offset)? as usize;
        let raw = be_u32(data, offset + 4)?;
        let value = if signed { raw as i32 as i64 } else { raw as i64 };
        if repeats > MAX_SAMPLES.saturating_sub(out.len()) { return None; }
        out.resize(out.len() + repeats, value);
    }
    Some(out)
}

fn parse_sizes(data: &[u8], stsz: Atom) -> Option<Vec<usize>> {
    let fixed = be_u32(data, stsz.payload + 4)? as usize;
    let count = be_u32(data, stsz.payload + 8)? as usize;
    if count > MAX_SAMPLES { return None; }
    if fixed != 0 { return Some(vec![fixed; count]); }
    if stsz.payload.checked_add(12 + count.checked_mul(4)?)? > stsz.end { return None; }
    let mut sizes = Vec::with_capacity(count);
    for index in 0..count {
        sizes.push(be_u32(data, stsz.payload + 12 + index * 4)? as usize);
    }
    Some(sizes)
}

fn parse_chunk_offsets(data: &[u8], start: usize, end: usize) -> Option<Vec<u64>> {
    let (atom, wide) = if let Some(atom) = find_child(data, start, end, b"stco") {
        (atom, false)
    } else {
        (find_child(data, start, end, b"co64")?, true)
    };
    let count = be_u32(data, atom.payload + 4)? as usize;
    let width = if wide { 8 } else { 4 };
    if count > MAX_SAMPLES || atom.payload.checked_add(8 + count.checked_mul(width)?)? > atom.end {
        return None;
    }
    let mut offsets = Vec::with_capacity(count);
    for index in 0..count {
        offsets.push(if wide {
            be_u64(data, atom.payload + 8 + index * 8)?
        } else {
            be_u32(data, atom.payload + 8 + index * 4)? as u64
        });
    }
    Some(offsets)
}

fn parse_stsc(data: &[u8], start: usize, end: usize) -> Option<Vec<(u32, u32)>> {
    let atom = find_child(data, start, end, b"stsc")?;
    let count = be_u32(data, atom.payload + 4)? as usize;
    if count == 0 || count > MAX_SAMPLES || atom.payload + 8 + count * 12 > atom.end { return None; }
    let mut entries = Vec::with_capacity(count);
    for index in 0..count {
        let offset = atom.payload + 8 + index * 12;
        entries.push((be_u32(data, offset)?, be_u32(data, offset + 4)?));
    }
    Some(entries)
}

fn parse_keyframes(data: &[u8], start: usize, end: usize, count: usize) -> Option<Vec<bool>> {
    let mut flags = vec![true; count];
    let Some(atom) = find_child(data, start, end, b"stss") else { return Some(flags); };
    flags.fill(false);
    let entries = be_u32(data, atom.payload + 4)? as usize;
    if entries > count || atom.payload + 8 + entries * 4 > atom.end { return None; }
    for index in 0..entries {
        let sample_number = be_u32(data, atom.payload + 8 + index * 4)? as usize;
        if sample_number == 0 || sample_number > count { return None; }
        flags[sample_number - 1] = true;
    }
    Some(flags)
}

fn find_descriptor(data: &[u8], mut offset: usize, end: usize, wanted: u8, depth: u8) -> Option<&[u8]> {
    if depth > 4 { return None; }
    while offset < end {
        let tag = *data.get(offset)?;
        offset += 1;
        let mut length = 0usize;
        let mut count = 0;
        loop {
            let byte = *data.get(offset)?;
            offset += 1;
            length = length.checked_mul(128)?.checked_add((byte & 0x7f) as usize)?;
            count += 1;
            if byte & 0x80 == 0 { break; }
            if count == 4 { return None; }
        }
        let body_end = offset.checked_add(length)?;
        if body_end > end { return None; }
        if tag == wanted { return data.get(offset..body_end); }
        let nested = match tag {
            0x03 => {
                let flags = *data.get(offset + 2)?;
                let mut nested = offset.checked_add(3)?;
                if flags & 0x80 != 0 { nested = nested.checked_add(2)?; }
                if flags & 0x40 != 0 {
                    let url_size = *data.get(nested)? as usize;
                    nested = nested.checked_add(1 + url_size)?;
                }
                if flags & 0x20 != 0 { nested = nested.checked_add(2)?; }
                nested
            }
            0x04 => offset.checked_add(13)?,
            _ => body_end,
        };
        if nested > body_end { return None; }
        if nested < body_end {
            if let Some(found) = find_descriptor(data, nested, body_end, wanted, depth + 1) {
                return Some(found);
            }
        }
        offset = body_end;
    }
    None
}

fn parse_decoder_config(data: &[u8], entry: Atom, codec: [u8; 4]) -> Option<(u8, Vec<u8>)> {
    let child_start = entry.payload.checked_add(match &codec {
        b"avc1" | b"avc3" => 78,
        b"mp4a" => 28,
        _ => return None,
    })?;
    let wanted = if &codec == b"mp4a" { b"esds" } else { b"avcC" };
    let atom = find_child(data, child_start, entry.end, wanted)?;
    if wanted == b"esds" {
        let start = atom.payload.checked_add(4)?;
        return Some((0, find_descriptor(data, start, atom.end, 0x05, 0)?.to_vec()));
    }
    let nal_length_size = (data.get(atom.payload + 4)? & 3) + 1;
    let mut offset = atom.payload + 6;
    let sps_count = (data.get(atom.payload + 5)? & 31) as usize;
    let mut annex_b = Vec::new();
    for _ in 0..sps_count {
        let length = be_u16(data, offset)? as usize;
        offset += 2;
        annex_b.extend_from_slice(&[0, 0, 0, 1]);
        annex_b.extend_from_slice(data.get(offset..offset.checked_add(length)?)?);
        offset += length;
    }
    let pps_count = *data.get(offset)? as usize;
    offset += 1;
    for _ in 0..pps_count {
        let length = be_u16(data, offset)? as usize;
        offset += 2;
        annex_b.extend_from_slice(&[0, 0, 0, 1]);
        annex_b.extend_from_slice(data.get(offset..offset.checked_add(length)?)?);
        offset += length;
    }
    Some((nal_length_size, annex_b))
}

fn parse_track(data: &[u8], trak: Atom, file_size: u64) -> Option<Track> {
    let mdia = find_child(data, trak.payload, trak.end, b"mdia")?;
    let hdlr = find_child(data, mdia.payload, mdia.end, b"hdlr")?;
    let handler = data.get(hdlr.payload + 8..hdlr.payload + 12)?;
    let kind = match handler {
        b"vide" => TrackKind::Video,
        b"soun" => TrackKind::Audio,
        _ => return None,
    };
    let mdhd = find_child(data, mdia.payload, mdia.end, b"mdhd")?;
    let timescale_offset = if version(data, mdhd)? == 1 { 20 } else { 12 };
    let timescale = be_u32(data, mdhd.payload + timescale_offset)?;
    if timescale == 0 { return None; }
    let minf = find_child(data, mdia.payload, mdia.end, b"minf")?;
    let stbl = find_child(data, minf.payload, minf.end, b"stbl")?;
    let stsd = find_child(data, stbl.payload, stbl.end, b"stsd")?;
    let entry = atom_at(data, stsd.payload + 8, stsd.end)?;
    let codec = entry.kind;
    let (nal_length_size, decoder_config) = parse_decoder_config(data, entry, codec)?;
    let sizes = parse_sizes(data, find_child(data, stbl.payload, stbl.end, b"stsz")?)?;
    let chunk_offsets = parse_chunk_offsets(data, stbl.payload, stbl.end)?;
    let stsc = parse_stsc(data, stbl.payload, stbl.end)?;
    let durations = expand_timing(data, stbl.payload, stbl.end, b"stts", false)?;
    if sizes.len() != durations.len() { return None; }
    let composition = if let Some(ctts) = find_child(data, stbl.payload, stbl.end, b"ctts") {
        expand_timing(data, stbl.payload, stbl.end, b"ctts", version(data, ctts)? == 1)?
    } else {
        vec![0; sizes.len()]
    };
    let keyframes = parse_keyframes(data, stbl.payload, stbl.end, sizes.len())?;
    let mut samples = Vec::with_capacity(sizes.len());
    let mut sample_index = 0usize;
    let mut decode_time = 0i64;
    let mut stsc_index = 0usize;
    for (chunk_index, &chunk_offset) in chunk_offsets.iter().enumerate() {
        let chunk_number = (chunk_index + 1) as u32;
        while stsc_index + 1 < stsc.len() && stsc[stsc_index + 1].0 <= chunk_number {
            stsc_index += 1;
        }
        let samples_per_chunk = stsc.get(stsc_index)?.1 as usize;
        let mut offset = usize::try_from(chunk_offset).ok()?;
        for _ in 0..samples_per_chunk {
            if sample_index >= sizes.len() { break; }
            let size = sizes[sample_index];
            let end = offset.checked_add(size)?;
            if end as u64 > file_size { return None; }
            let duration = durations[sample_index] as u32;
            samples.push(Sample {
                offset, size,
                pts: decode_time.checked_add(composition[sample_index])?,
                duration,
                keyframe: keyframes[sample_index],
            });
            decode_time = decode_time.checked_add(durations[sample_index])?;
            sample_index += 1;
            offset = end;
        }
    }
    if samples.len() != sizes.len() { return None; }
    let (width, height) = if kind == TrackKind::Video {
        (be_u16(data, entry.payload + 24)?, be_u16(data, entry.payload + 26)?)
    } else { (0, 0) };
    Some(Track { kind, codec, timescale, width, height, nal_length_size, decoder_config, samples })
}

pub(crate) fn parse_moov(moov: &[u8], file_size: u64) -> Option<Mp4File> {
    let mut result = Mp4File { video: None, audio: None };
    for atom in children(moov, 0, moov.len()).into_iter().filter(|a| &a.kind == b"trak").take(MAX_TRACKS) {
        let Some(track) = parse_track(moov, atom, file_size) else { continue; };
        match track.kind {
            TrackKind::Video if result.video.is_none() => result.video = Some(track),
            TrackKind::Audio if result.audio.is_none() => result.audio = Some(track),
            _ => {}
        }
    }
    if result.video.is_none() { return None; }
    Some(result)
}

const MAX_AVFS_FILE_BYTES: usize = 16 * 1024 * 1024;
const MAX_MOOV_BYTES: usize = 32 * 1024 * 1024;
const MAX_SAMPLE_NAL_BYTES: usize = 16 * 1024 * 1024;

unsafe fn dbg_num(label: &[u8], value: usize) {
    let mut buf = [0u8; 32];
    let mut d = 0usize;
    let mut v = value;
    if v == 0 { buf[0] = b'0'; d = 1; }
    else { while v > 0 { buf[d] = b'0' + (v % 10) as u8; d += 1; v /= 10; } }
    for &b in label { if b == 0 { break; } outb(0xE9, b); }
    for i in (0..d).rev() { outb(0xE9, buf[i]); }
    outb(0xE9, b'\n');
}

const AUDIO_HALF_SAMPLES: usize = 2048;
const AUDIO_BDL_COUNT: usize = 32;
const AUDIO_BDL_PER_HALF: usize = AUDIO_BDL_COUNT / 2;
const AUDIO_FRAMES_PER_ENTRY: usize = AUDIO_HALF_SAMPLES / AUDIO_BDL_PER_HALF;
const AUDIO_BDL_WORDS: usize = AUDIO_FRAMES_PER_ENTRY * 2;

#[repr(C)]
#[derive(Clone, Copy)]
struct AudioBdl { address: u32, words: u16, flags: u16 }
const _: () = assert!(core::mem::size_of::<AudioBdl>() == 8);
const BDL_IOC: u16 = 0x8000;

#[repr(C, align(8))]
struct AudioBdlTable([AudioBdl; AUDIO_BDL_COUNT]);
static mut AUDIO_BDL: AudioBdlTable = AudioBdlTable([AudioBdl { address: 0, words: 0, flags: 0 }; AUDIO_BDL_COUNT]);

#[repr(align(16))]
struct AudioPcm([i16; AUDIO_HALF_SAMPLES * 2 * 2]);
static mut AUDIO_PCM: AudioPcm = AudioPcm([0; AUDIO_HALF_SAMPLES * 2 * 2]);

extern "C" {
    fn avfs_get_filesize(path: *const u8) -> i32;
    fn avfs_read_file(path: *const u8, output: *mut u8, size: u32, offset: u32) -> i32;
    fn print(text: *const u8);
    fn get_ticks() -> u32;
    fn sleep_ms(ms: u32);
    fn inb(port: u16) -> u8;
    fn inw(port: u16) -> u16;
    fn outw(port: u16, value: u16);
    fn outb(port: u16, value: u8);
    fn inl(port: u16) -> u32;
    fn outl(port: u16, value: u32);
    fn pci_config_read_dword(bus: u8, slot: u8, function: u8, offset: u8) -> u32;
    fn pci_config_read_word(bus: u8, slot: u8, function: u8, offset: u8) -> u16;
    fn pci_config_write_word(bus: u8, slot: u8, function: u8, offset: u8, value: u16);
    fn vga_restore_text_80x50();
}

// ── Keyboard ring buffer ──────────────────────────────────────────────────────

const KEY_ESC:       u8 = 0x01;
const KEY_Q:         u8 = 0x10;
const KEY_SPACE:     u8 = 0x39;
const KEY_LEFT_EXT:  u8 = 0x4B;
const KEY_RIGHT_EXT: u8 = 0x4D;
const KEY_UP_EXT:    u8 = 0x48;
const KEY_DOWN_EXT:  u8 = 0x50;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Key { None, Esc, Q, Space, Left, Right, Up, Down }

static mut KB_EXT: bool = false;

const KB_QUEUE_SIZE: usize = 8;
static mut KB_QUEUE: [Key; KB_QUEUE_SIZE] = [Key::None; KB_QUEUE_SIZE];
static mut KB_HEAD: usize = 0;
static mut KB_TAIL: usize = 0;

unsafe fn kb_enqueue(key: Key) {
    let next = (KB_TAIL + 1) % KB_QUEUE_SIZE;
    if next != KB_HEAD {
        KB_QUEUE[KB_TAIL] = key;
        KB_TAIL = next;
    }
}

unsafe fn kb_dequeue() -> Key {
    if KB_HEAD == KB_TAIL { return Key::None; }
    let key = KB_QUEUE[KB_HEAD];
    KB_HEAD = (KB_HEAD + 1) % KB_QUEUE_SIZE;
    key
}

unsafe fn kb_poll() {
    core::arch::asm!("cli");
    let mut raw = [0u8; 32];
    let mut count = 0usize;
    while count < 32 {
        if inb(0x64) & 1 == 0 { break; }
        raw[count] = inb(0x60);
        count += 1;
    }
    core::arch::asm!("sti");

    for &byte in &raw[..count] {
        if byte == 0xE0 { KB_EXT = true; continue; }
        let ext = KB_EXT;
        KB_EXT = false;
        if byte >= 0x80 { continue; } // break codes — ignore
        let key = if ext {
            match byte {
                KEY_LEFT_EXT  => Key::Left,
                KEY_RIGHT_EXT => Key::Right,
                KEY_UP_EXT    => Key::Up,
                KEY_DOWN_EXT  => Key::Down,
                _ => Key::None,
            }
        } else {
            match byte {
                KEY_ESC   => Key::Esc,
                KEY_Q     => Key::Q,
                KEY_SPACE => Key::Space,
                _ => Key::None,
            }
        };
        if key != Key::None { kb_enqueue(key); }
    }
}

// ── AC97 ─────────────────────────────────────────────────────────────────────

struct Ac97 { codec: u16, bus_master: u16 }

impl Ac97 {
    unsafe fn initialize(sample_rate: u32) -> Option<Self> {
        let mut found = None;
        'scan: for bus in 0..=255u16 {
            for slot in 0..32u8 {
                for function in 0..8u8 {
                    let id = pci_config_read_dword(bus as u8, slot, function, 0);
                    if id == u32::MAX || id as u16 != 0x8086 || (id >> 16) as u16 != 0x2415 { continue; }
                    let bar_codec = pci_config_read_dword(bus as u8, slot, function, 0x10);
                    let bar_dma   = pci_config_read_dword(bus as u8, slot, function, 0x14);
                    if bar_codec & 1 == 0 || bar_dma & 1 == 0 { continue; }
                    let cmd = pci_config_read_word(bus as u8, slot, function, 4) | 0x0005;
                    pci_config_write_word(bus as u8, slot, function, 4, cmd);
                    found = Some(Self { codec: (bar_codec & !3) as u16, bus_master: (bar_dma & !3) as u16 });
                    break 'scan;
                }
            }
        }
        let device = found?;
        outb(device.bus_master + 0x1b, 2);
        sleep_ms(10);
        outb(device.bus_master + 0x1b, 0);
        sleep_ms(10);
        let slot_2a = inw(device.codec + 0x2a);
        outw(device.codec + 0x2a, slot_2a | 1);
        outw(device.codec + 0x2c, sample_rate as u16);
        outw(device.codec + 0x02, 0x0000);
        outw(device.codec + 0x18, 0x0000);
        Some(device)
    }

    unsafe fn load_half(&self, half: usize, pcm: &[i16], volume: u32) -> bool {
        let max_elements = AUDIO_HALF_SAMPLES * 2;
        if half > 1 || pcm.is_empty() || pcm.len() % 2 != 0 || pcm.len() > max_elements {
            return false;
        }
        let bdl       = core::ptr::addr_of_mut!(AUDIO_BDL).cast::<AudioBdl>();
        let pcm_dma   = core::ptr::addr_of_mut!(AUDIO_PCM.0).cast::<i16>();
        let half_base = pcm_dma.add(half * max_elements);

        if volume != 100 {
            let mut scaled = vec![0i16; pcm.len()];
            for (i, &sample) in pcm.iter().enumerate() {
                scaled[i] = ((sample as i32 * volume as i32) / 100).clamp(-32768, 32767) as i16;
            }
            core::ptr::copy_nonoverlapping(scaled.as_ptr(), half_base, scaled.len());
            if scaled.len() < max_elements {
                core::ptr::write_bytes(half_base.add(scaled.len()), 0, max_elements - scaled.len());
            }
        } else {
            core::ptr::copy_nonoverlapping(pcm.as_ptr(), half_base, pcm.len());
            if pcm.len() < max_elements {
                core::ptr::write_bytes(half_base.add(pcm.len()), 0, max_elements - pcm.len());
            }
        }

        let base     = half_base as usize;
        let bdl_base = half * AUDIO_BDL_PER_HALF;
        for index in 0..AUDIO_BDL_PER_HALF {
            let start_frame = index * AUDIO_FRAMES_PER_ENTRY;
            let last = index + 1 == AUDIO_BDL_PER_HALF;
            bdl.add(bdl_base + index).write(AudioBdl {
                address: (base + start_frame * 4) as u32,
                words:   AUDIO_BDL_WORDS as u16,
                flags:   if last { BDL_IOC } else { 0 },
            });
        }
        true
    }

    unsafe fn start(&self) {
        outw(self.bus_master + 0x16, 0x1c);
        outl(self.bus_master + 0x10, core::ptr::addr_of_mut!(AUDIO_BDL) as u32);
        outb(self.bus_master + 0x15, (AUDIO_BDL_COUNT - 1) as u8);
        outb(self.bus_master + 0x1b, 1);
    }

    unsafe fn publish(&self) {
        outb(self.bus_master + 0x15, (AUDIO_BDL_COUNT - 1) as u8);
        if inb(self.bus_master + 0x1b) & 1 == 0 {
            outw(self.bus_master + 0x16, 0x1c);
            outb(self.bus_master + 0x1b, 1);
        }
    }

    // CIV (+0x14): index of the entry the DMA engine is currently consuming.
    // Entries 0..AUDIO_BDL_PER_HALF belong to half 0; the rest to half 1.
    unsafe fn active_half(&self) -> usize {
        if (inb(self.bus_master + 0x14) as usize) < AUDIO_BDL_PER_HALF { 0 } else { 1 }
    }

    unsafe fn half_free(&self, half: usize) -> bool { self.active_half() != half }

    unsafe fn stop(&self) {
        outb(self.bus_master + 0x1b, 0);
        outw(self.bus_master + 0x16, 0x1c);
    }
}

// ── Screen ───────────────────────────────────────────────────────────────────

struct Screen {
    fb: *mut u32,
    width: usize,
    height: usize,
    back: usize,
    tables: Box<Tables>,
    rc: Vec<i32>, gc: Vec<i32>, bc: Vec<i32>,
}

struct Tables {
    luma: [i32; 256], cr_r: [i32; 256], cb_b: [i32; 256],
    cb_g: [i32; 256], cr_g: [i32; 256], clamp: [u8; 1280],
}

const KY: i32 = 76309;
const KRV: i32 = 117489;
const KBU: i32 = 138438;
const KGU: i32 = -13975;
const KGV: i32 = -34925;
const CLAMP_BIAS: i32 = 512;
const VBE_Y_OFFSET: u16 = 0x09;

const HUD_BG:     u32 = 0xFF_10_10_10;
const HUD_FILLED: u32 = 0xFF_E8_60_10;
const HUD_EMPTY:  u32 = 0xFF_40_40_40;
const HUD_TEXT:   u32 = 0xFF_FF_FF_FF;

const BAR_H:      usize = 20;
const BAR_THICK:  usize = 6;
const DIGIT_W:    usize = 5;
const DIGIT_H:    usize = 7;
const TIME_CHARS: usize = 5;
const TIME_PX_W:  usize = TIME_CHARS * (DIGIT_W + 1);

const FONT: [[u8; DIGIT_H]; 11] = [
    [0b11110, 0b10010, 0b10010, 0b10010, 0b10010, 0b10010, 0b11110],
    [0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110],
    [0b11110, 0b00010, 0b00010, 0b11110, 0b10000, 0b10000, 0b11110],
    [0b11110, 0b00010, 0b00010, 0b11110, 0b00010, 0b00010, 0b11110],
    [0b10010, 0b10010, 0b10010, 0b11110, 0b00010, 0b00010, 0b00010],
    [0b11110, 0b10000, 0b10000, 0b11110, 0b00010, 0b00010, 0b11110],
    [0b11110, 0b10000, 0b10000, 0b11110, 0b10010, 0b10010, 0b11110],
    [0b11110, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000],
    [0b11110, 0b10010, 0b10010, 0b11110, 0b10010, 0b10010, 0b11110],
    [0b11110, 0b10010, 0b10010, 0b11110, 0b00010, 0b00010, 0b11110],
    [0b00000, 0b00100, 0b00100, 0b00000, 0b00100, 0b00100, 0b00000],
];

unsafe fn draw_glyph(page: *mut u32, stride: usize, px: usize, py: usize, glyph: usize, color: u32) {
    for row in 0..DIGIT_H {
        let bits = FONT[glyph][row];
        for col in 0..DIGIT_W {
            if bits & (0b10000 >> col) != 0 {
                core::ptr::write_volatile(page.add((py + row) * stride + px + col), color);
            }
        }
    }
}

fn fmt_time(buf: &mut [u8; 5], total_s: u64) {
    let m = (total_s / 60).min(99);
    let s = total_s % 60;
    buf[0] = b'0' + (m / 10) as u8; buf[1] = b'0' + (m % 10) as u8;
    buf[2] = b':';
    buf[3] = b'0' + (s / 10) as u8; buf[4] = b'0' + (s % 10) as u8;
}

fn glyph_of(ch: u8) -> usize {
    match ch { b'0'..=b'9' => (ch - b'0') as usize, _ => 10 }
}

impl Screen {
    unsafe fn open(width: usize, height: usize) -> Option<Self> {
        let width = (width + 7) & !7;
        let fb_height = height + BAR_H;
        if crate::graphics_init(width as u32, fb_height as u32, 32) != 0 { return None; }
        let fb = core::ptr::addr_of!(crate::GRAPHICS_MODE.framebuffer).read();
        if fb.is_null() { crate::graphics_shutdown(); return None; }
        for i in 0..width * fb_height * 2 {
            core::ptr::write_volatile(fb.add(i), 0xFF00_0000);
        }
        crate::vbe_write(VBE_Y_OFFSET, 0);
        let mut tables = Box::new(Tables {
            luma: [0; 256], cr_r: [0; 256], cb_b: [0; 256],
            cb_g: [0; 256], cr_g: [0; 256], clamp: [0; 1280],
        });
        for i in 0..256usize {
            let c = i as i32 - 128;
            tables.luma[i] = (i as i32 - 16) * KY + 32768;
            tables.cr_r[i] = c * KRV; tables.cb_b[i] = c * KBU;
            tables.cb_g[i] = c * KGU; tables.cr_g[i] = c * KGV;
        }
        for i in 0..1280usize {
            tables.clamp[i] = (i as i32 - CLAMP_BIAS).clamp(0, 255) as u8;
        }
        let chroma = width / 2 + 1;
        Some(Self {
            fb, width, height, back: 1, tables,
            rc: vec![0; chroma], gc: vec![0; chroma], bc: vec![0; chroma],
        })
    }

    fn fb_height(&self) -> usize { self.height + BAR_H }

    unsafe fn present(&mut self, frame: &Frame) -> bool {
        let (w, h) = (frame.width, frame.height);
        if w == 0 || h == 0 || w % 2 != 0 || h % 2 != 0 || w > self.width || h > self.height
            || frame.y.len() < w * h || frame.u.len() < w * h / 4 || frame.v.len() < w * h / 4
        { return false; }
        let (cw, ch) = (w / 2, h / 2);
        let t = &*self.tables;
        let stride = self.width;
        let page = self.fb.add(self.back * stride * self.fb_height());
        for y in 0..h {
            let near = y >> 1;
            let far = if y & 1 == 0 { near.saturating_sub(1) } else { (near + 1).min(ch - 1) };
            let (u0, u1) = (&frame.u[near * cw..(near + 1) * cw], &frame.u[far * cw..(far + 1) * cw]);
            let (v0, v1) = (&frame.v[near * cw..(near + 1) * cw], &frame.v[far * cw..(far + 1) * cw]);
            for i in 0..cw {
                let u = ((3 * u0[i] as i32 + u1[i] as i32 + 2) >> 2) as usize;
                let v = ((3 * v0[i] as i32 + v1[i] as i32 + 2) >> 2) as usize;
                self.rc[i] = t.cr_r[v]; self.gc[i] = t.cb_g[u] + t.cr_g[v]; self.bc[i] = t.cb_b[u];
            }
            self.rc[cw] = self.rc[cw - 1]; self.gc[cw] = self.gc[cw - 1]; self.bc[cw] = self.bc[cw - 1];
            let luma = &frame.y[y * w..(y + 1) * w];
            let row = page.add(y * stride);
            for i in 0..cw {
                let (r0, g0, b0) = (self.rc[i], self.gc[i], self.bc[i]);
                let r1 = (r0 + self.rc[i + 1]) >> 1;
                let g1 = (g0 + self.gc[i + 1]) >> 1;
                let b1 = (b0 + self.bc[i + 1]) >> 1;
                let ya = t.luma[luma[2 * i] as usize];
                let yb = t.luma[luma[2 * i + 1] as usize];
                let px = |yv: i32, r: i32, g: i32, b: i32| -> u32 {
                    let cr = *t.clamp.get_unchecked((((yv + r) >> 16) + CLAMP_BIAS) as usize) as u32;
                    let cg = *t.clamp.get_unchecked((((yv + g) >> 16) + CLAMP_BIAS) as usize) as u32;
                    let cb = *t.clamp.get_unchecked((((yv + b) >> 16) + CLAMP_BIAS) as usize) as u32;
                    0xFF00_0000 | (cr << 16) | (cg << 8) | cb
                };
                core::ptr::write_volatile(row.add(2 * i),     px(ya, r0, g0, b0));
                core::ptr::write_volatile(row.add(2 * i + 1), px(yb, r1, g1, b1));
            }
        }
        true
    }

    unsafe fn draw_hud(&self, page_idx: usize, elapsed_ms: u64, total_ms: u64, paused: bool, no_hud: bool) {
        if no_hud { return; }
        let stride = self.width;
        let fb_h   = self.fb_height();
        let page   = self.fb.add(page_idx * stride * fb_h);
        let hud_y0 = self.height;
        for row in hud_y0..hud_y0 + BAR_H {
            for col in 0..stride {
                core::ptr::write_volatile(page.add(row * stride + col), HUD_BG);
            }
        }
        let gap      = 4usize;
        let track_x0 = TIME_PX_W + gap * 2;
        let track_x1 = stride.saturating_sub(TIME_PX_W + gap * 2);
        let track_w  = track_x1.saturating_sub(track_x0);
        let track_cy = hud_y0 + BAR_H / 2;
        let bar_y0   = track_cy.saturating_sub(BAR_THICK / 2);
        let filled_w = if total_ms > 0 {
            ((elapsed_ms.min(total_ms) as u128 * track_w as u128) / total_ms as u128) as usize
        } else { 0 };
        for row in bar_y0..bar_y0 + BAR_THICK {
            for col in track_x0..track_x1 {
                let color = if col < track_x0 + filled_w { HUD_FILLED } else { HUD_EMPTY };
                core::ptr::write_volatile(page.add(row * stride + col), color);
            }
        }
        let text_y    = track_cy.saturating_sub(DIGIT_H / 2);
        let elapsed_s = elapsed_ms / 1_000;
        let mut tbuf  = [0u8; 5];
        fmt_time(&mut tbuf, elapsed_s);
        for i in 0..TIME_CHARS {
            draw_glyph(page, stride, gap + i * (DIGIT_W + 1), text_y, glyph_of(tbuf[i]), HUD_TEXT);
        }
        fmt_time(&mut tbuf, total_ms / 1_000);
        let total_x = track_x1 + gap;
        for i in 0..TIME_CHARS {
            draw_glyph(page, stride, total_x + i * (DIGIT_W + 1), text_y, glyph_of(tbuf[i]), HUD_TEXT);
        }
        if paused {
            let mid_x = stride / 2;
            let pi_y0 = hud_y0 + (BAR_H.saturating_sub(9)) / 2;
            for row in pi_y0..pi_y0 + 9 {
                for col in mid_x.saturating_sub(4)..mid_x.saturating_sub(1) {
                    core::ptr::write_volatile(page.add(row * stride + col), HUD_TEXT);
                }
                for col in mid_x + 1..mid_x + 4 {
                    core::ptr::write_volatile(page.add(row * stride + col), HUD_TEXT);
                }
            }
        }
    }

    unsafe fn flip(&mut self, elapsed_ms: u64, total_ms: u64, paused: bool, no_hud: bool) {
        self.draw_hud(self.back, elapsed_ms, total_ms, paused, no_hud);
        crate::vbe_write(VBE_Y_OFFSET, (self.back * self.fb_height()) as u16);
        self.back ^= 1;
    }

    unsafe fn update_hud_inplace(&self, elapsed_ms: u64, total_ms: u64, paused: bool, no_hud: bool) {
        let front = 1 - self.back;
        self.draw_hud(front, elapsed_ms, total_ms, paused, no_hud);
    }
}

unsafe fn end_video() {
    crate::graphics_shutdown();
    vga_restore_text_80x50();
}

// ── Source ────────────────────────────────────────────────────────────────────

enum Source<'a> {
    Memory(&'a [u8]),
    Disk(crate::disk_store::DiskReader),
}

impl Source<'_> {
    fn size(&self) -> u64 {
        match self {
            Source::Memory(data) => data.len() as u64,
            Source::Disk(reader) => reader.size() as u64,
        }
    }
    fn read_at(&self, offset: usize, output: &mut [u8]) -> bool {
        match self {
            Source::Memory(data) => {
                let Some(end) = offset.checked_add(output.len()) else { return false; };
                let Some(bytes) = data.get(offset..end) else { return false; };
                output.copy_from_slice(bytes); true
            }
            Source::Disk(reader) => {
                matches!(reader.read_at(offset as u32, output), Ok(n) if n == output.len())
            }
        }
    }
}

fn find_moov(source: &Source, file_size: u64) -> Option<(usize, usize)> {
    let mut offset = 0u64;
    let mut saw_ftyp = false;
    let mut saw_mdat = false;
    let mut moov = None;
    while offset + 8 <= file_size {
        let mut header = [0u8; 16];
        if !source.read_at(offset as usize, &mut header[..8]) { return None; }
        let size32 = u32::from_be_bytes(header[..4].try_into().ok()?);
        let kind: [u8; 4] = header[4..8].try_into().ok()?;
        let (header_size, size) = match size32 {
            0 => (8u64, file_size - offset),
            1 => {
                if offset + 16 > file_size || !source.read_at(offset as usize, &mut header) { return None; }
                (16u64, u64::from_be_bytes(header[8..16].try_into().ok()?))
            }
            n => (8u64, n as u64),
        };
        if size < header_size || offset.checked_add(size)? > file_size { return None; }
        match &kind {
            b"ftyp" => saw_ftyp = true,
            b"mdat" => saw_mdat = true,
            b"moov" => moov = Some((offset + header_size, offset + size)),
            _ => {}
        }
        if size32 == 0 { break; }
        offset += size;
    }
    if !saw_ftyp || !saw_mdat { return None; }
    let (start, end) = moov?;
    Some((usize::try_from(start).ok()?, usize::try_from(end).ok()?))
}

fn ticks_to_ms(pts: i64, timescale: u32) -> u64 {
    (pts.max(0) as u64).saturating_mul(1_000) / timescale.max(1) as u64
}

fn sample_annex_b(bytes: &[u8], nal_length_size: u8, out: &mut Vec<u8>) -> bool {
    if nal_length_size == 0 || nal_length_size > 4 { return false; }
    out.clear();
    let mut cursor = 0usize;
    while cursor < bytes.len() {
        let Some(le) = cursor.checked_add(nal_length_size as usize) else { return false; };
        let Some(lb) = bytes.get(cursor..le) else { return false; };
        let mut nal_size = 0usize;
        for &b in lb {
            let Some(v) = nal_size.checked_mul(256).and_then(|v| v.checked_add(b as usize)) else { return false; };
            nal_size = v;
        }
        cursor = le;
        let Some(ne) = cursor.checked_add(nal_size) else { return false; };
        if nal_size == 0 || ne > bytes.len() || out.len() + nal_size + 4 > MAX_SAMPLE_NAL_BYTES { return false; }
        out.extend_from_slice(&[0, 0, 0, 1]);
        out.extend_from_slice(&bytes[cursor..ne]);
        cursor = ne;
    }
    true
}

type Frame = rusty_h264_common::YuvFrame;

const VIDEO_AHEAD_FRAMES: usize = 10;
const DECODE_COST_MS: u64 = 35;
const RESYNC_LAG_MS: u64 = 1_500;
const MAX_DECODE_ERRORS: u32 = 60;

fn sample_is_reference(bytes: &[u8], nal_length_size: u8) -> Option<bool> {
    let n = nal_length_size as usize;
    let mut cursor = 0usize;
    while cursor + n < bytes.len() {
        let size = bytes[cursor..cursor + n].iter().fold(0usize, |a, &b| (a << 8) | b as usize);
        let header = bytes[cursor + n];
        if matches!(header & 0x1f, 1 | 5) { return Some(header >> 5 != 0); }
        cursor = cursor.checked_add(n)?.checked_add(size)?;
    }
    None
}

// ── Video pipeline ────────────────────────────────────────────────────────────

struct VideoPipe<'a> {
    track: &'a Track,
    min_after: Vec<i64>,
    base_pts: i64,
    next: usize,
    held: Vec<(i64, Frame)>,
    ready: VecDeque<(u64, Frame)>,
    sample_buf: Vec<u8>,
    nal_buf: Vec<u8>,
    errors: u32,
    skipped: u32,
    dropped: u32,
    drawn: u32,
}

impl<'a> VideoPipe<'a> {
    fn new(track: &'a Track) -> Self {
        let count = track.samples.len();
        let mut min_after = vec![i64::MAX; count + 1];
        for i in (0..count).rev() {
            min_after[i] = min_after[i + 1].min(track.samples[i].pts);
        }
        let base_pts = min_after[0];
        Self {
            track, min_after, base_pts, next: 0,
            held: Vec::new(), ready: VecDeque::new(),
            sample_buf: Vec::new(), nal_buf: Vec::new(),
            errors: 0, skipped: 0, dropped: 0, drawn: 0,
        }
    }

    fn exhausted(&self) -> bool { self.next >= self.track.samples.len() }
    fn done(&self) -> bool { self.exhausted() && self.held.is_empty() && self.ready.is_empty() }
    fn pts_ms(&self, pts: i64) -> u64 { ticks_to_ms(pts - self.base_pts, self.track.timescale) }

    fn release(&mut self) {
        let limit = self.min_after[self.next];
        loop {
            let mut best: Option<usize> = None;
            for (i, held) in self.held.iter().enumerate() {
                if held.0 < limit && best.map_or(true, |b| held.0 < self.held[b].0) {
                    best = Some(i);
                }
            }
            let Some(i) = best else { break };
            let (pts, frame) = self.held.swap_remove(i);
            self.ready.push_back((self.pts_ms(pts), frame));
        }
    }

    fn step(&mut self, source: &Source, h264: &mut rusty_h264_decoder::Decoder, skip_before_ms: u64) -> bool {
        let track = self.track;
        let sample = &track.samples[self.next];
        self.next += 1;
        let due_ms = self.pts_ms(sample.pts);
        self.sample_buf.clear();
        self.sample_buf.resize(sample.size, 0);
        if !source.read_at(sample.offset, &mut self.sample_buf) { return false; }
        if !sample.keyframe && due_ms < skip_before_ms
            && sample_is_reference(&self.sample_buf, track.nal_length_size) == Some(false)
        {
            self.skipped += 1;
            self.release();
            return true;
        }
        let decoded = sample_annex_b(&self.sample_buf, track.nal_length_size, &mut self.nal_buf)
            .then(|| h264.decode(&self.nal_buf));
        match decoded {
            Some(Ok(Some(frame))) => { self.errors = 0; self.held.push((sample.pts, frame)); }
            Some(Ok(None)) => {}
            _ => { self.errors += 1; if self.errors > MAX_DECODE_ERRORS { return false; } }
        }
        self.release();
        true
    }

    fn resync_if_late(&mut self, now_ms: u64) {
        let samples = &self.track.samples;
        if self.exhausted() || now_ms <= self.pts_ms(samples[self.next].pts) + RESYNC_LAG_MS { return; }
        let mut target = None;
        for j in self.next + 1..samples.len() {
            if self.pts_ms(samples[j].pts) > now_ms { break; }
            if samples[j].keyframe { target = Some(j); }
        }
        if let Some(key) = target {
            self.held.clear();
            self.next = key;
            self.release();
        }
    }

    fn present_due(&mut self, now_ms: u64, screen: &mut Screen) -> bool {
        let mut newest = None;
        while self.ready.front().map_or(false, |f| f.0 <= now_ms) {
            if newest.is_some() { self.dropped += 1; }
            newest = self.ready.pop_front();
        }
        if let Some((_, frame)) = newest {
            self.drawn += 1;
            unsafe { screen.present(&frame) }
        } else { true }
    }

    fn seek(&mut self, h264: &mut rusty_h264_decoder::Decoder, target_ms: u64) -> u64 {
        let samples = &self.track.samples;
        let mut best = 0usize;
        for i in 0..samples.len() {
            if samples[i].keyframe && self.pts_ms(samples[i].pts) <= target_ms { best = i; }
            if self.pts_ms(samples[i].pts) > target_ms { break; }
        }
        self.next = best;
        self.held.clear();
        self.ready.clear();
        let _ = h264.decode(&self.track.decoder_config);
        self.pts_ms(samples[best].pts)
    }

    fn duration_ms(&self) -> u64 {
        if self.track.samples.is_empty() { return 0; }
        let last = &self.track.samples[self.track.samples.len() - 1];
        self.pts_ms(last.pts + last.duration as i64)
    }
}

// ── Audio helpers ─────────────────────────────────────────────────────────────

fn resample_pcm(input: &[i16], in_rate: u32, out_rate: u32) -> Vec<i16> {
    if in_rate == out_rate || in_rate == 0 || out_rate == 0 || input.is_empty() {
        return input.to_vec();
    }
    let channels = 2usize;
    let in_frames = input.len() / channels;
    let out_frames = ((in_frames as u64 * out_rate as u64) / in_rate as u64) as usize;
    let mut output = Vec::with_capacity(out_frames * channels);
    for i in 0..out_frames {
        let src_pos = ((i as u64 * in_rate as u64 + out_rate as u64 / 2) / out_rate as u64) as usize;
        let idx = src_pos.min(in_frames - 1);
        for c in 0..channels {
            output.push(input[idx * channels + c]);
        }
    }
    output
}

fn get_aac_sample_rate(config: &[u8]) -> u32 {
    if config.len() < 2 { return 44_100; }
    let freq_index = (((config[0] & 0x07) as u32) << 1) | ((config[1] >> 7) as u32);
    match freq_index {
        0  => 96_000,
        1  => 88_200,
        2  => 64_000,
        3  => 48_000,
        4  => 44_100,
        5  => 32_000,
        6  => 24_000,
        7  => 22_050,
        8  => 16_000,
        9  => 12_000,
        10 => 11_025,
        11 =>  8_000,
        12 =>  7_350,
        _  => 44_100,
    }
}

fn decode_audio_slice(
    source: &Source, audio: &Track, aac: &mut Option<rusty_aac::AacDecoder>,
    index: &mut usize, pcm: &mut Vec<i16>, sample_buf: &mut Vec<u8>,
    max_packets: usize, max_capacity: usize,
) -> bool {
    let Some(aac) = aac else {
        *index = (*index + max_packets).min(audio.samples.len());
        return true;
    };
    let native_rate = get_aac_sample_rate(&audio.decoder_config);
    let mut packets = 0usize;
    while *index < audio.samples.len() && packets < max_packets && pcm.len() + 2 <= max_capacity {
        let sample = &audio.samples[*index];
        *index += 1;
        packets += 1;
        sample_buf.clear();
        sample_buf.resize(sample.size, 0);
        if !source.read_at(sample.offset, sample_buf) { continue; }
        match aac.decode(sample_buf, Some(sample.pts)) {
            Ok(decoded) => {
                let channels = decoded.channels as usize;
                if channels == 0 { continue; }
                let mut raw_pcm = Vec::with_capacity(decoded.samples.len());
                for frame in decoded.samples.chunks(channels) {
                    let left  = frame[0].clamp(-1.0, 1.0);
                    let right = frame.get(1).copied().unwrap_or(frame[0]).clamp(-1.0, 1.0);
                    raw_pcm.push((left  * 32767.0) as i16);
                    raw_pcm.push((right * 32767.0) as i16);
                }
                let resampled = resample_pcm(&raw_pcm, native_rate, 48_000);
                for &s in &resampled {
                    if pcm.len() >= max_capacity { break; }
                    pcm.push(s);
                }
            }
            Err(_) => continue,
        }
    }
    true
}

fn audio_seek_index(audio: &Track, target_pts: i64) -> usize {
    let mut best = 0usize;
    for (i, s) in audio.samples.iter().enumerate() {
        if s.pts <= target_pts { best = i; } else { break; }
    }
    best
}

// ── CLI helpers ───────────────────────────────────────────────────────────────

#[derive(Clone)]
struct PlayOpts {
    seek_ms:   u64,
    no_hud:    bool,
    debug:     bool,
    info_only: bool,
    loop_play: bool,
    help:      bool,
    volume:    u32,
    fps_stats: bool,
    repeat:    usize,
}

impl PlayOpts {
    fn defaults() -> Self {
        Self {
            seek_ms: 0, no_hud: false, debug: false, info_only: false,
            loop_play: false, help: false, volume: 100, fps_stats: false, repeat: 1,
        }
    }
}

unsafe fn parse_args_str(args_ptr: *const u8) -> (PlayOpts, [u8; 256], usize) {
    let mut opts = PlayOpts::defaults();
    let mut path_buf = [0u8; 256];
    let mut path_len = 0usize;
    if args_ptr.is_null() { return (opts, path_buf, path_len); }
    let mut buf = [0u8; 256];
    let mut len = 0usize;
    while len < 255 { let b = *args_ptr.add(len); if b == 0 { break; } buf[len] = b; len += 1; }
    let mut i = 0usize;
    while i < len {
        while i < len && buf[i] == b' ' { i += 1; }
        if i >= len { break; }
        let start = i;
        while i < len && buf[i] != b' ' { i += 1; }
        let tok = &buf[start..i];
        if tok == b"--no-hud"        { opts.no_hud = true; }
        else if tok == b"--debug"    { opts.debug = true; }
        else if tok == b"--info"     { opts.info_only = true; }
        else if tok == b"--loop"     { opts.loop_play = true; }
        else if tok == b"--fps"      { opts.fps_stats = true; }
        else if tok == b"--help" || tok == b"-h" { opts.help = true; }
        else if tok.starts_with(b"--seek=") {
            let mut val = 0u64;
            for &b in &tok[7..] { if b >= b'0' && b <= b'9' { val = val * 10 + (b - b'0') as u64; } }
            opts.seek_ms = val * 1_000;
        } else if tok.starts_with(b"--volume=") {
            let mut val = 0u32;
            for &b in &tok[9..] { if b >= b'0' && b <= b'9' { val = val * 10 + (b - b'0') as u32; } }
            opts.volume = val.clamp(0, 200);
        } else if tok.starts_with(b"--repeat=") {
            let mut val = 0usize;
            for &b in &tok[9..] { if b >= b'0' && b <= b'9' { val = val * 10 + (b - b'0') as usize; } }
            opts.repeat = val.max(1);
        } else if path_len == 0 {
            let copy_len = tok.len().min(255);
            path_buf[..copy_len].copy_from_slice(&tok[..copy_len]);
            path_len = copy_len;
        }
    }
    (opts, path_buf, path_len)
}

unsafe fn print_help() {
    print(b"====================================================\n\0".as_ptr());
    print(b" MP4 Hardware Video/Audio Player - Help & Usage     \n\0".as_ptr());
    print(b"====================================================\n\0".as_ptr());
    print(b" Usage: mp4 [options] <path/to/file.mp4>            \n\0".as_ptr());
    print(b"                                                    \n\0".as_ptr());
    print(b" Options:                                           \n\0".as_ptr());
    print(b"   --help, -h   Show this help screen               \n\0".as_ptr());
    print(b"   --info       Show track info & metadata, then exit\n\0".as_ptr());
    print(b"   --seek=N     Start playback at N seconds         \n\0".as_ptr());
    print(b"   --volume=N   Set audio volume percentage (0-200) \n\0".as_ptr());
    print(b"   --repeat=N   Loop playback N times               \n\0".as_ptr());
    print(b"   --fps        Print live rendering FPS counters   \n\0".as_ptr());
    print(b"   --no-hud     Hide the overlay progress bar       \n\0".as_ptr());
    print(b"   --loop       Restart playback continuously       \n\0".as_ptr());
    print(b"   --debug      Emit performance metrics to port 0xE9\n\0".as_ptr());
    print(b"                                                    \n\0".as_ptr());
    print(b" In-Player Controls:                                \n\0".as_ptr());
    print(b"   SPACEBAR     Toggle Pause / Resume               \n\0".as_ptr());
    print(b"   LEFT / RIGHT Seek -/+ 10 seconds                 \n\0".as_ptr());
    print(b"   UP / DOWN    Seek -/+ 60 seconds                 \n\0".as_ptr());
    print(b"   ESC or Q     Exit player                         \n\0".as_ptr());
    print(b"====================================================\n\0".as_ptr());
}

unsafe fn print_info(video: &Track, audio: Option<&Track>, file_size: u64, duration_ms: u64) {
    let mut line = [0u8; 128];
    fn write_dec(buf: &mut [u8], mut pos: usize, mut v: u64) -> usize {
        if v == 0 { buf[pos] = b'0'; return pos + 1; }
        let start = pos;
        while v > 0 { buf[pos] = b'0' + (v % 10) as u8; v /= 10; pos += 1; }
        buf[start..pos].reverse(); pos
    }
    fn write_str(buf: &mut [u8], pos: usize, s: &[u8]) -> usize {
        let end = (pos + s.len()).min(buf.len() - 2);
        buf[pos..end].copy_from_slice(&s[..end - pos]); end
    }
    print(b"mp4: --- file info ---\n\0".as_ptr());
    let mut p = write_str(&mut line, 0, b"mp4: file_size=");
    p = write_dec(&mut line, p, file_size);
    p = write_str(&mut line, p, b" bytes\n"); line[p] = 0; print(line.as_ptr()); line.fill(0);
    let mut p = write_str(&mut line, 0, b"mp4: duration=");
    p = write_dec(&mut line, p, duration_ms / 1_000);
    p = write_str(&mut line, p, b"s (");
    p = write_dec(&mut line, p, duration_ms);
    p = write_str(&mut line, p, b"ms)\n"); line[p] = 0; print(line.as_ptr()); line.fill(0);
    let mut p = write_str(&mut line, 0, b"mp4: video codec=");
    line[p] = video.codec[0]; line[p+1] = video.codec[1];
    line[p+2] = video.codec[2]; line[p+3] = video.codec[3]; p += 4;
    p = write_str(&mut line, p, b" res=");
    p = write_dec(&mut line, p, video.width as u64);
    p = write_str(&mut line, p, b"x");
    p = write_dec(&mut line, p, video.height as u64);
    p = write_str(&mut line, p, b" ts=");
    p = write_dec(&mut line, p, video.timescale as u64);
    p = write_str(&mut line, p, b" samples=");
    p = write_dec(&mut line, p, video.samples.len() as u64);
    let ep = write_str(&mut line, p, b"\n"); line[ep] = 0; print(line.as_ptr()); line.fill(0);
    if let Some(audio) = audio {
        let mut p = write_str(&mut line, 0, b"mp4: audio codec=");
        line[p] = audio.codec[0]; line[p+1] = audio.codec[1];
        line[p+2] = audio.codec[2]; line[p+3] = audio.codec[3]; p += 4;
        p = write_str(&mut line, p, b" ts=");
        p = write_dec(&mut line, p, audio.timescale as u64);
        p = write_str(&mut line, p, b" samples=");
        p = write_dec(&mut line, p, audio.samples.len() as u64);
        let ep = write_str(&mut line, p, b"\n"); line[ep] = 0; print(line.as_ptr()); line.fill(0);
    }
}

// ── Main playback loop (no tasking) ──────────────────────────────────────────

unsafe fn play_file(path_ptr: *const u8, opts: &PlayOpts) -> i32 {
    if opts.help || path_ptr.is_null() || *path_ptr == 0 {
        print_help(); return -1;
    }
    let mut path_len = 0usize;
    while path_len < 512 && *path_ptr.add(path_len) != 0 { path_len += 1; }
    if path_len == 512 { print(b"mp4: path too long\n\0".as_ptr()); return -1; }

    let path_str = core::str::from_utf8(core::slice::from_raw_parts(path_ptr, path_len)).unwrap_or("");
    let is_disk = path_str.starts_with("/disk/");
    let mut memory_data = Vec::new();

    if !is_disk {
        let size = avfs_get_filesize(path_ptr);
        if size <= 0 || size as usize > MAX_AVFS_FILE_BYTES {
            print(b"mp4: file is missing, empty, or exceeds AVFS capacity\n\0".as_ptr()); return -1;
        }
        memory_data = vec![0u8; size as usize];
        if avfs_read_file(path_ptr, memory_data.as_mut_ptr(), size as u32, 0) != 0 {
            print(b"mp4: unable to read file\n\0".as_ptr()); return -1;
        }
    }

    let temp_source = if is_disk {
        match crate::disk_store::DiskReader::open(path_str) {
            Ok(reader) if reader.size() > 0 => Source::Disk(reader),
            Ok(_)  => { print(b"mp4: file is empty\n\0".as_ptr()); return -1; }
            Err(_) => { print(b"mp4: unable to open disk file\n\0".as_ptr()); return -1; }
        }
    } else {
        Source::Memory(&memory_data)
    };

    let file_size = temp_source.size();
    let Some((moov_start, moov_end)) = find_moov(&temp_source, file_size) else {
        print(b"mp4: unsupported or malformed MP4\n\0".as_ptr()); return -1;
    };
    let moov_len = moov_end - moov_start;
    if moov_len == 0 || moov_len > MAX_MOOV_BYTES {
        print(b"mp4: metadata missing or exceeds 32 MiB\n\0".as_ptr()); return -1;
    }
    let mut moov = vec![0u8; moov_len];
    if !temp_source.read_at(moov_start, &mut moov) {
        print(b"mp4: unable to read metadata\n\0".as_ptr()); return -1;
    }
    let Some(movie) = parse_moov(&moov, file_size) else {
        print(b"mp4: unsupported or malformed MP4\n\0".as_ptr()); return -1;
    };
    drop(moov);

    let Some(video) = movie.video else { return -1; };
    let audio = movie.audio;

    if !matches!(&video.codec, b"avc1" | b"avc3")
        || video.width == 0 || video.width > 640 || video.height == 0 || video.height > 360
    {
        print(b"mp4: target supports avc1 up to 640x360\n\0".as_ptr()); return -1;
    }
    if video.samples.is_empty() {
        print(b"mp4: video track is empty\n\0".as_ptr()); return -1;
    }

    let duration_ms = VideoPipe::new(&video).duration_ms();

    if opts.info_only {
        print_info(&video, audio.as_ref(), file_size, duration_ms);
        return 0;
    }

    if opts.debug {
        if let Some(ref aud) = audio {
            dbg_num(b"[dbg] aac_native_rate=\0", get_aac_sample_rate(&aud.decoder_config) as usize);
        }
    }

    let total_repeats = if opts.loop_play { usize::MAX } else { opts.repeat };
    let mut exit_code = 0i32;

    'repeat: for rep in 0..total_repeats {
        let source = if is_disk {
            match crate::disk_store::DiskReader::open(path_str) {
                Ok(reader) => Source::Disk(reader),
                Err(_) => { print(b"mp4: unable to open disk file\n\0".as_ptr()); exit_code = -1; break; }
            }
        } else {
            Source::Memory(&memory_data)
        };

        let mut h264 = rusty_h264_decoder::Decoder::new();
        if h264.decode(&video.decoder_config).is_err() {
            print(b"mp4: invalid AVC configuration\n\0".as_ptr()); exit_code = -1; break;
        }

        let mut aac: Option<rusty_aac::AacDecoder> = None;
        if let Some(ref aud_track) = audio {
            if let Ok(d) = rusty_aac::AacDecoder::with_config_bytes(&aud_track.decoder_config) {
                aac = Some(d);
            }
        }

        let ac97 = Ac97::initialize(48_000);
        if ac97.is_none() {
            print(b"mp4: warning - AC97 unavailable, playing video-only\n\0".as_ptr());
        }

        let mut pipe = VideoPipe::new(&video);
        let mut sample_buf: Vec<u8> = Vec::new();
        let mut audio_index = 0usize;

        let start_ms = if rep == 0 && opts.seek_ms > 0 {
            let actual = pipe.seek(&mut h264, opts.seek_ms);
            if let Some(ref aud_track) = audio {
                let base_pts = aud_track.samples.get(0).map_or(0, |s| s.pts);
                let tgt_pts  = base_pts + (opts.seek_ms as i64 * aud_track.timescale as i64 / 1_000);
                audio_index  = audio_seek_index(aud_track, tgt_pts);
            }
            actual
        } else { 0 };

        // Pre-fill two half-buffers of audio before starting DMA.
        let max_elements = AUDIO_HALF_SAMPLES * 2;
        let mut staging: Vec<i16> = Vec::with_capacity(max_elements * 4);

        if let Some(ref aud_track) = audio {
            while staging.len() < max_elements * 2 && audio_index < aud_track.samples.len() {
                if !decode_audio_slice(&source, aud_track, &mut aac, &mut audio_index, &mut staging, &mut sample_buf, 1, max_elements * 2) {
                    break;
                }
            }
        }

        if let Some(ref ac97_dev) = ac97 {
            for half in 0..2usize {
                let chunk_len = staging.len().min(max_elements);
                let mut chunk = vec![0i16; max_elements];
                if chunk_len > 0 {
                    chunk[..chunk_len].copy_from_slice(&staging[..chunk_len]);
                    staging.drain(0..chunk_len);
                }
                if !ac97_dev.load_half(half, &chunk, opts.volume) {
                    print(b"mp4: AC97 buffer load failed\n\0".as_ptr());
                    exit_code = -1; break 'repeat;
                }
            }
        }

        // Prime the video pipeline with a few frames before opening the screen.
        while pipe.ready.is_empty() && !pipe.exhausted() {
            if !pipe.step(&source, &mut h264, 0) {
                print(b"mp4: video decode failed\n\0".as_ptr());
                exit_code = -1; break 'repeat;
            }
        }

        let Some(mut screen) = Screen::open(video.width as usize, video.height as usize) else {
            print(b"mp4: VBE graphics mode unavailable\n\0".as_ptr());
            exit_code = -1; break 'repeat;
        };

        core::arch::asm!("sti");

        if let Some(ref ac97_dev) = ac97 {
            ac97_dev.start();
        }

        // next_fill tracks which half-buffer to write next.
        // We just loaded half 0 and half 1, so we start by refilling half 0
        // once the DMA engine moves past it (i.e. when active_half becomes 1).
        let mut next_fill: usize = 0;

        let mut clock_base: u32       = get_ticks();
        let mut playback_offset_ms    = start_ms;
        let mut paused                = false;
        let mut pause_ticks: u32      = 0;
        let mut fps_frames: u32       = 0;
        let mut fps_timer: u32        = get_ticks();
        let mut last_debug_ms: u64    = 0;

        // Reset the keyboard ring for this session.
        KB_HEAD = 0;
        KB_TAIL = 0;

        'inner: loop {
            // ── 1. Input ─────────────────────────────────────────────────────
            kb_poll();
            match kb_dequeue() {
                Key::Esc | Key::Q => {
                    if let Some(ref ac97_dev) = ac97 { ac97_dev.stop(); }
                    end_video();
                    print(b"mp4: playback stopped by user\n\0".as_ptr());
                    exit_code = 0;
                    break 'repeat;
                }

                Key::Space => {
                    if paused {
                        let dur = get_ticks().wrapping_sub(pause_ticks);
                        clock_base = clock_base.wrapping_add(dur);
                        paused = false;
                    } else {
                        paused = true;
                        pause_ticks = get_ticks();
                    }
                }

                key @ (Key::Left | Key::Right | Key::Up | Key::Down) => {
                    let now_ms = if paused {
                        playback_offset_ms
                    } else {
                        playback_offset_ms + get_ticks().wrapping_sub(clock_base) as u64
                    };
                    let delta: i64 = match key {
                        Key::Right =>  10_000,
                        Key::Left  => -10_000,
                        Key::Up    =>  60_000,
                        Key::Down  => -60_000,
                        _ => 0,
                    };
                    let target_ms = (now_ms as i64 + delta).max(0) as u64;
                    let actual_ms = pipe.seek(&mut h264, target_ms);

                    if let Some(ref aud_track) = audio {
                        let base_pts   = aud_track.samples.get(0).map_or(0, |s| s.pts);
                        let target_pts = base_pts + (target_ms as i64 * aud_track.timescale as i64 / 1_000);
                        audio_index = audio_seek_index(aud_track, target_pts);
                        staging.clear();
                    }

                    playback_offset_ms = actual_ms;
                    clock_base = get_ticks();
                    if paused { pause_ticks = clock_base; }

                    // Show the first ready frame immediately when paused.
                    if paused && !pipe.ready.is_empty() {
                        if let Some((_, frame)) = pipe.ready.pop_front() {
                            screen.present(&frame);
                            screen.update_hud_inplace(actual_ms, duration_ms, paused, opts.no_hud);
                        }
                    }
                }

                Key::None => {}
            }

            // ── 2. Audio DMA refill ───────────────────────────────────────────
            // Only write to the half the DMA engine has finished playing.
            // This gates audio consumption to the hardware clock — no more
            // than one half-buffer (~42 ms) per DMA period.
            if !paused {
                if let (Some(ref ac97_dev), Some(ref aud_track)) = (&ac97, &audio) {
                    if ac97_dev.half_free(next_fill) {
                        // Top up the staging buffer.
                        while staging.len() < max_elements * 4 && audio_index < aud_track.samples.len() {
                            if !decode_audio_slice(&source, aud_track, &mut aac, &mut audio_index, &mut staging, &mut sample_buf, 4, max_elements * 4) {
                                break;
                            }
                        }
                        let mut chunk = vec![0i16; max_elements];
                        let chunk_len = staging.len().min(max_elements);
                        if chunk_len > 0 {
                            chunk[..chunk_len].copy_from_slice(&staging[..chunk_len]);
                            staging.drain(0..chunk_len);
                        }
                        ac97_dev.load_half(next_fill, &chunk, opts.volume);
                        ac97_dev.publish();
                        next_fill = 1 - next_fill;
                    }
                }
            }

            // ── 3. Video decode & present ─────────────────────────────────────
            let now_ms = if paused {
                playback_offset_ms
            } else {
                playback_offset_ms + get_ticks().wrapping_sub(clock_base) as u64
            };

            if !paused {
                if !pipe.exhausted() && pipe.ready.len() < VIDEO_AHEAD_FRAMES {
                    pipe.resync_if_late(now_ms);
                    if !pipe.step(&source, &mut h264, now_ms + DECODE_COST_MS) {
                        if let Some(ref ac97_dev) = ac97 { ac97_dev.stop(); }
                        end_video();
                        print(b"mp4: video decode failed\n\0".as_ptr());
                        exit_code = -1; break 'repeat;
                    }
                }

                let drawn_before = pipe.drawn;
                if !pipe.present_due(now_ms, &mut screen) {
                    if let Some(ref ac97_dev) = ac97 { ac97_dev.stop(); }
                    end_video();
                    print(b"mp4: video presentation failed\n\0".as_ptr());
                    exit_code = -1; break 'repeat;
                }
                if pipe.drawn != drawn_before {
                    fps_frames += 1;
                    screen.flip(now_ms, duration_ms, false, opts.no_hud);
                }

                if opts.fps_stats && get_ticks().wrapping_sub(fps_timer) >= 1_000 {
                    dbg_num(b"[fps] frames/sec=\0", fps_frames as usize);
                    fps_frames = 0;
                    fps_timer = get_ticks();
                }

                if opts.debug && now_ms.saturating_sub(last_debug_ms) >= 1_000 {
                    dbg_num(b"[dbg] now_ms=\0",     now_ms as usize);
                    dbg_num(b"[dbg] v_next=\0",     pipe.next);
                    dbg_num(b"[dbg] v_ready=\0",    pipe.ready.len());
                    dbg_num(b"[dbg] v_drawn=\0",    pipe.drawn as usize);
                    dbg_num(b"[dbg] v_skipped=\0",  pipe.skipped as usize);
                    dbg_num(b"[dbg] v_dropped=\0",  pipe.dropped as usize);
                    dbg_num(b"[dbg] v_errors=\0",   pipe.errors as usize);
                    dbg_num(b"[dbg] a_index=\0",    audio_index);
                    dbg_num(b"[dbg] a_total=\0",    audio.as_ref().map_or(0, |a| a.samples.len()));
                    last_debug_ms = now_ms;
                }
            } else {
                // While paused just refresh the HUD and yield.
                screen.update_hud_inplace(now_ms, duration_ms, true, opts.no_hud);
                sleep_ms(5);
            }

            // ── 4. End-of-stream check ────────────────────────────────────────
            let audio_done = audio.as_ref().map_or(true, |a| audio_index >= a.samples.len());
            if pipe.done() && audio_done {
                break 'inner;
            }

            // ── 5. Yield when there is nothing urgent to do ───────────────────
            // Only sleep if the video pipeline is fully buffered AND the audio
            // half is still playing (i.e. not yet free to refill). This keeps
            // CPU usage low during normal playback without starving either stream.
            let video_idle = pipe.exhausted() || pipe.ready.len() >= VIDEO_AHEAD_FRAMES;
            let audio_idle = ac97.as_ref().map_or(true, |dev| !dev.half_free(next_fill));
            if video_idle && audio_idle {
                sleep_ms(1);
            }
        }

        if let Some(ref ac97_dev) = ac97 { ac97_dev.stop(); }
        end_video();
    }

    if exit_code == 0 {
        print(b"mp4: playback complete\n\0".as_ptr());
    }
    exit_code
}

// ── Public C API ──────────────────────────────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn rust_mp4_test_audio() -> i32 {
    let Some(ac97) = Ac97::initialize(48_000) else {
        print(b"mp4: AC97 device unavailable\n\0".as_ptr()); return -1;
    };
    let mut pcm = Vec::with_capacity(AUDIO_HALF_SAMPLES * 2);
    for frame in 0..AUDIO_HALF_SAMPLES {
        let sample = if (frame * 880 / 48_000) & 1 == 0 { 8192 } else { -8192 };
        pcm.push(sample); pcm.push(sample);
    }
    if !ac97.load_half(0, &pcm, 100) || !ac97.load_half(1, &pcm, 100) {
        print(b"mp4: AC97 buffer load failed\n\0".as_ptr()); return -1;
    }
    ac97.start();
    sleep_ms(6_000);
    ac97.stop();
    print(b"mp4: AC97 DMA test passed\n\0".as_ptr());
    0
}

#[no_mangle]
pub unsafe extern "C" fn rust_mp4_play_file(path: *const u8) -> i32 {
    let opts = PlayOpts::defaults();
    play_file(path, &opts)
}

#[no_mangle]
pub unsafe extern "C" fn rust_mp4_play_file_ex(args_ptr: *const u8, _unused: *const u8) -> i32 {
    let (opts, path_buf, path_len) = parse_args_str(args_ptr);
    if opts.help || path_len == 0 { print_help(); return -1; }
    play_file(path_buf.as_ptr(), &opts)
}

// ── Music player ──────────────────────────────────────────────────────────────
// Audio-only AAC-LC playback for the `ytm` client. The file is an .m4a in the fetch store (/disk/NAME); both
// fragmented (YouTube DASH, moof/trun) and ordinary (stbl) layouts work. The caller keeps calling rust_music_pump();
// it refills whichever DMA half the codec has finished with. The codec runs at the file's own sample rate.

struct Music {
    source: Source<'static>,
    track: Track,
    aac: rusty_aac::AacDecoder,
    ac97: Ac97,
    rate: u32,
    index: usize,
    staging: Vec<i16>,
    sample_buf: Vec<u8>,
    next_fill: usize,
    volume: u32,
    paused: bool,
    finished: bool,
    tail: u32,
    len_ms: u64,
}

static mut MUSIC: Option<Music> = None;

// Samples of the first audio track of a fragmented MP4: every moof/traf/trun run for the track in `trak`.
fn parse_fragmented_audio(source: &Source, moov: &[u8], trak: Atom, file_size: u64) -> Option<Track> {
    let mdia = find_child(moov, trak.payload, trak.end, b"mdia")?;
    let hdlr = find_child(moov, mdia.payload, mdia.end, b"hdlr")?;
    if moov.get(hdlr.payload + 8..hdlr.payload + 12)? != b"soun" { return None; }
    let mdhd = find_child(moov, mdia.payload, mdia.end, b"mdhd")?;
    let timescale = be_u32(moov, mdhd.payload + if version(moov, mdhd)? == 1 { 20 } else { 12 })?;
    if timescale == 0 { return None; }
    let minf = find_child(moov, mdia.payload, mdia.end, b"minf")?;
    let stbl = find_child(moov, minf.payload, minf.end, b"stbl")?;
    let stsd = find_child(moov, stbl.payload, stbl.end, b"stsd")?;
    let entry = atom_at(moov, stsd.payload + 8, stsd.end)?;
    if &entry.kind != b"mp4a" { return None; }
    let (_, decoder_config) = parse_decoder_config(moov, entry, entry.kind)?;
    let tkhd = find_child(moov, trak.payload, trak.end, b"tkhd")?;
    let track_id = be_u32(moov, tkhd.payload + if version(moov, tkhd)? == 1 { 20 } else { 12 })?;

    let (mut def_duration, mut def_size) = (1024u32, 0u32);
    if let Some(mvex) = find_child(moov, 0, moov.len(), b"mvex") {
        for trex in children(moov, mvex.payload, mvex.end) {
            if &trex.kind == b"trex" && be_u32(moov, trex.payload + 4) == Some(track_id) {
                def_duration = be_u32(moov, trex.payload + 12).filter(|d| *d != 0).unwrap_or(1024);
                def_size = be_u32(moov, trex.payload + 16).unwrap_or(0);
            }
        }
    }

    let mut samples: Vec<Sample> = Vec::new();
    let mut decode_time = 0i64;
    let mut offset = 0u64;
    while offset + 8 <= file_size {
        let mut header = [0u8; 16];
        if !source.read_at(offset as usize, &mut header[..8]) { break; }
        let size32 = u32::from_be_bytes(header[..4].try_into().ok()?);
        let kind: [u8; 4] = header[4..8].try_into().ok()?;
        let (header_size, size) = match size32 {
            0 => (8u64, file_size - offset),
            1 => {
                if offset + 16 > file_size || !source.read_at(offset as usize, &mut header) { break; }
                (16u64, u64::from_be_bytes(header[8..16].try_into().ok()?))
            }
            n => (8u64, n as u64),
        };
        if size < header_size || offset.checked_add(size)? > file_size { break; }
        if &kind == b"moof" {
            let body_len = (size - header_size) as usize;
            if body_len > 4 * 1024 * 1024 { return None; }
            let mut data = vec![0u8; body_len];
            if !source.read_at((offset + header_size) as usize, &mut data) { return None; }
            for traf in children(&data, 0, data.len()).into_iter().filter(|a| &a.kind == b"traf") {
                let Some(tfhd) = find_child(&data, traf.payload, traf.end, b"tfhd") else { continue };
                let flags = be_u32(&data, tfhd.payload)? & 0xFF_FFFF;
                if be_u32(&data, tfhd.payload + 4)? != track_id { continue; }
                let mut p = tfhd.payload + 8;
                let mut base = offset;
                if flags & 0x1 != 0 { base = be_u64(&data, p)?; p += 8; }
                if flags & 0x2 != 0 { p += 4; }
                let mut traf_duration = def_duration;
                let mut traf_size = def_size;
                if flags & 0x8 != 0 { traf_duration = be_u32(&data, p)?; p += 4; }
                if flags & 0x10 != 0 { traf_size = be_u32(&data, p)?; }
                if let Some(tfdt) = find_child(&data, traf.payload, traf.end, b"tfdt") {
                    decode_time = if version(&data, tfdt)? == 1 { be_u64(&data, tfdt.payload + 4)? as i64 } else { be_u32(&data, tfdt.payload + 4)? as i64 };
                }
                let mut cursor = base;
                for trun in children(&data, traf.payload, traf.end).into_iter().filter(|a| &a.kind == b"trun") {
                    let tf = be_u32(&data, trun.payload)? & 0xFF_FFFF;
                    let count = be_u32(&data, trun.payload + 4)? as usize;
                    let mut q = trun.payload + 8;
                    if tf & 0x1 != 0 { cursor = (base as i64 + be_u32(&data, q)? as i32 as i64) as u64; q += 4; }
                    if tf & 0x4 != 0 { q += 4; }
                    if samples.len() + count > MAX_SAMPLES { return None; }
                    for _ in 0..count {
                        let mut duration = traf_duration;
                        let mut sample_size = traf_size;
                        if tf & 0x100 != 0 { duration = be_u32(&data, q)?; q += 4; }
                        if tf & 0x200 != 0 { sample_size = be_u32(&data, q)?; q += 4; }
                        if tf & 0x400 != 0 { q += 4; }
                        if tf & 0x800 != 0 { q += 4; }
                        let end = cursor.checked_add(sample_size as u64)?;
                        if sample_size == 0 || end > file_size { return None; }
                        samples.push(Sample { offset: cursor as usize, size: sample_size as usize, pts: decode_time, duration, keyframe: true });
                        decode_time += duration as i64;
                        cursor = end;
                    }
                }
            }
        }
        if size32 == 0 { break; }
        offset += size;
    }
    if samples.is_empty() { return None; }
    Some(Track { kind: TrackKind::Audio, codec: *b"mp4a", timescale, width: 0, height: 0, nal_length_size: 0, decoder_config, samples })
}

// The first AAC audio track of the file, from the ordinary sample tables or from movie fragments.
fn load_audio_track(source: &Source) -> Option<Track> {
    let file_size = source.size();
    let (start, end) = find_moov(source, file_size)?;
    let len = end - start;
    if len == 0 || len > MAX_MOOV_BYTES { return None; }
    let mut moov = vec![0u8; len];
    if !source.read_at(start, &mut moov) { return None; }
    for trak in children(&moov, 0, moov.len()).into_iter().filter(|a| &a.kind == b"trak").take(MAX_TRACKS) {
        if let Some(track) = parse_track(&moov, trak, file_size) {
            if track.kind == TrackKind::Audio && &track.codec == b"mp4a" && !track.samples.is_empty() { return Some(track); }
            continue;
        }
        if let Some(track) = parse_fragmented_audio(source, &moov, trak, file_size) { return Some(track); }
    }
    None
}

// Decodes up to `max_packets` AAC frames into interleaved stereo i16 at the file's own rate.
fn music_decode(m: &mut Music, max_packets: usize, capacity: usize) {
    let mut packets = 0usize;
    while m.index < m.track.samples.len() && packets < max_packets && m.staging.len() + 2 <= capacity {
        let (offset, size, pts) = { let s = &m.track.samples[m.index]; (s.offset, s.size, s.pts) };
        m.index += 1;
        packets += 1;
        m.sample_buf.clear();
        m.sample_buf.resize(size, 0);
        if !m.source.read_at(offset, &mut m.sample_buf) { continue; }
        let Ok(decoded) = m.aac.decode(&m.sample_buf, Some(pts)) else { continue };
        let channels = decoded.channels as usize;
        if channels == 0 { continue; }
        for frame in decoded.samples.chunks(channels) {
            let left = frame[0].clamp(-1.0, 1.0);
            let right = frame.get(1).copied().unwrap_or(frame[0]).clamp(-1.0, 1.0);
            m.staging.push((left * 32767.0) as i16);
            m.staging.push((right * 32767.0) as i16);
        }
    }
}

// Fills both DMA halves from the staging buffer and starts the codec.
unsafe fn music_prime(m: &mut Music) {
    let max = AUDIO_HALF_SAMPLES * 2;
    while m.staging.len() < max * 2 && m.index < m.track.samples.len() {
        music_decode(m, 1, max * 4);
    }
    for half in 0..2usize {
        let mut chunk = vec![0i16; max];
        let n = m.staging.len().min(max);
        chunk[..n].copy_from_slice(&m.staging[..n]);
        m.staging.drain(0..n);
        m.ac97.load_half(half, &chunk, m.volume);
    }
    m.ac97.start();
    m.next_fill = 0;
    m.tail = 0;
}

unsafe fn music_close_inner() {
    if let Some(m) = (*core::ptr::addr_of_mut!(MUSIC)).take() { m.ac97.stop(); }
}

// Opens an .m4a in the fetch store and starts it. 0 ok, -1 cannot open, -2 not an AAC audio file, -3 decoder
// rejected the stream, -4 no sound card. volume is a percentage (0 to 200).
#[no_mangle]
pub unsafe extern "C" fn rust_music_open(path: *const u8, volume: u32) -> i32 {
    music_close_inner();
    if path.is_null() { return -1; }
    let mut n = 0usize;
    while n < 256 && *path.add(n) != 0 { n += 1; }
    let Ok(path_str) = core::str::from_utf8(core::slice::from_raw_parts(path, n)) else { return -1 };
    let Ok(reader) = crate::disk_store::DiskReader::open(path_str) else { return -1 };
    if reader.size() == 0 { return -1; }
    let source = Source::Disk(reader);
    let Some(track) = load_audio_track(&source) else { return -2 };
    let rate = get_aac_sample_rate(&track.decoder_config);
    let Ok(aac) = rusty_aac::AacDecoder::with_config_bytes(&track.decoder_config) else { return -3 };
    let Some(ac97) = Ac97::initialize(rate) else { return -4 };
    let len_ms = track.samples.last().map_or(0, |s| ticks_to_ms(s.pts + s.duration as i64 - track.samples[0].pts, track.timescale));
    let mut m = Music {
        source, track, aac, ac97, rate, index: 0, staging: Vec::with_capacity(AUDIO_HALF_SAMPLES * 8), sample_buf: Vec::new(),
        next_fill: 0, volume: volume.min(200), paused: false, finished: false, tail: 0, len_ms,
    };
    music_prime(&mut m);
    *core::ptr::addr_of_mut!(MUSIC) = Some(m);
    0
}

// The codec stops after the entry named by LVI. Keeping LVI one entry behind CIV means the engine cycles through the
// 32 entries forever; if it halted anyway (we were too slow) it is started again.
unsafe fn music_keep_running(m: &Music) {
    let civ = inb(m.ac97.bus_master + 0x14);
    outb(m.ac97.bus_master + 0x15, civ.wrapping_add(31) & 31);
    if inw(m.ac97.bus_master + 0x16) & 0x01 != 0 {
        outw(m.ac97.bus_master + 0x16, 0x1c);
        outb(m.ac97.bus_master + 0x1b, 1);
    }
}

// Refills the audio buffer when the codec is done with a half. Call at least every 20 ms. 1 playing, 0 finished, -1 not open.
#[no_mangle]
pub unsafe extern "C" fn rust_music_pump() -> i32 {
    let Some(m) = (*core::ptr::addr_of_mut!(MUSIC)).as_mut() else { return -1 };
    if m.finished { return 0; }
    if m.paused { return 1; }
    music_keep_running(m);
    if !m.ac97.half_free(m.next_fill) { return 1; }
    let max = AUDIO_HALF_SAMPLES * 2;
    while m.staging.len() < max && m.index < m.track.samples.len() {
        music_decode(m, 2, max * 4);
    }
    let mut chunk = vec![0i16; max];
    let n = m.staging.len().min(max);
    if n == 0 { m.tail += 1; } else {
        chunk[..n].copy_from_slice(&m.staging[..n]);
        m.staging.drain(0..n);
    }
    m.ac97.load_half(m.next_fill, &chunk, m.volume);
    m.next_fill = 1 - m.next_fill;
    if m.tail >= 3 {
        m.finished = true;
        m.ac97.stop();
        return 0;
    }
    1
}

// Pause (1) or resume (0).
#[no_mangle]
pub unsafe extern "C" fn rust_music_pause(paused: i32) {
    let Some(m) = (*core::ptr::addr_of_mut!(MUSIC)).as_mut() else { return };
    if m.finished || (paused != 0) == m.paused { return; }
    if paused != 0 { m.ac97.stop(); m.paused = true; } else { m.paused = false; music_prime(m); }
}

// Jumps to `ms` from the start of the track.
#[no_mangle]
pub unsafe extern "C" fn rust_music_seek(ms: u32) {
    let Some(m) = (*core::ptr::addr_of_mut!(MUSIC)).as_mut() else { return };
    let base = m.track.samples.first().map_or(0, |s| s.pts);
    let target = base + (ms as u64 * m.track.timescale as u64 / 1000) as i64;
    m.index = audio_seek_index(&m.track, target);
    m.staging.clear();
    m.finished = false;
    m.tail = 0;
    if !m.paused { m.ac97.stop(); music_prime(m); }
}

#[no_mangle]
pub unsafe extern "C" fn rust_music_set_volume(volume: u32) {
    if let Some(m) = (*core::ptr::addr_of_mut!(MUSIC)).as_mut() { m.volume = volume.min(200); }
}

// Playback position in ms (what is being heard, to within the buffered ~100 ms), or 0 when nothing is open.
#[no_mangle]
pub unsafe extern "C" fn rust_music_pos_ms() -> u32 {
    let Some(m) = (*core::ptr::addr_of!(MUSIC)).as_ref() else { return 0 };
    let decoded = match m.track.samples.get(m.index) {
        Some(s) => ticks_to_ms(s.pts - m.track.samples[0].pts, m.track.timescale),
        None => m.len_ms,
    };
    let buffered = (m.staging.len() as u64 / 2) * 1000 / m.rate.max(1) as u64 + 2 * AUDIO_HALF_SAMPLES as u64 * 1000 / m.rate.max(1) as u64;
    decoded.saturating_sub(buffered).min(m.len_ms) as u32
}

#[no_mangle]
pub unsafe extern "C" fn rust_music_len_ms() -> u32 {
    (*core::ptr::addr_of!(MUSIC)).as_ref().map_or(0, |m| m.len_ms as u32)
}

// Codec sample rate in Hz (0 when nothing is open).
#[no_mangle]
pub unsafe extern "C" fn rust_music_rate() -> u32 {
    (*core::ptr::addr_of!(MUSIC)).as_ref().map_or(0, |m| m.rate)
}

#[no_mangle]
pub unsafe extern "C" fn rust_music_close() { music_close_inner(); }

// ── Raw PCM stream ────────────────────────────────────────────────────────────
// For programs that mix their own audio (Doom): open at a rate, then whenever rust_pcm_ready() says the codec has
// finished a half, hand over AUDIO_HALF_SAMPLES stereo frames with rust_pcm_write().

static mut PCM_DEV: Option<Ac97> = None;
static mut PCM_NEXT: usize = 0;

unsafe fn pcm_close_inner() {
    if let Some(d) = (*core::ptr::addr_of_mut!(PCM_DEV)).take() { d.stop(); }
}

// Frames per half on success, -1 without a sound card.
#[no_mangle]
pub unsafe extern "C" fn rust_pcm_open(rate: u32) -> i32 {
    music_close_inner();
    pcm_close_inner();
    let Some(dev) = Ac97::initialize(rate) else { return -1 };
    let silence = [0i16; AUDIO_HALF_SAMPLES * 2];
    dev.load_half(0, &silence, 100);
    dev.load_half(1, &silence, 100);
    dev.start();
    PCM_NEXT = 0;
    *core::ptr::addr_of_mut!(PCM_DEV) = Some(dev);
    AUDIO_HALF_SAMPLES as i32
}

// Keeps the codec cycling (see music_keep_running); 1 when the next half can be written.
#[no_mangle]
pub unsafe extern "C" fn rust_pcm_ready() -> i32 {
    let Some(d) = (*core::ptr::addr_of!(PCM_DEV)).as_ref() else { return 0 };
    let civ = inb(d.bus_master + 0x14);
    outb(d.bus_master + 0x15, civ.wrapping_add(31) & 31);
    if inw(d.bus_master + 0x16) & 0x01 != 0 {
        outw(d.bus_master + 0x16, 0x1c);
        outb(d.bus_master + 0x1b, 1);
    }
    d.half_free(PCM_NEXT) as i32
}

// frames stereo frames (interleaved i16), at most AUDIO_HALF_SAMPLES; the rest of the half is silence.
#[no_mangle]
pub unsafe extern "C" fn rust_pcm_write(pcm: *const i16, frames: u32) {
    let Some(d) = (*core::ptr::addr_of!(PCM_DEV)).as_ref() else { return };
    let n = (frames as usize).min(AUDIO_HALF_SAMPLES);
    if n == 0 || pcm.is_null() { return; }
    d.load_half(PCM_NEXT, core::slice::from_raw_parts(pcm, n * 2), 100);
    PCM_NEXT = 1 - PCM_NEXT;
}

#[no_mangle]
pub unsafe extern "C" fn rust_pcm_close() { pcm_close_inner(); }

// ── Sound effects ─────────────────────────────────────────────────────────────
// Short effects for the Discord client: synthesized tones or a WAV file, played once through the AC97 codec.

const SFX_RATE: usize = 48_000;
const SFX_MAX_FRAMES: usize = SFX_RATE * 2;
const SFX_ENTRY_FRAMES: usize = 8192;
const SFX_ENTRIES: usize = SFX_MAX_FRAMES / SFX_ENTRY_FRAMES + 1;

#[repr(align(16))]
struct SfxPcm([i16; SFX_ENTRIES * SFX_ENTRY_FRAMES * 2]);
static mut SFX_PCM: SfxPcm = SfxPcm([0; SFX_ENTRIES * SFX_ENTRY_FRAMES * 2]);
static mut SFX_BDL: AudioBdlTable = AudioBdlTable([AudioBdl { address: 0, words: 0, flags: 0 }; AUDIO_BDL_COUNT]);
static mut SFX_DEV: Option<Ac97> = None;
static mut SFX_TRIED: bool = false;
static mut SINE: [i16; 1024] = [0; 1024];
static mut SINE_READY: bool = false;

// One period of a sine, from the parabola approximation with the usual correction term.
unsafe fn sine_init() {
    if SINE_READY { return; }
    let table = &mut *core::ptr::addr_of_mut!(SINE);
    for i in 0..512i64 {
        let s = i * (512 - i);                       // 0..65536 (q16)
        let ss = (s * s) >> 16;
        let y = s + ((230 * (ss - s)) >> 10);          // shifts: i64 division of negatives is avoided on this target
        let v = ((y * 32767) >> 16) as i16;
        table[i as usize] = v;
        table[i as usize + 512] = -v;
    }
    SINE_READY = true;
}

// Adds a decaying sine (optionally gliding to sweep_to Hz) to buf, starting at sample `start`.
unsafe fn add_tone(buf: &mut [i32], start: usize, freq: u32, len: usize, tau_ms: u32, gain: i64, sweep_to: u32) {
    let table = &*core::ptr::addr_of!(SINE);
    let end = (start + len).min(buf.len());
    let tau = (SFX_RATE as i64 * tau_ms as i64 / 1000).max(1);
    let dec: i64 = (1i64 << 30) - ((1i64 << 30) / tau);
    let (mut phase, mut amp) = (0u32, 1i64 << 30);
    for n in start..end {
        let i = (n - start) as i64;
        let f = if sweep_to == 0 { freq as i64 }
                else if sweep_to > freq { freq as i64 + (sweep_to - freq) as i64 * i / len as i64 }
                else { freq as i64 - (freq - sweep_to) as i64 * i / len as i64 };
        phase = phase.wrapping_add((f as u64 * 89_478) as u32);
        let attack = if i < 96 { i * (1i64 << 30) / 96 } else { 1i64 << 30 };
        let env = amp.min(attack);
        let s = table[(phase >> 22) as usize] as i64;
        buf[n] += (((s * env >> 30) * gain) >> 8) as i32;
        amp = amp * dec >> 30;
    }
}

unsafe fn synth(kind: u32) -> Vec<i32> {
    sine_init();
    let ms = |t: usize| SFX_RATE * t / 1000;
    let mut buf = vec![0i32; ms(match kind { 1 => 900, 3 => 1500, 2 => 160, _ => 500 })];
    match kind {
        1 => {   // chime: rising triad
            add_tone(&mut buf, 0, 880, ms(500), 110, 170, 0);
            add_tone(&mut buf, ms(120), 1109, ms(500), 110, 170, 0);
            add_tone(&mut buf, ms(240), 1319, ms(600), 150, 190, 0);
        }
        2 => add_tone(&mut buf, 0, 1100, ms(110), 35, 230, 250),   // pop
        3 => {   // bell: inharmonic partials
            add_tone(&mut buf, 0, 660, ms(1400), 380, 160, 0);
            add_tone(&mut buf, 0, 1650, ms(1000), 260, 90, 0);
            add_tone(&mut buf, 0, 2640, ms(700), 150, 60, 0);
        }
        _ => {   // ping
            add_tone(&mut buf, 0, 1319, ms(450), 90, 200, 0);
            add_tone(&mut buf, 0, 1975, ms(300), 60, 90, 0);
        }
    }
    buf
}

// PCM WAV (8 or 16 bit, mono or stereo, any rate) to mono samples at 48 kHz.
unsafe fn wav_to_mono(data: &[u8]) -> Option<Vec<i32>> {
    if data.len() < 44 || &data[0..4] != b"RIFF" || &data[8..12] != b"WAVE" { return None; }
    let (mut fmt, mut chans, mut rate, mut bits) = (0u16, 0usize, 0u64, 0usize);
    let (mut off, mut len) = (0usize, 0usize);
    let mut pos = 12usize;
    while pos + 8 <= data.len() {
        let size = u32::from_le_bytes([data[pos + 4], data[pos + 5], data[pos + 6], data[pos + 7]]) as usize;
        let body = pos + 8;
        match &data[pos..pos + 4] {
            b"fmt " if body + 16 <= data.len() => {
                fmt = u16::from_le_bytes([data[body], data[body + 1]]);
                chans = u16::from_le_bytes([data[body + 2], data[body + 3]]) as usize;
                rate = u32::from_le_bytes([data[body + 4], data[body + 5], data[body + 6], data[body + 7]]) as u64;
                bits = u16::from_le_bytes([data[body + 14], data[body + 15]]) as usize;
            }
            b"data" => { off = body; len = size.min(data.len() - body); break; }
            _ => {}
        }
        pos = body + size + (size & 1);
    }
    if fmt != 1 || !(1..=2).contains(&chans) || (bits != 8 && bits != 16) || rate < 4000 || len == 0 { return None; }
    let frame_bytes = chans * bits / 8;
    let frames_in = len / frame_bytes;
    let frames_out = (frames_in as u64 * SFX_RATE as u64 / rate).min(SFX_MAX_FRAMES as u64) as usize;
    let mut out = Vec::with_capacity(frames_out);
    for o in 0..frames_out {
        let i = (o as u64 * rate / SFX_RATE as u64) as usize;
        let mut sum = 0i32;
        for c in 0..chans {
            let p = off + i * frame_bytes + c * bits / 8;
            sum += if bits == 16 { i16::from_le_bytes([data[p], data[p + 1]]) as i32 } else { (data[p] as i32 - 128) << 8 };
        }
        out.push(sum / chans as i32);
    }
    Some(out)
}

unsafe fn sfx_device() -> Option<&'static Ac97> {
    if !SFX_TRIED {
        SFX_TRIED = true;
        *core::ptr::addr_of_mut!(SFX_DEV) = Ac97::initialize(SFX_RATE as u32);
    }
    (*core::ptr::addr_of!(SFX_DEV)).as_ref()
}

// Starts playing mono samples; returns the length in ms, or -1 when there is no sound card.
unsafe fn sfx_submit(mono: &[i32], volume: u32) -> i32 {
    let Some(dev) = sfx_device() else { return -1 };
    let frames = mono.len().min(SFX_MAX_FRAMES);
    if frames == 0 { return 0; }
    dev.stop();
    let entries = (frames + SFX_ENTRY_FRAMES - 1) / SFX_ENTRY_FRAMES;
    let gain_q8 = volume.min(100) as i64 * 256 / 100;
    let pcm = core::ptr::addr_of_mut!(SFX_PCM.0).cast::<i16>();
    for i in 0..entries * SFX_ENTRY_FRAMES {
        let v = if i < frames { ((mono[i] as i64 * gain_q8) >> 8).clamp(-32768, 32767) as i16 } else { 0 };
        pcm.add(i * 2).write(v);
        pcm.add(i * 2 + 1).write(v);
    }
    let bdl = core::ptr::addr_of_mut!(SFX_BDL).cast::<AudioBdl>();
    for e in 0..entries {
        bdl.add(e).write(AudioBdl {
            address: (pcm as usize + e * SFX_ENTRY_FRAMES * 4) as u32,
            words: (SFX_ENTRY_FRAMES * 2) as u16,
            flags: if e + 1 == entries { BDL_IOC } else { 0 },
        });
    }
    outw(dev.bus_master + 0x16, 0x1c);
    outl(dev.bus_master + 0x10, bdl as u32);
    outb(dev.bus_master + 0x15, (entries - 1) as u8);
    outb(dev.bus_master + 0x1b, 1);
    (frames * 1000 / SFX_RATE) as i32
}

// 1 when an AC97 codec was found.
#[no_mangle]
pub unsafe extern "C" fn rust_sfx_init() -> i32 { sfx_device().is_some() as i32 }

// kind: 0 ping, 1 chime, 2 pop, 3 bell. Returns the length in ms, or -1 without a sound card.
#[no_mangle]
pub unsafe extern "C" fn rust_sfx_builtin(kind: u32, volume: u32) -> i32 {
    if sfx_device().is_none() { return -1; }
    sfx_submit(&synth(kind), volume)
}

// Plays a WAV held in memory. Returns the length in ms, -1 without a sound card, -2 for an unsupported file.
#[no_mangle]
pub unsafe extern "C" fn rust_sfx_wav(data: *const u8, len: u32, volume: u32) -> i32 {
    if sfx_device().is_none() { return -1; }
    let Some(mono) = wav_to_mono(core::slice::from_raw_parts(data, len as usize)) else { return -2 };
    sfx_submit(&mono, volume)
}
