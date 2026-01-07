## Из директории этапа:
### Сборка проекта
```bash
cmake -S . -B build -DENABLE_TSAN=ON
cmake --build build
```

### Запуск тестов
```bash
ctest --test-dir build --output-on-failure --verbose
```

### Если тесты падают, но нет информации об упавшем тесте
```
.\build\tests\ops_tests.exe
```

## Запуск CLI
```bash
.\build\solution\ops_app.exe <необязательный целочисленный аргумент - количество заказов>
```