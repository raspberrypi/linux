// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! A linked list implementation.

use crate::init::PinInit;
use crate::sync::ArcBorrow;
use crate::types::Opaque;
use core::cell::UnsafeCell;
use core::iter::{DoubleEndedIterator, FusedIterator};
use core::marker::{PhantomData, PhantomPinned};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::{AtomicBool, Ordering};

mod impl_list_item_mod;
pub use self::impl_list_item_mod::{HasListLinks, HasSelfPtr};
pub use crate::{
    impl_has_list_links, impl_has_list_links_self_ptr, impl_list_arc_safe, impl_list_item,
};

mod arc;
pub use self::arc::{ListArc, ListArcSafe, TryNewListArc};

mod arc_field;
pub use self::arc_field::{define_list_arc_field_getter, ListArcField};

/// A linked list.
///
/// All elements in this linked list will be [`ListArc`] references to the value. Since a value can
/// only have one `ListArc` (for each pair of prev/next pointers), this ensures that the same
/// prev/next pointers are not used for several linked lists.
///
/// # Invariants
///
/// If the list is empty, then `first` is null. Otherwise, it points at the links field of the
/// first element of this list. The prev/next pointers of items in the list will always form a
/// cycle. This means that prev/next pointers for an item in a list are never null and never
/// dangling.
pub struct List<T: ?Sized + ListItem<ID>, const ID: u64 = 0> {
    first: *mut ListLinksFields,
    _ty: PhantomData<ListArc<T, ID>>,
}

// SAFETY: This is a container of `ListArc<T, ID>`, and access to the container allows the same
// type of access to the `ListArc<T, ID>` elements.
unsafe impl<T, const ID: u64> Send for List<T, ID>
where
    ListArc<T, ID>: Send,
    T: ?Sized + ListItem<ID>,
{
}
// SAFETY: This is a container of `ListArc<T, ID>`, and access to the container allows the same
// type of access to the `ListArc<T, ID>` elements.
unsafe impl<T, const ID: u64> Sync for List<T, ID>
where
    ListArc<T, ID>: Sync,
    T: ?Sized + ListItem<ID>,
{
}

/// Implemented by types where a [`ListArc<Self>`] can be inserted into a [`List`].
///
/// # Safety
///
/// Implementers must ensure that they provide the guarantees documented on the three methods
/// below.
///
/// [`ListArc<Self>`]: ListArc
pub unsafe trait ListItem<const ID: u64 = 0>: ListArcSafe<ID> {
    /// Views the [`ListLinks`] for this value.
    ///
    /// # Guarantees
    ///
    /// * If there is a currently active call to `prepare_to_insert`, then this returns the same
    ///   pointer as the one returned by the currently active call to `prepare_to_insert`.
    /// * If there is no currently active call to `prepare_to_insert`, then the returned pointer
    ///   points at a read-only [`ListLinks`] with two null pointers.
    ///
    /// # Safety
    ///
    /// The provided pointer must point at a valid value. (It need not be in an `Arc`.)
    unsafe fn view_links(me: *const Self) -> *mut ListLinks<ID>;

    /// View the full value given its [`ListLinks`] field.
    ///
    /// Can only be used when the value is in a list.
    ///
    /// # Guarantees
    ///
    /// * Returns the same pointer as the one passed to the previous call to `prepare_to_insert`.
    /// * The returned pointer is valid until the next call to `post_remove`.
    ///
    /// # Safety
    ///
    /// * The provided pointer must originate from the previous call to `prepare_to_insert`, or
    ///   from a call to `view_links` that happened after the previous call to `prepare_to_insert`.
    /// * Since the previous call to `prepare_to_insert`, the `post_remove` method must not have
    ///   been called.
    unsafe fn view_value(me: *mut ListLinks<ID>) -> *const Self;

