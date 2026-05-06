# Отчёт по лабораторной работе №6
## Надёжность передачи данных: измерение качества сети, эмуляция помех и механизм повторной отправки

**Студент:** `<ФИО>`  
**Группа:** `<Номер группы>`  
**Дата:** 05.05.2026  

---

### 1. Цель работы
Расширить клиент-серверное приложение, реализованное в предыдущей лабораторной работе, за счёт добавления механизмов контроля качества канала связи, учебной эмуляции сетевых помех и протокола надёжной доставки на прикладном уровне. В ходе работы требовалось:
- реализовать измерение RTT, джиттера и потерь пакетов;
- добавить эмуляцию задержек, потерь и повреждения данных на стороне сервера;
- внедрить механизм подтверждения доставки (`ACK`), повторной отправки при таймауте и защиты от дубликатов;
- обеспечить потокобезопасную работу всех разделяемых структур;
- провести экспериментальное тестирование устойчивости системы к сетевым нарушениям.

---

### 2. Теоретические сведения
Протокол TCP гарантирует надёжную доставку **потока байтов**, но не сохраняет границы логических сообщений и не контролирует целостность прикладных данных после десериализации. В реальных сетях возможны:
- потеря логических сообщений из-за обрывов соединений или таймаутов;
- дублирование пакетов при повторных передачах;
- повреждение полезных данных на промежуточных узлах;
- нестабильное время отклика (jitter).

Для решения этих задач на прикладном уровне применяются механизмы, аналогичные протоколам канального уровня (Go-Back-N, Selective Repeat):
- **ACK (Acknowledgement)** – подтверждение успешного получения и обработки сообщения;
- **Retransmission** – повторная отправка при отсутствии ACK в течение заданного таймаута;
- **Deduplication** – игнорирование сообщений с уже обработанными идентификаторами (`msg_id`);
- **Метрики качества**:  
  $\text{RTT} = t_{\text{recv}} - t_{\text{send}}$,  
  $\text{Jitter} = |\text{RTT}_n - \text{RTT}_{n-1}|$,  
  $\text{Loss} = \frac{N_{\text{sent}} - N_{\text{recv}}}{N_{\text{sent}}} \cdot 100\%$.

---

### 3. Архитектура и реализация

#### 3.1. Структура протокола
В протокол добавлен тип `MSG_ACK = 15` для подтверждений и `MSG_PING / MSG_PONG` для диагностики. Структура `MessageEx` содержит поля `type`, `msg_id`, `timestamp`, `sender`, `receiver`, `payload`, `length`, что позволяет однозначно идентифицировать и маршрутизировать сообщения.

#### 3.2. Диагностика сети (`/ping`, `/netdiag`)
Клиент сохраняет `msg_id` и время отправки (`now_ms()`). При получении `MSG_PONG` вычисляется RTT. Джиттер рассчитывается как модуль разницы текущего и предыдущего успешного RTT. Потери фиксируются по истечению абсолютного таймаута (`pthread_cond_timedwait`). Команда `/netdiag` агрегирует статистику и экспортирует её в JSON-файл `net_diag_<nickname>.json`.

#### 3.3. Эмуляция сетевых помех
На сервере реализована функция `simulate_network()`, вызываемая строго до передачи данных в прикладной уровень. Помехи применяются в заданном порядке:
1. `--delay` – пауза через `usleep()`;
2. `--drop` – вероятностный отказ обработки (возврат `0` из функции);
3. `--corrupt` – случайная замена одного байта в `payload`.
Все события логируются с префиксом `[Transport][SIM]`.

#### 3.4. Надёжная доставка
- **Клиент:** Функция `send_reliable()` помещает сообщение в очередь ожидания ACK, отправляет его и ожидает подтверждения 2 секунды. При таймауте выполняется до 3 повторных попыток. Логируется каждый этап (`[Transport][RETRY]`).
- **Сервер:** Циклический буфер `processed_ids[32]` хранит идентификаторы последних обработанных сообщений. При повторном получении того же `msg_id` сообщение игнорируется, а сервер всё равно отправляет `MSG_ACK` для остановки повторных передач клиента. Лог: `[Application][DEDUP]`.

#### 3.5. Потокобезопасность
Для синхронизации используются:
- `pthread_mutex_t` – для очередей клиентов, оффлайн-сообщений, истории и очередей ACK/PING;
- `pthread_cond_t` с абсолютными дедлайнами – для неблокирующего ожидания ACK и PONG;
- `atomic_int g_running` – для безопасного завершения потоков;
- `select()` в CLI – для неблокирующего чтения `stdin` параллельно с приёмом данных.

