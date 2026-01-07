# Этап 03. Масштабируемый producer–consumer конвейер

## Цель этапа

Развить producer–consumer pipeline (этап 02) до **инженерного уровня**, применив:

* принципы модели памяти C++;
* проектирование потокобезопасных структур данных;
* bounded очереди и backpressure;
* масштабирование по потокам;
* корректную отмену и завершение работы.

Результат этапа — **устойчивый, масштабируемый и управляемый конвейер**, корректно работающий под нагрузкой и при принудительном завершении.

---

## Ключевая идея этапа

Pipeline этапа 03 обязан:

1. **Ограничивать использование памяти**
   Все очереди имеют фиксированную емкость (bounded).

2. **Реализовывать backpressure**
   При переполнении очередей producer обязан ждать (с тайм-аутом), а не бесконтрольно производить данные.

3. **Масштабироваться по потокам**
   Каждая стадия обрабатывается пулом worker-потоков.

4. **Корректно завершаться и отменяться**
   Pipeline обязан:

   * корректно завершать работу через `shutdown()`;
   * уметь быстро и безопасно завершаться через `cancel()`.

---

## Ограничения

Запрещено:

* lock-free структуры данных;
* busy-wait (циклы ожидания с `yield`, `sleep_for`, пустыми проверками);
* изменение публичного интерфейса `Order`.

Разрешено и ожидается:

* `std::mutex`, `std::unique_lock`;
* `std::condition_variable`;
* `std::jthread` и **обязательное использование `std::stop_token`**;
* стандартные контейнеры STL.

---

## Файлы этапа

Используются и расширяются **только** следующие файлы:

* `order.hpp` (без изменения интерфейса);
* `queue.hpp` (bounded блокирующая очередь);
* `pipeline.hpp` (пулы стадий, backpressure, cancel);
* `metrics.hpp` (расширенные метрики).

---

# Часть 1. BoundedBlockingQueue с backpressure

## Файл: `queue.hpp`

Реализовать **bounded блокирующую очередь** для передачи данных между стадиями.

### Обязательный интерфейс

```cpp
template <typename T>
class BoundedBlockingQueue {
public:
    explicit BoundedBlockingQueue(std::size_t capacity);

    // Producer: блокируется, пока есть место или пока очередь не закрыта.
    // Возвращает false, если очередь закрыта.
    bool push(T value);

    // Producer: блокируется не более timeout.
    // Возвращает false при тайм-ауте или если очередь закрыта.
    template <class Rep, class Period>
    bool push_for(T value,
        const std::chrono::duration<Rep, Period>& timeout);

    // Consumer: блокируется до появления элемента или закрытия очереди.
    // Возвращает false, если очередь закрыта и пуста.
    bool wait_pop(T& out);

    // Consumer: блокируется не более timeout.
    // Возвращает false при тайм-ауте или если очередь закрыта и пуста.
    template <class Rep, class Period>
    bool wait_pop_for(T& out,
        const std::chrono::duration<Rep, Period>& timeout);

    void close();               // запрещает новые push, будит всех ожидающих
    bool closed() const;
    bool empty() const;

    std::size_t capacity() const;
    std::size_t size() const;

private:
    std::queue<T> queue_;
    std::size_t capacity_;
    bool closed_ = false;

    mutable std::mutex mutex_;
    std::condition_variable cv_not_empty_; // ждать, пока в очереди появится хотя бы один элемент (будит потребителей) 
    std::condition_variable cv_not_full_; // ждать, пока в очереди освободится место (будит производителей)
};
```

### Семантика

* Очередь **никогда не превышает capacity**.
* `push()`:

  * может ждать бесконечно;
  * обязан проснуться при `close()` и вернуть `false`;
* `push_for()`:

  * возвращает `false` при тайм-ауте или при `close()`;
* `wait_pop()` / `wait_pop_for()`:

  * возвращают `false` **только** если очередь закрыта и пуста;
* `close()`:

  * идемпотентен;
  * пробуждает **всех** ожидающих producer-ов и consumer-ов.

Очередь должна быть реализована **без busy-wait**, только через `condition_variable`.

---

## Часть 2. Pipeline с пулами потоков

### Архитектура стадий

Pipeline состоит из трех стадий:

1. **Prepare**: `Accepted -> Prepared`
2. **Pack**: `Prepared -> Packed`
3. **Deliver**: `Packed -> Delivered`