    /// This is called when an item is inserted into a [`List`].
    ///
    /// # Guarantees
    ///
    /// The caller is granted exclusive access to the returned [`ListLinks`] until `post_remove` is
    /// called.
    ///
    /// # Safety
    ///
    /// * The provided pointer must point at a valid value in an [`Arc`].
    /// * Calls to `prepare_to_insert` and `post_remove` on the same value must alternate.
    /// * The caller must own the [`ListArc`] for this value.
    /// * The caller must not give up ownership of the [`ListArc`] unless `post_remove` has been
    ///   called after this call to `prepare_to_insert`.
    ///
    /// [`Arc`]: crate::sync::Arc
    unsafe fn prepare_to_insert(me: *const Self) -> *mut ListLinks<ID>;

    /// This undoes a previous call to `prepare_to_insert`.
    ///
    /// # Guarantees
    ///
    /// The returned pointer is the pointer that was originally passed to `prepare_to_insert`.
    ///
    /// The caller is free to recreate the `ListArc` after this call.
    ///
    /// # Safety
    ///
    /// The provided pointer must be the pointer returned by the previous call to
    /// `prepare_to_insert`.
    unsafe fn post_remove(me: *mut ListLinks<ID>) -> *const Self;
}

#[repr(C)]
struct ListLinksFields {
    next: *mut ListLinksFields,
    prev: *mut ListLinksFields,
}

/// The prev/next pointers for an item in a linked list.
///
/// # Invariants
///
/// The fields are null if and only if this item is not in a list.
#[repr(transparent)]
pub struct ListLinks<const ID: u64 = 0> {
    inner: Opaque<ListLinksFields>,
}

unsafe impl<const ID: u64> Send for ListLinks<ID> {}
unsafe impl<const ID: u64> Sync for ListLinks<ID> {}

impl<const ID: u64> ListLinks<ID> {
    /// Creates a new initializer for this type.
    pub fn new() -> impl PinInit<Self> {
        // INVARIANT: Pin-init initializers can't be used on an existing `Arc`, so this value will
        // not be constructed in an `Arc` that already has a `ListArc`.
        ListLinks {
            inner: Opaque::new(ListLinksFields {
                prev: ptr::null_mut(),
                next: ptr::null_mut(),
            }),
        }
    }

    /// # Safety
    ///
    /// The pointer must be dereferencable.
    #[inline]
    unsafe fn fields(me: *mut Self) -> *mut ListLinksFields {
        // SAFETY: The caller promises that the pointer is valid.
        unsafe { Opaque::raw_get(ptr::addr_of!((*me).inner)) }
    }

    /// # Safety
    ///
    /// The pointer must be dereferencable.
    #[inline]
    unsafe fn from_fields(me: *mut ListLinksFields) -> *mut Self {
        me.cast()
    }
}

/// Similar to [`ListLinks`], but also contains a pointer to the full value.
///
/// This type can be used instead of [`ListLinks`] to support lists with trait objects.
#[repr(C)]
pub struct ListLinksSelfPtr<T: ?Sized, const ID: u64 = 0> {
    /// The `ListLinks` field inside this value.
    ///
    /// This is public so that it can be used with `impl_has_list_links!`.
    pub inner: ListLinks<ID>,
    self_ptr: UnsafeCell<MaybeUninit<*const T>>,
}

unsafe impl<T: ?Sized + Send, const ID: u64> Send for ListLinksSelfPtr<T, ID> {}
unsafe impl<T: ?Sized + Sync, const ID: u64> Sync for ListLinksSelfPtr<T, ID> {}

impl<T: ?Sized, const ID: u64> ListLinksSelfPtr<T, ID> {
    /// The offset from the [`ListLinks`] to the self pointer field.
    pub const LIST_LINKS_SELF_PTR_OFFSET: usize = core::mem::offset_of!(Self, self_ptr);

    /// Creates a new initializer for this type.
    pub fn new() -> impl PinInit<Self> {
        // INVARIANT: Pin-init initializers can't be used on an existing `Arc`, so this value will
        // not be constructed in an `Arc` that already has a `ListArc`.
        Self {
            inner: ListLinks {
                inner: Opaque::new(ListLinksFields {
                    prev: ptr::null_mut(),
                    next: ptr::null_mut(),
                }),
            },
            self_ptr: UnsafeCell::new(MaybeUninit::zeroed()),
        }
    }
}

