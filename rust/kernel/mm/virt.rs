// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Virtual memory.

use crate::{
    bindings,
    error::{to_result, Result},
    page::Page,
    types::Opaque,
};

/// A wrapper for the kernel's `struct vm_area_struct`.
///
/// It represents an area of virtual memory.
#[repr(transparent)]
pub struct Area {
    vma: Opaque<bindings::vm_area_struct>,
}

impl Area {
    /// Access a virtual memory area given a raw pointer.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `vma` is non-null and valid for the duration of the new area's
    /// lifetime, with shared access. The caller must ensure that using the pointer for immutable
    /// operations is okay.
    pub unsafe fn from_ptr<'a>(vma: *const bindings::vm_area_struct) -> &'a Self {
        // SAFETY: The caller ensures that the pointer is valid.
        unsafe { &*vma.cast() }
    }

    /// Access a virtual memory area given a raw pointer.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `vma` is non-null and valid for the duration of the new area's
    /// lifetime, with exclusive access. The caller must ensure that using the pointer for
    /// immutable and mutable operations is okay.
    pub unsafe fn from_ptr_mut<'a>(vma: *mut bindings::vm_area_struct) -> &'a mut Self {
        // SAFETY: The caller ensures that the pointer is valid.
        unsafe { &mut *vma.cast() }
    }

    /// Returns a raw pointer to this area.
    pub fn as_ptr(&self) -> *const bindings::vm_area_struct {
        self as *const Area as *const bindings::vm_area_struct
    }

    /// Returns the flags associated with the virtual memory area.
    ///
    /// The possible flags are a combination of the constants in [`flags`].
    pub fn flags(&self) -> usize {
        // SAFETY: `self.vma` is valid by the type invariants.
        unsafe { (*self.vma.get()).__bindgen_anon_2.vm_flags as _ }
    }

    /// Sets the flags associated with the virtual memory area.
    ///
    /// The possible flags are a combination of the constants in [`flags`].
    pub fn set_flags(&mut self, flags: usize) {
        // SAFETY: `self.vma` is valid by the type invariants.
        unsafe { (*self.vma.get()).__bindgen_anon_2.vm_flags = flags as _ };
    }

    /// Returns the start address of the virtual memory area.
    pub fn start(&self) -> usize {
        // SAFETY: `self.vma` is valid by the type invariants.
        unsafe { (*self.vma.get()).__bindgen_anon_1.__bindgen_anon_1.vm_start as _ }
    }

    /// Returns the end address of the virtual memory area.
    pub fn end(&self) -> usize {
        // SAFETY: `self.vma` is valid by the type invariants.
        unsafe { (*self.vma.get()).__bindgen_anon_1.__bindgen_anon_1.vm_end as _ }
    }

    /// Maps a single page at the given address within the virtual memory area.
    pub fn vm_insert_page(&mut self, address: usize, page: &Page) -> Result {
        // SAFETY: The page is guaranteed to be order 0. The range of `address` is already checked
        // by `vm_insert_page`. `self.vma` and `page.as_ptr()` are guaranteed by their respective
        // type invariants to be valid.
        to_result(unsafe { bindings::vm_insert_page(self.vma.get(), address as _, page.as_ptr()) })
    }

    /// Unmap pages in the given page range.
    pub fn zap_page_range_single(&self, address: usize, size: usize) {
        // SAFETY: The `vma` pointer is valid.
        unsafe {
            bindings::zap_page_range_single(
                self.vma.get(),
                address as _,
                size as _,
                core::ptr::null_mut(),
            )
        };
    }
}

/// Container for [`Area`] flags.
pub mod flags {
    use crate::bindings;

    /// No flags are set.
    pub const NONE: usize = bindings::VM_NONE as _;

    /// Mapping allows reads.
    pub const READ: usize = bindings::VM_READ as _;

    /// Mapping allows writes.
    pub const WRITE: usize = bindings::VM_WRITE as _;

    /// Mapping allows execution.
    pub const EXEC: usize = bindings::VM_EXEC as _;

    /// Mapping is shared.
    pub const SHARED: usize = bindings::VM_SHARED as _;

    /// Mapping may be updated to allow reads.
    pub const MAYREAD: usize = bindings::VM_MAYREAD as _;

    /// Mapping may be updated to allow writes.
    pub const MAYWRITE: usize = bindings::VM_MAYWRITE as _;

    /// Mapping may be updated to allow execution.
    pub const MAYEXEC: usize = bindings::VM_MAYEXEC as _;

    /// Mapping may be updated to be shared.
    pub const MAYSHARE: usize = bindings::VM_MAYSHARE as _;

    /// Do not copy this vma on fork.
    pub const DONTCOPY: usize = bindings::VM_DONTCOPY as _;

    /// Cannot expand with mremap().
    pub const DONTEXPAND: usize = bindings::VM_DONTEXPAND as _;

    /// Lock the pages covered when they are faulted in.
    pub const LOCKONFAULT: usize = bindings::VM_LOCKONFAULT as _;

    /// Is a VM accounted object.
    pub const ACCOUNT: usize = bindings::VM_ACCOUNT as _;

    /// should the VM suppress accounting.
    pub const NORESERVE: usize = bindings::VM_NORESERVE as _;

    /// Huge TLB Page VM.
    pub const HUGETLB: usize = bindings::VM_HUGETLB as _;

    /// Synchronous page faults.
    pub const SYNC: usize = bindings::VM_SYNC as _;

    /// Architecture-specific flag.
    pub const ARCH_1: usize = bindings::VM_ARCH_1 as _;

    /// Wipe VMA contents in child..
    pub const WIPEONFORK: usize = bindings::VM_WIPEONFORK as _;

    /// Do not include in the core dump.
    pub const DONTDUMP: usize = bindings::VM_DONTDUMP as _;

    /// Not soft dirty clean area.
    pub const SOFTDIRTY: usize = bindings::VM_SOFTDIRTY as _;

    /// Can contain "struct page" and pure PFN pages.
    pub const MIXEDMAP: usize = bindings::VM_MIXEDMAP as _;

    /// MADV_HUGEPAGE marked this vma.
    pub const HUGEPAGE: usize = bindings::VM_HUGEPAGE as _;

    /// MADV_NOHUGEPAGE marked this vma.
    pub const NOHUGEPAGE: usize = bindings::VM_NOHUGEPAGE as _;

    /// KSM may merge identical pages.
    pub const MERGEABLE: usize = bindings::VM_MERGEABLE as _;
}
