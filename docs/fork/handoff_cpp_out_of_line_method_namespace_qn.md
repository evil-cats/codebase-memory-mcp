# Handoff: namespace владельца out-of-line метода C++ сохраняется в `qualified_name`

Статус: реализовано в рабочем дереве; целевые регрессии пройдены, общие проверки
имеют зафиксированные независимые блокирующие причины.
Дата фиксации дефекта: 2026-09-01.
Ветка: `v0.10.8-lora`.
Текущая upstream-база: тег `v0.10.8`, коммит
`46ae198fc11cda80e817acbc5f5908d7c2de7032`.
Текущий коммит ветки на момент диагностики:
`7e6ced39826e20ad0dcd00c028230dc00cae81ce`.
Источник ошибочной обработки out-of-line методов: upstream-коммит
`a50b086809be2127a221fd4696fd15e24716d3b1`
(`Attribute out-of-line C++ method definitions to their class (distill #428)`).

## Назначение

Исправить построение `qualified_name` и `parent_class` для C++ и CUDA методов,
определённых вне тела класса, но внутри `namespace`.

После исправления объявление класса, определение метода, сырые вызовы,
семантические записи и граф должны использовать один и тот же полный QN
владельца. В частности, `DEFINES_METHOD` должен связывать существующий узел
`Class` с существующим узлом `Method` точным равенством QN, без сопоставления по
суффиксу и других приблизительных восстановлений.

## Наблюдаемый дефект

Дефект подтверждён на полном индексе проекта
`/home/slader/Projects/evilcats/mediasniper/projects/dsp`.

Исходные файлы:

- `src/handlers/adcamp/AdCampRequest.h` объявляет класс внутри
  `namespace sniper::dsp::handlers`;
- `src/handlers/adcamp/AdCampRequest.cpp` определяет
  `AdcampRequest::parse(...)` внутри того же namespace.

Класс в графе:

```text
label: Class
name: AdcampRequest
qualified_name:
src.handlers.adcamp.AdCampRequest.sniper::dsp::handlers.AdcampRequest
```

Out-of-line определение `parse()` в том же графе:

```text
label: Method
name: parse
parent_class:
src.handlers.adcamp.AdCampRequest.AdcampRequest
qualified_name:
src.handlers.adcamp.AdCampRequest.AdcampRequest.parse(const::sniper::dsp::RawRequest&,proto::openrtb::BidRequest&) const
```

В `parent_class` и `qualified_name` метода отсутствует сегмент
`sniper::dsp::handlers`, хотя исходное определение находится внутри этого
namespace. Поэтому выполняются оба нежелательных условия:

```text
method.parent_class != class.qualified_name
method.qualified_name не начинается с class.qualified_name + "."
```

У класса остаётся только ребро к деструктору, извлечённому непосредственно из
тела класса:

```text
AdcampRequest -[:DEFINES_METHOD]-> ~AdcampRequest()
```

Ребра `DEFINES_METHOD` к `parse()` нет. Сам узел `Method` при этом существует;
это не потеря определения и не пробел покрытия синтаксического анализатора.

На снимке индекса от 2026-09-01 по укороченному шаблону были построены 90
методов `parse()` из `src/handlers`. Следовательно, дефект системный и не
ограничен `AdcampRequest`.

## Как выявить дефект на существующем индексе

Сначала проверить актуальность проекта через `index_status`. Если проект ещё не
индексирован, выполнить `index_repository`; если индекс существует, не запускать
переиндексацию только ради диагностики без отдельной необходимости.

Для файлов примера проверить полноту индекса через `check_index_coverage`:

```text
src/handlers/adcamp/AdCampRequest.h
src/handlers/adcamp/AdCampRequest.cpp
```

Оба файла в подтверждённом снимке не имели записанных `parse_partial` или
`skipped`-пробелов.

Через `search_graph` найти точные узлы `AdcampRequest`, `~AdcampRequest` и
`parse`, запросив поля `parent_class` и `signature`. Затем проверить рёбра:

```cypher
MATCH (c:Class {
  qualified_name: 'src.handlers.adcamp.AdCampRequest.sniper::dsp::handlers.AdcampRequest'
})
OPTIONAL MATCH (c)-[r:DEFINES_METHOD]->(m)
RETURN c.qualified_name, type(r), m.qualified_name, m.parent_class
ORDER BY m.qualified_name
```

