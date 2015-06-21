/*------------------------------------------------------------------------
  POV LED poi sketch.  Uses the following Adafruit parts (X2 for two poi):

  - Trinket 5V or 3V (adafruit.com/product/1501 or 1500) (NOT Pro Trinket)
  - 150 mAh LiPoly battery (#1317)
  - LiPoly backpack (#2124)
  - Fast (#1766) or medium (#2384) vibration sensor switch

  See comments in code re: vibration switch and other optional parts.

  Use 'soda bottle preform' for enclosure w/5.25" (133 mm) inside depth.
  3D-printable cap and insert can be downloaded from Thingiverse:
  (add link here)
  Add leash - e.g. paracord, or fancy ones available from flowtoys.com.

  Needs Adafruit_DotStar library: github.com/adafruit/Adafruit_DotStar

  Adafruit invests time and resources providing this open source code,
  please support Adafruit and open-source hardware by purchasing
  products from Adafruit!

  Written by Phil Burgess / Paint Your Dragon for Adafruit Industries.
  MIT license, all text above must be included in any redistribution.
  See 'COPYING' file for additional notes.
  ------------------------------------------------------------------------*/

#include <Arduino.h>
#include <Adafruit_DotStar.h>
#include <avr/power.h>
#include <avr/sleep.h>
// #include <SPI.h> // Enable this line on Pro Trinket

// CONFIGURABLE STUFF ------------------------------------------------------

#include "graphics.h" // Graphics data is contained in this header file.
// It's generated using the 'convert.py' Python script.  Various image
// formats are supported, trading off color fidelity for PROGMEM space
// (particularly limited on Trinket).  Handles 1-, 4- and 8-bit-per-pixel
// palette-based images, plus 24-bit truecolor.  1- and 4-bit palettes can
// be altered in RAM while running to provide additional colors, but be
// mindful of peak & average current draw if you do that!  Power limiting
// is normally done in convert.py (keeps this code relatively small & fast).
// 1/4/8/24 were chosen because the AVR can handle these fairly easily;
// unpacking data of arbitrary depth would go slower (no >>n instruction).

// Ideally you use hardware SPI as it's much faster, though limited to
// specific pins.  If you really need to bitbang DotStar data & clock on
// different pins, optionally define those here:
#define LED_DATA_PIN  0
#define LED_CLOCK_PIN 1

// The vibration switch (aligned perpendicular to leash) is used as a
// poor man's accelerometer -- poi lights only when moving, saving some
// power.  The 'fast' vibration switch is VERY sensitive and will trigger
// at the slightest bump...while the 'medium' switch requires a certain
// minimum spin rate which may not trigger if you're doing mellow spins.
// Neither is perfect.  If you'd prefer just to leave out that component
// and have the poi run always-on, comment out this line:
#define MOTION_PIN 2

// Optional: select from multiple images using tactile button (#1489)
// between pin and ground.  Requires a suitably-built graphics.h file with
// more than one image.
#define SELECT_PIN 3

// Experimental: powering down DotStars when idle conserves more battery,
// but space is very tight and this requires creative free-wiring.  Use a
// PNP transistor (e.g. 2N2907) (w/220 Ohm resistor to base) as a 'high
// side' switch to DotStar +V.  DON'T do this NPN/low-side, may damage
// strip.  MOTION_PIN must also be defined to use this (pointless without).
#define POWER_PIN 4

#define SLEEP_TIME 2000  // Not-spinning time before sleep, in milliseconds

// Empty and full thresholds (millivolts) used for battery level display:
#define BATT_MIN_MV 3350 // Some headroom over battery cutoff near 2.9V
#define BATT_MAX_MV 4050 // And little below fresh-charged battery near 4.1V

// -------------------------------------------------------------------------

#if defined(LED_DATA_PIN) && defined(LED_CLOCK_PIN)
Adafruit_DotStar strip = Adafruit_DotStar(NUM_LEDS,
  LED_DATA_PIN, LED_CLOCK_PIN, DOTSTAR_GBR);
#else
Adafruit_DotStar strip = Adafruit_DotStar(NUM_LEDS, DOTSTAR_GBR);
#endif

