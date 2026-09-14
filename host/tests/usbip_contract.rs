use airdap_service::usbip::{Descriptors, Urb};

#[test]
fn enumeration_matches_reference_descriptors_byte_for_byte() {
    let expected: serde_json::Value =
        serde_json::from_str(include_str!("fixtures/usb-descriptors.json")).unwrap();
    let descriptors = Descriptors::new("ADP-001122334455").unwrap();
    for (name, bytes) in [
        ("device", descriptors.get(0x100, 0).unwrap()),
        ("configuration", descriptors.get(0x200, 0).unwrap()),
        ("bos", descriptors.get(0xf00, 0).unwrap()),
        ("serial", descriptors.get(0x303, 0x409).unwrap()),
        ("ms_os", descriptors.ms_os.clone()),
        ("record", descriptors.record()),
    ] {
        assert_eq!(
            hex::encode(bytes),
            expected[name].as_str().unwrap(),
            "{name}"
        );
    }
    assert!(descriptors.get(0x3ff, 0).is_err());
    assert!(descriptors.get(0x301, 1).is_err());
}

#[test]
fn submit_validates_device_direction_size_and_isochronous_fields() {
    let mut header = [0u8; 48];
    header[3] = 1;
    header[7] = 10;
    header[8..12].copy_from_slice(&0x10001u32.to_be_bytes());
    header[15] = 1;
    header[19] = 1;
    header[24..28].copy_from_slice(&64u32.to_be_bytes());
    assert_eq!(Urb::parse(&header).unwrap().size, 64);
    for (offset, value) in [(8, 99), (15, 2), (24, 1), (35, 2)] {
        let mut invalid = header;
        invalid[offset] = value;
        assert!(Urb::parse(&invalid).is_err());
    }
}
