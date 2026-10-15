# zet139_main.py — пример на Python: подключается к ZET 139 и раз в секунду печатает
# частоту датчика и среднее значение последней секунды по каждой оси. Остановка — Ctrl+C.
# Запуск: python zet139_main.py [серийный номер в 16-ричном виде]

import signal
import sys
import time
from ctypes import c_float
from zet139 import zet139, CHANNELS, OK, NOT_ENOUGH_DATA, DISCONNECTED

running = True


def on_ctrl_c(sig, frame):
    global running
    running = False


def mean(data):
    """Среднее по отсчётам, которые реально пришли. На месте потерянных данных
    библиотека ставит NaN (он не равен самому себе) — такие отсчёты пропускаем.
    Возвращает среднее и процент потерянных отсчётов"""
    values = [v for v in data if v == v]
    lost = (len(data) - len(values)) * 100 // len(data)
    return (sum(values) / len(values) if values else 0.0), lost


def main():
    global running
    signal.signal(signal.SIGINT, on_ctrl_c)

    serial = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0

    sensor = zet139()

    # ----- Шаг 1. Подключение -----
    result = sensor.connect(serial)
    if result != OK:
        print(f"end: connect error {result}")
        return 1

    state = sensor.get_state()
    print(f"{state['port']}: device s/n {state['serial']:x} connected ({state['frequency']} Hz)")

    portion = int(state['frequency'])
    data = (c_float * portion)()

    # ----- Шаг 2. Чтение данных -----
    while running:
        time.sleep(1)

        state = sensor.get_state()
        print(f"{state['port']}: frequency: {state['frequency_real']} Hz")

        for channel in range(CHANNELS):
            result = sensor.get_channel_data(channel, data, portion)
            if result == OK:
                value, lost = mean(data)
                print(f"{state['port']}: channel {channel}: mean value {value:.6f} (lost {lost}% of samples)")
            elif result == NOT_ENOUGH_DATA:
                print(f"{state['port']}: channel {channel}: not enough data yet")
            elif result == DISCONNECTED:
                print(f"{state['port']}: connection lost, reconnecting...")
                break
            else:
                print(f"{state['port']}: channel {channel}: get data error {result}")
                running = False
                break

    # ----- Шаг 3. Отключение -----
    sensor.disconnect()
    print(f"{state['port']}: device disconnected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
