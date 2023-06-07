// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

use core::ops::{Deref, DerefMut};
use kernel::{
    bindings::{self, *},
    types::{AsBytes, FromBytes},
};

macro_rules! pub_no_prefix {
    ($prefix:ident, $($newname:ident),+) => {
        $(pub(crate) const $newname: u32 = kernel::macros::concat_idents!($prefix, $newname);)+
    };
}

pub_no_prefix!(
    binder_driver_return_protocol_,
    BR_DEAD_REPLY,
    BR_FAILED_REPLY,
    BR_NOOP,
    BR_SPAWN_LOOPER,
    BR_TRANSACTION_COMPLETE,
    BR_OK,
    BR_INCREFS,
    BR_ACQUIRE,
    BR_RELEASE,
    BR_DECREFS
);

pub_no_prefix!(
    binder_driver_command_protocol_,
    BC_ENTER_LOOPER,
    BC_EXIT_LOOPER,
    BC_REGISTER_LOOPER,
    BC_INCREFS,
    BC_ACQUIRE,
    BC_RELEASE,
    BC_DECREFS,
    BC_INCREFS_DONE,
    BC_ACQUIRE_DONE
);

macro_rules! decl_wrapper {
    ($newname:ident, $wrapped:ty) => {
        #[derive(Copy, Clone, Default)]
        #[repr(transparent)]
        pub(crate) struct $newname($wrapped);

        // SAFETY: This macro is only used with types where this is ok.
        unsafe impl FromBytes for $newname {}
        unsafe impl AsBytes for $newname {}

        impl Deref for $newname {
            type Target = $wrapped;
            fn deref(&self) -> &Self::Target {
                &self.0
            }
        }

        impl DerefMut for $newname {
            fn deref_mut(&mut self) -> &mut Self::Target {
                &mut self.0
            }
        }
    };
}

decl_wrapper!(BinderNodeDebugInfo, bindings::binder_node_debug_info);
decl_wrapper!(BinderNodeInfoForRef, bindings::binder_node_info_for_ref);
decl_wrapper!(FlatBinderObject, bindings::flat_binder_object);
decl_wrapper!(BinderWriteRead, bindings::binder_write_read);
decl_wrapper!(BinderVersion, bindings::binder_version);
decl_wrapper!(ExtendedError, bindings::binder_extended_error);

impl BinderVersion {
    pub(crate) fn current() -> Self {
        Self(bindings::binder_version {
            protocol_version: bindings::BINDER_CURRENT_PROTOCOL_VERSION as _,
        })
    }
}

impl ExtendedError {
    pub(crate) fn new(id: u32, command: u32, param: i32) -> Self {
        Self(bindings::binder_extended_error { id, command, param })
    }
}
