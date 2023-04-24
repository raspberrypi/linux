// SPDX-License-Identifier: GPL-2.0

//! Buffers used in IO.

use crate::error::Result;
use alloc::vec::Vec;
use core::mem::{size_of, MaybeUninit};

/// Represents a buffer to be read from during IO.
pub trait IoBufferReader {
    /// Returns the number of bytes left to be read from the io buffer.
    ///
    /// Note that even reading less than this number of bytes may fail.
    fn len(&self) -> usize;

    /// Returns `true` if no data is available in the io buffer.
    fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Reads raw data from the io buffer into a raw kernel buffer.
    ///
    /// # Safety
    ///
    /// The output buffer must be valid.
    unsafe fn read_raw(&mut self, out: *mut u8, len: usize) -> Result;

    /// Reads all data remaining in the io buffer.
    ///
    /// Returns `EFAULT` if the address does not currently point to mapped, readable memory.
    fn read_all(&mut self) -> Result<Vec<u8>> {
        let mut data = Vec::<u8>::new();
        data.try_resize(self.len(), 0)?;

        // SAFETY: The output buffer is valid as we just allocated it.
        unsafe { self.read_raw(data.as_mut_ptr(), data.len())? };
        Ok(data)
    }

    /// Reads a byte slice from the io buffer.
    ///
    /// Returns `EFAULT` if the byte slice is bigger than the remaining size of the user slice or
    /// if the address does not currently point to mapped, readable memory.
    fn read_slice(&mut self, data: &mut [u8]) -> Result {
        // SAFETY: The output buffer is valid as it's coming from a live reference.
        unsafe { self.read_raw(data.as_mut_ptr(), data.len()) }
    }

    /// Reads the contents of a plain old data (POD) type from the io buffer.
    fn read<T: ReadableFromBytes>(&mut self) -> Result<T> {
        let mut out = MaybeUninit::<T>::uninit();
        // SAFETY: The buffer is valid as it was just allocated.
        unsafe { self.read_raw(out.as_mut_ptr() as _, size_of::<T>()) }?;
        // SAFETY: We just initialised the data.
        Ok(unsafe { out.assume_init() })
    }
}

/// Represents a buffer to be written to during IO.
pub trait IoBufferWriter {
    /// Returns the number of bytes left to be written into the io buffer.
    ///
    /// Note that even writing less than this number of bytes may fail.
    fn len(&self) -> usize;

    /// Returns `true` if the io buffer cannot hold any additional data.
    fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Writes zeroes to the io buffer.
    ///
    /// Differently from the other write functions, `clear` will zero as much as it can and update
    /// the writer internal state to reflect this. It will, however, return an error if it cannot
    /// clear `len` bytes.
    ///
    /// For example, if a caller requests that 100 bytes be cleared but a segfault happens after
    /// 20 bytes, then EFAULT is returned and the writer is advanced by 20 bytes.
    fn clear(&mut self, len: usize) -> Result;

    /// Writes a byte slice into the io buffer.
    ///
    /// Returns `EFAULT` if the byte slice is bigger than the remaining size of the io buffer or if
    /// the address does not currently point to mapped, writable memory.
    fn write_slice(&mut self, data: &[u8]) -> Result {
        // SAFETY: The input buffer is valid as it's coming from a live reference.
        unsafe { self.write_raw(data.as_ptr(), data.len()) }
    }

    /// Writes raw data to the io buffer from a raw kernel buffer.
    ///
    /// # Safety
    ///
    /// The input buffer must be valid.
    unsafe fn write_raw(&mut self, data: *const u8, len: usize) -> Result;

    /// Writes the contents of the given data into the io buffer.
    fn write<T: WritableToBytes>(&mut self, data: &T) -> Result {
        // SAFETY: The input buffer is valid as it's coming from a live
        // reference to a type that implements `WritableToBytes`.
        unsafe { self.write_raw(data as *const T as _, size_of::<T>()) }
    }
}

/// Specifies that a type is safely readable from byte slices.
///
/// Not all types can be safely read from byte slices; examples from
/// <https://doc.rust-lang.org/reference/behavior-considered-undefined.html> include `bool`
/// that must be either `0` or `1`, and `char` that cannot be a surrogate or above `char::MAX`.
///
/// # Safety
///
/// Implementers must ensure that the type is made up only of types that can be safely read from
/// arbitrary byte sequences (e.g., `u32`, `u64`, etc.).
pub unsafe trait ReadableFromBytes {}

// SAFETY: All bit patterns are acceptable values of the types below.
unsafe impl ReadableFromBytes for u8 {}
unsafe impl ReadableFromBytes for u16 {}
unsafe impl ReadableFromBytes for u32 {}
unsafe impl ReadableFromBytes for u64 {}
unsafe impl ReadableFromBytes for usize {}
unsafe impl ReadableFromBytes for i8 {}
unsafe impl ReadableFromBytes for i16 {}
unsafe impl ReadableFromBytes for i32 {}
unsafe impl ReadableFromBytes for i64 {}
unsafe impl ReadableFromBytes for isize {}

/// Specifies that a type is safely writable to byte slices.
///
/// This means that we don't read undefined values (which leads to UB) in preparation for writing
/// to the byte slice. It also ensures that no potentially sensitive information is leaked into the
/// byte slices.
///
/// # Safety
///
/// A type must not include padding bytes and must be fully initialised to safely implement
/// [`WritableToBytes`] (i.e., it doesn't contain [`MaybeUninit`] fields). A composition of
/// writable types in a structure is not necessarily writable because it may result in padding
/// bytes.
pub unsafe trait WritableToBytes {}

// SAFETY: Initialised instances of the following types have no uninitialised portions.
unsafe impl WritableToBytes for u8 {}
unsafe impl WritableToBytes for u16 {}
unsafe impl WritableToBytes for u32 {}
unsafe impl WritableToBytes for u64 {}
unsafe impl WritableToBytes for usize {}
unsafe impl WritableToBytes for i8 {}
unsafe impl WritableToBytes for i16 {}
unsafe impl WritableToBytes for i32 {}
unsafe impl WritableToBytes for i64 {}
unsafe impl WritableToBytes for isize {}
