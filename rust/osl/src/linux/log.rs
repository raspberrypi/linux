pub use kernel::{pr_debug, pr_err, pr_info, pr_warn};

#[doc(hidden)]
#[allow(clippy::crate_in_macro_def)]
#[macro_export]
macro_rules! log_print (
    // The non-continuation cases (most of them, e.g. `INFO`).
    ($crate::log::LogLevel::Error, $($arg:tt)*) => (
        $crate::log::pr_err!($($arg)*)
    );
    ($crate::log::LogLevel::Warn, $($arg:tt)*) => (
        $crate::log::pr_warn!($($arg)*)
    );
    ($crate::log::LogLevel::Info, $($arg:tt)*) => (
        $crate::log::pr_info!($($arg)*)
    );
    ($crate::log::LogLevel::Debug, $($arg:tt)*) => (
        $crate::log::pr_debug!($($arg)*)
    );
);
