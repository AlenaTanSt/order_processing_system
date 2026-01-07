# Этап 04. Завершение pipeline: жизненный цикл, shutdown, инварианты

## Цель этапа

Pipeline должен стать:
- управляемым по жизненному циклу;
- предсказуемым при остановке;
- безопасным при разрушении;
- корректным при исключениях;
- формально определенным по контракту.

---

## 1. Жизненный цикл Pipeline

### Логическая модель состояний

Pipeline обязан иметь явно определенную модель состояний (enum class в глобальной области файла `pipeline.hpp`).

```cpp
enum class PipelineState {
    Created,
    Running,
    Draining,
    Stopped,
    Failed
};
````

### Публичный интерфейс

```cpp
class Pipeline {
public:
    struct Config { 
    // остается прежним
    }

    explicit Pipeline(Config cfg = {}); // Потоки не запущены, очереди инициализированы. Состояние: Created.
    ~Pipeline() noexcept; // Не бросает исключений. Если pipeline работает - выполняет shutdown_now(). Не зависает.

    void start(); // Created -> Running. Повторный вызов безопасен (idempotent).
    void shutdown(); // graceful: Running -> Draining -> Stopped.
    void shutdown_now(); // forced: из любого состояния переводит в Stopped (если не было исключения). Не зависает.

    bool is_running() const noexcept; // true только в состоянии Running.
    bool is_stopped() const noexcept; // true в состоянии Stopped или Failed.
    PipelineState state() const noexcept; // Текущее состояние pipeline.

    bool submit(Order order); // Принимает заказ только в Running. Не бросает исключений.

