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

## Запуск CLI
```bash
.\build\solution\ops_app.exe <необязательный целочисленный аргумент - количество заказов>
```