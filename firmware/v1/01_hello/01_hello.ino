// ANCHOR sketch 01 - is the ESP32-S3 alive?
// Expected: "Hello ANCHOR..." then 0, 1, 2, 3... once per second.
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Hello ANCHOR - the ESP32-S3 is alive!");
}

void loop() {
  Serial.println(millis() / 1000);
  delay(1000);
}
