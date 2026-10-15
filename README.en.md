# ZET 139 Communication Library

A cross-platform C library for acquiring signals from ZET 139 three-axis sensors over Modbus RTU via a USB virtual COM port.

## Features

- **Cross-platform support**: Compatible with Windows and Linux
- **Automatic sensor discovery**: Searches USB by VID, PID and serial number
- **Continuous data recording**: Three axes (X, Y, Z) are written in a background thread into 5-second ring buffers
- **Reading the last N samples**: Data of any axis in the channel units (g)
- **Data gaps**: Time is tracked by the sensor clock, missing samples are filled with NaN
- **Two sample rates**: Nominal (read on connect) and real (updated once per second)
- **Automatic reconnection**: On connection loss the sensor is searched and connected again

## Supported Hardware

ZET 139 three-axis vibration sensors

## Project Structure

```bash
zet139/
├── include/
│   └── zet139.h                # Public API header file
├── src/
│   └── zet139.c                # Library implementation
├── example/
│   ├── example_zet139.c        # Usage example
│   └── python/                 # Python wrapper and example
├── win32/
│   └── zet139.def              # DLL exports
├── CMakeLists.txt              # Build configuration
├── COPYING
├── README.md
└── README.en.md
```

## Requirements

- **CMake** (version 3.10 or higher)
- **C compiler** with C99 support
- **[libmodbus](https://github.com/stephane/libmodbus)**

## Building

### Using CMake (Recommended)

```bash
# Create build directory
mkdir build
cd build

# Configure the project
cmake ..

# Build the library and the example
cmake --build .
```

Options: `BUILD_SHARED`, `BUILD_STATIC`, `BUILD_EXAMPLES`.

Result: the shared library `zet139` (`zet139.dll` / `libzet139.so`), the static library `zet139_s` and three example builds: `example_zet139_static`, `example_zet139_shared` and `example_zet139_source` (built directly from the sources).

### Using vcpkg for Windows

Is is recommended to use [vcpkg](https://vcpkg.io/en/getting-started.html).

Install vcpkg and libmodbus (no admin privileges required):

```bash
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
bootstrap-vcpkg.bat
vcpkg install libmodbus
```

Then use CMake with toolchain file provided by vcpkg:

```bash
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[path to vcpkg]/scripts/buildsystems/vcpkg.cmake
make
```

## API Overview

### Main Functions

```c
// Connecting and disconnecting
zet139_connect(uint64_t serial, struct zet139_device** device_ptr);
zet139_disconnect(struct zet139_device** device_ptr);

// Sensor state
zet139_get_state(struct zet139_device* device, struct zet139_state* state);

// Getting data
zet139_channel_get_data(struct zet139_device* device, uint32_t channel, float* data, uint32_t size);
```

- `zet139_connect` finds the sensor (`serial` = 0 — the first one found), connects and immediately starts recording in the background.
- `zet139_channel_get_data` copies the last `size` samples of the axis `channel` (0 - X, 1 - Y, 2 - Z) to `data`. Values go from oldest to newest: `data[size - 1]` is the latest.

### Data Structures

```c
struct zet139_state {
    uint16_t connected;          // 1 — sensor is connected and data is recorded
    float frequency;             // Nominal sample rate, Hz
    float frequency_real;        // Real sample rate, Hz (0 — not read yet)
    uint64_t serial;             // Serial number of the found sensor
    char port[32];               // COM port ("COM3", "/dev/ttyACM0")
};
```

## Usage Example

```c
#include "zet139.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    struct zet139_device* device = NULL;

    // Connect to the first sensor found
    if (zet139_connect(0, &device) != 0) {
        fprintf(stderr, "Connection error\n");
        return -1;
    }

    // Sensor state
    struct zet139_state state;
    zet139_get_state(device, &state);

    // Buffer for one second of samples
    uint32_t size = (uint32_t)state.frequency;
    float* data = malloc(size * sizeof(float));

    // ... wait for data to accumulate (while zet139_channel_get_data returns -4) ...

    // Read the last second of the Z axis
    if (zet139_channel_get_data(device, 2, data, size) == 0) {
        printf("Got the last second of the Z axis\n");
    }

    // Clean up resources
    free(data);
    zet139_disconnect(&device);

    return 0;
}
```

A complete example is in `example/example_zet139.c`: once per second it prints the sensor sample rate and the mean value of each axis (NaN is not included in the mean). The serial number of the required sensor (hex) can be passed as the first argument.

## Error Handling

All functions return 0 on success and negative values on error.

`zet139_connect`: -1 - invalid arguments, -2 - out of memory, -3 - sensor not found, -4 - failed to open the port, -5 - sensor did not respond, -6 - failed to create the thread.

`zet139_channel_get_data`: -1 - invalid arguments, -2 - invalid channel, -3 - invalid size (0 or larger than the buffer), -4 - not enough data, -5 - communication lost (reconnecting).

## Communication Protocol

The library talks to the sensor over Modbus RTU via a USB virtual COM port.

### Communication Parameters

- **Speed**: 230400 baud
- **Format**: 8 data bits, odd parity, 1 stop bit
- **Slave address**: 2

### Getting Data

Data is recorded to the ring buffers (5 seconds) continuously from the moment of connection. `zet139_channel_get_data` copies the **last** `size` samples: e.g. with `size` = sample rate it is the last second.

- `size` larger than the buffer (5 seconds) — error -3;
- fewer than `size` samples recorded since connection — error -4;
- communication with the sensor is lost (reconnecting) — error -5.

Sensor polling:

- one Modbus request reads up to 125 registers from the sensor FIFO (the Modbus maximum) — for the int16 format that is a 16-byte header and up to 39 frames of 6 bytes each (X, Y, Z, 2 bytes per axis);
- reading the FIFO consumes the data: the sensor never sends the same samples twice;
- while there is data the next request follows immediately, with no pause; if the FIFO is empty the thread sleeps for 10 ms.

### Sample Rate

- `frequency` is the nominal rate: read from the channel registers on every connection. It sets the buffer size (5 seconds × rate) and is used to compute sample numbers.
- `frequency_real` is the real rate: once per second the background thread reads it from a separate sensor register (0xC504, a float). Until it has been read even once — for about a second after the first connection — the field holds 0. After a reconnection the last value read is kept, and if the sensor parameters changed it is 0 again.

Both rates are `float`. To get the "one second" block size, cast the nominal rate to an integer: `(uint32_t)state.frequency`.

### Data Gaps (NaN)

Time is tracked by the sensor's own clock — the computer's system time is not used:

- in each portion header the sensor reports `seconds` (whole seconds of its operation) and `counter` (sample number within the second); together they give the number of the first sample of the portion: `seconds × frequency + counter` (the integer part of the nominal rate is used);
- before a portion is written, its start is compared to the end of the previous one (start + number of frames):
  - equal — the data is continuous, the portion is simply appended;
  - later — some samples are missing: NaN is written in their place (if the gap is longer than the buffer, the whole buffer becomes NaN);
  - earlier — time went backwards (e.g. the sensor restarted): the whole buffer is cleared (NaN) and writing restarts from the beginning.

NaN values count as stored samples: `zet139_channel_get_data` may return a block partly filled with NaN. Means and other calculations over such a block must skip NaN (`x == x` is false only for NaN).

### Connection Loss

- the connection is considered lost on the first exchange error (or invalid response), or if there is no valid response for 10 seconds;
- the port is closed, `state.connected` becomes 0, `zet139_channel_get_data` returns -5;
- once per second the sensor with the same serial number is searched on USB, the port is opened, the parameters are read again and recording continues;
- if the sensor parameters (data format, nominal sample rate, LSB weight) did not change, the buffers with accumulated data are kept and the time without connection is filled with NaN by the sensor clock (if the sensor restarted meanwhile and its time went backwards, the buffers are cleared); the real rate `frequency_real` is not part of this comparison;
- if the parameters changed, the buffers are recreated (filled with NaN) and accumulation starts from scratch: until `size` samples are collected, `zet139_channel_get_data` returns -4.

## Platform Support

### Windows

- Visual Studio project files can be generated with CMake
- [vcpkg](https://vcpkg.io/en/getting-started.html) is recommended
- Sensor discovery: the registry `HKLM\SYSTEM\CurrentControlSet\Enum` is searched for `VID_2FFD&PID_xxxx` devices with a ZET 139 PID; the subkey name is the serial number, the COM port is taken from `Device Parameters\PortName`. The port must be present in `HKLM\HARDWARE\DEVICEMAP\SERIALCOMM` (device is currently connected)

### Linux

- Sensor discovery: `/sys/class/tty/ttyACM*` entries are scanned, the USB device `idVendor`, `idProduct` and `serial` are checked

## License

This project is licensed under the MIT License - see the COPYING file for details.
