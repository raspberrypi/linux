// SPDX-License-Identifier: GPL-2.0

//! Seq file bindings.
//!
//! C header: [`include/linux/seq_file.h`](../../../../include/linux/seq_file.h)

use crate::{bindings, c_str, types::Opaque};

/// A helper for implementing special files, where the complete contents can be generated on each
/// access.
pub struct SeqFile(Opaque<bindings::seq_file>);

impl SeqFile {
    /// Creates a new [`SeqFile`] from a raw pointer.
    ///
    /// # Safety
    ///
    /// The caller must ensure that, for the duration of 'a, the pointer must point at a valid
    /// `seq_file` and that it will not be accessed via anything other than the returned reference.
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::seq_file) -> &'a mut SeqFile {
        // SAFETY: The safety requirements guarantee the validity of the dereference, while the
        // `Credential` type being transparent makes the cast ok.
        unsafe { &mut *ptr.cast() }
    }

    /// Used by the [`seq_print`] macro.
    ///
    /// [`seq_print`]: crate::seq_print
    pub fn call_printf(&mut self, args: core::fmt::Arguments<'_>) {
        // SAFETY: Passing a void pointer to `Arguments` is valid for `%pA`.
        unsafe {
            bindings::seq_printf(
                self.0.get(),
                c_str!("%pA").as_char_ptr(),
                &args as *const _ as *const core::ffi::c_void,
            );
        }
    }
}

/// Use for writing to a [`SeqFile`] with the ordinary Rust formatting syntax.
#[macro_export]
macro_rules! seq_print {
    ($m:expr, $($arg:tt)+) => (
        $m.call_printf(format_args!($($arg)+))
    );
}