/// A utility for tracking whether a [`ListArc`] exists using an atomic.
///
/// # Invariant
///
/// If the boolean is `false`, then there is no [`ListArc`] for this value.
#[repr(transparent)]
pub struct AtomicListArcTracker<const ID: u64 = 0> {
    inner: AtomicBool,
    _pin: PhantomPinned,
}

impl<const ID: u64> AtomicListArcTracker<ID> {
    /// Creates a new initializer for this type.
    pub fn new() -> impl PinInit<Self> {
        // INVARIANT: Pin-init initializers can't be used on an existing `Arc`, so this value will
        // not be constructed in an `Arc` that already has a `ListArc`.
        Self {
            inner: AtomicBool::new(false),
            _pin: PhantomPinned,
        }
    }
}

impl<const ID: u64> ListArcSafe<ID> for AtomicListArcTracker<ID> {
    unsafe fn on_create_list_arc_from_unique(&mut self) {
        // INVARIANT: We just created a ListArc, so the boolean should be true.
        *self.inner.get_mut() = true;
    }

    unsafe fn on_drop_list_arc(&self) {
        // INVARIANT: We just dropped a ListArc, so the boolean should be false.
        self.inner.store(false, Ordering::Release);
    }
}

// SAFETY: If this method returns `true`, then by the type invariant there is no `ListArc` before
// this call, so it is okay to create a new `ListArc`.
//
// The acquire ordering will synchronize with the release store from the destruction of any
// previous `ListArc`, so any such destructions happens-before the creation of the new `ListArc`.
unsafe impl<const ID: u64> TryNewListArc<ID> for AtomicListArcTracker<ID> {
    fn try_new_list_arc(&self) -> bool {
        // INVARIANT: If this method returns true, then the boolean used to be false, and is no
        // longer false, so it is okay for the caller to create a new [`ListArc`].
        self.inner
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
    }
}

impl<T: ?Sized + ListItem<ID>, const ID: u64> List<T, ID> {
    /// Creates a new empty list.
    pub const fn new() -> Self {
        Self {
            first: ptr::null_mut(),
            _ty: PhantomData,
        }
    }

    /// Returns whether this list is empty.
    pub fn is_empty(&self) -> bool {
        self.first.is_null()
    }

    /// Add the provided item to the back of the list.
    pub fn push_back(&mut self, item: ListArc<T, ID>) {
        let item = unsafe { ListLinks::fields(T::prepare_to_insert(ListArc::into_raw(item))) };

        if self.first.is_null() {
            self.first = item;
            // SAFETY: The caller just gave us ownership of these fields.
            // INVARIANT: A linked list with one item should be cyclic.
            unsafe {
                (*item).next = item;
                (*item).prev = item;
            }
        } else {
            let next = self.first;
            // SAFETY: We just checked that `next` is non-null.
            let prev = unsafe { (*next).prev };
            // SAFETY: Pointers in a linked list are never dangling, and the caller just gave us
            // ownership of the fields on `item`.
            // INVARIANT: This correctly inserts `item` between `prev` and `next`.
            unsafe {
                (*item).next = next;
                (*item).prev = prev;
                (*prev).next = item;
                (*next).prev = item;
            }
        }
    }

    /// Add the provided item to the front of the list.
    pub fn push_front(&mut self, item: ListArc<T, ID>) {
        let item = unsafe { ListLinks::fields(T::prepare_to_insert(ListArc::into_raw(item))) };

        if self.first.is_null() {
            // SAFETY: The caller just gave us ownership of these fields.
            // INVARIANT: A linked list with one item should be cyclic.
            unsafe {
                (*item).next = item;
                (*item).prev = item;
            }
        } else {
            let next = self.first;
            // SAFETY: We just checked that `next` is non-null.
            let prev = unsafe { (*next).prev };
            // SAFETY: Pointers in a linked list are never dangling, and the caller just gave us
            // ownership of the fields on `item`.
            // INVARIANT: This correctly inserts `item` between `prev` and `next`.
            unsafe {
                (*item).next = next;
                (*item).prev = prev;
                (*prev).next = item;
                (*next).prev = item;
            }
        }
        self.first = item;
    }

