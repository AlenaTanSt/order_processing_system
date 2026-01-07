# Этап 02. Producer-Consumer конвейер с передачей данных

## Цель этапа

Реализовать **классический producer-consumer конвейер** с явной передачей данных между стадиями, обеспечив:

* блокирующее ожидание данных;
* отсутствие busy-wait и гонок данных;
* корректное распространение завершения;
* читаемую и поддерживаемую архитектуру стадий;
* воспроизводимые и проверяемые метрики.

---

## Ключевая идея этапа

На этом этапе pipeline работает **поточно**, а не пакетно:

* заказы принимаются через `submit`;
* несколько стадий обрабатывают заказы **параллельно**, передавая данные друг другу;
* каждая связь между стадиями — **потокобезопасная блокирующая очередь**;
* стадии **не знают друг о друге напрямую**;
* завершение работы происходит через **закрытие очередей**, а не через флаги или общие переменные.

---

## Ограничения

Запрещено:

* `std::atomic`;
* lock-free структуры;
* busy-wait (циклы с `yield`, `sleep_for` вместо ожидания);
* изменение публичного интерфейса `Order`.

Разрешено и ожидается:

* `std::mutex`, `std::unique_lock`, `std::lock_guard`;
* `std::condition_variable`;
* `std::jthread` или RAII-обертка над `std::thread`;
* STL контейнеры.

---

## Файлы этапа

Используются и расширяются **только** следующие файлы:

* `order.hpp` (без изменения интерфейса);
* `queue.hpp`;
* `pipeline.hpp`;
* `metrics.hpp`.

Допускается добавление вспомогательных типов **внутри этих файлов**, при условии, что публичные интерфейсы из задания сохраняются.

---

# Часть 1. Блокирующая потокобезопасная очередь

## Файл: `queue.hpp`

Реализовать обобщенную блокирующую очередь для передачи данных между стадиями.

### Обязательный интерфейс

```cpp
template <typename T>
class BlockingQueue {
public:
    void push(T value);

    // Блокируется до появления элемента или закрытия очереди.
    // Возвращает false, если очередь закрыта и пуста.
    bool wait_pop(T& out);

    // Блокируется не более timeout.
    // Возвращает false при тайм-ауте или если очередь закрыта и пуста.
    template <class Rep, class Period>
    bool wait_pop_for(T& out,
        const std::chrono::duration<Rep, Period>& timeout);

    // Закрывает очередь: новые push запрещены,
    // все ожидающие потоки должны быть разбужены.
    void close();

    bool closed() const;
    bool empty() const;

private:
    std::queue<T> queue_;
    bool closed_ = false;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
};
```

### Обязательная семантика

* `push`:

  * потокобезопасен;
  * если очередь закрыта — бросает `std::logic_error`;
* `wait_pop`:

  * блокируется, пока:

    * не появится элемент, или
    * очередь не будет закрыта;
  * возвращает `true`, если элемент извлечен;
  * возвращает `false`, если очередь закрыта и пуста;
* `wait_pop_for`:

  * блокируется до элемента или истечения тайм-аута;
  * возвращает `false`, если тайм-аут истек **или** очередь закрыта и пуста;
* `close`:

  * идемпотентен;
  * пробуждает всех ожидающих потоков.

---

# Часть 2. Архитектура стадий

## Файл: `pipeline.hpp`

Pipeline состоит из **последовательных стадий**, соединенных очередями.

Минимально реализуются три стадии:

1. **Prepare**: `Accepted -> Prepared`
2. **Pack**: `Prepared -> Packed`
3. **Deliver**: `Packed -> Delivered`

Каждая стадия — **отдельный worker-поток**.

---

## Интерфейс Pipeline

```cpp
class Pipeline {
public:
    Pipeline();
    ~Pipeline();

    void start();
    void submit(Order order);
    void shutdown();

    Metrics metrics() const;
    const std::vector<Order>& delivered_orders() const;

private:
    BlockingQueue<Order> q_in_; // Входная очередь
    BlockingQueue<Order> q_prepare_; // Очередь между Prepare и Pack
    BlockingQueue<Order> q_pack_; // Очередь между Pack и Deliver

    std::jthread prepare_worker_; // Поток стадии Prepare
    std::jthread pack_worker_; // Поток стадии Pack
    std::jthread deliver_worker_; // Поток стадии Deliver

    std::vector<Order> delivered_;
    
    Metrics metrics_;
    mutable std::mutex metrics_mutex_; // Защита metrics_:

    bool started_ = false; // Флаг запуска pipeline
    bool shutdown_called_ = false; // Защита от повторного shutdown()
    std::mutex lifecycle_mutex_; // Защита started_ / shutdown_called_
};
```

---

## Семантика методов

### start()

* запускает worker-потоки стадий;
* повторный вызов либо запрещен (`logic_error`), либо идемпотентен (выберите и зафиксируйте).

### submit(order)

* потокобезопасен;
* добавляет заказ во входную очередь;
* увеличивает `accepted_count`.

