// USB Host bridge: RP2040 does the actual USB Host role (TinyUSB Host,
// same proven-working core as rp2040_host_check.ino) and forwards
// already-decoded HID mount/report events to an ESP32-S3 over a plain
// UART link, instead of an SPI-attached MAX3421E - see
// mds/2026-08-23_rp2040_as_host_bridge_plan.md for why (MAX3421E's SPI
// protocol turned out sensitive to wiring quality; a UART byte stream is
// expected to tolerate that much better).
//
// Requires Tools -> USB Stack -> "Adafruit TinyUSB Host (native)".
// RP2040 has a single native USB peripheral, so once it's acting as
// Host, the same port can't also be a USB-CDC Serial port - this sketch
// uses Serial1 (hardware UART, GP0=TX/GP1=RX) as the link to the ESP32
// instead of for human-readable debug output (unlike
// rp2040_host_check.ino, which is still the one to use for that).
#include "Adafruit_TinyUSB.h"

#ifndef USE_TINYUSB_HOST
#error This sketch requires "Tools -> USB Stack -> Adafruit TinyUSB Host (native)"
#endif

Adafruit_USBH_Host USBHost;

// ── Wire protocol (must match usb_host_rp2040_bridge.c's #defines) ─────
// [0xAA sync][msg_type][dev_addr][idx][itf_protocol][len_lo][len_hi][payload...][checksum]
// checksum = XOR of every byte from msg_type through the last payload byte.
#define BRIDGE_SYNC_BYTE      0xAA
#define BRIDGE_MSG_HEARTBEAT  0x01
#define BRIDGE_MSG_MOUNT      0x02
#define BRIDGE_MSG_UNMOUNT    0x03
#define BRIDGE_MSG_REPORT     0x04
#define BRIDGE_BAUD           460800

#define HEARTBEAT_INTERVAL_MS 500

static uint32_t last_heartbeat_ms;

static void send_frame(uint8_t msg_type, uint8_t dev_addr, uint8_t idx, uint8_t itf_protocol,
                        const uint8_t *payload, uint16_t len) {
  uint8_t checksum = 0;

  Serial1.write((uint8_t)BRIDGE_SYNC_BYTE);

  Serial1.write(msg_type);
  checksum ^= msg_type;
  Serial1.write(dev_addr);
  checksum ^= dev_addr;
  Serial1.write(idx);
  checksum ^= idx;
  Serial1.write(itf_protocol);
  checksum ^= itf_protocol;

  uint8_t len_lo = (uint8_t)(len & 0xFF);
  uint8_t len_hi = (uint8_t)(len >> 8);
  Serial1.write(len_lo);
  checksum ^= len_lo;
  Serial1.write(len_hi);
  checksum ^= len_hi;

  for (uint16_t i = 0; i < len; i++) {
    Serial1.write(payload[i]);
    checksum ^= payload[i];
  }

  Serial1.write(checksum);
}

void setup() {
  Serial1.begin(BRIDGE_BAUD);
  delay(500);

  // Match ESP32's approach (explicit SET_PROTOCOL(Report)) - see
  // rp2040_host_check.ino / mds/2026-08-22_rp2040_host_check.md.
  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);

  USBHost.begin(0);
  last_heartbeat_ms = millis();
}

void loop() {
  USBHost.task();

  // Sent regardless of USB device state so the ESP32 side can probe for
  // this bridge's presence (usb_host_rp2040_bridge_probe()) without
  // needing anything plugged in yet.
  uint32_t now = millis();
  if (now - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS) {
    last_heartbeat_ms = now;
    send_frame(BRIDGE_MSG_HEARTBEAT, 0, 0, 0, NULL, 0);
  }
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report_desc, uint16_t desc_len) {
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);
  // usb_host_rp2040_bridge.c's MAX_PAYLOAD_LEN - a longer descriptor
  // would desync the frame on the ESP32 side, so truncate defensively
  // rather than send something it will reject anyway.
  if (desc_len > 512) {
    desc_len = 512;
  }
  send_frame(BRIDGE_MSG_MOUNT, dev_addr, idx, itf_protocol, report_desc, desc_len);

  if (!tuh_hid_receive_report(dev_addr, idx)) {
    // No Serial1 debug output available here anymore (repurposed for the
    // bridge protocol) - nothing more to do about it than the same retry
    // policy tuh_hid_report_received_cb already has.
  }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx) {
  send_frame(BRIDGE_MSG_UNMOUNT, dev_addr, idx, 0, NULL, 0);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report, uint16_t len) {
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);
  if (len > 512) {
    len = 512;
  }
  send_frame(BRIDGE_MSG_REPORT, dev_addr, idx, itf_protocol, report, len);

  // Keep polling - TinyUSB does not auto-resubmit.
  tuh_hid_receive_report(dev_addr, idx);
}
