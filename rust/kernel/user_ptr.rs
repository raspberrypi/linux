// SPDX-License-Identifier: GPL-2.0

//! User pointers.
//!
//! C header: [`include/linux/uaccess.h`](../../../../include/linux/uaccess.h)

use crate::{
    bindings,
    error::code::*,
    error::Result,
    io_buffer::{IoBufferReader, IoBufferWriter},
};
use alloc::vec::Vec;

/// A reference to an area in userspace memory, which can be either
/// read-only or read-write.
///
/// All methods on this struct are safe: invalid pointers return
/// `EFAULT`. Concurrent access, *including data races to/from userspace
/// memory*, is permitted, because fundamentally another userspace
/// thread/process could always be modifying memory at the same time
/// (in the same way that userspace Rust's [`std::io`] permits data races
/// with the contents of files on disk). In the presence of a race, the
/// exact byte values read/written are unspecified but the operation is
/// well-defined. Kernelspace code should validate its copy of data
/// after completing a read, and not expect that multiple reads of the
/// same address will return the same value.
///
/// All APIs enforce the invariant that a given byte of memory from userspace
/// may only be read once. By preventing double-fetches we avoid TOCTOU
/// vulnerabilities. This is accomplished by taking `self` by value to prevent
/// obtaining multiple readers on a given [`UserSlicePtr`], and the readers
/// only permitting forward reads.
///
/// Constructing a [`UserSlicePtr`] performs no checks on the provided
/// address and length, it can safely be constructed inside a kernel thread
/// with no current userspace process. Reads and writes wrap the kernel APIs
/// `copy_from_user` and `copy_to_user`, which check the memory map of the
/// current process and enforce that the address range is within the user
/// range (no additional calls to `access_ok` are needed).
///
/// [`std::io`]: https://doc.rust-lang.org/std/io/index.html
pub struct UserSlicePtr(*mut core::ffi::c_void, usize);

impl UserSlicePtr {
    /// Constructs a user slice from a raw pointer and a length in bytes.
    ///
    /// # Safety
    ///
    /// Callers must be careful to avoid time-of-check-time-of-use
    /// (TOCTOU) issues. The simplest way is to create a single instance of
    /// [`UserSlicePtr`] per user memory block as it reads each byte at
    /// most once.
    pub unsafe fn new(ptr: *mut core::ffi::c_void, length: usize) -> Self {
        UserSlicePtr(ptr, length)
    }

    /// Reads the entirety of the user slice.
    ///
    /// Returns `EFAULT` if the address does not currently point to
    /// mapped, readable memory.
    pub fn read_all(self) -> Result<Vec<u8>> {
        self.reader().read_all()
    }

    /// Constructs a [`UserSlicePtrReader`].
    pub fn reader(self) -> UserSlicePtrReader {
        UserSlicePtrReader(self.0, self.1)
    }

    /// Writes the provided slice into the user slice.
    ///
    /// Returns `EFAULT` if the address does not currently point to
    /// mapped, writable memory (in which case some data from before the
    /// fault may be written), or `data` is larger than the user slice
    /// (in which case no data is written).
    pub fn write_all(self, data: &[u8]) -> Result {
        self.writer().write_slice(data)
    }

    /// Constructs a [`UserSlicePtrWriter`].
    pub fn writer(self) -> UserSlicePtrWriter {
        UserSlicePtrWriter(self.0, self.1)
    }

    /// Constructs both a [`UserSlicePtrReader`] and a [`UserSlicePtrWriter`].
    pub fn reader_writer(self) -> (UserSlicePtrReader, UserSlicePtrWriter) {
        (
            UserSlicePtrReader(self.0, self.1),
            UserSlicePtrWriter(self.0, self.1),
        )
    }
}

/// A reader for [`UserSlicePtr`].
///
/// Used to incrementally read from the user slice.
pub struct UserSlicePtrReader(*mut core::ffi::c_void, usize);

impl IoBufferReader for UserSlicePtrReader {
    /// Returns the number of bytes left to be read from this.
    ///
    /// Note that even reading less than this number of bytes may fail.
    fn len(&self) -> usize {
        self.1
    }

    /// Reads raw data from the user slice into a raw kernel buffer.
    ///
    /// # Safety
    ///
    /// The output buffer must be valid.
    unsafe fn read_raw(&mut self, out: *mut u8, len: usize) -> Result {
        if len > self.1 || len > u32::MAX as usize {
            return Err(EFAULT);
        }
        let res = unsafe { bindings::copy_from_user(out as _, self.0, len as _) };
        if res != 0 {
            return Err(EFAULT);
        }
        // Since this is not a pointer to a valid object in our program,
        // we cannot use `add`, which has C-style rules for defined
        // behavior.
        self.0 = self.0.wrapping_add(len);
        self.1 -= len;
        Ok(())
    }
}

/// A writer for [`UserSlicePtr`].
///
/// Used to incrementally write into the user slice.
pub struct UserSlicePtrWriter(*mut core::ffi::c_void, usize);

impl IoBufferWriter for UserSlicePtrWriter {
    fn len(&self) -> usize {
        self.1
    }

    fn clear(&mut self, mut len: usize) -> Result {
        let mut ret = Ok(());
        if len > self.1 {
            ret = Err(EFAULT);
            len = self.1;
        }

        // SAFETY: The buffer will be validated by `clear_user`. We ensure that `len` is within
        // bounds in the check above.
        let left = unsafe { bindings::clear_user(self.0, len as _) } as usize;
        if left != 0 {
            ret = Err(EFAULT);
            len -= left;
        }

        self.0 = self.0.wrapping_add(len);
        self.1 -= len;
        ret
    }

    unsafe fn write_raw(&mut self, data: *const u8, len: usize) -> Result {
        if len > self.1 || len > u32::MAX as usize {
            return Err(EFAULT);
        }
        let res = unsafe { bindings::copy_to_user(self.0, data as _, len as _) };
        if res != 0 {
            return Err(EFAULT);
        }
        // Since this is not a pointer to a valid object in our program,
        // we cannot use `add`, which has C-style rules for defined
        // behavior.
        self.0 = self.0.wrapping_add(len);
        self.1 -= len;
        Ok(())
    }
}
