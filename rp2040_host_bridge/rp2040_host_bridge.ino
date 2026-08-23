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
// uses Serial1 (hardware UART0, GP0=TX/GP1=RX) as the link to the ESP32
// instead of for human-readable debug output. RP2040 has a second,
// independent hardware UART (UART1/Serial2) free for that - set
// BRIDGE_DEBUG to 1 below to print human-readable mount/report dumps
// there (same format as rp2040_host_check.ino) without disturbing the
// live bridge link on Serial1.
#include <string.h>

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

// Set to 1 to also print human-readable debug lines (mount/unmount/raw
// report dumps, same format as rp2040_host_check.ino) on Serial2 -
// RP2040's *second* hardware UART (UART1), independent of Serial1/UART0
// used above for the binary bridge protocol to the ESP32. The two UARTs
// are separate peripherals with separate pins, so this doesn't
// interfere with the live bridge link at all.
//
// Default arduino-pico pins for Serial2 are GP4=TX/GP5=RX - if that
// doesn't match your wiring, remap before Serial2.begin() with
// Serial2.setTX(pin)/Serial2.setRX(pin).
//
// Note the RP2040 board's native USB connector (wherever the dongle is
// plugged in) is wired directly to the chip's own USB D+/D- pins - see
// mds/2026-08-22_rp2040_host_check.md - there's no UART bridge behind
// it, so it can't be used for debug output at all once acting as Host
// (same reason Serial1 had to be repurposed for the bridge protocol
// instead of debug text in the first place).
#define BRIDGE_DEBUG 0

// Toggle for a running reports/sec counter, printed on Serial2 once a
// second (mds/2026-08-24_rp2040_bridge_fps_investigation.md measurement
// plan, point 1: how fast is the dongle/tuh_hid_report_received_cb()
// actually firing, independent of the UART link to the ESP32 or
// anything downstream of it). This does NOT run on a second core/thread
// - a plain counter incremented inside the existing callback and
// checked once per loop() iteration is enough, and staying single-core
// avoids having to make USBHost's TinyUSB Host stack itself
// thread-safe across cores (arduino-pico's core1 via setup1()/loop1()
// is real, but TinyUSB's tuh_* calls all assume they're only ever
// touched from the one task/core running USBHost.task() - splitting
// that across cores would need its own locking and isn't worth it just
// to print a counter). Separate from BRIDGE_DEBUG's raw hex dumps
// above: those turned out to be heavy enough to perturb timing while
// chasing the type-c crash (mds/2026-08-23_rp2040_host_status.md) - an
// integer increment plus one printf/sec should not have that problem,
// but keep an eye out.
#define RATE_MONITOR 1

// Toggle for a one-shot, interactive "how fast can this mouse/dongle
// actually poll" measurement, run once in setup() before the bridge
// starts normal operation. Prompts over Serial2 for a few seconds of
// continuous mouse movement, then reports the peak instantaneous and
// average reports/sec seen. Off by default - it blocks setup() waiting
// on real mouse input, so only turn on for a deliberate bench test.
#define POLL_CEILING_TEST 0
#define POLL_CEILING_WAIT_SECONDS  5   // time given to let the device enumerate first
#define POLL_CEILING_TEST_SECONDS  5   // measurement window once movement starts
#define POLL_CEILING_WINDOW_MS   100   // sub-window size used to find the peak rate

#if BRIDGE_DEBUG || RATE_MONITOR || POLL_CEILING_TEST
#define DEBUG_BEGIN()      Serial2.begin(115200)
#define DEBUG_PRINTF(...)  Serial2.printf(__VA_ARGS__)
#else
#define DEBUG_BEGIN()
#define DEBUG_PRINTF(...)
#endif

#if RATE_MONITOR || POLL_CEILING_TEST
// Incremented from tuh_hid_report_received_cb() below, read/reset from
// loop() and/or run_poll_ceiling_test() - both only ever run from the
// same core/task as USBHost.task(), so this is single-threaded in
// practice despite not being atomic.
static uint32_t s_report_count;
#endif

static uint32_t last_heartbeat_ms;