До исправления запрос возвращает только деструктор. Отдельный поиск узла
`Method` с `file_path = 'src/handlers/adcamp/AdCampRequest.cpp'` и
`name = 'parse'` показывает сокращённый `parent_class`.

Реальный DSP-репозиторий служит доказательством масштаба, но не должен быть
обязательной зависимостью тестов этой доработки. Каноническое воспроизведение
должно жить в локальном детерминированном тестовом примере.

## Минимальное локальное воспроизведение

Добавить тестовый пример C++ с классом и out-of-line методом внутри лексического
namespace:

```cpp
namespace sniper::dsp::handlers {

bool enabled = true;
int helper();

class AdcampRequest {
public:
    bool parse() const;
};

bool AdcampRequest::parse() const {
    return enabled && helper() != 0;
}

} // namespace sniper::dsp::handlers
```

На неисправной реализации класс получает QN с namespace, а метод после
специальной обработки узла `qualified_identifier`, найденного в поле
`declarator`, получает QN только с `AdcampRequest`. Тест должен падать на точном
сравнении `parent_class` с QN класса, а не на косвенном подсчёте узлов.

## Первопричина

### Правильный namespace сначала присутствует

`walk_defs()` в `internal/cbm/extract_defs.c` распознаёт C++ namespace как область
видимости, вычисляет расширенный `enclosing_class_qn` и передаёт его дочерним
узлам.

`extract_func_def()` сначала использует `ctx->enclosing_class_qn` и строит для
функции внутри namespace правильную основу QN.

### Out-of-line обработка затирает правильный владелец

Позже тот же `extract_func_def()` вызывает
`cbm_cpp_out_of_line_parent_class()` и безусловно пересобирает `class_qn` через
`cbm_fqn_compute(rel_path, scope_name)`.

`cbm_cpp_out_of_line_parent_class()` в `internal/cbm/extract_defs.c` намеренно
спускается по `qualified_identifier` или `scoped_identifier` до самого
внутреннего `name`. Для `AdcampRequest::parse` он возвращает только
`AdcampRequest`; для `ns::Foo::bar` внешний `ns` также отбрасывается.

В результате уже известный лексический namespace не дополняется именем класса,
а полностью заменяется QN, построенным из пути файла и короткого имени класса.

### Три пути расходятся по-разному

Исправление только `extract_func_def()` недостаточно. Тот же владелец строится в
трёх местах:

1. `extract_func_def()` в `internal/cbm/extract_defs.c` создаёт узел `Method`,
   его `qualified_name` и `parent_class`.
2. `compute_func_qn()` в `internal/cbm/extract_unified.c` создаёт
   `enclosing_func_qn` для сырых вызовов. Сейчас его out-of-line ветка также
   строит владельца из короткого `scope_name`, игнорируя
   `state->enclosing_class_qn`.
3. `cbm_enclosing_func_qn()` в `internal/cbm/helpers.c` создаёт владельца для
   `CBMUsage`, чтений и записей, исключений, назначений типов и других
   семантических записей. Для метода внутри namespace общий обход предков может
   построить `namespace.parse`, после чего out-of-line ветка уже не выполняется;
   имя класса исчезает другим способом.

Все три пути должны выдавать один и тот же QN. Нельзя считать правильным
исправление, после которого появляется `DEFINES_METHOD`, но вызовы или
использования продолжают ссылаться на другой `enclosing_func_qn`.

### Создание ребра работает по точному контракту

`process_def()` в `src/pipeline/pass_definitions.c` и
`register_and_link_def()` в `src/pipeline/pass_parallel.c` ищут владельца через
точный `cbm_gbuf_find_by_qn(def->parent_class)`. Если такого QN нет, ребро не
создаётся.

Эту часть менять не нужно. Приблизительный поиск на стороне графа замаскирует
ошибку идентичности и создаст риск связать одноимённые классы из разных
namespace.

## Контракт после исправления

Для каждого C++ или CUDA out-of-line метода должны выполняться следующие
инварианты:

1. `method.parent_class` точно равен `class.qualified_name`.
2. `method.qualified_name` начинается с
   `class.qualified_name + "."` и заканчивается существующей канонической
   сигнатурой C++.
3. `CBMCall.enclosing_func_qn` внутри метода точно равен
   `method.qualified_name`.
