# w5500-async-driver

Легковесная асинхронная библиотека на чистом **C99** для аппаратного сетевого
контроллера **WizNet W5500**. Общий репозиторий для всех стадий разработки
(см. [docs/](docs/README.md)).

## Возможности

- **Три слоя архитектуры** по ТЗ: абстракция железа (HAL-интерфейс через
  коллбеки), ядро протокола (чистый C, без заголовков вендоров) и
  событийный сокетный API.
- **Multi-context**: состояние всех чипов живёт в контексте
  (`w5500_handle_t`), глобальных переменных драйвера нет — можно держать
  несколько W5500 на одной шине.
- **Неблокирующее API**: `connect`/`send`/`recv` возвращаются сразу,
  прогресс приходит через коллбеки (`CONNECTED`, `DATA_RECEIVED`,
  `DISCONNECTED`, `SEND_DONE`, `ERROR`) из `w5500_poll()`.
- **Zero-copy**: приём данных идёт из RX-буфера чипа напрямую в буфер
  приложения, передача — напрямую из пользовательского буфера в TX-память
  чипа, без промежуточных копий.
- **Таймауты вместо вечного поллинга**: любое ожидание регистров чипа
  ограничено программным таймаутом и возвращает `W5500_ERR_TIMEOUT`.
- **Потокобезопасность**: необязательные хуки `lock`/`unlock` в интерфейсе
  платформы (FreeRTOS-порт прилагается).
- **Без malloc в сокетах**: аппаратные сокеты распределяются из массива
  внутри контекста. Контекст можно выделить статически
  (`w5500_init_static`) или через `malloc` (`w5500_init`).
- **Порт STM32 на bare-metal CMSIS**: работа с SPI/GPIO напрямую через
  регистры (без HAL), задержки через DWT; настройка пинов — макросами.

## Структура

```
├── include/              # Публичные заголовки API
│   ├── w5500.h           # Ядро: чип, сеть, таймауты, raw-доступ
│   └── w5500_sockets.h   # Событийный API сокетов
├── src/                  # Реализация ядра (чистый C99)
│   ├── w5500.c
│   ├── w5500_sockets.c
│   ├── w5500_internal.h  # Внутренний контекст (скрыт от пользователя)
│   └── w5500_registers.h # Карта регистров W5500
├── ports/
│   ├── stm32/            # Bare-metal CMSIS-порт (SPI/GPIO/DWT)
│   └── freertos/         # Мьютекс для многозадачности
├── examples/
│   ├── stm32f4_baremetal/  # TCP-клиент без ОС
│   └── stm32f4_freertos/   # TCP-сервер (echo) под FreeRTOS
├── tests/                # Юнит-тесты + эмулятор W5500 для хоста
├── docs/                 # ТЗ стадий
└── Makefile              # Хост-сборка ядра и тестов
```

## Быстрый старт

```c
#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_port_stm32.h"

static void on_event(w5500_socket_t s, w5500_event_t ev,
                     w5500_result_t st, void *arg)
{
    if (ev == W5500_EVENT_DATA_RECEIVED) {
        uint8_t buf[128];
        uint16_t n;
        while (w5500_socket_recv(s, buf, sizeof buf, &n) == W5500_OK && n)
            ; /* обработать n байт из buf */
    }
}

int main(void)
{
    w5500_plat_if_t plat = {0};
    w5500_handle_t nic;
    w5500_socket_t sock;
    static const uint8_t ip[4]   = {192, 168, 1, 42};
    static const uint8_t mask[4] = {255, 255, 255, 0};
    static const uint8_t gw[4]   = {192, 168, 1, 1};
    static const uint8_t mac[6]  = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    static const uint8_t srv[4]  = {192, 168, 1, 10};

    w5500_port_stm32_init();
    w5500_port_stm32_fill_plat(&plat);

    w5500_init(&nic, &plat);
    w5500_check(nic);                       /* VERSIONR == 0x04 */
    w5500_set_mac(nic, mac);
    w5500_set_ip(nic, ip);                  /* + mask/gateway */
    w5500_set_subnet_mask(nic, mask);
    w5500_set_gateway(nic, gw);

    w5500_socket_open(nic, W5500_PROTO_TCP, 5000, on_event, NULL, &sock);
    w5500_socket_connect(sock, srv, 5000);

    for (;;) {
        w5500_poll(nic);                    /* вызывать периодически */
        w5500_port_stm32_delay_ms(10);
    }
}
```

Полные примеры: [examples/stm32f4_baremetal](examples/stm32f4_baremetal/main.c),
[examples/stm32f4_freertos](examples/stm32f4_freertos/main.c).

## Сборка и тесты (хост)

Ядро собирается любым C99-компилятором; тесты гоняются против
программного эмулятора W5500 (без железа):

```sh
make test    # -Wall -Wextra -std=c99 -Werror, все тесты должны пройти
make lib     # build/libw5500.a
```

Порты `ports/stm32` и `ports/freertos` в хост-сборку не входят: им нужны
CMSIS-заголовки устройства и заголовки FreeRTOS соответственно.

## Интеграция в прошивку

1. Добавьте `include/`, `src/*.c` в свой проект.
2. Подключите `ports/stm32/w5500_port_stm32.c` (STM32F4, CMSIS) либо
   напишите свой порт, реализовав `w5500_plat_if_t`.
3. Пины по умолчанию: SPI1 — PA4=CS, PA5=SCK, PA6=MISO, PA7=MOSI,
   режим SPI 0, делитель BR=3 (fPCLK/16). Переопределяются макросами
   `W5500_STM32_*` (см. [ports/stm32/w5500_port_stm32.h](ports/stm32/w5500_port_stm32.h)).
4. Для RTOS: `w5500_rtos_lock_init()` + `w5500_rtos_bind_plat(&plat)`,
   `w5500_poll()` вызывайте из одной сетевой задачи.
5. Для DMA: после `w5500_port_stm32_fill_plat()` подмените
   `plat.spi_transfer_burst` собственной DMA-реализацией.

## Стадии

| Стадия | Статус | Описание |
|--------|--------|----------|
| 1 | ✅ | Ядро + асинхронные сокеты + порты STM32/FreeRTOS + тесты |
| 2+ | 📋 | См. [docs/](docs/README.md) |

ТЗ стадии 1: [docs/stages/stage1.md](docs/stages/stage1.md).

## Лицензия

Не выбрана — при желании добавьте файл LICENSE.
