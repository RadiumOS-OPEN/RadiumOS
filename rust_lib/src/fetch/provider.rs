use alloc::{boxed::Box, sync::Arc, vec, vec::Vec};
use rustls::crypto::{
    cipher::*, hash, hmac, tls13::HkdfUsingHmac, ActiveKeyExchange, CryptoProvider,
    GetRandomFailed, KeyProvider, SecureRandom, SharedSecret, SupportedKxGroup,
};
use rustls::{
    CipherSuite, CipherSuiteCommon, ConnectionTrafficSecrets, ContentType, Error, NamedGroup,
    PeerMisbehaved, ProtocolVersion, SupportedCipherSuite, Tls13CipherSuite,
};
use zeroize::Zeroize;

use crate::prp;

pub(super) fn provider() -> CryptoProvider {
    CryptoProvider {
        cipher_suites: vec![SupportedCipherSuite::Tls13(&Tls13CipherSuite {
            common: CipherSuiteCommon {
                suite: CipherSuite::TLS13_CHACHA20_POLY1305_SHA256,
                hash_provider: &Sha256,
                confidentiality_limit: u64::MAX,
            },
            hkdf_provider: &HkdfUsingHmac(&HmacSha256),
            aead_alg: &Chacha20Poly1305,
            quic: None,
        })],
        kx_groups: vec![&X25519],
        signature_verification_algorithms: super::verify::ALGORITHMS,
        secure_random: &Random,
        key_provider: &NoClientKey,
    }
}

struct Sha256;

impl hash::Hash for Sha256 {
    fn start(&self) -> Box<dyn hash::Context> {
        Box::new(HashContext(prp::Sha256::new()))
    }

    fn hash(&self, data: &[u8]) -> hash::Output {
        hash::Output::new(&prp::sha256(data))
    }

    fn output_len(&self) -> usize {
        32
    }

    fn algorithm(&self) -> hash::HashAlgorithm {
        hash::HashAlgorithm::SHA256
    }
}

struct HashContext(prp::Sha256);

impl hash::Context for HashContext {
    fn fork_finish(&self) -> hash::Output {
        hash::Output::new(&self.0.clone().finish())
    }

    fn fork(&self) -> Box<dyn hash::Context> {
        Box::new(Self(self.0.clone()))
    }

    fn finish(self: Box<Self>) -> hash::Output {
        hash::Output::new(&self.0.finish())
    }

    fn update(&mut self, data: &[u8]) {
        self.0.update(data);
    }
}

struct HmacSha256;
struct HmacKey(Vec<u8>);

impl Drop for HmacKey {
    fn drop(&mut self) {
        self.0.zeroize();
    }
}

impl hmac::Hmac for HmacSha256 {
    fn with_key(&self, key: &[u8]) -> Box<dyn hmac::Key> {
        Box::new(HmacKey(key.to_vec()))
    }

    fn hash_output_len(&self) -> usize {
        32
    }
}

impl hmac::Key for HmacKey {
    fn sign_concat(&self, first: &[u8], middle: &[&[u8]], last: &[u8]) -> hmac::Tag {
        let mut parts = Vec::with_capacity(middle.len() + 2);
        parts.push(first);
        parts.extend_from_slice(middle);
        parts.push(last);
        let mut digest = prp::hmac_parts(&self.0, &parts);
        let tag = hmac::Tag::new(&digest);
        digest.zeroize();
        tag
    }

    fn tag_len(&self) -> usize {
        32
    }
}

struct Chacha20Poly1305;
struct RecordCipher {
    key: AeadKey,
    iv: Iv,
}

impl Tls13AeadAlgorithm for Chacha20Poly1305 {
    fn encrypter(&self, key: AeadKey, iv: Iv) -> Box<dyn MessageEncrypter> {
        Box::new(RecordCipher { key, iv })
    }

    fn decrypter(&self, key: AeadKey, iv: Iv) -> Box<dyn MessageDecrypter> {
        Box::new(RecordCipher { key, iv })
    }

    fn key_len(&self) -> usize {
        32
    }

    fn extract_keys(
        &self,
        key: AeadKey,
        iv: Iv,
    ) -> Result<ConnectionTrafficSecrets, UnsupportedOperationError> {
        Ok(ConnectionTrafficSecrets::Chacha20Poly1305 { key, iv })
    }
}

impl MessageEncrypter for RecordCipher {
    fn encrypt(
        &mut self,
        msg: OutboundPlainMessage<'_>,
        seq: u64,
    ) -> Result<OutboundOpaqueMessage, Error> {
        let total_len = self.encrypted_payload_len(msg.payload.len());
        let mut payload = PrefixedPayload::with_capacity(total_len);
        payload.extend_from_chunks(&msg.payload);
        payload.extend_from_slice(&[u8::from(msg.typ)]);
        let key = self
            .key
            .as_ref()
            .try_into()
            .map_err(|_| Error::EncryptError)?;
        let tag = prp::aead_encrypt(
            key,
            &Nonce::new(&self.iv, seq).0,
            &make_tls13_aad(total_len),
            payload.as_mut(),
        )
        .ok_or(Error::EncryptError)?;
        payload.extend_from_slice(&tag);
        Ok(OutboundOpaqueMessage::new(
            ContentType::ApplicationData,
            ProtocolVersion::TLSv1_2,
            payload,
        ))
    }

    fn encrypted_payload_len(&self, payload_len: usize) -> usize {
        payload_len + 17
    }
}