// Re-announce currently-mounted devices periodically: tuh_hid_mount_cb()
// only fires once, at the actual moment of USB enumeration - if the
// ESP32 side reboots (e.g. reflashing firmware) while a device is
// already mounted here (RP2040 wasn't rebooted), it would otherwise
// never learn about it at all (REPORT frames keep arriving but never
// get registered/dispatched on that side - see
// mds/2026-08-23_rp2040_as_host_bridge_plan.md). Track mounted devices
// here and periodically re-send their MOUNT frame; usb_host_rp2040_bridge.c's
// registration is idempotent so re-announcing an already-known device
// is harmless.
#define MAX_TRACKED_DEVICES    4
#define MAX_TRACKED_DESC_LEN   512
#define REANNOUNCE_INTERVAL_MS 2000

typedef struct {
  bool in_use;
  uint8_t dev_addr;
  uint8_t idx;
  uint8_t itf_protocol;
  uint16_t desc_len;
  uint8_t desc[MAX_TRACKED_DESC_LEN];
} tracked_device_t;

static tracked_device_t tracked_devices[MAX_TRACKED_DEVICES];
static uint32_t last_reannounce_ms;

static tracked_device_t *find_tracked_device(uint8_t dev_addr, uint8_t idx) {
  for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
    if (tracked_devices[i].in_use && tracked_devices[i].dev_addr == dev_addr && tracked_devices[i].idx == idx) {
      return &tracked_devices[i];
    }
  }
  return NULL;
}

static tracked_device_t *alloc_tracked_device(void) {
  for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
    if (!tracked_devices[i].in_use) {
      return &tracked_devices[i];
    }
  }
  return NULL;
}

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

#if POLL_CEILING_TEST
// Blocking, interactive measurement - see the POLL_CEILING_TEST comment
// above. Runs USBHost.task() itself in a tight loop for the duration
// (mount/report callbacks still fire normally, including send_frame()
// to the ESP32 if one happens to be listening), so any device already
// plugged in enumerates and gets polled exactly as it would in normal
// operation - this just measures the callback rate rather than doing
// anything different with it.
static void run_poll_ceiling_test(void) {
  DEBUG_PRINTF("\r\n[poll-ceiling-test] waiting %us for the device to enumerate...\r\n",
               (unsigned)POLL_CEILING_WAIT_SECONDS);
  uint32_t wait_until = millis() + POLL_CEILING_WAIT_SECONDS * 1000UL;
  while ((int32_t)(wait_until - millis()) > 0) {
    USBHost.task();
  }

  DEBUG_PRINTF("[poll-ceiling-test] move the mouse continuously/steadily for %us NOW...\r\n",
               (unsigned)POLL_CEILING_TEST_SECONDS);
  s_report_count = 0;
  uint32_t last_total = 0;
  uint32_t peak_rate = 0;
  uint32_t window_start = millis();
  uint32_t test_start = window_start;
  uint32_t test_until = test_start + POLL_CEILING_TEST_SECONDS * 1000UL;

  while ((int32_t)(test_until - millis()) > 0) {
    USBHost.task();
    uint32_t now = millis();
    if (now - window_start >= POLL_CEILING_WINDOW_MS) {
      uint32_t current_total = s_report_count;
      uint32_t window_delta = current_total - last_total;
      last_total = current_total;
      window_start = now;
      uint32_t window_rate = window_delta * 1000UL / POLL_CEILING_WINDOW_MS;
      if (window_rate > peak_rate) {
        peak_rate = window_rate;
      }
    }
  }

  uint32_t total_reports = s_report_count;
  uint32_t avg_rate = total_reports * 1000UL / (POLL_CEILING_TEST_SECONDS * 1000UL);
  DEBUG_PRINTF("[poll-ceiling-test] done: %lu reports in %us (avg %lu/s, peak %lu/s over %ums windows)\r\n",
               (unsigned long)total_reports, (unsigned)POLL_CEILING_TEST_SECONDS,
               (unsigned long)avg_rate, (unsigned long)peak_rate, (unsigned)POLL_CEILING_WINDOW_MS);
  DEBUG_PRINTF("[poll-ceiling-test] resuming normal bridge operation.\r\n\r\n");
  s_report_count = 0;
}
#endif

void setup() {
  Serial1.begin(BRIDGE_BAUD);
  DEBUG_BEGIN();
  delay(500);
  DEBUG_PRINTF("RP2040 host bridge: starting\r\n");

  // Match ESP32's approach (explicit SET_PROTOCOL(Report)) - see
  // rp2040_host_check.ino / mds/2026-08-22_rp2040_host_check.md.
  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);

  USBHost.begin(0);

#if POLL_CEILING_TEST
  run_poll_ceiling_test();
#endif

  last_heartbeat_ms = millis();
  last_reannounce_ms = millis();
}