4. `CBMUsage.enclosing_func_qn`, `CBMReadWrite.enclosing_func_qn` и другие
   записи, которые тестовый пример действительно создаёт внутри метода, точно
   равны `method.qualified_name`.
5. В полном графе существует ровно одно ожидаемое ребро
   `Class -[:DEFINES_METHOD]-> Method`.
6. Свободная функция внутри namespace не превращается в `Method`.
7. Существующие out-of-line методы без namespace, конструкторы, деструкторы,
   операторы, шаблоны и перегрузки не теряют прежнюю каноническую идентичность.

Для примера DSP ожидается:

<!-- markdownlint-disable MD013 -->

```text
method.parent_class =
src.handlers.adcamp.AdCampRequest.sniper::dsp::handlers.AdcampRequest

method.qualified_name =
src.handlers.adcamp.AdCampRequest.sniper::dsp::handlers.AdcampRequest.parse(const::sniper::dsp::RawRequest&,proto::openrtb::BidRequest&) const
```

<!-- markdownlint-enable MD013 -->

## Требуемое устройство исправления

### Единый конструктор QN владельца

Вынести построение полного владельца out-of-line C++/CUDA метода в одну общую
функцию, доступную всем трём потребителям. Имя и точная сигнатура функции могут
следовать стилю текущего кода, но её контракт должен принимать или однозначно
восстанавливать:

- `function_definition` и его квалифицированный узел в поле `declarator`;
- относительный путь файла;
- исходный текст;
- уже вычисленный QN лексической области, если определение находится внутри
  namespace.

Общая функция возвращает полный QN класса-владельца либо `NULL`, если узел не
является out-of-line методом. Она не должна возвращать только последний
идентификатор области.

Общая функция должна:

1. Извлечь всю часть владельца из поля `declarator` перед именем метода.
2. Отделить класс от явных квалификаторов namespace или вложенных классов, не
   выбрасывая их.
3. Для `namespace ns { R C::m(); }` дополнить лексический QN сегментом `C`.
4. Для определения с явным полным квалификатором на уровне файла сохранить весь
   `ns::C`, если грамматика tree-sitter разобрала конструкцию без `ERROR`.
5. Не добавлять путь модуля повторно, если лексический QN уже является полным.
6. Использовать то же представление компактной записи `namespace`, которое
   создают `compute_class_qn()` и `extract_class_def()`; не вводить второй формат
   QN.

Если текущая грамматика tree-sitter не позволяет надёжно изолировать вариант с
полным `ns::C::m` на уровне файла, это не разрешает снова отбрасывать
квалификатор. Минимально обязательный сценарий этой карточки — реальная форма
DSP: лексический `namespace sniper::dsp::handlers` плюс
`AdcampRequest::parse`.

### Подключение общего результата

- В `extract_func_def()` использовать общий QN владельца для
  `def.parent_class` и основы `def.qualified_name` до применения
  `cbm_cpp_callable_identity()`.
- В `compute_func_qn()` использовать тот же QN владельца до применения
  `cbm_cpp_callable_qualified_name()`.
- В `cbm_enclosing_func_qn()` распознавать out-of-line метод до общей резервной
  ветки `namespace + function`; передавать общей функции лексический namespace и
  получать тот же QN владельца.
- Обновить декларации в `internal/cbm/helpers.h`, если общая функция переносится
  или меняет контракт.
- Удалить или сузить старый
  `cbm_cpp_out_of_line_parent_class()`, чтобы рядом не осталось второго
  независимого алгоритма построения владельца.

### Недопустимые обходы

Не исправлять дефект следующими способами:

- сопоставление по суффиксу или поиск класса только по `name` при создании
  `DEFINES_METHOD`;
- удаление namespace из QN класса;
- создание alias-узла класса с укороченным QN;
- изменение только `def.parent_class` без согласования полного QN метода и
  `enclosing_func_qn`;
- проверки через `strstr(..., "AdcampRequest")`, которые проходят и на
  укороченном значении;
- специальное правило для каталога `src/handlers` или проекта DSP.

## Обязательные регрессионные тесты

### Точное извлечение определений и семантических записей

В `tests/test_extraction.c` добавить зарегистрированный тест
`cpp_out_of_line_method_preserves_lexical_namespace` на минимальном тестовом
примере из этой карточки.

Тест должен:

1. Проверить отсутствие `has_error` и `parse_incomplete`.
2. Найти единственные ожидаемые `Class` с именем `AdcampRequest` и `Method` с
   именем `parse`.
