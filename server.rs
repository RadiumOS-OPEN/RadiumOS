use std::{
    collections::{HashMap, HashSet, VecDeque},
    fs,
    future::Future,
    io,
    io::{BufReader, ErrorKind, Write},
    net::{IpAddr, SocketAddr},
    os::unix::fs::OpenOptionsExt,
    path::{Path, PathBuf},
    pin::Pin,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

use anyhow::{Context, Result, bail};
use argon2::Argon2;
use axum::{
    Router,
    body::to_bytes,
    extract::{ConnectInfo, Path as AxumPath, RawQuery, Request, State},
    http::{HeaderMap, HeaderValue, StatusCode, header},
    response::{IntoResponse, Response},
    routing::{get, post},
};
use axum_server::accept::Accept;
use axum_server::tls_rustls::RustlsConfig;
use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use chrono::{SecondsFormat, Utc};
use clap::{Parser, Subcommand};
use ed25519_dalek::{Signature, Verifier, VerifyingKey};
use rand::{RngCore, rngs::OsRng};
use rusqlite::{Connection, OptionalExtension, TransactionBehavior, params};
use rustls::ServerConfig;
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use serde_json::{Map, Number, Value};
use sha2::{Digest, Sha256};
use subtle::ConstantTimeEq;
use tokio::{
    io::{AsyncRead, AsyncWrite},
    net::TcpStream,
    time::{MissedTickBehavior, interval},
};
use tokio_rustls::TlsAcceptor;

const REGISTER_PREFIX: &[u8] = b"rchat-register-v1\0";
const KEY_PREFIX: &[u8] = b"rchat-key-v1\0";
const AUTH_PREFIX: &[u8] = b"rchat-auth-v1\0";
const ID_PREFIX: &[u8] = b"rchat-id-v1\0";
const OTHER_BODY_LIMIT: usize = 16 * 1024;
const MESSAGE_BODY_LIMIT: usize = 48 * 1024;
const MAX_CIPHERTEXT: usize = 24_692;

#[derive(Parser)]
#[command(name = "rchat-server")]
struct Cli {
    #[arg(long, default_value = "config.json")]
    config: PathBuf,
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    Serve {
        #[arg(long)]
        host: Option<IpAddr>,
        #[arg(long, value_parser = clap::value_parser!(u16).range(1..))]
        port: Option<u16>,
    },
    Keygen {
        #[arg(long = "host", default_value = "localhost")]
        hosts: Vec<String>,
    },
    Invite {
        #[command(subcommand)]
        command: InviteCommand,
    },
}

#[derive(Subcommand)]
enum InviteCommand {
    Add {
        code: String,
        #[arg(long)]
        uses: u64,
    },
    Revoke {
        code: String,
    },
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct Config {
    #[serde(default = "default_bind")]
    bind: String,
    #[serde(default = "default_database")]
    database_path: PathBuf,
    #[serde(default)]
    account_ttl_seconds: u64,
    #[serde(default = "default_message_ttl")]
    message_ttl_seconds: u64,
    tls_certificate_path: Option<PathBuf>,
    tls_private_key_path: Option<PathBuf>,
    #[serde(default)]
    allow_insecure_http: bool,
}

fn default_bind() -> String {
    "127.0.0.1:8443".into()
}

fn default_database() -> PathBuf {
    "rchat.db".into()
}

fn default_message_ttl() -> u64 {
    30 * 24 * 60 * 60
}

#[derive(Clone)]
struct AppState {
    db: Db,
    config: Arc<Config>,
    limits: Arc<RateLimits>,
}

enum Transport {
    Http,
    Https(RustlsConfig),
    HttpAndHttps(RustlsConfig),
}

trait ConnectionStream: AsyncRead + AsyncWrite + Unpin + Send {}

impl<T> ConnectionStream for T where T: AsyncRead + AsyncWrite + Unpin + Send {}

#[derive(Clone)]
struct HttpAndHttpsAcceptor {
    tls: TlsAcceptor,
}

impl<S> Accept<TcpStream, S> for HttpAndHttpsAcceptor
where
    S: Send + 'static,
{
    type Stream = Box<dyn ConnectionStream>;
    type Service = S;
    type Future = Pin<Box<dyn Future<Output = io::Result<(Self::Stream, S)>> + Send>>;

    fn accept(&self, stream: TcpStream, service: S) -> Self::Future {
        let tls = self.tls.clone();
        Box::pin(async move {
            let connection = tokio::time::timeout(Duration::from_secs(10), async {
                let mut first_byte = [0];
                if stream.peek(&mut first_byte).await? == 0 {
                    return Err(io::Error::new(
                        io::ErrorKind::UnexpectedEof,
                        "connection closed before sending a request",
                    ));
                }
                if first_byte[0] == 0x16 {
                    return tls
                        .accept(stream)
                        .await
                        .map(|stream| Box::new(stream) as Box<dyn ConnectionStream>);
                }
                Ok(Box::new(stream) as Box<dyn ConnectionStream>)
            })
            .await
            .map_err(|_| {
                io::Error::new(io::ErrorKind::TimedOut, "connection handshake timed out")
            })??;
            Ok((connection, service))
        })
    }
}

#[derive(Clone)]
struct Db {
    path: Arc<PathBuf>,
}

impl Db {
    fn new(path: PathBuf) -> Self {
        Self {
            path: Arc::new(path),
        }
    }

    async fn run<T, F>(&self, operation: F) -> Result<T>
    where
        T: Send + 'static,
        F: FnOnce(&mut Connection) -> Result<T> + Send + 'static,
    {
        let path = self.path.clone();
        tokio::task::spawn_blocking(move || {
            let mut connection = Connection::open(path.as_ref())
                .with_context(|| format!("could not open SQLite database {}", path.display()))?;
            connection.busy_timeout(Duration::from_secs(5))?;
            connection.execute_batch("PRAGMA foreign_keys = ON")?;
            operation(&mut connection)
        })
        .await
        .context("database task stopped")?
    }

    async fn init(&self) -> Result<()> {
        self.run(|connection| {
            connection.execute_batch(
                "PRAGMA journal_mode = WAL;
                 PRAGMA synchronous = NORMAL;
                 CREATE TABLE IF NOT EXISTS invites (
                     id INTEGER PRIMARY KEY,
                     salt BLOB NOT NULL UNIQUE,
                     code_hash BLOB NOT NULL,
                     remaining INTEGER NOT NULL CHECK (remaining >= 0),
                     revoked INTEGER NOT NULL DEFAULT 0 CHECK (revoked IN (0, 1))
                 );
                 CREATE TABLE IF NOT EXISTS clients (
                     client_id TEXT PRIMARY KEY,
                     auth_key BLOB NOT NULL UNIQUE,
                     encryption_key BLOB NOT NULL,
                     encryption_signature BLOB NOT NULL,
                     last_active INTEGER NOT NULL
                 );
                 CREATE TABLE IF NOT EXISTS challenges (
                     challenge_id BLOB PRIMARY KEY,
                     client_id TEXT NOT NULL REFERENCES clients(client_id) ON DELETE CASCADE,
                     challenge BLOB NOT NULL,
                     expires_at INTEGER NOT NULL
                 );
                 CREATE TABLE IF NOT EXISTS sessions (
                     token_hash BLOB PRIMARY KEY,
                     client_id TEXT NOT NULL REFERENCES clients(client_id) ON DELETE CASCADE,
                     expires_at INTEGER NOT NULL
                 );
                 CREATE TABLE IF NOT EXISTS messages (
                     sequence INTEGER PRIMARY KEY AUTOINCREMENT,
                     sender_id TEXT NOT NULL REFERENCES clients(client_id) ON DELETE CASCADE,
                     recipient_id TEXT NOT NULL REFERENCES clients(client_id) ON DELETE CASCADE,
                     client_message_id BLOB NOT NULL,
                     enc BLOB,
                     ciphertext BLOB,
                     fingerprint BLOB NOT NULL,
                     received_at INTEGER NOT NULL,
                     expires_at INTEGER NOT NULL,
                     acknowledged INTEGER NOT NULL DEFAULT 0 CHECK (acknowledged IN (0, 1)),
                     UNIQUE(sender_id, client_message_id)
                 );
                 CREATE INDEX IF NOT EXISTS messages_recipient_sequence
                     ON messages(recipient_id, sequence);
                 CREATE INDEX IF NOT EXISTS clients_last_active ON clients(last_active);
                 CREATE INDEX IF NOT EXISTS challenges_expiry ON challenges(expires_at);
                 CREATE INDEX IF NOT EXISTS sessions_expiry ON sessions(expires_at);",
            )?;
            Ok(())
        })
        .await
    }
}

#[derive(Default)]
struct RateLimits {
    entries: Mutex<HashMap<String, VecDeque<Instant>>>,
}

impl RateLimits {
    fn check(&self, key: String, maximum: usize, window: Duration) -> Result<(), u64> {
        let now = Instant::now();
        let mut entries = self.entries.lock().expect("rate limiter lock poisoned");
        let attempts = entries.entry(key).or_default();
        while attempts
            .front()
            .is_some_and(|time| now.duration_since(*time) >= window)
        {
            attempts.pop_front();
        }
        if attempts.len() >= maximum {
            let retry = window.saturating_sub(now.duration_since(*attempts.front().unwrap()));
            return Err(retry.as_secs().max(1));
        }
        attempts.push_back(now);
        Ok(())
    }
}

#[derive(Debug)]
struct ApiError {
    status: StatusCode,
    code: &'static str,
    message: &'static str,
    retry_after: Option<u64>,
}

impl ApiError {
    fn new(status: StatusCode, code: &'static str, message: &'static str) -> Self {
        Self {
            status,
            code,
            message,
            retry_after: None,
        }
    }

    fn internal(error: impl std::fmt::Display) -> Self {
        eprintln!("request failed: {error}");
        Self::new(
            StatusCode::INTERNAL_SERVER_ERROR,
            "internal_error",
            "Internal server error",
        )
    }

    fn invalid_json() -> Self {
        Self::new(StatusCode::BAD_REQUEST, "invalid_json", "Invalid JSON body")
    }

    fn invalid_token() -> Self {
        Self::new(
            StatusCode::UNAUTHORIZED,
            "invalid_token",
            "Invalid access token",
        )
    }

    fn rate_limited(seconds: u64) -> Self {
        Self {
            status: StatusCode::TOO_MANY_REQUESTS,
            code: "rate_limited",
            message: "Rate limit exceeded",
            retry_after: Some(seconds),
        }
    }
}

#[derive(Serialize)]
struct ErrorEnvelope {
    error: ErrorBody,
}

#[derive(Serialize)]
struct ErrorBody {
    code: &'static str,
    message: &'static str,
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        let mut response = (
            self.status,
            axum::Json(ErrorEnvelope {
                error: ErrorBody {
                    code: self.code,
                    message: self.message,
                },
            }),
        )
            .into_response();
        if self.status == StatusCode::UNAUTHORIZED {
            response
                .headers_mut()
                .insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
            if self.code == "invalid_token" {
                response
                    .headers_mut()
                    .insert(header::WWW_AUTHENTICATE, HeaderValue::from_static("Bearer"));
            }
        }
        if let Some(seconds) = self.retry_after {
            response.headers_mut().insert(
                header::RETRY_AFTER,
                HeaderValue::from_str(&seconds.to_string()).unwrap(),
            );
        }
        response
    }
}

type ApiResult<T> = std::result::Result<T, ApiError>;

#[derive(Deserialize)]
struct RegistrationRequest {
    invite_code: String,
    auth_public_key: String,
    encryption_public_key: String,
    encryption_key_signature: String,
    registration_signature: String,
}

#[derive(Serialize)]
struct ClientIdResponse {
    client_id: String,
}

#[derive(Deserialize)]
struct ChallengeRequest {
    client_id: String,
}

#[derive(Serialize)]
struct ChallengeResponse {
    challenge_id: String,
    challenge: String,
    expires_in: u64,
}

#[derive(Deserialize)]
struct SessionRequest {
    client_id: String,
    challenge_id: String,
    signature: String,
}

#[derive(Serialize)]
struct SessionResponse {
    access_token: String,
    token_type: &'static str,
    expires_in: u64,
}

#[derive(Serialize)]
struct ClientResponse {
    client_id: String,
    auth_public_key: String,
    encryption_public_key: String,
    encryption_key_signature: String,
}

#[derive(Deserialize)]
struct SubmitMessageRequest {
    recipient_id: String,
    client_message_id: String,
    enc: String,
    ciphertext: String,
}

#[derive(Clone, Serialize)]
struct SubmitMessageResponse {
    server_message_id: String,
    received_at: String,
    expires_at: String,
}

#[derive(Serialize)]
struct PollResponse {
    messages: Vec<MessageResponse>,
    cursor: String,
}

#[derive(Serialize)]
struct MessageResponse {
    server_message_id: String,
    sender_id: String,
    client_message_id: String,
    enc: String,
    ciphertext: String,
    received_at: String,
    expires_at: String,
}

#[derive(Deserialize)]
struct AckRequest {
    server_message_ids: Vec<String>,
}

#[derive(Serialize)]
struct AckResponse {
    acked: usize,
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();
    let config = load_config(&cli.config)?;

    match cli.command {
        Command::Keygen { hosts } => keygen(&config, hosts),
        Command::Serve { host, port } => {
            let db = Db::new(config.database_path.clone());
            db.init().await?;
            serve(config, db, host, port).await
        }
        Command::Invite { command } => match command {
            InviteCommand::Add { code, uses } => {
                let db = Db::new(config.database_path);
                db.init().await?;
                add_invite(db, code, uses).await
            }
            InviteCommand::Revoke { code } => {
                let db = Db::new(config.database_path);
                db.init().await?;
                revoke_invite(db, code).await
            }
        },
    }
}

fn keygen(config: &Config, hosts: Vec<String>) -> Result<()> {
    let certificate_path = config
        .tls_certificate_path
        .as_deref()
        .context("set tls_certificate_path in config.json before running keygen")?;
    let key_path = config
        .tls_private_key_path
        .as_deref()
        .context("set tls_private_key_path in config.json before running keygen")?;
    if certificate_path == key_path {
        bail!("TLS certificate and private key paths must be different");
    }
    if certificate_path.exists() || key_path.exists() {
        bail!(
            "refusing to overwrite {} or {}; move or delete both files first",
            certificate_path.display(),
            key_path.display()
        );
    }

    let rcgen::CertifiedKey { cert, signing_key } =
        rcgen::generate_simple_self_signed(hosts.clone())
            .context("could not generate TLS keypair")?;
    let mut key_file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(key_path)
        .with_context(|| format!("could not create TLS private key {}", key_path.display()))?;
    let mut certificate_file = match fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o644)
        .open(certificate_path)
    {
        Ok(file) => file,
        Err(error) => {
            drop(key_file);
            let _ = fs::remove_file(key_path);
            return Err(error).with_context(|| {
                format!(
                    "could not create TLS certificate {}",
                    certificate_path.display()
                )
            });
        }
    };
    let write_result = (|| {
        key_file.write_all(signing_key.serialize_pem().as_bytes())?;
        certificate_file.write_all(cert.pem().as_bytes())?;
        key_file.sync_all()?;
        certificate_file.sync_all()?;
        Ok::<_, std::io::Error>(())
    })();
    if let Err(error) = write_result {
        drop(key_file);
        drop(certificate_file);
        let _ = fs::remove_file(key_path);
        let _ = fs::remove_file(certificate_path);
        return Err(error).context("could not write TLS keypair");
    }

    println!(
        "generated self-signed TLS certificate for {}",
        hosts.join(", ")
    );
    println!("certificate: {}", certificate_path.display());
    println!("private key: {}", key_path.display());
    println!("clients must explicitly trust this certificate");
    Ok(())
}

