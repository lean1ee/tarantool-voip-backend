<!-- autopilot:start -->
# Tarantool Backend для Kamailio, OpenSIPS и RTPEngine

Высокопроизводительный In-Memory бэкенд на базе **Tarantool 3.x** для синхронизации состояний медиа-сессий **RTPEngine** и SIP-сигнализации **Kamailio** и **OpenSIPS**.

## Архитектура решения

1. **`tarantool_backend/` (Tarantool 3.x Lua Server)**
   - `app/schema.lua`: Определение спейсов `rtpe_calls`, `kam_dialogs`, `kam_usrloc`, `cluster_nodes` с индексами (primary, by_node, by_expire).
   - `app/rtpe_service.lua`: Хранимые процедуры управления вызовами (`call_upsert`, `call_delete`, `call_restore`, `select_optimal_node`).
   - `app/ttl_worker.lua`: Фоновый файбер периодической очистки устаревших сессий.
   - `init.lua`: Точка входа сервера Tarantool.

2. **`rtpengine_tarantool/` (C Драйвер для RTPEngine Daemon)**
   - `include/tarantool.h`: C API для интеграции в RTPEngine (`rtpe_tarantool_save_call`, `rtpe_tarantool_delete_call`, `rtpe_tarantool_restore`).
   - `include/msgpuck.h`: Легковесный MessagePack кодировщик/декодировщик без внешних зависимостей.
   - `src/tarantool.c`: Неблокирующий IProto клиент с поддержкой `libevent`, Greeting handshake, аутентификации и асинхронного сохранения сессий.

3. **`kamailio_tarantool/` (Модуль для Kamailio)**
   - `ndb_tarantool/`: Пул асинхронных соединений и KEMI-биндинги (`sr_kemi_tarantool_call`, `sr_kemi_tarantool_eval`) для вызова процедур Tarantool из SIP-скриптов.

4. **`opensips_tarantool/` (Модуль для OpenSIPS)**
   - `cachedb_tarantool/`: Драйвер интерфейса `cachedb_funcs_t` для OpenSIPS 3.x с поддержкой `tarantool_call`, `tarantool_eval`, TCP keepalive и пула соединений.

5. **`asterisk_tarantool/` (Комплект модулей для Asterisk 20/22/master)**
   - `res/res_tarantool.c`: Core IProto пул соединений, TCP keepalive, auto-reconnect и CLI команды.
   - `res/res_config_tarantool.c`: Realtime-движок `ast_config_engine` (Sorcery, PJSIP объекты, статические .conf).
   - `funcs/func_tarantool.c`: Функции диалплана `${TARANTOOL(...)}` и `${TARANTOOL_EVAL(...)}`.
   - `cdr/cdr_tarantool.c`: Неблокирующий высокоскоростной логгер CDR в потоковый WAL.

6. **`docker/` и `docker-compose.yml` (Тестовый полигон Docker)**
   - `docker/Dockerfile.rtpengine`: Образ RTPEngine с драйвером Tarantool (`lean1ee/rtpengine`).
   - `docker/Dockerfile.kamailio`: Образ Kamailio с модулем ndb_tarantool (`lean1ee/kamailio`).
   - `docker/Dockerfile.opensips`: Образ OpenSIPS с модулем cachedb_tarantool (`lean1ee/opensips`).
   - `docker/Dockerfile.asterisk`: Образ Asterisk с модулями Tarantool (`lean1ee/asterisk`).
   - `docker-compose.yml`: Топология кластера (Tarantool 3.x + Redis 8.x + RTPEngine + Kamailio + OpenSIPS + Asterisk).

6. **`tests/` (Интеграционный и бенчмарк стенд)**
   - `tests/run_full_matrix_benchmark.py`: Сравнительный матричный бенчмарк всех 4 связок (Kamailio / OpenSIPS + Tarantool / Redis).
   - `tests/test_all_stacks.py`: Сквозной тест SIP сигнализации и медиа.
   - `tests/run_e2e_docker.py`: Сквозной тест живого Docker кластера.
   - `tests/mock_tarantool_server.py`: Автономный эмулятор IProto сервера Tarantool.
   - `tests/test_tarantool_backend.py`: Модульные тесты логики бэкенда и TTL.

## Команды управления

| Команда | Описание |
|---------|----------|
| `docker compose up -d` | Запуск полного кластера (Tarantool + RTPEngine + Kamailio + OpenSIPS) |
| `python tests/run_full_matrix_benchmark.py` | Запуск полного матричного бенчмарка (все 4 связки) |
| `python tests/test_all_stacks.py` | Сквозное тестирование SIPp для Kamailio и OpenSIPS |
| `python -m unittest discover -s tests` | Запуск полного набора всех 7 модульных тестов |
| `docker compose logs -f` | Просмотр логов работы кластера в реальном времени |
| `docker compose down` | Остановка кластера |

## Как здесь работает Autopilot

Сборка ведётся навыком `/autopilot`. Требования, спецификация и таски — в `.autopilot/`.
Прогресс — `.autopilot/dashboard.html`.
<!-- autopilot:end -->
