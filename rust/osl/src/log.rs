//! Defines the OS Layper log API.
//!
//! Every OS should provide log_print! macro
//!
//!# Use Example
//!
//! ```
//! #[macro_use]
//! extern crate osl;
//! const __LOG_PREFIX: &[u8] = b"sample\0";
//! fn func() {
//!     log_info!("Hello!")
//! }
//!

/// Log Level
pub enum LogLevel {
    /// The "error" level
    Error,
    /// The "warn" level.
    Warn,
    /// The "info" level.
    Info,
    /// The "debug" level.
    Debug,
}

#[cfg(feature = "linux")]
pub use crate::linux::log::*;

/// Prints an error-level message.
///
/// Use this level for informational messages.
///
/// # Examples
///
/// ```
/// log_err!("hello {}\n", "there");
/// ```
#[macro_export]
macro_rules! log_err (
    ($($arg:tt)*) => (
        $crate::log_print!($crate::log::LogLevel::Error, $($arg)*)
    )
);

/// Prints an warn-level message.
///
/// Use this level for informational messages.
///
/// # Examples
///
/// ```
/// log_warn!("hello {}\n", "there");
/// ```
#[macro_export]
macro_rules! log_warn (
    ($($arg:tt)*) => (
        $crate::log_print!($crate::log::LogLevel::Warn, $($arg)*)
    )
);

/// Prints an info-level message.
///
/// Use this level for informational messages.
///
/// # Examples
///
/// ```
/// log_info!("hello {}\n", "there");
/// ```
#[macro_export]
macro_rules! log_info (
    ($($arg:tt)*) => (
        $crate::log_print!($crate::log::LogLevel::Info, $($arg)*)
    )
);

/// Prints an info-level message.
///
/// Use this level for informational messages.
///
/// # Examples
///
/// ```
/// log_debug!("hello {}\n", "there");
/// ```
#[macro_export]
macro_rules! log_debug (
    ($($arg:tt)*) => (
        $crate::log_print!($crate::log::LogLevel::Debug, $($arg)*)
    )
);
