use alloc::{vec, vec::Vec};

const SECTOR_SIZE: usize = 512;
const WRITE_CHUNK: usize = 127 * SECTOR_SIZE;
const DIRECTORY_LBA: u32 = 1;
const DIRECTORY_ENTRIES: usize = 32;
const ENTRY_SIZE: usize = 136;
const DIRECTORY_SECTORS: u32 = 9;
const DATA_START_LBA: u32 = 16;
// The store lives in the hidden file STORE.BIN of the FAT32 volume (src/Avfs/Avfs.c, STORE_LBA / STORE_SECTORS): 224 MiB.
const BASE_LBA: u32 = 65_536;
const DISK_SECTORS: u32 = 458_752;
const MAX_FILE_SIZE: u64 = u32::MAX as u64;
const MAGIC: &[u8; 8] = b"RDFETCH1";

// All sector traffic goes through the C driver (src/Avfs/Avfs.c: DMA when the controller has it), which owns the ATA registers.
extern "C" {
    fn avfs_raw_read(lba: u32, count: u32, buf: *mut u8) -> i32;
    fn avfs_raw_write(lba: u32, count: u32, buf: *const u8) -> i32;
    fn avfs_raw_flush();
}

fn check_range(lba: u32, count: u32) -> Result<u32, &'static str> {
    if lba.checked_add(count).map_or(true, |end| end > DISK_SECTORS) || lba + BASE_LBA + count >= (1 << 28) {
        return Err("disk: LBA out of range");
    }
    Ok(lba + BASE_LBA)
}

fn read_sectors(lba: u32, output: &mut [u8]) -> Result<(), &'static str> {
    let count = (output.len() / SECTOR_SIZE) as u32;
    let abs = check_range(lba, count)?;
    if unsafe { avfs_raw_read(abs, count, output.as_mut_ptr()) } != 0 { return Err("disk: ATA read error"); }
    Ok(())
}

fn write_sectors(lba: u32, input: &[u8]) -> Result<(), &'static str> {
    let count = (input.len() / SECTOR_SIZE) as u32;
    let abs = check_range(lba, count)?;
    if unsafe { avfs_raw_write(abs, count, input.as_ptr()) } != 0 { return Err("disk: ATA write error"); }
    Ok(())
}

fn read_sector(lba: u32, output: &mut [u8; SECTOR_SIZE]) -> Result<(), &'static str> { read_sectors(lba, output) }

fn write_sector(lba: u32, input: &[u8; SECTOR_SIZE]) -> Result<(), &'static str> { write_sectors(lba, input) }

fn flush_cache() -> Result<(), &'static str> {
    unsafe { avfs_raw_flush(); }
    Ok(())
}

fn initialize() -> Result<(), &'static str> {
    let mut header = [0; SECTOR_SIZE];
    read_sector(0, &mut header)?;
    if &header[..8] == MAGIC {
        let next = u32::from_le_bytes(header[8..12].try_into().unwrap());
        if next < DATA_START_LBA || next >= DISK_SECTORS {
            return Err("disk: invalid fetch-disk metadata");
        }
        return Ok(());
    }
    header.fill(0);
    header[..8].copy_from_slice(MAGIC);
    header[8..12].copy_from_slice(&DATA_START_LBA.to_le_bytes());
    write_sector(0, &header)?;
    let blank = [0; SECTOR_SIZE];
    for index in 0..DIRECTORY_SECTORS {
        write_sector(DIRECTORY_LBA + index, &blank)?;
    }
    Ok(())
}

fn read_directory() -> Result<Vec<u8>, &'static str> {
    initialize()?;
    let mut directory = vec![0; DIRECTORY_SECTORS as usize * SECTOR_SIZE];
    for index in 0..DIRECTORY_SECTORS as usize {
        let mut sector = [0; SECTOR_SIZE];
        read_sector(DIRECTORY_LBA + index as u32, &mut sector)?;
        directory[index * SECTOR_SIZE..(index + 1) * SECTOR_SIZE].copy_from_slice(&sector);
    }
    Ok(directory)
}

fn find_entry(directory: &[u8], path: &[u8]) -> Result<usize, &'static str> {
    let mut free = None;
    for index in 0..DIRECTORY_ENTRIES {
        let start = index * ENTRY_SIZE;
        let name = &directory[start..start + 128];
        if name.first().copied().unwrap_or(0) == 0 {
            if free.is_none() { free = Some(index); }
        } else if name.split(|byte| *byte == 0).next() == Some(path) {
            return Ok(index);
        }
    }
    free.ok_or("disk: directory full")
}

fn directory_high_water(directory: &[u8]) -> Result<u32, &'static str> {
    let mut next = DATA_START_LBA;
    for index in 0..DIRECTORY_ENTRIES {
        let start = index * ENTRY_SIZE;
        if directory[start] == 0 { continue; }
        let data_lba = u32::from_le_bytes(directory[start + 128..start + 132].try_into().unwrap());
        let size = u32::from_le_bytes(directory[start + 132..start + 136].try_into().unwrap());
        let sectors = ((size as u64 + SECTOR_SIZE as u64 - 1) / SECTOR_SIZE as u64) as u32;
        next = next.max(data_lba.checked_add(sectors).ok_or("disk: file extent overflow")?);
    }
    Ok(next)
}

