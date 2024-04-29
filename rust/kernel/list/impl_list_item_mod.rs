// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Helpers for implementing [`ListItem`] safely.
//!
//! [`ListItem`]: crate::list::ListItem

use crate::list::ListLinks;

/// Declares that this type has a `ListLinks<ID>` field at a fixed offset.
///
/// This trait is only used to help implement `ListItem` safely. If `ListItem` is implemented
/// manually, then this trait is not needed.
///
/// # Safety
///
/// All values of this type must have a `ListLinks<ID>` field at the given offset.
pub unsafe trait HasListLinks<const ID: u64 = 0> {
    /// The offset of the `ListLinks` field.
    const OFFSET: usize;

    /// Returns a pointer to the [`ListLinks<T, ID>`] field.
    ///
    /// # Safety
    ///
    /// The provided pointer must point at a valid struct of type `Self`.
    ///
    /// [`ListLinks<T, ID>`]: ListLinks
    // We don't really need this method, but it's necessary for the implementation of
    // `impl_has_work!` to be correct.
    #[inline]
    unsafe fn raw_get_list_links(ptr: *mut Self) -> *mut ListLinks<ID> {
        // SAFETY: The caller promises that the pointer is valid.
        unsafe { (ptr as *mut u8).add(Self::OFFSET) as *mut ListLinks<ID> }
    }
}

/// Implements the [`HasListLinks`] trait for the given type.
#[macro_export]
macro_rules! impl_has_list_links {
    ($(impl$(<$($implarg:ident),*>)?
       HasListLinks$(<$id:tt>)?
       for $self:ident $(<$($selfarg:ty),*>)?
       { self$(.$field:ident)* }
    )*) => {$(
        // SAFETY: The implementation of `raw_get_list_links` only compiles if the field has the
        // right type.
        unsafe impl$(<$($implarg),*>)? $crate::list::HasListLinks$(<$id>)? for
            $self $(<$($selfarg),*>)?
        {
            const OFFSET: usize = ::core::mem::offset_of!(Self, $($field).*) as usize;

            #[inline]
            unsafe fn raw_get_list_links(ptr: *mut Self) -> *mut $crate::list::ListLinks$(<$id>)? {
                // SAFETY: The caller promises that the pointer is not dangling.
                unsafe {
                    ::core::ptr::addr_of_mut!((*ptr)$(.$field)*)
                }
            }
        }
    )*};
}

/// Declares that the `ListLinks<ID>` field in this struct is inside a `ListLinksSelfPtr<T, ID>`.
///
/// # Safety
///
/// The `ListLinks<ID>` field of this struct at the offset `HasListLinks<ID>::OFFSET` must be
/// inside a `ListLinksSelfPtr<T, ID>`.
pub unsafe trait HasSelfPtr<T: ?Sized, const ID: u64 = 0>
where
    Self: HasListLinks<ID>,
{
}

/// Implements the [`HasListLinks`] and [`HasSelfPtr`] traits for the given type.
#[macro_export]
macro_rules! impl_has_list_links_self_ptr {
    ($(impl$({$($implarg:tt)*})?
       HasSelfPtr<$item_type:ty $(, $id:tt)?>
       for $self:ident $(<$($selfarg:ty),*>)?
       { self.$field:ident }
    )*) => {$(
        // SAFETY: The implementation of `raw_get_list_links` only compiles if the field has the
        // right type.
        unsafe impl$(<$($implarg)*>)? $crate::list::HasSelfPtr<$item_type $(, $id)?> for
            $self $(<$($selfarg),*>)?
        {}

        unsafe impl$(<$($implarg)*>)? $crate::list::HasListLinks$(<$id>)? for
            $self $(<$($selfarg),*>)?
        {
            const OFFSET: usize = ::core::mem::offset_of!(Self, $field) as usize;

            #[inline]
            unsafe fn raw_get_list_links(ptr: *mut Self) -> *mut $crate::list::ListLinks$(<$id>)? {
                // SAFETY: The caller promises that the pointer is not dangling.
                let ptr: *mut $crate::list::ListLinksSelfPtr<$item_type $(, $id)?> =
                    unsafe { ::core::ptr::addr_of_mut!((*ptr).$field) };
                ptr.cast()
            }
        }
    )*};
}

