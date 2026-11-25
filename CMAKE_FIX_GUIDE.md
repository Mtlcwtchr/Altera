## ИСПРАВЛЕНИЕ ОШИБКИ CMake: Ninja vs Visual Studio

### Проблема
Ошибка: "Error: generator : Visual Studio 17 2022 Does not match the generator used previously: Ninja"

**Причина:** Старые build директории содержали кеш с Ninja генератором, а CLion пытался использовать Visual Studio генератор.

---

### ✅ Решение - Что уже сделано:

1. ✓ Удалены ВСЕ старые `cmake-build-*` директории
2. ✓ Обновлена конфигурация CLion (`.idea/cmake.xml`)
3. ✓ Создан `CMakeUserPresets.json` с Visual Studio конфигурацией
4. ✓ Создан финальный build скрипт: `build_msvc_final.bat`

---

### 📋 ВАРИАНТ 1: Быстрая сборка из командной строки

Просто запусти скрипт:
```powershell
D:\AlteraGame\altera\build_msvc_final.bat
```

Этот скрипт:
1. Настроит Visual Studio 2022 окружение
2. Сконфигурирует CMake с Visual Studio 17 2022 генератором
3. Соберет lr_client
4. Покажет путь к готовому бинарику

---

### 📋 ВАРИАНТ 2: Сборка через CLion IDE

1. **Перезагрузи проект CMake в CLion:**
   - Меню: Tools → CMake → Reload CMake Project
   - ИЛИ: Нажми на кнопку перезагрузки в окне CMake

2. **Выбери конфигурацию:**
   - В левом углу CLion (где выбирается конфигурация) выбери: **msvc-relwithdebinfo**
   - Если вместо этого видишь "RelWithDebInfo", значит кеш не очищен

3. **Собери проект:**
   - Меню: Build → Build Project
   - ИЛИ: Ctrl+F9

4. **Или собери конкретный таргет:**
   - Меню: Build → Build → lr_client
   - ИЛИ: Кликни правой кнопкой на target в CMake проекте и выбери "Build"

---

### 🔍 Проверка

После успешной сборки бинарник будет здесь:
```
D:\AlteraGame\altera\cmake-build-msvc-relwithdebinfo\RelWithDebInfo\lr_client.exe
```

Тест:
```powershell
D:\AlteraGame\altera\cmake-build-msvc-relwithdebinfo\RelWithDebInfo\lr_client.exe --help
```

---

### ⚠️ Если по-прежнему ошибка

1. **Убедись что все directori удалены:**
   ```powershell
   Get-ChildItem D:\AlteraGame\altera -Directory -Filter "cmake-build-*"
   ```
   
   Не должно быть никаких результатов!

2. **Очисти кеш CMake в CLion:**
   - File → Settings → Build, Execution, Deployment → CMake
   - Удали все профили и переключись на "msvc-relwithdebinfo" (должен создаться автоматически)

3. **Если "msvc-relwithdebinfo" не показывается:**
   - Перезагрузи CLion полностью
   - File → Invalidate Caches → Invalidate and Restart

---

### 📝 Почему Visual Studio вместо MinGW?

- MinGW не имеет полных Windows SDK заголовков
- DirectXShaderCompiler требует типы: `BOOL`, `HANDLE`, `DWORD`, `HWND` и т.д.
- Эти типы определены только в Windows SDK, который идет с Visual Studio
- MSVC компилятор + Visual Studio генератор CMake = правильная конфигурация для Windows

---

### 🚀 Быстрый старт

```powershell
# Вариант A - Командная строка (самый надежный)
D:\AlteraGame\altera\build_msvc_final.bat

# Вариант B - CLion
# 1. Tools → CMake → Reload CMake Project
# 2. Ctrl+F9 (Build Project)
```

**После этого всё должно работать!**