    /// Removes the last item from this list.
    pub fn pop_back(&mut self) -> Option<ListArc<T, ID>> {
        if self.first.is_null() {
            return None;
        }

        // SAFETY: We just checked that the list is not empty.
        let last = unsafe { (*self.first).prev };
        // SAFETY: The last item of this list is in this list.
        Some(unsafe { self.remove_internal(last) })
    }

    /// Removes the first item from this list.
    pub fn pop_front(&mut self) -> Option<ListArc<T, ID>> {
        if self.first.is_null() {
            return None;
        }

        // SAFETY: The first item of this list is in this list.
        Some(unsafe { self.remove_internal(self.first) })
    }

    /// Removes the provided item from this list and returns it.
    ///
    /// This returns `None` if the item is not in the list.
    ///
    /// # Safety
    ///
    /// The provided item must not be in a different linked list.
    pub unsafe fn remove(&mut self, item: &T) -> Option<ListArc<T, ID>> {
        let mut item = unsafe { ListLinks::fields(T::view_links(item)) };
        // SAFETY: The user provided a reference, and reference are never dangling.
        //
        // As for why this is not a data race, there are two cases:
        //
        //  * If `item` is not in any list, then these fields are read-only and null.
        //  * If `item` is in this list, then we have exclusive access to these fields since we
        //    have a mutable reference to the list.
        //
        // In either case, there's no race.
        let next = unsafe { (*item).next };
        // SAFETY: See above.
        let prev = unsafe { (*item).prev };

        debug_assert_eq!(next.is_null(), prev.is_null());
        if !next.is_null() {
            // This is really a no-op, but this ensures that `item` is a raw pointer that was
            // obtained straight from the allocator without going through a reference anywhere.
            // This ensures that the list is valid under the more restrictive strict provenance
            // ruleset.
            //
            // SAFETY: We just checked that `next` is not null, and its not dangling by the
            // list invariants.
            unsafe {
                debug_assert_eq!(item, (*next).prev);
                item = (*next).prev;
            }

            // SAFETY: We just checked that `item` is in a list, so the caller guarantees that it
            // is in this list. The pointers are in the right order.
            Some(unsafe { self.remove_internal_inner(item, next, prev) })
        } else {
            None
        }
    }

    /// Removes the provided item from the list.
    ///
    /// # Safety
    ///
    /// The pointer must point at an item in this list.
    unsafe fn remove_internal(&mut self, item: *mut ListLinksFields) -> ListArc<T, ID> {
        // SAFETY: The caller promises that this pointer is not dangling, and there's no data race
        // since we have a mutable reference to the list containing `item`.
        let next = unsafe { (*item).next };
        // SAFETY: See above.
        let prev = unsafe { (*item).prev };
        // SAFETY: The pointers are ok and in the right order.
        unsafe { self.remove_internal_inner(item, next, prev) }
    }

    /// Removes the provided item from the list.
    ///
    /// # Safety
    ///
    /// The pointer must point at an item in this list, and we must have `(*item).next == next` and
    /// `(*item).prev == prev`.
    unsafe fn remove_internal_inner(
        &mut self,
        item: *mut ListLinksFields,
        next: *mut ListLinksFields,
        prev: *mut ListLinksFields,
    ) -> ListArc<T, ID> {
        // SAFETY: We have exclusive access to items in the list, and prev/next pointers are
        // never null for items in a list.
        //
        // INVARIANT: There are three cases:
        //  * If the list has at least three items, then after removing the item, `prev` and `next`
        //    will be next to each other.
        //  * If the list has two items, then the remaining item will point at itself.
        //  * If this is the only item, then these writes have no effect since we immediately
        //    override them with null.
        unsafe {
            (*next).prev = prev;
            (*prev).next = next;
        }
        // SAFETY: We have exclusive access to items in the list.
        // INVARIANT: The item is no longer in a list, so the pointers should be null.
        unsafe {
            (*item).prev = ptr::null_mut();
            (*item).next = ptr::null_mut();
        }
        // INVARIANT: There are three cases:
        //  * If `item` was not the first item, then `self.first` should remain unchanged.
        //  * If `item` was the first item and there is another item, then we just updated
        //    `prev->next` to `next`, which is the new first item, and setting `item->next` to null
        //    did not modify `prev->next`.
        //  * If `item` was the only item in the list, then `prev == item`, and we just set
        //    `item->next` to null, so this correctly sets `first` to null now that the list is
        //    empty.
        if self.first == item {
            // SAFETY: The `prev` field of an item in a list is never dangling.
            self.first = unsafe { (*prev).next };
        }

        // SAFETY: We just removed a `ListArc` from the list, so we can turn it back into a
        // `ListArc`.
        unsafe { ListArc::from_raw(T::post_remove(ListLinks::from_fields(item))) }
    }

