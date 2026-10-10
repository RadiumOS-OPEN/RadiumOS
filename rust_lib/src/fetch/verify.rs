use p256::ecdsa::signature::hazmat::PrehashVerifier;
use rsa::{pkcs1::der::Decode, traits::PublicKeyParts, BigUint, Pkcs1v15Sign, Pss, RsaPublicKey};
use rustls::crypto::WebPkiSupportedAlgorithms;
use rustls::pki_types::{
    alg_id, AlgorithmIdentifier, InvalidSignature, SignatureVerificationAlgorithm,
};
use rustls::SignatureScheme;
use sha2::{Digest, Sha256, Sha384, Sha512};

#[derive(Debug)]
enum Algorithm {
    P256Sha256,
    P256Sha384,
    P384Sha256,
    P384Sha384,
    Ed25519,
    RsaPkcs1Sha256,
    RsaPkcs1Sha384,
    RsaPkcs1Sha512,
    RsaPssSha256,
    RsaPssSha384,
    RsaPssSha512,
}

impl SignatureVerificationAlgorithm for Algorithm {
    fn verify_signature(
        &self,
        public_key: &[u8],
        message: &[u8],
        signature: &[u8],
    ) -> Result<(), InvalidSignature> {
        match self {
            Self::P256Sha256 | Self::P256Sha384 => {
                let key = p256::ecdsa::VerifyingKey::from_sec1_bytes(public_key)
                    .map_err(|_| InvalidSignature)?;
                let signature =
                    p256::ecdsa::Signature::from_der(signature).map_err(|_| InvalidSignature)?;
                match self {
                    Self::P256Sha256 => key.verify_prehash(&Sha256::digest(message), &signature),
                    _ => key.verify_prehash(&Sha384::digest(message), &signature),
                }
                .map_err(|_| InvalidSignature)
            }
            Self::P384Sha256 | Self::P384Sha384 => {
                let key = p384::ecdsa::VerifyingKey::from_sec1_bytes(public_key)
                    .map_err(|_| InvalidSignature)?;
                let signature =
                    p384::ecdsa::Signature::from_der(signature).map_err(|_| InvalidSignature)?;
                match self {
                    Self::P384Sha256 => key.verify_prehash(&Sha256::digest(message), &signature),
                    _ => key.verify_prehash(&Sha384::digest(message), &signature),
                }
                .map_err(|_| InvalidSignature)
            }
            Self::Ed25519 => {
                let key = public_key.try_into().map_err(|_| InvalidSignature)?;
                let signature = signature.try_into().map_err(|_| InvalidSignature)?;
                if crate::prp::ed25519_verify(key, message, signature) {
                    Ok(())
                } else {
                    Err(InvalidSignature)
                }
            }
            _ => {
                // Bound the DER input before allocating its integers.
                if public_key.len() > 1100 {
                    return Err(InvalidSignature);
                }
                let encoded =
                    rsa::pkcs1::RsaPublicKey::from_der(public_key).map_err(|_| InvalidSignature)?;
                if encoded.modulus.as_bytes().len() > 1024
                    || encoded.public_exponent.as_bytes().len() > 5
                {
                    return Err(InvalidSignature);
                }
                let key = RsaPublicKey::new_with_max_size(
                    BigUint::from_bytes_be(encoded.modulus.as_bytes()),
                    BigUint::from_bytes_be(encoded.public_exponent.as_bytes()),
                    8192,
                )
                .map_err(|_| InvalidSignature)?;
                if !(2048..=8192).contains(&key.n().bits()) || key.e().bits() > 33 {
                    return Err(InvalidSignature);
                }
                match self {
                    Self::RsaPkcs1Sha256 => key.verify(
                        Pkcs1v15Sign::new::<Sha256>(),
                        &Sha256::digest(message),
                        signature,
                    ),
                    Self::RsaPkcs1Sha384 => key.verify(
                        Pkcs1v15Sign::new::<Sha384>(),
                        &Sha384::digest(message),
                        signature,
                    ),
                    Self::RsaPkcs1Sha512 => key.verify(
                        Pkcs1v15Sign::new::<Sha512>(),
                        &Sha512::digest(message),
                        signature,
                    ),
                    Self::RsaPssSha256 => {
                        key.verify(Pss::new::<Sha256>(), &Sha256::digest(message), signature)
                    }
                    Self::RsaPssSha384 => {
                        key.verify(Pss::new::<Sha384>(), &Sha384::digest(message), signature)
                    }
                    Self::RsaPssSha512 => {
                        key.verify(Pss::new::<Sha512>(), &Sha512::digest(message), signature)
                    }
                    _ => return Err(InvalidSignature),
                }
                .map_err(|_| InvalidSignature)
            }
        }
    }

