// Minimal USB HID host sketch to check whether the Maxxter 9-button
// mouse dongle (248a:8579) sends full-length Report Protocol reports
// (7 bytes, incl. wheel) to an RP2040 acting as USB host, or the same
// 3-byte Boot-shaped truncation observed on ESP32-S3 - see
// mds/2026-08-22_rp2040_host_check.md.
//
// Requires Tools -> USB Stack -> "Adafruit TinyUSB Host (native)".
// RP2040 has a single native USB peripheral, so once it's acting as
// Host, the same port can't also be a USB-CDC Serial port - all output
// goes over Serial1 (hardware UART) instead. Wire GP0(TX)/GP1(RX) (or
// whatever your board's UART0 pins are) to a USB-serial adapter.
#include "Adafruit_TinyUSB.h"

#ifndef USE_TINYUSB_HOST
#error This sketch requires "Tools -> USB Stack -> Adafruit TinyUSB Host (native)"
#endif

Adafruit_USBH_Host USBHost;

void setup() {
  Serial1.begin(115200);
  delay(500);
  Serial1.println("RP2040 HID host check: starting");

  // Match ESP32's approach (explicit SET_PROTOCOL(Report)) rather than
  // TinyUSB's own default (HID_PROTOCOL_BOOT) - we specifically want to
  // see what this mouse sends once told to use Report Protocol.
  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);

  USBHost.begin(0);
}

void loop() {
  USBHost.task();
  Serial1.flush();
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report_desc, uint16_t desc_len) {
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);
  Serial1.printf("HID mount: dev_addr=%u idx=%u itf_protocol=%u (0=None 1=Keyboard 2=Mouse)\r\n",
                 dev_addr, idx, itf_protocol);

  Serial1.printf("Report descriptor (%u bytes):\r\n", desc_len);
  for (uint16_t i = 0; i < desc_len; i++) {
    Serial1.printf("%02x ", report_desc[i]);
    if ((i % 16) == 15) Serial1.println();
  }
  Serial1.println();

  if (!tuh_hid_receive_report(dev_addr, idx)) {
    Serial1.println("Error: cannot request initial report");
  }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx) {
  Serial1.printf("HID unmount: dev_addr=%u idx=%u\r\n", dev_addr, idx);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report, uint16_t len) {
  Serial1.printf("[%u:%u] raw report (%u bytes): ", dev_addr, idx, len);
  for (uint16_t i = 0; i < len; i++) {
    Serial1.printf("%02x ", report[i]);
  }
  Serial1.println();

  // Keep polling - TinyUSB does not auto-resubmit.
  tuh_hid_receive_report(dev_addr, idx);
}