void setup() {
#if defined(__AVR_ATtiny85__) && (F_CPU == 16000000L)
  clock_prescale_set(clock_div_1);   // Enable 16 MHz on Trinket
#endif

#ifdef POWER_PIN
  pinMode(POWER_PIN, OUTPUT);
  digitalWrite(POWER_PIN, LOW); // Power-on LED strip
#endif
  strip.begin();                // Allocate DotStar buffer, init SPI
  strip.clear();                // Make sure strip is clear
  strip.show();                 // before measuring battery

  // Display battery level bargraph on startup.  It's just a vague estimate
  // based on cell voltage (drops with discharge) but doesn't handle curve.
  uint16_t mV  = readVoltage();
  uint8_t  lvl = (mV >= BATT_MAX_MV) ? NUM_LEDS : // Full (or nearly)
                 (mV <= BATT_MIN_MV) ?        1 : // Drained
                 1 + ((mV - BATT_MIN_MV) * NUM_LEDS + (NUM_LEDS / 2)) /
                 (BATT_MAX_MV - BATT_MIN_MV + 1); // # LEDs lit (1-NUM_LEDS)
  for(uint8_t i=0; i<lvl; i++) {                  // Each LED to batt level...
    uint8_t g = (i * 5 + 2) / NUM_LEDS;           // Red to green
    strip.setPixelColor(i, 4-g, g, 0);
    strip.show();                                 // Animate a bit
    delay(250 / NUM_LEDS);
  }
  delay(1500);                                    // Hold last state a moment
  strip.clear();                                  // Then clear strip
  strip.show();

  imageInit(); // Initialize pointers for default image

#ifdef SELECT_PIN
  pinMode(SELECT_PIN, INPUT_PULLUP);
#endif
#ifdef MOTION_PIN
  pinMode(MOTION_PIN, INPUT_PULLUP);
  sleep();     // Sleep until motion detected
#endif
}

// GLOBAL STATE STUFF ------------------------------------------------------

uint32_t prev        = 0L; // Used for time measurement
uint8_t  imageNumber = 0,  // Current image being displayed
         imageType,        // Image type: PALETTE[1,4,8] or TRUECOLOR
        *imagePalette,     // -> palette data in PROGMEM
        *imagePixels,      // -> pixel data in PROGMEM
         palette[16][3];   // RAM-based color table for 1- or 4-bit images
uint16_t imageLines,       // Number of lines in active image
         imageLine;        // Current line number in image
#ifdef SELECT_PIN
uint8_t  debounce    = 0;  // Debounce counter for image select pin
#endif

void imageInit() { // Initialize global image state for current imageNumber
  imageType    = pgm_read_byte(&images[imageNumber].type);
  imageLines   = pgm_read_word(&images[imageNumber].lines);
  imageLine    = 0;
  imagePalette = (uint8_t *)pgm_read_word(&images[imageNumber].palette);
  imagePixels  = (uint8_t *)pgm_read_word(&images[imageNumber].pixels);
  // 1- and 4-bit images have their color palette loaded into RAM both for
  // faster access and to allow dynamic color changing.  Not done w/8-bit
  // because that would require inordinate RAM (328P could handle it, but
  // I'd rather keep the RAM free for other features in the future).
  if(imageType == PALETTE1)      memcpy_P(palette, imagePalette,  2 * 3);
  else if(imageType == PALETTE4) memcpy_P(palette, imagePalette, 16 * 3);
}

// MAIN LOOP ---------------------------------------------------------------