    /// Moves all items from `other` into `self`.
    ///
    /// The items of `other` are added to the back of `self`, so the last item of `other` becomes
    /// the last item of `self`.
    pub fn push_all_back(&mut self, other: &mut List<T, ID>) {
        // First, we insert the elements into `self`. At the end, we make `other` empty.
        if self.is_empty() {
            // INVARIANT: All of the elements in `other` become elements of `self`.
            self.first = other.first;
        } else if !other.is_empty() {
            let other_first = other.first;
            let other_last = unsafe { (*other_first).prev };
            let self_first = self.first;
            let self_last = unsafe { (*self_first).prev };

            // SAFETY: We have exclusive access to both lists, so we can update the pointers.
            // INVARIANT: This correctly sets the pointers to merge both lists.
            unsafe {
                (*self_first).prev = other_last;
                (*other_last).next = self_first;
                (*self_last).next = other_first;
                (*other_first).prev = self_last;
            }
        }

        // INVARIANT: The other list is now empty, so update its pointer.
        other.first = ptr::null_mut();
    }

    /// Returns a cursor to the first element of the list.
    ///
    /// If the list is empty, this returns `None`.
    pub fn cursor_front(&mut self) -> Option<Cursor<'_, T, ID>> {
        if self.first.is_null() {
            None
        } else {
            Some(Cursor {
                current: self.first,
                list: self,
            })
        }
    }

    /// Creates an iterator over the list.
    pub fn iter(&self) -> Iter<'_, T, ID> {
        // INVARIANT: If the list is empty, both pointers are null. Otherwise, both pointers point
        // at the first element of the same list.
        Iter {
            current: self.first,
            stop: self.first,
            _ty: PhantomData,
        }
    }
}

impl<T: ?Sized + ListItem<ID>, const ID: u64> Drop for List<T, ID> {
    fn drop(&mut self) {
        while let Some(item) = self.pop_front() {
            drop(item);
        }
    }
}

/// A cursor into a [`List`].
///
/// # Invariants
///
/// The `current` pointer points a value in `list`.
pub struct Cursor<'a, T: ?Sized + ListItem<ID>, const ID: u64 = 0> {
    current: *mut ListLinksFields,
    list: &'a mut List<T, ID>,
}

impl<'a, T: ?Sized + ListItem<ID>, const ID: u64> Cursor<'a, T, ID> {
    /// Access the current element of this cursor.
    pub fn current(&self) -> ArcBorrow<'_, T> {
        // SAFETY: The `current` pointer points a value in the list.
        let me = unsafe { T::view_value(ListLinks::from_fields(self.current)) };
        // SAFETY:
        // * All values in a list are stored in an `Arc`.
        // * The value cannot be removed from the list for the duration of the lifetime annotated
        //   on the returned `ArcBorrow`, because removing it from the list would require mutable
        //   access to the cursor or the list. However, the `ArcBorrow` holds an immutable borrow
        //   on the cursor, which in turn holds an immutable borrow on the list, so any such
        //   mutable access requires first releasing the immutable borrow on the cursor.
        // * Values in a list never have a `UniqueArc` reference.
        unsafe { ArcBorrow::from_raw(me) }
    }

    /// Move the cursor to the next element.
    pub fn next(self) -> Option<Cursor<'a, T, ID>> {
        // SAFETY: The `current` field is always in a list.
        let next = unsafe { (*self.current).next };

        if next == self.list.first {
            None
        } else {
            Some(Cursor {
                current: next,
                list: self.list,
            })
        }
    }

    /// Move the cursor to the previous element.
    pub fn prev(self) -> Option<Cursor<'a, T, ID>> {
        // SAFETY: The `current` field is always in a list.
        let prev = unsafe { (*self.current).prev };

        if self.current == self.list.first {
            None
        } else {
            Some(Cursor {
                current: prev,
                list: self.list,
            })
        }
    }

    /// Remove the current element from the list.
    pub fn remove(self) -> ListArc<T, ID> {
        // SAFETY: The `current` pointer always points at a member of the list.
        unsafe { self.list.remove_internal(self.current) }
    }
}

