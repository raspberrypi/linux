// SPDX-License-Identifier: GPL-2.0

//! Synchronisation primitives.
//!
//! This module contains the kernel APIs related to synchronisation that have been ported or
//! wrapped for usage by Rust code in the kernel.

mod arc;
mod condvar;
pub mod lock;
mod locked_by;

#[cfg(CONFIG_LOCKDEP)]
mod lockdep;
#[cfg(not(CONFIG_LOCKDEP))]
mod no_lockdep;
#[cfg(not(CONFIG_LOCKDEP))]
use no_lockdep as lockdep;

pub use arc::{Arc, ArcBorrow, UniqueArc};
pub use condvar::CondVar;
pub use lock::{mutex::Mutex, spinlock::SpinLock};
pub use lockdep::LockClassKey;
pub use locked_by::LockedBy;

/// Defines a new static lock class and returns a pointer to it.
#[doc(hidden)]
#[macro_export]
macro_rules! static_lock_class {
    () => {{
        static CLASS: $crate::sync::LockClassKey = $crate::sync::LockClassKey::new();
        &CLASS
    }};
}

/// Returns the given string, if one is provided, otherwise generates one based on the source code
/// location.
#[doc(hidden)]
#[macro_export]
macro_rules! optional_name {
    () => {
        $crate::c_str!(::core::concat!(::core::file!(), ":", ::core::line!()))
    };
    ($name:literal) => {
        $crate::c_str!($name)
    };
}
