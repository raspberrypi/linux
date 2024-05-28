use kernel::prelude::*;

pub use kernel::prelude::Error;

impl From<crate::error::Errno> for Error {
    fn from(errno: crate::error::Errno) -> Self {
        match errno {
            crate::error::Errno::InvalidArgs => EINVAL,
            crate::error::Errno::NoSuchDevice => ENODEV,
            crate::error::Errno::TimeOut => ETIMEDOUT,
            crate::error::Errno::Busy => EBUSY,
            crate::error::Errno::Io => EIO,
            crate::error::Errno::Again => EAGAIN,
        }
    }
}
