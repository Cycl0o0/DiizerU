//! Unified API error type that renders to the `Error` schema in /proto.

use axum::{http::StatusCode, response::IntoResponse, Json};
use serde_json::json;

#[derive(Debug, thiserror::Error)]
pub enum ApiError {
    #[error("unauthorized")]
    Unauthorized,
    #[error("forbidden: {0}")]
    Forbidden(String),
    #[error("not found")]
    NotFound,
    #[error("rate limited")]
    RateLimited,
    #[error("service busy")]
    Busy,
    #[error("bad request: {0}")]
    BadRequest(String),
    #[error("upstream error: {0}")]
    Upstream(String),
    #[error("internal: {0}")]
    Internal(String),
}

impl ApiError {
    fn status(&self) -> StatusCode {
        match self {
            ApiError::Unauthorized => StatusCode::UNAUTHORIZED,
            ApiError::Forbidden(_) => StatusCode::FORBIDDEN,
            ApiError::NotFound => StatusCode::NOT_FOUND,
            ApiError::RateLimited => StatusCode::TOO_MANY_REQUESTS,
            ApiError::Busy => StatusCode::SERVICE_UNAVAILABLE,
            ApiError::BadRequest(_) => StatusCode::BAD_REQUEST,
            ApiError::Upstream(_) => StatusCode::BAD_GATEWAY,
            ApiError::Internal(_) => StatusCode::INTERNAL_SERVER_ERROR,
        }
    }
    fn code(&self) -> &'static str {
        match self {
            ApiError::Unauthorized => "unauthorized",
            ApiError::Forbidden(_) => "forbidden",
            ApiError::NotFound => "not_found",
            ApiError::RateLimited => "rate_limited",
            ApiError::Busy => "busy",
            ApiError::BadRequest(_) => "bad_request",
            ApiError::Upstream(_) => "upstream",
            ApiError::Internal(_) => "internal",
        }
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> axum::response::Response {
        // NB: never include tokens in the message; these strings are log-safe.
        let body = Json(json!({ "error": self.code(), "message": self.to_string() }));
        (self.status(), body).into_response()
    }
}

pub type ApiResult<T> = Result<T, ApiError>;