Каждая стадия обрабатывается **пулом worker-потоков**.

---

### Конфигурация Pipeline

```cpp
class Pipeline { 
public: 
    struct Config { 
        std::size_t q_in_capacity = 256; // Максимальное количество заказов во входной очереди. 
        std::size_t q_prepare_capacity = 256; // Максимальная емкость очереди между стадиями Prepare и Pack. 
        std::size_t q_pack_capacity = 256; // Максимальная емкость очереди между стадиями Pack и Deliver. 
        
        std::size_t prepare_workers = 1; // Количество worker-потоков стадии Prepare. 
        std::size_t pack_workers = 1; // Количество worker-потоков стадии Pack. 
        std::size_t deliver_workers = 1; // Количество worker-потоков стадии Deliver. 
        
        std::chrono::milliseconds push_timeout{100}; // Максимальное время ожидания свободного места в очереди при submit() и push между стадиями. 
        std::chrono::milliseconds pop_timeout{50}; // Максимальное время ожидания элемента в очереди worker-потоком. 
    };
};
```

---

## Публичный интерфейс Pipeline

```cpp
public:
    explicit Pipeline(Config cfg = {});
    ~Pipeline();

    void start();

    void submit(Order order);

    void shutdown();
    void cancel();

    Metrics metrics() const;
    const std::vector<Order>& delivered_orders() const;
```

---

## Жизненный цикл

### start()

* запускает все worker-потоки;
* повторный вызов **запрещен** и должен приводить к `std::logic_error`.

### submit()

* потокобезопасен;
* допускается **до вызова `start()`**;
* применяет backpressure:

  * ждет место в очереди не более `cfg.push_timeout`;
  * при тайм-ауте или закрытии:

    * увеличивает `submit_timeout_count`;
    * бросает `std::runtime_error`.

### shutdown()

* закрывает входную очередь;
* гарантирует “доталкивание” заказов через все стадии;
* дожидается завершения **всех** worker-потоков;
* после возврата состояние pipeline стабильно.

### cancel()

* завершает pipeline **как можно быстрее**;
* закрывает все очереди;
* инициирует `stop_token` у всех worker-ов;
* допускается, что часть заказов не будет доставлена;
* идемпотентен.

### ~Pipeline()

* обязан корректно завершить pipeline;
* не должен вызывать `std::terminate`.

---

## Часть 3. Worker-циклы и stop_token

Каждый worker-поток обязан:

* извлекать данные через `wait_pop_for(..., pop_timeout)`;
* регулярно проверять `stop_token.stop_requested()`;
* корректно завершаться:

  * при закрытии очереди и отсутствии данных;
  * при запросе отмены (`cancel()`).

---

## Часть 4. Метрики этапа 03

### Файл: `metrics.hpp`

```cpp
struct Metrics { 

std::uint64_t accepted_count = 0; 
std::uint64_t prepared_count = 0; 
std::uint64_t packed_count = 0; 
std::uint64_t delivered_count = 0; 

std::chrono::nanoseconds total_lead_time{0}; 

std::uint64_t submit_timeout_count = 0; 
// Количество неуспешных submit() из-за backpressure. 
// Увеличивается, когда submit не смог положить заказ в очередь 
// в пределах push_timeout (очередь переполнена или закрыта). 

std::uint64_t prepare_workers_used = 0; 
std::uint64_t pack_workers_used = 0; 
std::uint64_t deliver_workers_used = 0; 
// Фактически запущенное количество worker-потоков на каждой стадии. 
// Используется для проверки масштабирования и корректного старта pipeline. 

std::uint64_t q_in_push = 0; 
std::uint64_t q_in_pop = 0; 
std::size_t q_in_max_size = 0; 
// Метрики входной очереди:
// - q_in_push: сколько заказов было успешно добавлено через submit() 
// - q_in_pop: сколько заказов было извлечено стадией Prepare 
// - q_in_max_size: максимальный размер очереди за все время работы 
// (показывает уровень нагрузки и степень backpressure). 

std::uint64_t q_prepare_push = 0; 
std::uint64_t q_prepare_pop = 0; 
std::size_t q_prepare_max_size = 0; 
// Метрики очереди между Prepare и Pack: 
// - q_prepare_push: сколько заказов Prepare передала дальше 
// - q_prepare_pop: сколько заказов Pack реально получил 
// - q_prepare_max_size: максимальное заполнение очереди 
// (позволяет выявить узкие места между стадиями). 

std::uint64_t q_pack_push = 0; 
std::uint64_t q_pack_pop = 0; 
std::size_t q_pack_max_size = 0; 
// Метрики очереди между Pack и Deliver: 
// - q_pack_push: сколько заказов Pack отправила на доставку 
// - q_pack_pop: сколько заказов Deliver обработал 
// - q_pack_max_size: пиковая нагрузка на последнюю очередь. 
};
```

