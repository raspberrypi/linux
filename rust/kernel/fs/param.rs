// SPDX-License-Identifier: GPL-2.0

//! File system parameters and parsing them.
//!
//! C headers: [`include/linux/fs_parser.h`](../../../../../include/linux/fs_parser.h)

use crate::{bindings, error::Result, file, fs, str::CStr};
use core::{marker::PhantomData, ptr};

/// The value of a file system parameter.
pub enum Value<'a> {
    /// The value is undefined.
    Undefined,

    /// There is no value, but parameter itself is a flag.
    Flag,

    /// The value is the given string.
    String(&'a CStr),

    /// The value is the given binary blob.
    Blob(&'a mut [u8]),

    /// The value is the given file.
    File(&'a file::File),

    /// The value is the given filename and the given directory file descriptor (which may be
    /// `AT_FDCWD`, to indicate the current directory).
    Filename(&'a fs::Filename, i32),
}

impl<'a> Value<'a> {
    pub(super) fn from_fs_parameter(p: &'a bindings::fs_parameter) -> Self {
        match p.type_() {
            bindings::fs_value_type_fs_value_is_string => {
                // SAFETY: `type_` is string, so it is ok to use the union field. Additionally, it
                // is guaranteed to be valid while `p` is valid.
                Self::String(unsafe { CStr::from_char_ptr(p.__bindgen_anon_1.string) })
            }
            bindings::fs_value_type_fs_value_is_flag => Self::Flag,
            bindings::fs_value_type_fs_value_is_blob => {
                // SAFETY: `type_` is blob, so it is ok to use the union field and size.
                // Additionally, it is guaranteed to be valid while `p` is valid.
                let slice = unsafe {
                    &mut *ptr::slice_from_raw_parts_mut(p.__bindgen_anon_1.blob.cast(), p.size)
                };
                Self::Blob(slice)
            }
            bindings::fs_value_type_fs_value_is_file => {
                // SAFETY: `type_` is file, so it is ok to use the union field. Additionally, it is
                // guaranteed to be valid while `p` is valid.
                let file_ptr = unsafe { p.__bindgen_anon_1.file };
                if file_ptr.is_null() {
                    Self::Undefined
                } else {
                    // SAFETY: `file_ptr` is non-null and guaranteed to be valid while `p` is.
                    Self::File(unsafe { file::File::from_ptr(file_ptr) })
                }
            }
            bindings::fs_value_type_fs_value_is_filename => {
                // SAFETY: `type_` is filename, so it is ok to use the union field. Additionally,
                // it is guaranteed to be valid while `p` is valid.
                let filename_ptr = unsafe { p.__bindgen_anon_1.name };
                if filename_ptr.is_null() {
                    Self::Undefined
                } else {
                    // SAFETY: `filename_ptr` is non-null and guaranteed to be valid while `p` is.
                    Self::Filename(unsafe { fs::Filename::from_ptr(filename_ptr) }, p.dirfd)
                }
            }
            _ => Self::Undefined,
        }
    }
}

/// A specification of a file system parameter.
pub struct Spec {
    name: &'static CStr,
    flags: u16,
    type_: bindings::fs_param_type,
    extra: *const core::ffi::c_void,
}

const DEFAULT: Spec = Spec {
    name: crate::c_str!(""),
    flags: 0,
    type_: None,
    extra: core::ptr::null(),
};

macro_rules! define_param_type {
    ($name:ident, $fntype:ty, $spec:expr, |$param:ident, $result:ident| $value:expr) => {
        /// Module to support `$name` parameter types.
        pub mod $name {
            use super::*;

            #[doc(hidden)]
            pub const fn spec(name: &'static CStr) -> Spec {
                const GIVEN: Spec = $spec;
                Spec { name, ..GIVEN }
            }

            #[doc(hidden)]
            pub const fn handler<S>(setfn: fn(&mut S, $fntype) -> Result) -> impl Handler<S> {
                let c =
                    move |s: &mut S,
                          $param: &bindings::fs_parameter,
                          $result: &bindings::fs_parse_result| { setfn(s, $value) };
                ConcreteHandler {
                    setfn: c,
                    _p: PhantomData,
                }
            }
        }
    };
}

// SAFETY: This is only called when the parse result is a boolean, so it is ok to access to union
// field.
define_param_type!(flag, bool, Spec { ..DEFAULT }, |_p, r| unsafe {
    r.__bindgen_anon_1.boolean
});

define_param_type!(
    flag_no,
    bool,
    Spec {
        flags: bindings::fs_param_neg_with_no as _,
        ..DEFAULT
    },
    // SAFETY: This is only called when the parse result is a boolean, so it is ok to access to
    // union field.
    |_p, r| unsafe { r.__bindgen_anon_1.boolean }
);

define_param_type!(
    bool,
    bool,
    Spec {
        type_: Some(bindings::fs_param_is_bool),
        ..DEFAULT
    },
    // SAFETY: This is only called when the parse result is a boolean, so it is ok to access to
    // union field.
    |_p, r| unsafe { r.__bindgen_anon_1.boolean }
);

define_param_type!(
    u32,
    u32,
    Spec {
        type_: Some(bindings::fs_param_is_u32),
        ..DEFAULT
    },
    // SAFETY: This is only called when the parse result is a u32, so it is ok to access to union
    // field.
    |_p, r| unsafe { r.__bindgen_anon_1.uint_32 }
);

define_param_type!(
    u32oct,
    u32,
    Spec {
        type_: Some(bindings::fs_param_is_u32),
        extra: 8 as _,
        ..DEFAULT
    },
    // SAFETY: This is only called when the parse result is a u32, so it is ok to access to union
    // field.
    |_p, r| unsafe { r.__bindgen_anon_1.uint_32 }
);

define_param_type!(
    u32hex,
    u32,
    Spec {
        type_: Some(bindings::fs_param_is_u32),
        extra: 16 as _,
        ..DEFAULT
    },
    // SAFETY: This is only called when the parse result is a u32, so it is ok to access to union
    // field.
    |_p, r| unsafe { r.__bindgen_anon_1.uint_32 }
);

define_param_type!(
    s32,
    i32,
    Spec {
        type_: Some(bindings::fs_param_is_s32),
        ..DEFAULT
    },
    // SAFETY: This is only called when the parse result is an i32, so it is ok to access to union
    // field.
    |_p, r| unsafe { r.__bindgen_anon_1.int_32 }
);

define_param_type!(
    u64,
    u64,
    Spec {
        type_: Some(bindings::fs_param_is_u64),
        extra: 16 as _,
        ..DEFAULT
    },
    // SAFETY: This is only called when the parse result is a u32, so it is ok to access to union
    // field.
    |_p, r| unsafe { r.__bindgen_anon_1.uint_64 }
);

define_param_type!(
    string,
    &CStr,
    Spec {
        type_: Some(bindings::fs_param_is_string),
        ..DEFAULT
    },
    // SAFETY: This is only called when the parse result is a string, so it is ok to access to
    // union field.
    |p, _r| unsafe { CStr::from_char_ptr(p.__bindgen_anon_1.string) }
);

/// Module to support `enum` parameter types.
pub mod enum_ {
    use super::*;

    #[doc(hidden)]
    pub const fn spec(name: &'static CStr, options: ConstantTable<'static>) -> Spec {
        Spec {
            name,
            type_: Some(bindings::fs_param_is_enum),
            extra: options.first as *const _ as _,
            ..DEFAULT
        }
    }

    #[doc(hidden)]
    pub const fn handler<S>(setfn: fn(&mut S, u32) -> Result) -> impl Handler<S> {
        let c = move |s: &mut S, _p: &bindings::fs_parameter, r: &bindings::fs_parse_result| {
            // SAFETY: This is only called when the parse result is an enum, so it is ok to access
            // to union field.
            setfn(s, unsafe { r.__bindgen_anon_1.uint_32 })
        };
        ConcreteHandler {
            setfn: c,
            _p: PhantomData,
        }
    }
}

const ZERO_SPEC: bindings::fs_parameter_spec = bindings::fs_parameter_spec {
    name: core::ptr::null(),
    type_: None,
    opt: 0,
    flags: 0,
    data: core::ptr::null(),
};

/// A zero-terminated parameter spec array, followed by handlers.
#[repr(C)]
pub struct SpecArray<const N: usize, S: 'static> {
    specs: [bindings::fs_parameter_spec; N],
    sentinel: bindings::fs_parameter_spec,
    handlers: [&'static dyn Handler<S>; N],
}

impl<const N: usize, S: 'static> SpecArray<N, S> {
    /// Creates a new spec array.
    ///
    /// Users are encouraged to use the [`define_fs_params`] macro to define the
    /// [`super::Context::PARAMS`] constant.
    ///
    /// # Safety
    ///
    /// The type of the elements in `handlers` must be compatible with the types in specs. For
    /// example, if `specs` declares that the i-th element is a bool then the i-th handler
    /// should be for a bool.
    pub const unsafe fn new(specs: [Spec; N], handlers: [&'static dyn Handler<S>; N]) -> Self {
        let mut array = Self {
            specs: [ZERO_SPEC; N],
            sentinel: ZERO_SPEC,
            handlers,
        };
        let mut i = 0usize;
        while i < N {
            array.specs[i] = bindings::fs_parameter_spec {
                name: specs[i].name.as_char_ptr(),
                type_: specs[i].type_,
                opt: i as _,
                flags: specs[i].flags,
                data: specs[i].extra,
            };
            i += 1;
        }
        array
    }

    /// Returns a [`SpecTable`] backed by `self`.
    ///
    /// This is used to essentially erase the array size.
    pub const fn as_table(&self) -> SpecTable<'_, S> {
        SpecTable {
            first: &self.specs[0],
            handlers: &self.handlers,
            _p: PhantomData,
        }
    }
}

/// A parameter spec table.
///
/// The table is guaranteed to be zero-terminated.
///
/// Users are encouraged to use the [`define_fs_params`] macro to define the
/// [`super::Context::PARAMS`] constant.
pub struct SpecTable<'a, S: 'static> {
    pub(super) first: &'a bindings::fs_parameter_spec,
    pub(super) handlers: &'a [&'static dyn Handler<S>],
    _p: PhantomData<S>,
}

impl<S> SpecTable<'static, S> {
    pub(super) const fn empty() -> Self {
        Self {
            first: &ZERO_SPEC,
            handlers: &[],
            _p: PhantomData,
        }
    }
}

