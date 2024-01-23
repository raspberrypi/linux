// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Memory management.
//!
//! C header: [`include/linux/mm.h`](../../../../include/linux/mm.h)

use crate::bindings;

use core::{marker::PhantomData, mem::ManuallyDrop, ptr::NonNull};

pub mod virt;

/// A smart pointer that references a `struct mm` and owns an `mmgrab` refcount.
///
/// # Invariants
///
/// An `MmGrab` owns an `mmgrab` refcount to the inner `struct mm_struct`.
pub struct MmGrab {
    mm: NonNull<bindings::mm_struct>,
}

impl MmGrab {
    /// Call `mmgrab` on `current.mm`.
    pub fn mmgrab_current() -> Option<Self> {
        // SAFETY: It's safe to get the `mm` field from current.
        let mm = unsafe {
            let current = bindings::get_current();
            (*current).mm
        };

        let mm = NonNull::new(mm)?;

        // SAFETY: We just checked that `mm` is not null.
        unsafe { bindings::mmgrab(mm.as_ptr()) };

        // INVARIANT: We just created an `mmgrab` refcount.
        Some(Self { mm })
    }

    /// Check whether this vma is associated with this mm.
    pub fn is_same_mm(&self, area: &virt::Area) -> bool {
        // SAFETY: The `vm_mm` field of the area is immutable, so we can read it without
        // synchronization.
        let vm_mm = unsafe { (*area.as_ptr()).vm_mm };

        vm_mm == self.mm.as_ptr()
    }

    /// Calls `mmget_not_zero` and returns a handle if it succeeds.
    pub fn mmget_not_zero(&self) -> Option<MmGet> {
        // SAFETY: We know that `mm` is still valid since we hold an `mmgrab` refcount.
        let success = unsafe { bindings::mmget_not_zero(self.mm.as_ptr()) };

        if success {
            Some(MmGet { mm: self.mm })
        } else {
            None
        }
    }
}

// SAFETY: It is safe to call `mmdrop` on another thread than where `mmgrab` was called.
unsafe impl Send for MmGrab {}
// SAFETY: All methods on this struct are safe to call in parallel from several threads.
unsafe impl Sync for MmGrab {}

impl Drop for MmGrab {
    fn drop(&mut self) {
        // SAFETY: This gives up an `mmgrab` refcount to a valid `struct mm_struct`.
        // INVARIANT: We own an `mmgrab` refcount, so we can give it up.
        unsafe { bindings::mmdrop(self.mm.as_ptr()) };
    }
}

/// A smart pointer that references a `struct mm` and owns an `mmget` refcount.
///
/// Values of this type are created using [`MmGrab::mmget_not_zero`].
///
/// # Invariants
///
/// An `MmGet` owns an `mmget` refcount to the inner `struct mm_struct`.
pub struct MmGet {
    mm: NonNull<bindings::mm_struct>,
}

impl MmGet {
    /// Lock the mmap read lock.
    pub fn mmap_write_lock(&self) -> MmapWriteLock<'_> {
        // SAFETY: The pointer is valid since we hold a refcount.
        unsafe { bindings::mmap_write_lock(self.mm.as_ptr()) };

        // INVARIANT: We just acquired the write lock, so we can transfer to this guard.
        //
        // The signature of this function ensures that the `MmapWriteLock` will not outlive this
        // `mmget` refcount.
        MmapWriteLock {
            mm: self.mm,
            _lifetime: PhantomData,
        }
    }

    /// When dropping this refcount, use `mmput_async` instead of `mmput`.
    pub fn use_async_put(self) -> MmGetAsync {
        // Disable destructor of `self`.
        let me = ManuallyDrop::new(self);

        MmGetAsync { mm: me.mm }
    }
}

impl Drop for MmGet {
    fn drop(&mut self) {
        // SAFETY: We acquired a refcount when creating this object.
        unsafe { bindings::mmput(self.mm.as_ptr()) };
    }
}

/// A smart pointer that references a `struct mm` and owns an `mmget` refcount, that will be
/// dropped using `mmput_async`.
///
/// Values of this type are created using [`MmGet::use_async_put`].
///
/// # Invariants
///
/// An `MmGetAsync` owns an `mmget` refcount to the inner `struct mm_struct`.
pub struct MmGetAsync {
    mm: NonNull<bindings::mm_struct>,
}

