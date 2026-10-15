# zet139.py — позволяет вызывать библиотеку zet139 (написана на C) из Python.
#
# Модуль ctypes умеет загружать DLL/SO-файл и вызывать функции из него.

import os
import sys
from ctypes import (
    c_char, c_int, c_uint16, c_uint32, c_uint64, c_float,
    Structure, POINTER, byref, CDLL
)

CHANNELS = 3  # число осей: 0 — X, 1 — Y, 2 — Z

# Коды результата get_channel_data (те же числа, что в zet139.h)
OK = 0
INVALID_SIZE = -3
NOT_ENOUGH_DATA = -4
DISCONNECTED = -5


# Раскладка полей должна совпадать со struct zet139_state из zet139.h
class zet139_state(Structure):
    _fields_ = [
        ("connected", c_uint16),
        ("frequency", c_float),
        ("frequency_real", c_float),
        ("serial", c_uint64),
        ("port", c_char * 32),
    ]


class zet139_device(Structure):
    pass


class zet139:
    """Работа с датчиком ZET 139 из Python"""

    def __init__(self, library_path=None):
        self._device = None

        lib_name = "zet139.dll" if sys.platform == "win32" else "libzet139.so"

        if library_path:
            self.lib_path = library_path
        else:
            for path in (os.path.join(os.getcwd(), lib_name),
                         os.path.join(os.getcwd(), "build", lib_name),
                         os.path.join(os.getcwd(), "lib", lib_name)):
                if os.path.exists(path):
                    self.lib_path = path
                    break
            else:
                raise FileNotFoundError(f"Could not find {lib_name} in common locations")

        self.lib = CDLL(self.lib_path)

        device = POINTER(zet139_device)

        self.lib.zet139_connect.argtypes = [c_uint64, POINTER(device)]
        self.lib.zet139_connect.restype = c_int
        self.lib.zet139_disconnect.argtypes = [POINTER(device)]
        self.lib.zet139_disconnect.restype = c_int
        self.lib.zet139_get_state.argtypes = [device, POINTER(zet139_state)]
        self.lib.zet139_get_state.restype = c_int
        self.lib.zet139_channel_get_data.argtypes = [device, c_uint32, POINTER(c_float), c_uint32]
        self.lib.zet139_channel_get_data.restype = c_int

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.disconnect()

    def connect(self, serial=0):
        """Найти датчик и подключиться (serial=0 — любой датчик). Возвращает 0 или код ошибки"""
        device_ptr = POINTER(zet139_device)()
        result = self.lib.zet139_connect(serial, byref(device_ptr))
        if result == 0:
            self._device = device_ptr
        return result

    def disconnect(self):
        """Отключиться от датчика"""
        if self._device:
            self.lib.zet139_disconnect(byref(self._device))
            self._device = None

    def get_state(self):
        """Сведения о датчике: словарь connected, frequency (номинальная, Гц),
        frequency_real (фактическая, Гц; 0 — ещё не прочитана), serial, port
        (или None при ошибке)"""
        state = zet139_state()
        if self.lib.zet139_get_state(self._device, byref(state)) != 0:
            return None
        return {
            'connected': bool(state.connected),
            'frequency': state.frequency,
            'frequency_real': state.frequency_real,
            'serial': state.serial,
            'port': state.port.decode('utf-8'),
        }

    def get_channel_data(self, channel, data, size):
        """Скопировать в data последние size значений оси channel.
        data — массив, созданный так: (c_float * size)(). Возвращает OK или код ошибки.
        На месте потерянных отсчётов в data стоит NaN"""
        result = self.lib.zet139_channel_get_data(self._device, channel, data, size)
        return result