/// A zero-terminated parameter constant array.
#[repr(C)]
pub struct ConstantArray<const N: usize> {
    consts: [bindings::constant_table; N],
    sentinel: bindings::constant_table,
}

impl<const N: usize> ConstantArray<N> {
    /// Creates a new constant array.
    ///
    /// Users are encouraged to use the [`define_fs_params`] macro to define the
    /// [`super::Context::PARAMS`] constant.
    pub const fn new(consts: [(&'static CStr, u32); N]) -> Self {
        const ZERO: bindings::constant_table = bindings::constant_table {
            name: core::ptr::null(),
            value: 0,
        };
        let mut array = Self {
            consts: [ZERO; N],
            sentinel: ZERO,
        };
        let mut i = 0usize;
        while i < N {
            array.consts[i] = bindings::constant_table {
                name: consts[i].0.as_char_ptr(),
                value: consts[i].1 as _,
            };
            i += 1;
        }
        array
    }

    /// Returns a [`ConstantTable`] backed by `self`.
    ///
    /// This is used to essentially erase the array size.
    pub const fn as_table(&self) -> ConstantTable<'_> {
        ConstantTable {
            first: &self.consts[0],
        }
    }
}

/// A parameter constant table.
///
/// The table is guaranteed to be zero-terminated.
pub struct ConstantTable<'a> {
    pub(super) first: &'a bindings::constant_table,
}

