// SPDX-License-Identifier: GPL-2.0

//! File systems.
//!
//! C headers: [`include/linux/fs.h`](../../../../include/linux/fs.h)

use crate::error::{from_result, to_result, Error, Result};
use crate::file;
use crate::types::{ARef, AlwaysRefCounted, ForeignOwnable, Opaque, ScopeGuard};
use crate::{
    bindings, container_of, delay::coarse_sleep, error::code::*, pr_warn, str::CStr, ThisModule,
};
use alloc::boxed::Box;
use core::mem::{align_of, size_of, ManuallyDrop, MaybeUninit};
use core::sync::atomic::{AtomicU64, Ordering};
use core::time::Duration;
use core::{marker::PhantomData, marker::PhantomPinned, ops::Deref, pin::Pin, ptr};

use macros::vtable;

pub mod param;

/// Type of superblock keying.
///
/// It determines how C's `fs_context_operations::get_tree` is implemented.
pub enum Super {
    /// Only one such superblock may exist.
    Single,

    /// As [`Super::Single`], but reconfigure if it exists.
    SingleReconf,

    /// Superblocks with different data pointers may exist.
    Keyed,

    /// Multiple independent superblocks may exist.
    Independent,

    /// Uses a block device.
    BlockDev,
}

/// A file system context.
///
/// It is used to gather configuration to then mount or reconfigure a file system.
#[vtable]
pub trait Context<T: Type + ?Sized> {
    /// Type of the data associated with the context.
    type Data: ForeignOwnable + Send + Sync + 'static;

    /// The typed file system parameters.
    ///
    /// Users are encouraged to define it using the [`crate::define_fs_params`] macro.
    const PARAMS: param::SpecTable<'static, Self::Data> = param::SpecTable::empty();

    /// Creates a new context.
    fn try_new() -> Result<Self::Data>;

    /// Parses a parameter that wasn't specified in [`Self::PARAMS`].
    fn parse_unknown_param(
        _data: &mut Self::Data,
        _name: &CStr,
        _value: param::Value<'_>,
    ) -> Result {
        Err(ENOPARAM)
    }

    /// Parses the whole parameter block, potentially skipping regular handling for parts of it.
    ///
    /// The return value is the portion of the input buffer for which the regular handling
    /// (involving [`Self::PARAMS`] and [`Self::parse_unknown_param`]) will still be carried out.
    /// If it's `None`, the regular handling is not performed at all.
    fn parse_monolithic<'a>(
        _data: &mut Self::Data,
        _buf: Option<&'a mut [u8]>,
    ) -> Result<Option<&'a mut [u8]>> {
        Ok(None)
    }

    /// Returns the superblock data to be used by this file system context.
    ///
    /// This is only needed when [`Type::SUPER_TYPE`] is [`Super::Keyed`], otherwise it is never
    /// called. In the former case, when the fs is being mounted, an existing superblock is reused
    /// if one can be found with the same data as the returned value; otherwise a new superblock is
    /// created.
    fn tree_key(_data: &mut Self::Data) -> Result<T::Data> {
        Err(ENOTSUPP)
    }
}

/// An empty file system context.
///
/// That is, one that doesn't take any arguments and doesn't hold any state. It is a convenience
/// type for file systems that don't need context for mounting/reconfiguring.
pub struct EmptyContext;

#[vtable]
impl<T: Type + ?Sized> Context<T> for EmptyContext {
    type Data = ();
    fn try_new() -> Result {
        Ok(())
    }
}

pub(crate) struct Tables<T: Type + ?Sized>(T);
impl<T: Type + ?Sized> Tables<T> {
    const CONTEXT: bindings::fs_context_operations = bindings::fs_context_operations {
        free: Some(Self::free_callback),
        parse_param: Some(Self::parse_param_callback),
        get_tree: Some(Self::get_tree_callback),
        reconfigure: Some(Self::reconfigure_callback),
        parse_monolithic: if <T::Context as Context<T>>::HAS_PARSE_MONOLITHIC {
            Some(Self::parse_monolithic_callback)
        } else {
            None
        },
        dup: None,
    };

    unsafe extern "C" fn free_callback(fc: *mut bindings::fs_context) {
        // SAFETY: The callback contract guarantees that `fc` is valid.
        let fc = unsafe { &*fc };

        let ptr = fc.fs_private;
        if !ptr.is_null() {
            // SAFETY: `fs_private` was initialised with the result of a `to_pointer` call in
            // `init_fs_context_callback`, so it's ok to call `from_foreign` here.
            unsafe { <T::Context as Context<T>>::Data::from_foreign(ptr) };
        }

        let ptr = fc.s_fs_info;
        if !ptr.is_null() {
            // SAFETY: `s_fs_info` may be initialised with the result of a `to_pointer` call in
            // `get_tree_callback` when keyed superblocks are used (`get_tree_keyed` sets it), so
            // it's ok to call `from_foreign` here.
            unsafe { T::Data::from_foreign(ptr) };
        }
    }

