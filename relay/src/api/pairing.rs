//! Device-code pairing + OAuth (PKCE) endpoints.
//!
//! Flow: console POST /pair/start -> shows user_code on TV. User opens /pair on
//! phone, enters user_code (+ invite code if central), clicks login -> redirect
//! to real accounts.spotify.com -> callback exchanges code, checks allowlist,
//! seals refresh token, issues a relay session token. Console POST /pair/poll
//! receives that token. We NEVER show a Spotify password field.

use axum::{
    extract::{Query, State},
    response::{Html, IntoResponse, Redirect},
    Json,
};
use serde::Deserialize;
use serde_json::{json, Value};

use crate::auth::pairing::{PairStatus, PairingRecord};
use crate::auth::{authorize_url, exchange_code, fetch_profile, gen_pkce};
use crate::crypto::{random_token, user_code};
use crate::error::{ApiError, ApiResult};
use crate::state::AppState;
use crate::store::UserRecord;

const PAIR_TTL_SECS: i64 = 900;

/// Loopback redirect accepted by Spotify's keymaster client (RFC 8252). The
/// phone can't deliver this back to the relay, so the user pastes the resulting
/// URL (see auth_login / auth_paste). Must match exactly at token exchange.
const KM_REDIRECT: &str = "http://127.0.0.1:8898/login";

#[derive(Deserialize)]
pub struct PairStartReq {
    #[serde(default)]
    device_name: Option<String>,
}

pub async fn pair_start(
    State(state): State<AppState>,
    body: Option<Json<PairStartReq>>,
) -> ApiResult<Json<Value>> {
    if state.killswitch_on() {
        return Err(ApiError::Busy);
    }
    let now = crate::now_epoch();
    let pkce = gen_pkce();
    let rec = PairingRecord {
        device_code: random_token(32),
        user_code: user_code(),
        device_name: body
            .and_then(|b| b.0.device_name)
            .unwrap_or_else(|| "Wii U".into()),
        pkce_verifier: pkce.verifier,
        oauth_state: random_token(24),
        status: PairStatus::Pending,
        invite_code: None,
        relay_session_token: None,
        expires_at: now + PAIR_TTL_SECS,
    };
    let resp = json!({
        "device_code": rec.device_code,
        "user_code": rec.user_code,
        "verify_url": format!("{}/v1/pair", state.cfg.public_base_url),
        "interval": 5,
        "expires_in": PAIR_TTL_SECS,
    });
    // challenge is recomputed in auth_login from the stored verifier
    let _ = pkce.challenge;
    state.pairing.insert(rec);
    Ok(Json(resp))
}

#[derive(Deserialize)]
pub struct PairPollReq {
    device_code: String,
}

pub async fn pair_poll(
    State(state): State<AppState>,
    Json(req): Json<PairPollReq>,
) -> ApiResult<Json<Value>> {
    let now = crate::now_epoch();
    let rec = state
        .pairing
        .get_by_device_code(&req.device_code, now)
        .ok_or(ApiError::NotFound)?;
    let status = match rec.status {
        PairStatus::Pending => "pending",
        PairStatus::Approved => "approved",
        PairStatus::Denied => "denied",
        PairStatus::Expired => "expired",
    };
    let mut out = json!({ "status": status });
    if rec.status == PairStatus::Approved {
        out["relay_session_token"] = json!(rec.relay_session_token);
    }
    Ok(Json(out))
}

