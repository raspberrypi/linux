// SPDX-License-Identifier: GPL-2.0

//! Dummy lockdep utilities.
//!
//! Takes the place of the `lockdep` module when lockdep is disabled.

/// A dummy, zero-sized lock class.
pub struct LockClassKey();

impl LockClassKey {
    /// Creates a new dummy lock class key.
    pub const fn new() -> Self {
        Self()
    }

    pub(crate) fn as_ptr(&self) -> *mut bindings::lock_class_key {
        core::ptr::null_mut()
    }
}