    unsafe extern "C" fn parse_param_callback(
        fc: *mut bindings::fs_context,
        param: *mut bindings::fs_parameter,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: The callback contract guarantees that `fc` is valid.
            let ptr = unsafe { (*fc).fs_private };

            // SAFETY: The value of `ptr` (coming from `fs_private` was initialised in
            // `init_fs_context_callback` to the result of an `into_foreign` call. Since the
            // context is valid, `from_foreign` wasn't called yet, so `ptr` is valid. Additionally,
            // the callback contract guarantees that callbacks are serialised, so it is ok to
            // mutably reference it.
            let mut data =
                unsafe { <<T::Context as Context<T>>::Data as ForeignOwnable>::borrow_mut(ptr) };
            let mut result = bindings::fs_parse_result::default();
            // SAFETY: All parameters are valid at least for the duration of the call.
            let opt =
                unsafe { bindings::fs_parse(fc, T::Context::PARAMS.first, param, &mut result) };

            // SAFETY: The callback contract guarantees that `param` is valid for the duration of
            // the callback.
            let param = unsafe { &*param };
            if opt >= 0 {
                let opt = opt as usize;
                if opt >= T::Context::PARAMS.handlers.len() {
                    return Err(EINVAL);
                }
                T::Context::PARAMS.handlers[opt].handle_param(&mut data, param, &result)?;
                return Ok(0);
            }

            if opt != ENOPARAM.to_errno() {
                return Err(Error::from_errno(opt));
            }

            if !T::Context::HAS_PARSE_UNKNOWN_PARAM {
                return Err(ENOPARAM);
            }

            let val = param::Value::from_fs_parameter(param);
            // SAFETY: The callback contract guarantees the parameter key to be valid and last at
            // least the duration of the callback.
            T::Context::parse_unknown_param(
                &mut data,
                unsafe { CStr::from_char_ptr(param.key) },
                val,
            )?;
            Ok(0)
        })
    }

    unsafe extern "C" fn fill_super_callback(
        sb_ptr: *mut bindings::super_block,
        fc: *mut bindings::fs_context,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: The callback contract guarantees that `fc` is valid. It also guarantees that
            // the callbacks are serialised for a given `fc`, so it is safe to mutably dereference
            // it.
            let fc = unsafe { &mut *fc };
            let ptr = core::mem::replace(&mut fc.fs_private, ptr::null_mut());

            // SAFETY: The value of `ptr` (coming from `fs_private` was initialised in
            // `init_fs_context_callback` to the result of an `into_foreign` call. The context is
            // being used to initialise a superblock, so we took over `ptr` (`fs_private` is set to
            // null now) and call `from_foreign` below.
            let data =
                unsafe { <<T::Context as Context<T>>::Data as ForeignOwnable>::from_foreign(ptr) };

            // SAFETY: The callback contract guarantees that `sb_ptr` is a unique pointer to a
            // newly-created superblock.
            let newsb = unsafe { NewSuperBlock::new(sb_ptr) };
            T::fill_super(data, newsb)?;
            Ok(0)
        })
    }

    unsafe extern "C" fn get_tree_callback(fc: *mut bindings::fs_context) -> core::ffi::c_int {
        // N.B. When new types are added below, we may need to update `kill_sb_callback` to ensure
        // that we're cleaning up properly.
        match T::SUPER_TYPE {
            Super::Single => unsafe {
                // SAFETY: `fc` is valid per the callback contract. `fill_super_callback` also has
                // the right type and is a valid callback.
                bindings::get_tree_single(fc, Some(Self::fill_super_callback))
            },
            Super::SingleReconf => unsafe {
                // SAFETY: `fc` is valid per the callback contract. `fill_super_callback` also has
                // the right type and is a valid callback.
                bindings::get_tree_single_reconf(fc, Some(Self::fill_super_callback))
            },
            Super::Independent => unsafe {
                // SAFETY: `fc` is valid per the callback contract. `fill_super_callback` also has
                // the right type and is a valid callback.
                bindings::get_tree_nodev(fc, Some(Self::fill_super_callback))
            },
            Super::BlockDev => unsafe {
                // SAFETY: `fc` is valid per the callback contract. `fill_super_callback` also has
                // the right type and is a valid callback.
                bindings::get_tree_bdev(fc, Some(Self::fill_super_callback))
            },
            Super::Keyed => {
                from_result(|| {
                    // SAFETY: `fc` is valid per the callback contract.
                    let ctx = unsafe { &*fc };
                    let ptr = ctx.fs_private;

                    // SAFETY: The value of `ptr` (coming from `fs_private` was initialised in
                    // `init_fs_context_callback` to the result of an `into_foreign` call. Since
                    // the context is valid, `from_foreign` wasn't called yet, so `ptr` is valid.
                    // Additionally, the callback contract guarantees that callbacks are
                    // serialised, so it is ok to mutably reference it.
                    let mut data = unsafe {
                        <<T::Context as Context<T>>::Data as ForeignOwnable>::borrow_mut(ptr)
                    };
                    let fs_data = T::Context::tree_key(&mut data)?;
                    let fs_data_ptr = fs_data.into_foreign();

                    // `get_tree_keyed` reassigns `ctx.s_fs_info`, which should be ok because
                    // nowhere else is it assigned a non-null value. However, we add the assert
                    // below to ensure that there are no unexpected paths on the C side that may do
                    // this.
                    assert_eq!(ctx.s_fs_info, core::ptr::null_mut());

                    // SAFETY: `fc` is valid per the callback contract. `fill_super_callback` also
                    // has the right type and is a valid callback. Lastly, we just called
                    // `into_foreign` above, so `fs_data_ptr` is also valid.
                    to_result(unsafe {
                        bindings::get_tree_keyed(
                            fc,
                            Some(Self::fill_super_callback),
                            fs_data_ptr as _,
                        )
                    })?;
                    Ok(0)
                })
            }
        }
    }

    unsafe extern "C" fn reconfigure_callback(_fc: *mut bindings::fs_context) -> core::ffi::c_int {
        EINVAL.to_errno()
    }

    unsafe extern "C" fn parse_monolithic_callback(
        fc: *mut bindings::fs_context,
        buf: *mut core::ffi::c_void,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: The callback contract guarantees that `fc` is valid.
            let ptr = unsafe { (*fc).fs_private };

            // SAFETY: The value of `ptr` (coming from `fs_private` was initialised in
            // `init_fs_context_callback` to the result of an `into_foreign` call. Since the
            // context is valid, `from_foreign` wasn't called yet, so `ptr` is valid. Additionally,
            // the callback contract guarantees that callbacks are serialised, so it is ok to
            // mutably reference it.
            let mut data =
                unsafe { <<T::Context as Context<T>>::Data as ForeignOwnable>::borrow_mut(ptr) };
            let page = if buf.is_null() {
                None
            } else {
                // SAFETY: This callback is called to handle the `mount` syscall, which takes a
                // page-sized buffer as data.
                Some(unsafe { &mut *ptr::slice_from_raw_parts_mut(buf.cast(), crate::PAGE_SIZE) })
            };
            let regular = T::Context::parse_monolithic(&mut data, page)?;
            if let Some(buf) = regular {
                // SAFETY: Both `fc` and `buf` are guaranteed to be valid; the former because the
                // callback is still ongoing and the latter because its lifefime is tied to that of
                // `page`, which is also valid for the duration of the callback.
                to_result(unsafe {
                    bindings::generic_parse_monolithic(fc, buf.as_mut_ptr().cast())
                })?;
            }
            Ok(0)
        })
    }

    pub(crate) const SUPER_BLOCK: bindings::super_operations = bindings::super_operations {
        alloc_inode: if size_of::<T::INodeData>() != 0 {
            Some(Self::alloc_inode_callback)
        } else {
            None
        },
        destroy_inode: None,
        free_inode: if size_of::<T::INodeData>() != 0 {
            Some(Self::free_inode_callback)
        } else {
            None
        },
        dirty_inode: None,
        write_inode: None,
        drop_inode: None,
        evict_inode: None,
        put_super: None,
        sync_fs: None,
        freeze_super: None,
        freeze_fs: None,
        thaw_super: None,
        unfreeze_fs: None,
        statfs: None,
        remount_fs: None,
        umount_begin: None,
        show_options: None,
        show_devname: None,
        show_path: None,
        show_stats: None,
        #[cfg(CONFIG_QUOTA)]
        quota_read: None,
        #[cfg(CONFIG_QUOTA)]
        quota_write: None,
        #[cfg(CONFIG_QUOTA)]
        get_dquots: None,
        nr_cached_objects: None,
        free_cached_objects: None,
        shutdown: None,
    };

    unsafe extern "C" fn alloc_inode_callback(
        sb: *mut bindings::super_block,
    ) -> *mut bindings::inode {
        // SAFETY: The callback contract guarantees that `sb` is valid for read.
        let super_type = unsafe { (*sb).s_type };

        // SAFETY: This callback is only used in `Registration`, so `super_type` is necessarily
        // embedded in a `Registration`, which is guaranteed to be valid because it has a
        // superblock associated to it.
        let reg = unsafe { &*container_of!(super_type, Registration, fs) };

        // SAFETY: `sb` and `reg.inode_cache` are guaranteed to be valid by the callback contract
        // and by the existence of a superblock respectively.
        let ptr = unsafe { bindings::alloc_inode_sb(sb, reg.inode_cache, bindings::GFP_KERNEL) }
            as *mut INodeWithData<T::INodeData>;
        if ptr.is_null() {
            return ptr::null_mut();
        }
        reg.alloc_count.fetch_add(1, Ordering::Relaxed);
        ptr::addr_of_mut!((*ptr).inode)
    }

    unsafe extern "C" fn free_inode_callback(inode: *mut bindings::inode) {
        // SAFETY: The inode is guaranteed to be valid by the callback contract. Additionally, the
        // superblock is also guaranteed to still be valid by the inode existence.
        let super_type = unsafe { (*(*inode).i_sb).s_type };

        // SAFETY: This callback is only used in `Registration`, so `super_type` is necessarily
        // embedded in a `Registration`, which is guaranteed to be valid because it has a
        // superblock associated to it.
        let reg = unsafe { &*container_of!(super_type, Registration, fs) };
        let ptr = container_of!(inode, INodeWithData<T::INodeData>, inode);

        // SAFETY: The code in `try_new_inode` always initialises the inode data after allocating
        // it, so it is safe to drop it here.
        unsafe {
            core::ptr::drop_in_place(
                (*(ptr as *mut INodeWithData<T::INodeData>))
                    .data
                    .as_mut_ptr(),
            )
        };

        // The callback contract guarantees that the inode was previously allocated via the
        // `alloc_inode_callback` callback, so it is safe to free it back to the cache.
        unsafe { bindings::kmem_cache_free(reg.inode_cache, ptr as _) };

        reg.alloc_count.fetch_sub(1, Ordering::Release);
    }
}