/// GET /v1/pair — phone-facing Deezer onboarding: TV code + ARL (+ invite),
/// with a tutorial on how to find the ARL. Deezer-themed.
pub async fn verify_page(State(state): State<AppState>) -> Html<String> {
    let invite_field = if state.cfg.open_onboarding {
        String::new()
    } else {
        r#"<label>Invite code<input name="invite" autocapitalize=off autocorrect=off required></label>"#.into()
    };
    Html(format!(
        r#"<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>DiizerU — Link your Wii U</title>
<style>body{{font-family:system-ui;background:#0f0f13;color:#eee;display:flex;
justify-content:center;padding:1.5rem}}main{{max-width:380px;width:100%}}
h2{{background:linear-gradient(90deg,#a238ff,#ff0092);-webkit-background-clip:text;
background-clip:text;color:transparent}}
label{{display:block;margin:1rem 0;font-size:.95rem}}
input,textarea{{width:100%;box-sizing:border-box;padding:.6rem;font-size:1rem;margin-top:.3rem;
border-radius:8px;border:1px solid #333;background:#1b1b22;color:#eee}}
textarea{{height:84px;font-family:monospace;font-size:.8rem}}
button{{width:100%;padding:.85rem;margin-top:1rem;background:#a238ff;border:0;
border-radius:24px;color:#fff;font-weight:700;font-size:1rem}}
details{{background:#16161c;border-radius:10px;padding:.8rem 1rem;margin:1rem 0}}
summary{{cursor:pointer;color:#a238ff;font-weight:600}}
ol{{line-height:1.55;color:#ccc;padding-left:1.1rem}}
code{{background:#26262e;padding:.1rem .3rem;border-radius:4px}}</style></head>
<body><main>
<h2>DiizerU</h2>
<p>Link your Wii U to your Deezer account.</p>

<details open><summary>How to get your Deezer token (ARL)</summary>
<ol>
<li>On a computer, open <code>deezer.com</code> and log in (Deezer Premium).</li>
<li>Press <b>F12</b> to open developer tools.</li>
<li>Go to <b>Application</b> (Chrome) or <b>Storage</b> (Firefox) → <b>Cookies</b> → <code>https://www.deezer.com</code>.</li>
<li>Find the cookie named <code>arl</code> and copy its long value.</li>
<li>Paste it below with the code shown on your TV.</li>
</ol>
<p style="font-size:.8rem;color:#999">Stored encrypted on the relay; never shown or logged.</p>
</details>

<form method="post" action="/v1/pair/deezer">
<label>TV code<input name="user_code" placeholder="ABCD-1234" autocapitalize=characters autocorrect=off required></label>
{invite_field}
<label>Deezer ARL<textarea name="arl" placeholder="paste the arl cookie value" autocapitalize=off autocorrect=off spellcheck=false required></textarea></label>
<button type="submit">Link Wii U</button>
</form>
<p style="text-align:center;color:#777;font-size:.8rem;margin-top:1.5rem">
Made with &lt;3 by Cycl0o0 &middot; Not affiliated with Deezer or Nintendo</p>
</main></body></html>"#
    ))
}

#[derive(Deserialize)]
pub struct LoginQuery {
    user_code: String,
    #[serde(default)]
    invite: Option<String>,
}

/// GET /v1/auth/login?user_code=&invite= — resolve pairing, redirect to Spotify.
pub async fn auth_login(
    State(state): State<AppState>,
    Query(q): Query<LoginQuery>,
) -> ApiResult<Html<String>> {
    let dc = state
        .pairing
        .device_code_for_user_code(q.user_code.trim())
        .ok_or(ApiError::BadRequest("unknown code".into()))?;
    let rec = state.pairing.get(&dc).ok_or(ApiError::NotFound)?;
    if rec.status != PairStatus::Pending {
        return Err(ApiError::BadRequest("code not pending".into()));
    }
    state.pairing.set_invite(&dc, q.invite);
    // recompute the PKCE challenge from the stored verifier
    let challenge = {
        use base64::Engine;
        use sha2::{Digest, Sha256};
        base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(Sha256::digest(rec.pkce_verifier.as_bytes()))
    };
    // The Spotify keymaster client only accepts loopback redirects, which a
    // phone can't deliver back to this relay — so the user authorizes, then
    // pastes the (failed-to-load) redirect URL back here. This is the standard
    // headless-librespot dance.
    let url = authorize_url(
        &state.cfg.spotify_client_id,
        KM_REDIRECT,
        &challenge,
        &rec.oauth_state,
    );
    Ok(Html(format!(
        r#"<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>DiizerU — Sign in</title>
<style>body{{font-family:system-ui;background:#121212;color:#eee;display:flex;
justify-content:center;padding:1.5rem}}main{{max-width:360px;width:100%}}
a.btn,button{{display:block;width:100%;box-sizing:border-box;text-align:center;
padding:.8rem;margin:1rem 0;background:#a238ff;border:0;border-radius:24px;
color:#000;font-weight:700;font-size:1rem;text-decoration:none}}
textarea{{width:100%;box-sizing:border-box;height:90px;border-radius:8px;
border:1px solid #333;background:#1e1e1e;color:#eee;padding:.6rem}}
ol{{line-height:1.5;color:#ccc}}</style></head><body><main>
<h2>Almost there</h2>
<ol>
<li>Tap <b>Sign in with Spotify</b> below.</li>
<li>After approving, the page will say it <b>can't connect</b> (that's normal).</li>
<li><b>Copy that page's full address</b> (starts with http://127.0.0.1...).</li>
<li>Paste it below and tap <b>Finish</b>.</li>
</ol>
<a class="btn" href="{url}" target="_blank" rel="noopener">Sign in with Spotify</a>
<form method="post" action="/v1/auth/paste">
<input type="hidden" name="device_code" value="{dc}">
<textarea name="redirect_url" placeholder="Paste the http://127.0.0.1... address here" required></textarea>
<button type="submit">Finish</button>
</form>
</main></body></html>"#,
        url = url,
        dc = rec.device_code,
    )))
}

#[derive(Deserialize)]
pub struct PasteForm {
    device_code: String,
    redirect_url: String,
}

/// POST /v1/auth/paste — user pastes the loopback redirect URL; we extract the
/// code and finish auth (keymaster token works for both audio + Web API).
pub async fn auth_paste(
    State(state): State<AppState>,
    axum::Form(form): axum::Form<PasteForm>,
) -> ApiResult<Html<String>> {
    let code = extract_code(&form.redirect_url)
        .ok_or(ApiError::BadRequest("no ?code= in pasted URL".into()))?;
    finalize(&state, &form.device_code, &code, KM_REDIRECT).await
}

/// Extract the `code` query param from a pasted redirect URL.
fn extract_code(url: &str) -> Option<String> {
    let q = url.split('?').nth(1)?;
    for kv in q.split('&') {
        let mut it = kv.splitn(2, '=');
        if it.next() == Some("code") {
            let raw = it.next()?;
            return Some(urlencoding::decode(raw).map(|c| c.into_owned()).unwrap_or_else(|_| raw.to_string()));
        }
    }
    None
}

#[derive(Deserialize)]
pub struct CallbackQuery {
    #[serde(default)]
    code: Option<String>,
    #[serde(default)]
    state: Option<String>,
    #[serde(default)]
    error: Option<String>,
}

/// GET /v1/auth/callback — kept for clients/redirects that can reach us directly.
pub async fn auth_callback(
    State(state): State<AppState>,
    Query(q): Query<CallbackQuery>,
) -> ApiResult<Html<String>> {
    if let Some(err) = q.error {
        return Ok(Html(page("Authorization denied", &err)));
    }
    let oauth_state = q.state.ok_or(ApiError::BadRequest("missing state".into()))?;
    let code = q.code.ok_or(ApiError::BadRequest("missing code".into()))?;
    let dc = state
        .pairing
        .device_code_for_state(&oauth_state)
        .ok_or(ApiError::BadRequest("bad state".into()))?;
    finalize(&state, &dc, &code, &state.cfg.oauth_redirect_uri).await
}

/// Exchange the code, gate on the allowlist, store the sealed token, and issue
/// the console's relay session token. `redirect_uri` must match the one used in
/// the authorize request (loopback for the paste flow).
async fn finalize(
    state: &AppState,
    dc: &str,
    code: &str,
    redirect_uri: &str,
) -> ApiResult<Html<String>> {
    let rec = state.pairing.get(dc).ok_or(ApiError::NotFound)?;

    let tokens = exchange_code(
        &state.http,
        &state.cfg.spotify_client_id,
        state.cfg.spotify_client_secret.as_deref(),
        redirect_uri,
        code,
        &rec.pkce_verifier,
    )
    .await
    .map_err(|e| ApiError::Upstream(e.to_string()))?;
    tracing::info!(granted_scope = %tokens.scope, "token exchanged");

    // Resolve the Spotify user_id. With librespot, ask the session (no Web API
    // /me, which Spotify 429s for the keymaster client on a server IP).
    #[cfg(feature = "librespot")]
    let (user_id, display_name, product) = {
        let uid = crate::audio::librespot_source::user_id_from_token(&tokens.access_token)
            .await
            .map_err(|e| ApiError::Upstream(e.to_string()))?;
        (uid, String::new(), "premium".to_string())
    };
    #[cfg(not(feature = "librespot"))]
    let (user_id, display_name, product) = {
        let p = fetch_profile(&state.http, &tokens.access_token)
            .await
            .map_err(|e| ApiError::Upstream(e.to_string()))?;
        (p.id, p.display_name.unwrap_or_default(), p.product.unwrap_or_default())
    };

    // ---- ALLOWLIST GATE (core of the private beta) ----
    let now = crate::now_epoch();
    let allowed = if state.store.is_allowed(&user_id) {
        true
    } else if state.cfg.open_onboarding {
        state.store.allow_user(&user_id);
        true
    } else if let Some(invite) = &rec.invite_code {
        state.store.consume_invite(invite, &user_id, now)
    } else {
        false
    };
    if !allowed {
        state.pairing.deny(dc);
        return Ok(Html(page(
            "Not invited",
            "This Spotify account isn't on the DiizerU beta allowlist.",
        )));
    }

    let refresh = tokens
        .refresh_token
        .ok_or(ApiError::Upstream("no refresh token".into()))?;
    let sealed = state
        .cipher
        .seal(&refresh)
        .map_err(|_| ApiError::Internal("seal".into()))?;
    state.store.upsert_user(UserRecord {
        user_id: user_id.clone(),
        display_name,
        product,
        enc_refresh_token: sealed,
        created_at: now,
        state: crate::store::AllowState::Allowed,
    });

    let relay_token = random_token(40);
    state.store.create_relay_session(crate::store::RelaySession {
        token: relay_token.clone(),
        user_id: user_id.clone(),
        created_at: now,
    });
    state.pairing.approve(dc, relay_token);

    Ok(Html(page(
        "Wii U linked ✔",
        "You can return to your Wii U — it will connect in a moment.",
    )))
}

fn page(title: &str, msg: &str) -> String {
    format!(
        r#"<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>{title}</title>
<style>body{{font-family:system-ui;background:#121212;color:#eee;text-align:center;padding:3rem}}
h1{{color:#a238ff}}</style></head><body><h1>{title}</h1><p>{msg}</p></body></html>"#
    )
}

// ---- Deezer onboarding (feature = "deezer") ----
#[cfg(feature = "deezer")]
#[derive(Deserialize)]
pub struct DeezerPairForm {
    user_code: String,
    #[serde(default)]
    invite: Option<String>,
    arl: String,
}

/// POST /v1/pair/deezer — validate the ARL, gate on allowlist, store the ARL
/// (encrypted), and approve the pairing so the console gets a session token.
#[cfg(feature = "deezer")]
pub async fn deezer_pair(
    State(state): State<AppState>,
    axum::Form(f): axum::Form<DeezerPairForm>,
) -> ApiResult<Html<String>> {
    let dc = state
        .pairing
        .device_code_for_user_code(f.user_code.trim())
        .ok_or(ApiError::BadRequest("unknown TV code".into()))?;
    let rec = state.pairing.get(&dc).ok_or(ApiError::NotFound)?;
    if rec.status != PairStatus::Pending {
        return Ok(Html(page("Code expired", "Relaunch DiizerU on your Wii U for a fresh code.")));
    }

    // Validate the ARL by logging in to Deezer.
    let arl = f.arl.trim().to_string();
    let client = crate::deezer::DeezerClient::new(state.http.clone(), arl.clone());
    let session = match client.login().await {
        Ok(s) => s,
        Err(e) => return Ok(Html(page("Deezer login failed", &format!("{e}")))),
    };
    let user_id = format!("deezer:{}", session.user_id);

    // Allowlist gate (private beta).
    let now = crate::now_epoch();
    let allowed = if state.store.is_allowed(&user_id) {
        true
    } else if state.cfg.open_onboarding {
        state.store.allow_user(&user_id);
        true
    } else if let Some(invite) = &f.invite {
        state.store.consume_invite(invite.trim(), &user_id, now)
    } else {
        false
    };
    if !allowed {
        state.pairing.deny(&dc);
        return Ok(Html(page("Not invited", "This account isn't on the DiizerU beta allowlist.")));
    }

    // Store the ARL encrypted at rest (reuses the sealed-token field).
    let sealed = state
        .cipher
        .seal(&arl)
        .map_err(|_| ApiError::Internal("seal".into()))?;
    state.store.upsert_user(UserRecord {
        user_id: user_id.clone(),
        display_name: String::new(),
        product: "deezer".into(),
        enc_refresh_token: sealed,
        created_at: now,
        state: crate::store::AllowState::Allowed,
    });

    let relay_token = random_token(40);
    state.store.create_relay_session(crate::store::RelaySession {
        token: relay_token.clone(),
        user_id,
        created_at: now,
    });
    state.pairing.approve(&dc, relay_token);
    Ok(Html(page("Wii U linked ✔", "Return to your Wii U — it will connect in a moment.")))
}
