// The SK6812 frame the HAT's LED driver loads into the PCM FIFO.
#include <cstdio>

#include "hat_leds.h"

static int failures = 0;
#define CHECK(c) \
    do { \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

int main() {
    using aow::hat::sk6812Byte;
    // Each LED bit is four PCM bits, most significant first: 0 = 1000, 1 = 1100.
    CHECK(sk6812Byte(0x00) == 0x88888888u);
    CHECK(sk6812Byte(0xFF) == 0xCCCCCCCCu);
    CHECK(sk6812Byte(0x80) == 0xC8888888u);
    CHECK(sk6812Byte(0x01) == 0x8888888Cu);
    CHECK(sk6812Byte(0xA5) == 0xC8C88C8Cu);

    // Chain order: LED 0 first; wire order G, R, B; a zero word last so the line rests low.
    const aow::indicators::Rgb leds[4] = {{0xFF, 0x00, 0x01}, {}, {}, {0x00, 0x80, 0x00}};
    uint32_t f[aow::hat::kLedWords];
    aow::hat::sk6812Frame(leds, f);
    CHECK(f[0] == 0x88888888u);  // LED 0 green 0
    CHECK(f[1] == 0xCCCCCCCCu);  // LED 0 red FF
    CHECK(f[2] == 0x8888888Cu);  // LED 0 blue 01
    CHECK(f[3] == 0x88888888u && f[8] == 0x88888888u);
    CHECK(f[9] == 0xC8888888u);  // LED 3 green 80
    CHECK(f[12] == 0);
    CHECK(aow::hat::kLedWords <= 64);  // must fit the PCM FIFO

    if (failures) std::printf("hat_leds: %d failure(s)\n", failures);
    else std::printf("all hat_leds tests passed\n");
    return failures ? 1 : 0;
}
