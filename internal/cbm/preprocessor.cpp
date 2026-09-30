// Обёртка simplecpp: legacy-развёртка для дополнительного прохода и подготовка
// основного C++-представления с картой исходных строк. Ошибки и пустой успешный
// результат различаются; токены подключённых файлов не выдаются за основной файл.
// Реализация simplecpp включается в ту же единицу трансляции для CGo и Makefile.
#include "vendored/simplecpp/simplecpp.cpp"

#include "preprocessor.h"
#include "vendored/simplecpp/simplecpp.h"

#include <sstream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <climits>
#include <memory>
#include <cctype>

extern "C" {

static bool has_preprocessor_work(const char *source, int source_len) {
    if (!source || source_len <= 0) {
        return false;
    }
    for (int i = 0; i < source_len - 1; i++) {
        if (source[i] == '#') {
            // Skip whitespace after #
            int j = i + 1;
            while (j < source_len && (source[j] == ' ' || source[j] == '\t'))
                j++;
            int remaining = source_len - j;
            if (remaining >= 6 && strncmp(source + j, "define", 6) == 0) {
                return true;
            }
            if (remaining >= 5 && strncmp(source + j, "ifdef", 5) == 0) {
                return true;
            }
            if (remaining >= 6 && strncmp(source + j, "ifndef", 6) == 0) {
                return true;
            }
            if (remaining >= 3 && strncmp(source + j, "if ", 3) == 0) {
                return true;
            }
        }
    }
    return false;
}

static int count_expanded_lines(const std::string &text) {
    int count = 1;
    for (char c : text) {
        if (c == '\n') {
            count++;
        }
    }
    return count;
}

// Консервативная проверка до simplecpp: лексер может удалить короткий #line
// вместе с доказательством смены координат. Строка, похожая на такую директиву
// даже внутри многострочного текста, требует резерва, пока нет физической карты.
static bool has_source_line_control(const std::string &source) {
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line)) {
        size_t pos = line.find_first_not_of(" \t\r\v\f");
        if (pos == std::string::npos)
            continue;
        if (line[pos] == '#')
            ++pos;
        else if (line.compare(pos, 2, "%:") == 0)
            pos += 2;
        else
            continue;
        pos = line.find_first_not_of(" \t\r\v\f", pos);
        if (pos == std::string::npos)
            continue;
        if (std::isdigit((unsigned char)line[pos]) || line[pos] == '\\' || line[pos] == '/')
            return true;
        size_t end = pos;
        while (end < line.size() && (std::isalnum((unsigned char)line[end]) || line[end] == '_'))
            ++end;
        const std::string name = line.substr(pos, end - pos);
        if (name == "line" || name == "file" || name == "endfile" ||
            (end < line.size() && line[end] == '\\'))
            return true;
    }
    return false;
}

static bool parse_line_directive(const char *line, size_t len, uint32_t *out_line,
                                 std::string *out_file) {
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i >= len || line[i++] != '#') {
        return false;
    }
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    static const char prefix[] = "line";
    if (i + sizeof(prefix) - 1 > len || strncmp(line + i, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    i += sizeof(prefix) - 1;
    if (i >= len || (line[i] != ' ' && line[i] != '\t')) {
        return false;
    }
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i >= len || line[i] < '0' || line[i] > '9') {
        return false;
    }
    uint64_t parsed_line = 0;
    while (i < len && line[i] >= '0' && line[i] <= '9') {
        parsed_line = parsed_line * 10u + (uint64_t)(line[i] - '0');
        if (parsed_line > UINT32_MAX) {
            return false;
        }
        i++;
    }
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i >= len || line[i++] != '"') {
        return false;
    }
    size_t file_start = i;
    while (i < len && line[i] != '"') {
        i++;
    }
    if (i >= len) {
        return false;
    }
    *out_line = (uint32_t)parsed_line;
    *out_file = std::string(line + file_start, i - file_start);
    return true;
}