#[doc(hidden)]
pub trait Handler<S> {
    fn handle_param(
        &self,
        state: &mut S,
        p: &bindings::fs_parameter,
        r: &bindings::fs_parse_result,
    ) -> Result;
}

struct ConcreteHandler<
    S,
    T: Fn(&mut S, &bindings::fs_parameter, &bindings::fs_parse_result) -> Result,
> {
    setfn: T,
    _p: PhantomData<S>,
}

impl<S, T: Fn(&mut S, &bindings::fs_parameter, &bindings::fs_parse_result) -> Result> Handler<S>
    for ConcreteHandler<S, T>
{
    fn handle_param(
        &self,
        state: &mut S,
        p: &bindings::fs_parameter,
        r: &bindings::fs_parse_result,
    ) -> Result {
        (self.setfn)(state, p, r)
    }
}

/// Counts the number of comma-separated entries surrounded by braces.
///
/// # Examples
///
/// ```
/// # use kernel::count_brace_items;
///
/// assert_eq!(0, count_brace_items!());
/// assert_eq!(1, count_brace_items!({A}));
/// assert_eq!(1, count_brace_items!({A},));
/// assert_eq!(2, count_brace_items!({A}, {B}));
/// assert_eq!(2, count_brace_items!({A}, {B},));
/// assert_eq!(3, count_brace_items!({A}, {B}, {C}));
/// assert_eq!(3, count_brace_items!({A}, {B}, {C},));
/// ```
#[macro_export]
macro_rules! count_brace_items {
    ({$($item:tt)*}, $($remaining:tt)*) => { 1 + $crate::count_brace_items!($($remaining)*) };
    ({$($item:tt)*}) => { 1 };
    () => { 0 };
}

