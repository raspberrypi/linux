// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

use kernel::prelude::*;

use crate::defs::*;

pub(crate) type BinderResult<T = ()> = core::result::Result<T, BinderError>;

/// An error that will be returned to userspace via the `BINDER_WRITE_READ` ioctl rather than via
/// errno.
pub(crate) struct BinderError {
    pub(crate) reply: u32,
    source: Option<Error>,
}

impl BinderError {
    pub(crate) fn new_dead() -> Self {
        Self {
            reply: BR_DEAD_REPLY,
            source: None,
        }
    }

    pub(crate) fn new_frozen() -> Self {
        Self {
            reply: BR_FROZEN_REPLY,
            source: None,
        }
    }

    pub(crate) fn new_frozen_oneway() -> Self {
        Self {
            reply: BR_TRANSACTION_PENDING_FROZEN,
            source: None,
        }
    }

    pub(crate) fn should_pr_warn(&self) -> bool {
        self.source.is_some()
    }

    pub(crate) fn is_dead(&self) -> bool {
        self.reply == BR_DEAD_REPLY
    }

    pub(crate) fn as_errno(&self) -> core::ffi::c_int {
        self.source.unwrap_or(EINVAL).to_errno()
    }
}

/// Convert an errno into a `BinderError` and store the errno used to construct it. The errno
/// should be stored as the thread's extended error when given to userspace.
impl From<Error> for BinderError {
    fn from(source: Error) -> Self {
        Self {
            reply: BR_FAILED_REPLY,
            source: Some(source),
        }
    }
}

impl From<kernel::file::BadFdError> for BinderError {
    fn from(source: kernel::file::BadFdError) -> Self {
        BinderError::from(Error::from(source))
    }
}

impl From<core::alloc::AllocError> for BinderError {
    fn from(_: core::alloc::AllocError) -> Self {
        Self {
            reply: BR_FAILED_REPLY,
            source: Some(ENOMEM),
        }
    }
}

impl From<alloc::collections::TryReserveError> for BinderError {
    fn from(_: alloc::collections::TryReserveError) -> Self {
        Self {
            reply: BR_FAILED_REPLY,
            source: Some(ENOMEM),
        }
    }
}

impl core::fmt::Debug for BinderError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self.reply {
            BR_FAILED_REPLY => match self.source.as_ref() {
                Some(source) => f
                    .debug_struct("BR_FAILED_REPLY")
                    .field("source", source)
                    .finish(),
                None => f.pad("BR_FAILED_REPLY"),
            },
            BR_DEAD_REPLY => f.pad("BR_DEAD_REPLY"),
            BR_FROZEN_REPLY => f.pad("BR_FROZEN_REPLY"),
            BR_TRANSACTION_PENDING_FROZEN => f.pad("BR_TRANSACTION_PENDING_FROZEN"),
            BR_TRANSACTION_COMPLETE => f.pad("BR_TRANSACTION_COMPLETE"),
            _ => f
                .debug_struct("BinderError")
                .field("reply", &self.reply)
                .finish(),
        }
    }
}