### Инварианты после `shutdown()`

* `delivered_count <= packed_count <= prepared_count <= accepted_count`;
* `q_in_push == q_in_pop == accepted_count`;
* `q_prepare_push == q_prepare_pop == prepared_count`;
* `q_pack_push == q_pack_pop == packed_count`;
* `delivered_orders().size() == delivered_count`;
* `total_lead_time` = сумма `(delivered_time - accepted_time)` по delivered;
* `*_workers_used == cfg.*_workers`.

Метрики возвращаются как **консистентный snapshot**.

---

## Часть 5. Что проверяется тестами

Тесты этапа 03 **обязательно проверяют**:

1. `BoundedBlockingQueue`:

   * корректное блокирование producer при переполнении;
   * корректное блокирование consumer при пустоте;
   * корректную работу тайм-аутов;
   * пробуждение всех ожидающих при `close()`;
   * отсутствие busy-wait.

2. `Pipeline`:

   * запуск нужного числа worker-потоков;
   * корректную доставку всех заказов при `shutdown()`;
   * корректную работу backpressure (`submit_timeout_count`);
   * корректную работу `cancel()` без deadlock и зависших потоков;
   * стабильность `metrics()` и `delivered_orders()` после завершения.

---

## CLI

Небольшое изменение в файле src/main.cpp с добавлением новых режимов и метрик:
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

int main(int argc, char* argv[]) {
    std::size_t orders_count = 5000;
    std::string mode = "shutdown"; // shutdown | cancel

    // Usage:
    //   ops_app
    //   ops_app [orders_count]
    //   ops_app [orders_count] [shutdown|cancel]
    if (argc == 2) {
        try {
            orders_count = static_cast<std::size_t>(std::stoull(argv[1]));
        }
        catch (...) {
            std::cerr << "Usage: ops_app [orders_count] [shutdown|cancel]\n";
            return 1;
        }
    }
    else if (argc == 3) {
        try {
            orders_count = static_cast<std::size_t>(std::stoull(argv[1]));
        }
        catch (...) {
            std::cerr << "Usage: ops_app [orders_count] [shutdown|cancel]\n";
            return 1;
        }

        mode = argv[2];
        if (mode != "shutdown" && mode != "cancel") {
            std::cerr << "Usage: ops_app [orders_count] [shutdown|cancel]\n";
            return 1;
        }
    }
    else if (argc > 3) {
        std::cerr << "Usage: ops_app [orders_count] [shutdown|cancel]\n";
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

    pipeline.start();

    std::uint64_t next_id = 1;

    std::size_t submitted_ok = 0;
    std::size_t submit_failed = 0;

    const auto t0 = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < orders_count; ++i) {
        try {
            pipeline.submit(Order{ static_cast<OrderId>(next_id++) });
            ++submitted_ok;
        }
        catch (const std::runtime_error&) {
            // submit timeout / closed
            ++submit_failed;
        }
        catch (...) {
            std::cerr << "submit failed with unexpected exception\n";
            return 2;
        }
    }

    if (mode == "shutdown") {
        pipeline.shutdown();
    } else {
        pipeline.cancel();
    }

    const auto t1 = std::chrono::steady_clock::now();

    const Metrics m = pipeline.metrics();
    const auto& delivered = pipeline.delivered_orders();

    std::cout << "Mode: " << mode << "\n";
    std::cout << "Requested submits: " << orders_count << "\n";
    std::cout << "Submit ok: " << submitted_ok << "\n";
    std::cout << "Submit failed: " << submit_failed << "\n\n";

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
        if (!(m.q_in_push == m.q_in_pop && m.q_prepare_push == m.q_prepare_pop && m.q_pack_push == m.q_pack_pop)) {
            std::cout << "WARN: queue push/pop counters mismatch after shutdown\n";
        }
    } else {

    }

    return 0;
}
```

Так же в связи с добавлением режимов запуска появились новые команды для CLI. Их вы можете найти в текущем шаге в файле BUILD_CLI.md