    fn public_key_alg_id(&self) -> AlgorithmIdentifier {
        match self {
            Self::P256Sha256 | Self::P256Sha384 => alg_id::ECDSA_P256,
            Self::P384Sha256 | Self::P384Sha384 => alg_id::ECDSA_P384,
            Self::Ed25519 => alg_id::ED25519,
            _ => alg_id::RSA_ENCRYPTION,
        }
    }

    fn signature_alg_id(&self) -> AlgorithmIdentifier {
        match self {
            Self::P256Sha256 | Self::P384Sha256 => alg_id::ECDSA_SHA256,
            Self::P256Sha384 | Self::P384Sha384 => alg_id::ECDSA_SHA384,
            Self::Ed25519 => alg_id::ED25519,
            Self::RsaPkcs1Sha256 => alg_id::RSA_PKCS1_SHA256,
            Self::RsaPkcs1Sha384 => alg_id::RSA_PKCS1_SHA384,
            Self::RsaPkcs1Sha512 => alg_id::RSA_PKCS1_SHA512,
            Self::RsaPssSha256 => alg_id::RSA_PSS_SHA256,
            Self::RsaPssSha384 => alg_id::RSA_PSS_SHA384,
            Self::RsaPssSha512 => alg_id::RSA_PSS_SHA512,
        }
    }
}

pub(super) const ALGORITHMS: WebPkiSupportedAlgorithms = WebPkiSupportedAlgorithms {
    all: &[
        &Algorithm::P256Sha256,
        &Algorithm::P256Sha384,
        &Algorithm::P384Sha256,
        &Algorithm::P384Sha384,
        &Algorithm::Ed25519,
        &Algorithm::RsaPssSha256,
        &Algorithm::RsaPssSha384,
        &Algorithm::RsaPssSha512,
        &Algorithm::RsaPkcs1Sha256,
        &Algorithm::RsaPkcs1Sha384,
        &Algorithm::RsaPkcs1Sha512,
    ],
    mapping: &[
        (
            SignatureScheme::ECDSA_NISTP256_SHA256,
            &[&Algorithm::P256Sha256],
        ),
        (
            SignatureScheme::ECDSA_NISTP384_SHA384,
            &[&Algorithm::P384Sha384],
        ),
        (SignatureScheme::ED25519, &[&Algorithm::Ed25519]),
        (SignatureScheme::RSA_PSS_SHA256, &[&Algorithm::RsaPssSha256]),
        (SignatureScheme::RSA_PSS_SHA384, &[&Algorithm::RsaPssSha384]),
        (SignatureScheme::RSA_PSS_SHA512, &[&Algorithm::RsaPssSha512]),
        (
            SignatureScheme::RSA_PKCS1_SHA256,
            &[&Algorithm::RsaPkcs1Sha256],
        ),
        (
            SignatureScheme::RSA_PKCS1_SHA384,
            &[&Algorithm::RsaPkcs1Sha384],
        ),
        (
            SignatureScheme::RSA_PKCS1_SHA512,
            &[&Algorithm::RsaPkcs1Sha512],
        ),
    ],
};

#[cfg(test)]
mod tests {
    use super::*;
    use p256::ecdsa::signature::hazmat::PrehashSigner;

    fn hex(value: &str) -> alloc::vec::Vec<u8> {
        value
            .as_bytes()
            .chunks_exact(2)
            .map(|pair| u8::from_str_radix(core::str::from_utf8(pair).unwrap(), 16).unwrap())
            .collect()
    }