/// A file system type.
pub trait Type {
    /// The context used to build fs configuration before it is mounted or reconfigured.
    type Context: Context<Self> + ?Sized = EmptyContext;

    /// Type of data allocated for each inode.
    type INodeData: Send + Sync = ();

    /// Data associated with each file system instance.
    type Data: ForeignOwnable + Send + Sync = ();

    /// Determines whether the filesystem is based on the dcache.
    ///
    /// When this is `true`, adding a dentry results in an increased refcount. Removing them
    /// results in a matching decrement, and `kill_litter_super` is used when killing the
    /// superblock so that these extra references are removed.
    const DCACHE_BASED: bool = false;

    /// Determines how superblocks for this file system type are keyed.
    const SUPER_TYPE: Super;

    /// The name of the file system type.
    const NAME: &'static CStr;

    /// The flags of this file system type.
    ///
    /// It is a combination of the flags in the [`flags`] module.
    const FLAGS: i32;

    /// Initialises a super block for this file system type.
    fn fill_super(
        data: <Self::Context as Context<Self>>::Data,
        sb: NewSuperBlock<'_, Self>,
    ) -> Result<&SuperBlock<Self>>;
}

/// File system flags.
pub mod flags {
    use crate::bindings;

    /// The file system requires a device.
    pub const REQUIRES_DEV: i32 = bindings::FS_REQUIRES_DEV as _;

    /// The options provided when mounting are in binary form.
    pub const BINARY_MOUNTDATA: i32 = bindings::FS_BINARY_MOUNTDATA as _;

    /// The file system has a subtype. It is extracted from the name and passed in as a parameter.
    pub const HAS_SUBTYPE: i32 = bindings::FS_HAS_SUBTYPE as _;

    /// The file system can be mounted by userns root.
    pub const USERNS_MOUNT: i32 = bindings::FS_USERNS_MOUNT as _;

    /// Disables fanotify permission events.
    pub const DISALLOW_NOTIFY_PERM: i32 = bindings::FS_DISALLOW_NOTIFY_PERM as _;

    /// The file system has been updated to handle vfs idmappings.
    pub const ALLOW_IDMAP: i32 = bindings::FS_ALLOW_IDMAP as _;

    /// The file systen will handle `d_move` during `rename` internally.
    pub const RENAME_DOES_D_MOVE: i32 = bindings::FS_RENAME_DOES_D_MOVE as _;
}

/// A file system registration.
pub struct Registration {
    is_registered: bool,
    fs: Opaque<bindings::file_system_type>,
    inode_cache: *mut bindings::kmem_cache,
    alloc_count: AtomicU64,
    _pin: PhantomPinned,
}

// SAFETY: `Registration` doesn't really provide any `&self` methods, so it is safe to pass
// references to it around.
unsafe impl Sync for Registration {}

// SAFETY: Both registration and unregistration are implemented in C and safe to be performed from
// any thread, so `Registration` is `Send`.
unsafe impl Send for Registration {}

impl Registration {
    /// Creates a new file system registration.
    ///
    /// It is not visible or accessible yet. A successful call to [`Registration::register`] needs
    /// to be made before users can mount it.
    pub fn new() -> Self {
        Self {
            is_registered: false,
            fs: Opaque::new(bindings::file_system_type::default()),
            inode_cache: ptr::null_mut(),
            alloc_count: AtomicU64::new(0),
            _pin: PhantomPinned,
        }
    }

    /// Registers a file system so that it can be mounted by users.
    ///
    /// The file system is described by the [`Type`] argument.
    ///
    /// It is automatically unregistered when the registration is dropped.
    pub fn register<T: Type + ?Sized>(self: Pin<&mut Self>, module: &'static ThisModule) -> Result {
        // SAFETY: We never move out of `this`.
        let this = unsafe { self.get_unchecked_mut() };

        if this.is_registered {
            return Err(EINVAL);
        }

        if this.inode_cache.is_null() {
            let size = size_of::<T::INodeData>();
            if size != 0 {
                // We only create the cache if the size is non-zero.
                //
                // SAFETY: `NAME` is static, so always valid.
                this.inode_cache = unsafe {
                    bindings::kmem_cache_create(
                        T::NAME.as_char_ptr(),
                        size_of::<INodeWithData<T::INodeData>>() as _,
                        align_of::<INodeWithData<T::INodeData>>() as _,
                        bindings::SLAB_RECLAIM_ACCOUNT
                            | bindings::SLAB_MEM_SPREAD
                            | bindings::SLAB_ACCOUNT,
                        Some(Self::inode_init_once_callback::<T>),
                    )
                };
                if this.inode_cache.is_null() {
                    return Err(ENOMEM);
                }
            }
        }

        let mut fs = this.fs.get();
        // SAFETY: `fs` is valid as it points to the `self.fs`.
        unsafe {
            (*fs).owner = module.0;
            (*fs).name = T::NAME.as_char_ptr();
            (*fs).fs_flags = T::FLAGS;
            (*fs).init_fs_context = Some(Self::init_fs_context_callback::<T>);
            (*fs).kill_sb = Some(Self::kill_sb_callback::<T>);
        }

        // SAFETY: This block registers all fs type keys with lockdep. We just need the memory
        // locations to be owned by the caller, which is the case.
        unsafe {
            bindings::lockdep_register_key(&mut (*fs).s_lock_key);
            bindings::lockdep_register_key(&mut (*fs).s_umount_key);
            bindings::lockdep_register_key(&mut (*fs).s_vfs_rename_key);
            bindings::lockdep_register_key(&mut (*fs).i_lock_key);
            bindings::lockdep_register_key(&mut (*fs).i_mutex_key);
            bindings::lockdep_register_key(&mut (*fs).invalidate_lock_key);
            bindings::lockdep_register_key(&mut (*fs).i_mutex_dir_key);
            for key in &mut (*fs).s_writers_key {
                bindings::lockdep_register_key(key);
            }
        }

        let ptr = this.fs.get();

        // SAFETY: `ptr` is valid as it points to the `self.fs`.
        let key_guard = ScopeGuard::new(|| unsafe { Self::unregister_keys(ptr) });

        // SAFETY: Pointers stored in `fs` are either static so will live for as long as the
        // registration is active (it is undone in `drop`).
        to_result(unsafe { bindings::register_filesystem(ptr) })?;
        key_guard.dismiss();
        this.is_registered = true;
        Ok(())
    }