fn load_config(path: &Path) -> Result<Config> {
    let bytes = match fs::read(path) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == ErrorKind::NotFound => {
            let config = Config {
                bind: default_bind(),
                database_path: default_database(),
                account_ttl_seconds: 0,
                message_ttl_seconds: default_message_ttl(),
                tls_certificate_path: Some("server.crt".into()),
                tls_private_key_path: Some("server.key".into()),
                allow_insecure_http: false,
            };
            let mut bytes = serde_json::to_vec_pretty(&config)?;
            bytes.push(b'\n');
            match fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(path)
            {
                Ok(mut file) => {
                    file.write_all(&bytes)?;
                    eprintln!(
                        "created {}; review the TLS certificate paths before starting the server",
                        path.display()
                    );
                    bytes
                }
                Err(error) if error.kind() == ErrorKind::AlreadyExists => fs::read(path)?,
                Err(error) => {
                    return Err(error)
                        .with_context(|| format!("could not create {}", path.display()));
                }
            }
        }
        Err(error) => {
            return Err(error).with_context(|| format!("could not read {}", path.display()));
        }
    };
    parse_config(path, &bytes)
}

fn parse_config(path: &Path, bytes: &[u8]) -> Result<Config> {
    let result = (|| {
        let mut config: Config = parse_strict(bytes)?;
        let bind = config
            .bind
            .parse::<SocketAddr>()
            .context("invalid bind address")?;
        if bind.port() == 0 {
            bail!("bind port must be from 1 through 65535");
        }
        if config.database_path.as_os_str().is_empty() {
            bail!("database_path must not be empty");
        }
        match (&config.tls_certificate_path, &config.tls_private_key_path) {
            (Some(_), Some(_)) | (None, None) => {}
            _ => bail!("both TLS certificate paths are required when either is configured"),
        }
        if config.message_ttl_seconds == 0 || config.message_ttl_seconds > i64::MAX as u64 {
            bail!("message_ttl_seconds must be from 1 through {}", i64::MAX);
        }
        if config.account_ttl_seconds > i64::MAX as u64 {
            bail!("account_ttl_seconds must be no larger than {}", i64::MAX);
        }
        let directory = path.parent().unwrap_or_else(|| Path::new("."));
        if config.database_path.is_relative() {
            config.database_path = directory.join(&config.database_path);
        }
        if let Some(certificate_path) = &mut config.tls_certificate_path
            && certificate_path.is_relative()
        {
            *certificate_path = directory.join(&*certificate_path);
        }
        if let Some(key_path) = &mut config.tls_private_key_path
            && key_path.is_relative()
        {
            *key_path = directory.join(&*key_path);
        }
        Ok(config)
    })();
    result.with_context(|| {
        format!(
            "{} is invalid; delete it to generate a fresh configuration, or restore a valid copy",
            path.display()
        )
    })
}

async fn serve(config: Config, db: Db, host: Option<IpAddr>, port: Option<u16>) -> Result<()> {
    let configured: SocketAddr = config.bind.parse().context("invalid bind address")?;
    let address = SocketAddr::new(
        host.unwrap_or(configured.ip()),
        port.unwrap_or(configured.port()),
    );
    let transport = select_transport(&config).await?;
    let state = AppState {
        db,
        config: Arc::new(config),
        limits: Arc::new(RateLimits::default()),
    };
    tokio::spawn(run_expiry_loop(state.clone()));
    let app = app(state);

    match transport {
        Transport::Https(tls) => {
            eprintln!("listening on https://{address}");
            axum_server::bind_rustls(address, tls)
                .serve(app.into_make_service_with_connect_info::<SocketAddr>())
                .await?;
        }
        Transport::Http => {
            eprintln!("listening on http://{address}");
            axum_server::bind(address)
                .serve(app.into_make_service_with_connect_info::<SocketAddr>())
                .await?;
        }
        Transport::HttpAndHttps(tls) => {
            eprintln!("listening on http://{address} and https://{address}");
            let acceptor = HttpAndHttpsAcceptor {
                tls: TlsAcceptor::from(tls.get_inner()),
            };
            axum_server::bind(address)
                .acceptor(acceptor)
                .serve(app.into_make_service_with_connect_info::<SocketAddr>())
                .await?;
        }
    }
    Ok(())
}

async fn select_transport(config: &Config) -> Result<Transport> {
    match (
        &config.tls_certificate_path,
        &config.tls_private_key_path,
        config.allow_insecure_http,
    ) {
        (Some(certificate), Some(key), allow_http) => {
            let tls = load_tls(certificate, key).await?;
            Ok(if allow_http {
                Transport::HttpAndHttps(tls)
            } else {
                Transport::Https(tls)
            })
        }
        (None, None, true) => Ok(Transport::Http),
        (None, None, false) => bail!(
            "TLS is required; configure tls_certificate_path and tls_private_key_path, or explicitly set allow_insecure_http to true"
        ),
        _ => bail!("both tls_certificate_path and tls_private_key_path are required"),
    }
}

fn app(state: AppState) -> Router {
    Router::new()
        .route("/v1/clients", post(register_client))
        .route("/v1/clients/{client_id}", get(get_client))
        .route("/v1/auth/challenges", post(create_challenge))
        .route("/v1/auth/sessions", post(create_session))
        .route("/v1/messages", post(submit_message).get(poll_messages))
        .route("/v1/messages/ack", post(ack_messages))
        .with_state(state)
}

