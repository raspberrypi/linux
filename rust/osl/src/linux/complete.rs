use crate::error::{to_error, Errno, Result};

use kernel::{completion::Completion, new_completion, prelude::*, sync::Arc};

/// Osl Complete
#[pin_data]
pub struct OslCompletion {
    #[pin]
    val: Completion,
}

impl crate::sync::GeneralComplete for OslCompletion {
    fn new() -> Result<Arc<Self>> {
        Arc::pin_init(pin_init!(Self {
            val <- new_completion!("OslCompletion::completion")
        }))
    }

    fn reinit(&self) {
        self.val.reinit();
    }

    fn complete(&self) {
        self.val.complete();
    }

    fn wait_for_completion(&self) {
        self.val.wait_for_completion();
    }

    fn wait_for_completion_timeout(&self, timeout: u32) -> Result<()> {
        if self.val.wait_for_completion_timeout_sec(timeout as usize) == 0 {
            return to_error(Errno::TimeOut);
        };
        Ok(())
    }
}
