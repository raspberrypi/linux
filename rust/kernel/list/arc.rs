// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! A wrapper around `Arc` for linked lists.

use crate::error;
use crate::prelude::*;
use crate::sync::{Arc, ArcBorrow, UniqueArc};
use core::alloc::AllocError;
use core::marker::Unsize;
use core::ops::Deref;
use core::pin::Pin;

/// Declares that this type has some way to ensure that there is exactly one `ListArc` instance for
/// this id.
pub trait ListArcSafe<const ID: u64 = 0> {
    /// Informs the tracking inside this type that a new [`ListArc`] has just been created.
    ///
    /// # Safety
    ///
    /// Must not be called if a [`ListArc`] already exist for this value.
    unsafe fn on_create_list_arc_from_unique(&mut self);
    /// Informs the tracking inside this type that a [`ListArc`] no longer exists.
    ///
    /// # Safety
    ///
    /// Must only be called if there previously was a [`ListArc`], but there no longer is one.
    unsafe fn on_drop_list_arc(&self);
}

/// Declares that this type is able to safely attempt to create `ListArc`s at arbitrary times.
///
/// # Safety
///
/// Implementers must ensure that `try_new_list_arc` never allows the creation of multiple
/// `ListArc` references to the same value.
pub unsafe trait TryNewListArc<const ID: u64 = 0>: ListArcSafe<ID> {
    /// Attempts to convert an `Arc<Self>` into an `ListArc<Self>`. Returns `true` if the
    /// conversion was successful.
    fn try_new_list_arc(&self) -> bool;
}

/// A wrapper around `Arc` that's guaranteed unique for the given id.
#[repr(transparent)]
pub struct ListArc<T, const ID: u64 = 0>
where
    T: ListArcSafe<ID> + ?Sized,
{
    arc: Arc<T>,
}

impl<T: ListArcSafe<ID>, const ID: u64> ListArc<T, ID> {
    /// Constructs a new reference counted instance of `T`.
    pub fn try_new(contents: T) -> Result<Self, AllocError> {
        Ok(Self::from_unique(UniqueArc::try_new(contents)?))
    }

    /// Use the given initializer to in-place initialize a `T`.
    ///
    /// If `T: !Unpin` it will not be able to move afterwards.
    pub fn pin_init<E>(init: impl PinInit<T, E>) -> error::Result<Self>
    where
        Error: From<E>,
    {
        Ok(Self::from_pin_unique(UniqueArc::pin_init(init)?))
    }
}

