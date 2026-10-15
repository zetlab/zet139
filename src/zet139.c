// zet139.c — библиотека для трёхосевых датчиков вибрации ZET 139.
//
// Подключается к датчику через USB (виден в системе как COM-порт),
// постоянно читает измерения по осям X, Y, Z и хранит последние 5 секунд
// в кольцевых буферах. Пользователь в любой момент забирает последние N
// значений через zet139_channel_get_data.
//
// Путь данных: датчик -> read_portion -> write_portion (NaN на месте
// пропусков) -> store_portion / store_frames -> буферы -> zet139_channel_get_data.
//
// Фоновый поток (polling_loop) читает датчик и пишет в буферы; поток
// пользователя читает буферы через zet139_channel_get_data.

#if !defined(_WIN32)
#define _DEFAULT_SOURCE
#endif

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HANDLE thread_t;
typedef CRITICAL_SECTION mutex_t;
#else
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
typedef pthread_t thread_t;
typedef pthread_mutex_t mutex_t;
#endif

#include <modbus.h>

#include "zet139.h"

// Параметры связи с датчиком
#define USB_VENDOR_ID 0x2FFD
#define PORT_NAME_MAX_LENGTH 32
#define BAUDRATE 230400
#define PARITY 'O'
#define DATA_BITS 8
#define STOP_BITS 1
#define SLAVE_ADDRESS 2

// Адреса регистров датчика
#define FRAME_FORMAT_REGISTER (0xC000 + 128 * 40)
#define DATA_FIFO_REGISTER (0xC004 + 128 * 40)
#define CHANNEL_REGISTER(channel) (0xC004 + 128 * (40 + (channel)))

// Формат данных, которые присылает датчик
#define FRAME_FORMAT_REGISTERS 4
#define FRAME_FORMAT_SHORT_ID 0xD5

#define CHANNEL_REGISTERS 34
#define CHANNEL_FREQUENCY_OFFSET 4
#define CHANNEL_RESOLUTION_OFFSET 64

#define FREQ_REGISTERS 50436
#define FREQ_REGISTER_COUNT 2

#define PORTION_HEADER_BYTES 16
#define PORTION_TIME_SECONDS_OFFSET 0
#define PORTION_TIME_COUNTER_OFFSET 4
#define PORTION_MAX_REGISTERS MODBUS_MAX_READ_REGISTERS
#define PORTION_MAX_BYTES (PORTION_MAX_REGISTERS * 2)

// Размер буферов и паузы опроса
#define BUFFER_SECONDS 5
#define BUFFER_MIN_SAMPLES 1024
#define BUFFER_MAX_SAMPLES (1 << 22)

#define NO_RESPONSE_TIMEOUT_MS 10000
#define RECONNECT_PERIOD_MS 1000
#define FREQUENCY_REFRESH_PERIOD_MS 1000
#define IDLE_SLEEP_MS 10
#define WAIT_SLEEP_MS 100

// Параметры, которые датчик сообщает о себе при подключении
struct zet139_parameters
{
	int short_samples;
	float frequency;
	float frequency_real;
	float resolution[ZET139_CHANNELS];
};

// Всё, что библиотека знает об одном подключённом датчике.
struct zet139_device
{
	modbus_t* modbus;

	thread_t thread;
	int thread_started;
	volatile int running;

	uint64_t last_response_time;
	int has_data;
	int64_t data_end;

	mutex_t mutex;
	int connected;
	uint64_t serial;
	char port[PORT_NAME_MAX_LENGTH];
	struct zet139_parameters parameters;
	float* buffer[ZET139_CHANNELS];
	uint32_t buffer_size;
	uint32_t write_position;
	uint32_t stored_count;
};

static void mutex_init(mutex_t* mutex)
{
#if defined(_WIN32)
	InitializeCriticalSection(mutex);
#else
	pthread_mutex_init(mutex, NULL);
#endif
}

static void mutex_destroy(mutex_t* mutex)
{
#if defined(_WIN32)
	DeleteCriticalSection(mutex);
#else
	pthread_mutex_destroy(mutex);
#endif
}

