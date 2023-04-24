// SPDX-License-Identifier: GPL-2.0

//! IO vector iterators.
//!
//! C header: [`include/linux/uio.h`](../../../../include/linux/uio.h)

use crate::{
    bindings,
    error::code::*,
    error::Result,
    io_buffer::{IoBufferReader, IoBufferWriter},
};

/// Wraps the kernel's `struct iov_iter`.
///
/// # Invariants
///
/// The pointer `IovIter::ptr` is non-null and valid.
pub struct IovIter {
    ptr: *mut bindings::iov_iter,
}

impl IovIter {
    fn common_len(&self) -> usize {
        // SAFETY: `IovIter::ptr` is guaranteed to be valid by the type invariants.
        unsafe { (*self.ptr).__bindgen_anon_2.__bindgen_anon_1.count }
    }

    /// Constructs a new [`struct iov_iter`] wrapper.
    ///
    /// # Safety
    ///
    /// The pointer `ptr` must be non-null and valid for the lifetime of the object.
    pub(crate) unsafe fn from_ptr(ptr: *mut bindings::iov_iter) -> Self {
        // INVARIANTS: the safety contract ensures the type invariant will hold.
        Self { ptr }
    }
}

impl IoBufferWriter for IovIter {
    fn len(&self) -> usize {
        self.common_len()
    }

    fn clear(&mut self, mut len: usize) -> Result {
        while len > 0 {
            // SAFETY: `IovIter::ptr` is guaranteed to be valid by the type invariants.
            let written = unsafe { bindings::iov_iter_zero(len, self.ptr) };
            if written == 0 {
                return Err(EFAULT);
            }

            len -= written;
        }
        Ok(())
    }

    unsafe fn write_raw(&mut self, data: *const u8, len: usize) -> Result {
        let res = unsafe { bindings::_copy_to_iter(data as _, len, self.ptr) };
        if res != len {
            Err(EFAULT)
        } else {
            Ok(())
        }
    }
}

impl IoBufferReader for IovIter {
    fn len(&self) -> usize {
        self.common_len()
    }

    unsafe fn read_raw(&mut self, out: *mut u8, len: usize) -> Result {
        let res = unsafe { bindings::_copy_from_iter(out as _, len, self.ptr) };
        if res != len {
            Err(EFAULT)
        } else {
            Ok(())
        }
    }
}