impl<T, const ID: u64> ListArc<T, ID>
where
    T: ListArcSafe<ID> + ?Sized,
{
    /// Convert a [`UniqueArc`] into a [`ListArc`].
    pub fn from_unique(mut unique: UniqueArc<T>) -> Self {
        // SAFETY: We have a `UniqueArc`, so we can call this method.
        unsafe { T::on_create_list_arc_from_unique(&mut unique) };
        let arc = Arc::from(unique);
        // SAFETY: We just called `on_create_list_arc_from_unique` on an arc without a `ListArc`,
        // so we can create a `ListArc`.
        unsafe { Self::transmute_from_arc(arc) }
    }

    /// Convert a pinned [`UniqueArc`] into a [`ListArc`].
    pub fn from_pin_unique(unique: Pin<UniqueArc<T>>) -> Self {
        // SAFETY: A `ListArc` is pinned, so we're not actually throwing away the pinning.
        Self::from_unique(unsafe { Pin::into_inner_unchecked(unique) })
    }

    /// Like [`from_unique`], but creates two `ListArcs`.
    pub fn pair_from_unique<const ID2: u64>(mut unique: UniqueArc<T>) -> (Self, ListArc<T, ID2>)
    where
        T: ListArcSafe<ID2>,
    {
        assert_ne!(ID, ID2);

        // SAFETY: We have a `UniqueArc`, so we can call this method.
        unsafe { <T as ListArcSafe<ID>>::on_create_list_arc_from_unique(&mut unique) };
        // SAFETY: We have a `UniqueArc`, so we can call this method. The two ids are not equal.
        unsafe { <T as ListArcSafe<ID2>>::on_create_list_arc_from_unique(&mut unique) };

        let arc1 = Arc::from(unique);
        let arc2 = Arc::clone(&arc1);

        // SAFETY: We just called `on_create_list_arc_from_unique` on an arc without a `ListArc`,
        // so we can create a `ListArc`.
        unsafe {
            (
                Self::transmute_from_arc(arc1),
                ListArc::transmute_from_arc(arc2),
            )
        }
    }

    /// Like [`pair_from_unique`], but uses a pinned arc.
    pub fn pair_from_pin_unique<const ID2: u64>(
        unique: Pin<UniqueArc<T>>,
    ) -> (Self, ListArc<T, ID2>)
    where
        T: ListArcSafe<ID2>,
    {
        // SAFETY: A `ListArc` is pinned, so we're not actually throwing away the pinning.
        Self::pair_from_unique(unsafe { Pin::into_inner_unchecked(unique) })
    }

    /// Try to create a new `ListArc`.
    ///
    /// This fails if this value already has a `ListArc`.
    pub fn try_from_arc(arc: Arc<T>) -> Result<Self, Arc<T>>
    where
        T: TryNewListArc<ID>,
    {
        if arc.try_new_list_arc() {
            // SAFETY: The `try_new_list_arc` method just told us that its okay to create a new
            // `ListArc`.
            Ok(unsafe { Self::transmute_from_arc(arc) })
        } else {
            Err(arc)
        }
    }

    /// Try to create a new `ListArc`.
    ///
    /// If it's not possible to create a new `ListArc`, then the `Arc` is dropped. This will never
    /// run the destructor of the value.
    pub fn try_from_arc_or_drop(arc: Arc<T>) -> Option<Self>
    where
        T: TryNewListArc<ID>,
    {
        match Self::try_from_arc(arc) {
            Ok(list_arc) => Some(list_arc),
            Err(arc) => Arc::into_unique_or_drop(arc).map(Self::from_pin_unique),
        }
    }

    unsafe fn transmute_from_arc(me: Arc<T>) -> Self {
        // SAFETY: ListArc is repr(transparent).
        unsafe { core::mem::transmute(me) }
    }
    fn transmute_to_arc(self) -> Arc<T> {
        // SAFETY: ListArc is repr(transparent).
        unsafe { core::mem::transmute(self) }
    }

    /// Like `Arc::into_raw`.
    pub fn into_raw(self) -> *const T {
        Arc::into_raw(Self::transmute_to_arc(self))
    }

    /// Like `Arc::from_raw`.
    ///
    /// # Safety
    ///
    /// Beyond the safety requirements from `Arc::from_raw`, this must originate from a previous
    /// call to `ListArc::into_raw`.
    ///
    /// Note that this may not be used to convert an `Arc` into a `ListArc` because that would not
    /// update the tracking for whether a `ListArc` exists.
    pub unsafe fn from_raw(ptr: *const T) -> Self {
        unsafe { Self::transmute_from_arc(Arc::from_raw(ptr)) }
    }

    /// Converts the `ListArc` into an [`Arc`].
    pub fn into_arc(self) -> Arc<T> {
        let arc = Self::transmute_to_arc(self);
        // SAFETY: There is no longer a `ListArc`, so we can call this method.
        unsafe { T::on_drop_list_arc(&arc) };
        arc
    }

    /// Clone a `ListArc` into an [`Arc`].
    pub fn clone_arc(&self) -> Arc<T> {
        self.arc.clone()
    }

    /// Returns a reference to an [`Arc`] from the given [`ListArc`].
    ///
    /// This is useful when the argument of a function call is an [`&Arc`] (e.g., in a method
    /// receiver), but we have a [`ListArc`] instead.
    ///
    /// [`&Arc`]: Arc
    #[inline]
    pub fn as_arc(&self) -> &Arc<T> {
        &self.arc
    }

    /// Returns an [`ArcBorrow`] from the given [`ListArc`].
    ///
    /// This is useful when the argument of a function call is an [`ArcBorrow`] (e.g., in a method
    /// receiver), but we have an [`Arc`] instead. Getting an [`ArcBorrow`] is free when optimised.
    #[inline]
    pub fn as_arc_borrow(&self) -> ArcBorrow<'_, T> {
        self.arc.as_arc_borrow()
    }

    /// Compare whether two [`ListArc`] pointers reference the same underlying object.
    pub fn ptr_eq(this: &Self, other: &Self) -> bool {
        Arc::ptr_eq(&this.arc, &other.arc)
    }
}

impl<T, const ID: u64> Deref for ListArc<T, ID>
where
    T: ListArcSafe<ID> + ?Sized,
{
    type Target = T;

    fn deref(&self) -> &Self::Target {
        self.arc.deref()
    }
}

impl<T, const ID: u64> Drop for ListArc<T, ID>
where
    T: ListArcSafe<ID> + ?Sized,
{
    fn drop(&mut self) {
        // SAFETY: We're destroying the `ListArc`.
        unsafe {
            T::on_drop_list_arc(&self.arc);
        }
    }
}

// This is to allow [`ListArc`] (and variants) to be used as the type of `self`.
impl<T, const ID: u64> core::ops::Receiver for ListArc<T, ID> where T: ListArcSafe<ID> + ?Sized {}

// This is to allow coercion from `ListArc<T>` to `ListArc<U>` if `T` can be converted to the
// dynamically-sized type (DST) `U`.
impl<T, U, const ID: u64> core::ops::CoerceUnsized<ListArc<U, ID>> for ListArc<T, ID>
where
    T: ListArcSafe<ID> + Unsize<U> + ?Sized,
    U: ListArcSafe<ID> + ?Sized,
{
}

// This is to allow `ListArc<U>` to be dispatched on when `ListArc<T>` can be coerced into
// `ListArc<U>`.
impl<T, U, const ID: u64> core::ops::DispatchFromDyn<ListArc<U, ID>> for ListArc<T, ID>
where
    T: ListArcSafe<ID> + Unsize<U> + ?Sized,
    U: ListArcSafe<ID> + ?Sized,
{
}