fn commit_directory(directory: &[u8], next_lba: u32) -> Result<(), &'static str> {
    let mut header = [0; SECTOR_SIZE];
    header[..8].copy_from_slice(MAGIC);
    header[8..12].copy_from_slice(&next_lba.to_le_bytes());
    write_sector(0, &header)?;
    for index in 0..DIRECTORY_SECTORS as usize {
        let mut sector = [0; SECTOR_SIZE];
        sector.copy_from_slice(&directory[index * SECTOR_SIZE..(index + 1) * SECTOR_SIZE]);
        write_sector(DIRECTORY_LBA + index as u32, &sector)?;
    }
    flush_cache()
}

pub(crate) struct DiskWriter {
    path: [u8; 128],
    start_lba: u32,
    bytes_written: u64,
    buffer: Vec<u8>,          // data not yet on disk, written 127 sectors at a time
}

impl DiskWriter {
    pub fn begin(path: &str) -> Result<Self, &'static str> {
        let name = path.strip_prefix("/disk/").ok_or("disk: output must begin /disk/")?;
        if name.is_empty() || name.len() >= 128 || name.bytes().any(|byte| byte < 32 || byte == 127) {
            return Err("disk: invalid disk filename");
        }
        let directory = read_directory()?;
        let _entry = find_entry(&directory, name.as_bytes())?;
        let start_lba = directory_high_water(&directory)?;
        if start_lba >= DISK_SECTORS { return Err("disk: out of capacity"); }
        let mut path_buffer = [0; 128];
        path_buffer[..name.len()].copy_from_slice(name.as_bytes());
        Ok(Self { path: path_buffer, start_lba, bytes_written: 0, buffer: Vec::with_capacity(WRITE_CHUNK) })
    }

    pub fn write(&mut self, mut data: &[u8]) -> Result<(), &'static str> {
        let new_size = self.bytes_written.checked_add(data.len() as u64).ok_or("disk: file too large")?;
        if new_size > MAX_FILE_SIZE || self.start_lba as u64 + (new_size + 511) / 512 > DISK_SECTORS as u64 {
            return Err("disk: file exceeds 4-GiB limit or disk capacity");
        }
        while !data.is_empty() {
            let count = core::cmp::min(WRITE_CHUNK - self.buffer.len(), data.len());
            self.buffer.extend_from_slice(&data[..count]);
            data = &data[count..];
            if self.buffer.len() == WRITE_CHUNK { self.flush_buffer()?; }
        }
        Ok(())
    }

    fn flush_buffer(&mut self) -> Result<(), &'static str> {
        if self.buffer.is_empty() { return Ok(()); }
        let real = self.buffer.len();
        let padded = (real + SECTOR_SIZE - 1) / SECTOR_SIZE * SECTOR_SIZE;
        self.buffer.resize(padded, 0);
        let lba = self.start_lba + (self.bytes_written / SECTOR_SIZE as u64) as u32;
        write_sectors(lba, &self.buffer)?;
        self.bytes_written += real as u64;
        self.buffer.clear();
        Ok(())
    }

    pub fn finish(mut self) -> Result<(), &'static str> {
        self.flush_buffer()?;
        let mut directory = read_directory()?;
        let name_end = self.path.iter().position(|byte| *byte == 0).unwrap_or(128);
        let entry = find_entry(&directory, &self.path[..name_end])?;
        let offset = entry * ENTRY_SIZE;
        directory[offset..offset + 128].copy_from_slice(&self.path);
        directory[offset + 128..offset + 132].copy_from_slice(&self.start_lba.to_le_bytes());
        directory[offset + 132..offset + 136].copy_from_slice(&(self.bytes_written as u32).to_le_bytes());
        let next = self.start_lba + ((self.bytes_written + 511) / 512) as u32;
        commit_directory(&directory, next)
    }
}

pub(crate) fn file_size(path: &str) -> Result<Option<u32>, &'static str> {
    let name = path.strip_prefix("/disk/").ok_or("disk: path must begin /disk/")?;
    let directory = read_directory()?;
    for index in 0..DIRECTORY_ENTRIES {
        let offset = index * ENTRY_SIZE;
        let current = &directory[offset..offset + 128];
        if current[0] != 0 && current.split(|byte| *byte == 0).next() == Some(name.as_bytes()) {
            return Ok(Some(u32::from_le_bytes(directory[offset + 132..offset + 136].try_into().unwrap())));
        }
    }
    Ok(None)
}

pub(crate) struct DiskReader {
    start_lba: u32,
    size: u32,
}