void loop() {
#ifdef MOTION_PIN
  // Tried to do this with watchdog timer but encountered gas pains, so...
  uint32_t t = millis();               // Current time, milliseconds
  if(!digitalRead(MOTION_PIN)) {       // Vibration switch pulled down?
    prev = t;                          // Yes, reset timer
  } else if((t - prev) > SLEEP_TIME) { // No, SLEEP_TIME elapsed w/no switch?
    sleep();                           // Power down
    prev = t;                          // Reset timer on wake
  }
#endif

#ifdef SELECT_PIN
  if(digitalRead(SELECT_PIN)) {        // Image select?
    debounce = 0;                      // Not pressed -- reset counter
  } else {                             // Pressed...
    if(debounce++ >= 25) {             // Debounce input
      // If you don't have a select button, these two lines could be
      // put in the sleep() function to change images on each wake:
      if(++imageNumber >= NUM_IMAGES) imageNumber = 0;
      imageInit();                     // Switch to next image
      while(!digitalRead(SELECT_PIN)); // Wait for release
      debounce = 0;
    }
  }
#endif

  // Transfer one scanline from pixel data to LED strip:

  switch(imageType) {

    case PALETTE1: { // 1-bit (2 color) palette-based image
      uint8_t  pixelNum = 0, byteNum, bitNum, pixels, idx,
              *ptr = (uint8_t *)&imagePixels[imageLine * NUM_LEDS / 8];
      for(byteNum = NUM_LEDS/8; byteNum--; ) { // Always padded to next byte
        pixels = pgm_read_byte(ptr++);  // 8 pixels of data (pixel 0 = LSB)
        for(bitNum = 8; bitNum--; pixels >>= 1) {
          idx = pixels & 1; // Color table index for pixel (0 or 1)
          strip.setPixelColor(pixelNum++,
            palette[idx][0], palette[idx][1], palette[idx][2]);
        }
      }
      break;
    }

    case PALETTE4: { // 4-bit (16 color) palette-based image
      uint8_t  pixelNum, p1, p2,
              *ptr = (uint8_t *)&imagePixels[imageLine * NUM_LEDS / 2];
      for(pixelNum = 0; pixelNum < NUM_LEDS; ) {
        p2  = pgm_read_byte(ptr++); // Data for two pixels...
        p1  = p2 >> 4;              // Shift down 4 bits for first pixel
        p2 &= 0x0F;                 // Mask out low 4 bits for second pixel
        strip.setPixelColor(pixelNum++,
          palette[p1][0], palette[p1][1], palette[p1][2]);
        strip.setPixelColor(pixelNum++,
          palette[p2][0], palette[p2][1], palette[p2][2]);
      }
      break;
    }

    case PALETTE8: { // 8-bit (256 color) PROGMEM-palette-based image
      uint16_t  o;
      uint8_t   pixelNum,
               *ptr = (uint8_t *)&imagePixels[imageLine * NUM_LEDS];
      for(pixelNum = 0; pixelNum < NUM_LEDS; pixelNum++) {
        o = pgm_read_byte(ptr++) * 3; // Offset into imagePalette
        strip.setPixelColor(pixelNum,
          pgm_read_byte(&imagePalette[o++]),
          pgm_read_byte(&imagePalette[o++]),
          pgm_read_byte(&imagePalette[o]));
      }
      break;
    }

    case TRUECOLOR: { // 24-bit ('truecolor') image (no palette)
      uint8_t  pixelNum,
              *ptr = (uint8_t *)&imagePixels[imageLine * NUM_LEDS * 3];
      for(pixelNum = 0; pixelNum < NUM_LEDS; pixelNum++) {
        strip.setPixelColor(pixelNum,
          pgm_read_byte(ptr++),
          pgm_read_byte(ptr++),
          pgm_read_byte(ptr++));
      }
      break;
    }
  }

  strip.show(); // Refresh LEDs
#if !defined(LED_DATA_PIN) && !defined(LED_CLOCK_PIN)
  delay(1);     // Because hardware SPI is ludicrously fast
#endif
  if(++imageLine >= imageLines) imageLine = 0; // Next scanline, wrap around
}

// POWER-SAVING STUFF -- Relentlessly non-portable -------------------------