async fn load_tls(certificate_path: &Path, key_path: &Path) -> Result<RustlsConfig> {
    let mut certificate_reader = BufReader::new(
        fs::File::open(certificate_path).with_context(|| {
            format!(
                "could not open TLS certificate {}; update its path in config.json or provide the file",
                certificate_path.display()
            )
        })?,
    );
    let certificates = rustls_pemfile::certs(&mut certificate_reader)
        .collect::<std::result::Result<Vec<_>, _>>()
        .with_context(|| {
            format!(
                "could not parse TLS certificate {}",
                certificate_path.display()
            )
        })?;
    let mut key_reader = BufReader::new(fs::File::open(key_path).with_context(|| {
        format!(
            "could not open TLS private key {}; update its path in config.json or provide the file",
            key_path.display()
        )
    })?);
    let key = rustls_pemfile::private_key(&mut key_reader)
        .with_context(|| format!("could not parse TLS private key {}", key_path.display()))?
        .with_context(|| format!("TLS private key {} contains no key", key_path.display()))?;
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let config = ServerConfig::builder_with_provider(provider)
        .with_protocol_versions(&[&rustls::version::TLS13])?
        .with_no_client_auth()
        .with_single_cert(certificates, key)
        .context("TLS certificate and private key are invalid or do not match")?;
    Ok(RustlsConfig::from_config(Arc::new(config)))
}

async fn add_invite(db: Db, code: String, uses: u64) -> Result<()> {
    validate_invite(&code).map_err(|error| anyhow::anyhow!(error.message))?;
    if uses == 0 || uses > i64::MAX as u64 {
        bail!(
            "--uses must be a positive integer no larger than {}",
            i64::MAX
        );
    }
    let salt = random_bytes::<16>();
    let hash = invite_hash(code.as_bytes(), &salt)?;
    db.run(move |connection| {
        let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
        let mut statement =
            transaction.prepare("SELECT id, salt, code_hash, remaining, revoked FROM invites")?;
        let rows = statement.query_map([], |row| {
            Ok((
                row.get::<_, i64>(0)?,
                row.get::<_, Vec<u8>>(1)?,
                row.get::<_, Vec<u8>>(2)?,
                row.get::<_, i64>(3)?,
                row.get::<_, bool>(4)?,
            ))
        })?;
        let mut matching = None;
        for row in rows {
            let (id, existing_salt, expected, remaining, revoked) = row?;
            let actual = invite_hash(code.as_bytes(), &existing_salt)?;
            if bool::from(actual.as_slice().ct_eq(&expected)) {
                matching = Some((id, remaining, revoked));
            }
        }
        drop(statement);
        if let Some((id, remaining, revoked)) = matching {
            if remaining > 0 && !revoked {
                bail!("invite already exists and still has uses remaining");
            }
            transaction.execute(
                "UPDATE invites SET remaining = ?1, revoked = 0 WHERE id = ?2",
                params![uses as i64, id],
            )?;
        } else {
            transaction.execute(
                "INSERT INTO invites(salt, code_hash, remaining) VALUES (?1, ?2, ?3)",
                params![salt.as_slice(), hash.as_slice(), uses as i64],
            )?;
        }
        transaction.commit()?;
        Ok(())
    })
    .await?;
    println!("invite added in {}", db.path.display());
    Ok(())
}

async fn revoke_invite(db: Db, code: String) -> Result<()> {
    validate_invite(&code).map_err(|error| anyhow::anyhow!(error.message))?;
    let revoked = db
        .run(move |connection| {
            let mut statement = connection.prepare("SELECT id, salt, code_hash FROM invites")?;
            let rows = statement.query_map([], |row| {
                Ok((
                    row.get::<_, i64>(0)?,
                    row.get::<_, Vec<u8>>(1)?,
                    row.get::<_, Vec<u8>>(2)?,
                ))
            })?;
            let mut matching = Vec::new();
            for row in rows {
                let (id, salt, expected) = row?;
                let actual = invite_hash(code.as_bytes(), &salt)?;
                if actual.as_slice().ct_eq(&expected).into() {
                    matching.push(id);
                }
            }
            drop(statement);
            for id in &matching {
                connection.execute("UPDATE invites SET revoked = 1 WHERE id = ?1", [id])?;
            }
            Ok(!matching.is_empty())
        })
        .await?;
    if !revoked {
        bail!("invite not found");
    }
    println!("invite revoked");
    Ok(())
}

async fn register_client(
    ConnectInfo(source): ConnectInfo<SocketAddr>,
    State(state): State<AppState>,
    request: Request,
) -> ApiResult<Response> {
    rate_limit(&state, format!("enroll:{}", source.ip()), 10, 60)?;
    let body: RegistrationRequest =
        read_json(request, OTHER_BODY_LIMIT, "request_too_large").await?;
    validate_invite(&body.invite_code)?;
    let auth_key = decode_fixed::<32>(&body.auth_public_key, "invalid_encoding")?;
    let encryption_key = decode_fixed::<32>(&body.encryption_public_key, "invalid_encoding")?;
    let encryption_signature =
        decode_fixed::<64>(&body.encryption_key_signature, "invalid_encoding")?;
    let registration_signature =
        decode_fixed::<64>(&body.registration_signature, "invalid_encoding")?;
    let verifying_key = VerifyingKey::from_bytes(&auth_key).map_err(|_| invalid_signature())?;

    let mut binding_input = Vec::with_capacity(KEY_PREFIX.len() + encryption_key.len());
    binding_input.extend_from_slice(KEY_PREFIX);
    binding_input.extend_from_slice(&encryption_key);
    verifying_key
        .verify(
            &binding_input,
            &Signature::from_bytes(&encryption_signature),
        )
        .map_err(|_| invalid_signature())?;

    let invite_length: u16 = body
        .invite_code
        .len()
        .try_into()
        .map_err(|_| invalid_invite())?;
    let mut registration_input = Vec::with_capacity(169 + body.invite_code.len());
    registration_input.extend_from_slice(REGISTER_PREFIX);
    registration_input.extend_from_slice(&invite_length.to_be_bytes());
    registration_input.extend_from_slice(body.invite_code.as_bytes());
    registration_input.extend_from_slice(&auth_key);
    registration_input.extend_from_slice(&encryption_key);
    registration_input.extend_from_slice(&encryption_signature);
    verifying_key
        .verify(
            &registration_input,
            &Signature::from_bytes(&registration_signature),
        )
        .map_err(|_| invalid_signature())?;

    let client_id = derive_client_id(&auth_key);
    let stored_client_id = client_id.clone();
    let now = Utc::now().timestamp();
    let ttl = state.config.account_ttl_seconds;
    let invite_code = body.invite_code;
    let outcome = state
        .db
        .run(move |connection| {
            let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
            expire_accounts(&transaction, now, ttl)?;
            let existing = transaction
                .query_row(
                    "SELECT auth_key, encryption_key, encryption_signature FROM clients
                     WHERE auth_key = ?1 OR client_id = ?2",
                    params![auth_key.as_slice(), stored_client_id],
                    |row| {
                        Ok((
                            row.get::<_, Vec<u8>>(0)?,
                            row.get::<_, Vec<u8>>(1)?,
                            row.get::<_, Vec<u8>>(2)?,
                        ))
                    },
                )
                .optional()?;
            if let Some((stored_auth, stored_key, stored_signature)) = existing {
                if stored_auth == auth_key
                    && stored_key == encryption_key
                    && stored_signature == encryption_signature
                {
                    transaction.execute(
                        "UPDATE clients SET last_active = ?1 WHERE auth_key = ?2",
                        params![now, auth_key.as_slice()],
                    )?;
                    transaction.commit()?;
                    return Ok(RegistrationOutcome::Existing);
                }
                transaction.commit()?;
                return Ok(RegistrationOutcome::Conflict);
            }

            let mut statement = transaction.prepare(
                "SELECT id, salt, code_hash FROM invites WHERE revoked = 0 AND remaining > 0",
            )?;
            let rows = statement.query_map([], |row| {
                Ok((row.get::<_, i64>(0)?, row.get::<_, Vec<u8>>(1)?, row.get::<_, Vec<u8>>(2)?))
            })?;
            let mut invite_id = None;
            for row in rows {
                let (id, salt, expected) = row?;
                let actual = invite_hash(invite_code.as_bytes(), &salt)?;
                if actual.as_slice().ct_eq(&expected).into() {
                    invite_id = Some(id);
                }
            }
            drop(statement);
            let Some(invite_id) = invite_id else {
                transaction.commit()?;
                return Ok(RegistrationOutcome::InvalidInvite);
            };
            transaction.execute(
                "INSERT INTO clients(client_id, auth_key, encryption_key, encryption_signature, last_active)
                 VALUES (?1, ?2, ?3, ?4, ?5)",
                params![stored_client_id, auth_key.as_slice(), encryption_key.as_slice(), encryption_signature.as_slice(), now],
            )?;
            transaction.execute(
                "UPDATE invites SET remaining = remaining - 1 WHERE id = ?1 AND revoked = 0 AND remaining > 0",
                [invite_id],
            )?;
            transaction.commit()?;
            Ok(RegistrationOutcome::Created)
        })
        .await
        .map_err(ApiError::internal)?;

    let response = ClientIdResponse { client_id };
    match outcome {
        RegistrationOutcome::Created => {
            Ok((StatusCode::CREATED, axum::Json(response)).into_response())
        }
        RegistrationOutcome::Existing => Ok((StatusCode::OK, axum::Json(response)).into_response()),
        RegistrationOutcome::Conflict => Err(ApiError::new(
            StatusCode::CONFLICT,
            "identity_conflict",
            "Authentication key is already bound to different encryption-key data",
        )),
        RegistrationOutcome::InvalidInvite => Err(invalid_invite()),
    }
}

enum RegistrationOutcome {
    Created,
    Existing,
    Conflict,
    InvalidInvite,
}

async fn create_challenge(
    ConnectInfo(source): ConnectInfo<SocketAddr>,
    State(state): State<AppState>,
    request: Request,
) -> ApiResult<Response> {
    rate_limit(&state, format!("challenge:{}", source.ip()), 20, 60)?;
    let body: ChallengeRequest = read_json(request, OTHER_BODY_LIMIT, "request_too_large").await?;
    decode_client_id(&body.client_id)?;
    let challenge_id = random_bytes::<16>();
    let challenge = random_bytes::<32>();
    let now = Utc::now().timestamp();
    let client_id = body.client_id;
    let ttl = state.config.account_ttl_seconds;
    let inserted = state
        .db
        .run(move |connection| {
            let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
            expire_accounts(&transaction, now, ttl)?;
            expire_ephemeral(&transaction, now)?;
            let exists = transaction.query_row(
                "SELECT EXISTS(SELECT 1 FROM clients WHERE client_id = ?1)",
                [&client_id],
                |row| row.get::<_, bool>(0),
            )?;
            if exists {
                transaction.execute(
                    "INSERT INTO challenges(challenge_id, client_id, challenge, expires_at) VALUES (?1, ?2, ?3, ?4)",
                    params![challenge_id.as_slice(), client_id, challenge.as_slice(), now + 60],
                )?;
            }
            transaction.commit()?;
            Ok(exists)
        })
        .await
        .map_err(ApiError::internal)?;
    if !inserted {
        return Err(client_not_found());
    }
    let response = ChallengeResponse {
        challenge_id: encode(&challenge_id),
        challenge: encode(&challenge),
        expires_in: 60,
    };
    Ok(no_store(
        (StatusCode::CREATED, axum::Json(response)).into_response(),
    ))
}

