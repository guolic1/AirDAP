use airdap_service::{
    credential::Credential,
    network::{decode_response, encode_frame, parse_hello},
};

#[test]
fn credential_roundtrip_and_integrity() {
    let c = Credential::new("ADP-001122334455", [0x11; 32]).unwrap();
    let json = c.to_json().unwrap();
    assert_eq!(
        Credential::from_json(&json).unwrap().identity(),
        "AIRDAP:ADP-001122334455"
    );
    assert!(Credential::from_json(&json.replace("ADP-001122334455", "ADP-001122334456")).is_err());
    assert!(Credential::new("../secrets", [0; 32]).is_err());
    assert!(!format!("{c:?}").contains(&"11".repeat(32)));
}

#[test]
fn frames_reject_mismatched_and_reserved_fields() {
    let encoded = encode_frame(7, 0x12345678, 9, b"ok").unwrap();
    assert_eq!(
        hex::encode(&encoded),
        "41444150010700001234567800000009000200006f6b"
    );
    assert_eq!(decode_response(&encoded, 7, 0x12345678, 9).unwrap(), b"ok");
    assert!(decode_response(&encoded, 7, 0x12345678, 10).is_err());
    for index in [0, 4, 5, 6, 18] {
        let mut bad = encoded.clone();
        bad[index] ^= 1;
        assert!(decode_response(&bad, 7, 0x12345678, 9).is_err());
    }
    assert!(decode_response(&encoded[..21], 7, 0x12345678, 9).is_err());
    assert!(encode_frame(1, 0, 1, b"").is_err());
    assert!(encode_frame(1, 1, 0, b"").is_err());
    assert!(encode_frame(1, 1, 1, &[0; 4097]).is_err());
}

#[test]
fn hello_binds_the_physical_device_identity() {
    let mut hello = vec![0u8; 20];
    hello.extend_from_slice(b"ADP-001122334455vtest");
    assert_eq!(
        parse_hello(&hello, "ADP-001122334455").unwrap().firmware,
        "vtest"
    );
    assert!(parse_hello(&hello, "ADP-001122334456").is_err());
    assert!(parse_hello(&hello[..36], "ADP-001122334455").is_err());
}