static void mutex_lock(mutex_t* mutex)
{
#if defined(_WIN32)
	EnterCriticalSection(mutex);
#else
	pthread_mutex_lock(mutex);
#endif
}

static void mutex_unlock(mutex_t* mutex)
{
#if defined(_WIN32)
	LeaveCriticalSection(mutex);
#else
	pthread_mutex_unlock(mutex);
#endif
}

static uint64_t time_ms(void)
{
#if defined(_WIN32)
	return GetTickCount64();
#else
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
#endif
}

static void sleep_ms(uint32_t ms)
{
#if defined(_WIN32)
	Sleep(ms);
#else
	struct timespec pause = { ms / 1000, (long)(ms % 1000) * 1000000 };
	nanosleep(&pause, NULL);
#endif
}

static uint16_t read_u16(const uint8_t* bytes)
{
	return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static uint32_t read_u32(const uint8_t* bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static float read_float(const uint8_t* bytes)
{
	uint32_t bits = read_u32(bytes);
	float value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static int is_zet139_product_id(uint32_t product_id)
{
	switch (product_id)
	{
	case 0x0008: // ZET 139
	case 0x000A: // ZET 139A
	case 0x000D: // ZET 139B
	case 0x0017: // ZET 139B
	case 0x0012: // ZET 139C
	case 0x000C: // ZET 139D
	case 0x000B: // ZET 139F
		return 1;
	default:
		return 0;
	}
}

#if defined(_WIN32)

static int is_port_present(const char* port)
{
	HKEY key;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
		return 0;

	int present = 0;
	for (DWORD index = 0; !present; ++index)
	{
		char name[256];
		DWORD name_size = sizeof(name);
		char value[PORT_NAME_MAX_LENGTH];
		DWORD value_size = sizeof(value) - 1;
		DWORD type = 0;

		LONG result = RegEnumValueA(key, index, name, &name_size, NULL, &type, (LPBYTE)value, &value_size);
		if (result == ERROR_NO_MORE_ITEMS)
			break;
		if (result != ERROR_SUCCESS || type != REG_SZ)
			continue;

		value[value_size] = '\0';
		present = (_stricmp(value, port) == 0);
	}

	RegCloseKey(key);
	return present;
}

static int find_in_product_key(HKEY product_key, uint64_t wanted_serial, char* port, uint64_t* serial)
{
	for (DWORD index = 0; ; ++index)
	{
		char serial_text[256];
		DWORD serial_text_size = sizeof(serial_text);
		if (RegEnumKeyExA(product_key, index, serial_text, &serial_text_size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
			return 0;

		char* end = NULL;
		uint64_t found_serial = _strtoui64(serial_text, &end, 16);
		if (end == serial_text || *end != '\0')
			continue;
		if (wanted_serial != 0 && found_serial != wanted_serial)
			continue;

		char parameters_path[300];
		snprintf(parameters_path, sizeof(parameters_path), "%s\\Device Parameters", serial_text);

		char found_port[PORT_NAME_MAX_LENGTH];
		DWORD found_port_size = sizeof(found_port);
		if (RegGetValueA(product_key, parameters_path, "PortName", RRF_RT_REG_SZ, NULL, found_port, &found_port_size) != ERROR_SUCCESS)
			continue;

		if (!is_port_present(found_port))
			continue;

		strncpy(port, found_port, PORT_NAME_MAX_LENGTH - 1);
		port[PORT_NAME_MAX_LENGTH - 1] = '\0';
		*serial = found_serial;
		return 1;
	}
}

// Поиск подключённого датчика ZET 139 на USB по VID/PID и серийному номеру.
// wanted_serial = 0 — подойдёт любой. Возвращает 0 и заполняет port и serial, если нашли.
static int find_device(uint64_t wanted_serial, char* port, uint64_t* serial)
{
	const char prefix[] = "VID_2FFD&PID_";
	const size_t prefix_length = sizeof(prefix) - 1;

	HKEY enum_key;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum", 0, KEY_READ, &enum_key) != ERROR_SUCCESS)
		return -1;

	int found = 0;
	for (DWORD bus_index = 0; !found; ++bus_index)
	{
		char bus_name[256];
		DWORD bus_name_size = sizeof(bus_name);
		if (RegEnumKeyExA(enum_key, bus_index, bus_name, &bus_name_size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
			break;

		HKEY bus_key;
		if (RegOpenKeyExA(enum_key, bus_name, 0, KEY_READ, &bus_key) != ERROR_SUCCESS)
			continue;

		for (DWORD product_index = 0; !found; ++product_index)
		{
			char product_name[256];
			DWORD product_name_size = sizeof(product_name);
			if (RegEnumKeyExA(bus_key, product_index, product_name, &product_name_size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
				break;

			if (_strnicmp(product_name, prefix, prefix_length) != 0)
				continue;

			char product_id_text[5] = { 0 };
			memcpy(product_id_text, product_name + prefix_length, 4);
			char* end = NULL;
			uint32_t product_id = (uint32_t)strtoul(product_id_text, &end, 16);
			if (end != product_id_text + 4 || !is_zet139_product_id(product_id))
				continue;

			HKEY product_key;
			if (RegOpenKeyExA(bus_key, product_name, 0, KEY_READ, &product_key) != ERROR_SUCCESS)
				continue;

			found = find_in_product_key(product_key, wanted_serial, port, serial);
			RegCloseKey(product_key);
		}

		RegCloseKey(bus_key);
	}

	RegCloseKey(enum_key);
	return found ? 0 : -1;
}

#else

static int read_sysfs_hex(const char* directory, const char* file_name, uint64_t* value)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", directory, file_name);

	FILE* file = fopen(path, "r");
	if (file == NULL)
		return -1;

	char text[64] = { 0 };
	int ok = fgets(text, sizeof(text), file) != NULL;
	fclose(file);
	if (!ok)
		return -1;

	char* end = NULL;
	*value = strtoull(text, &end, 16);
	return end == text ? -1 : 0;
}

static int find_device(uint64_t wanted_serial, char* port, uint64_t* serial)
{
	DIR* directory = opendir("/sys/class/tty");
	if (directory == NULL)
		return -1;

	int found = 0;
	struct dirent* entry;
	while (!found && (entry = readdir(directory)) != NULL)
	{
		if (strncmp(entry->d_name, "ttyACM", 6) != 0)
			continue;

		char link[PATH_MAX];
		snprintf(link, sizeof(link), "/sys/class/tty/%s/device", entry->d_name);

		char usb_device[PATH_MAX];
		if (realpath(link, usb_device) == NULL)
			continue;

		char* last_slash = strrchr(usb_device, '/');
		if (last_slash == NULL)
			continue;
		*last_slash = '\0';

		uint64_t vendor_id = 0, product_id = 0, found_serial = 0;
		if (read_sysfs_hex(usb_device, "idVendor", &vendor_id) != 0 || vendor_id != USB_VENDOR_ID)
			continue;
		if (read_sysfs_hex(usb_device, "idProduct", &product_id) != 0 || !is_zet139_product_id((uint32_t)product_id))
			continue;
		if (read_sysfs_hex(usb_device, "serial", &found_serial) != 0)
			continue;
		if (wanted_serial != 0 && found_serial != wanted_serial)
			continue;

		snprintf(port, PORT_NAME_MAX_LENGTH, "/dev/%s", entry->d_name);
		*serial = found_serial;
		found = 1;
	}

	closedir(directory);
	return found ? 0 : -1;
}

#endif

static int read_registers_as_bytes(modbus_t* modbus, int address, int nregs, uint8_t* bytes)
{
	uint16_t registers[CHANNEL_REGISTERS];

	if (nregs <= 0 || nregs > CHANNEL_REGISTERS)
		return -1;

	if (modbus_read_registers(modbus, address, nregs, registers) != nregs)
		return -1;

	for (int i = 0; i < nregs; ++i)
	{
		bytes[i * 2] = (uint8_t)(registers[i] & 0xFF);
		bytes[i * 2 + 1] = (uint8_t)(registers[i] >> 8);
	}

	return 0;
}

// Аналог modbus_read_input_registers, но учитывает, что в ответе может быть меньше данных, чем запрошено
static int read_input_registers_as_fifo(modbus_t* modbus, int addr, int nregs, uint16_t* data)
{
	uint8_t buf[MODBUS_MAX_ADU_LENGTH];
	int rc;
	int i;
	uint8_t *dst;
	int offset;
	int function;
	int len;

	buf[0] = modbus_get_slave(modbus);
	buf[1] = MODBUS_FC_READ_INPUT_REGISTERS;
	buf[2] = addr >> 8;
	buf[3] = addr & 0x00ff;
	buf[4] = nregs >> 8;
	buf[5] = nregs & 0x00ff;

	rc = modbus_send_raw_request(modbus, buf, 6);
	if (rc > 0) {
		rc = modbus_receive_confirmation(modbus, buf);
		if (rc == -1)
			return -1;

		offset = modbus_get_header_length(modbus);

		function = buf[offset];

		if (function >= 0x80) {
			int exception_code = buf[offset + 1];
			if (exception_code < MODBUS_EXCEPTION_MAX) {
				errno = MODBUS_ENOBASE + exception_code;
			} else {
				errno = EMBBADEXC;
			}
			return -1;
		}

		if (buf[offset] != MODBUS_FC_READ_INPUT_REGISTERS) {
			errno = EMBBADDATA;
			return -1;
		}

		len = buf[offset + 1];
		if (len > MODBUS_MAX_READ_REGISTERS * 2 || len > nregs * 2) {
			errno = EMBBADDATA;
			return -1;
		}
		if (rc < offset + 1 + len) {
			errno = EMBBADDATA;
			return -1;
		}

		dst = (uint8_t *)data;
		for (i = 0; i < len; i += 2) {
			/* shift reg hi_byte to temp OR with lo_byte */
			dst[i + 0] = buf[offset + 3 + i];
			dst[i + 1] = buf[offset + 2 + i];
		}

		rc = (len / 2);
	}

	return rc;
}

static int open_port(struct zet139_device* device, const char* port)
{
	char path[PORT_NAME_MAX_LENGTH + 4];

#if defined(_WIN32)
	snprintf(path, sizeof(path), "\\\\.\\%s", port);
#else
	snprintf(path, sizeof(path), "%s", port);
#endif

	device->modbus = modbus_new_rtu(path, BAUDRATE, PARITY, DATA_BITS, STOP_BITS);
	if (device->modbus == NULL)
		return -1;

	if (modbus_connect(device->modbus) != 0)
	{
		modbus_free(device->modbus);
		device->modbus = NULL;
		return -1;
	}

	if (modbus_set_slave(device->modbus, SLAVE_ADDRESS) != 0)
	{
		modbus_close(device->modbus);
		modbus_free(device->modbus);
		device->modbus = NULL;
		return -1;
	}

	return 0;
}

static void close_port(struct zet139_device* device)
{
	if (device->modbus != NULL)
	{
		modbus_close(device->modbus);
		modbus_free(device->modbus);
		device->modbus = NULL;
	}
}

static int read_device_parameters(modbus_t* modbus, struct zet139_parameters* parameters)
{
	uint8_t format[FRAME_FORMAT_REGISTERS * 2];
	uint8_t channel_info[CHANNEL_REGISTERS * 2];

	memset(parameters, 0, sizeof(*parameters));

	if (read_registers_as_bytes(modbus, FRAME_FORMAT_REGISTER, FRAME_FORMAT_REGISTERS, format) != 0)
		return -1;

	uint32_t format_id = (read_u32(format) >> 12) & 0x3FF;
	parameters->short_samples = (format_id == FRAME_FORMAT_SHORT_ID);

	for (uint32_t channel = 0; channel < ZET139_CHANNELS; ++channel)
	{
		if (read_registers_as_bytes(modbus, CHANNEL_REGISTER(channel), CHANNEL_REGISTERS, channel_info) != 0)
			return -1;

		parameters->frequency = read_float(channel_info + CHANNEL_FREQUENCY_OFFSET);
		parameters->resolution[channel] = read_float(channel_info + CHANNEL_RESOLUTION_OFFSET);

		if (parameters->frequency == 0.0f)
			return -1;
	}

	return 0;
}

static int read_frequency(modbus_t* modbus, float* frequency)
{
	uint8_t bytes[FREQ_REGISTER_COUNT * 2];

	if (read_registers_as_bytes(modbus, FREQ_REGISTERS, FREQ_REGISTER_COUNT, bytes) != 0)
		return -1;

	float value = read_float(bytes);
	if (!isfinite(value) || value <= 0.0f)
		return -1;

	*frequency = value;
	return 0;
}

// Совпадают ли параметры, от которых зависят буферы. Фактическая частота
// (frequency_real) не сравнивается: фоновый поток обновляет её раз в секунду,
// а сразу после подключения она ещё не прочитана.
static int same_buffer_parameters(const struct zet139_parameters* a, const struct zet139_parameters* b)
{
	return a->short_samples == b->short_samples
		&& a->frequency == b->frequency
		&& memcmp(a->resolution, b->resolution, sizeof(a->resolution)) == 0;
}

// Подготовка буферов на 5 секунд. Если параметры датчика не изменились
// с прошлого раза, старые буферы с накопленными данными сохраняются —
// иначе выделяются новые буферы, заполненные NaN.
static int apply_parameters(struct zet139_device* device, const struct zet139_parameters* parameters)
{
	if (device->buffer[0] != NULL && same_buffer_parameters(&device->parameters, parameters))
		return 0;

	uint64_t size = (uint64_t)parameters->frequency * BUFFER_SECONDS;
	if (size < BUFFER_MIN_SAMPLES)
		size = BUFFER_MIN_SAMPLES;
	if (size > BUFFER_MAX_SAMPLES)
		size = BUFFER_MAX_SAMPLES;

	float* new_buffer[ZET139_CHANNELS] = { NULL };
	for (uint32_t channel = 0; channel < ZET139_CHANNELS; ++channel)
	{
		new_buffer[channel] = malloc((size_t)size * sizeof(float));
		if (new_buffer[channel] == NULL)
		{
			for (uint32_t i = 0; i < channel; ++i)
				free(new_buffer[i]);
			return -1;
		}
		for (uint64_t i = 0; i < size; ++i)
			new_buffer[channel][i] = NAN;
	}

	mutex_lock(&device->mutex);

	float* old_buffer[ZET139_CHANNELS];
	for (uint32_t channel = 0; channel < ZET139_CHANNELS; ++channel)
	{
		old_buffer[channel] = device->buffer[channel];
		device->buffer[channel] = new_buffer[channel];
	}
	device->parameters = *parameters;
	device->buffer_size = (uint32_t)size;
	device->write_position = 0;
	device->stored_count = 0;
	device->has_data = 0;

	mutex_unlock(&device->mutex);

	for (uint32_t channel = 0; channel < ZET139_CHANNELS; ++channel)
		free(old_buffer[channel]);
	return 0;
}

// Полное подключение к датчику: поиск его на USB, открыть порт, прочитать
// параметры, подготовить буферы. Вызывается при старте и при каждом переподключении.
// Возвращает: 0 — успех; -2 — не хватило памяти; -3 — датчик не найден;
// -4 — порт не открылся; -5 — датчик не ответил.
static int connect_to_device(struct zet139_device* device)
{
	char port[PORT_NAME_MAX_LENGTH];
	uint64_t serial = 0;

	if (find_device(device->serial, port, &serial) != 0)
		return -3;

	if (open_port(device, port) != 0)
	{
		close_port(device);
		return -4;
	}

	struct zet139_parameters parameters;
	if (read_device_parameters(device->modbus, &parameters) != 0)
	{
		close_port(device);
		return -5;
	}

	if (apply_parameters(device, &parameters) != 0)
	{
		close_port(device);
		return -2;
	}

	device->last_response_time = time_ms();

	mutex_lock(&device->mutex);
	device->serial = serial;
	snprintf(device->port, PORT_NAME_MAX_LENGTH, port);
	device->port[PORT_NAME_MAX_LENGTH - 1] = '\0';
	device->connected = 1;
	mutex_unlock(&device->mutex);

	return 0;
}

static void lose_connection(struct zet139_device* device)
{
	close_port(device);

	mutex_lock(&device->mutex);
	device->connected = 0;
	mutex_unlock(&device->mutex);
}

static void fill_nan(struct zet139_device* device, uint32_t position, uint32_t count)
{
	for (uint32_t channel = 0; channel < ZET139_CHANNELS; ++channel)
	{
		float* samples = device->buffer[channel] + position;
		for (uint32_t i = 0; i < count; ++i)
			samples[i] = NAN;
	}
}

static void write_nan(struct zet139_device* device, uint32_t count)
{
	uint32_t position = device->write_position;
	uint32_t space_until_end = device->buffer_size - position;

	if (count <= space_until_end)
	{
		fill_nan(device, position, count);
		position += count;
	}
	else
	{
		fill_nan(device, position, space_until_end);
		fill_nan(device, 0, count - space_until_end);
		position = count - space_until_end;
	}

	if (position == device->buffer_size)
		position = 0;

	device->write_position = position;
}

// Разбор frame_count кадров (X, Y, Z по каждому) из samples и запись
// в буферы осей начиная с ячейки position. Кадры должны помещаться до конца
// буфера без перехода в начало. Возвращает указатель на первый байт после разобранных кадров.
static const uint8_t* store_frames(struct zet139_device* device, const uint8_t* samples, uint32_t position, uint32_t frame_count)
{
	float* x = device->buffer[0] + position;
	float* y = device->buffer[1] + position;
	float* z = device->buffer[2] + position;

	if (device->parameters.short_samples)
	{
		float x_resolution = device->parameters.resolution[0];
		float y_resolution = device->parameters.resolution[1];
		float z_resolution = device->parameters.resolution[2];

		for (uint32_t i = 0; i < frame_count; ++i)
		{
			x[i] = (int16_t)read_u16(samples) * x_resolution;
			y[i] = (int16_t)read_u16(samples + 2) * y_resolution;
			z[i] = (int16_t)read_u16(samples + 4) * z_resolution;
			samples += 6;
		}
	}
	else
	{
		for (uint32_t i = 0; i < frame_count; ++i)
		{
			x[i] = read_float(samples);
			y[i] = read_float(samples + 4);
			z[i] = read_float(samples + 8);
			samples += 12;
		}
	}

	return samples;
}

static void store_portion(struct zet139_device* device, const uint8_t* samples, uint32_t frame_count)
{
	uint32_t position = device->write_position;
	uint32_t space_until_end = device->buffer_size - position;

	if (frame_count <= space_until_end)
	{
		store_frames(device, samples, position, frame_count);
		position += frame_count;
	}
	else
	{
		samples = store_frames(device, samples, position, space_until_end);
		uint32_t rest = frame_count - space_until_end;
		store_frames(device, samples, 0, rest);
		position = rest;
	}

	if (position == device->buffer_size)
		position = 0;

	device->write_position = position;
}

// Запись порции в буферы, а на месте пропущенных вставка NaN. 
// Время измеряется датчиком, в номерах отсчётов (portion_end — номер отсчёта, на
// котором закончилась порция). Начало порции сравнивается с концом предыдущей:
//   совпало       — данные без разрыва, просто дописываем;
//   позже         — был пропуск: сначала пишем NaN на пропавшие отсчёты
//                   (пропуск длиннее буфера — весь буфер становится NaN);
//   раньше        — время пошло назад: буфер очищается (NaN), запись с начала.
static void write_portion(struct zet139_device* device, const uint8_t* samples, uint32_t frame_count, int64_t portion_end)
{
	int64_t portion_start = portion_end - frame_count;
	uint32_t buffer_size = device->buffer_size;

	mutex_lock(&device->mutex);

	uint64_t stored = device->stored_count;

	if (device->has_data)
	{
		int64_t gap = portion_start - device->data_end;

		if (gap < 0)
		{
			fill_nan(device, 0, buffer_size);
			device->write_position = 0;
			stored = 0;
		}
		else if (gap > (int64_t)buffer_size)
		{
			fill_nan(device, 0, buffer_size);
			device->write_position = 0;
			stored += (uint64_t)gap;
		}
		else if (gap > 0)
		{
			write_nan(device, (uint32_t)gap);
			stored += (uint64_t)gap;
		}
	}

	store_portion(device, samples, frame_count);
	stored += frame_count;

	if (stored > buffer_size)
		stored = buffer_size;
	device->stored_count = (uint32_t)stored;

	device->data_end = portion_end;
	device->has_data = 1;

	mutex_unlock(&device->mutex);
}

// Чтение у датчика одной порции из FIFO и запись её в буферы.
// Возвращает число прочитанных кадров (0 — у датчика пока нет новых данных)
// или -1 при ошибке обмена.
static int read_portion(struct zet139_device* device)
{
	uint16_t response[PORTION_MAX_REGISTERS];

	int registers = read_input_registers_as_fifo(device->modbus, DATA_FIFO_REGISTER, PORTION_MAX_REGISTERS, response);
	if (registers < 0)
		return -1;

	const uint8_t* bytes = (const uint8_t*)response;
	uint32_t bytes_count = (uint32_t)registers * 2;

	if (bytes_count < PORTION_HEADER_BYTES)
		return -1;

	uint32_t sample_bytes = device->parameters.short_samples ? 2 : 4;
	uint32_t frame_bytes = sample_bytes * ZET139_CHANNELS;
	uint32_t data_bytes = bytes_count - PORTION_HEADER_BYTES;

	if (data_bytes % frame_bytes != 0)
		return -1;

	device->last_response_time = time_ms();

	uint32_t frame_count = data_bytes / frame_bytes;
	uint32_t frequency = (uint32_t)device->parameters.frequency;

	if (frame_count == 0)
		return 0;

	uint32_t seconds = read_u32(bytes + PORTION_TIME_SECONDS_OFFSET);
	uint32_t counter = read_u32(bytes + PORTION_TIME_COUNTER_OFFSET);
	int64_t portion_end = (int64_t)((uint64_t)seconds * frequency + counter + frame_count);

	write_portion(device, bytes + PORTION_HEADER_BYTES, frame_count, portion_end);

	return (int)frame_count;
}

// Главный цикл фонового потока: пока есть связь — раз в секунду перечитывает
// частоту датчика и читает порции из FIFO; при ошибке обмена или 10 с без
// ответа закрывает порт и раз в секунду пытается найти датчик и подключиться
// заново.
static void polling_loop(struct zet139_device* device)
{
	uint64_t last_attempt_time = time_ms();
	uint64_t last_frequency_refresh = time_ms();

	while (device->running)
	{
		if (!device->connected)
		{
			if (time_ms() - last_attempt_time < RECONNECT_PERIOD_MS)
			{
				sleep_ms(WAIT_SLEEP_MS);
				continue;
			}

			last_attempt_time = time_ms();
			connect_to_device(device);
			last_frequency_refresh = time_ms();
			continue;
		}

		if (time_ms() - last_frequency_refresh >= FREQUENCY_REFRESH_PERIOD_MS)
		{
			last_frequency_refresh = time_ms();

			float frequency;
			if (read_frequency(device->modbus, &frequency) == 0)
			{
				mutex_lock(&device->mutex);
				device->parameters.frequency_real = frequency;
				mutex_unlock(&device->mutex);
			}
		}

		int frames = read_portion(device);

		if (frames < 0 || time_ms() - device->last_response_time >= NO_RESPONSE_TIMEOUT_MS)
		{
			lose_connection(device);
			last_attempt_time = time_ms();
			continue;
		}

		if (frames == 0)
			sleep_ms(IDLE_SLEEP_MS);
	}
}

#if defined(_WIN32)
static DWORD WINAPI thread_entry(LPVOID argument)
{
	polling_loop((struct zet139_device*)argument);
	return 0;
}
#else
static void* thread_entry(void* argument)
{
	polling_loop((struct zet139_device*)argument);
	return NULL;
}
#endif

static int start_thread(struct zet139_device* device)
{
#if defined(_WIN32)
	device->thread = CreateThread(NULL, 0, thread_entry, device, 0, NULL);
	return device->thread != NULL ? 0 : -1;
#else
	return pthread_create(&device->thread, NULL, thread_entry, device) == 0 ? 0 : -1;
#endif
}

static void wait_thread(struct zet139_device* device)
{
#if defined(_WIN32)
	WaitForSingleObject(device->thread, INFINITE);
	CloseHandle(device->thread);
#else
	pthread_join(device->thread, NULL);
#endif
}

static void free_device(struct zet139_device* device)
{
	if (device->thread_started)
	{
		device->running = 0;
		wait_thread(device);
	}

	close_port(device);

	for (uint32_t channel = 0; channel < ZET139_CHANNELS; ++channel)
		free(device->buffer[channel]);

	mutex_destroy(&device->mutex);
	free(device);
}

// Поиск датчика ZET 139 на USB (serial = 0 — первый найденный), подключение
// и запуск фонового потока, который сразу начинает писать данные в буферы.
ZET139_API zet139_connect(uint64_t serial, struct zet139_device** device_ptr)
{
	if (device_ptr == NULL)
		return -1;

	struct zet139_device* device = calloc(1, sizeof(struct zet139_device));
	if (device == NULL)
		return -2;

	mutex_init(&device->mutex);
	device->serial = serial;

	int result = connect_to_device(device);
	if (result != 0)
	{
		free_device(device);
		return result;
	}

	device->running = 1;

	if (start_thread(device) != 0)
	{
		free_device(device);
		return -6;
	}

	device->thread_started = 1;
	*device_ptr = device;

	return 0;
}

// Остановка фонового потока, закрыть порт, освободить память датчика.
ZET139_API zet139_disconnect(struct zet139_device** device_ptr)
{
	if (device_ptr == NULL || *device_ptr == NULL)
		return -1;

	free_device(*device_ptr);
	*device_ptr = NULL;

	return 0;
}

// Опрос связи датчика, его частоты, серийного номера и порта.
ZET139_API zet139_get_state(struct zet139_device* device, struct zet139_state* state)
{
	if (device == NULL || state == NULL)
		return -1;

	mutex_lock(&device->mutex);
	state->connected = (uint16_t)device->connected;
	state->frequency = device->parameters.frequency;
	state->frequency_real = device->parameters.frequency_real;
	state->serial = device->serial;
	snprintf(state->port, sizeof(state->port), device->port);
	state->port[sizeof(state->port) - 1] = '\0';
	mutex_unlock(&device->mutex);

	return 0;
}

// Копирование в data последних size отсчётов канала channel (0 — X, 1 — Y, 2 — Z).
ZET139_API zet139_channel_get_data(struct zet139_device* device, uint32_t channel, float* data, uint32_t size)
{
	if (device == NULL || data == NULL)
		return -1;
	if (channel >= ZET139_CHANNELS)
		return -2;
	if (size == 0)
		return -3;

	mutex_lock(&device->mutex);

	uint32_t buffer_size = device->buffer_size;

	if (size > buffer_size)
	{
		mutex_unlock(&device->mutex);
		return -3;
	}

	if (!device->connected)
	{
		mutex_unlock(&device->mutex);
		return -5;
	}

	if (device->stored_count < size)
	{
		mutex_unlock(&device->mutex);
		return -4;
	}

	const float* buffer = device->buffer[channel];

	uint32_t start = (device->write_position + buffer_size - size) % buffer_size;
	uint32_t until_end = buffer_size - start;

	if (size <= until_end)
	{
		memcpy(data, buffer + start, size * sizeof(float));
	}
	else
	{
		memcpy(data, buffer + start, until_end * sizeof(float));
		memcpy(data + until_end, buffer, (size - until_end) * sizeof(float));
	}

	mutex_unlock(&device->mutex);

	return 0;
}