### shutdown()

* запрещает новые `submit` (закрывает входную очередь);
* гарантирует, что:

  * все уже принятые заказы будут обработаны;
  * данные “доталкиваются” через все стадии;
* дожидается завершения всех worker-потоков;
* после возврата:

  * `metrics()` и `delivered_orders()` стабильны.

### ~Pipeline()

* не должен вызывать `std::terminate`;
* обязан корректно завершить pipeline, даже если `shutdown()` не был вызван явно.

---

# Часть 3. Тайм-ауты ожидания

Каждая стадия обязана использовать `wait_pop_for` хотя бы в одном месте своего рабочего цикла.

Цель:

* показать корректный шаблон ожидания;
* избежать “вечного блокирования”;
* корректно реагировать на закрытие очередей.

---

# Часть 4. Метрики

## Файл: `metrics.hpp`

```cpp
struct Metrics {
    std::uint64_t accepted_count = 0;
    std::uint64_t prepared_count = 0;
    std::uint64_t packed_count = 0;
    std::uint64_t delivered_count = 0;

    std::chrono::nanoseconds total_lead_time{0};

    std::uint64_t q_in_push = 0; // сколько заказов положили в q_in_ (submit)
    std::uint64_t q_in_pop = 0;   // сколько заказов извлекла стадия Prepare

    std::uint64_t q_prepare_push = 0;  // сколько заказов Prepare отправила дальше
    std::uint64_t q_prepare_pop = 0;   // сколько Pack реально получил

    std::uint64_t q_pack_push = 0; // сколько заказов Pack отправила дальше
    std::uint64_t q_pack_pop = 0; // сколько Deliver реально получил
};
```

### Инварианты после `shutdown()`

* `delivered_count <= packed_count <= prepared_count <= accepted_count`;
* для каждой очереди: `*_push == *_pop`;
* `delivered_orders().size() == delivered_count`;
* `total_lead_time` =
  сумма `(delivered_time - accepted_time)` по всем доставленным заказам.

---

# Часть 5. Корректность завершения

Завершение должно происходить **строго через закрытие очередей**:

* `shutdown()` закрывает входную очередь;
* каждая стадия:

  * читает вход до `false` от `wait_pop`;
  * закрывает свою выходную очередь;
* последняя стадия завершает работу, сохраняя доставленные заказы.

Запрещены:

* глобальные флаги завершения без очередей;
* принудительное прерывание потоков;
* пропуск элементов “в середине” конвейера.

---

# Часть 6. Что именно проверяется тестами

Тесты этапа 02 **обязательно проверяют**:

* корректную работу `BlockingQueue`:

  * блокирующее ожидание;
  * пробуждение при `close`;
  * корректную работу тайм-аутов;
* отсутствие busy-wait;
* корректную передачу заказов между стадиями;
* отсутствие потерь и дубликатов заказов;
* корректность всех счетчиков метрик;
* корректность `total_lead_time`;
* корректное завершение `shutdown()`:

  * без deadlock;
  * без зависших потоков;
  * без потери данных;
* стабильность `metrics()` и `delivered_orders()` после `shutdown()`.

---

## CLI

В связи с изменением публичного интерфейса конвейера необходимо заменить код в src/main.cpp для нашего мини-CLI.

```
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "order.hpp"
#include "pipeline.hpp"
#include "metrics.hpp"

int main(int argc, char* argv[]) {
    std::size_t orders_count = 500;

    if (argc == 2) {
        try {
            orders_count = static_cast<std::size_t>(std::stoull(argv[1]));
        }
        catch (...) {
            std::cerr << "Usage: ops_app [orders_count]\n";
            return 1;
        }
    }
    else if (argc > 2) {
        std::cerr << "Usage: ops_app [orders_count]\n";
        return 1;
    }

    Pipeline pipeline;
    pipeline.start();

    std::uint64_t next_id = 1;

    for (std::size_t i = 0; i < orders_count; ++i) {
        pipeline.submit(Order{ next_id++ });
    }

    pipeline.shutdown();

    const Metrics m = pipeline.metrics();
    const auto& delivered = pipeline.delivered_orders();

    std::cout << "Accepted:   " << m.accepted_count << "\n";
    std::cout << "Prepared:   " << m.prepared_count << "\n";
    std::cout << "Packed:     " << m.packed_count << "\n";
    std::cout << "Delivered:  " << m.delivered_count << "\n";
    std::cout << "Delivered vector size: " << delivered.size() << "\n";

    std::cout << "Queue in  push/pop:      " << m.q_in_push << "/" << m.q_in_pop << "\n";
    std::cout << "Queue prep push/pop:     " << m.q_prepare_push << "/" << m.q_prepare_pop << "\n";
    std::cout << "Queue pack push/pop:     " << m.q_pack_push << "/" << m.q_pack_pop << "\n";

    using namespace std::chrono;
    std::cout << "Total lead time (ms): "
              << duration_cast<milliseconds>(m.total_lead_time).count()
              << "\n";

    return 0;
}
```