    /// Unregisters the lockdep keys in the file system type.
    ///
    /// # Safety
    ///
    /// `fs` must be non-null and valid.
    unsafe fn unregister_keys(fs: *mut bindings::file_system_type) {
        // SAFETY: This block unregisters all fs type keys from lockdep. They must have been
        // registered before.
        unsafe {
            bindings::lockdep_unregister_key(ptr::addr_of_mut!((*fs).s_lock_key));
            bindings::lockdep_unregister_key(ptr::addr_of_mut!((*fs).s_umount_key));
            bindings::lockdep_unregister_key(ptr::addr_of_mut!((*fs).s_vfs_rename_key));
            bindings::lockdep_unregister_key(ptr::addr_of_mut!((*fs).i_lock_key));
            bindings::lockdep_unregister_key(ptr::addr_of_mut!((*fs).i_mutex_key));
            bindings::lockdep_unregister_key(ptr::addr_of_mut!((*fs).invalidate_lock_key));
            bindings::lockdep_unregister_key(ptr::addr_of_mut!((*fs).i_mutex_dir_key));
            for i in 0..(*fs).s_writers_key.len() {
                bindings::lockdep_unregister_key(ptr::addr_of_mut!((*fs).s_writers_key[i]));
            }
        }
    }

    unsafe extern "C" fn init_fs_context_callback<T: Type + ?Sized>(
        fc_ptr: *mut bindings::fs_context,
    ) -> core::ffi::c_int {
        from_result(|| {
            let data = T::Context::try_new()?;
            // SAFETY: The callback contract guarantees that `fc_ptr` is the only pointer to a
            // newly-allocated fs context, so it is safe to mutably reference it.
            let fc = unsafe { &mut *fc_ptr };
            fc.fs_private = data.into_foreign() as _;
            fc.ops = &Tables::<T>::CONTEXT;
            Ok(0)
        })
    }

    unsafe extern "C" fn kill_sb_callback<T: Type + ?Sized>(sb_ptr: *mut bindings::super_block) {
        if let Super::BlockDev = T::SUPER_TYPE {
            // SAFETY: When the superblock type is `BlockDev`, we have a block device so it's safe
            // to call `kill_block_super`. Additionally, the callback contract guarantees that
            // `sb_ptr` is valid.
            unsafe { bindings::kill_block_super(sb_ptr) }
        } else if T::DCACHE_BASED {
            // SAFETY: We always call a `get_tree_nodev` variant from `get_tree_callback` without a
            // device when `T::SUPER_TYPE` is not `BlockDev`, so we never have a device in such
            // cases, therefore it is ok to call the function below. Additionally, the callback
            // contract guarantees that `sb_ptr` is valid, and we have all positive dentries biased
            // by +1 when `T::DCACHE_BASED`.
            unsafe { bindings::kill_litter_super(sb_ptr) }
        } else {
            // SAFETY: We always call a `get_tree_nodev` variant from `get_tree_callback` without a
            // device when `T::SUPER_TYPE` is not `BlockDev`, so we never have a device in such
            // cases, therefore it is ok to call the function below. Additionally, the callback
            // contract guarantees that `sb_ptr` is valid.
            unsafe { bindings::kill_anon_super(sb_ptr) }
        }

        // SAFETY: The callback contract guarantees that `sb_ptr` is valid.
        let sb = unsafe { &*sb_ptr };

        // SAFETY: The `kill_sb` callback being called implies that the `s_type` field is valid.
        unsafe { Self::unregister_keys(sb.s_type) };

        let ptr = sb.s_fs_info;
        if !ptr.is_null() {
            // SAFETY: The only place where `s_fs_info` is assigned is `NewSuperBlock::init`, where
            // it's initialised with the result of a `to_pointer` call. We checked above that ptr
            // is non-null because it would be null if we never reached the point where we init the
            // field.
            unsafe { T::Data::from_foreign(ptr) };
        }
    }

    unsafe extern "C" fn inode_init_once_callback<T: Type + ?Sized>(
        outer_inode: *mut core::ffi::c_void,
    ) {
        let ptr = outer_inode as *mut INodeWithData<T::INodeData>;
        // This is only used in `register`, so we know that we have a valid `INodeWithData`
        // instance whose inode part can be initialised.
        unsafe { bindings::inode_init_once(ptr::addr_of_mut!((*ptr).inode)) };
    }

    fn has_super_blocks(&self) -> bool {
        unsafe extern "C" fn fs_cb(_: *mut bindings::super_block, ptr: *mut core::ffi::c_void) {
            // SAFETY: This function is only called below, while `ptr` is known to `has_sb`.
            unsafe { *(ptr as *mut bool) = true };
        }

        let mut has_sb = false;
        // SAFETY: `fs` is valid, and `fs_cb` only touches `has_sb` during the call.
        unsafe {
            bindings::iterate_supers_type(self.fs.get(), Some(fs_cb), (&mut has_sb) as *mut _ as _)
        }
        has_sb
    }
}