    #[test]
    fn ecdsa_signatures_reject_changed_messages_and_invalid_der() {
        let message = b"RadiumOS certificate verification";
        let key = p256::ecdsa::SigningKey::from_slice(&[1; 32]).unwrap();
        let public = key.verifying_key().to_encoded_point(false);
        for algorithm in [Algorithm::P256Sha256, Algorithm::P256Sha384] {
            let signature: p256::ecdsa::Signature = match algorithm {
                Algorithm::P256Sha256 => key.sign_prehash(&Sha256::digest(message)).unwrap(),
                _ => key.sign_prehash(&Sha384::digest(message)).unwrap(),
            };
            let der = signature.to_der();
            assert!(algorithm
                .verify_signature(public.as_bytes(), message, der.as_bytes())
                .is_ok());
            assert!(algorithm
                .verify_signature(public.as_bytes(), b"changed", der.as_bytes())
                .is_err());
            assert!(algorithm
                .verify_signature(public.as_bytes(), message, &der.as_bytes()[1..])
                .is_err());
        }
        let key = p384::ecdsa::SigningKey::from_slice(&[1; 48]).unwrap();
        let public = key.verifying_key().to_encoded_point(false);
        for algorithm in [Algorithm::P384Sha256, Algorithm::P384Sha384] {
            let signature: p384::ecdsa::Signature = match algorithm {
                Algorithm::P384Sha256 => key.sign_prehash(&Sha256::digest(message)).unwrap(),
                _ => key.sign_prehash(&Sha384::digest(message)).unwrap(),
            };
            let der = signature.to_der();
            assert!(algorithm
                .verify_signature(public.as_bytes(), message, der.as_bytes())
                .is_ok());
            assert!(algorithm
                .verify_signature(public.as_bytes(), b"changed", der.as_bytes())
                .is_err());
        }
        for algorithm in ALGORITHMS.all {
            assert!(algorithm.verify_signature(&[], message, &[]).is_err());
        }
    }