    Metrics metrics() const; // Снимок метрик. Потокобезопасно.
    const std::vector<Order>& delivered_orders() const; // Список успешно доставленных заказов (Delivered). Потокобезопасно.

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
};
```

Гарантии потокобезопасности:

* Методы: state(), is_running(), is_stopped(), metrics(), delivered_orders() допускают конкурентный вызов из разных потоков.
* submit() допускает конкурентный вызов несколькими producer-потоками, если pipeline в состоянии Running.

---

## 2. Контракт методов Pipeline

### start()

```cpp
void Pipeline::start();
```

Контракт:

* Переводит pipeline из Created в Running.
* Запускает worker-потоки.
* Повторный вызов в Running игнорируется (idempotent).
* В состояниях Draining, Stopped, Failed: бросает std::logic_error.

---

### submit(Order order)

```cpp
bool Pipeline::submit(Order order);
```

Контракт:

* Допустим только в состоянии Running.
* При успешном принятии:

  * увеличивает accepted_count;
  * заказ получает статус Accepted.
* Если pipeline в состоянии Draining, Stopped, Failed:

  * заказ не принимается;
  * метод возвращает false;
  * заказ получает статус Rejected (или Canceled, но единообразно по проекту).
* Метод не бросает исключений.

---

### shutdown() (graceful)

```cpp
void Pipeline::shutdown();
```

Контракт:

* Переводит pipeline из Running в Draining.
* Новые заказы не принимаются.
* Все ранее принятые заказы должны быть обработаны до статуса Delivered (graceful не отменяет "принятые" заказы).
* Все worker-потоки завершаются корректно, join выполняется гарантированно.
* Повторный вызов безопасен и не приводит к deadlock.
* После завершения состояние Stopped.

---

### shutdown_now() (forced)

```cpp
void Pipeline::shutdown_now();
```

Контракт:

* Немедленно инициирует остановку.
* Закрывает все очереди и пробуждает все ожидающие потоки.
* Worker-потоки выходят как можно быстрее и затем join.
* Заказы "в полете" могут не быть доставлены. Такие заказы должны быть помечены как Canceled или Failed (единообразно по проекту).
* Повторный вызов безопасен.
* Если остановка инициирована без исключений в worker-потоках, итоговое состояние Stopped.

---

### delivered_orders()

```cpp
const std::vector<Order>& Pipeline::delivered_orders() const;
```

Контракт:

* Возвращает контейнер всех заказов, завершенных со статусом Delivered.
* Порядок: соответствует порядку фактического завершения.
* Метод потокобезопасен: допускается вызов параллельно работе pipeline.
* После shutdown_now() контейнер может содержать только часть доставленных заказов.

---

### ~Pipeline()

```cpp
Pipeline::~Pipeline() noexcept;
```

Контракт:

* Никогда не бросает исключений.
* Никогда не блокируется бесконечно.
* Если pipeline еще работает, обязан выполнить shutdown_now().
* Все ресурсы освобождаются, все потоки join.

---

## 3. Очереди (queue.hpp)

### Обязательный интерфейс очереди

```cpp
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity);

    bool push(T value);        // блокирующий: ждет место, либо возвращает false если закрыта
    bool try_push(T value);    // неблокирующий

    bool pop(T& out);          // блокирующий: ждет элемент, либо возвращает false если (закрыта и пуста)
    bool try_pop(T& out);      // неблокирующий

    void close();              // закрывает очередь и будит всех ожидающих
    bool is_closed() const noexcept;

    std::size_t size() const noexcept;
    std::size_t capacity() const noexcept;

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;
};
```

### Контракт очереди

* push:

  * блокируется при заполнении;
  * возвращает false, если очередь закрыта.
* pop:

  * блокируется при пустоте;
  * возвращает false, если очередь закрыта и пуста.
* close():

  * идемпотентен;
  * пробуждает всех ожидающих push/pop.
* После close() ни один поток не должен блокироваться навсегда.

---

## 4. Обработка исключений

Требования:

* Любое исключение внутри worker-потока:

  * перехватывается;
  * переводит pipeline в состояние Failed;
  * инициирует shutdown_now().
* Исключения:

  * не приводят к std::terminate;
  * не выходят за пределы потоков.

---

## 5. Метрики (metrics.hpp)

Интерфейс остается без изменений.

Требования:

* Метрики потокобезопасны.
* Метрики корректны при:

  * graceful shutdown;
  * forced shutdown;
  * исключениях.

---

## 6. Инварианты pipeline

Pipeline считается корректным, если всегда выполняется:

1. Нет утечки потоков (все потоки join).
2. Нет зависших ожиданий (нет deadlock при shutdown/shutdown_now).
3. Нет data race.
4. После состояния Stopped: submit всегда возвращает false.
5. После shutdown_now(): все очереди закрыты.
6. После разрушения Pipeline: не остается активных потоков.

---

## 7. Ограничения этапа

Запрещено:

* добавлять новые стадии pipeline;
* менять семантику уже существующих стадий обработки;
* использовать detach;
* использовать sleep как механизм управления;
* вводить глобальные mutex / condition_variable.

## CLI
В связи с изменением публичного интерфейса необходимо обновить код src/main.cpp.
```
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include "order.hpp"
#include "pipeline.hpp"
#include "metrics.hpp"

