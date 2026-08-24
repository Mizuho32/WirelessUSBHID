// Bring-up check for the RP2040 board + toolchain/flash pipeline,
// before touching the more complex USB Host sketch in
// rp2040_host_check/ - see mds/usb_hid/2026-08-22_rp2040_host_check.md.
//
// Uses the default USB stack (regular USB-CDC Serial, no TinyUSB
// Host), so this can be verified with the normal Arduino Serial
// Monitor / any terminal - no UART adapter needed yet.
void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);
  Serial2.begin(115200);
  Serial.println("Setup from RP2040! 0");
  Serial1.println("Setup from RP2040! 1");
  Serial2.println("Setup from RP2040! 2");
  pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
  Serial.println("Hello from RP2040! 0");
  Serial1.println("Hello from RP2040! 1");
  Serial2.println("Hello from RP2040! 2");
  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
  digitalWrite(LED_BUILTIN, LOW);
  delay(100);
}