void loop() {
  USBHost.task();

  uint32_t now = millis();

#if RATE_MONITOR
  {
    static uint32_t last_rate_print_ms;
    if (now - last_rate_print_ms >= 1000) {
      last_rate_print_ms = now;
      uint32_t count = s_report_count;
      s_report_count = 0;
      DEBUG_PRINTF("[rate] %lu reports/sec\r\n", (unsigned long)count);
    }
  }
#endif

  // Sent regardless of USB device state so the ESP32 side can probe for
  // this bridge's presence (usb_host_rp2040_bridge_probe()) without
  // needing anything plugged in yet.
  if (now - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS) {
    last_heartbeat_ms = now;
    send_frame(BRIDGE_MSG_HEARTBEAT, 0, 0, 0, NULL, 0);
  }

  if (now - last_reannounce_ms >= REANNOUNCE_INTERVAL_MS) {
    last_reannounce_ms = now;
    for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
      if (tracked_devices[i].in_use) {
        send_frame(BRIDGE_MSG_MOUNT, tracked_devices[i].dev_addr, tracked_devices[i].idx,
                   tracked_devices[i].itf_protocol, tracked_devices[i].desc, tracked_devices[i].desc_len);
      }
    }
  }
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report_desc, uint16_t desc_len) {
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);
  DEBUG_PRINTF("HID mount: dev_addr=%u idx=%u itf_protocol=%u (0=None 1=Keyboard 2=Mouse)\r\n",
               dev_addr, idx, itf_protocol);
#if BRIDGE_DEBUG
  DEBUG_PRINTF("Report descriptor (%u bytes):\r\n", desc_len);
  for (uint16_t i = 0; i < desc_len; i++) {
    DEBUG_PRINTF("%02x ", report_desc[i]);
    if ((i % 16) == 15) DEBUG_PRINTF("\r\n");
  }
  DEBUG_PRINTF("\r\n");
#endif

  // usb_host_rp2040_bridge.c's MAX_PAYLOAD_LEN - a longer descriptor
  // would desync the frame on the ESP32 side, so truncate defensively
  // rather than send something it will reject anyway.
  if (desc_len > 512) {
    desc_len = 512;
  }
  send_frame(BRIDGE_MSG_MOUNT, dev_addr, idx, itf_protocol, report_desc, desc_len);

  // Track for periodic re-announcement (see the block comment above
  // tracked_devices) - find_tracked_device() first in case a stale
  // entry for this dev_addr/idx slipped through without its umount
  // (shouldn't normally happen, but avoids leaking a tracked_devices
  // slot if it did).
  tracked_device_t *tracked = find_tracked_device(dev_addr, idx);
  if (!tracked) {
    tracked = alloc_tracked_device();
  }
  if (tracked) {
    tracked->in_use = true;
    tracked->dev_addr = dev_addr;
    tracked->idx = idx;
    tracked->itf_protocol = itf_protocol;
    tracked->desc_len = desc_len < MAX_TRACKED_DESC_LEN ? desc_len : MAX_TRACKED_DESC_LEN;
    memcpy(tracked->desc, report_desc, tracked->desc_len);
  }

  if (!tuh_hid_receive_report(dev_addr, idx)) {
    DEBUG_PRINTF("Error: cannot request initial report (dev_addr=%u idx=%u)\r\n", dev_addr, idx);
  }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx) {
  DEBUG_PRINTF("HID unmount: dev_addr=%u idx=%u\r\n", dev_addr, idx);
  send_frame(BRIDGE_MSG_UNMOUNT, dev_addr, idx, 0, NULL, 0);

  tracked_device_t *tracked = find_tracked_device(dev_addr, idx);
  if (tracked) {
    tracked->in_use = false;
  }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report, uint16_t len) {
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);
#if RATE_MONITOR || POLL_CEILING_TEST
  s_report_count++;
#endif
#if BRIDGE_DEBUG
  DEBUG_PRINTF("[%u:%u] raw report (%u bytes): ", dev_addr, idx, len);
  for (uint16_t i = 0; i < len; i++) {
    DEBUG_PRINTF("%02x ", report[i]);
  }
  DEBUG_PRINTF("\r\n");
#endif

  if (len > 512) {
    len = 512;
  }
  send_frame(BRIDGE_MSG_REPORT, dev_addr, idx, itf_protocol, report, len);

  // Keep polling - TinyUSB does not auto-resubmit.
  tuh_hid_receive_report(dev_addr, idx);
}
