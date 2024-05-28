//! Defines the OS Layper Vec.
//!
//! Every OS should provide Vec TYPE
//!
//!

#[cfg(feature = "linux")]
pub use kernel::prelude::Vec;

#[cfg(feature = "arceos")]
pub use alloc::vec::Vec;
