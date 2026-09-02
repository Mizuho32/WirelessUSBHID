// USB Host bridge: RP2040 does the actual USB Host role (TinyUSB Host,
// same proven-working core as rp2040_host_check.ino) and forwards
// already-decoded HID mount/report events to an ESP32-S3 over a plain
// UART link, instead of an SPI-attached MAX3421E - see
// mds/usb_hid/2026-08-23_rp2040_as_host_bridge_plan.md for why (MAX3421E's SPI
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

// Dormant sleep (mds/usb_hid/2026-08-31_rp2040_sleep_plan.md) - base
// pico-sdk calls only. Tried pico-sdk's own official pico_low_power
// library first (this core's bundled pico-sdk has it, unlike pico-extras'
// older pico_sleep, which this core doesn't bundle at all) - rejected
// after it turned out to hard-fail the link: low_power.c is compiled
// (inside this core's prebuilt lib/rp2040/libpico.a) with tinyusb-aware
// USB shutdown/restart around the actual dormant call (tuh_deinit()/
// tuh_init()), which drags in libpico.a's own bundled copy of TinyUSB -
// a straight-up "multiple definition" clash against this sketch's real
// TinyUSB Host stack (Adafruit_TinyUSB_Arduino, a separate full copy
// compiled per-sketch for actual USB Host operation, USE_TINYUSB_HOST).
// See enter_rp2040_dormant() below for the hand-rolled sequence instead.
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pll.h"
#include "hardware/xosc.h"

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
// RP2040-side RATE_MONITOR stats, sent over the same Serial1 link so
// they show up in the ESP32's own console log without needing a
// separate USB-serial adapter on Serial2 - see the RATE_MONITOR comment
// below. Payload: 3x uint32 LE (reports_per_sec, min_interval_us,
// max_interval_us) - safe to memcpy as a struct since both RP2040 (ARM,
// Cortex-M0+) and ESP32-S3 (Xtensa) are little-endian.
#define BRIDGE_MSG_STATS      0x05
// ESP32->RP2040 direction (opposite of everything above) - see
// mds/usb_hid/2026-08-31_rp2040_sleep_plan.md and usb_host_rp2040_bridge.c's
// matching enum. Both len=0. BRIDGE_CMD_WAKE's *content* doesn't actually
// do anything here - by the time this sketch's UART is back up to parse
// it, the dormant wake (a GPIO edge, not a parsed command) has already
// happened. It's sent anyway so this side has something to log to confirm
// the post-wake resync worked.
#define BRIDGE_CMD_SLEEP      0x06
#define BRIDGE_CMD_WAKE       0x07
#define BRIDGE_BAUD           460800
// GP1 doubles as Serial1's RX pin and the dormant-wake GPIO - see
// enter_rp2040_dormant(). Must match usb_host_rp2040_bridge.c's
// BRIDGE_UART_RX_PIN wiring (cross-connected: ESP32 TX -> this pin).
#define BRIDGE_UART_RX_PIN    1

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
// mds/usb_hid/2026-08-22_rp2040_host_check.md - there's no UART bridge behind
// it, so it can't be used for debug output at all once acting as Host
// (same reason Serial1 had to be repurposed for the bridge protocol
// instead of debug text in the first place).
#define BRIDGE_DEBUG 0

// Toggle for a running reports/sec counter, printed on Serial2 once a
// second (mds/usb_hid/2026-08-24_rp2040_bridge_fps_investigation.md measurement
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
// chasing the type-c crash (mds/usb_hid/2026-08-23_rp2040_host_status.md) - an
// integer increment plus one printf/sec should not have that problem,
// but keep an eye out.
#define RATE_MONITOR 0

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