impl DiskReader {
    pub fn open(path: &str) -> Result<Self, &'static str> {
        let name = path.strip_prefix("/disk/").ok_or("disk: path must begin /disk/")?;
        let directory = read_directory()?;
        for index in 0..DIRECTORY_ENTRIES {
            let entry = index * ENTRY_SIZE;
            let current = &directory[entry..entry + 128];
            if current[0] != 0 && current.split(|byte| *byte == 0).next() == Some(name.as_bytes()) {
                let start_lba = u32::from_le_bytes(directory[entry + 128..entry + 132].try_into().unwrap());
                let size = u32::from_le_bytes(directory[entry + 132..entry + 136].try_into().unwrap());
                return Ok(Self { start_lba, size });
            }
        }
        Err("disk: file not found")
    }

    pub fn size(&self) -> u32 { self.size }

    pub fn read_at(&self, offset: u32, output: &mut [u8]) -> Result<usize, &'static str> {
        if offset >= self.size { return Ok(0); }
        let count = core::cmp::min(output.len(), (self.size - offset) as usize);
        let mut copied = 0;
        let mut run = vec![0u8; 127 * SECTOR_SIZE];
        while copied < count {
            let absolute = offset as u64 + copied as u64;
            let lba = self.start_lba + (absolute / SECTOR_SIZE as u64) as u32;
            let in_sector = (absolute % SECTOR_SIZE as u64) as usize;
            let wanted = core::cmp::min(count - copied, 127 * SECTOR_SIZE - in_sector);
            let sectors = (in_sector + wanted + SECTOR_SIZE - 1) / SECTOR_SIZE;
            read_sectors(lba, &mut run[..sectors * SECTOR_SIZE])?;
            output[copied..copied + wanted].copy_from_slice(&run[in_sector..in_sector + wanted]);
            copied += wanted;
        }
        Ok(copied)
    }
}

pub(crate) fn read(path: &str, offset: u32, output: &mut [u8]) -> Result<usize, &'static str> {
    DiskReader::open(path)?.read_at(offset, output)
}

pub(crate) fn self_test() -> Result<(), &'static str> {
    const PATH: &str = "/disk/.fetch-selftest";
    let mut expected = [0u8; 1024];
    for (index, byte) in expected.iter_mut().enumerate() {
        *byte = (index as u8).wrapping_mul(37).wrapping_add(11);
    }
    let mut writer = DiskWriter::begin(PATH)?;
    writer.write(&expected)?;
    writer.finish()?;
    let mut actual = [0u8; 1024];
    let read = read(PATH, 0, &mut actual)?;
    if read != expected.len() || actual != expected {
        return Err("disk: self-test data mismatch");
    }
    Ok(())
}

/// Bytes still free after the last file (files are laid out one after another).
pub(crate) fn free_bytes() -> Result<u64, &'static str> {
    let directory = read_directory()?;
    let used = directory_high_water(&directory)?;
    Ok((DISK_SECTORS - used.min(DISK_SECTORS)) as u64 * SECTOR_SIZE as u64)
}

pub(crate) fn capacity_bytes() -> u64 {
    (DISK_SECTORS - DATA_START_LBA) as u64 * SECTOR_SIZE as u64
}

/// Forgets a file; if it was the last one its space is reusable at once.
pub(crate) fn remove(path: &str) -> Result<bool, &'static str> {
    let name = path.strip_prefix("/disk/").ok_or("disk: path must begin /disk/")?;
    let mut directory = read_directory()?;
    for index in 0..DIRECTORY_ENTRIES {
        let offset = index * ENTRY_SIZE;
        let current = &directory[offset..offset + 128];
        if current[0] != 0 && current.split(|byte| *byte == 0).next() == Some(name.as_bytes()) {
            directory[offset..offset + ENTRY_SIZE].fill(0);
            let next = directory_high_water(&directory)?;
            commit_directory(&directory, next)?;
            return Ok(true);
        }
    }
    Ok(false)
}

unsafe fn c_path<'a>(pointer: *const u8) -> Option<&'a str> {
    if pointer.is_null() { return None; }
    let mut len = 0;
    while len < 256 && *pointer.add(len) != 0 { len += 1; }
    if len == 256 { return None; }
    core::str::from_utf8(core::slice::from_raw_parts(pointer, len)).ok()
}

/// Size of a file in the fetch store, or -1 if it is not there (or bigger than 2 GiB).
#[no_mangle]
pub unsafe extern "C" fn rust_disk_store_size(path: *const u8) -> i32 {
    match c_path(path).map(file_size) {
        Some(Ok(Some(size))) if size <= i32::MAX as u32 => size as i32,
        _ => -1,
    }
}

/// Reads up to `len` bytes at `offset`; returns how many were read, or -1.
#[no_mangle]
pub unsafe extern "C" fn rust_disk_store_read(path: *const u8, offset: u32, buffer: *mut u8, len: u32) -> i32 {
    if buffer.is_null() { return -1; }
    let Some(path) = c_path(path) else { return -1 };
    match read(path, offset, core::slice::from_raw_parts_mut(buffer, len as usize)) {
        Ok(count) => count as i32,
        Err(_) => -1,
    }
}

/// 0 if the file was removed, -1 if there was none or the disk failed.
#[no_mangle]
pub unsafe extern "C" fn rust_disk_store_remove(path: *const u8) -> i32 {
    match c_path(path).map(remove) {
        Some(Ok(true)) => 0,
        _ => -1,
    }
}