static bool build_line_map(const std::string &expanded, const std::string &main_file,
                           uint32_t *original_line_by_expanded_line,
                           uint8_t *belongs_to_main_file) {
    std::string current_file = main_file;
    uint32_t current_line = 1;
    int expanded_line = 1;
    size_t line_start = 0;

    while (line_start <= expanded.size()) {
        size_t line_end = expanded.find('\n', line_start);
        if (line_end == std::string::npos) {
            line_end = expanded.size();
        }

        uint32_t directive_line = 0;
        std::string directive_file;
        if (parse_line_directive(expanded.c_str() + line_start, line_end - line_start,
                                 &directive_line, &directive_file)) {
            current_file = directive_file;
            current_line = directive_line;
            original_line_by_expanded_line[expanded_line] = 0;
            belongs_to_main_file[expanded_line] = 0;
        } else {
            original_line_by_expanded_line[expanded_line] = current_line;
            belongs_to_main_file[expanded_line] = current_file == main_file ? 1 : 0;
            if (current_line < UINT32_MAX) {
                current_line++;
            }
        }

        if (line_end == expanded.size()) {
            break;
        }
        line_start = line_end + 1;
        expanded_line++;
    }
    return true;
}

// Общая подготовка. Основной проход с явным контекстом не использует эвристику
// поиска директив: макрос может встречаться и без `#define` в самом файле.
// `primary` исключает чужие токены до сериализации; библиотечные объекты имеют
// автоматическое время жизни, итоговые C-буферы передаются вызывающему.
static CBMPreprocessedSource *preprocess_with_map_impl(const char *source, int source_len,
                                                       const char *filename,
                                                       const char **extra_defines,
                                                       const char **include_paths, int cpp_mode,
                                                       bool primary, CBMPreprocessStatus *status) {
    if (status)
        *status = CBM_PREPROCESS_FAILED;
    if (!source || source_len < 0)
        return NULL;
    if (source_len == 0 || (!primary && !has_preprocessor_work(source, source_len))) {
        if (status)
            *status = CBM_PREPROCESS_UNCHANGED;
        return NULL;
    }

    try {
        simplecpp::DUI dui;
        if (extra_defines) {
            for (int i = 0; extra_defines[i]; i++)
                dui.defines.push_back(extra_defines[i]);
        }
        if (include_paths) {
            for (int i = 0; include_paths[i]; i++)
                dui.includePaths.push_back(include_paths[i]);
        }
        dui.std = cpp_mode ? "c++17" : "c11";

        std::string src(source, source_len);
        if (primary && has_source_line_control(src))
            return NULL;
        std::istringstream istr(src);
        std::vector<std::string> files;
        files.push_back(filename ? filename : "<input>");

        simplecpp::OutputList diagnostics;
        simplecpp::OutputList *messages = primary ? &diagnostics : nullptr;
        simplecpp::TokenList rawtokens(istr, files, files[0], messages);
        if (primary) {
            // Пользовательский #line может переименовать файл или задать
            // виртуальные строки. Пока нет физической карты таких директив,
            // используем явный резерв вместо выдуманных исходных координат.
            const unsigned original_lines = (unsigned)count_expanded_lines(src);
            for (const simplecpp::Token *tok = rawtokens.cfront(); tok; tok = tok->next) {
                if (tok->location.fileIndex != 0 || tok->location.line > original_lines)
                    return NULL;
                if (tok->str() == "#") {
                    const simplecpp::Token *directive = tok->next;
                    while (directive && directive->comment)
                        directive = directive->next;
                    if (directive && (directive->number || directive->str() == "line" ||
                                      directive->str() == "file" || directive->str() == "endfile"))
                        return NULL;
                }
            }
        }
        simplecpp::TokenList output(files);
        simplecpp::FileDataCache filedata = simplecpp::load(rawtokens, files, dui, messages);

        simplecpp::preprocess(output, rawtokens, files, filedata, dui, messages);
        if (primary) {
            // #line в заголовке не должен выдавать его токены за основной файл.
            // Проверяется и кеш, дополненный ленивыми include при preprocess.
            for (const auto &header : filedata) {
                for (const simplecpp::Token *tok = header->tokens.cfront(); tok; tok = tok->next) {
                    if (tok->location.fileIndex == 0)
                        return NULL;
                }
            }
        }

        bool missing_headers = false;
        for (const auto &message : diagnostics) {
            switch (message.type) {
            case simplecpp::Output::WARNING:
            case simplecpp::Output::PORTABILITY_BACKSLASH:
                break;
            case simplecpp::Output::MISSING_HEADER:
                missing_headers = true;
                break;
            default:
                return NULL;
            }
        }

        // Заголовки уже предоставили определения макросов. Их C++-токены должны
        // индексироваться в собственных файлах, а не повторно в каждом include.
        simplecpp::TokenList own_tokens(files);
        if (primary) {
            for (const simplecpp::Token *tok = output.cfront(); tok; tok = tok->next) {
                if (tok->location.fileIndex == 0) {
                    own_tokens.push_back(
                        new simplecpp::Token(tok->str(), tok->location, tok->whitespaceahead));
                }
            }
        }
        std::string result = primary ? own_tokens.stringify() : output.stringify();
        if (result.size() > INT_MAX - 1u)
            return NULL;

        // Загруженные заголовки больше не нужны; строки имён остаются в files.
        simplecpp::cleanup(filedata);

        std::unique_ptr<CBMPreprocessedSource, decltype(&cbm_preprocessed_source_free)> pp(
            (CBMPreprocessedSource *)calloc(1, sizeof(CBMPreprocessedSource)),
            cbm_preprocessed_source_free);
        if (!pp) {
            return NULL;
        }
        int line_count = count_expanded_lines(result);
        pp->source = (char *)malloc(result.size() + 1);
        pp->original_line_by_expanded_line =
            (uint32_t *)calloc((size_t)line_count + 1u, sizeof(uint32_t));
        pp->belongs_to_main_file = (uint8_t *)calloc((size_t)line_count + 1u, sizeof(uint8_t));
        pp->expanded_line_count = line_count;
        pp->source_len = (int)result.size();
        pp->missing_headers = missing_headers;
        if (!pp->source || !pp->original_line_by_expanded_line || !pp->belongs_to_main_file) {
            return NULL;
        }
        memcpy(pp->source, result.c_str(), result.size() + 1);
        if (!build_line_map(result, files[0], pp->original_line_by_expanded_line,
                            pp->belongs_to_main_file)) {
            return NULL;
        }
        if (primary) {
            // Служебные #line нужны карте, но не являются C++-кодом. Пробелы
            // сохраняют смещения; карта уже построена по исходной сериализации.
            int line = 1;
            for (int i = 0; i < pp->source_len; ++i) {
                if (pp->source[i] == '\n')
                    ++line;
                else if (!pp->belongs_to_main_file[line])
                    pp->source[i] = ' ';
            }
        }
        if (status)
            *status = CBM_PREPROCESS_OK;
        return pp.release();
    } catch (...) {
        // Отказ с сохранённым статусом FAILED разрешает вызывающему взять оригинал.
        return NULL;
    }
}

