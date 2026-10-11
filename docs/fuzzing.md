# Фаззинг драйвера W5500

Семь фазз-харнессов поверх эмулятора чипа (`tests/mock_w5500.c`) гоняются
libFuzzer + ASan/UBSan. Каждое свойство проверяется `abort()`, поэтому
libFuzzer останавливается и сохраняет падающий вход как обычный краш.

## Харнесс

| Цель | Что фаззит | Ключевые инварианты |
|------|------------|---------------------|
| `fuzz-reg-access` | `w5500_read_regs`/`write_regs` с произвольными `addr/bsb/len` | roundtrip с известными исключениями эмулятора (MR, Sn_CR, Sn_IR, FSR/RSR, kind-0), `len==0 → ERR_PARAM` |
| `fuzz-socket-lifecycle` | скрипт OPEN/CLOSE/LISTEN/CONNECT/... на 4 слотах | баланс `in_use` ↔ `Sn_SR`, NOMEM при исчерпании слотов, консистентность после каждой операции |
| `fuzz-poll` | `w5500_poll()` с покерами IR/SR, инъекциями и REENTRANCY-коллбеками | зеркало слотов в коллбеке, повторный вход в драйвер из коллбека, баланс lock/unlock |
| `fuzz-send-recv` | `send/sendto/recv/rx_available/tx_free/get_peer` + NULL-пробы | `*sent ≤ len`, таймаут ⇒ `*sent==0`, `*recvd ≤ pending`, RX_RSR == учёту инъекций |
| `fuzz-net-config` | set/get сети, SETBUF-матрица, timeout, link, sw_reset, NULL-пробы | сырой образ регистров == API, неизменность соседних байт, `rc==OK ⇔ kb∈{1,2,4}` |
| `fuzz-context` | `init`/`init_static`/`deinit` с канарейкой 0xA5 | при `mem_size < context_size` ничего не пишется; за границей контекста канарейка живёт до и после `deinit` |
| `fuzz-spi-frame` | `w5500_ctrl_byte`, декодер кадра мока, полные кадры write→read | точная раскладка `(BSB<<3)|(RWB<<2)|OM`, покадровый протокол select→addr→ctrl→data→deselect |

Общие фикстуры: `tests/fuzz/fuzz_common.h` (сброс эмулятора на каждый вход,
фиксированный таймаут 4 мс — сам таймаут **не** фаззится, чтобы вход не мог
зависнуть в цикле ожидания), `tests/fuzz/fuzz_runner.c` — standalone-runner
для режимов без libFuzzer.

Внутренности драйвера доступны харнессам через `-DW5500_TEST_HARNESS`
(`w5500_ctrl_byte`, слоты контекста).

## Запуск

```sh
make fuzz-all                    # все 7 целей, libFuzzer + ASan/UBSan, 120 с
make fuzz-reg-access             # одна цель
FUZZ_TIME=60 make fuzz-all       # секунд на цель
FUZZ_MODE=asan make fuzz-all     # без libFuzzer: standalone-runner + ASan/UBSan
FUZZ_MODE=none make fuzz-reg-access  # без санитайзеров, прогон по corpus
```

- `FUZZ_MODE=fuzzer` (по умолчанию) — `-fsanitize=fuzzer,address,undefined`;
  нужен Clang со `libFuzzer`. На Windows этот режим локально **не линкуется**
  (libFuzzer собран против MSVC STL, которого нет в scoop-clang) —
  используйте `FUZZ_MODE=asan`.
- `FUZZ_MODE=asan` — тот же runner входа/свойств, покрытие включает
  SanitizerCoverage через `-DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION`.
  На Windows exe нуждается в `clang_rt.asan_dynamic-x86_64.dll`; Makefile
  добавляет `$(clang -print-resource-dir)/lib/windows` в `PATH` при запуске.

## Corpus и словарь

- `tests/fuzz/corpus/*.bin` — проверенные сиды (zero, ff, regs, frame,
  api_script). Общие для всех целей: каждая цель трактует байты по-своему.
- `tests/fuzz/w5500.dict` — токены протокола: SPI-кодировки BSB/RWB, адреса
  регистров, команды `Sn_CR`, биты `Sn_IR`, размеры буферов.
- Рабочий corpus пишется в `build/fuzz/corpus/<цель>/` (в `.gitignore`),
  сиды копируются туда `cp -n` при каждом запуске — чекаут не пачкается.

## CI

`.github/workflows/ci.yml`:

- **host-tests** — `make test` (GCC, юнит-тесты);
- **fuzz** — `make FUZZ_CC=clang FUZZ_TIME=5 fuzz-all` (libFuzzer), с кешем
  apt-пакетов и кешем `build/fuzz/corpus` между запусками (ключ на коммит +
  restore-key по префиксу), чтобы покрытие накапливалось.

## Сознательно не фаззится

- **Двойной `w5500_deinit()`** — документированный use-after-free.
- **Значение таймаута** — большие значения превращают циклы ожидания в
  гигантские, вход перестаёт быть ограниченным по времени (в самих
  харнессах фиксировано 4 мс).
- **Реальный SPI** — проверяется эмулятором; харнесс `fuzz-spi-frame`
  покрывает формат кадра, но не электрику шины.
