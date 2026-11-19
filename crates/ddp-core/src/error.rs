//! Small error helper so the crate keeps `Result<T, String>` at its boundaries.

pub type CoreResult<T> = Result<T, String>;

pub fn error(message: impl Into<String>) -> String {
    message.into()
}
