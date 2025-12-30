## Из директории этапа:
### Сборка проекта
```bash
cmake -S . -B build -DENABLE_TSAN=ON
cmake --build build
```

### Запуск тестов
```bash
ctest --test-dir build
```

## Или удобнее:
```bash
cmake --build build --target check

```