/// An iterator into a [`List`].
///
/// # Invariants
///
/// The `current` pointer points at a value in a list, or it is null if the iterator has reached
/// the end of the list. The `stop` pointer points at the first value in the same list, or it is
/// null if the list is empty.
#[derive(Clone)]
pub struct Iter<'a, T: ?Sized + ListItem<ID>, const ID: u64 = 0> {
    current: *mut ListLinksFields,
    stop: *mut ListLinksFields,
    _ty: PhantomData<&'a ListArc<T, ID>>,
}

impl<'a, T: ?Sized + ListItem<ID>, const ID: u64> Iterator for Iter<'a, T, ID> {
    type Item = ArcBorrow<'a, T>;

    fn next(&mut self) -> Option<ArcBorrow<'a, T>> {
        if self.current.is_null() {
            return None;
        }

        let current = self.current;

        // SAFETY: We just checked that `current` is not null, so it is in a list, and hence not
        // dangling. There's no race because the iterator holds an immutable borrow to the list.
        let next = unsafe { (*current).next };
        // INVARIANT: If `current` was the last element of the list, then this updates it to null.
        // Otherwise, we update it to the next element.
        self.current = if next != self.stop {
            next
        } else {
            ptr::null_mut()
        };

        // SAFETY: The `current` pointer points a value in the list.
        let item = unsafe { T::view_value(ListLinks::from_fields(current)) };
        // SAFETY:
        // * All values in a list are stored in an `Arc`.
        // * The value cannot be removed from the list for the duration of the lifetime annotated
        //   on the returned `ArcBorrow`, because removing it from the list would require mutable
        //   access to the list. However, the `ArcBorrow` is annotated with the iterator's
        //   lifetime, and the list is immutably borrowed for that lifetime.
        // * Values in a list never have a `UniqueArc` reference.
        Some(unsafe { ArcBorrow::from_raw(item) })
    }
}

impl<'a, T: ?Sized + ListItem<ID>, const ID: u64> FusedIterator for Iter<'a, T, ID> {}

impl<'a, T: ?Sized + ListItem<ID>, const ID: u64> IntoIterator for &'a List<T, ID> {
    type IntoIter = Iter<'a, T, ID>;
    type Item = ArcBorrow<'a, T>;

    fn into_iter(self) -> Iter<'a, T, ID> {
        self.iter()
    }
}

/// An owning iterator into a [`List`].
pub struct IntoIter<T: ?Sized + ListItem<ID>, const ID: u64 = 0> {
    list: List<T, ID>,
}

impl<T: ?Sized + ListItem<ID>, const ID: u64> Iterator for IntoIter<T, ID> {
    type Item = ListArc<T, ID>;

    fn next(&mut self) -> Option<ListArc<T, ID>> {
        self.list.pop_front()
    }
}

impl<T: ?Sized + ListItem<ID>, const ID: u64> FusedIterator for IntoIter<T, ID> {}

impl<T: ?Sized + ListItem<ID>, const ID: u64> DoubleEndedIterator for IntoIter<T, ID> {
    fn next_back(&mut self) -> Option<ListArc<T, ID>> {
        self.list.pop_back()
    }
}

impl<T: ?Sized + ListItem<ID>, const ID: u64> IntoIterator for List<T, ID> {
    type IntoIter = IntoIter<T, ID>;
    type Item = ListArc<T, ID>;

    fn into_iter(self) -> IntoIter<T, ID> {
        IntoIter { list: self }
    }
}