#if RATE_MONITOR
// Tracks the shortest gap seen between two consecutive
// tuh_hid_report_received_cb() calls, reset every print window
// (mds/usb_hid/2026-08-24_rp2040_bridge_fps_investigation.md follow-up: ESP32
// dispatch/UART/type-c submission all measured clean at ~100Hz with no
// drops, so if this device is actually *capable* of polling faster than
// that, the ceiling must be here on the RP2040 host side - either the
// device's own bInterval, or the single-buffered "process then re-arm"
// pattern below adding software latency between polls. A steady
// ~10000us minimum every window points at the device's own bInterval
// (nothing to fix); a minimum well below the observed average interval
// (e.g. bursts of ~2000-3000us gaps that don't sustain) points at a
// software-side stall being the real ceiling instead.
static uint32_t s_last_report_us;
static uint32_t s_min_interval_us;
static uint32_t s_max_interval_us; // the "quiet gap" size, if delivery is bursty
#endif

static uint32_t last_heartbeat_ms;

// Re-announce currently-mounted devices periodically: tuh_hid_mount_cb()
// only fires once, at the actual moment of USB enumeration - if the
// ESP32 side reboots (e.g. reflashing firmware) while a device is
// already mounted here (RP2040 wasn't rebooted), it would otherwise
// never learn about it at all (REPORT frames keep arriving but never
// get registered/dispatched on that side - see
// mds/usb_hid/2026-08-23_rp2040_as_host_bridge_plan.md). Track mounted devices
// here and periodically re-send their MOUNT frame; usb_host_rp2040_bridge.c's
// registration is idempotent so re-announcing an already-known device
// is harmless.
// 8 (was 4): a hub with a 5-button mouse (1 HID interface) + keyboard
// (3 HID interfaces - Boot Keyboard, a Vendor Page 0xFF60 interface, and
// a Report-ID-multiplexed complex interface, see
// mds/usb_hid/2026-08-22_consumer_control.md) + a 9-button mouse (up to
// ~4 interfaces per mds/usb_hid/2026-08-22_9buttons_mouse.md's
// investigation - main mouse, a macro keyboard IF, likely a Consumer
// Control IF, possibly a vendor IF) adds up to more than 4 mountable HID
// interfaces. With the table full, tuh_hid_mount_cb() below silently
// dropped whichever interface(s) mounted last (order/timing-dependent
// at cold power-on) from ever being tracked - meaning if the ESP32
// boots/reboots *after* the RP2040 already enumerated everything, those
// dropped interfaces are never re-announced (the periodic reannounce
// loop below is the *only* way an already-running RP2040 tells a
// freshly-booted ESP32 about devices it missed the original one-shot
// MOUNT frame for) and stay invisible on that side until the RP2040
// itself is reset. Bumped well past today's actual interface count for
// headroom.
#define MAX_TRACKED_DEVICES    8
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

// LED_BUILTIN as a simple "HID device present" indicator: on once any HID
// interface has mounted, off again once the last one unmounts. Counts
// mounted *interfaces* independently of tracked_devices[] (which can fill
// up - see the MAX_TRACKED_DEVICES comment above - and shouldn't gate this
// LED), so a keyboard alone mounting 3 interfaces just means 3 increments/
// decrements that net out correctly.
//
// The "off" side doubles as a placeholder for the not-yet-implemented
// dormant-sleep trigger from mds/usb_hid/2026-08-31_rp2040_sleep_plan.md
// (currently on hold): once ESP32->RP2040 sleep commands exist, that
// handler should turn this LED off too, same as reaching zero mounted
// interfaces does today.
static uint16_t s_hid_mount_count;

static void update_hid_led(void) {
  digitalWrite(LED_BUILTIN, s_hid_mount_count > 0 ? HIGH : LOW);
}

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

