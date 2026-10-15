# ZET 139 Python Interface Documentation

## Overview

This documentation covers two Python files that provide an interface for working with ZET 139 three-axis sensors over Modbus RTU via USB:

- ```zet139.py``` - Main wrapper for the ZET 139 library
- ```zet139_main.py``` - Example application demonstrating library usage

## File: zet139.py

### Purpose

This file provides a Python wrapper for the ZET 139 library, allowing Python applications to get data from ZET 139 three-axis sensors.

### Main Components

#### Data Structures

- ```zet139_state```: Sensor state structure (the ```get_state()``` method returns it as a dictionary)
  - ```connected```: Connection status
  - ```frequency```: Nominal sample rate, Hz (float)
  - ```frequency_real```: Real sample rate, Hz (float, 0 — not read yet)
  - ```serial```: Serial number
  - ```port```: Sensor COM port

#### Main Class: zet139

#### Initialization

```python
def __init__(self, library_path=None)
```

Automatically detects the appropriate library file (.dll on Windows, .so on Linux) and loads it. If no path is given, the file is searched in the current folder, then in ```build/``` and ```lib/``` (relative to the current folder, not the script folder).

#### Main Methods

- ```connect(serial=0)```: Find the sensor on USB and connect (0 — the first one found); returns the result code
- ```disconnect()```: Stop recording and disconnect
- ```get_state()```: Get the sensor state (dictionary ```connected```, ```frequency```, ```frequency_real```, ```serial```, ```port```)
- ```get_channel_data(channel, data, size)```: Copy the last ```size``` samples of the axis (0 - X, 1 - Y, 2 - Z) to ```data```

The class can be used as a context manager: ```with zet139() as sensor:``` — ```disconnect()``` is called automatically when the block ends (you still call ```connect()``` yourself).

#### get_channel_data Result Codes

- ```OK``` (0) — data copied
- ```INVALID_SIZE``` (-3) — size is 0 or larger than the buffer (5 seconds)
- ```NOT_ENOUGH_DATA``` (-4) — fewer than size samples recorded yet
- ```DISCONNECTED``` (-5) — communication with the sensor is lost, reconnecting

Values go from oldest to newest. NaN stands in place of missing samples — skip it in calculations.

## File: zet139_main.py

### Purpose

This file provides a complete example of using the zet139 library to get data from a ZET 139 sensor.

### Main Features

1. **Signal handling**: Correct handling of SIGINT (Ctrl+C) for graceful shutdown
2. **Connection**: Automatic sensor discovery on USB, the serial number in hex can be passed as an argument
3. **Data acquisition**: Once per second, reading the last second of data of each axis
4. **Data processing**: Computing the mean value skipping NaN and printing the real sample rate of the sensor

### Main Workflow

1. Connect to the sensor
2. Get the state and the nominal sample rate
3. Once per second: print the real sample rate and read data of the X, Y, Z axes
4. Handle the result codes (not enough data, connection lost, error)
5. Disconnect on exit by Ctrl+C

## Usage Example

```python
from ctypes import c_float
from zet139 import zet139, OK

# Initialize the library
sensor = zet139()

# Connect to the first sensor found
if sensor.connect() != OK:
    print("Failed to connect to the sensor")
    exit(-1)

# Sensor state and a buffer for one second of samples
state = sensor.get_state()
size = int(state['frequency'])          # the rate is a float, the array size must be an integer
data = (c_float * size)()

# Read the last second of the Z axis
result = sensor.get_channel_data(2, data, size)
if result == OK:
    values = [v for v in data if v == v]    # skip NaN
    print("Got the last second of the Z axis, mean:", sum(values) / len(values))

# Clean up resources
sensor.disconnect()
```

## Error Handling

The library reports errors through return values:

- ```connect()``` returns 0 (```OK```) or a negative error code (-3 — sensor not found, -4 — failed to open the port and others, see the library README)
- ```get_channel_data()``` returns ```OK``` or one of the codes listed above
- ```get_state()``` returns ```None``` on error
- ```FileNotFoundError``` is raised if the library file is not found

## Supported Platforms

- **Windows**: Uses zet139.dll
- **Linux**: Uses libzet139.so

The library automatically detects the platform and tries to find the corresponding library file in the standard locations. Run the scripts from the ```example/python``` folder with the built library placed next to them: copy ```zet139.dll``` (Windows) or ```libzet139.so``` (Linux) from the build folder (```build```).

```bash
python zet139_main.py [serial number in hex]
```

## Dependencies

- Python 3.x
- ctypes library (standard library)
- ZET 139 library (zet139.dll or libzet139.so)