impl MmGetAsync {
    /// Lock the mmap read lock.
    pub fn mmap_write_lock(&self) -> MmapWriteLock<'_> {
        // SAFETY: The pointer is valid since we hold a refcount.
        unsafe { bindings::mmap_write_lock(self.mm.as_ptr()) };

        // INVARIANT: We just acquired the write lock, so we can transfer to this guard.
        //
        // The signature of this function ensures that the `MmapWriteLock` will not outlive this
        // `mmget` refcount.
        MmapWriteLock {
            mm: self.mm,
            _lifetime: PhantomData,
        }
    }

    /// Try to lock the mmap read lock.
    pub fn mmap_read_trylock(&self) -> Option<MmapReadLock<'_>> {
        // SAFETY: The pointer is valid since we hold a refcount.
        let success = unsafe { bindings::mmap_read_trylock(self.mm.as_ptr()) };

        if success {
            // INVARIANT: We just acquired the read lock, so we can transfer to this guard.
            //
            // The signature of this function ensures that the `MmapReadLock` will not outlive this
            // `mmget` refcount.
            Some(MmapReadLock {
                mm: self.mm,
                _lifetime: PhantomData,
            })
        } else {
            None
        }
    }
}

impl Drop for MmGetAsync {
    fn drop(&mut self) {
        // SAFETY: We acquired a refcount when creating this object.
        unsafe { bindings::mmput_async(self.mm.as_ptr()) };
    }
}

/// A guard for the mmap read lock.
///
/// # Invariants
///
/// This `MmapReadLock` guard owns the mmap read lock. For the duration of 'a, the `mmget` refcount
/// will remain positive.
pub struct MmapReadLock<'a> {
    mm: NonNull<bindings::mm_struct>,
    _lifetime: PhantomData<&'a bindings::mm_struct>,
}

impl<'a> MmapReadLock<'a> {
    /// Look up a vma at the given address.
    pub fn vma_lookup(&self, vma_addr: usize) -> Option<&virt::Area> {
        // SAFETY: The `mm` pointer is known to be valid while this read lock is held.
        let vma = unsafe { bindings::vma_lookup(self.mm.as_ptr(), vma_addr as u64) };

        if vma.is_null() {
            None
        } else {
            // SAFETY: We just checked that a vma was found, so the pointer is valid. Furthermore,
            // the returned area will borrow from this read lock guard, so it can only be used
            // while the read lock is still held. The returned reference is immutable, so the
            // reference cannot be used to modify the area.
            unsafe { Some(virt::Area::from_ptr(vma)) }
        }
    }
}

impl Drop for MmapReadLock<'_> {
    fn drop(&mut self) {
        // SAFETY: We acquired the lock when creating this object.
        unsafe { bindings::mmap_read_unlock(self.mm.as_ptr()) };
    }
}

/// A guard for the mmap write lock.
///
/// # Invariants
///
/// This `MmapReadLock` guard owns the mmap write lock. For the duration of 'a, the `mmget` refcount
/// will remain positive.
pub struct MmapWriteLock<'a> {
    mm: NonNull<bindings::mm_struct>,
    _lifetime: PhantomData<&'a mut bindings::mm_struct>,
}

impl<'a> MmapWriteLock<'a> {
    /// Look up a vma at the given address.
    pub fn vma_lookup(&mut self, vma_addr: usize) -> Option<&mut virt::Area> {
        // SAFETY: The `mm` pointer is known to be valid while this read lock is held.
        let vma = unsafe { bindings::vma_lookup(self.mm.as_ptr(), vma_addr as u64) };

        if vma.is_null() {
            None
        } else {
            // SAFETY: We just checked that a vma was found, so the pointer is valid. Furthermore,
            // the returned area will borrow from this write lock guard, so it can only be used
            // while the write lock is still held. We hold the write lock, so mutable operations on
            // the area are okay.
            unsafe { Some(virt::Area::from_ptr_mut(vma)) }
        }
    }
}

impl Drop for MmapWriteLock<'_> {
    fn drop(&mut self) {
        // SAFETY: We acquired the lock when creating this object.
        unsafe { bindings::mmap_write_unlock(self.mm.as_ptr()) };
    }
}
