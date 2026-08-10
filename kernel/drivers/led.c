#include "drivers/led.h"

__attribute__((weak)) int platform_led_set(int enabled)
{
    (void)enabled;
    return -1;
}

static int led_state;

static void led_set(int enabled)
{
    led_state = enabled ? 1 : 0;
    (void)platform_led_set(led_state);
}

void led_on(void)
{
    led_set(1);
}

void led_off(void)
{
    led_set(0);
}

void led_toggle(void)
{
    led_set(!led_state);
}

int led_status(void)
{
    return led_state;
}