async fn create_session(
    ConnectInfo(source): ConnectInfo<SocketAddr>,
    State(state): State<AppState>,
    request: Request,
) -> ApiResult<Response> {
    rate_limit(&state, format!("session:{}", source.ip()), 20, 60)?;
    let body: SessionRequest = read_json(request, OTHER_BODY_LIMIT, "request_too_large").await?;
    decode_client_id(&body.client_id)?;
    let challenge_id = decode_fixed::<16>(&body.challenge_id, "invalid_encoding")?;
    let signature = decode_fixed::<64>(&body.signature, "invalid_encoding")?;
    let token = random_bytes::<32>();
    let token_hash = Sha256::digest(token);
    let now = Utc::now().timestamp();
    let ttl = state.config.account_ttl_seconds;
    let client_id = body.client_id;

    let outcome = state
        .db
        .run(move |connection| {
            let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
            expire_accounts(&transaction, now, ttl)?;
            let auth_key = transaction
                .query_row(
                    "SELECT auth_key FROM clients WHERE client_id = ?1",
                    [&client_id],
                    |row| row.get::<_, Vec<u8>>(0),
                )
                .optional()?;
            let Some(auth_key) = auth_key else {
                transaction.commit()?;
                return Ok(SessionOutcome::ClientNotFound);
            };
            let challenge_row = transaction
                .query_row(
                    "SELECT challenge, expires_at FROM challenges WHERE challenge_id = ?1 AND client_id = ?2",
                    params![challenge_id.as_slice(), client_id],
                    |row| Ok((row.get::<_, Vec<u8>>(0)?, row.get::<_, i64>(1)?)),
                )
                .optional()?;
            transaction.execute(
                "DELETE FROM challenges WHERE challenge_id = ?1 AND client_id = ?2",
                params![challenge_id.as_slice(), client_id],
            )?;
            let Some((challenge, expires_at)) = challenge_row else {
                transaction.commit()?;
                return Ok(SessionOutcome::InvalidChallenge);
            };
            if expires_at <= now {
                transaction.commit()?;
                return Ok(SessionOutcome::InvalidChallenge);
            }
            let key_bytes: [u8; 32] = auth_key.try_into().map_err(|_| anyhow::anyhow!("invalid auth key in database"))?;
            let verifying_key = VerifyingKey::from_bytes(&key_bytes)?;
            let mut auth_input = Vec::with_capacity(AUTH_PREFIX.len() + 48);
            auth_input.extend_from_slice(AUTH_PREFIX);
            auth_input.extend_from_slice(&challenge_id);
            auth_input.extend_from_slice(&challenge);
            if verifying_key.verify(&auth_input, &Signature::from_bytes(&signature)).is_err() {
                transaction.commit()?;
                return Ok(SessionOutcome::InvalidSignature);
            }
            transaction.execute(
                "INSERT INTO sessions(token_hash, client_id, expires_at) VALUES (?1, ?2, ?3)",
                params![token_hash.as_slice(), client_id, now + 900],
            )?;
            transaction.execute(
                "UPDATE clients SET last_active = ?1 WHERE client_id = ?2",
                params![now, client_id],
            )?;
            transaction.commit()?;
            Ok(SessionOutcome::Created)
        })
        .await
        .map_err(ApiError::internal)?;

    match outcome {
        SessionOutcome::Created => Ok(no_store(
            (
                StatusCode::CREATED,
                axum::Json(SessionResponse {
                    access_token: encode(&token),
                    token_type: "Bearer",
                    expires_in: 900,
                }),
            )
                .into_response(),
        )),
        SessionOutcome::ClientNotFound => Err(client_not_found()),
        SessionOutcome::InvalidChallenge => Err(ApiError::new(
            StatusCode::UNAUTHORIZED,
            "invalid_challenge",
            "Invalid authentication challenge",
        )),
        SessionOutcome::InvalidSignature => Err(invalid_signature()),
    }
}

enum SessionOutcome {
    Created,
    ClientNotFound,
    InvalidChallenge,
    InvalidSignature,
}

async fn get_client(
    State(state): State<AppState>,
    AxumPath(client_id): AxumPath<String>,
    request: Request,
) -> ApiResult<Response> {
    decode_client_id(&client_id)?;
    let authenticated_id = authenticate(&state, request.headers()).await?;
    rate_limit(&state, format!("lookup:{authenticated_id}"), 240, 60)?;
    let now = Utc::now().timestamp();
    let ttl = state.config.account_ttl_seconds;
    let lookup_id = client_id.clone();
    let client = state
        .db
        .run(move |connection| {
            let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
            expire_accounts(&transaction, now, ttl)?;
            let row = transaction
                .query_row(
                    "SELECT auth_key, encryption_key, encryption_signature FROM clients WHERE client_id = ?1",
                    [&lookup_id],
                    |row| Ok((row.get::<_, Vec<u8>>(0)?, row.get::<_, Vec<u8>>(1)?, row.get::<_, Vec<u8>>(2)?)),
                )
                .optional()?;
            transaction.commit()?;
            Ok(row)
        })
        .await
        .map_err(ApiError::internal)?;
    let Some((auth_key, encryption_key, encryption_signature)) = client else {
        return Err(client_not_found());
    };
    Ok(axum::Json(ClientResponse {
        client_id,
        auth_public_key: encode(&auth_key),
        encryption_public_key: encode(&encryption_key),
        encryption_key_signature: encode(&encryption_signature),
    })
    .into_response())
}

async fn submit_message(State(state): State<AppState>, request: Request) -> ApiResult<Response> {
    let sender_id = authenticate(&state, request.headers()).await?;
    rate_limit(&state, format!("submit:{sender_id}"), 120, 60)?;
    let body: SubmitMessageRequest =
        read_json(request, MESSAGE_BODY_LIMIT, "request_too_large").await?;
    let recipient_bytes = decode_client_id(&body.recipient_id)?;
    let client_message_id = decode_fixed::<16>(&body.client_message_id, "invalid_encoding")?;
    let enc = decode_fixed::<32>(&body.enc, "invalid_encoding")?;
    let ciphertext = decode_canonical(&body.ciphertext, "invalid_encoding")?;
    if ciphertext.len() > MAX_CIPHERTEXT {
        return Err(ApiError::new(
            StatusCode::PAYLOAD_TOO_LARGE,
            "message_too_large",
            "Message ciphertext is too large",
        ));
    }
    let mut fingerprint_input = Vec::with_capacity(48 + ciphertext.len());
    fingerprint_input.extend_from_slice(&recipient_bytes);
    fingerprint_input.extend_from_slice(&enc);
    fingerprint_input.extend_from_slice(&ciphertext);
    let fingerprint = Sha256::digest(&fingerprint_input);
    let recipient_id = body.recipient_id;
    let now = Utc::now().timestamp();
    let expires_at = now
        .checked_add(state.config.message_ttl_seconds as i64)
        .ok_or_else(|| ApiError::internal("message expiry overflow"))?;
    let sender_for_db = sender_id.clone();
    let outcome = state
        .db
        .run(move |connection| {
            let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
            let recipient_exists = transaction.query_row(
                "SELECT EXISTS(SELECT 1 FROM clients WHERE client_id = ?1)",
                [&recipient_id],
                |row| row.get::<_, bool>(0),
            )?;
            if !recipient_exists {
                transaction.commit()?;
                return Ok(MessageOutcome::RecipientNotFound);
            }
            let existing = transaction
                .query_row(
                    "SELECT sequence, fingerprint, received_at, expires_at FROM messages
                     WHERE sender_id = ?1 AND client_message_id = ?2",
                    params![sender_for_db, client_message_id.as_slice()],
                    |row| Ok((row.get::<_, i64>(0)?, row.get::<_, Vec<u8>>(1)?, row.get::<_, i64>(2)?, row.get::<_, i64>(3)?)),
                )
                .optional()?;
            if let Some((sequence, stored_fingerprint, received_at, stored_expiry)) = existing {
                transaction.commit()?;
                return Ok(if stored_fingerprint.as_slice().ct_eq(fingerprint.as_slice()).into() {
                    MessageOutcome::Existing(sequence, received_at, stored_expiry)
                } else {
                    MessageOutcome::Conflict
                });
            }
            transaction.execute(
                "INSERT INTO messages(sender_id, recipient_id, client_message_id, enc, ciphertext, fingerprint, received_at, expires_at)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
                params![sender_for_db, recipient_id, client_message_id.as_slice(), enc.as_slice(), ciphertext, fingerprint.as_slice(), now, expires_at],
            )?;
            let sequence = transaction.last_insert_rowid();
            transaction.commit()?;
            Ok(MessageOutcome::Created(sequence))
        })
        .await
        .map_err(ApiError::internal)?;

    match outcome {
        MessageOutcome::Created(sequence) => Ok((
            StatusCode::CREATED,
            axum::Json(message_receipt(sequence, now, expires_at)),
        )
            .into_response()),
        MessageOutcome::Existing(sequence, received, expires) => Ok((
            StatusCode::OK,
            axum::Json(message_receipt(sequence, received, expires)),
        )
            .into_response()),
        MessageOutcome::Conflict => Err(ApiError::new(
            StatusCode::CONFLICT,
            "message_id_conflict",
            "Client message ID is already used for different message data",
        )),
        MessageOutcome::RecipientNotFound => Err(client_not_found()),
    }
}

enum MessageOutcome {
    Created(i64),
    Existing(i64, i64, i64),
    Conflict,
    RecipientNotFound,
}