impl Default for Registration {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for Registration {
    fn drop(&mut self) {
        if self.is_registered {
            // SAFETY: When `is_registered` is `true`, a previous call to `register_filesystem` has
            // succeeded, so it is safe to unregister here.
            unsafe { bindings::unregister_filesystem(self.fs.get()) };

            // TODO: Test this.
            if self.has_super_blocks() {
                // If there are mounted superblocks of this registration, we cannot release the
                // memory because it may be referenced, which would be a memory violation.
                pr_warn!(
                    "Attempting to unregister a file system (0x{:x}) with mounted super blocks\n",
                    self.fs.get() as usize
                );
                while self.has_super_blocks() {
                    pr_warn!("Sleeping 1s before retrying...\n");
                    coarse_sleep(Duration::from_secs(1));
                }
            }
        }

        if !self.inode_cache.is_null() {
            // Check if all inodes have been freed. If that's not the case, we may run into
            // user-after-frees of the registration and kmem cache, so wait for it to drop to zero
            // before proceeding.
            //
            // The expectation is that developers will fix this if they run into this warning.
            if self.alloc_count.load(Ordering::Acquire) > 0 {
                pr_warn!(
                    "Attempting to unregister a file system (0x{:x}) with allocated inodes\n",
                    self.fs.get() as usize
                );
                while self.alloc_count.load(Ordering::Acquire) > 0 {
                    pr_warn!("Sleeping 1s before retrying...\n");
                    coarse_sleep(Duration::from_secs(1));
                }
            }

            // SAFETY: Just an FFI call with no additional safety requirements.
            unsafe { bindings::rcu_barrier() };

            // SAFETY: We know there are no more allocations in this cache and that it won't be
            // used to allocate anymore because the filesystem is unregistered (so new mounts can't
            // be created) and there are no more superblocks nor inodes.
            //
            // TODO: Can a dentry keep a file system alive? It looks like the answer is yes because
            // it has a pointer to the superblock. How do we keep it alive? `d_init` may be an
            // option to increment some count.
            unsafe { bindings::kmem_cache_destroy(self.inode_cache) };
        }
    }
}

struct INodeWithData<T> {
    data: MaybeUninit<T>,
    inode: bindings::inode,
}

/// State of [`NewSuperBlock`] that indicates that [`NewSuperBlock::init`] needs to be called
/// eventually.
pub struct NeedsInit;

/// State of [`NewSuperBlock`] that indicates that [`NewSuperBlock::init_root`] needs to be called
/// eventually.
pub struct NeedsRoot;

/// Required superblock parameters.
///
/// This is used in [`NewSuperBlock::init`].
pub struct SuperParams {
    /// The magic number of the superblock.
    pub magic: u32,

    /// The size of a block in powers of 2 (i.e., for a value of `n`, the size is `2^n`.
    pub blocksize_bits: u8,

    /// Maximum size of a file.
    pub maxbytes: i64,

    /// Granularity of c/m/atime in ns (cannot be worse than a second).
    pub time_gran: u32,
}

impl SuperParams {
    /// Default value for instances of [`SuperParams`].
    pub const DEFAULT: Self = Self {
        magic: 0,
        blocksize_bits: crate::PAGE_SIZE as _,
        maxbytes: bindings::MAX_LFS_FILESIZE,
        time_gran: 1,
    };
}

/// A superblock that is still being initialised.
///
/// It uses type states to ensure that callers use the right sequence of calls.
///
/// # Invariants
///
/// The superblock is a newly-created one and this is the only active pointer to it.
pub struct NewSuperBlock<'a, T: Type + ?Sized, S = NeedsInit> {
    sb: &'a mut SuperBlock<T>,

    // This also forces `'a` to be invariant.
    _p: PhantomData<&'a mut &'a S>,
}

impl<'a, T: Type + ?Sized> NewSuperBlock<'a, T, NeedsInit> {
    /// Creates a new instance of [`NewSuperBlock`].
    ///
    /// # Safety
    ///
    /// `sb` must point to a newly-created superblock and it must be the only active pointer to it.
    unsafe fn new(sb: *mut bindings::super_block) -> Self {
        // INVARIANT: The invariants are satisfied by the safety requirements of this function.
        Self {
            // SAFETY: The safety requirements ensure that `sb` is valid for dereference.
            sb: unsafe { &mut *sb.cast() },
            _p: PhantomData,
        }
    }

    /// Initialises the superblock so that it transitions to the [`NeedsRoot`] type state.
    pub fn init(
        self,
        data: T::Data,
        params: &SuperParams,
    ) -> Result<NewSuperBlock<'a, T, NeedsRoot>> {
        let sb = self.sb.0.get();

        // SAFETY: `sb` is valid as it points to `self.sb`
        unsafe {
            (*sb).s_magic = params.magic as _;
            (*sb).s_op = &Tables::<T>::SUPER_BLOCK;
            (*sb).s_maxbytes = params.maxbytes;
            (*sb).s_time_gran = params.time_gran;
            (*sb).s_blocksize_bits = params.blocksize_bits;
            (*sb).s_blocksize = 1;
            if (*sb).s_blocksize.leading_zeros() < params.blocksize_bits.into() {
                return Err(EINVAL);
            }
            (*sb).s_blocksize = 1 << (*sb).s_blocksize_bits;
        }

        // Keyed file systems already have `s_fs_info` initialised.
        let info = data.into_foreign() as *mut _;
        if let Super::Keyed = T::SUPER_TYPE {
            // SAFETY: We just called `into_foreign` above.
            unsafe { T::Data::from_foreign(info) };

            // SAFETY: `sb` is valid as it points to `self.sb`
            unsafe {
                if (*sb).s_fs_info != info {
                    return Err(EINVAL);
                }
            }
        } else {
            // SAFETY: `sb` is valid as it points to `self.sb`
            unsafe {
                (*sb).s_fs_info = info;
            }
        }

        Ok(NewSuperBlock {
            sb: self.sb,
            _p: PhantomData,
        })
    }
}

