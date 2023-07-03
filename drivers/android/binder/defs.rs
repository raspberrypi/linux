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
    BR_TRANSACTION,
    BR_TRANSACTION_SEC_CTX,
    BR_REPLY,
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
    BC_TRANSACTION,
    BC_TRANSACTION_SG,
    BC_FREE_BUFFER,
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

pub_no_prefix!(flat_binder_object_flags_, FLAT_BINDER_FLAG_TXN_SECURITY_CTX);

pub_no_prefix!(transaction_flags_, TF_ONE_WAY, TF_CLEAR_BUF);

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
decl_wrapper!(BinderTransactionData, bindings::binder_transaction_data);
decl_wrapper!(
    BinderTransactionDataSecctx,
    bindings::binder_transaction_data_secctx
);
decl_wrapper!(
    BinderTransactionDataSg,
    bindings::binder_transaction_data_sg
);
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

impl BinderTransactionData {
    pub(crate) fn with_buffers_size(self, buffers_size: u64) -> BinderTransactionDataSg {
        BinderTransactionDataSg(bindings::binder_transaction_data_sg {
            transaction_data: self.0,
            buffers_size,
        })
    }
}

impl BinderTransactionDataSecctx {
    /// View the inner data as wrapped in `BinderTransactionData`.
    pub(crate) fn tr_data(&mut self) -> &mut BinderTransactionData {
        // SAFETY: Transparent wrapper is safe to transmute.
        unsafe {
            &mut *(&mut self.transaction_data as *mut bindings::binder_transaction_data
                as *mut BinderTransactionData)
        }
    }
}

impl ExtendedError {
    pub(crate) fn new(id: u32, command: u32, param: i32) -> Self {
        Self(bindings::binder_extended_error { id, command, param })
    }
}
