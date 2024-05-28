//! This is an OS interface abstraction layer developed for cross-kernel drivers.
//!
//! Include:
//! - error: error type used by drivers
//! - log: log interface used by drivers

#![no_std]

#[cfg(feature = "linux")]
mod linux;
#[cfg(feature = "arceos")]
mod arceos;

#[macro_use]
extern crate derive_builder;

/// Prefix to appear before log messages printed from within the `osl` crate.
const __LOG_PREFIX: &[u8] = b"osl\0";

pub mod error;
pub mod log;
pub mod math;
pub mod sleep;
pub mod sync;
pub mod time;
pub mod vec;

pub mod driver;
