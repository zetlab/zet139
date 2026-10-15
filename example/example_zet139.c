// Что делает программа:
//   1) находит датчик ZET 139 на USB и подключается к нему;
//   2) каждую секунду печатает текущую частоту датчика, берёт последнюю
//      секунду измерений по осям X, Y, Z и печатает среднее значение каждой оси;
//   3) по нажатию Ctrl+C отключается от датчика и завершается.
//
// Запуск: example_zet139_static.exe [серийный номер в 16-ричном виде]
// Без номера берётся первый найденный датчик.

#include "zet139.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile sig_atomic_t running = 1;

static void on_ctrl_c(int signal_number)
{
	if (signal_number == SIGINT)
		running = 0;
}

static void sleep_1s(void)
{
#if defined(_WIN32)
	Sleep(1000);
#else
	struct timespec pause = { 1, 0 };
	nanosleep(&pause, NULL);
#endif
}

static float mean(const float* data, uint32_t size, uint32_t* lost)
{
	float sum = 0.f;
	uint32_t count = 0;

	for (uint32_t i = 0; i < size; ++i)
	{
		if (data[i] == data[i])
		{
			sum += data[i];
			count++;
		}
	}

	*lost = size - count;
	return count > 0 ? sum / (float)count : 0.f;
}

// Подключается к датчику, раз в секунду печатает среднее по каждой оси и по Ctrl+C отключается.
int main(int argc, char* argv[])
{
	uint64_t serial = argc > 1 ? strtoull(argv[1], NULL, 16) : 0;

	printf("start: example of working with ZET 139 device via Modbus\n");
	signal(SIGINT, on_ctrl_c);

	// ----- Шаг 1. Подключение -----
	struct zet139_device* device = NULL;
	int result = zet139_connect(serial, &device);
	if (result != 0)
	{
		fprintf(stderr, "end: connect error %d\n", result);
		return 1;
	}

	struct zet139_state state;
	zet139_get_state(device, &state);
	printf("%s: device s/n %llx connected (%f Hz)\n",
		state.port, (unsigned long long)state.serial, state.frequency);

	uint32_t portion = (uint32_t)state.frequency;
	float* data = malloc(portion * sizeof(float));

	// ----- Шаг 2. Чтение данных -----
	while (running && data != NULL)
	{
		sleep_1s();

		zet139_get_state(device, &state);
		printf("%s: frequency: %f Hz\n", state.port, state.frequency_real);

		for (uint32_t channel = 0; channel < ZET139_CHANNELS; ++channel)
		{
			result = zet139_channel_get_data(device, channel, data, portion);

			if (result == 0)
			{
				uint32_t lost = 0;
				float value = mean(data, portion, &lost);
				printf("%s: channel %u: mean value: %f (lost %u%% of samples)\n",
					state.port, channel, value, lost * 100 / portion);
			}
			else if (result == -4)
			{
				printf("%s: channel %u: not enough data yet\n", state.port, channel);
			}
			else if (result == -5)
			{
				printf("%s: connection lost, reconnecting...\n", state.port);
				break;
			}
			else
			{
				fprintf(stderr, "%s: channel %u: get data error %d\n", state.port, channel, result);
				running = 0;
				break;
			}
		}
	}

	free(data);

	// ----- Шаг 3. Отключение -----
	zet139_disconnect(&device);
	printf("%s: device disconnected\n", state.port);

	printf("end: example of working with ZET 139 device via Modbus\n");
	return 0;
}