3. Проверить точное равенство `method->parent_class` и
   `class->qualified_name`.
4. Проверить, что остаток `method->qualified_name` после полного QN класса точно
   соответствует `.parse() const`, а не просто содержит имя класса.
5. Найти вызов `helper()` и проверить точное равенство его
   `enclosing_func_qn` с QN метода. Это защищает путь `compute_func_qn()`.
6. Найти обычную запись `CBMUsage` для `enabled` и проверить точное равенство её
   `enclosing_func_qn` с QN метода. Это защищает путь
   `cbm_enclosing_func_qn()`.
7. Зарегистрировать тест через `RUN_TEST` в тестовом наборе `extraction`.

Если извлекатель по действующему контракту не создаёт `CBMUsage` для `enabled`,
заменить только эту запись на другую детерминированно создаваемую запись,
использующую `cbm_enclosing_func_qn()`, либо добавить прямой модульный тест этой
общей функции. Нельзя молча оставить третий путь без проверки.

Существующий
`repro_issue554_cpp_out_of_line_method_class_qualified` в
`tests/repro/repro_extraction.c` не заменяет новый тест: его тестовый пример не
содержит namespace, а текущие проверки допускают
`qualified_name OR parent_class` и ищут только подстроку `Foo`.

### Точное ребро полного конвейера

В `tests/test_edge_structural.c` добавить зарегистрированный тест
`es_defines_method_cpp_out_of_line_namespace` с двумя файлами:

- `AdCampRequest.h` объявляет класс и `parse()` внутри
  `namespace sniper::dsp::handlers`;
- `AdCampRequest.cpp` определяет `AdcampRequest::parse()` внутри того же
  namespace.

Использовать существующий тестовый контур, запускающий рабочий конвейер
индексации, и `es_exact_edge_by_name()`. Проверка должна требовать ровно одно
ребро:

```text
AdcampRequest -[:DEFINES_METHOD]-> parse
```

Недостаточно проверить только общее количество рёбер `DEFINES_METHOD`: его может
удовлетворить деструктор или другой метод. Тест нужно зарегистрировать через
`RUN_TEST` в тестовом наборе `edge_structural`.

### Сохранение прежнего поведения

Существующие тесты должны продолжить покрывать:

- `void Foo::bar()` без namespace;
- привязку вызовов к владельцу внутри `Foo::bar()`;
- out-of-line конструктор и деструктор;
- каноническую идентичность перегрузок C++, cv/ref-квалификаторы, шаблоны и
  `requires`;
- согласованность последовательного и параллельного путей
  `DEFINES_METHOD`.

## Проверка реализации

Канонический быстрый маршрут проекта для тестов:

```bash
scripts/test.sh --suites "extraction edge_structural c_lsp pipeline parallel incremental"
```

После узких тестов выполнить каноническую проверку линтерами:

```bash
scripts/lint.sh --ci
```

Перед признанием доработки готовой выполнить полную обязательную проверку:

```bash
scripts/test.sh
```

Проверки документа и изменений:

```bash
markdownlint-cli2 --config "$HOME/.codex/.markdownlint-cli2.yaml" \
  docs/fork/handoff_cpp_out_of_line_method_namespace_qn.md
git diff --check
```

Если обязательная команда недоступна или падает по не относящейся к изменению
причине, зафиксировать точную команду, код выхода и оставшийся риск. Не заменять
каноническую обёртку произвольным ручным запуском тестового бинарника.

## Результат реализации от 2026-09-01

### Выполненные изменения

- Общая функция `cbm_cpp_out_of_line_owner_qn()` сохраняет полный владелец из
  квалифицированного `declarator` и дополняет уже вычисленный лексический
  namespace без повторного добавления QN модуля.
- Один результат используется в `extract_func_def()`, `compute_func_qn()` и
  `cbm_enclosing_func_qn()`. Старый отдельный
  `cbm_cpp_out_of_line_parent_class()` удалён.
- `parent_class`, канонический QN метода, QN сырых вызовов и QN семантических
  записей теперь строятся из одного владельца.
- Точное связывание `DEFINES_METHOD` можно безопасно повторить после
  материализации всех определений; приблизительный поиск по имени или суффиксу
  не добавлялся.
- Добавлены точные регрессии
  `cpp_out_of_line_method_preserves_lexical_namespace` и
  `es_defines_method_cpp_out_of_line_namespace`.

