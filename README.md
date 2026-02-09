# ESP8266 multitool

Мультитул‑прошивка для NodeMCU V3 (ESP8266MOD): мост UART по Wi‑Fi (telnet) и I2C‑инструменты (скан, чтение/запись, пассивный лог).

## ВНИМАНИЕ!!!
!!!ЧТЕНИЕ/ЗАПИСЬ SPI НЕ ПРОВЕРЕНЫ НА РАБОТОСПОСОБНОСТЬ!!!

## Возможности

- UART мост по Wi‑Fi (telnet) для работы с консолью устройств.
- Управление концами строк (CR/CRLF/LF) для старых загрузчиков.
- I2C master: scan/read/write, поддержка 8‑ и 16‑битных регистров.
- I2C passive: слушатель с логированием транзакций (best‑effort).
- SPI flash: чтение, запись, стирание, JEDEC ID.

## Подключение UART

По умолчанию используется `Serial.swap()`:

- RX NodeMCU = GPIO13
- TX NodeMCU = GPIO15
- GND общий

Важно: уровни только 3.3V, питание устройства от NodeMCU не подавать.

## Подключение I2C

По умолчанию:

- SDA = GPIO4 (D2)
- SCL = GPIO5 (D1)
- GND общий

Можно поменять пины командой `i2c pins <sda> <scl>`.

## Подключение SPI

По умолчанию (HSPI):

- SCK = GPIO14 (D5)
- MISO = GPIO12 (D6)
- MOSI = GPIO13 (D7)
- CS = GPIO16 (D0)
- GND общий

Пины можно изменить командой `spi pins <sck> <miso> <mosi> <cs>`.

## Wi‑Fi

Прошивка работает в режиме STA и подключается к:

- SSID: ``
- PASS: ``
- IP: `192.168.0.156`
- GW: `192.168.0.1`
- MASK: `255.255.255.0`

Если нужно изменить — правь значения в `UARTDrocher.ino`.

## Telnet

Подключение стандартное. Можно как и через PuTTY, так и через netcat, telnet, etc

Команды:

- `help` или `/help`
- `go` или `/go` — режим моста (все байты идут в UART)
- `Ctrl+L` — выход из моста
- `cr` / `crlf` / `lf` — управление концом строки
- `scan` — авто‑поиск UART скорости (нужен активный вывод)
- `baud <rate>` — выставить UART скорость

## I2C команды

- `i2c scan`
- `i2c read <addr> <reg> <len>`
- `i2c read16 <addr> <reg16> <len>`
- `i2c readraw <addr> <len>`
- `i2c write <addr> <reg> <b1> <b2> ...`
- `i2c pins <sda> <scl>`
- `i2c clock <hz>` (10k–400k)
- `i2c mode passive`
- `i2c mode master`

Примеры:

```
i2c scan
i2c read 0x50 0x00 16
i2c read16 0x68 0x0000 8
i2c readraw 0x32 8
```

## SPI команды

- `spi id`
- `spi read <addr> <len>`
- `spi write <addr> <b1> <b2> ...`
- `spi erase4k <addr>`
- `spi erase64k <addr>`
- `spi erasechip`
- `spi pins <sck> <miso> <mosi> <cs>`
- `spi clock <hz>`
- `spi mode <0-3>`

Примеры:

```
spi id
spi read 0x000000 64
spi erase4k 0x000000
spi write 0x000000 DE AD BE EF
```

## Примечания

- Пассивный I2C‑лог на ESP8266 не гарантирует 100% точность.
- Если на шине уже есть мастер (SoC приставки), активные I2C команды могут конфликтовать.

## Сборка

1) Открыть `UARTDrocher.ino` в Arduino IDE.
2) Плата: NodeMCU 1.0 (ESP‑12E Module).
3) Залить прошивку и подключиться по telnet.
