//! Defines the OS Layper sleep API.
//!
//! Every OS should provide usleep func
//!
//!
//! # Use example
//! ```
//! use osl::sleep;
//!
//! fn func() {
//!     sleep::usleep(100);
//! }

#[cfg(feature = "linux")]
pub use crate::linux::sleep::*;
