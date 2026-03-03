#include <Adafruit_NeoPixel.h>

#define STRIP_PIN 8
#define NUM_PIXELS 19

Adafruit_NeoPixel strip(NUM_PIXELS, STRIP_PIN, NEO_GRB + NEO_KHZ800);

uint8_t brightness = 0;
int8_t direction = 1;



void setup() {
  strip.begin();
  for (int i = 0; i < NUM_PIXELS; i++) {
    strip.setPixelColor(i, strip.Color(0, 0, 0)); // Black
  }
    strip.setPixelColor(1, strip.Color(0, 120, 0)); // Red
    strip.setPixelColor(2, strip.Color(0, 120, 0)); // Red
    strip.setPixelColor(3, strip.Color(0, 120, 0)); // Red
    strip.setPixelColor(4, strip.Color(0, 120, 0)); // Red
    strip.setPixelColor(5, strip.Color(0, 120, 0)); // Red

    strip.setPixelColor(7, strip.Color(0, 120, 0)); // Red
    strip.setPixelColor(8, strip.Color(0, 120, 0)); // Red
    strip.setPixelColor(9, strip.Color(0, 120, 0)); // Red
    strip.setPixelColor(10, strip.Color(0, 120, 0)); // Red
    strip.setPixelColor(11, strip.Color(0, 120, 0)); // Red

    strip.setPixelColor(13, strip.Color(0, 120, 0)); // Red
    strip.setPixelColor(14, strip.Color(0, 120, 0)); // Red
    strip.setPixelColor(15, strip.Color(0, 120, 0)); // Red
    strip.setPixelColor(16, strip.Color(0, 120, 0)); // Red
    strip.setPixelColor(17, strip.Color(0, 120, 0)); // Red

  strip.show();

}

void loop() {
  delay(10);

}
