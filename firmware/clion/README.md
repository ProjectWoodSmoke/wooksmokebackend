# Air Quality Monitor firmware — CLion port

A port of `firmware/main.cpp` (Arduino IDE, Arduino Nano ESP32) to a CLion +
PlatformIO project, split from one 1,656-line file into per-subsystem modules.

**Runtime behaviour is unchanged.** Same boot sequence, same log lines, same
loop order, same timings, same wire format. Only two things differ from the
Arduino IDE version, both build-level:

1. The build is driven by `platformio.ini` instead of the IDE's board menus.
2. Device credentials moved out of the source into `include/secrets.h`, which
   is gitignored.

---

## Why PlatformIO

CLion has no native Arduino support. The three realistic paths are PlatformIO,
ESP-IDF, and hand-rolled CMake around `arduino-cli`. PlatformIO is the one with
a first-class CLion plugin (build/upload/monitor from the toolbar, working code
insight), it keeps the Arduino framework — so the existing code compiles as-is,
which ESP-IDF would not — and it pins library versions per project instead of
sharing one global `~/Documents/Arduino/libraries`.

CMake is not the build system here. `CMakeLists.txt` exists only so CLion has a
project to open and index; the actual compile goes through PlatformIO.

---

## First-time setup

1. **Install PlatformIO Core**

   ```bash
   brew install platformio
   ```

   On macOS this is cleaner than `pip install --user platformio`, which would
   land in the Command Line Tools' Python 3.9. Homebrew puts `pio` in
   `/opt/homebrew/bin`.

2. **Install the PlatformIO plugin in CLion**

   Settings → Plugins → Marketplace → "PlatformIO for CLion". Restart CLion.

3. **Create your secrets file**

   ```bash
   cp include/secrets.example.h include/secrets.h
   ```

   Then fill in `PRODUCT_UID`, `AUTH_TOKEN` and the device identity. The values
   the Arduino sketch used are in `firmware/main.cpp` in git history — note that
   the token there has been committed in plaintext and should be rotated.

4. **Restore the Bosch BMV080 SDK** if `lib/SparkFun_BMV080_Arduino_Library/`
   is missing its `.a` files — see "The BMV080 needs Bosch's SDK" below. Without
   it the build fails on `pm_sensor.cpp` and nothing else.

5. **Generate the CLion project files**

   ```bash
   pio project init --ide clion
   ```

   This writes `CMakeListsPrivate.txt` (toolchain paths, framework include dirs,
   `-D` defines) and downloads every library into `.pio/libdeps/`. First run
   also pulls the ESP32 toolchain — expect a few minutes.

6. **Open this folder in CLion** (`firmware/clion`, not the repo root). When it
   asks "Open as: CMake project / PlatformIO project", choose **PlatformIO
   project** — that gives you working Build/Upload/Monitor configurations. The
   plain CMake option only gets you indexing.

   If the plugin reports "PlatformIO Core not found": Toolbox launches CLion via
   `open -na`, which does not inherit your shell `PATH`, so `/opt/homebrew/bin`
   can be invisible to it. Set the full path in
   Settings → Build, Execution, Deployment → PlatformIO, or just build from
   CLion's built-in terminal, which does have your `PATH`.

## Build, upload, monitor

From the CLion toolbar, pick the `Production` target and hit build/run — or from
a terminal:

```bash
pio run
```

```bash
pio run --target upload
```

```bash
pio device monitor
```

The monitor runs at 115200 with the ESP32 exception decoder enabled, so a panic
prints a `file:line` backtrace rather than raw addresses.

If an upload fails to find the board, double-tap RESET to force the Nano ESP32
into DFU mode and retry, or pin the port in `platformio.ini` (`upload_port`).

---

## File map

