// SPDX-License-Identifier: GPL-2.0

//! I2c kernel interface
//!
//! C header: [`include/linux/completion.h`]
//!

use crate::{
    types::Opaque,
    str::CStr,
    sync::LockClassKey,
    prelude::PinInit,
    bindings,
};

/// Linux completion wrapper
///
/// Wraps the kernel's C `struct completion`.
///
#[repr(transparent)]
pub struct Completion(Opaque<bindings::completion>);

// SAFETY: `Device` only holds a pointer to a C device, which is safe to be used from any thread.
unsafe impl Send for Completion {}

// SAFETY: `Device` only holds a pointer to a C device, references to which are safe to be used
// from any thread.
unsafe impl Sync for Completion {}

/// Creates a [`completion`] initialiser with the given name and a newly-created lock class.
///
/// It uses the name if one is given, otherwise it generates one based on the file name and line
/// number.
#[macro_export]
macro_rules! new_completion {
    ($($name:literal)?) => {
        $crate::completion::Completion::new($crate::optional_name!($($name)?), $crate::static_lock_class!())
    };
}

impl Completion {
    /// Creates a new instance of [`Completion`].
    #[inline]
    #[allow(clippy::new_ret_no_self)]
    pub fn new(name: &'static CStr, key: LockClassKey) -> impl PinInit<Self>
    {
        unsafe {
            kernel::init::pin_init_from_closure(move |slot| {
                let slot = Self::raw_get(slot);
                (*slot).done = 0;
                bindings::__init_swait_queue_head(
                    &mut ((*slot).wait),
                    name.as_char_ptr(),
                    key.as_ptr(),
                );
                Ok(())
            })
        }
    }

    /// Get a pointer to the inner `completion`.
    ///
    /// # Safety
    ///
    /// The provided pointer must not be dangling and must be properly aligned. (But the memory
    /// need not be initialized.)
    #[inline]
    pub unsafe fn raw_get(ptr: *const Self) -> *mut bindings::completion {
        // SAFETY: The caller promises that the pointer is aligned and not dangling.
        //
        // A pointer cast would also be ok due to `#[repr(transparent)]`. We use `addr_of!` so that
        // the compiler does not complain that the `work` field is unused.
        unsafe { Opaque::raw_get(core::ptr::addr_of!((*ptr).0)) }
    }

    /// completion reinit 
    pub fn reinit(&self) {
        unsafe {(*(self.0.get())).done = 0;}
    }

    /// This waits to be signaled for completion of a specific task. It is NOT
    /// interruptible and there is no timeout.
    pub fn wait_for_completion(&self) {
        // SAFETY: call ffi and ptr is valid
        unsafe{
            bindings::wait_for_completion(self.0.get())
        }
    }

    /// complete 
    pub fn complete(&self) {
        // SAFETY: call ffi and ptr is valid
        unsafe {
            bindings::complete(self.0.get())
        }
    }

    /// wait_for_completion_timeout: - waits for completion of a task (w/timeout)
    /// Return: 0 if timed out, and positive (at least 1, or number of sec)
    pub fn wait_for_completion_timeout_sec(&self, timeout: usize) -> usize {
        let jiff = timeout * (bindings::HZ as usize);
        let left_jiff = self.wait_for_completion_timeout(jiff);
        if left_jiff == 0 {
            return 0;
        }

        if left_jiff/(bindings::HZ as usize) == 0 {
            return 1;
        } else {
            return left_jiff/(bindings::HZ as usize);
        }
    }

    /// wait_for_completion_timeout: - waits for completion of a task (w/timeout)
    /// Return: 0 if timed out, and positive (at least 1, or number of jiffies left
    /// till timeout) if completed.
    fn wait_for_completion_timeout(&self, jiff: usize) -> usize {
        // SAFETY: call ffi and ptr is valid
        unsafe {
            bindings::wait_for_completion_timeout(self.0.get(), jiff.try_into().unwrap()).try_into().unwrap()
        }
    }
}