async fn poll_messages(
    State(state): State<AppState>,
    RawQuery(raw_query): RawQuery,
    request: Request,
) -> ApiResult<Response> {
    let client_id = authenticate(&state, request.headers()).await?;
    rate_limit(&state, format!("poll:{client_id}"), 120, 60)?;
    let (after_text, limit) = parse_poll_query(raw_query.as_deref())?;
    let after = match after_text {
        Some(cursor) => i64::from_be_bytes(decode_fixed::<8>(&cursor, "invalid_encoding")?),
        None => 0,
    };
    if after < 0 {
        return Err(ApiError::new(
            StatusCode::BAD_REQUEST,
            "invalid_encoding",
            "Invalid cursor",
        ));
    }
    let now = Utc::now().timestamp();
    let recipient = client_id.clone();
    let rows = state
        .db
        .run(move |connection| {
            let mut statement = connection.prepare(
                "SELECT sequence, sender_id, client_message_id, enc, ciphertext, received_at, expires_at
                 FROM messages
                 WHERE recipient_id = ?1 AND sequence > ?2 AND acknowledged = 0
                   AND expires_at > ?3 AND enc IS NOT NULL AND ciphertext IS NOT NULL
                 ORDER BY sequence LIMIT ?4",
            )?;
            let messages = statement
                .query_map(params![recipient, after, now, limit], |row| {
                    Ok((
                        row.get::<_, i64>(0)?,
                        row.get::<_, String>(1)?,
                        row.get::<_, Vec<u8>>(2)?,
                        row.get::<_, Vec<u8>>(3)?,
                        row.get::<_, Vec<u8>>(4)?,
                        row.get::<_, i64>(5)?,
                        row.get::<_, i64>(6)?,
                    ))
                })?
                .collect::<std::result::Result<Vec<_>, _>>()?;
            Ok(messages)
        })
        .await
        .map_err(ApiError::internal)?;
    let cursor_sequence = rows.last().map_or(after, |row| row.0);
    let messages = rows
        .into_iter()
        .map(
            |(sequence, sender_id, client_message_id, enc, ciphertext, received, expires)| {
                MessageResponse {
                    server_message_id: format_message_id(sequence),
                    sender_id,
                    client_message_id: encode(&client_message_id),
                    enc: encode(&enc),
                    ciphertext: encode(&ciphertext),
                    received_at: timestamp(received),
                    expires_at: timestamp(expires),
                }
            },
        )
        .collect();
    Ok(axum::Json(PollResponse {
        messages,
        cursor: encode(&cursor_sequence.to_be_bytes()),
    })
    .into_response())
}

async fn ack_messages(State(state): State<AppState>, request: Request) -> ApiResult<Response> {
    let client_id = authenticate(&state, request.headers()).await?;
    rate_limit(&state, format!("ack:{client_id}"), 120, 60)?;
    let body: AckRequest = read_json(request, OTHER_BODY_LIMIT, "request_too_large").await?;
    if body.server_message_ids.is_empty() || body.server_message_ids.len() > 100 {
        return Err(ApiError::new(
            StatusCode::BAD_REQUEST,
            "invalid_message",
            "Provide 1 through 100 message IDs",
        ));
    }
    let mut unique = HashSet::with_capacity(body.server_message_ids.len());
    let mut sequences = Vec::with_capacity(body.server_message_ids.len());
    for id in body.server_message_ids {
        let sequence = parse_message_id(&id)?;
        if !unique.insert(sequence) {
            return Err(ApiError::new(
                StatusCode::BAD_REQUEST,
                "invalid_message",
                "Message IDs must be distinct",
            ));
        }
        sequences.push(sequence);
    }
    let requested = sequences.len();
    let recipient = client_id;
    let acknowledged = state
        .db
        .run(move |connection| {
            let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
            for sequence in &sequences {
                let owner = transaction
                    .query_row(
                        "SELECT recipient_id FROM messages WHERE sequence = ?1",
                        [sequence],
                        |row| row.get::<_, String>(0),
                    )
                    .optional()?;
                if owner.as_deref() != Some(recipient.as_str()) {
                    transaction.rollback()?;
                    return Ok(false);
                }
            }
            for sequence in sequences {
                transaction.execute(
                    "UPDATE messages SET acknowledged = 1, enc = NULL, ciphertext = NULL WHERE sequence = ?1",
                    [sequence],
                )?;
            }
            transaction.commit()?;
            Ok(true)
        })
        .await
        .map_err(ApiError::internal)?;
    if !acknowledged {
        return Err(ApiError::new(
            StatusCode::NOT_FOUND,
            "message_not_found",
            "Message not found",
        ));
    }
    Ok(axum::Json(AckResponse { acked: requested }).into_response())
}

async fn authenticate(state: &AppState, headers: &HeaderMap) -> ApiResult<String> {
    let authorization = headers
        .get(header::AUTHORIZATION)
        .and_then(|value| value.to_str().ok())
        .ok_or_else(ApiError::invalid_token)?;
    let token_text = authorization
        .strip_prefix("Bearer ")
        .ok_or_else(ApiError::invalid_token)?;
    let token =
        decode_fixed::<32>(token_text, "invalid_token").map_err(|_| ApiError::invalid_token())?;
    let token_hash = Sha256::digest(token);
    let now = Utc::now().timestamp();
    let ttl = state.config.account_ttl_seconds;
    state
        .db
        .run(move |connection| {
            let transaction =
                connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
            let client_id = transaction
                .query_row(
                    "SELECT client_id FROM sessions WHERE token_hash = ?1 AND expires_at > ?2",
                    params![token_hash.as_slice(), now],
                    |row| row.get::<_, String>(0),
                )
                .optional()?;
            if let Some(client_id) = &client_id {
                transaction.execute(
                    "UPDATE clients SET last_active = ?1 WHERE client_id = ?2",
                    params![now, client_id],
                )?;
            }
            expire_accounts(&transaction, now, ttl)?;
            expire_ephemeral(&transaction, now)?;
            transaction.commit()?;
            Ok(client_id)
        })
        .await
        .map_err(ApiError::internal)?
        .ok_or_else(ApiError::invalid_token)
}

fn expire_accounts(connection: &Connection, now: i64, ttl: u64) -> Result<()> {
    if ttl > 0 {
        let cutoff = now.saturating_sub(ttl.min(i64::MAX as u64) as i64);
        connection.execute("DELETE FROM clients WHERE last_active <= ?1", [cutoff])?;
    }
    Ok(())
}

fn expire_ephemeral(connection: &Connection, now: i64) -> Result<()> {
    connection.execute("DELETE FROM challenges WHERE expires_at <= ?1", [now])?;
    connection.execute("DELETE FROM sessions WHERE expires_at <= ?1", [now])?;
    connection.execute(
        "UPDATE messages SET enc = NULL, ciphertext = NULL WHERE expires_at <= ?1",
        [now],
    )?;
    Ok(())
}

fn expiry_sweep_period(message_ttl_seconds: u64) -> Duration {
    Duration::from_secs(message_ttl_seconds.min(60).max(1))
}

async fn run_expiry_loop(state: AppState) {
    let mut ticks = interval(expiry_sweep_period(state.config.message_ttl_seconds));
    ticks.set_missed_tick_behavior(MissedTickBehavior::Delay);
    loop {
        ticks.tick().await;
        let now = Utc::now().timestamp();
        let ttl = state.config.account_ttl_seconds;
        if let Err(error) = state
            .db
            .run(move |connection| {
                expire_accounts(connection, now, ttl)?;
                expire_ephemeral(connection, now)?;
                Ok(())
            })
            .await
        {
            eprintln!("expiry cleanup failed: {error:#}");
        }
    }
}

fn parse_poll_query(query: Option<&str>) -> ApiResult<(Option<String>, u16)> {
    let pairs: Vec<(String, String)> = query
        .map(serde_urlencoded::from_str)
        .transpose()
        .map_err(|_| ApiError::new(StatusCode::BAD_REQUEST, "invalid_limit", "Invalid query"))?
        .unwrap_or_default();
    let mut after = None;
    let mut limit = None;
    for (key, value) in pairs {
        match key.as_str() {
            "after" if after.is_none() => after = Some(value),
            "limit" if limit.is_none() => {
                limit = Some(value.parse::<u16>().map_err(|_| {
                    ApiError::new(
                        StatusCode::BAD_REQUEST,
                        "invalid_limit",
                        "Limit must be from 1 through 100",
                    )
                })?)
            }
            "after" => {
                return Err(ApiError::new(
                    StatusCode::BAD_REQUEST,
                    "invalid_encoding",
                    "Duplicate cursor",
                ));
            }
            "limit" => {
                return Err(ApiError::new(
                    StatusCode::BAD_REQUEST,
                    "invalid_limit",
                    "Duplicate limit",
                ));
            }
            _ => {}
        }
    }
    let limit = limit.unwrap_or(50);
    if !(1..=100).contains(&limit) {
        return Err(ApiError::new(
            StatusCode::BAD_REQUEST,
            "invalid_limit",
            "Limit must be from 1 through 100",
        ));
    }
    Ok((after, limit))
}

async fn read_json<T: DeserializeOwned>(
    request: Request,
    limit: usize,
    too_large_code: &'static str,
) -> ApiResult<T> {
    let content_type = request
        .headers()
        .get(header::CONTENT_TYPE)
        .and_then(|value| value.to_str().ok())
        .and_then(|value| value.split(';').next())
        .map(str::trim);
    if !content_type.is_some_and(|value| value.eq_ignore_ascii_case("application/json")) {
        return Err(ApiError::invalid_json());
    }
    let bytes = to_bytes(request.into_body(), limit).await.map_err(|_| {
        ApiError::new(
            StatusCode::PAYLOAD_TOO_LARGE,
            too_large_code,
            "Request body is too large",
        )
    })?;
    parse_strict(&bytes).map_err(|_| ApiError::invalid_json())
}

fn parse_strict<T: DeserializeOwned>(bytes: &[u8]) -> Result<T> {
    let mut deserializer = serde_json::Deserializer::from_slice(bytes);
    let value = StrictValue::deserialize(&mut deserializer)?;
    deserializer.end()?;
    Ok(serde_json::from_value(value.0)?)
}

struct StrictValue(Value);

impl<'de> Deserialize<'de> for StrictValue {
    fn deserialize<D>(deserializer: D) -> std::result::Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        deserializer.deserialize_any(StrictVisitor)
    }
}

struct StrictVisitor;

impl<'de> serde::de::Visitor<'de> for StrictVisitor {
    type Value = StrictValue;

    fn expecting(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("a JSON value without duplicate object members")
    }

    fn visit_bool<E>(self, value: bool) -> std::result::Result<Self::Value, E> {
        Ok(StrictValue(Value::Bool(value)))
    }

    fn visit_i64<E>(self, value: i64) -> std::result::Result<Self::Value, E> {
        Ok(StrictValue(Value::Number(Number::from(value))))
    }

    fn visit_u64<E>(self, value: u64) -> std::result::Result<Self::Value, E> {
        Ok(StrictValue(Value::Number(Number::from(value))))
    }

    fn visit_f64<E: serde::de::Error>(self, value: f64) -> std::result::Result<Self::Value, E> {
        Number::from_f64(value)
            .map(Value::Number)
            .map(StrictValue)
            .ok_or_else(|| E::custom("non-finite JSON number"))
    }

    fn visit_str<E>(self, value: &str) -> std::result::Result<Self::Value, E> {
        Ok(StrictValue(Value::String(value.into())))
    }

    fn visit_string<E>(self, value: String) -> std::result::Result<Self::Value, E> {
        Ok(StrictValue(Value::String(value)))
    }

    fn visit_none<E>(self) -> std::result::Result<Self::Value, E> {
        Ok(StrictValue(Value::Null))
    }

    fn visit_unit<E>(self) -> std::result::Result<Self::Value, E> {
        Ok(StrictValue(Value::Null))
    }

    fn visit_seq<A>(self, mut sequence: A) -> std::result::Result<Self::Value, A::Error>
    where
        A: serde::de::SeqAccess<'de>,
    {
        let mut values = Vec::new();
        while let Some(value) = sequence.next_element::<StrictValue>()? {
            values.push(value.0);
        }
        Ok(StrictValue(Value::Array(values)))
    }

