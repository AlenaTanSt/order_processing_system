# Этап 00: Последовательная обработка заказов

## Цель этапа

Реализовать последовательную (однопоточную) систему обработки заказов. На этом этапе запрещено использовать потоки, мьютексы, атомики и любые средства конкурентности.

Этот этап является базовой точкой отсчета для курса. В последующих этапах этот код будет расширяться и распараллеливаться, поэтому важны корректность интерфейсов и детерминированность поведения.

## Общие требования

1. Вся обработка выполняется в одном потоке.
2. Запрещено использовать:

* `std::thread`, `std::async`
* `std::mutex`, `std::shared_mutex`, `std::condition_variable`
* `std::atomic`
* любые сторонние библиотеки конкурентности

3. Разрешено использовать только стандартную библиотеку C++.
4. Код должен быть:

* корректным,
* детерминированным,
* читаемым.

5. Все исходники и заголовки решения должны располагаться только в папке `solution/`.
6. Все тесты должны успешно проходить.

---

## Модель предметной области

### Статусы заказа

Заказ проходит следующие стадии строго по порядку:

1. `Accepted` - заказ принят
2. `Prepared` - заказ подготовлен
3. `Packed` - заказ упакован
4. `Delivered` - заказ доставлен (финальное состояние)

Никакие другие статусы на этапе 00 не используются.

### Заказ (Order)

Каждый заказ имеет:

* уникальный идентификатор: `std::uint64_t`;

* текущий статус;

* временные метки прохождения стадий (`std::chrono::steady_clock::time_point`), которые будут использоваться в следующих этапах:

* `accepted_time`

* `prepared_time`

* `packed_time`

* `delivered_time`

---

## Что необходимо реализовать

Все файлы располагаются в `solution/`:

* `order.hpp`
* `queue.hpp`
* `metrics.hpp`
* `pipeline.hpp`
* `pipeline.cpp`

---

## 1. `order.hpp`

### Требуемые сущности

1. Перечисление статусов:

```cpp
enum class OrderStatus {
    Accepted,
    Prepared,
    Packed,
    Delivered
};
```

2. Алиас идентификатора:

```cpp
using OrderId = std::uint64_t;
```

3. Класс `Order`:

```cpp
class Order {
public:
    explicit Order(OrderId id_);
    void advance_to(OrderStatus new_status);

public:
    OrderId id;
    OrderStatus status;

    std::chrono::steady_clock::time_point accepted_time;
    std::chrono::steady_clock::time_point prepared_time;
    std::chrono::steady_clock::time_point packed_time;
    std::chrono::steady_clock::time_point delivered_time;
};
```

### Требования к поведению

* Идентификаторы должны быть уникальными в рамках запуска программы.
* Начальное состояние каждого заказа - `OrderStatus::Accepted`.
* `accepted_time` должен быть установлен в конструкторе при создании заказа.
* `advance_to(new_status)` должен:

  * разрешать переходы только вперед по цепочке `Accepted -> Prepared -> Packed -> Delivered`;
  * устанавливать соответствующую временную метку стадии на `std::chrono::steady_clock::now()`;
  * не менять предыдущие временные метки;
  * если пытаются выполнить некорректный переход (например, назад или "перепрыгнуть" через стадию), поведение должно быть определенным и одинаковым во всех запусках - `throw std::logic_error` с кратким сообщением.

---

## 2. `queue.hpp`

Реализовать простую очередь заказов без потокобезопасности в классе `OrderQueue`.

### Минимальный интерфейс

```cpp
class OrderQueue {
public:
    void push(const Order& order);
    void push(Order&& order);

    Order pop();          // извлекает следующий заказ (FIFO)
    bool empty() const;
};
```

### Требования

* Очередь должна сохранять порядок (FIFO).
* `pop()` корректно извлекает первый элемент и удаляет его из очереди.
* Поведение `pop()` на пустой очереди должно быть определенным и детерминированным - `throw std::out_of_range`

Разрешено использовать `std::queue<Order>` или `std::deque<Order>`.

---

## 3. `metrics.hpp`

Реализовать простую систему метрик в виде структуры `Metrics`.

### Требуемые поля

```cpp
struct Metrics {
    std::size_t accepted_count = 0; // количество принятых заказов
    std::size_t delivered_count = 0; // количество доставленных заказов
    std::size_t processed_count = 0; // количество обработанных заказов

    std::chrono::steady_clock::duration total_processing_time{}; // общее время обработки
};
```

### Требования к обновлению

* `accepted_count` увеличивается при добавлении заказа в систему (`Pipeline::submit`).
* `processed_count` увеличивается при обработке заказа (один раз на заказ в `process_all`).
* `delivered_count` увеличивается, когда заказ достигает финального состояния `Delivered`.
* `total_processing_time` накапливает суммарное время обработки всех доставленных заказов:
  * добавляется `order.delivered_time - order.accepted_time` для каждого заказа, завершенного в `Delivered`.

---

## 4. `pipeline.hpp` / `pipeline.cpp`

Реализовать класс `Pipeline`, управляющий обработкой заказов.

### Обязанности Pipeline

* Принимать новые заказы.
* Последовательно проводить каждый заказ через все стадии.
* Завершать каждый заказ в состоянии `Delivered`.
* Обновлять метрики.

### Минимальный интерфейс

```cpp
class Pipeline {
public:
    void submit(Order order);
    void process_all();

    const Metrics& metrics() const;
    const std::vector<Order>& delivered_orders() const;

private:
    OrderQueue queue_;
    Metrics metrics_;
    std::vector<Order> delivered_;
};
```

### Требования к `submit`

* Добавляет заказ во внутреннюю очередь.
* Увеличивает `metrics_.accepted_count`.

### Требования к `process_all`

* Заказы обрабатываются строго последовательно.
* Для каждого заказа стадии выполняются строго в порядке:
  `Accepted -> Prepared -> Packed -> Delivered`
* Порядок заказов сохраняется.
* После достижения `Delivered` заказ добавляется в `delivered_`.
* Обновляются метрики:

  * `processed_count` - один раз на заказ
  * `delivered_count` - один раз на доставленный заказ
  * `total_processing_time` - добавляется `delivered_time - accepted_time`