impl<'a, T: Type + ?Sized> NewSuperBlock<'a, T, NeedsRoot> {
    /// Initialises the root of the superblock.
    pub fn init_root(self, dentry: RootDEntry<T>) -> Result<&'a SuperBlock<T>> {
        // SAFETY: `self.sb` is valid
        unsafe {
            (*self.sb.0.get()).s_root = ManuallyDrop::new(dentry).ptr;
        }
        Ok(self.sb)
    }

    fn populate_dir(
        &self,
        parent: &DEntry<T>,
        ino: &mut u64,
        entries: &[Entry<'_, T>],
        recursion: usize,
    ) -> Result
    where
        T::INodeData: Clone,
    {
        if recursion == 0 {
            return Err(E2BIG);
        }

        for e in entries {
            *ino += 1;
            match e {
                Entry::File(name, mode, value, inode_create) => {
                    let params = INodeParams {
                        mode: *mode,
                        ino: *ino,
                        value: value.clone(),
                    };
                    let inode = inode_create(self, params)?;
                    self.try_new_dentry(inode, parent, name)?;
                }
                Entry::Special(name, mode, value, typ, dev) => {
                    let params = INodeParams {
                        mode: *mode,
                        ino: *ino,
                        value: value.clone(),
                    };
                    let inode = self.sb.try_new_special_inode(*typ, *dev, params)?;
                    self.try_new_dentry(inode, parent, name)?;
                }
                Entry::Directory(name, mode, value, dir_entries) => {
                    let params = INodeParams {
                        mode: *mode,
                        ino: *ino,
                        value: value.clone(),
                    };
                    let inode = self.sb.try_new_dcache_dir_inode(params)?;
                    let new_parent = self.try_new_dentry(inode, parent, name)?;
                    self.populate_dir(&new_parent, ino, dir_entries, recursion - 1)?;
                }
            }
        }

        Ok(())
    }

    /// Creates a new root dentry populated with the given entries.
    pub fn try_new_populated_root_dentry(
        &self,
        root_value: T::INodeData,
        entries: &[Entry<'_, T>],
    ) -> Result<RootDEntry<T>>
    where
        T::INodeData: Clone,
    {
        let root_inode = self.sb.try_new_dcache_dir_inode(INodeParams {
            mode: 0o755,
            ino: 1,
            value: root_value,
        })?;
        let root = self.try_new_root_dentry(root_inode)?;
        let mut ino = 1u64;
        self.populate_dir(&root, &mut ino, entries, 10)?;
        Ok(root)
    }

    /// Creates a new empty root dentry.
    pub fn try_new_root_dentry(&self, inode: ARef<INode<T>>) -> Result<RootDEntry<T>> {
        // SAFETY: The inode is referenced, so it is safe to read the read-only field `i_sb`.
        if unsafe { (*inode.0.get()).i_sb } != self.sb.0.get() {
            return Err(EINVAL);
        }

        // SAFETY: The caller owns a reference to the inode, so it is valid. The reference is
        // transferred to the callee.
        let dentry =
            ptr::NonNull::new(unsafe { bindings::d_make_root(ManuallyDrop::new(inode).0.get()) })
                .ok_or(ENOMEM)?;
        Ok(RootDEntry {
            ptr: dentry.as_ptr(),
            _p: PhantomData,
        })
    }

    /// Creates a new dentry with the given name, under the given parent, and backed by the given
    /// inode.
    pub fn try_new_dentry(
        &self,
        inode: ARef<INode<T>>,
        parent: &DEntry<T>,
        name: &CStr,
    ) -> Result<ARef<DEntry<T>>> {
        // SAFETY: Both `inode` and `parent` are referenced, so it is safe to read the read-only
        // fields `i_sb` and `d_sb`.
        if unsafe { (*parent.0.get()).d_sb } != self.sb.0.get()
            || unsafe { (*inode.0.get()).i_sb } != self.sb.0.get()
        {
            return Err(EINVAL);
        }

        // SAFETY: `parent` is valid (we have a shared reference to it), and `name` is valid for
        // the duration of the call (the callee makes a copy of the name).
        let dentry = ptr::NonNull::new(unsafe {
            bindings::d_alloc_name(parent.0.get(), name.as_char_ptr())
        })
        .ok_or(ENOMEM)?;

        // SAFETY: `dentry` was just allocated so it is valid. The callee takes over the reference
        // to the inode.
        unsafe { bindings::d_add(dentry.as_ptr(), ManuallyDrop::new(inode).0.get()) };

        // SAFETY: `dentry` was just allocated, and the caller holds a reference, which it
        // transfers to `dref`.
        let dref = unsafe { ARef::from_raw(dentry.cast::<DEntry<T>>()) };

        if T::DCACHE_BASED {
            // Bias the refcount by +1 when adding a positive dentry.
            core::mem::forget(dref.clone());
        }

        Ok(dref)
    }

    /// Creates a new inode that is a directory.
    ///
    /// The directory is based on the dcache, implemented by `simple_dir_operations` and
    /// `simple_dir_inode_operations`.
    pub fn try_new_dcache_dir_inode(
        &self,
        params: INodeParams<T::INodeData>,
    ) -> Result<ARef<INode<T>>> {
        self.sb.try_new_dcache_dir_inode(params)
    }

    /// Creates a new "special" inode.
    pub fn try_new_special_inode(
        &self,
        typ: INodeSpecialType,
        rdev: Option<u32>,
        params: INodeParams<T::INodeData>,
    ) -> Result<ARef<INode<T>>> {
        self.sb.try_new_special_inode(typ, rdev, params)
    }

    /// Creates a new regular file inode.
    pub fn try_new_file_inode<F: file::Operations<OpenData = T::INodeData>>(
        &self,
        params: INodeParams<T::INodeData>,
    ) -> Result<ARef<INode<T>>> {
        self.sb.try_new_file_inode::<F>(params)
    }
}

/// The type of a special inode.
///
/// This is used in functions like [`SuperBlock::try_new_special_inode`] to specify the type of
/// an special inode; in this example, it's for it to be created.
#[derive(Clone, Copy)]
#[repr(u16)]
pub enum INodeSpecialType {
    /// Character device.
    Char = bindings::S_IFCHR as _,

    /// Block device.
    Block = bindings::S_IFBLK as _,

    /// A pipe (FIFO, first-in first-out) inode.
    Fifo = bindings::S_IFIFO as _,

    /// A unix-domain socket.
    Sock = bindings::S_IFSOCK as _,
}

/// Required inode parameters.
///
/// This is used when creating new inodes.
pub struct INodeParams<T> {
    /// The access mode. It's a mask that grants execute (1), write (2) and read (4) access to
    /// everyone, the owner group, and the owner.
    pub mode: u16,

    /// Number of the inode.
    pub ino: u64,

    /// Value to attach to this node.
    pub value: T,
}

struct FsAdapter<T: Type + ?Sized>(PhantomData<T>);
impl<T: Type + ?Sized> file::OpenAdapter<T::INodeData> for FsAdapter<T> {
    unsafe fn convert(
        inode: *mut bindings::inode,
        _file: *mut bindings::file,
    ) -> *const T::INodeData {
        let ptr = container_of!(inode, INodeWithData<T::INodeData>, inode);
        // SAFETY: Add safety annotation.
        let outer = unsafe { &*ptr };
        outer.data.as_ptr()
    }
}

/// A file system super block.
///
/// Wraps the kernel's `struct super_block`.
#[repr(transparent)]
pub struct SuperBlock<T: Type + ?Sized>(pub(crate) Opaque<bindings::super_block>, PhantomData<T>);

impl<T: Type + ?Sized> SuperBlock<T> {
    fn try_new_inode(
        &self,
        mode_type: u16,
        params: INodeParams<T::INodeData>,
        init: impl FnOnce(&mut bindings::inode),
    ) -> Result<ARef<INode<T>>> {
        // SAFETY: `sb` is initialised (`NeedsRoot` typestate implies it), so it is safe to pass it
        // to `new_inode`.
        let inode =
            ptr::NonNull::new(unsafe { bindings::new_inode(self.0.get()) }).ok_or(ENOMEM)?;

        {
            let ptr = container_of!(inode.as_ptr(), INodeWithData<T::INodeData>, inode);

            // SAFETY: This is a newly-created inode. No other references to it exist, so it is
            // safe to mutably dereference it.
            let outer = unsafe { &mut *(ptr as *mut INodeWithData<T::INodeData>) };

            // N.B. We must always write this to a newly allocated inode because the free callback
            // expects the data to be initialised and drops it.
            outer.data.write(params.value);

            // SAFETY: `current_time` requires that `inode.sb` be valid, which is the case here
            // since we allocated the inode through the superblock.
            let time = unsafe { bindings::current_time(&mut outer.inode) };
            outer.inode.i_mtime = time;
            outer.inode.i_atime = time;
            outer.inode.i_ctime = time;

            outer.inode.i_ino = params.ino;
            outer.inode.i_mode = params.mode & 0o777 | mode_type;

            init(&mut outer.inode);
        }

        // SAFETY: `inode` only has one reference, and it's being relinquished to the `ARef`
        // instance.
        Ok(unsafe { ARef::from_raw(inode.cast()) })
    }

    /// Creates a new inode that is a directory.
    ///
    /// The directory is based on the dcache, implemented by `simple_dir_operations` and
    /// `simple_dir_inode_operations`.
    pub fn try_new_dcache_dir_inode(
        &self,
        params: INodeParams<T::INodeData>,
    ) -> Result<ARef<INode<T>>> {
        self.try_new_inode(bindings::S_IFDIR as _, params, |inode| {
            // SAFETY: `simple_dir_operations` never changes, it's safe to reference it.
            inode.__bindgen_anon_3.i_fop = unsafe { &bindings::simple_dir_operations };

            // SAFETY: `simple_dir_inode_operations` never changes, it's safe to reference it.
            inode.i_op = unsafe { &bindings::simple_dir_inode_operations };

            // Directory inodes start off with i_nlink == 2 (for "." entry).
            // SAFETY: `inode` is valid for write.
            unsafe { bindings::inc_nlink(inode) };
        })
    }

    /// Creates a new "special" inode.
    pub fn try_new_special_inode(
        &self,
        typ: INodeSpecialType,
        rdev: Option<u32>,
        params: INodeParams<T::INodeData>,
    ) -> Result<ARef<INode<T>>> {
        // SAFETY: `inode` is valid as it's a mutable reference.
        self.try_new_inode(typ as _, params, |inode| unsafe {
            bindings::init_special_inode(inode, inode.i_mode, rdev.unwrap_or(0))
        })
    }

    /// Creates a new regular file inode.
    pub fn try_new_file_inode<F: file::Operations<OpenData = T::INodeData>>(
        &self,
        params: INodeParams<T::INodeData>,
    ) -> Result<ARef<INode<T>>> {
        self.try_new_inode(bindings::S_IFREG as _, params, |inode| {
            // SAFETY: The adapter is compatible because it assumes an inode created by a `T` file
            // system, which is the case here.
            inode.__bindgen_anon_3.i_fop =
                unsafe { file::OperationsVtable::<FsAdapter<T>, F>::build() };
        })
    }
}

/// Wraps the kernel's `struct inode`.
///
/// # Invariants
///
/// Instances of this type are always ref-counted, that is, a call to `ihold` ensures that the
/// allocation remains valid at least until the matching call to `iput`.
#[repr(transparent)]
pub struct INode<T: Type + ?Sized>(pub(crate) Opaque<bindings::inode>, PhantomData<T>);

impl<T: Type + ?Sized> INode<T> {
    /// Returns the file-system-determined data associated with the inode.
    pub fn fs_data(&self) -> &T::INodeData {
        let ptr = container_of!(self.0.get(), INodeWithData<T::INodeData>, inode);
        // SAFETY: Add safety annotation.
        unsafe { (*ptr::addr_of!((*ptr).data)).assume_init_ref() }
    }
}

// SAFETY: The type invariants guarantee that `INode` is always ref-counted.
unsafe impl<T: Type + ?Sized> AlwaysRefCounted for INode<T> {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference means that the refcount is nonzero.
        unsafe { bindings::ihold(self.0.get()) };
    }

    unsafe fn dec_ref(obj: ptr::NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is nonzero.
        unsafe { bindings::iput(obj.cast().as_ptr()) }
    }
}

