//! Runtime configuration, loaded from environment (see .env.example).
//! `RELAY_MODE` only changes URL/onboarding behavior, never the wire protocol.

use std::time::Duration;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RelayMode {
    Central,
    SelfHosted,
}

impl RelayMode {
    fn parse(s: &str) -> Self {
        match s.to_ascii_lowercase().as_str() {
            "self-hosted" | "self_hosted" | "selfhosted" => RelayMode::SelfHosted,
            _ => RelayMode::Central,
        }
    }
    pub fn as_str(&self) -> &'static str {
        match self {
            RelayMode::Central => "central",
            RelayMode::SelfHosted => "self-hosted",
        }
    }
}

#[derive(Clone)]
pub struct Config {
    pub mode: RelayMode,
    pub bind_addr: String,
    /// Public base URL (used in pairing verify_url). e.g. https://your-domain.example
    pub public_base_url: String,
    /// 32-byte master key (base64) for encrypting the ARL at rest.
    pub master_key: [u8; 32],
    pub admin_token: String,
    pub session_idle_timeout: Duration,
    pub max_concurrent_sessions: usize,
    /// In self-hosted mode the allowlist is implicitly {self}; codes optional.
    pub open_onboarding: bool,
}

fn env_or(key: &str, default: &str) -> String {
    std::env::var(key).unwrap_or_else(|_| default.to_string())
}

impl Config {
    pub fn from_env() -> anyhow::Result<Self> {
        let mode = RelayMode::parse(&env_or("RELAY_MODE", "central"));

        let master_key = {
            use base64::Engine;
            let raw = std::env::var("DIIZERU_MASTER_KEY")
                .map_err(|_| anyhow::anyhow!("DIIZERU_MASTER_KEY must be set (base64, 32 bytes)"))?;
            let bytes = base64::engine::general_purpose::STANDARD
                .decode(raw.trim())
                .map_err(|e| anyhow::anyhow!("DIIZERU_MASTER_KEY not valid base64: {e}"))?;
            if bytes.len() != 32 {
                anyhow::bail!("DIIZERU_MASTER_KEY must decode to exactly 32 bytes");
            }
            let mut k = [0u8; 32];
            k.copy_from_slice(&bytes);
            k
        };

        let admin_token = std::env::var("DIIZERU_ADMIN_TOKEN")
            .map_err(|_| anyhow::anyhow!("DIIZERU_ADMIN_TOKEN must be set"))?;
        if admin_token.len() < 16 {
            anyhow::bail!("DIIZERU_ADMIN_TOKEN too short (>=16 chars)");
        }

        let public_base_url = env_or("PUBLIC_BASE_URL", "https://your-domain.example");

        Ok(Config {
            mode: mode.clone(),
            bind_addr: env_or("BIND_ADDR", "0.0.0.0:8080"),
            public_base_url,
            master_key,
            admin_token,
            session_idle_timeout: Duration::from_secs(
                env_or("SESSION_IDLE_TIMEOUT_SECS", "900").parse().unwrap_or(900),
            ),
            max_concurrent_sessions: env_or("MAX_CONCURRENT_SESSIONS", "5")
                .parse()
                .unwrap_or(5),
            open_onboarding: matches!(mode, RelayMode::SelfHosted),
        })
    }
}
