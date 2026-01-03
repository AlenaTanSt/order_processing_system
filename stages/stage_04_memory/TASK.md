## Цель этапа

Доработать конвейер stage03 до "сервисной" модели, где:

* конвейерные потоки создаются один раз и переиспользуются;
* `submit()` становится потокобезопасным и может вызываться из разных потоков параллельно;
* ввод ограничен (bounded queues) и реализует backpressure без busy-wait;
* `process_all()` перестает быть "местом, где создаются потоки", и превращается в "барьер ожидания", который гарантирует, что все заказы, принятые до момента вызова, будут доставлены.

## Общие требования

* Использовать только стандартную библиотеку C++.
* Никакого busy-wait.
* Никакого `detach`.
* Все файлы строго в `solution/`.
* Тесты должны стабильно проходить при многократных запусках и под TSAN.

## Ограничения

### Разрешено

* `std::thread`
* `std::mutex`, `std::lock_guard`, `std::unique_lock`
* `std::condition_variable`
* `std::queue`, `std::deque`, `std::vector`, `std::optional`
* `std::chrono`
* `std::atomic`

### Запрещено

* `std::future`, `std::promise`, `std::async`
* busy-wait
* `detach`
* любые глобальные переменные для обмена между потоками

## Что необходимо реализовать

### 1) BoundedBlockingQueue

На базе `BlockingQueue` из stage03 сделать ограниченную по емкости очередь (сама BlockingQueue больше не понадобится):

```cpp
template <class T>
class BoundedBlockingQueue {
public:
    explicit BoundedBlockingQueue(std::size_t capacity);

    BoundedBlockingQueue(const BoundedBlockingQueue&) = delete;
    BoundedBlockingQueue& operator=(const BoundedBlockingQueue&) = delete;

    // Блокируется, пока:
    // - не появится место, или
    // - очередь не будет закрыта.
    // Возвращает false, если закрыта и элемент не принят.
    bool push_blocking(T value);

    // Блокируется, пока:
    // - не появится элемент, или
    // - очередь не будет закрыта.
    // Возвращает std::nullopt, если очередь закрыта и пуста.
    std::optional<T> pop_blocking();

    void close();

    std::size_t capacity() const noexcept;
};
```

Требования:

* `push_blocking()` обязан блокироваться при заполнении (backpressure).
* `close()` будит всех ждущих и переводит очередь в закрытое состояние.
* После `close()`:

  * `push_blocking()` всегда возвращает `false`;
  * `pop_blocking()` возвращает `nullopt`, когда очередь пуста.

### 2) Pipeline становится "долгоживущим"

В stage03 конвейерные потоки создавались внутри `process_all()`. В stage04:

* потоки стадий создаются при первом использовании `Pipeline` (рекомендуемо: лениво при первом `submit()` или первом `process_all()`), и живут до разрушения объекта `Pipeline`;
* `Pipeline::~Pipeline()` обязан корректно завершить все потоки (закрыть очереди, разбудить, join).

Архитектура стадий остается как в stage03 (Prepare -> Pack -> Deliver), но очереди теперь постоянные члены `Pipeline`, а не локальные переменные `process_all()`.

```
class Pipeline {
public:
    Pipeline();
    explicit Pipeline(std::size_t queue_capacity);

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;

    ~Pipeline();

    // Потокобезопасно. Может блокироваться из-за backpressure (bounded queue).
    void submit(Order order);

    // Барьер: дождаться, что все заказы, принятые до начала вызова, доставлены.
    void process_all();

    const Metrics& metrics() const noexcept;

    // Доставленные заказы в детерминированном порядке подачи (по seq).
    const std::vector<Order>& delivered_orders() const noexcept;
```

### 3) Потокобезопасный submit

`submit(Order order)` теперь:

* потокобезопасен;
* присваивает заказу монотонный "sequence number" (например, `std::uint64_t seq`), чтобы обеспечить детерминированный порядок результата при конкурентной подаче;
* увеличивает `accepted_count`;
* кладет заказ в входную очередь первой стадии (bounded).

Важно: входная "неблокирующая queue_" из stage01-03 больше не нужна в том же виде. Теперь вход - это bounded очередь стадий.

### 4) process_all как "барьер ожидания"

Семантика `process_all()`:

* она должна дождаться, что **все заказы, которые были успешно приняты submit() до начала вызова process_all()**, будут доведены до Delivered и добавлены в `delivered_`;
* после этого `process_all()` возвращает управление.

Подсказка по реализации без изменения публичного интерфейса:

* хранить глобальный счетчик "accepted_seq" и "delivered_seq" (или "delivered_count") и condition_variable для ожидания прогресса;
* `process_all()` фиксирует "target" на старте и ждет, пока delivered догонит target.

### 5) Детерминированный порядок delivered_orders()

Требование жесткое: `delivered_orders()` должен быть строго в порядке подачи (по sequence number), даже если submit конкурентный.

Рекомендация:

* передавать через очереди `IndexedOrder { seq, order }`;
* финальная стадия кладет результаты во временное хранилище (например, `std::map/вектор-буфер` по seq) и "выпускает" их в `delivered_` строго по возрастанию seq;
* допускается "буферизация дыр": если пришел seq=10, а seq=9 еще не доставлен, держим 10 в буфере.

### 6) Метрики

Метрики теперь обновляются в многопоточном режиме, поэтому:

* счетчики (`accepted_count`, `processed_count`, `delivered_count`) должны быть потокобезопасны;
* `total_processing_time` тоже должен обновляться безопасно.

Разрешено:

* либо `std::atomic` для счетчиков и отдельная защита для времени,
* либо полный `mutex`-guard для всей структуры `Metrics`.

```
struct Metrics {
    std::atomic<std::size_t> accepted_count{ 0 };
    std::atomic<std::size_t> processed_count{ 0 };
    std::atomic<std::size_t> delivered_count{ 0 };

    std::chrono::steady_clock::duration total_processing_time{};

    // Потокобезопасное добавление времени.
    void add_processing_time(std::chrono::steady_clock::duration d) {
        // ваша реализация
    }

private:
    mutable std::mutex time_m_;
};
```

Важно: поведение "накопительное" сохраняется (как в прошлых этапах).

## Инварианты корректности

Для каждого заказа:

* `accepted_time <= prepared_time <= packed_time <= delivered_time`
* финальный статус Delivered.

## Критерии приемки

Этап зачтен, если:

* нет busy-wait;
* нет `detach`;
* `submit()` потокобезопасен;
* `process_all()` корректно ждет "срез" принятых заказов;
* порядок delivered детерминирован при конкурентном submit;
* тесты стабильны.