    #[test]
    fn rsa_openssl_signatures_reject_tampering() {
        // OpenSSL signatures use the digest length as the PSS salt length.
        let public = hex("3082010a0282010100a9e110a04e6abd8dc9c475bffe449a3cd0ed9f4162a37b546141ee433ff11964bbe4b826d27a71fd9b33deb2717c411897cc700d05320bf23c2bbd030a1618329255d341e083820225d2def8aeaac0b6b7a42bbb3b7afec1dd37ed4297f027e578da6eb567f66525602088fa95a24b2d014573fe4b395d0f659ebaf0c7b4d0e6f09617159775dad991002722188f94ace77b4590bf03beea2f0e9e8771d6a60244f1e904589e2901f042eead31ccaffdf382d97f3370fbffbf243453648db77ea49179b37177293d7840e38aed35afb0fff32bdff34ac6f4fbe1e85d6758bbe04bf07c65e4e17a1e1f4123e62bf630cb5dbcc2bbce3fa3c3567740ebeb9410ad0203010001");
        for (algorithm, signature) in [
            (Algorithm::RsaPkcs1Sha256, "47324a923332c9eda2494c0b14a851defd4450bcbb0ef5e68198924466a18aff8500d7707ea24f5e0caa2f57a2876192e59f559f221d7fb548951292676f0161225a43278251b36728479b4d370544e41e3e8111dd8909cc35abf1eb2de2e4f2d6ed74a8ad4b12555d720e1d80eceb3d276e5de214334a5ba48c4ea3b7a44418f3e831a27aeb5d08c774fa42fb934c16ac867e73c99f89a0cf05481ed512950aff857c1d5f1cbc188ed617b0f0ac327352df6c0d698c212a4cd6fa250a495b71453047004b21ab51a1b721cff240cdcc2230582e0c687121fa000ea00c4085a1d53bee9c42b4ffb970e8ffe402a62f95597b83854869aa4868234e4d73959457"),
            (Algorithm::RsaPkcs1Sha384, "8d3ffa8c48760fd43d4b9957fb42bb16cae03c64333a91194d298b029cbff117b77fb2f1506286ac0db01745dddd02bbc405fa53f8d7797498d7f8c9726d3281abdc2fa324feff25a79b917ca75715c9e7fbeaabc138b9a63eb164ee4a96e4423bd4d967701bc82a9063a6d048d30d00ca36a22cab0963e1c4d9f234e3434bb808ffe85ddc1280572f852208cfb3162e105a5bc99f3a8fe11ffbf360a011a0baaba0cd60e7e018ecdb5b968eb994a6bcadd5a374af89dde2409cd2e2f58afe9da12a0f2ac16f749d9644e0c5012609af97d564e25ddc40d722a337744ec83c0a79e422a2fab38df9f9bd58ee9db6512cadec2d6049dc9d4e50a3d08c6e165dbc"),
            (Algorithm::RsaPkcs1Sha512, "77c28591f964aeaf68e171beb05c92dd59149eca360c800a4fb1cb213b5512667648e630015c6cbd3228e633662f401ae5017eb9f82a2fbf55e30b2015c3ad2914b891bc5bcb3a146f8816a63cfa9122dda5eee324281125dec17fe28b6a1d967b8183757690f069c68678168c219b08c6bcf49b4b935762c4edc234793c98feb280857bcf9966c541107784408a528260542856c899196222b14c17296dc268faec6a7f186df0c7929e6897128e3a1a75f0879f3702cd8d9f78c6877897843f7492d22c454492efa896245ff9f76941b656c7c67d9f7dac414398b71290ce6be75e387e8b7c3b5a42f5fbc9fe9c258169ecd2706fd3f7b2be89d132534eca96"),
            (Algorithm::RsaPssSha256, "782c9187603b7d8941c4d627d0de5235eaed68b3084d223f33e085c6c601d832919f97ffe5f2edea602f75d635dba94610eeb16761e0195c201ea3a41edb667941c8685296768be333c5b8271e8430a03e9e633c42b0d98e9cb644a0f27a4b0c4a7450751bcc00b5deb2e0b36f09ba33efcf58b11d6bc341f02a6bbcea6f6a28db9939b37ccae8648ae6e3a8c3d61b1902fb94a526b92cc8cad91deeafe64a9fd04c46b8080279a05436134e79732adfce50f0496a4cb01baa7d1e4acd0d1eea643a9713a2b7ea0bdd9d4c6f66c86641c8ea282835a901e009f298b001e5fa6dc0c165a4d1d0e9bb6403855a6b660c7ee280b68d8a1ce9e9df79eb6290805074"),
            (Algorithm::RsaPssSha384, "2d5a177dec49940cb9a63fb17ff011e353e382c0a9d19c872b661664eaa7fcaeb517c8023d568be40b89c5baf6f2474fccf9a525d8234c1e50283a0aa7fd81b7db4356d0417dbb9827a41fcae7c81fda9cf8c6e97a602e85f34c96abf905b2d17bac20ae04b432dc8306806eca09633eea0498adc230563f4cfd136fb624580a605162cec00f1c02192f9f250b92c340e341e8186680e7d0c430711cf60afa0562c84faf4abc9e3799af8771514b69e079277c56ccd6025ffb37d79a1b42ceec5f912b00b2aa82255a1b4875bdae721326a0b78c10e8e5206b7a189f7e9809a121ad9d105bc12fc7a418deb324b99169f2fb978c1d7487e94c6a221f067c9e77"),
            (Algorithm::RsaPssSha512, "3fc6902cd0b91635128463bd95680720b02b45913d3bca56e5e502140ed0e0c04ee78a72a88fde0fbe41edde310edfaf58b4b2cdbb934ed69ac9ec139fff4d51292900c938d026d5538a9b52b1bb34e683118faa9d5a385f87edab8d2bf91b55173fd60320f4652995adf531bade281edda45ce5e2c7581630bb2b53f997e7e0629a5e24c29c28fc8eb84bef9a5fd7f91b3d52c995c9b37728aa2403d8b83d5bfd9574f9b5982ba8de3f7cabf1df49709e0559af1eb8f461bce2a5a348dbe8819d672939c1d943951c596fb34bb85614a2f62217186d1678be9d6f76ffadded6d24c903c02479e42095525014f3a0ff29b5e2cc90b48f0b17d999fc6154d0f3a"),
        ] {
            let signature = hex(signature);
            assert!(algorithm.verify_signature(&public, b"RadiumOS certificate verification", &signature).is_ok());
            assert!(algorithm.verify_signature(&public, b"changed", &signature).is_err());
            assert!(algorithm.verify_signature(&public, b"RadiumOS certificate verification", &signature[1..]).is_err());
            let mut trailing = public.clone();
            trailing.push(0);
            assert!(algorithm.verify_signature(&trailing, b"RadiumOS certificate verification", &signature).is_err());
        }
    }