---

### 4. Тестирование и результаты

#### 4.1. Сценарий проверки
Сервер запущен с параметрами эмуляции нестабильной сети:
```bash
./server.exe --delay=100 --drop=0.3 --corrupt=0.1
```

#### 4.2. Фрагменты логов

Потеря + повторная отправка:
```text
  --CLIENT--
  [Transport][SEND] send MSG_TEXT (id=3)

  --SERVER--
  [Transport][SIM] DELAY 100ms (id=3)
  [Transport][SIM] DROP (id=3, rate=0.30)
  [2026-05-06 14:20:00][Network Access] frame RECV via network interface
  [2026-05-06 14:20:00][Internet] src=127.0.0.1 dst=127.0.0.1 proto=TCP
  [2026-05-06 14:20:00][Transport] RECV port=44830 -> port=8888
  [2026-05-06 14:20:00][Application] deserialize MessageEx

  --CLIENT--
  [Transport][RETRY] wait ACK timeout
  [Transport][RETRY] resend 1/3 (id=3)

  --SERVER--
  [Transport][SIM] DELAY 100ms (id=3)
  [2026-05-06 14:20:00][Network Access] frame RECV via network interface
  [2026-05-06 14:20:00][Internet] src=127.0.0.1 dst=127.0.0.1 proto=TCP
  [2026-05-06 14:20:00][Transport] RECV port=44830 -> port=8888
  [2026-05-06 14:20:00][Application] handle MSG_TEXT -> broadcast
  [Application][ACK] process MSG_TEXT (id=3)
  [Transport][ACK] send MSG_ACK (id=3)

  --CLIENT--
  [Transport][ACK] recv MSG_ACK (id=3)
```

Пример работы netdiag 
```text
  > /netdiag
  RTT avg : 102.2 ms
  Jitter  : 0.4 ms
  Loss    : 30.0%
  > [Application] saved net_diag_pig.json
```

```json
{
  "nickname": "pig",
  "sent": 10,
  "received": 7,
  "rtt_avg_ms": 102.164,
  "jitter_avg_ms": 0.400,
  "loss_percent": 30.000
}
```

Дедупликация:
```text
  --CLIENT--
  > Hi
  [Transport][SEND] send MSG_TEXT (id=6)

  --SERVER--
  [2026-05-06 14:20:00][Network Access] frame RECV via network interface
  [2026-05-06 14:20:00][Internet] src=127.0.0.1 dst=127.0.0.1 proto=TCP
  [2026-05-06 14:20:00][Transport] RECV port=46374 -> port=8888
  [2026-05-06 14:20:00][Application] deserialize MessageEx
  [Transport][SIM] DELAY 3000ms (id=6)
  [2026-05-06 14:20:03][Network Access] frame RECV via network interface
  [2026-05-06 14:20:03][Internet] src=127.0.0.1 dst=127.0.0.1 proto=TCP
  [2026-05-06 14:20:03][Transport] RECV port=46374 -> port=8888
  [2026-05-06 14:20:03][Application] handle MSG_TEXT -> broadcast
  [Application][ACK] process MSG_TEXT (id=6)
  [Transport][ACK] send MSG_ACK (id=6)

  --CLIENT--
  [Transport][RETRY] wait ACK timeout
  [Transport][RETRY] resend 1/3 (id=6)

  --SERVER--
  [2026-05-06 14:20:03][Network Access] frame RECV via network interface
  [2026-05-06 14:20:03][Internet] src=127.0.0.1 dst=127.0.0.1 proto=TCP
  [2026-05-06 14:20:03][Transport] RECV port=46374 -> port=8888
  [2026-05-06 14:20:03][Application] deserialize MessageEx
  [Transport][SIM] DELAY 3000ms (id=6)
  [Transport][SIM] CORRUPT (id=6, rate=0.10)
  [2026-05-06 14:20:06][Network Access] frame APP via network interface
  [2026-05-06 14:20:06][Internet] src=127.0.0.1 dst=127.0.0.1 proto=TCP
  [2026-05-06 14:20:06][Transport] APP port=46374 -> port=8888
  [2026-05-06 14:20:06][Application] DEDUP check
  [Application][DEDUP] duplicate ignored (id=6)
  [Transport][RETRY] resend detected (id=6)
  [Transport][ACK] send MSG_ACK (id=6)

  --CLIENT--
  [Transport][ACK] recv MSG_ACK (id=6)
```