impl MessageDecrypter for RecordCipher {
    fn decrypt<'a>(
        &mut self,
        mut msg: InboundOpaqueMessage<'a>,
        seq: u64,
    ) -> Result<InboundPlainMessage<'a>, Error> {
        if msg.payload.len() < 17 {
            return Err(Error::DecryptError);
        }
        let aad = make_tls13_aad(msg.payload.len());
        let plain_len = msg.payload.len() - 16;
        let mut tag = [0u8; 16];
        tag.copy_from_slice(&msg.payload[plain_len..]);
        let key = self
            .key
            .as_ref()
            .try_into()
            .map_err(|_| Error::DecryptError)?;
        if !prp::aead_decrypt(
            key,
            &Nonce::new(&self.iv, seq).0,
            &aad,
            &mut msg.payload[..plain_len],
            &tag,
        ) {
            return Err(Error::DecryptError);
        }
        msg.payload.truncate(plain_len);
        msg.into_tls13_unpadded_message()
    }
}

#[derive(Debug)]
struct Random;

impl SecureRandom for Random {
    fn fill(&self, buf: &mut [u8]) -> Result<(), GetRandomFailed> {
        if prp::random_bytes(buf) {
            Ok(())
        } else {
            Err(GetRandomFailed)
        }
    }
}

#[derive(Debug)]
struct NoClientKey;

impl KeyProvider for NoClientKey {
    fn load_private_key(
        &self,
        _: rustls::pki_types::PrivateKeyDer<'static>,
    ) -> Result<Arc<dyn rustls::sign::SigningKey>, Error> {
        Err(Error::General(
            "fetch does not support client certificates".into(),
        ))
    }
}

#[derive(Debug)]
struct X25519;
struct KeyExchange {
    private: [u8; 32],
    public: [u8; 32],
}

impl Drop for KeyExchange {
    fn drop(&mut self) {
        self.private.zeroize();
    }
}

impl SupportedKxGroup for X25519 {
    fn start(&self) -> Result<Box<dyn ActiveKeyExchange>, Error> {
        let mut exchange = Box::new(KeyExchange {
            private: [0; 32],
            public: [0; 32],
        });
        Random.fill(&mut exchange.private)?;
        exchange.public = prp::x25519(&exchange.private, &prp::X25519_BASEPOINT);
        Ok(exchange)
    }

    fn name(&self) -> NamedGroup {
        NamedGroup::X25519
    }

    fn ffdhe_group(&self) -> Option<rustls::ffdhe_groups::FfdheGroup<'static>> {
        None
    }
}

impl ActiveKeyExchange for KeyExchange {
    fn complete(self: Box<Self>, peer_pub_key: &[u8]) -> Result<SharedSecret, Error> {
        let peer = peer_pub_key
            .try_into()
            .map_err(|_| Error::PeerMisbehaved(PeerMisbehaved::InvalidKeyShare))?;
        let mut secret = prp::x25519(&self.private, peer);
        if secret.iter().fold(0u8, |value, byte| value | byte) == 0 {
            secret.zeroize();
            return Err(Error::PeerMisbehaved(PeerMisbehaved::InvalidKeyShare));
        }
        let shared = SharedSecret::from(secret.as_slice());
        secret.zeroize();
        Ok(shared)
    }

    fn pub_key(&self) -> &[u8] {
        &self.public
    }

    fn group(&self) -> NamedGroup {
        NamedGroup::X25519
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn provider_preserves_hash_and_record_contracts() {
        use rustls::crypto::{hash::Hash, hmac::Hmac};
        let mut hash = Sha256.start();
        hash.update(b"a");
        assert_eq!(hash.fork_finish().as_ref(), prp::sha256(b"a"));
        let mut fork = hash.fork();
        hash.update(b"b");
        fork.update(b"c");
        assert_eq!(hash.finish().as_ref(), prp::sha256(b"ab"));
        assert_eq!(fork.finish().as_ref(), prp::sha256(b"ac"));
        assert_eq!(
            HmacSha256
                .with_key(b"key")
                .sign_concat(b"a", &[b"b"], b"c")
                .as_ref(),
            prp::hmac_sha256(b"key", b"abc")
        );

        let mut enc = Chacha20Poly1305.encrypter([7; 32].into(), [8; 12].into());
        let mut dec = Chacha20Poly1305.decrypter([7; 32].into(), [8; 12].into());
        let encrypted = enc
            .encrypt(
                OutboundPlainMessage {
                    typ: ContentType::Handshake,
                    version: ProtocolVersion::TLSv1_3,
                    payload: OutboundChunks::new(&[b"hello", b" world"]),
                },
                42,
            )
            .unwrap();
        let mut bytes = encrypted.payload.as_ref().to_vec();
        let mut corrupted = bytes.clone();
        corrupted[0] ^= 1;
        assert!(dec
            .decrypt(
                InboundOpaqueMessage::new(
                    ContentType::ApplicationData,
                    ProtocolVersion::TLSv1_2,
                    &mut corrupted
                ),
                42
            )
            .is_err());
        assert!(dec
            .decrypt(
                InboundOpaqueMessage::new(
                    ContentType::ApplicationData,
                    ProtocolVersion::TLSv1_2,
                    &mut bytes.clone()
                ),
                43
            )
            .is_err());
        let plain = dec
            .decrypt(
                InboundOpaqueMessage::new(
                    ContentType::ApplicationData,
                    ProtocolVersion::TLSv1_2,
                    &mut bytes,
                ),
                42,
            )
            .unwrap();
        assert_eq!(plain.typ, ContentType::Handshake);
        assert_eq!(plain.payload, b"hello world");
        for peer in [&[0u8; 32][..], &[1u8; 31][..]] {
            let exchange = Box::new(KeyExchange {
                private: [7; 32],
                public: [0; 32],
            });
            assert!(exchange.complete(peer).is_err());
        }
    }
}