    fn visit_map<A>(self, mut object: A) -> std::result::Result<Self::Value, A::Error>
    where
        A: serde::de::MapAccess<'de>,
    {
        let mut values = Map::new();
        while let Some(key) = object.next_key::<String>()? {
            if values.contains_key(&key) {
                return Err(serde::de::Error::custom(format!(
                    "duplicate member {key:?}"
                )));
            }
            values.insert(key, object.next_value::<StrictValue>()?.0);
        }
        Ok(StrictValue(Value::Object(values)))
    }
}

fn validate_invite(code: &str) -> ApiResult<()> {
    if !(12..=64).contains(&code.len()) || !code.bytes().all(|byte| (0x20..=0x7e).contains(&byte)) {
        return Err(invalid_invite());
    }
    Ok(())
}

fn invite_hash(code: &[u8], salt: &[u8]) -> Result<[u8; 32]> {
    let mut hash = [0_u8; 32];
    Argon2::default()
        .hash_password_into(code, salt, &mut hash)
        .map_err(|error| anyhow::anyhow!("invite hashing failed: {error}"))?;
    Ok(hash)
}

fn decode_client_id(value: &str) -> ApiResult<[u8; 16]> {
    decode_fixed(value, "invalid_client_id").map_err(|_| {
        ApiError::new(
            StatusCode::BAD_REQUEST,
            "invalid_client_id",
            "Invalid client ID",
        )
    })
}

fn decode_fixed<const N: usize>(value: &str, code: &'static str) -> ApiResult<[u8; N]> {
    let bytes = decode_canonical(value, code)?;
    bytes
        .try_into()
        .map_err(|_| ApiError::new(StatusCode::BAD_REQUEST, code, "Invalid binary encoding"))
}

fn decode_canonical(value: &str, code: &'static str) -> ApiResult<Vec<u8>> {
    let decoded = URL_SAFE_NO_PAD
        .decode(value)
        .map_err(|_| ApiError::new(StatusCode::BAD_REQUEST, code, "Invalid binary encoding"))?;
    if encode(&decoded) != value {
        return Err(ApiError::new(
            StatusCode::BAD_REQUEST,
            code,
            "Invalid binary encoding",
        ));
    }
    Ok(decoded)
}

fn encode(bytes: &[u8]) -> String {
    URL_SAFE_NO_PAD.encode(bytes)
}

fn derive_client_id(auth_key: &[u8; 32]) -> String {
    let mut digest = Sha256::new();
    digest.update(ID_PREFIX);
    digest.update(auth_key);
    encode(&digest.finalize()[..16])
}

fn random_bytes<const N: usize>() -> [u8; N] {
    let mut bytes = [0_u8; N];
    OsRng.fill_bytes(&mut bytes);
    bytes
}

fn rate_limit(state: &AppState, key: String, maximum: usize, seconds: u64) -> ApiResult<()> {
    state
        .limits
        .check(key, maximum, Duration::from_secs(seconds))
        .map_err(ApiError::rate_limited)
}

fn message_receipt(sequence: i64, received_at: i64, expires_at: i64) -> SubmitMessageResponse {
    SubmitMessageResponse {
        server_message_id: format_message_id(sequence),
        received_at: timestamp(received_at),
        expires_at: timestamp(expires_at),
    }
}

fn format_message_id(sequence: i64) -> String {
    format!("{sequence:016x}")
}

fn parse_message_id(value: &str) -> ApiResult<i64> {
    if value.len() != 16
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err(ApiError::new(
            StatusCode::BAD_REQUEST,
            "invalid_message",
            "Invalid server message ID",
        ));
    }
    i64::from_str_radix(value, 16)
        .ok()
        .filter(|value| *value > 0)
        .ok_or_else(|| {
            ApiError::new(
                StatusCode::BAD_REQUEST,
                "invalid_message",
                "Invalid server message ID",
            )
        })
}

fn timestamp(seconds: i64) -> String {
    chrono::DateTime::from_timestamp(seconds, 0)
        .expect("database timestamp out of range")
        .to_rfc3339_opts(SecondsFormat::Secs, true)
}

fn no_store(mut response: Response) -> Response {
    response
        .headers_mut()
        .insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    response
}

fn invalid_invite() -> ApiError {
    ApiError::new(StatusCode::UNAUTHORIZED, "invalid_invite", "Invalid invite")
}

fn invalid_signature() -> ApiError {
    ApiError::new(
        StatusCode::UNAUTHORIZED,
        "invalid_signature",
        "Invalid signature",
    )
}