    #[test]
    fn openssl_ecdsa_and_ed25519_signatures() {
        for (algorithm, public, signature) in [
            (Algorithm::P256Sha256, "04a09a003fda3aa5b6135eb0c5986ab399a3d7909d8a7af33559ae142acf32c6a6497309bef986b2f9742860e9873cb594048ff9573e44da189d859b038d21d4c4", "3046022100bda06d38b731fc1ce674bf86bc4ffbb18cd9a493eac9ff1a9cf0c93f7dbf64b6022100f06d8a23176ea6da10b31d8a3cc5042e18aee4df3bd9dbaabf7c6a90e09ded49"),
            (Algorithm::P256Sha384, "04a09a003fda3aa5b6135eb0c5986ab399a3d7909d8a7af33559ae142acf32c6a6497309bef986b2f9742860e9873cb594048ff9573e44da189d859b038d21d4c4", "3045022100fb4cbf6c36c40fdac424f60e9bdcb6f85a774f1815b7afc4a52fe145fab1991b02202e1439562c59d42fd26821f23c86e483f60604e88a62cb316a4bd15dbe309e5a"),
            (Algorithm::P384Sha256, "046c9b837e9ae25b2200fab30fcbdf77eaffd68b8dfa58a694988ecd4261fbaee9296f40c6c89ed8ace5875e51cee190908a6aeee2e39a8121ee8ed9bc6368d039596a94eb2ff1e0c6e5e6efeeae690e3cb26c5359b8b9af17de57e3a202678a83", "30650231009ca63a7c256e1c0c1cd68d3017c096cde44a0f2477e5f2656496b2ca3f24f0ce00d4bf917e09c3c2a71b7cddefdddda402307b1acbe6249721a625a804bb2bc2613c66da0ece8de2195fb3f7b446ae8033dc4f6a2b7165002b627c0a0546a1e36029"),
            (Algorithm::P384Sha384, "046c9b837e9ae25b2200fab30fcbdf77eaffd68b8dfa58a694988ecd4261fbaee9296f40c6c89ed8ace5875e51cee190908a6aeee2e39a8121ee8ed9bc6368d039596a94eb2ff1e0c6e5e6efeeae690e3cb26c5359b8b9af17de57e3a202678a83", "3066023100a6b73f321226e9643111358e0c5d0ea54552f780765bd1391b89bf108a4f5528f23bae7533a1c8125c76b7e376ef6be0023100cfa45250f4666410f8011ad6ebe4fb1c31ca12a8fadac110befdcd1502ddeddf5c420a976b8ea299dfd75de3a4d378b2"),
            (Algorithm::Ed25519, "13332a7d8047910b7bdfbb6f19e685026bbaa44237c6369948dae283eaa93d8d", "18194a7d62865381ce6851b67b8ebefa35415c81f89534dd5476202ac44ccf3f3937f657680aad05230ce0bdf58f10d344171b2c817d61a4c54762434433cc0e"),
        ] {
            let public = hex(public);
            let signature = hex(signature);
            assert!(algorithm.verify_signature(&public, b"RadiumOS certificate verification", &signature).is_ok());
            assert!(algorithm.verify_signature(&public, b"changed", &signature).is_err());
            assert!(algorithm.verify_signature(&public[1..], b"RadiumOS certificate verification", &signature).is_err());
        }
    }
}
