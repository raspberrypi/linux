//! Defines the OS Layper time API.
//!
//! Every OS should provide time func
//!
//!
//! # Use example
//! ```
//!
//! fn func() {
//!     osl::time::current_time_us();
//! }

/// One usec to nsec
pub const NSEC_PER_USEC: u64 = 1000;

#[cfg(feature = "linux")]
pub use crate::linux::time::*;
