//! Device-code pairing for Deezer.
//!
//! Console `POST /pair/start` -> shows a user_code on the TV. The user opens
//! `GET /pair` on a phone, enters the code + their Deezer ARL (+ invite in
//! central mode), and `POST /pair/deezer` validates the ARL, stores it
//! encrypted, and approves the pairing. The console polls `POST /pair/poll`
//! and receives an opaque relay session token.

use axum::{extract::State, response::Html, Json};
use serde::Deserialize;
use serde_json::{json, Value};

use crate::auth::pairing::{PairStatus, PairingRecord};
use crate::crypto::{random_token, user_code};
use crate::error::{ApiError, ApiResult};
use crate::state::AppState;
use crate::store::UserRecord;

const PAIR_TTL_SECS: i64 = 900;

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
    let rec = PairingRecord {
        device_code: random_token(32),
        user_code: user_code(),
        device_name: body
            .and_then(|b| b.0.device_name)
            .unwrap_or_else(|| "Wii U".into()),
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

/// GET /v1/pair — phone-facing onboarding: TV code + Deezer ARL (+ invite), with
/// a tutorial on how to find the ARL.
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

fn page(title: &str, msg: &str) -> String {
    format!(
        r#"<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>{title}</title>
<style>body{{font-family:system-ui;background:#121212;color:#eee;text-align:center;padding:3rem}}
h1{{color:#a238ff}}</style></head><body><h1>{title}</h1><p>{msg}</p></body></html>"#
    )
}

#[derive(Deserialize)]
pub struct DeezerPairForm {
    user_code: String,
    #[serde(default)]
    invite: Option<String>,
    arl: String,
}

/// POST /v1/pair/deezer — validate the ARL, gate on allowlist, store the ARL
/// (encrypted), and approve the pairing so the console gets a session token.
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

    // Store the ARL encrypted at rest.
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