/// Defines the file system parameters of a given file system context.
///
/// # Examples
/// ```
/// # use kernel::prelude::*;
/// # use kernel::{c_str, fs, str::CString};
///
/// #[derive(Default)]
/// struct State {
///     flag: Option<bool>,
///     flag_no: Option<bool>,
///     bool_value: Option<bool>,
///     u32_value: Option<u32>,
///     i32_value: Option<i32>,
///     u64_value: Option<u64>,
///     str_value: Option<CString>,
///     enum_value: Option<u32>,
/// }
///
/// fn set_u32(s: &mut Box<State>, v: u32) -> Result {
///     s.u32_value = Some(v);
///     Ok(())
/// }
///
/// struct Example;
///
/// #[vtable]
/// impl fs::Context<Self> for Example {
///     type Data = Box<State>;
///
///     kernel::define_fs_params!{Box<State>,
///         {flag, "flag", |s, v| { s.flag = Some(v); Ok(()) } },
///         {flag_no, "flagno", |s, v| { s.flag_no = Some(v); Ok(()) } },
///         {bool, "bool", |s, v| { s.bool_value = Some(v); Ok(()) } },
///         {u32, "u32", set_u32 },
///         {u32oct, "u32oct", set_u32 },
///         {u32hex, "u32hex", set_u32 },
///         {s32, "s32", |s, v| { s.i32_value = Some(v); Ok(()) } },
///         {u64, "u64", |s, v| { s.u64_value = Some(v); Ok(()) } },
///         {string, "string", |s, v| {
///             s.str_value = Some(CString::try_from_fmt(fmt!("{v}"))?);
///             Ok(())
///         }},
///         {enum, "enum", [("first", 10), ("second", 20)], |s, v| {
///             s.enum_value = Some(v);
///             Ok(())
///         }},
///     }
///
///     fn try_new() -> Result<Self::Data> {
///         Ok(Box::try_new(State::default())?)
///     }
/// }
///
/// # impl fs::Type for Example {
/// #    type Context = Self;
/// #    const SUPER_TYPE: fs::Super = fs::Super::Independent;
/// #    const NAME: &'static CStr = c_str!("example");
/// #    const FLAGS: i32 = 0;
/// #
/// #    fn fill_super<'a>(
/// #        _data: Box<State>,
/// #        sb: fs::NewSuperBlock<'_, Self>,
/// #    ) -> Result<&fs::SuperBlock<Self>> {
/// #        let sb = sb.init(
/// #            (),
/// #            &fs::SuperParams {
/// #                magic: 0x6578616d,
/// #                ..fs::SuperParams::DEFAULT
/// #            },
/// #        )?;
/// #        let sb = sb.init_root()?;
/// #        Ok(sb)
/// #    }
/// # }
/// ```
#[macro_export]
macro_rules! define_fs_params {
    ($data_type:ty, $({$($t:tt)*}),+ $(,)?) => {
        const PARAMS: $crate::fs::param::SpecTable<'static, $data_type> =
            {
                use $crate::fs::param::{self, ConstantArray, Spec, SpecArray, Handler};
                use $crate::c_str;
                const COUNT: usize = $crate::count_brace_items!($({$($t)*},)*);
                const SPECS: [Spec; COUNT] = $crate::define_fs_params!(@specs $({$($t)*},)*);
                const HANDLERS: [&dyn Handler<$data_type>; COUNT] =
                    $crate::define_fs_params!(@handlers $data_type, $({$($t)*},)*);
                // SAFETY: We defined matching specs and handlers above.
                const ARRAY: SpecArray<COUNT, $data_type> =
                    unsafe { SpecArray::new(SPECS, HANDLERS) };
                ARRAY.as_table()
            };
    };

    (@handlers $data_type:ty, $({$($t:tt)*},)*) => {
        [ $($crate::define_fs_params!(@handler $data_type, $($t)*),)* ]
    };
    (@handler $data_type:ty, enum, $name:expr, $opts:expr, $closure:expr) => {
        &param::enum_::handler::<$data_type>($closure)
    };
    (@handler $data_type:ty, $type:ident, $name:expr, $closure:expr) => {
        &param::$type::handler::<$data_type>($closure)
    };

    (@specs $({$($t:tt)*},)*) => {[ $($crate::define_fs_params!(@spec $($t)*),)* ]};
    (@spec enum, $name:expr, [$($opts:tt)*], $closure:expr) => {
        {
            const COUNT: usize = $crate::count_paren_items!($($opts)*);
            const OPTIONS: ConstantArray<COUNT> =
                ConstantArray::new($crate::define_fs_params!(@c_str_first $($opts)*));
            param::enum_::spec(c_str!($name), OPTIONS.as_table())
        }
    };
    (@spec $type:ident, $name:expr, $closure:expr) => { param::$type::spec(c_str!($name)) };

    (@c_str_first $(($first:expr, $second:expr)),+ $(,)?) => {
        [$((c_str!($first), $second),)*]
    };
}
