// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

use kernel::{
    list::{HasListLinks, List, ListArc, ListArcSafe, ListItem, ListLinks},
    prelude::*,
    str::{CStr, CString},
    sync::{Arc, Mutex},
};

use crate::process::Process;

// This module defines the global variable containing the list of contexts. Since the
// `kernel::sync` bindings currently don't support mutexes in globals, we use a temporary
// workaround.
//
// TODO: Once `kernel::sync` has support for mutexes in globals, remove this module.
mod context_global {
    use super::ContextList;
    use core::cell::UnsafeCell;
    use core::mem::MaybeUninit;
    use kernel::init::PinInit;
    use kernel::list::List;
    use kernel::sync::lock::mutex::{Mutex, MutexBackend};
    use kernel::sync::lock::Guard;

    /// A temporary wrapper used to define a mutex in a global.
    pub(crate) struct Contexts {
        inner: UnsafeCell<MaybeUninit<Mutex<ContextList>>>,
    }

    impl Contexts {
        /// # Safety
        ///
        /// The caller must call `init` before the first call to `lock`.
        pub(crate) const unsafe fn new() -> Self {
            Contexts {
                inner: UnsafeCell::new(MaybeUninit::uninit()),
            }
        }

        /// Called when the module is initialized.
        ///
        /// # Safety
        ///
        /// Must only be called once.
        ///
        /// The struct must not be moved after this call.
        pub(crate) unsafe fn init(&self) {
            let ptr = self.inner.get() as *mut Mutex<ContextList>;
            let init = kernel::new_mutex!(ContextList { list: List::new() }, "ContextList");

            // SAFETY: The caller guarantees that they only call this method once, and that all
            // calls to `lock` happen after this call. These are the only ways to access `inner`,
            // so there is no data race when we perform unsynchronized access to `inner` here.
            //
            // Additionally, the caller promised to not move this struct after this call to `init`,
            // so it's okay to use a pinned initializer here.
            match unsafe { init.__pinned_init(ptr) } {
                Ok(()) => {}
                Err(e) => match e {},
            }
        }

        pub(crate) fn lock(&self) -> Guard<'_, ContextList, MutexBackend> {
            let ptr = self.inner.get() as *const Mutex<ContextList>;

            // SAFETY: When constructing this type, the caller promised to call `init` before
            // calling `lock`, so the mutex has been intiailized at this point.
            unsafe { (*ptr).lock() }
        }
    }

    // SAFETY: This allows you to call `lock` from several threads in parallel, but that's okay as
    // the mutex will correctly synchronize this access.
    unsafe impl Sync for Contexts {}
}

// SAFETY: We call `init` as the very first thing in the initialization of this module, so there
// are no calls to `lock` before `init` is called.
pub(crate) static CONTEXTS: context_global::Contexts = unsafe { context_global::Contexts::new() };

pub(crate) struct ContextList {
    list: List<Context>,
}

/// This struct keeps track of the processes using this context, and which process is the context
/// manager.
struct Manager {
    all_procs: List<Process>,
}

/// There is one context per binder file (/dev/binder, /dev/hwbinder, etc)
#[pin_data]
pub(crate) struct Context {
    #[pin]
    manager: Mutex<Manager>,
    pub(crate) name: CString,
    #[pin]
    links: ListLinks,
}

kernel::list::impl_has_list_links! {
    impl HasListLinks<0> for Context { self.links }
}
kernel::list::impl_list_arc_safe! {
    impl ListArcSafe<0> for Context { untracked; }
}
kernel::list::impl_list_item! {
    impl ListItem<0> for Context {
        using ListLinks;
    }
}

impl Context {
    pub(crate) fn new(name: &CStr) -> Result<Arc<Self>> {
        let name = CString::try_from(name)?;
        let list_ctx = ListArc::pin_init(pin_init!(Context {
            name,
            links <- ListLinks::new(),
            manager <- kernel::new_mutex!(Manager {
                all_procs: List::new(),
            }, "Context::manager"),
        }))?;

        let ctx = list_ctx.clone_arc();
        CONTEXTS.lock().list.push_back(list_ctx);

        Ok(ctx)
    }

    /// Called when the file for this context is unlinked.
    ///
    /// No-op if called twice.
    pub(crate) fn deregister(&self) {
        // SAFETY: We never add the context to any other linked list than this one, so it is either
        // in this list, or not in any list.
        unsafe {
            CONTEXTS.lock().list.remove(self);
        }
    }

    pub(crate) fn register_process(self: &Arc<Self>, proc: ListArc<Process>) {
        if !Arc::ptr_eq(self, &proc.ctx) {
            pr_err!("Context::register_process called on the wrong context.");
            return;
        }
        self.manager.lock().all_procs.push_back(proc);
    }

    pub(crate) fn deregister_process(self: &Arc<Self>, proc: &Process) {
        if !Arc::ptr_eq(self, &proc.ctx) {
            pr_err!("Context::deregister_process called on the wrong context.");
            return;
        }
        // SAFETY: We just checked that this is the right list.
        unsafe {
            self.manager.lock().all_procs.remove(proc);
        }
    }
}
