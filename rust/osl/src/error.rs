//! Defines the OS Layper general error type.
//!
//! Every OS should provide Error type and an error code conversion method
//! such as the following implementation on Linux.
//!
//! # Examples
//!
//! ```
//! use kernel::prelude::error{Error,code};
//!
//! impl From<crate::error::Errno> for Error {
//!     fn from(errno: crate::error::Errno) -> Self {
//!         match errno {
//!             crate::error::Errno::InvalidArgs => code::EINVAL,
//!             ...
//!         }
//!     }
//! }
//! ```
//!
//! Driver use osl error
//!
//! ```
//! use osl::error::{Errno, Result, to_error}
//!
//! fn func(t: i32) -> Result<()> {
//!     if t > 0 {
//!         Ok(())
//!     } else {
//!         to_error(Errno::InvaidArgs)
//!     }
//! }
//! ```

/// The general error type.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Errno {
    /// Invalid arguments.
    InvalidArgs,
    /// No such device.
    NoSuchDevice,
    /// Timeout
    TimeOut,
    /// Device is busy
    Busy,
    /// IO error
    Io,
    /// Try Again
    Again,
}

#[cfg(feature = "linux")]
pub use crate::linux::error::*;

/// A [`Result`] with an [`Error`] error type.
pub type Result<T> = core::result::Result<T, Error>;

/// Give an errno, return OS Error
pub fn to_error<T>(errno: Errno) -> Result<T> {
    Err::<T, Error>(Error::from(errno))
}
