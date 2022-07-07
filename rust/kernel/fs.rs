// SPDX-License-Identifier: GPL-2.0

//! File systems.
//!
//! C headers: [`include/linux/fs.h`](../../../../include/linux/fs.h)

use crate::error::{from_result, to_result, Result};
use crate::types::{AlwaysRefCounted, ForeignOwnable, Opaque, ScopeGuard};
use crate::{bindings, error::code::*, str::CStr, ThisModule};
use core::{marker::PhantomPinned, pin::Pin, ptr};
use macros::vtable;

/// A file system context.
///
/// It is used to gather configuration to then mount or reconfigure a file system.
#[vtable]
pub trait Context<T: Type + ?Sized> {
    /// Type of the data associated with the context.
    type Data: ForeignOwnable + Send + Sync + 'static;

    /// Creates a new context.
    fn try_new() -> Result<Self::Data>;
}

struct Tables<T: Type + ?Sized>(T);
impl<T: Type + ?Sized> Tables<T> {
    const CONTEXT: bindings::fs_context_operations = bindings::fs_context_operations {
        free: Some(Self::free_callback),
        parse_param: None,
        get_tree: Some(Self::get_tree_callback),
        reconfigure: Some(Self::reconfigure_callback),
        parse_monolithic: None,
        dup: None,
    };

    unsafe extern "C" fn free_callback(fc: *mut bindings::fs_context) {
        // SAFETY: The callback contract guarantees that `fc` is valid.
        let ptr = unsafe { (*fc).fs_private };
        if !ptr.is_null() {
            // SAFETY: `fs_private` was initialised with the result of a `to_pointer` call in
            // `init_fs_context_callback`, so it's ok to call `from_foreign` here.
            unsafe { <T::Context as Context<T>>::Data::from_foreign(ptr) };
        }
    }

    unsafe extern "C" fn fill_super_callback(
        sb_ptr: *mut bindings::super_block,
        _fc: *mut bindings::fs_context,
    ) -> core::ffi::c_int {
        from_result(|| {
            // The following is temporary code to create the root inode and dentry. It will be
            // replaced with calls to Rust code.

            // SAFETY: The callback contract guarantees that `sb_ptr` is the only pointer to a
            // newly-allocated superblock, so it is safe to mutably reference it.
            let sb = unsafe { &mut *sb_ptr };

            sb.s_maxbytes = bindings::MAX_LFS_FILESIZE;
            sb.s_blocksize = crate::PAGE_SIZE as _;
            sb.s_blocksize_bits = bindings::PAGE_SHIFT as _;
            sb.s_magic = T::MAGIC as _;
            sb.s_op = &Tables::<T>::SUPER_BLOCK;
            sb.s_time_gran = 1;

            // Create and initialise the root inode.
            let inode = unsafe { bindings::new_inode(sb) };
            if inode.is_null() {
                return Err(ENOMEM);
            }

            {
                // SAFETY: This is a newly-created inode. No other references to it exist, so it is
                // safe to mutably dereference it.
                let inode = unsafe { &mut *inode };

                // SAFETY: `current_time` requires that `inode.sb` be valid, which is the case here
                // since we allocated the inode through the superblock.
                let time = unsafe { bindings::current_time(inode) };
                inode.i_ino = 1;
                inode.i_mode = (bindings::S_IFDIR | 0o755) as _;
                inode.i_mtime = time;
                inode.i_atime = time;
                inode.i_ctime = time;

                // SAFETY: `simple_dir_operations` never changes, it's safe to reference it.
                inode.__bindgen_anon_3.i_fop = unsafe { &bindings::simple_dir_operations };

                // SAFETY: `simple_dir_inode_operations` never changes, it's safe to reference it.
                inode.i_op = unsafe { &bindings::simple_dir_inode_operations };

                // SAFETY: `inode` is valid for write.
                unsafe { bindings::set_nlink(inode, 2) };
            }

            // SAFETY: `d_make_root` requires that `inode` be valid and referenced, which is the
            // case for this call.
            //
            // It takes over the inode, even on failure, so we don't need to clean it up.
            let dentry = unsafe { bindings::d_make_root(inode) };
            if dentry.is_null() {
                return Err(ENOMEM);
            }

            sb.s_root = dentry;
            Ok(0)
        })
    }

    unsafe extern "C" fn get_tree_callback(fc: *mut bindings::fs_context) -> core::ffi::c_int {
        // SAFETY: `fc` is valid per the callback contract. `fill_super_callback` also has the
        // right type and is a valid callback.
        unsafe { bindings::get_tree_nodev(fc, Some(Self::fill_super_callback)) }
    }

    unsafe extern "C" fn reconfigure_callback(_fc: *mut bindings::fs_context) -> core::ffi::c_int {
        EINVAL.to_errno()
    }

    const SUPER_BLOCK: bindings::super_operations = bindings::super_operations {
        alloc_inode: None,
        destroy_inode: None,
        free_inode: None,
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
}

/// A file system type.
pub trait Type {
    /// The context used to build fs configuration before it is mounted or reconfigured.
    type Context: Context<Self> + ?Sized;

    /// The name of the file system type.
    const NAME: &'static CStr;

    /// The magic number associated with the file system.
    ///
    /// This is normally one of the values in `include/uapi/linux/magic.h`.
    const MAGIC: u32;

    /// The flags of this file system type.
    ///
    /// It is a combination of the flags in the [`flags`] module.
    const FLAGS: i32;
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
        // SAFETY: We always call `get_tree_nodev` from `get_tree_callback`, so we never have a
        // device, so it is ok to call the function below. Additionally, the callback contract
        // guarantees that `sb_ptr` is valid.
        unsafe { bindings::kill_anon_super(sb_ptr) }

        // SAFETY: The callback contract guarantees that `sb_ptr` is valid, and the `kill_sb`
        // callback being called implies that the `s_type` is also valid.
        unsafe { Self::unregister_keys((*sb_ptr).s_type) };
    }
}

impl Drop for Registration {
    fn drop(&mut self) {
        if self.is_registered {
            // SAFETY: When `is_registered` is `true`, a previous call to `register_filesystem` has
            // succeeded, so it is safe to unregister here.
            unsafe { bindings::unregister_filesystem(self.fs.get()) };
        }
    }
}

/// Wraps the kernel's `struct inode`.
///
/// # Invariants
///
/// Instances of this type are always ref-counted, that is, a call to `ihold` ensures that the
/// allocation remains valid at least until the matching call to `iput`.
#[repr(transparent)]
pub struct INode(pub(crate) Opaque<bindings::inode>);

// SAFETY: The type invariants guarantee that `INode` is always ref-counted.
unsafe impl AlwaysRefCounted for INode {
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
pub struct DEntry(pub(crate) Opaque<bindings::dentry>);

// SAFETY: The type invariants guarantee that `DEntry` is always ref-counted.
unsafe impl AlwaysRefCounted for DEntry {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference means that the refcount is nonzero.
        unsafe { bindings::dget(self.0.get()) };
    }

    unsafe fn dec_ref(obj: ptr::NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is nonzero.
        unsafe { bindings::dput(obj.cast().as_ptr()) }
    }
}