| File | What lives there |
| --- | --- |
| `src/main.cpp` | `setup()`, `loop()`, and the three scheduled jobs: `captureSnapshot()`, `sendPendingReadings()`, `ensureDeviceRegistered()` |
| `include/config.h` | Every compile-time tunable: pins, intervals, buffer size, BSEC and BMV080 settings, with the reasoning for each |
| `include/secrets.h` | Product UID, auth token, device identity (gitignored) |
| `include/reading_types.h` | The `READING_*` wire codes — must stay in sync with `config/readingTypes.js` |
| `include/sensor_data.h` | The `SensorData` snapshot struct and `altitude()` |
| `busy_flags.{h,cpp}` | `i2cBusy` / `dataBusy` reentrancy guards |
| `i2c_bus.{h,cpp}` | Pin-state diagnosis, stuck-bus recovery, address scan |
| `time_sync.{h,cpp}` | Notecard `card.time` sync and the millis()-interpolated epoch |
| `notecard_device.{h,cpp}` | The shared `Notecard` instance |
| `notecard_manager.{h,cpp}` | Hub config, device registration, the batched compact-format uplink |
| `env_sensor.{h,cpp}` | BME688 + BSEC2: init, output callback, NVS calibration save/restore |
| `pm_sensor.{h,cpp}` | BMV080: start/duty-cycle config, drain loop, stall watchdog, heartbeat |
| `lightning_sensor.{h,cpp}` | AS3935 over SPI: init, IRQ polling, event snapshots |
| `readings_buffer.{h,cpp}` | The 40-entry snapshot ring buffer |

Headers go in `include/`, sources in `src/` — PlatformIO's convention, and it
keeps the public interface of each module obvious.

### Conventions

- **Headers carry Doxygen** (`/** */`), so CLion shows the contract on hover and
  in autocomplete. Sources use plain `//` for implementation notes.
- **Sensor modules share one shape**: `xSensorBegin()` to start, `xSensorService()`
  to pump, called from `setup()` and `loop()` respectively. Module names are
  functional (`pmSensor`), log prefixes name the chip (`[BMV080]`).
- **Anything not in a header is `static`** — file-private by default.
- **Comments explain why, not what.** Where a value looks arbitrary (the 400 kHz
  bus clock, `delay(1)`, `sync:false`, the derived PM timeouts) the constraint
  behind it is written down, because deleting it invites someone to "fix" the
  code back into a bug.

### Differences from the original single-file sketch

Behaviour is identical, verified by diffing every string literal against
`firmware/main.cpp`: all 107 log/output strings are byte-for-byte the same in
both, with no additions. RAM use is identical (56,708 bytes).

- Driver objects (`Bsec2`, `SparkFunBMV080`, `SparkFun_AS3935`, `Preferences`)
  are `static` in their owning `.cpp`.
- `bufferIndex` / `bufferCount` / `hasDataToSend` are private to
  `readings_buffer.cpp`; callers go through `bufferedSnapshotCount()`,
  `hasBufferedData()` and `drainSnapshots()`. The ring arithmetic is the same
  code that was inline in the old `sendData()`.
- The BME688 and AS3935 init blocks moved from `setup()` into
  `envSensorBegin()` and `lightningSensorBegin()`, called in the same order.
- Renamed for consistency or accuracy: `readSensors` → `captureSnapshot` (it
  samples caches, it does not read sensors), `startBmv080` → `pmSensorBegin`,
  `serviceBsec`/`serviceBmv080`/`pollLightningDetector` → `*SensorService`,
  `syncTimeBase_Internal` → `syncTimeBaseLocked` (names the precondition).
- `altitude()` is `inline`; it now appears in more than one translation unit.
- `#define` → `constexpr` wherever the value is not used by the preprocessor.
  `BMV080_DUTY_CYCLE_SECONDS` and `BMV080_POWER_PIN` stay macros because `#if`
  branches on them.
- `checkBsecStatus()` takes `const Bsec2&` instead of `Bsec2` by value, and
  `sendBatchedData()` takes `const SensorData*`. Dropping the per-call object
  copy is where most of the 16-byte flash reduction comes from.

---

## Libraries

`lib_deps` in `platformio.ini` points at the upstream git repos rather than
registry names, because that is unambiguous and because Bosch's BSEC2 is not
reliably mirrored in the PlatformIO registry:

- `Bosch-BME68x-Library` + `Bosch-BSEC2-Library`
- `SparkFun_Toolkit`
- `SparkFun_AS3935_Lightning_Detector_Arduino_Library`
- `note-arduino` (Blues Notecard)

They are unpinned so the first build succeeds against current upstream. Once
you have a build you trust, pin each one to a tag (`...git#v1.2.3`) — run
`pio pkg list` to see what resolved.

