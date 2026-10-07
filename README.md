# esphome-i2s-clockless-led-strip

> [!NOTE]
> This component is being upstreamed to ESPHome in [PR #20255](https://github.com/esphome/esphome/pull/20255).

An ESPHome component for driving a clockless LED strip with the I2S peripheral and DMA.  It works with the WS2811 / WS2812B / SK6812 family of devices with RGB or RGBW pixels.

This component is similar to the [esp32_rmt_led_strip](https://esphome.io/components/light/esp32_rmt_led_strip/) component but it is more immune to flickering.  The RMT peripheral can only buffer enough symbols for a few pixels at once and flickering occurs when the RMT interrupt cannot run fast enough to refill the buffer because the microcontroller is busy performing higher priority tasks.  Conversely, the I2S peripheral can use much bigger buffers and tolerates more interrupt latency at the cost of more memory.

## Component schema

```yaml
light:
  - platform: i2s_clockless_led_strip
    id: my_light
    name: "My Light"
    pin: GPIOXX
    num_leds: 30
    channel_colors: GRB
```

## Configuration variables

- **pin** (**Required**, [Pin Schema](https://esphome.io/guides/configuration-types#pin-schema)): The pin for the data line of the light.
- **num_leds** (**Required**, int): The number of LEDs in the strip.
- **channel_colors** (**Required**, string): The order in which the strip expects its color channels. List each of
  `R`, `G` and `B` exactly once, optionally with a single `W` in any position; for example `GRB`, `BGR`, `GRBW`,
  `WRGB` or `RWGB`. The value is case-insensitive. Including a `W` makes this a four channel RGBW strip.
- All other options from [Light](https://esphome.io/components/light/#config-light).

## Development

This project follows the ESPHome code style with [pre-commit](https://pre-commit.com/) hooks for linting and code formatting.

To run the style checks manually on all files and fix issues, run the following command:

```bash
uv run pre-commit run --all-files
```
