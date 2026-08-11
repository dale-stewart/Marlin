/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */
#pragma once

/**
 * Arduino's legacy binary-literal macros.
 *
 * `B00000011` predates C++14's `0b00000011` and AVR's Arduino.h defines all 256 of them.
 * Firmware written against a register datasheet still uses them, so a host build has to
 * provide them or the file will not compile. Generated rather than hand-listed: writing out
 * the two that happen to be used today invites the next one to fail the same way.
 */
#define B00000000 0b00000000
#define B00000001 0b00000001
#define B00000010 0b00000010
#define B00000011 0b00000011
#define B00000100 0b00000100
#define B00000101 0b00000101
#define B00000110 0b00000110
#define B00000111 0b00000111
#define B00001000 0b00001000
#define B00001001 0b00001001
#define B00001010 0b00001010
#define B00001011 0b00001011
#define B00001100 0b00001100
#define B00001101 0b00001101
#define B00001110 0b00001110
#define B00001111 0b00001111
#define B00010000 0b00010000
#define B00010001 0b00010001
#define B00010010 0b00010010
#define B00010011 0b00010011
#define B00010100 0b00010100
#define B00010101 0b00010101
#define B00010110 0b00010110
#define B00010111 0b00010111
#define B00011000 0b00011000
#define B00011001 0b00011001
#define B00011010 0b00011010
#define B00011011 0b00011011
#define B00011100 0b00011100
#define B00011101 0b00011101
#define B00011110 0b00011110
#define B00011111 0b00011111
#define B00100000 0b00100000
#define B00100001 0b00100001
#define B00100010 0b00100010
#define B00100011 0b00100011
#define B00100100 0b00100100
#define B00100101 0b00100101
#define B00100110 0b00100110
#define B00100111 0b00100111
#define B00101000 0b00101000
#define B00101001 0b00101001
#define B00101010 0b00101010
#define B00101011 0b00101011
#define B00101100 0b00101100
#define B00101101 0b00101101
#define B00101110 0b00101110
#define B00101111 0b00101111
#define B00110000 0b00110000
#define B00110001 0b00110001
#define B00110010 0b00110010
#define B00110011 0b00110011
#define B00110100 0b00110100
#define B00110101 0b00110101
#define B00110110 0b00110110
#define B00110111 0b00110111
#define B00111000 0b00111000
#define B00111001 0b00111001
#define B00111010 0b00111010
#define B00111011 0b00111011
#define B00111100 0b00111100
#define B00111101 0b00111101
#define B00111110 0b00111110
#define B00111111 0b00111111
#define B01000000 0b01000000
#define B01000001 0b01000001
#define B01000010 0b01000010
#define B01000011 0b01000011
#define B01000100 0b01000100
#define B01000101 0b01000101
#define B01000110 0b01000110
#define B01000111 0b01000111
#define B01001000 0b01001000
#define B01001001 0b01001001
#define B01001010 0b01001010
#define B01001011 0b01001011
#define B01001100 0b01001100
#define B01001101 0b01001101
#define B01001110 0b01001110
#define B01001111 0b01001111
#define B01010000 0b01010000
#define B01010001 0b01010001
#define B01010010 0b01010010
#define B01010011 0b01010011
#define B01010100 0b01010100
#define B01010101 0b01010101
#define B01010110 0b01010110
#define B01010111 0b01010111
#define B01011000 0b01011000
#define B01011001 0b01011001
#define B01011010 0b01011010
#define B01011011 0b01011011
#define B01011100 0b01011100
#define B01011101 0b01011101
#define B01011110 0b01011110
#define B01011111 0b01011111
#define B01100000 0b01100000
#define B01100001 0b01100001
#define B01100010 0b01100010
#define B01100011 0b01100011
#define B01100100 0b01100100
#define B01100101 0b01100101
#define B01100110 0b01100110
#define B01100111 0b01100111
#define B01101000 0b01101000
#define B01101001 0b01101001
#define B01101010 0b01101010
#define B01101011 0b01101011
#define B01101100 0b01101100
#define B01101101 0b01101101
#define B01101110 0b01101110
#define B01101111 0b01101111
#define B01110000 0b01110000
#define B01110001 0b01110001
#define B01110010 0b01110010
#define B01110011 0b01110011
#define B01110100 0b01110100
#define B01110101 0b01110101
#define B01110110 0b01110110
#define B01110111 0b01110111
#define B01111000 0b01111000
#define B01111001 0b01111001
#define B01111010 0b01111010
#define B01111011 0b01111011
#define B01111100 0b01111100
#define B01111101 0b01111101
#define B01111110 0b01111110
#define B01111111 0b01111111
#define B10000000 0b10000000
#define B10000001 0b10000001
#define B10000010 0b10000010
#define B10000011 0b10000011
#define B10000100 0b10000100
#define B10000101 0b10000101
#define B10000110 0b10000110
#define B10000111 0b10000111
#define B10001000 0b10001000
#define B10001001 0b10001001
#define B10001010 0b10001010
#define B10001011 0b10001011
#define B10001100 0b10001100
#define B10001101 0b10001101
#define B10001110 0b10001110
#define B10001111 0b10001111
#define B10010000 0b10010000
#define B10010001 0b10010001
#define B10010010 0b10010010
#define B10010011 0b10010011
#define B10010100 0b10010100
#define B10010101 0b10010101
#define B10010110 0b10010110
#define B10010111 0b10010111
#define B10011000 0b10011000
#define B10011001 0b10011001
#define B10011010 0b10011010
#define B10011011 0b10011011
#define B10011100 0b10011100
#define B10011101 0b10011101
#define B10011110 0b10011110
#define B10011111 0b10011111
#define B10100000 0b10100000
#define B10100001 0b10100001
#define B10100010 0b10100010
#define B10100011 0b10100011
#define B10100100 0b10100100
#define B10100101 0b10100101
#define B10100110 0b10100110
#define B10100111 0b10100111
#define B10101000 0b10101000
#define B10101001 0b10101001
#define B10101010 0b10101010
#define B10101011 0b10101011
#define B10101100 0b10101100
#define B10101101 0b10101101
#define B10101110 0b10101110
#define B10101111 0b10101111
#define B10110000 0b10110000
#define B10110001 0b10110001
#define B10110010 0b10110010
#define B10110011 0b10110011
#define B10110100 0b10110100
#define B10110101 0b10110101
#define B10110110 0b10110110
#define B10110111 0b10110111
#define B10111000 0b10111000
#define B10111001 0b10111001
#define B10111010 0b10111010
#define B10111011 0b10111011
#define B10111100 0b10111100
#define B10111101 0b10111101
#define B10111110 0b10111110
#define B10111111 0b10111111
#define B11000000 0b11000000
#define B11000001 0b11000001
#define B11000010 0b11000010
#define B11000011 0b11000011
#define B11000100 0b11000100
#define B11000101 0b11000101
#define B11000110 0b11000110
#define B11000111 0b11000111
#define B11001000 0b11001000
#define B11001001 0b11001001
#define B11001010 0b11001010
#define B11001011 0b11001011
#define B11001100 0b11001100
#define B11001101 0b11001101
#define B11001110 0b11001110
#define B11001111 0b11001111
#define B11010000 0b11010000
#define B11010001 0b11010001
#define B11010010 0b11010010
#define B11010011 0b11010011
#define B11010100 0b11010100
#define B11010101 0b11010101
#define B11010110 0b11010110
#define B11010111 0b11010111
#define B11011000 0b11011000
#define B11011001 0b11011001
#define B11011010 0b11011010
#define B11011011 0b11011011
#define B11011100 0b11011100
#define B11011101 0b11011101
#define B11011110 0b11011110
#define B11011111 0b11011111
#define B11100000 0b11100000
#define B11100001 0b11100001
#define B11100010 0b11100010
#define B11100011 0b11100011
#define B11100100 0b11100100
#define B11100101 0b11100101
#define B11100110 0b11100110
#define B11100111 0b11100111
#define B11101000 0b11101000
#define B11101001 0b11101001
#define B11101010 0b11101010
#define B11101011 0b11101011
#define B11101100 0b11101100
#define B11101101 0b11101101
#define B11101110 0b11101110
#define B11101111 0b11101111
#define B11110000 0b11110000
#define B11110001 0b11110001
#define B11110010 0b11110010
#define B11110011 0b11110011
#define B11110100 0b11110100
#define B11110101 0b11110101
#define B11110110 0b11110110
#define B11110111 0b11110111
#define B11111000 0b11111000
#define B11111001 0b11111001
#define B11111010 0b11111010
#define B11111011 0b11111011
#define B11111100 0b11111100
#define B11111101 0b11111101
#define B11111110 0b11111110
#define B11111111 0b11111111