#ifdef MOTION_PIN
void sleep() {

  // Turn off LEDs...
  strip.clear();                 // Issue '0' data
  strip.show();
#ifdef POWER_PIN
  digitalWrite(POWER_PIN, HIGH); // Cut power
#if !defined(LED_DATA_PIN) && !defined(LED_CLOCK_PIN)
#ifdef __AVR_ATtiny85__
  pinMode(1, INPUT);             // Set SPI data & clock to inputs else
  pinMode(2, INPUT);             // DotStars power parasitically, jerks.
#else
  pinMode(11, INPUT);
  pinMode(13, INPUT);
#endif // ATtiny
#endif // Data/clock/pins
#endif // POWER_PIN

  power_all_disable(); // Peripherals ALL OFF, best sleep-state battery use

  // Enable pin-change interrupt on motion pin
#ifdef __AVR_ATtiny85__
  PCMSK = _BV(MOTION_PIN);  // Pin mask
  GIMSK = _BV(PCIE);        // Interrupt enable
#else
  volatile uint8_t *p = portInputRegister(digitalPinToPort(MOTION_PIN));
  if(p == &PIND) {          // Pins 0-7 = PCINT16-23
    PCMSK2 = _BV(MOTION_PIN);
    PCICR  = _BV(PCIE2);
  } else if(p == &PINB) {   // Pins 8-13 = PCINT0-5
    PCMSK0 = _BV(MOTION_PIN- 8);
    PCICR  = _BV(PCIE0);
  } else if(p == &PINC) {   // Pins 14-20 = PCINT8-14
    PCMSK1 = _BV(MOTION_PIN-14);
    PCICR  = _BV(PCIE1);
  }
#endif

  // If select pin is enabled, that wakes too!
#ifdef SELECT_PIN
  debounce = 0;
#ifdef __AVR_ATtiny85__
  PCMSK |= _BV(SELECT_PIN); // Add'l pin mask
#else
  volatile uint8_t *p = portInputRegister(digitalPinToPort(SELECT_PIN));
  if(p == &PIND) {        // Pins 0-7 = PCINT16-23
    PCMSK2 = _BV(SELECT_PIN);
    PCICR  = _BV(PCIE2);
  } else if(p == &PINB) { // Pins 8-13 = PCINT0-5
    PCMSK0 = _BV(SELECT_PIN- 8);
    PCICR  = _BV(PCIE0);
  } else if(p == &PINC) { // Pins 14-20 = PCINT8-14
    PCMSK1 = _BV(SELECT_PIN-14);
    PCICR  = _BV(PCIE1);
  }
#endif // ATtiny
#endif // SELECT_PIN

  set_sleep_mode(SLEEP_MODE_PWR_DOWN); // Deepest sleep mode
  sleep_enable();
  interrupts();
  sleep_mode();                        // Power down

  // Resumes here on wake

  // Clear pin change settings so interrupt won't fire again
#ifdef __AVR_ATtiny85__
  GIMSK = PCMSK = 0;
#else
  PCICR = PCMSK0 = PCMSK1 = PCMSK2 = 0;
#endif
  power_timer0_enable();        // Used by millis()
#if !defined(LED_DATA_PIN) && !defined(LED_CLOCK_PIN)
#ifdef __AVR_ATtiny85__
  pinMode(1, OUTPUT);           // Re-enable SPI pins
  pinMode(2, OUTPUT);
  power_usi_enable();           // Used by DotStar
#else
  pinMode(11, OUTPUT);          // Re-enable SPI pins
  pinMode(13, OUTPUT);
  power_spi_enable();           // Used by DotStar
#endif // ATtiny
#endif // Data/clock pins
#ifdef POWER_PIN
  digitalWrite(POWER_PIN, LOW); // Power-up LEDs
#endif
}

EMPTY_INTERRUPT(PCINT0_vect); // Pin change (does nothing, but required)
#ifndef __AVR_ATtiny85__
ISR(PCINT1_vect, ISR_ALIASOF(PCINT0_vect));
ISR(PCINT2_vect, ISR_ALIASOF(PCINT0_vect));
#endif

#endif // MOTION_PIN

// Battery monitoring idea adapted from JeeLabs article:
// jeelabs.org/2012/05/04/measuring-vcc-via-the-bandgap/
// Code from Adafruit TimeSquare project, added Trinket support.
static uint16_t readVoltage() {
  int      i, prev;
  uint8_t  count;
  uint16_t mV;

  // Select AVcc voltage reference + Bandgap (1.8V) input
#ifdef __AVR_ATtiny85__
  ADMUX  = _BV(MUX3) | _BV(MUX2);
#else
  ADMUX  = _BV(REFS0) |
           _BV(MUX3)  | _BV(MUX2) | _BV(MUX1);
#endif
  ADCSRA = _BV(ADEN)  |                          // Enable ADC
           _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0); // 1/128 prescaler (125 KHz)
  // Datasheet notes that the first bandgap reading is usually garbage as
  // voltages are stabilizing.  It practice, it seems to take a bit longer
  // than that.  Tried various delays, but still inconsistent and kludgey.
  // Instead, repeated readings are taken until four concurrent readings
  // stabilize within 10 mV.
  for(prev=9999, count=0; count<4; ) {
    for(ADCSRA |= _BV(ADSC); ADCSRA & _BV(ADSC); ); // Start, await ADC conv.
    i  = ADC;                                       // Result
    mV = i ? (1100L * 1023 / i) : 0;                // Scale to millivolts
    if(abs((int)mV - prev) <= 10) count++;   // +1 stable reading
    else                          count = 0; // too much change, start over
    prev = mV;
  }
  ADCSRA = 0; // ADC off
  return mV;
}
