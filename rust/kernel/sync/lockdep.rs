// SPDX-License-Identifier: GPL-2.0

//! Lockdep utilities.
//!
//! This module abstracts the parts of the kernel lockdep API relevant to Rust
//! modules, including lock classes.

use crate::types::Opaque;

/// Represents a lockdep class. It's a wrapper around C's `lock_class_key`.
#[repr(transparent)]
pub struct LockClassKey(Opaque<bindings::lock_class_key>);

impl LockClassKey {
    /// Creates a new lock class key.
    pub const fn new() -> Self {
        Self(Opaque::uninit())
    }

    pub(crate) fn as_ptr(&self) -> *mut bindings::lock_class_key {
        self.0.get()
    }
}

// SAFETY: `bindings::lock_class_key` is designed to be used concurrently from multiple threads and
// provides its own synchronization.
unsafe impl Sync for LockClassKey {}