// Puts the chip into dormant sleep, blocking until a falling edge on
// BRIDGE_UART_RX_PIN wakes it (armed below) - see
// mds/usb_hid/2026-08-31_rp2040_sleep_plan.md. Dormants the crystal (XOSC),
// not the ROSC - so, unlike pico_low_power's own ROSC-sourced dormant path
// (rejected above), ROSC is never touched here and needs no explicit
// restart on the way back.
//
//   1. Detach clk_ref/clk_sys from their PLLs onto the crystal directly,
//      *before* stopping it. Skipping this is the actual failure mode in
//      raspberrypi/pico-extras#41 ("Pico doesn't wake up properly from
//      Dormant/Sleep") - clk_sys stays routed through pll_sys, whose
//      reference (XOSC) is about to vanish; when XOSC comes back, the PLL
//      doesn't just relock on its own, and clk_sys can end up wedged.
//   2. Stop clk_adc/clk_usb (unused while dormant) and deinit both PLLs -
//      nothing should still expect either of them running once XOSC stops.
//   3. Reconfigure the RX pin as a plain GPIO input and arm the dormant
//      wake IRQ on it (falling edge = a UART start bit, whether that's a
//      real BRIDGE_CMD_WAKE frame or just the next heartbeat).
//   4. xosc_dormant() - actually stops the crystal. Execution freezes here
//      (this function doesn't return by executing further instructions -
//      clk_sys itself has stopped) until the armed GPIO edge restarts it,
//      at which point xosc_dormant() resumes running and returns once
//      XOSC reports stable again.
//   5. xosc_dormant() returning means we're awake again, but the USB Host
//      controller doesn't survive the trip - clk_usb was stopped in step 2,
//      and nothing here re-drives root-port reset/re-enumeration for
//      USBHost/TinyUSB afterwards. Confirmed on real hardware
//      (mds/usb_hid/2026-09-02_rp2040_sleep_impl.md): a single directly-
//      attached device comes back with corrupted bridge-UART traffic
//      (something is still arriving, worse the more the device moves, but
//      TinyUSB's own state is desynced from the controller), and a
//      downstream hub doesn't come back at all - its port-status/interrupt
//      endpoint handshaking with the now-confused host controller never
//      recovers. A full chip reset (physical RST) always fixes it, so
//      rather than try to carefully resume USBHost in place, just do that
//      in software instead - see rp2040.reboot() below.
//
// UNVERIFIED ON REAL HARDWARE beyond the above as of this writing - see
// mds/usb_hid/2026-08-31_rp2040_sleep_plan.md's 未検証 section (mainly:
// whether a 460800bps UART start bit's ~2us low pulse reliably trips the
// dormant GPIO edge detector). If xosc_dormant() itself hangs instead of
// waking, recovery is a normal BOOTSEL reflash (nothing is corrupted/
// persisted), but it does mean physical access is needed - exactly why
// usb_suspend_rp2040_sleep defaults off on the ESP32 side
// (mruby_filter.h) until confirmed working.
static void enter_rp2040_dormant(void) {
  DEBUG_PRINTF("SLEEP cmd received - entering dormant\r\n");
  digitalWrite(LED_BUILTIN, LOW);

  Serial1.end();
  gpio_init(BRIDGE_UART_RX_PIN);
  gpio_set_input_enabled(BRIDGE_UART_RX_PIN, true);
  gpio_set_dormant_irq_enabled(BRIDGE_UART_RX_PIN, GPIO_IRQ_EDGE_FALL, true);

  clock_configure(clk_ref, CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC, 0,
                  XOSC_MHZ * MHZ, XOSC_MHZ * MHZ);
  clock_configure(clk_sys, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF, 0,
                  XOSC_MHZ * MHZ, XOSC_MHZ * MHZ);
  clock_stop(clk_adc);
  clock_stop(clk_usb);
  pll_deinit(pll_sys);
  pll_deinit(pll_usb);

  xosc_dormant(); // blocks until woken

  // Warm chip reset (watchdog_reboot() under the hood) - never returns.
  // setup() runs fresh from here, including a clean USBHost.begin(), same
  // as a physical RST - see the comment above.
  rp2040.reboot();
}

