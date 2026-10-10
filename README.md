# fesk-demo-face
A [Second Movement](https://github.com/joeycastillo/second-movement) watch face that sends text as sound, using FESK (frequency shift keying) over the piezo buzzer.

## Description

Transmits either a built-in test message or the contents of the file `test` on the watch filesystem. Messages can be decoded with [fesk-rt](https://github.com/eiriksm/fesk-rt). See `src/fesk_demo_face.h` for the buttons.

## Installation

Install with composer:

```
composer require eiriksm/fesk-demo-face:dev-main
```

The face depends on the Second Movement firmware headers (`movement.h`, `fesk_session.h`, `filesystem.h` and friends), so it only builds as part of the firmware.
