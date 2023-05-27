// SPDX-License-Identifier: GPL-2.0

//! Lockdep utilities.
//!
//! This module abstracts the parts of the kernel lockdep API relevant to Rust
//! modules, including lock classes.

use crate::types::Opaque;

/// Represents a lockdep class. It's a wrapper around C's `lock_class_key`.
#[repr(transparent)]
pub struct StaticLockClassKey(Opaque<bindings::lock_class_key>);

impl StaticLockClassKey {
    /// Creates a new lock class key.
    pub const fn new() -> Self {
        Self(Opaque::uninit())
    }

    /// Returns the lock class key reference for this static lock class.
    pub const fn key(&self) -> LockClassKey {
        LockClassKey(self.0.get())
    }
}

// SAFETY: `bindings::lock_class_key` just represents an opaque memory location, and is never
// actually dereferenced.
unsafe impl Sync for StaticLockClassKey {}

/// A reference to a lock class key. This is a raw pointer to a lock_class_key,
/// which is required to have a static lifetime.
#[derive(Copy, Clone)]
pub struct LockClassKey(*mut bindings::lock_class_key);

impl LockClassKey {
    pub(crate) fn as_ptr(&self) -> *mut bindings::lock_class_key {
        self.0
    }
}

// SAFETY: `bindings::lock_class_key` just represents an opaque memory location, and is never
// actually dereferenced.
unsafe impl Send for LockClassKey {}
unsafe impl Sync for LockClassKey {}