fn client_not_found() -> ApiError {
    ApiError::new(
        StatusCode::NOT_FOUND,
        "client_not_found",
        "Client not found",
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::{body::Body, http::Method};
    use ed25519_dalek::{Signer, SigningKey};
    use serde_json::json;
    use std::os::unix::fs::PermissionsExt;
    use tower::ServiceExt;

    #[test]
    fn identity_vector() {
        let auth_key = hex("a73ae1147dab604bf355e61ff431b93c2d6be5560a604bf63af6b751d8637b9a");
        assert_eq!(
            derive_client_id(&auth_key.try_into().unwrap()),
            "Xx18SuN26XEApaxndPMsJQ"
        );
    }

    #[test]
    fn binding_and_auth_vectors() {
        let key = VerifyingKey::from_bytes(
            &hex("a73ae1147dab604bf355e61ff431b93c2d6be5560a604bf63af6b751d8637b9a")
                .try_into()
                .unwrap(),
        )
        .unwrap();
        let encryption_key =
            hex("43bd924b7521c689f6a2f2e51f629bbf7a8439c373b0f3066019c5b1d6fcfe60");
        let mut binding = KEY_PREFIX.to_vec();
        binding.extend_from_slice(&encryption_key);
        let signature = decode_fixed::<64>(
            "RyFC1wFSYW2EGjFZeWFtl_X857effDxUD8KbZa2IbHw468AzfC7Z3dK6-4ae3icVr_2kGC7hsjmn7BvxJm-8Ag",
            "invalid_encoding",
        )
        .unwrap();
        key.verify(&binding, &Signature::from_bytes(&signature))
            .unwrap();
        binding[20] ^= 1;
        assert!(
            key.verify(&binding, &Signature::from_bytes(&signature))
                .is_err()
        );

        let mut auth = AUTH_PREFIX.to_vec();
        auth.extend_from_slice(&hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"));
        auth.extend_from_slice(&hex(
            "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
        ));
        let signature = decode_fixed::<64>(
            "Qmqsqn-871BZrNbttmKqg3yF2USySDgV-WoNS75zKm2TpXJQMhgeNyoAzHXK9Dm3fDVgWvhSxriwJqyQgoVHDA",
            "invalid_encoding",
        )
        .unwrap();
        key.verify(&auth, &Signature::from_bytes(&signature))
            .unwrap();

        let mut registration = REGISTER_PREFIX.to_vec();
        registration.extend_from_slice(&21_u16.to_be_bytes());
        registration.extend_from_slice(b"correct horse battery");
        registration.extend_from_slice(key.as_bytes());
        registration.extend_from_slice(&encryption_key);
        registration.extend_from_slice(&decode_fixed::<64>(
            "RyFC1wFSYW2EGjFZeWFtl_X857effDxUD8KbZa2IbHw468AzfC7Z3dK6-4ae3icVr_2kGC7hsjmn7BvxJm-8Ag",
            "invalid_encoding",
        ).unwrap());
        let signature = decode_fixed::<64>(
            "d280k8CyIY3ZrT2-Emjn4ciqJdPHwnUmbL9QxXq9CY5MOxo1j-OZ3Lp8nnTTBmgDfoIrDkmf3QilIrTw6zaOAg",
            "invalid_encoding",
        )
        .unwrap();
        key.verify(&registration, &Signature::from_bytes(&signature))
            .unwrap();
    }

    #[test]
    fn strict_json_rejects_duplicate_members() {
        assert!(parse_strict::<ChallengeRequest>(br#"{"client_id":"a","client_id":"b"}"#).is_err());
        assert!(parse_strict::<ChallengeRequest>(br#"{"client_id":"a","future":true}"#).is_ok());
    }

    #[test]
    fn canonical_encodings_and_message_ids() {
        assert!(decode_client_id("Xx18SuN26XEApaxndPMsJQ").is_ok());
        assert!(decode_client_id("Xx18SuN26XEApaxndPMsJR").is_err());
        assert!(decode_client_id("Xx18SuN26XEApaxndPMsJQ==").is_err());
        assert_eq!(parse_message_id("0000000000000001").unwrap(), 1);
        assert!(parse_message_id("000000000000000A").is_err());
    }

    #[test]
    fn config_is_generated_and_invalid_files_are_preserved() {
        let path = std::env::temp_dir().join(format!(
            "rchat-server-config-test-{}.json",
            encode(&random_bytes::<16>())
        ));
        let config = load_config(&path).unwrap();
        let directory = path.parent().unwrap();
        assert_eq!(config.bind, "127.0.0.1:8443");
        assert_eq!(config.database_path, directory.join("rchat.db"));
        assert_eq!(
            config.tls_certificate_path,
            Some(directory.join("server.crt"))
        );
        assert!(path.exists());

        fs::write(&path, b"{broken").unwrap();
        let error = load_config(&path).unwrap_err().to_string();
        assert!(error.contains("delete it to generate a fresh configuration"));
        assert_eq!(fs::read(&path).unwrap(), b"{broken");
        fs::remove_file(path).unwrap();
    }

    #[tokio::test]
    async fn keygen_creates_loadable_files_without_overwriting_them() {
        let suffix = encode(&random_bytes::<16>());
        let certificate_path = std::env::temp_dir().join(format!("rchat-keygen-{suffix}.crt"));
        let key_path = std::env::temp_dir().join(format!("rchat-keygen-{suffix}.key"));
        let config = Config {
            bind: default_bind(),
            database_path: default_database(),
            account_ttl_seconds: 0,
            message_ttl_seconds: default_message_ttl(),
            tls_certificate_path: Some(certificate_path.clone()),
            tls_private_key_path: Some(key_path.clone()),
            allow_insecure_http: false,
        };

        keygen(&config, vec!["localhost".into(), "127.0.0.1".into()]).unwrap();
        load_tls(&certificate_path, &key_path).await.unwrap();
        assert!(matches!(
            select_transport(&config).await.unwrap(),
            Transport::Https(_)
        ));
        let mut dual_transport = config.clone();
        dual_transport.allow_insecure_http = true;
        assert!(matches!(
            select_transport(&dual_transport).await.unwrap(),
            Transport::HttpAndHttps(_)
        ));
        assert_eq!(
            fs::metadata(&key_path).unwrap().permissions().mode() & 0o777,
            0o600
        );
        let original_key = fs::read(&key_path).unwrap();
        assert!(keygen(&config, vec!["localhost".into()]).is_err());
        assert_eq!(fs::read(&key_path).unwrap(), original_key);

        fs::remove_file(certificate_path).unwrap();
        fs::remove_file(key_path).unwrap();
    }

    #[test]
    fn keygen_defaults_to_localhost() {
        let cli = Cli::try_parse_from(["rchat-server", "keygen"]).unwrap();
        let Command::Keygen { hosts } = cli.command else {
            panic!("wrong command");
        };
        assert_eq!(hosts, ["localhost"]);
    }

    #[test]
    fn serve_accepts_host_and_port_overrides() {
        let cli = Cli::try_parse_from(["rchat-server", "serve", "--host", "::1", "--port", "9443"])
            .unwrap();
        let Command::Serve { host, port } = cli.command else {
            panic!("wrong command");
        };
        assert_eq!(host, Some("::1".parse().unwrap()));
        assert_eq!(port, Some(9443));
        assert!(Cli::try_parse_from(["rchat-server", "serve", "--port", "0"]).is_err());
        assert!(Cli::try_parse_from(["rchat-server", "serve", "--host", "localhost"]).is_err());
    }

    #[tokio::test]
    async fn insecure_http_works_without_tls_paths() {
        let config = Config {
            bind: default_bind(),
            database_path: default_database(),
            account_ttl_seconds: 0,
            message_ttl_seconds: default_message_ttl(),
            tls_certificate_path: None,
            tls_private_key_path: None,
            allow_insecure_http: true,
        };
        assert!(matches!(
            select_transport(&config).await.unwrap(),
            Transport::Http
        ));
    }

    #[tokio::test]
    async fn exhausted_invite_can_be_added_again() {
        let database_path = std::env::temp_dir().join(format!(
            "rchat-invite-test-{}.db",
            encode(&random_bytes::<16>())
        ));
        let db = Db::new(database_path.clone());
        db.init().await.unwrap();
        add_invite(db.clone(), "correct horse battery".into(), 1)
            .await
            .unwrap();
        db.run(|connection| {
            connection.execute("UPDATE invites SET remaining = 0", [])?;
            Ok(())
        })
        .await
        .unwrap();

        add_invite(db.clone(), "correct horse battery".into(), 2)
            .await
            .unwrap();
        assert!(
            add_invite(db.clone(), "correct horse battery".into(), 3)
                .await
                .is_err()
        );
        revoke_invite(db.clone(), "correct horse battery".into())
            .await
            .unwrap();
        add_invite(db.clone(), "correct horse battery".into(), 2)
            .await
            .unwrap();
        db.run(|connection| {
            let state: (i64, bool) =
                connection.query_row("SELECT remaining, revoked FROM invites", [], |row| {
                    Ok((row.get(0)?, row.get(1)?))
                })?;
            assert_eq!(state, (2, false));
            Ok(())
        })
        .await
        .unwrap();
        remove_test_database(database_path);
    }

    #[tokio::test]
    async fn enrollment_authentication_and_message_queue() {
        let (router, db, database_path) = test_app(2, 86_400).await;
        let alice = SigningKey::from_bytes(&[1; 32]);
        let bob = SigningKey::from_bytes(&[2; 32]);
        let alice_registration = registration(&alice, [3; 32]);
        let bob_registration = registration(&bob, [4; 32]);
        let alice_id = derive_client_id(alice.verifying_key().as_bytes());
        let bob_id = derive_client_id(bob.verifying_key().as_bytes());

        assert_eq!(
            send_json(
                &router,
                Method::POST,
                "/v1/clients",
                alice_registration.clone(),
                None
            )
            .await
            .status(),
            StatusCode::CREATED
        );
        assert_eq!(
            send_json(
                &router,
                Method::POST,
                "/v1/clients",
                alice_registration,
                None
            )
            .await
            .status(),
            StatusCode::OK
        );
        assert_eq!(
            send_json(&router, Method::POST, "/v1/clients", bob_registration, None)
                .await
                .status(),
            StatusCode::CREATED
        );

        let alice_token = authenticate_client(&router, &alice, &alice_id).await;
        let bob_token = authenticate_client(&router, &bob, &bob_id).await;
        assert_eq!(
            send(
                &router,
                Method::GET,
                &format!("/v1/clients/{bob_id}"),
                Body::empty(),
                Some(&alice_token),
            )
            .await
            .status(),
            StatusCode::OK
        );
        assert_eq!(
            send(
                &router,
                Method::POST,
                "/v1/messages",
                Body::from(vec![b' '; MESSAGE_BODY_LIMIT + 1]),
                Some(&alice_token),
            )
            .await
            .status(),
            StatusCode::PAYLOAD_TOO_LARGE
        );
        assert_eq!(
            send(
                &router,
                Method::GET,
                "/v1/messages?limit=0",
                Body::empty(),
                Some(&bob_token),
            )
            .await
            .status(),
            StatusCode::BAD_REQUEST
        );
        let message = json!({
            "recipient_id": bob_id,
            "client_message_id": encode(&[5; 16]),
            "enc": encode(&[6; 32]),
            "ciphertext": encode(&[7; 48]),
        });
        let created = send_json(
            &router,
            Method::POST,
            "/v1/messages",
            message.clone(),
            Some(&alice_token),
        )
        .await;
        assert_eq!(created.status(), StatusCode::CREATED);
        let receipt = response_json(created).await;
        let message_id = receipt["server_message_id"].as_str().unwrap();
        assert_eq!(
            send_json(
                &router,
                Method::POST,
                "/v1/messages",
                message.clone(),
                Some(&alice_token),
            )
            .await
            .status(),
            StatusCode::OK
        );
        let mut changed = message;
        changed["ciphertext"] = Value::String(encode(&[8; 48]));
        assert_eq!(
            send_json(
                &router,
                Method::POST,
                "/v1/messages",
                changed,
                Some(&alice_token),
            )
            .await
            .status(),
            StatusCode::CONFLICT
        );

        let polled = send(
            &router,
            Method::GET,
            "/v1/messages?limit=50",
            Body::empty(),
            Some(&bob_token),
        )
        .await;
        assert_eq!(polled.status(), StatusCode::OK);
        assert_eq!(
            response_json(polled).await["messages"]
                .as_array()
                .unwrap()
                .len(),
            1
        );

        let ack = json!({"server_message_ids": [message_id]});
        for expected in [StatusCode::OK, StatusCode::OK] {
            assert_eq!(
                send_json(
                    &router,
                    Method::POST,
                    "/v1/messages/ack",
                    ack.clone(),
                    Some(&bob_token),
                )
                .await
                .status(),
                expected
            );
        }
        assert_eq!(
            send_json(
                &router,
                Method::POST,
                "/v1/messages/ack",
                ack,
                Some(&alice_token),
            )
            .await
            .status(),
            StatusCode::NOT_FOUND
        );
        let empty = send(
            &router,
            Method::GET,
            "/v1/messages",
            Body::empty(),
            Some(&bob_token),
        )
        .await;
        assert!(
            response_json(empty).await["messages"]
                .as_array()
                .unwrap()
                .is_empty()
        );

        db.run(|connection| {
            let count: i64 =
                connection.query_row("SELECT COUNT(*) FROM messages", [], |row| row.get(0))?;
            assert_eq!(count, 1);
            Ok(())
        })
        .await
        .unwrap();
        remove_test_database(database_path);
    }

    #[tokio::test]
    async fn bad_signature_consumes_challenge_and_expiration_cascades() {
        let (router, db, database_path) = test_app(1, 60).await;
        let signing_key = SigningKey::from_bytes(&[9; 32]);
        let client_id = derive_client_id(signing_key.verifying_key().as_bytes());
        assert_eq!(
            send_json(
                &router,
                Method::POST,
                "/v1/clients",
                registration(&signing_key, [10; 32]),
                None,
            )
            .await
            .status(),
            StatusCode::CREATED
        );
        let challenge = new_challenge(&router, &client_id).await;
        let challenge_id = challenge["challenge_id"].as_str().unwrap();
        let invalid = json!({
            "client_id": client_id,
            "challenge_id": challenge_id,
            "signature": encode(&[0; 64]),
        });
        assert_eq!(
            send_json(&router, Method::POST, "/v1/auth/sessions", invalid, None)
                .await
                .status(),
            StatusCode::UNAUTHORIZED
        );
        let valid = session_request(&signing_key, &client_id, &challenge);
        let reused = send_json(&router, Method::POST, "/v1/auth/sessions", valid, None).await;
        assert_eq!(reused.status(), StatusCode::UNAUTHORIZED);
        assert_eq!(
            response_json(reused).await["error"]["code"],
            "invalid_challenge"
        );

        let token = authenticate_client(&router, &signing_key, &client_id).await;
        let queued = json!({
            "recipient_id": client_id,
            "client_message_id": encode(&[11; 16]),
            "enc": encode(&[12; 32]),
            "ciphertext": encode(&[13; 32]),
        });
        assert_eq!(
            send_json(&router, Method::POST, "/v1/messages", queued, Some(&token),)
                .await
                .status(),
            StatusCode::CREATED
        );
        new_challenge(&router, &client_id).await;

        let expired_client_id = client_id.clone();
        db.run(move |connection| {
            connection.execute(
                "UPDATE clients SET last_active = ?1 WHERE client_id = ?2",
                params![Utc::now().timestamp() - 61, expired_client_id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
        let response = send_json(
            &router,
            Method::POST,
            "/v1/auth/challenges",
            json!({"client_id": client_id}),
            None,
        )
        .await;
        assert_eq!(response.status(), StatusCode::NOT_FOUND);
        db.run(|connection| {
            for table in ["clients", "sessions", "challenges", "messages"] {
                let count: i64 =
                    connection.query_row(&format!("SELECT COUNT(*) FROM {table}"), [], |row| {
                        row.get(0)
                    })?;
                assert_eq!(count, 0, "{table} survived account expiration");
            }
            Ok(())
        })
        .await
        .unwrap();
        remove_test_database(database_path);
    }

    #[tokio::test]
    async fn authenticated_use_refreshes_last_active() {
        let (router, db, database_path) = test_app(1, 60).await;
        let signing_key = SigningKey::from_bytes(&[14; 32]);
        let client_id = derive_client_id(signing_key.verifying_key().as_bytes());
        assert_eq!(
            send_json(
                &router,
                Method::POST,
                "/v1/clients",
                registration(&signing_key, [15; 32]),
                None,
            )
            .await
            .status(),
            StatusCode::CREATED
        );
        let token = authenticate_client(&router, &signing_key, &client_id).await;
        let queued = json!({
            "recipient_id": client_id,
            "client_message_id": encode(&[16; 16]),
            "enc": encode(&[17; 32]),
            "ciphertext": encode(&[18; 32]),
        });
        let created = send_json(&router, Method::POST, "/v1/messages", queued, Some(&token)).await;
        assert_eq!(created.status(), StatusCode::CREATED);
        let message_id = response_json(created).await["server_message_id"]
            .as_str()
            .unwrap()
            .to_string();

        let stale = Utc::now().timestamp() - 50;
        set_last_active(&db, &client_id, stale).await;
        assert_eq!(
            send(
                &router,
                Method::GET,
                "/v1/messages",
                Body::empty(),
                Some(&token),
            )
            .await
            .status(),
            StatusCode::OK
        );
        assert!(client_last_active(&db, &client_id).await > stale);

        set_last_active(&db, &client_id, stale).await;
        assert_eq!(
            send(
                &router,
                Method::GET,
                &format!("/v1/clients/{client_id}"),
                Body::empty(),
                Some(&token),
            )
            .await
            .status(),
            StatusCode::OK
        );
        assert!(client_last_active(&db, &client_id).await > stale);

        set_last_active(&db, &client_id, stale).await;
        assert_eq!(
            send_json(
                &router,
                Method::POST,
                "/v1/messages",
                json!({
                    "recipient_id": client_id,
                    "client_message_id": encode(&[19; 16]),
                    "enc": encode(&[20; 32]),
                    "ciphertext": encode(&[21; 32]),
                }),
                Some(&token),
            )
            .await
            .status(),
            StatusCode::CREATED
        );
        assert!(client_last_active(&db, &client_id).await > stale);

        set_last_active(&db, &client_id, stale).await;
        assert_eq!(
            send_json(
                &router,
                Method::POST,
                "/v1/messages/ack",
                json!({ "server_message_ids": [message_id] }),
                Some(&token),
            )
            .await
            .status(),
            StatusCode::OK
        );
        assert!(client_last_active(&db, &client_id).await > stale);

        set_last_active(&db, &client_id, Utc::now().timestamp() - 61).await;
        assert_eq!(
            send(
                &router,
                Method::GET,
                "/v1/messages",
                Body::empty(),
                Some(&token),
            )
            .await
            .status(),
            StatusCode::OK
        );
        let clients: i64 = db
            .run({
                let client_id = client_id.clone();
                move |connection| {
                    Ok(connection.query_row(
                        "SELECT COUNT(*) FROM clients WHERE client_id = ?1",
                        [&client_id],
                        |row| row.get(0),
                    )?)
                }
            })
            .await
            .unwrap();
        assert_eq!(clients, 1);
        remove_test_database(database_path);
    }

    #[tokio::test]
    async fn unauthenticated_challenge_does_not_refresh_last_active() {
        let (router, db, database_path) = test_app(1, 86_400).await;
        let signing_key = SigningKey::from_bytes(&[22; 32]);
        let client_id = derive_client_id(signing_key.verifying_key().as_bytes());
        assert_eq!(
            send_json(
                &router,
                Method::POST,
                "/v1/clients",
                registration(&signing_key, [23; 32]),
                None,
            )
            .await
            .status(),
            StatusCode::CREATED
        );
        let stale = Utc::now().timestamp() - 10;
        set_last_active(&db, &client_id, stale).await;
        new_challenge(&router, &client_id).await;
        assert_eq!(client_last_active(&db, &client_id).await, stale);
        remove_test_database(database_path);
    }

    #[tokio::test]
    async fn expire_ephemeral_clears_expired_ciphertext() {
        let (router, db, database_path) = test_app(1, 0).await;
        let signing_key = SigningKey::from_bytes(&[24; 32]);
        let client_id = derive_client_id(signing_key.verifying_key().as_bytes());
        assert_eq!(
            send_json(
                &router,
                Method::POST,
                "/v1/clients",
                registration(&signing_key, [25; 32]),
                None,
            )
            .await
            .status(),
            StatusCode::CREATED
        );
        let token = authenticate_client(&router, &signing_key, &client_id).await;
        assert_eq!(
            send_json(
                &router,
                Method::POST,
                "/v1/messages",
                json!({
                    "recipient_id": client_id,
                    "client_message_id": encode(&[26; 16]),
                    "enc": encode(&[27; 32]),
                    "ciphertext": encode(&[28; 32]),
                }),
                Some(&token),
            )
            .await
            .status(),
            StatusCode::CREATED
        );

        db.run(|connection| {
            connection.execute(
                "UPDATE messages SET expires_at = ?1",
                [Utc::now().timestamp() - 1],
            )?;
            expire_ephemeral(connection, Utc::now().timestamp())?;
            let payload: (Option<Vec<u8>>, Option<Vec<u8>>) =
                connection.query_row("SELECT enc, ciphertext FROM messages", [], |row| {
                    Ok((row.get(0)?, row.get(1)?))
                })?;
            let count: i64 =
                connection.query_row("SELECT COUNT(*) FROM messages", [], |row| row.get(0))?;
            assert_eq!(payload, (None, None));
            assert_eq!(count, 1);
            Ok(())
        })
        .await
        .unwrap();
        remove_test_database(database_path);
    }

    #[test]
    fn expiry_sweep_uses_message_ttl_capped_at_one_minute() {
        assert_eq!(expiry_sweep_period(1), Duration::from_secs(1));
        assert_eq!(expiry_sweep_period(60), Duration::from_secs(60));
        assert_eq!(
            expiry_sweep_period(default_message_ttl()),
            Duration::from_secs(60)
        );
    }

    async fn test_app(invite_uses: u64, account_ttl_seconds: u64) -> (Router, Db, PathBuf) {
        let database_path = std::env::temp_dir().join(format!(
            "rchat-server-test-{}.db",
            encode(&random_bytes::<16>())
        ));
        let db = Db::new(database_path.clone());
        db.init().await.unwrap();
        add_invite(db.clone(), "correct horse battery".into(), invite_uses)
            .await
            .unwrap();
        let state = AppState {
            db: db.clone(),
            config: Arc::new(Config {
                bind: default_bind(),
                database_path: database_path.clone(),
                account_ttl_seconds,
                message_ttl_seconds: default_message_ttl(),
                tls_certificate_path: None,
                tls_private_key_path: None,
                allow_insecure_http: true,
            }),
            limits: Arc::new(RateLimits::default()),
        };
        (app(state), db, database_path)
    }

    fn registration(signing_key: &SigningKey, encryption_key: [u8; 32]) -> Value {
        let auth_key = signing_key.verifying_key();
        let mut binding = KEY_PREFIX.to_vec();
        binding.extend_from_slice(&encryption_key);
        let binding_signature = signing_key.sign(&binding).to_bytes();
        let invite = "correct horse battery";
        let mut input = REGISTER_PREFIX.to_vec();
        input.extend_from_slice(&(invite.len() as u16).to_be_bytes());
        input.extend_from_slice(invite.as_bytes());
        input.extend_from_slice(auth_key.as_bytes());
        input.extend_from_slice(&encryption_key);
        input.extend_from_slice(&binding_signature);
        json!({
            "invite_code": invite,
            "auth_public_key": encode(auth_key.as_bytes()),
            "encryption_public_key": encode(&encryption_key),
            "encryption_key_signature": encode(&binding_signature),
            "registration_signature": encode(&signing_key.sign(&input).to_bytes()),
        })
    }

    async fn authenticate_client(
        router: &Router,
        signing_key: &SigningKey,
        client_id: &str,
    ) -> String {
        let challenge = new_challenge(router, client_id).await;
        let response = send_json(
            router,
            Method::POST,
            "/v1/auth/sessions",
            session_request(signing_key, client_id, &challenge),
            None,
        )
        .await;
        assert_eq!(response.status(), StatusCode::CREATED);
        response_json(response).await["access_token"]
            .as_str()
            .unwrap()
            .into()
    }

    async fn new_challenge(router: &Router, client_id: &str) -> Value {
        let response = send_json(
            router,
            Method::POST,
            "/v1/auth/challenges",
            json!({"client_id": client_id}),
            None,
        )
        .await;
        assert_eq!(response.status(), StatusCode::CREATED);
        response_json(response).await
    }

    fn session_request(signing_key: &SigningKey, client_id: &str, challenge: &Value) -> Value {
        let challenge_id =
            decode_fixed::<16>(challenge["challenge_id"].as_str().unwrap(), "test").unwrap();
        let challenge_bytes =
            decode_fixed::<32>(challenge["challenge"].as_str().unwrap(), "test").unwrap();
        let mut input = AUTH_PREFIX.to_vec();
        input.extend_from_slice(&challenge_id);
        input.extend_from_slice(&challenge_bytes);
        json!({
            "client_id": client_id,
            "challenge_id": challenge["challenge_id"],
            "signature": encode(&signing_key.sign(&input).to_bytes()),
        })
    }

    async fn set_last_active(db: &Db, client_id: &str, last_active: i64) {
        let client_id = client_id.to_string();
        db.run(move |connection| {
            connection.execute(
                "UPDATE clients SET last_active = ?1 WHERE client_id = ?2",
                params![last_active, client_id],
            )?;
            Ok(())
        })
        .await
        .unwrap();
    }

    async fn client_last_active(db: &Db, client_id: &str) -> i64 {
        let client_id = client_id.to_string();
        db.run(move |connection| {
            Ok(connection.query_row(
                "SELECT last_active FROM clients WHERE client_id = ?1",
                [&client_id],
                |row| row.get(0),
            )?)
        })
        .await
        .unwrap()
    }

    async fn send_json(
        router: &Router,
        method: Method,
        uri: &str,
        value: Value,
        token: Option<&str>,
    ) -> Response {
        send(router, method, uri, Body::from(value.to_string()), token).await
    }

    async fn send(
        router: &Router,
        method: Method,
        uri: &str,
        body: Body,
        token: Option<&str>,
    ) -> Response {
        let mut builder = Request::builder()
            .method(method)
            .uri(uri)
            .extension(ConnectInfo(
                "127.0.0.1:12345".parse::<SocketAddr>().unwrap(),
            ));
        builder = builder.header(header::CONTENT_TYPE, "application/json");
        if let Some(token) = token {
            builder = builder.header(header::AUTHORIZATION, format!("Bearer {token}"));
        }
        router
            .clone()
            .oneshot(builder.body(body).unwrap())
            .await
            .unwrap()
    }

    async fn response_json(response: Response) -> Value {
        let bytes = to_bytes(response.into_body(), usize::MAX).await.unwrap();
        serde_json::from_slice(&bytes).unwrap()
    }

    fn remove_test_database(path: PathBuf) {
        let _ = fs::remove_file(&path);
        let _ = fs::remove_file(path.with_extension("db-shm"));
        let _ = fs::remove_file(path.with_extension("db-wal"));
    }

    fn hex(value: &str) -> Vec<u8> {
        value
            .as_bytes()
            .chunks_exact(2)
            .map(|pair| u8::from_str_radix(std::str::from_utf8(pair).unwrap(), 16).unwrap())
            .collect()
    }
}