/// Wraps the kernel's `struct dentry`.
///
/// # Invariants
///
/// Instances of this type are always ref-counted, that is, a call to `dget` ensures that the
/// allocation remains valid at least until the matching call to `dput`.
#[repr(transparent)]
pub struct DEntry<T: Type + ?Sized>(pub(crate) Opaque<bindings::dentry>, PhantomData<T>);

// SAFETY: The type invariants guarantee that `DEntry` is always ref-counted.
unsafe impl<T: Type + ?Sized> AlwaysRefCounted for DEntry<T> {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference means that the refcount is nonzero.
        unsafe { bindings::dget(self.0.get()) };
    }

    unsafe fn dec_ref(obj: ptr::NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is nonzero.
        unsafe { bindings::dput(obj.cast().as_ptr()) }
    }
}

/// A dentry that is meant to be used as the root of a file system.
///
/// We have a specific type for the root dentry because we may need to do extra work when it is
/// dropped. For example, if [`Type::DCACHE_BASED`] is `true`, we need to remove the extra
/// reference held on each child dentry.
///
/// # Invariants
///
/// `ptr` is always valid and ref-counted.
pub struct RootDEntry<T: Type + ?Sized> {
    ptr: *mut bindings::dentry,
    _p: PhantomData<T>,
}

impl<T: Type + ?Sized> Deref for RootDEntry<T> {
    type Target = DEntry<T>;

    fn deref(&self) -> &Self::Target {
        // SAFETY: Add safety annotation.
        unsafe { &*self.ptr.cast() }
    }
}

impl<T: Type + ?Sized> Drop for RootDEntry<T> {
    fn drop(&mut self) {
        if T::DCACHE_BASED {
            // All dentries have an extra ref on them, so we use `d_genocide` to drop it.
            // SAFETY: Add safety annotation.
            unsafe { bindings::d_genocide(self.ptr) };

            // SAFETY: Add safety annotation.
            unsafe { bindings::shrink_dcache_parent(self.ptr) };
        }

        // SAFETY: Add safety annotation.
        unsafe { bindings::dput(self.ptr) };
    }
}

/// Wraps the kernel's `struct filename`.
#[repr(transparent)]
pub struct Filename(pub(crate) Opaque<bindings::filename>);

impl Filename {
    /// Creates a reference to a [`Filename`] from a valid pointer.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `ptr` is valid and remains valid for the lifetime of the
    /// returned [`Filename`] instance.
    pub(crate) unsafe fn from_ptr<'a>(ptr: *const bindings::filename) -> &'a Filename {
        // SAFETY: The safety requirements guarantee the validity of the dereference, while the
        // `Filename` type being transparent makes the cast ok.
        unsafe { &*ptr.cast() }
    }
}

/// Kernel module that exposes a single file system implemented by `T`.
pub struct Module<T: Type> {
    _fs: Pin<Box<Registration>>,
    _p: PhantomData<T>,
}

impl<T: Type + Sync> crate::Module for Module<T> {
    fn init(_name: &'static CStr, module: &'static ThisModule) -> Result<Self> {
        let mut reg = Pin::from(Box::try_new(Registration::new())?);
        reg.as_mut().register::<T>(module)?;
        Ok(Self {
            _fs: reg,
            _p: PhantomData,
        })
    }
}

/// Returns a device id from its major and minor components.
pub const fn mkdev(major: u16, minor: u32) -> u32 {
    (major as u32) << bindings::MINORBITS | minor
}