namespace {

void print_usage() {
    std::cerr << "Usage: ops_app [orders_count] [shutdown|shutdown_now]\n";
}

const char* to_string(PipelineState s) {
    switch (s) {
    case PipelineState::Created:  return "Created";
    case PipelineState::Running:  return "Running";
    case PipelineState::Draining: return "Draining";
    case PipelineState::Stopped:  return "Stopped";
    case PipelineState::Failed:   return "Failed";
    default:                      return "Unknown";
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::size_t orders_count = 5000;
    std::string mode = "shutdown"; // shutdown | shutdown_now

    // Usage:
    //   ops_app
    //   ops_app [orders_count]
    //   ops_app [orders_count] [shutdown|shutdown_now]
    if (argc == 2) {
        try {
            orders_count = static_cast<std::size_t>(std::stoull(argv[1]));
        }
        catch (...) {
            print_usage();
            return 1;
        }
    }
    else if (argc == 3) {
        try {
            orders_count = static_cast<std::size_t>(std::stoull(argv[1]));
        }
        catch (...) {
            print_usage();
            return 1;
        }

        mode = argv[2];
        if (mode != "shutdown" && mode != "shutdown_now") {
            print_usage();
            return 1;
        }
    }
    else if (argc > 3) {
        print_usage();
        return 1;
    }

    Pipeline::Config cfg;

    cfg.q_in_capacity = 128;
    cfg.q_prepare_capacity = 128;
    cfg.q_pack_capacity = 128;

    cfg.prepare_workers = 2;
    cfg.pack_workers = 2;
    cfg.deliver_workers = 2;

    cfg.push_timeout = std::chrono::milliseconds{ 50 };
    cfg.pop_timeout  = std::chrono::milliseconds{ 20 };

    Pipeline pipeline(cfg);

    try {
        pipeline.start();
    }
    catch (const std::exception& e) {
        std::cerr << "start failed: " << e.what() << "\n";
        return 2;
    }
    catch (...) {
        std::cerr << "start failed with unknown exception\n";
        return 2;
    }

    std::uint64_t next_id = 1;

    std::size_t submitted_ok = 0;
    std::size_t submit_failed = 0;

    const auto t0 = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < orders_count; ++i) {
        const bool ok = pipeline.submit(Order{ static_cast<OrderId>(next_id++) });
        if (ok) ++submitted_ok;
        else ++submit_failed;
    }

    if (mode == "shutdown") {
        pipeline.shutdown();
    }
    else {
        pipeline.shutdown_now();
    }

    const auto t1 = std::chrono::steady_clock::now();

    const Metrics m = pipeline.metrics();
    const auto& delivered = pipeline.delivered_orders();

    std::cout << "Mode: " << mode << "\n";
    std::cout << "Requested submits: " << orders_count << "\n";
    std::cout << "Submit ok: " << submitted_ok << "\n";
    std::cout << "Submit failed: " << submit_failed << "\n\n";

    std::cout << "Pipeline state: " << to_string(pipeline.state()) << "\n\n";

    std::cout << "Accepted:  " << m.accepted_count << "\n";
    std::cout << "Prepared:  " << m.prepared_count << "\n";
    std::cout << "Packed:    " << m.packed_count << "\n";
    std::cout << "Delivered: " << m.delivered_count << "\n";
    std::cout << "Delivered vector size: " << delivered.size() << "\n\n";

    std::cout << "submit_timeout_count: " << m.submit_timeout_count << "\n";
    std::cout << "workers used (prepare/pack/deliver): "
              << m.prepare_workers_used << "/"
              << m.pack_workers_used << "/"
              << m.deliver_workers_used << "\n\n";

    std::cout << "q_in      push/pop/max: " << m.q_in_push << "/" << m.q_in_pop
              << "/" << m.q_in_max_size << "\n";
    std::cout << "q_prepare push/pop/max: " << m.q_prepare_push << "/" << m.q_prepare_pop
              << "/" << m.q_prepare_max_size << "\n";
    std::cout << "q_pack    push/pop/max: " << m.q_pack_push << "/" << m.q_pack_pop
              << "/" << m.q_pack_max_size << "\n\n";

    using namespace std::chrono;
    std::cout << "Total lead time (ms): "
              << duration_cast<milliseconds>(m.total_lead_time).count()
              << "\n";

    std::cout << "Wall time (ms): "
              << duration_cast<milliseconds>(t1 - t0).count()
              << "\n\n";

    if (mode == "shutdown") {
        if (m.delivered_count != m.accepted_count) {
            std::cout << "WARN: delivered_count != accepted_count in shutdown mode\n";
        }
        if (delivered.size() != static_cast<std::size_t>(m.delivered_count)) {
            std::cout << "WARN: delivered_orders size mismatch metrics\n";
        }
        if (!(m.q_in_push == m.q_in_pop &&
              m.q_prepare_push == m.q_prepare_pop &&
              m.q_pack_push == m.q_pack_pop)) {
            std::cout << "WARN: queue push/pop counters mismatch after shutdown\n";
        }
    }
    else {
        if (m.delivered_count > m.packed_count ||
            m.packed_count > m.prepared_count ||
            m.prepared_count > m.accepted_count) {
            std::cout << "WARN: stage counters are inconsistent after shutdown_now\n";
        }
        if (delivered.size() != static_cast<std::size_t>(m.delivered_count)) {
            std::cout << "WARN: delivered_orders size mismatch metrics after shutdown_now\n";
        }
    }

    return 0;
}
```
Так же в связи с изменением режимов запуска обновились команды для CLI. Их вы можете найти в текущем шаге в файле BUILD_CLI.md