The BMV080 library is the exception: it is vendored into
`lib/SparkFun_BMV080_Arduino_Library/` rather than fetched, for the reason
below.

### The BMV080 needs Bosch's SDK

SparkFun's BMV080 library is only a wrapper. The actual driver is Bosch's
closed-source BMV080 SDK, which SparkFun is not licensed to redistribute, so
the public repo ships **README placeholders** where the SDK files belong. Build
against a plain `git clone` of it and you get:

```
sfTk/sfDevBMV080.h:25:2: error: #error "BOSCH BMV080 SDK Must be installed."
```

The SDK is four files:

| File | Where |
| --- | --- |
| `bmv080.h`, `bmv080_defs.h` | `src/sfTk/` |
| `lib_bmv080.a`, `lib_postProcessor.a` | `src/<arch>/` — `esp32s3` for the Nano ESP32 |

`library.properties` declares `precompiled=true` and
`ldflags=-l_bmv080 -l_postProcessor`, and PlatformIO resolves `<arch>` from the
board's `build.mcu`. So once the files are in place it links with no extra
build flags.

Vendoring the whole library into `lib/` (which takes priority over `lib_deps`)
means the SDK is not wiped whenever `.pio/libdeps` is reinstalled — the failure
mode you'd otherwise hit at random.

**The Bosch files are gitignored**, on the same licensing reasoning that keeps
them out of SparkFun's repo. That means a fresh clone will not build until they
are restored. Two ways:

- If you have the Arduino IDE library installed with the SDK already added:

  ```bash
  rsync -a --exclude='.DS_Store' ~/Documents/Arduino/libraries/SparkFun_BMV080_Arduino_Library/ firmware/clion/lib/SparkFun_BMV080_Arduino_Library/
  ```

- Otherwise download the SDK from Bosch and follow SparkFun's install
  instructions at <https://github.com/sparkfun/SparkFun_BMV080_Arduino_Library>.

If you check Bosch's license and conclude you may redistribute the SDK inside a
private repo, delete the four `lib/SparkFun_BMV080_Arduino_Library/...` lines
from `.gitignore` and commit them — that makes clones self-contained.

### If BSEC2 fails to link

BSEC2 is a precompiled Bosch blob (`libalgobsec.a`) selected per-architecture,
declared with `precompiled=full` / `ldflags=-lalgobsec` in its
`library.properties`. PlatformIO normally honours that. If you get undefined
references to `bsec_*`, point the linker at the ESP32-S3 archive by hand:

```ini
build_flags =
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DCORE_DEBUG_LEVEL=0
    -L${platformio.libdeps_dir}/${this.__env__}/Bosch-BSEC2-Library/src/esp32
    -lalgobsec
```

Check the actual folder name under `.pio/libdeps/arduino_nano_esp32/` first —
the per-MCU subdirectory may be `esp32` or `esp32s3` depending on the release.

---

## Keeping the two versions in step

The original `firmware/main.cpp` is untouched, so the Arduino IDE build still
works. That is fine short-term, but two copies of this firmware will drift.
Once you've flashed the CLion build and confirmed it behaves identically,
delete `firmware/main.cpp` and make this the only version.

---

## Extending: gas classification

The firmware produces BSEC2's standard air-quality metrics (IAQ,
CO2-equivalent, breath-VOC-equivalent, gas percentage). Those are composite
scores — they do not identify *which* gas is present.

Telling distinct gases apart needs BSEC2's separate scanning mode, which is a
workflow rather than a code change:

1. Record labelled scans in Bosch's BME AI-Studio and train a classifier. That
   exports a binary config blob; it is not derivable from datasheet code.
2. Load it with `envSensor.setConfig()` **before** `setState()` and
   `updateSubscription()` in `env_sensor.cpp`.
3. Subscribe to `BSEC_OUTPUT_GAS_ESTIMATE_1..4` — a 0–1 confidence per trained
   class — at `BSEC_SAMPLE_RATE_SCAN`, instead of or alongside the IAQ set.
4. That mode's sample rate and duty cycle differ from LP/ULP; follow the timing
   the exported config expects.
5. Reserve codes 22–25 in `config/readingTypes.js` before the firmware starts
   sending them.

(This lived as a comment block at the bottom of `main.cpp` in the Arduino
version. It is documentation, not code, so it lives here now.)