#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <cstring>

#include <pinmapping.h>

#define HIGH         0x01
#define LOW          0x00

#define INPUT          0x00
#define OUTPUT         0x01
#define INPUT_PULLUP   0x02
#define INPUT_PULLDOWN 0x03

#define LSBFIRST     0
#define MSBFIRST     1

#define CHANGE       0x02
#define FALLING      0x03
#define RISING       0x04

typedef uint8_t byte;
#define PROGMEM
#define PSTR(v) (v)
#define PGM_P const char *

// Used for libraries, preprocessor, and constants
#define abs(x) ((x)>0?(x):-(x))

#ifndef isnan
  #define isnan std::isnan
#endif
#ifndef isinf
  #define isinf std::isinf
#endif

#define sq(v) ((v) * (v))
#define constrain(value, arg_min, arg_max) ((value) < (arg_min) ? (arg_min) :((value) > (arg_max) ? (arg_max) : (value)))

// Interrupts
void cli(); // Disable
void sei(); // Enable
void attachInterrupt(uint32_t pin, void (*callback)(), uint32_t mode);
void detachInterrupt(uint32_t pin);

extern "C" {
  void GpioEnableInt(uint32_t port, uint32_t pin, uint32_t mode);
  void GpioDisableInt(uint32_t port, uint32_t pin);
}

// Time functions
extern "C" void delay(const int ms);
void delayMicroseconds(unsigned long);
unsigned long millis();

// IO functions
void pinMode(const pin_t, const uint8_t);
void digitalWrite(pin_t, uint8_t);
bool digitalRead(pin_t);
void analogWrite(pin_t, int);
uint16_t analogRead(pin_t);

int32_t random(int32_t);
int32_t random(int32_t, int32_t);
void randomSeed(uint32_t);

char *dtostrf(double __val, signed char __width, unsigned char __prec, char *__s);

int map(uint16_t x, uint16_t in_min, uint16_t in_max, uint16_t out_min, uint16_t out_max);