// ── Incoming (ESP32->RP2040) command parser ────────────────────────
// Mirrors usb_host_rp2040_bridge.c's own parser field-for-field (same
// frame format, opposite direction), but only BRIDGE_CMD_SLEEP/_WAKE exist
// so far and both are len=0 - any payload bytes are just consumed and
// discarded to stay in sync with the checksum, not stored anywhere.
enum {
  CST_WAIT_SYNC, CST_TYPE, CST_ADDR, CST_IDX, CST_PROTO, CST_LEN_LO, CST_LEN_HI, CST_PAYLOAD, CST_CHECKSUM,
};

static uint8_t  s_cmd_state = CST_WAIT_SYNC;
static uint8_t  s_cmd_msg_type;
static uint16_t s_cmd_len, s_cmd_payload_idx;
static uint8_t  s_cmd_checksum;

static void handle_cmd_frame(void) {
  switch (s_cmd_msg_type) {
    case BRIDGE_CMD_SLEEP:
      enter_rp2040_dormant();
      break;
    case BRIDGE_CMD_WAKE:
      DEBUG_PRINTF("WAKE cmd received (already awake - informational only)\r\n");
      break;
    default:
      DEBUG_PRINTF("unknown incoming msg_type 0x%02x, ignoring\r\n", s_cmd_msg_type);
      break;
  }
}

