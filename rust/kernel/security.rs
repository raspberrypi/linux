// SPDX-License-Identifier: GPL-2.0

//! Linux Security Modules (LSM).
//!
//! C header: [`include/linux/security.h`](srctree/include/linux/security.h).

use crate::{
    bindings,
    cred::Credential,
    error::{to_result, Result},
    file::File,
};

/// Calls the security modules to determine if the given task can become the manager of a binder
/// context.
pub fn binder_set_context_mgr(mgr: &Credential) -> Result {
    // SAFETY: `mrg.0` is valid because the shared reference guarantees a nonzero refcount.
    to_result(unsafe { bindings::security_binder_set_context_mgr(mgr.as_ptr()) })
}

/// Calls the security modules to determine if binder transactions are allowed from task `from` to
/// task `to`.
pub fn binder_transaction(from: &Credential, to: &Credential) -> Result {
    // SAFETY: `from` and `to` are valid because the shared references guarantee nonzero refcounts.
    to_result(unsafe { bindings::security_binder_transaction(from.as_ptr(), to.as_ptr()) })
}

/// Calls the security modules to determine if task `from` is allowed to send binder objects
/// (owned by itself or other processes) to task `to` through a binder transaction.
pub fn binder_transfer_binder(from: &Credential, to: &Credential) -> Result {
    // SAFETY: `from` and `to` are valid because the shared references guarantee nonzero refcounts.
    to_result(unsafe { bindings::security_binder_transfer_binder(from.as_ptr(), to.as_ptr()) })
}

/// Calls the security modules to determine if task `from` is allowed to send the given file to
/// task `to` (which would get its own file descriptor) through a binder transaction.
pub fn binder_transfer_file(from: &Credential, to: &Credential, file: &File) -> Result {
    // SAFETY: `from`, `to` and `file` are valid because the shared references guarantee nonzero
    // refcounts.
    to_result(unsafe {
        bindings::security_binder_transfer_file(from.as_ptr(), to.as_ptr(), file.as_ptr())
    })
}

/// A security context string.
///
/// # Invariants
///
/// The `secdata` and `seclen` fields correspond to a valid security context as returned by a
/// successful call to `security_secid_to_secctx`, that has not yet been destroyed by calling
/// `security_release_secctx`.
pub struct SecurityCtx {
    secdata: *mut core::ffi::c_char,
    seclen: usize,
}

impl SecurityCtx {
    /// Get the security context given its id.
    pub fn from_secid(secid: u32) -> Result<Self> {
        let mut secdata = core::ptr::null_mut();
        let mut seclen = 0u32;
        // SAFETY: Just a C FFI call. The pointers are valid for writes.
        to_result(unsafe { bindings::security_secid_to_secctx(secid, &mut secdata, &mut seclen) })?;

        // INVARIANT: If the above call did not fail, then we have a valid security context.
        Ok(Self {
            secdata,
            seclen: seclen as usize,
        })
    }

    /// Returns whether the security context is empty.
    pub fn is_empty(&self) -> bool {
        self.seclen == 0
    }

    /// Returns the length of this security context.
    pub fn len(&self) -> usize {
        self.seclen
    }

    /// Returns the bytes for this security context.
    pub fn as_bytes(&self) -> &[u8] {
        let ptr = self.secdata;
        if ptr.is_null() {
            debug_assert_eq!(self.seclen, 0);
            // We can't pass a null pointer to `slice::from_raw_parts` even if the length is zero.
            return &[];
        }

        // SAFETY: The call to `security_secid_to_secctx` guarantees that the pointer is valid for
        // `seclen` bytes. Furthermore, if the length is zero, then we have ensured that the
        // pointer is not null.
        unsafe { core::slice::from_raw_parts(ptr.cast(), self.seclen) }
    }
}

impl Drop for SecurityCtx {
    fn drop(&mut self) {
        // SAFETY: By the invariant of `Self`, this frees a pointer that came from a successful
        // call to `security_secid_to_secctx` and has not yet been destroyed by
        // `security_release_secctx`.
        unsafe { bindings::security_release_secctx(self.secdata, self.seclen as u32) };
    }
}
