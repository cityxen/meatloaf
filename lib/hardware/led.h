#ifndef LED_H
#define LED_H

#include "../../include/pinmap.h"

#include "fnSystem.h"

enum eLed
{
    LED_WIFI = 0,
    LED_BUS,
    LED_BT,
    LED_COUNT
};

class LedManager
{
public:
    LedManager();
    void setup();
    void set(eLed led, bool one=true);
    void toggle(eLed led);
    void blink(eLed led, int count=1);

private:
    bool mLedState[eLed::LED_COUNT] = { 0 };
    gpio_num_t mLedPin[eLed::LED_COUNT];
};

extern LedManager fnLedManager;
#endif // guard