### Полученные результаты проверок

- Исходный красный запуск
  `scripts/test.sh --suites "extraction edge_structural"` завершился с кодом `1`
  на новых проверках до исправления.
- Канонический объединённый маршрут
  `scripts/test.sh --suites "extraction edge_types_probe edge_structural c_lsp go_lsp rust_lsp pipeline parallel incremental"`
  завершился с кодом `0`: `2202 passed`.
- `git diff --check` завершился с кодом `0`.
- Markdown-lint этой карточки завершился с кодом `0`, ошибок нет.
- `scripts/lint.sh --ci` завершился с кодом `2`: `cppcheck` не разобрал три
  существующих выражения с `(TSNode){0}` в
  `internal/cbm/extract_defs.c:3027`, `internal/cbm/extract_calls.c:2694` и
  `internal/cbm/extract_unified.c:1779`. Эти строки не входят в изменённые
  diff-hunks; оставшийся риск — общий lint-gate проекта не зелёный.
- Независимые ветки `make -f Makefile.cbm lint-format lint-no-suppress`
  завершились с кодом `0` после применения проектного `clang-format`.
- Полный `scripts/test.sh` завершился с кодом `2` до запуска основного набора:
  шаг `0o` требует отсутствующую утилиту `zip` для сборки
  `codebase-memory-mcp-darwin-arm64.mcpb`. Утилита не устанавливалась.
- Установка бинарника, перезапуск процессов и переиндексация DSP не выполнялись.

## Живая проверка после установки

Установка нового бинарника, перезапуск процесса или демона и переиндексация
реального DSP не входят автоматически в реализацию карточки и требуют отдельного
разрешения.

Если такая проверка разрешена, после установки требуется полная переиндексация
DSP. Инкрементальный проход по неизменившимся исходникам не гарантирует удаления
узлов со старым укороченным QN после обновления алгоритма идентичности.

После полной переиндексации проверить:

1. `parse.parent_class == AdcampRequest.qualified_name`.
2. `parse.qualified_name` начинается с полного QN класса.
3. У класса есть `DEFINES_METHOD` к `parse()` и деструктору.
4. Узел `Method` со старым QN без namespace отсутствует.
5. Вызовы и записи `CBMUsage` внутри `parse()` принадлежат новому полному QN
   метода.

## Критерии готовности

- Все три пути построения QN используют общий полный QN владельца.
- Новый тест набора `extraction` падает на исходном коде и проходит после
  исправления.
- Новый структурный тест доказывает точное ребро от класса к `parse()`.
- Существующие out-of-line регрессии и проверки перегрузок не сломаны.
- Узкие тестовые наборы, каноническая проверка линтерами, полный
  `scripts/test.sh`, Markdown-lint и `git diff --check` прошли либо для каждой
  непройденной обязательной проверки явно указана блокирующая причина.
- Исходники DSP не изменялись в рамках исправления индексатора.
- Установка, перезапуск и переиндексация не выполнялись без отдельного разрешения.

## Вне границ доработки

- Изменение формата namespace во всех C++ QN.
- Изменение схемы SQLite или уникального ключа `(project, qualified_name)`.
- Приблизительное разрешение `parent_class` по короткому имени.
- Общая задача связывания класса и реализации, если их разные относительные пути
  создают разные QN модуля; пример этой карточки использует одинаковую основу
  `AdCampRequest.h` и `AdCampRequest.cpp`.
- Изменение исходников `/home/slader/Projects/evilcats/mediasniper/projects/dsp`.
- Автоматическая установка бинарника, перезапуск демона, очистка кэша или
  переиндексация пользовательских проектов.

## Восстановление после обновления upstream

При следующем переносе сначала проверить, существует ли в upstream единый
конструктор полного QN владельца для out-of-line C++/CUDA методов и используют ли
его извлечение определений, область вызова и семантические записи.

Наличие только метки `Method` или только `parent_class`, содержащего короткое имя
класса, не считается эквивалентным исправлением. Эквивалент доказан лишь точными
инвариантами и `DEFINES_METHOD` из этой карточки.

## Откат

До установки исправление откатывается отменой изменений общей функции, трёх
потребителей и новых тестов. После установки и полной переиндексации откат
бинарника также требует полной переиндексации: старый и исправленный алгоритмы
создают разные QN для затронутых методов.