/// Declares a kernel module that exposes a single file system.
///
/// The `type` argument must be a type which implements the [`Type`] trait. Also accepts various
/// forms of kernel metadata.
///
/// # Examples
///
/// ```ignore
/// use kernel::prelude::*;
/// use kernel::{c_str, fs};
///
/// module_fs! {
///     type: MyFs,
///     name: b"my_fs_kernel_module",
///     author: b"Rust for Linux Contributors",
///     description: b"My very own file system kernel module!",
///     license: b"GPL",
/// }
///
/// struct MyFs;
///
/// impl fs::Type for MyFs {
///     const SUPER_TYPE: fs::Super = fs::Super::Independent;
///     const NAME: &'static CStr = c_str!("example");
///     const FLAGS: i32 = 0;
///
///     fn fill_super(_data: (), sb: fs::NewSuperBlock<'_, Self>) -> Result<&fs::SuperBlock<Self>> {
///         let sb = sb.init(
///             (),
///             &fs::SuperParams {
///                 magic: 0x6578616d,
///                 ..fs::SuperParams::DEFAULT
///             },
///         )?;
///         let root_inode = sb.try_new_dcache_dir_inode(fs::INodeParams {
///             mode: 0o755,
///             ino: 1,
///             value: (),
///         })?;
///         let root = sb.try_new_root_dentry(root_inode)?;
///         let sb = sb.init_root(root)?;
///         Ok(sb)
///     }
/// }
/// ```
#[macro_export]
macro_rules! module_fs {
    (type: $type:ty, $($f:tt)*) => {
        type ModuleType = $crate::fs::Module<$type>;
        $crate::macros::module! {
            type: ModuleType,
            $($f)*
        }
    }
}

/// Defines a slice of file system entries.
///
/// This is meant as a helper for the definition of file system entries in a more compact form than
/// if declared directly using the types.
///
/// # Examples
///
/// ```
/// # use kernel::prelude::*;
/// use kernel::{c_str, file, fs};
///
/// struct MyFs;
///
/// impl fs::Type for MyFs {
///     type INodeData = &'static [u8];
///
///     // ...
/// #    const SUPER_TYPE: fs::Super = fs::Super::Independent;
/// #    const NAME: &'static CStr = c_str!("example");
/// #    const FLAGS: i32 = fs::flags::USERNS_MOUNT;
/// #    const DCACHE_BASED: bool = true;
/// #
/// #    fn fill_super(_: (), _: fs::NewSuperBlock<'_, Self>) -> Result<&fs::SuperBlock<Self>> {
/// #        todo!()
/// #    }
/// }
///
/// struct MyFile;
///
/// #[vtable]
/// impl file::Operations for MyFile {
///     type OpenData = &'static [u8];
///
///     // ...
/// #    fn open(_context: &Self::OpenData, _file: &file::File) -> Result<Self::Data> {
/// #        Ok(())
/// #    }
/// }
///
/// const ENTRIES: &[fs::Entry<'_, MyFs>] = kernel::fs_entries![
///     file("test1", 0o600, "abc\n".as_bytes(), MyFile),
///     file("test2", 0o600, "def\n".as_bytes(), MyFile),
///     char("test3", 0o600, [].as_slice(), (10, 125)),
///     sock("test4", 0o755, [].as_slice()),
///     fifo("test5", 0o755, [].as_slice()),
///     block("test6", 0o755, [].as_slice(), (1, 1)),
///     dir(
///         "dir1",
///         0o755,
///         [].as_slice(),
///         [
///             file("test1", 0o600, "abc\n".as_bytes(), MyFile),
///             file("test2", 0o600, "def\n".as_bytes(), MyFile),
///         ],
///     ),
/// ];
/// ```
#[macro_export]
macro_rules! fs_entries {
    ($($kind:ident ($($t:tt)*)),* $(,)?) => {
        &[
            $($crate::fs_entries!(@single $kind($($t)*)),)*
        ]
    };
    (@single file($name:literal, $mode:expr, $value:expr, $file_ops:ty $(,)?)) => {
        $crate::fs::Entry::File(
            $crate::c_str!($name),
            $mode,
            $value,
            $crate::fs::file_creator::<_, $file_ops>(),
        )
    };
    (@single dir($name:literal, $mode:expr, $value:expr, [$($t:tt)*] $(,)?)) => {
        $crate::fs::Entry::Directory(
            $crate::c_str!($name),
            $mode,
            $value,
            $crate::fs_entries!($($t)*),
        )
    };
    (@single nod($name:literal, $mode:expr, $value:expr, $nod_type:ident, $dev:expr $(,)?)) => {
        $crate::fs::Entry::Special(
            $crate::c_str!($name),
            $mode,
            $value,
            $crate::fs::INodeSpecialType::$nod_type,
            $dev,
        )
    };
    (@single char($name:literal, $mode:expr, $value:expr, ($major:expr, $minor:expr) $(,)?)) => {
        $crate::fs_entries!(
            @single nod($name, $mode, $value, Char, Some($crate::fs::mkdev($major, $minor))))
    };
    (@single block($name:literal, $mode:expr, $value:expr, ($major:expr, $minor:expr) $(,)?)) => {
        $crate::fs_entries!(
            @single nod($name, $mode, $value, Block, Some($crate::fs::mkdev($major, $minor))))
    };
    (@single sock($name:literal, $mode:expr, $value:expr $(,)?)) => {
        $crate::fs_entries!(@single nod($name, $mode, $value, Sock, None))
    };
    (@single fifo($name:literal, $mode:expr, $value:expr $(,)?)) => {
        $crate::fs_entries!(@single nod($name, $mode, $value, Fifo, None))
    };
}

/// A file system entry.
///
/// This is used statically describe the files and directories of a file system in functions that
/// take such data as arguments, for example, [`NewSuperBlock::try_new_populated_root_dentry`].
pub enum Entry<'a, T: Type + ?Sized> {
    /// A regular file.
    File(&'a CStr, u16, T::INodeData, INodeCreator<T>),

    /// A directory and its children.
    Directory(&'a CStr, u16, T::INodeData, &'a [Entry<'a, T>]),

    /// A special file, the type of which is given by [`INodeSpecialType`].
    Special(&'a CStr, u16, T::INodeData, INodeSpecialType, Option<u32>),
}

/// A function that creates and inode.
pub type INodeCreator<T> = fn(
    &NewSuperBlock<'_, T, NeedsRoot>,
    INodeParams<<T as Type>::INodeData>,
) -> Result<ARef<INode<T>>>;

/// Returns an [`INodeCreator`] that creates a regular file with the given file operations.
///
/// This is used by the [`fs_entries`] macro to elide the type implementing the [`file::Operations`]
/// trait.
pub const fn file_creator<T: Type + ?Sized, F: file::Operations<OpenData = T::INodeData>>(
) -> INodeCreator<T> {
    fn file_creator<T: Type + ?Sized, F: file::Operations<OpenData = T::INodeData>>(
        new_sb: &NewSuperBlock<'_, T, NeedsRoot>,
        params: INodeParams<T::INodeData>,
    ) -> Result<ARef<INode<T>>> {
        new_sb.sb.try_new_file_inode::<F>(params)
    }
    file_creator::<T, F>
}