static void feed_cmd_byte(uint8_t b) {
  switch (s_cmd_state) {
    case CST_WAIT_SYNC:
      if (b == BRIDGE_SYNC_BYTE) s_cmd_state = CST_TYPE;
      break;
    case CST_TYPE:
      s_cmd_msg_type = b;
      s_cmd_checksum = b;
      s_cmd_state = CST_ADDR;
      break;
    case CST_ADDR:
      s_cmd_checksum ^= b;
      s_cmd_state = CST_IDX;
      break;
    case CST_IDX:
      s_cmd_checksum ^= b;
      s_cmd_state = CST_PROTO;
      break;
    case CST_PROTO:
      s_cmd_checksum ^= b;
      s_cmd_state = CST_LEN_LO;
      break;
    case CST_LEN_LO:
      s_cmd_len = b;
      s_cmd_checksum ^= b;
      s_cmd_state = CST_LEN_HI;
      break;
    case CST_LEN_HI:
      s_cmd_len |= (uint16_t)((uint16_t)b << 8);
      s_cmd_checksum ^= b;
      s_cmd_payload_idx = 0;
      s_cmd_state = (s_cmd_len == 0) ? CST_CHECKSUM : CST_PAYLOAD;
      break;
    case CST_PAYLOAD:
      s_cmd_checksum ^= b;
      if (++s_cmd_payload_idx >= s_cmd_len) s_cmd_state = CST_CHECKSUM;
      break;
    case CST_CHECKSUM:
      if (b == s_cmd_checksum) {
        handle_cmd_frame();
      } else {
        DEBUG_PRINTF("incoming cmd checksum mismatch (msg_type=0x%02x), resyncing\r\n", s_cmd_msg_type);
      }
      s_cmd_state = CST_WAIT_SYNC;
      break;
  }
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
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial1.begin(BRIDGE_BAUD);
  DEBUG_BEGIN();
  delay(500);
  DEBUG_PRINTF("RP2040 host bridge: starting\r\n");

  // Match ESP32's approach (explicit SET_PROTOCOL(Report)) - see
  // rp2040_host_check.ino / mds/usb_hid/2026-08-22_rp2040_host_check.md.
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

  while (Serial1.available()) {
    feed_cmd_byte((uint8_t)Serial1.read());
  }

  uint32_t now = millis();

#if RATE_MONITOR
  {
    static uint32_t last_rate_print_ms;
    if (now - last_rate_print_ms >= 1000) {
      last_rate_print_ms = now;
      uint32_t count = s_report_count;
      uint32_t min_interval = s_min_interval_us;
      uint32_t max_interval = s_max_interval_us;
      s_report_count = 0;
      s_min_interval_us = 0;
      s_max_interval_us = 0;
      DEBUG_PRINTF("[rate] %lu reports/sec, min interval %luus, max interval %luus\r\n",
                    (unsigned long)count, (unsigned long)min_interval, (unsigned long)max_interval);

      // Also send as a BRIDGE_MSG_STATS frame over the same Serial1 link
      // to the ESP32 - shows up in its console log
      // (usb_host_rp2040_bridge.c logs it directly under an
      // "[rp2040-rate]" tag) without needing a second USB-serial adapter
      // wired to Serial2 just to see these numbers.
      uint32_t stats_payload[3] = { count, min_interval, max_interval };
      send_frame(BRIDGE_MSG_STATS, 0, 0, 0, (const uint8_t *)stats_payload, sizeof(stats_payload));
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
  } else {
    // Table full (MAX_TRACKED_DEVICES) - this interface's initial MOUNT
    // frame above still went out, so it works fine as long as the ESP32
    // was already listening at that exact moment, but it will never be
    // re-announced (see the tracked_devices comment) if the ESP32 boots
    // later. Was silent before - this was hard to diagnose.
    DEBUG_PRINTF("WARNING: tracked_devices full (%d) - dev_addr=%u idx=%u will not be re-announced\r\n",
                 MAX_TRACKED_DEVICES, dev_addr, idx);
  }

  if (!tuh_hid_receive_report(dev_addr, idx)) {
    DEBUG_PRINTF("Error: cannot request initial report (dev_addr=%u idx=%u)\r\n", dev_addr, idx);
  }

  s_hid_mount_count++;
  update_hid_led();
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx) {
  DEBUG_PRINTF("HID unmount: dev_addr=%u idx=%u\r\n", dev_addr, idx);
  send_frame(BRIDGE_MSG_UNMOUNT, dev_addr, idx, 0, NULL, 0);

  tracked_device_t *tracked = find_tracked_device(dev_addr, idx);
  if (tracked) {
    tracked->in_use = false;
  }

  if (s_hid_mount_count > 0) {
    s_hid_mount_count--;
  }
  update_hid_led();
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report, uint16_t len) {
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);
#if RATE_MONITOR || POLL_CEILING_TEST
  s_report_count++;
#endif
#if RATE_MONITOR
  {
    uint32_t now_us = micros();
    if (s_last_report_us != 0) {
      uint32_t interval = now_us - s_last_report_us;
      if (s_min_interval_us == 0 || interval < s_min_interval_us) {
        s_min_interval_us = interval;
      }
      if (interval > s_max_interval_us) {
        s_max_interval_us = interval;
      }
    }
    s_last_report_us = now_us;
  }
#endif
#if BRIDGE_DEBUG
  DEBUG_PRINTF("[%u:%u] raw report (%u bytes): ", dev_addr, idx, len);
  for (uint16_t i = 0; i < len; i++) {
    DEBUG_PRINTF("%02x ", report[i]);
  }
  DEBUG_PRINTF("\r\n");
#endif

  // Reverted: an earlier attempt here re-armed (tuh_hid_receive_report())
  // BEFORE send_frame(), on the theory that send_frame()'s ~350us of
  // Serial1.write() calls were adding that much software latency to
  // every poll cycle before TinyUSB was told to accept the next
  // transfer. In practice this corrupted the UART stream instead
  // (checksum mismatches / bogus frame lengths on the ESP32 side,
  // worst right after boot) - re-arming appears able to trigger a
  // reentrant call into this same callback (a burst of already-buffered
  // reports arriving back-to-back) before the in-progress send_frame()
  // for the previous report had finished writing all its bytes,
  // interleaving two frames' bytes on the wire. Measured impact was
  // negligible anyway (matches the ~350us << 10ms observed report
  // period), so not worth the risk - back to the safe order: fully
  // transmit, then re-arm.
  if (len > 512) {
    len = 512;
  }
  send_frame(BRIDGE_MSG_REPORT, dev_addr, idx, itf_protocol, report, len);
  tuh_hid_receive_report(dev_addr, idx);
}