// Совместимый дополнительный проход C/CUDA и C++ без явно заданного контекста.
CBMPreprocessedSource *cbm_preprocess_with_map(const char *source, int source_len,
                                               const char *filename, const char **extra_defines,
                                               const char **include_paths, int cpp_mode) {
    return preprocess_with_map_impl(source, source_len, filename, extra_defines, include_paths,
                                    cpp_mode, false, nullptr);
}

// Успешный пустой текст сохраняет статус OK; отказ не маскируется пустой строкой.
CBMPreprocessedSource *cbm_preprocess_cpp(const char *source, int source_len, const char *filename,
                                          const char **extra_defines, const char **include_paths,
                                          CBMPreprocessStatus *status) {
    if (!status)
        return NULL;
    return preprocess_with_map_impl(source, source_len, filename, extra_defines, include_paths, 1,
                                    true, status);
}

char *cbm_preprocess(const char *source, int source_len, const char *filename,
                     const char **extra_defines, const char **include_paths, int cpp_mode) {
    CBMPreprocessedSource *pp = cbm_preprocess_with_map(source, source_len, filename, extra_defines,
                                                        include_paths, cpp_mode);
    if (!pp) {
        return NULL;
    }
    char *out = pp->source;
    pp->source = NULL;
    cbm_preprocessed_source_free(pp);
    return out;
}

void cbm_preprocess_free(char *expanded) {
    free(expanded);
}

void cbm_preprocessed_source_free(CBMPreprocessedSource *pp) {
    if (!pp) {
        return;
    }
    free(pp->source);
    free(pp->original_line_by_expanded_line);
    free(pp->belongs_to_main_file);
    free(pp);
}

} // extern "C"