/// Declares that this type supports `ListArc`.
#[macro_export]
macro_rules! impl_list_arc_safe {
    (impl$({$($generics:tt)*})? ListArcSafe<$num:tt> for $t:ty { untracked; } $($rest:tt)*) => {
        impl$(<$($generics)*>)? ListArcSafe<$num> for $t {
            unsafe fn on_create_list_arc_from_unique(&mut self) {}
            unsafe fn on_drop_list_arc(&self) {}
        }
        $crate::list::impl_list_arc_safe! { $($rest)* }
    };

    (impl$({$($generics:tt)*})? ListArcSafe<$num:tt> for $t:ty {
        tracked_by $field:ident : $fty:ty;
    } $($rest:tt)*) => {
        impl$(<$($generics)*>)? ListArcSafe<$num> for $t {
            unsafe fn on_create_list_arc_from_unique(&mut self) {
                let me = self as *mut Self;
                let field: *mut $fty = unsafe { ::core::ptr::addr_of_mut!((*me).$field) };
                unsafe { <$fty as $crate::list::ListArcSafe<$num>>::on_create_list_arc_from_unique(
                        &mut *field
                ) };
            }
            unsafe fn on_drop_list_arc(&self) {
                let me = self as *const Self;
                let field: *const $fty = unsafe { ::core::ptr::addr_of!((*me).$field) };
                unsafe { <$fty as $crate::list::ListArcSafe<$num>>::on_drop_list_arc(&*field) };
            }
        }
        unsafe impl$(<$($generics)*>)? TryNewListArc<$num> for $t
        where
            $fty: TryNewListArc<$num>,
        {
            fn try_new_list_arc(&self) -> bool {
                let me = self as *const Self;
                let field: *const $fty = unsafe { ::core::ptr::addr_of!((*me).$field) };
                unsafe { <$fty as $crate::list::TryNewListArc<$num>>::try_new_list_arc(&*field) }
            }
        }
        $crate::list::impl_list_arc_safe! { $($rest)* }
    };

    () => {};
}

/// Implements the [`ListItem`] trait for the given type.
///
/// Assumes that the type implements [`HasListLinks`]. If using the `ListLinksSelfPtr` strategy,
/// then it also assumes that the type implements the [`HasSelfPtr`] trait.
///
/// [`ListItem`]: crate::list::ListItem
#[macro_export]
macro_rules! impl_list_item {
    (
        impl$({$($generics:tt)*})? ListItem<$num:tt> for $t:ty {
            using ListLinks;
        } $($rest:tt)*
    ) => {
        unsafe impl$(<$($generics)*>)? ListItem<$num> for $t {
            unsafe fn view_links(me: *const Self) -> *mut ListLinks<$num> {
                unsafe {
                    <Self as HasListLinks<$num>>::raw_get_list_links(me.cast_mut())
                }
            }

            unsafe fn view_value(me: *mut ListLinks<$num>) -> *const Self {
                let offset = <Self as HasListLinks<$num>>::OFFSET;
                unsafe { (me as *const u8).sub(offset) as *const Self }
            }

            unsafe fn prepare_to_insert(me: *const Self) -> *mut ListLinks<$num> {
                unsafe { Self::view_links(me) }
            }

            unsafe fn post_remove(me: *mut ListLinks<$num>) -> *const Self {
                unsafe { Self::view_value(me) }
            }
        }
    };

    (
        impl$({$($generics:tt)*})? ListItem<$num:tt> for $t:ty {
            using ListLinksSelfPtr;
        } $($rest:tt)*
    ) => {
        unsafe impl$(<$($generics)*>)? ListItem<$num> for $t {
            unsafe fn prepare_to_insert(me: *const Self) -> *mut ListLinks<$num> {
                let links_field = unsafe { Self::view_links(me) };

                let spoff = ListLinksSelfPtr::<Self, $num>::LIST_LINKS_SELF_PTR_OFFSET;
                let self_ptr = unsafe { (links_field as *const u8).add(spoff)
                    as *const ::core::cell::UnsafeCell<*const Self> };
                let cell_inner = ::core::cell::UnsafeCell::raw_get(self_ptr);

                unsafe { ::core::ptr::write(cell_inner, me) };
                links_field
            }

            unsafe fn view_links(me: *const Self) -> *mut ListLinks<$num> {
                unsafe {
                    <Self as HasListLinks<$num>>::raw_get_list_links(me.cast_mut())
                }
            }

            unsafe fn view_value(links_field: *mut ListLinks<$num>) -> *const Self {
                let spoff = ListLinksSelfPtr::<Self, $num>::LIST_LINKS_SELF_PTR_OFFSET;
                let self_ptr = unsafe { (links_field as *const u8).add(spoff)
                    as *const ::core::cell::UnsafeCell<*const Self> };
                let cell_inner = ::core::cell::UnsafeCell::raw_get(self_ptr);
                unsafe {
                    ::core::ptr::read(cell_inner)
                }
            }

            unsafe fn post_remove(me: *mut ListLinks<$num>) -> *const Self {
                unsafe { Self::view_value(me) }
            }
        }
    };
}
