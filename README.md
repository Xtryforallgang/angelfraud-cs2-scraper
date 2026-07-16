# Angelfraud Scraper 

**Angelfraud Scraper** — лёгкий C++ оверлей для *CS2*, который показывает суммарную стоимость инвентаря всех игроков в матче через API панели.

**👨‍💻 Разработчик:** [@try0raw](https://t.me/try0raw) — по вопросам и предложениям

---

## ☕ Поддержать проект

USDT (TRC20): `TGXz2uMr3YxPQi26xo22W129WS9QMq9LZP`

Спасибо всем, кто поддерживает разработку! ❤️

---

## ✨ Возможности

- **Прозрачный оверлей** поверх CS2 (DirectX 11 + Dear ImGui)
- **Автоматическое извлечение PHPSESSID** из всех популярных браузеров (Firefox, Chrome, Edge, Brave, Opera, Vivaldi, Chromium, Yandex) — не нужно вводить вручную
- **Аппаратная AES-GCM дешифровка** для Chrome v80+ (Windows BCrypt) + DPAPI fallback для старых версий
- **Извлечение cf_clearance** из Chrome-браузеров для обхода Cloudflare Turnstile
- SteamID игроков из матча через **GSI allplayers**
- **Суммарная цена инвентаря** через `POST /core` — зелёным `$XX.XX` или статус: PRIVATE, EMPTY, BANNED
- **Избранное** — добавляйте игроков в список избранного с заметками
- **Мгновенное обновление** при добавлении/удалении из избранного
- Окно оверлея можно **перетаскивать** и **изменять размер**
- **GSI diagnostic** — встроенная диагностика подключения

---

## ⚠️ Дисклеймер

Этот проект является независимой разработкой, не связанной с Valve Corporation. Инструмент не модифицирует память игры, не внедряется в процессы и использует только официальные интерфейсы CS2 (GSI) и публичное API сайта. Используйте ответственно.

---

## 🛠️ Установка и сборка

### Требования
- **Visual Studio 2022** с C++ Desktop Development workload
- **Windows 10/11** (x64)
- **CS2** (Counter-Strike 2)

### Сборка
1. Открой `ErScripts.sln` в Visual Studio 2022
2. Выбери конфигурацию **Release x64**
3. Нажми **Build Solution** (F7)
4. Готовый `.exe` будет в `ErScripts/x64/Release/Angelfraud.exe` — без консольного окна

### Запуск
1. Запусти `Angelfraud.exe` **от имени администратора** (требуется для оверлея)
2. Программа сама найдёт установку CS2, создаст GSI конфиг и окно оверлея
3. PHPSESSID будет автоматически извлечён из любого установленного браузера
4. **INSERT** — показать/скрыть меню оверлея
5. **END** — выход

> **Важно**: Перед первым запуском залогиньтесь на сайте панели в браузере, чтобы кука PHPSESSID появилась.

---

## ⚙️ Конфигурация

Файл: `ErScripts/configs/default.json` (создаётся автоматически при первом запуске)

```json
{
    "overlay": {
        "fps": 120,
        "vsync": false,
        "menuBind": "VK_INSERT",
        "posX": 100,
        "posY": 100,
        "width": 400,
        "height": 500
    },
    "angelfraud": {
        "phpsessid": "ваш_phpsessid_тут",
        "cacheTtlSeconds": 300,
        "autoExtractCookies": true
    }
}
```

---

## 🧠 Как это работает

```
CS2  ──(GSI http :23561)──▶  GSIServer
                                │
                                ▼
                         Парсинг allplayers
                         (SteamID, имя, observer_slot)
                                │
                                ▼
                       AngelfraudAPI (POST /core)
                                │
                                ▼
                      ┌─────────────────────┐
                      │ Имя игрока  │  Цена  │
                      │─────────────────────│
                      │ ★ Player1   │ $245.30│
                      │ Player2     │ PRIVATE│
                      │ Player3     │  $12.40│
                      └─────────────────────┘
                         (Dear ImGui overlay)
```

1. **CookieExtractor** — при старте сканирует все браузеры (Firefox, Chrome, Edge, Brave, Opera, Vivaldi, Chromium, Yandex) в поисках `PHPSESSID` и `cf_clearance` для API панели. Использует AES-GCM (BCrypt) для новых Chrome и DPAPI для старых.
2. **GSI Server** (`GSIServer.cpp`) — ловит JSON-пакеты от CS2 на порту 23561. Извлекает `allplayers` — всех игроков в матче с их SteamID, именами, аватарами.
3. **AngelfraudAPI** (`AngelfraudAPI.cpp`) — в фоновом потоке ходит в `POST /core` с `steam_input=<SteamID64>`, получает `total_value`. Кэширует результат на 5 минут.
4. **Overlay** (`Overlay.cpp`) — прозрачное окно поверх CS2 на DirectX 11 с Dear ImGui. Рисует таблицу игроков и цен, окно избранного.

---

## 🎮 Управление

| Клавиша | Действие |
|---------|----------|
| **INSERT** | Открыть/закрыть меню оверлея |
| **END** | Выход из программы |
| **Мышь (drag)** | Перетаскивание окна (по заголовку) |
| **★ / ☆** | Добавить/удалить игрока в избранное |

---

## 🏗️ Структура проекта

```
ErScripts/
├── main.cpp                 # Точка входа (Windows subsystem)
├── Config.h/.cpp            # Чтение/запись JSON конфига
├── Globals.h/.cpp           # MatchPlayer структура, глобалы
├── Logger.h/.cpp            # Логи с ANSI-цветами
├── SteamTools.h/.cpp        # Поиск CS2 через Steam
├── GSIServer.h/.cpp         # GSI HTTP сервер (:23561)
├── AngelfraudAPI.h/.cpp     # HTTP клиент (WinHTTP) для /core
├── CookieExtractor.h/.cpp   # Извлечение кук из всех браузеров
├── Overlay.h/.cpp           # DX11/ImGui оверлей
├── OverlayHelper.cpp        # RenderText
├── imgui/                   # Dear ImGui (v1.91.8)
├── resource.h, ErScripts.rc # Ресурсы
└── erscripts.ico            # Иконка
```

---

## 🛡️ Зависимости

| Компонент | Назначение |
|-----------|------------|
| **[Dear ImGui](https://github.com/ocornut/imgui)** | Отрисовка UI (окна, таблицы, текст) |
| **[nlohmann/json](https://github.com/nlohmann/json)** | Парсинг GSI JSON и конфига |
| **[cpp-httplib](https://github.com/yhirose/cpp-httplib)** | HTTP сервер для GSI |
| **WinHTTP** | HTTPS запросы к API панели |
| **DirectX 11 / DXGI / DWM API** | Оверлей поверх CS2 |
| **Crypt32 / BCrypt (CNG)** | DPAPI + AES-GCM дешифровка кук Chrome |

---

## 🔐 Поддержка браузеров

| Браузер | Метод дешифровки | Статус |
|---------|-----------------|--------|
| Firefox | Прямое чтение `cookies.sqlite` | ✅ |
| Chrome (v80+) | AES-GCM (BCrypt) | ✅ |
| Chrome (старые) | DPAPI (CryptUnprotectData) | ✅ |
| Edge | AES-GCM | ✅ |
| Brave | AES-GCM | ✅ |
| Opera | AES-GCM | ✅ |
| Vivaldi | AES-GCM | ✅ |
| Chromium | AES-GCM | ✅ |
| Yandex Browser | AES-GCM | ✅ |

---

## 📜 Лицензия

MIT.
