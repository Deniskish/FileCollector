#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

// Потокобезопасный сборщик файлов из чанков, приходящих в произвольном порядке.
//
// Хранение:
//  * Буфер файла выделяется один раз в CollectFile и никогда не реаллоцируется.
//  * Покрытие файла хранится как набор непересекающихся слитых интервалов
//    (std::map begin -> end). Вставка чанка: O(log k + m), где k — число
//    «дыр» в файле, m — число интервалов, которые чанк перекрывает. Соседние
//    интервалы сливаются, поэтому по мере сборки структура только сжимается.
//  * Из чанка копируются только ещё не полученные байты: пересечения и
//    дубликаты повторно не копируются.
//
// Синхронизация:
//  * shared_mutex на таблицу файлов (на запись берётся только при появлении
//    нового fileId) + собственный mutex у каждого файла. Разные файлы
//    обрабатываются независимо.
//  * Под мьютексом файла чанк только резервирует свои непокрытые участки
//    (короткая операция над map); memcpy выполняется без блокировки.
//    Зарезервированные участки не пересекаются, поэтому несколько потоков
//    пишут в один буфер одновременно без гонок данных.
//  * Файл готов, когда число фактически записанных (а не зарезервированных)
//    байт равно размеру файла.
//
// Получение данных:
//  * TryTakeFile / WaitAndTakeFile — полный файл перемещением, без копирования.
//  * ReadRange — частичное чтение уже полученного диапазона в буфер вызывающего.
//  * GetReceivedRanges / GetProgress — что уже получено.
class FileCollector {
public:
    using Bytes = std::vector<uint8_t>;

    struct Range {
        size_t begin;  // включительно
        size_t end;    // не включительно

        friend bool operator==(const Range&, const Range&) = default;
    };

    struct Progress {
        size_t received = 0;  // записано байт
        size_t total = 0;     // размер файла (0, пока не вызван CollectFile)
        bool complete = false;
    };

    // Регистрирует файл. Повторный вызов для собираемого или собранного файла
    // игнорируется. Для уже выданного fileId начинает новую сборку.
    void CollectFile(uint32_t fileId, size_t fileSize) {
        Bytes buffer(fileSize);  // выделяем до блокировки
        auto entry = FindOrCreate(fileId);

        std::lock_guard lock(entry->mtx);
        if (entry->state == State::Collecting || entry->state == State::Ready)
            return;

        entry->state = State::Collecting;
        entry->size = fileSize;
        entry->written = 0;
        entry->data = std::move(buffer);
        entry->reserved.clear();
        entry->filled.clear();

        // Применяем чанки, пришедшие раньше CollectFile. Мьютекс удерживается,
        // других резервирований нет, поэтому копируем сразу.
        auto pending = std::move(entry->pending);
        entry->pending.clear();
        for (auto& [pos, chunk] : pending) {
            if (!InBounds(pos, chunk.size(), fileSize))
                continue;
            for (const Range& g : Reserve(entry->reserved, pos, pos + chunk.size()))
                entry->written += CopyRange(entry->data.data(), chunk, pos, g);
        }
        entry->filled = entry->reserved;

        if (entry->written == entry->size)
            MarkReady(*entry);
    }

    // Добавляет чанк. Чанки до CollectFile буферизуются; чанки за границей
    // файла и чанки уже собранного файла отбрасываются.
    void OnNewChunk(uint32_t fileId, size_t pos, Bytes&& chunk) {
        if (chunk.empty())
            return;
        auto entry = FindOrCreate(fileId);

        std::vector<Range> gaps;
        uint8_t* base = nullptr;
        {
            std::lock_guard lock(entry->mtx);
            switch (entry->state) {
            case State::Pending:
                entry->pending.emplace_back(pos, std::move(chunk));
                return;
            case State::Ready:
            case State::Taken:
                return;
            case State::Collecting:
                break;
            }
            if (!InBounds(pos, chunk.size(), entry->size))
                return;

            gaps = Reserve(entry->reserved, pos, pos + chunk.size());
            if (gaps.empty())
                return;  // все байты чанка уже получены
            base = entry->data.data();
        }

        // Без блокировки: участки принадлежат только этому потоку, а буфер
        // не может быть отдан, пока written < size.
        size_t copied = 0;
        for (const Range& g : gaps)
            copied += CopyRange(base, chunk, pos, g);

        std::lock_guard lock(entry->mtx);
        entry->written += copied;
        if (entry->written == entry->size) {
            MarkReady(*entry);
            return;
        }
        for (const Range& g : gaps)
            Reserve(entry->filled, g.begin, g.end);
    }

    // Полный файл без копирования: отдаёт буфер перемещением и освобождает
    // память коллектора. Возвращает nullopt, если файл не собран или уже выдан.
    std::optional<Bytes> TryTakeFile(uint32_t fileId) {
        auto entry = Find(fileId);
        if (!entry)
            return std::nullopt;
        std::lock_guard lock(entry->mtx);
        if (entry->state != State::Ready)
            return std::nullopt;
        return TakeLocked(*entry);
    }

    // То же, но ждёт завершения сборки не дольше timeout. Можно вызывать
    // до CollectFile. Из нескольких ожидающих файл получит ровно один.
    template <class Rep, class Period>
    std::optional<Bytes> WaitAndTakeFile(uint32_t fileId,
                                         std::chrono::duration<Rep, Period> timeout) {
        auto entry = FindOrCreate(fileId);
        std::unique_lock lock(entry->mtx);
        const bool done = entry->cv.wait_for(lock, timeout, [&] {
            return entry->state == State::Ready || entry->state == State::Taken;
        });
        if (!done || entry->state != State::Ready)
            return std::nullopt;
        return TakeLocked(*entry);
    }

    // Частичное чтение: копирует [pos, pos + out.size()) в буфер вызывающего,
    // если весь диапазон уже получен. Копируется ровно запрошенный объём.
    bool ReadRange(uint32_t fileId, size_t pos, std::span<uint8_t> out) const {
        auto entry = Find(fileId);
        if (!entry)
            return false;
        std::lock_guard lock(entry->mtx);
        if (entry->state != State::Collecting && entry->state != State::Ready)
            return false;
        if (!InBounds(pos, out.size(), entry->size))
            return false;
        if (out.empty())
            return true;
        if (entry->state == State::Collecting &&
            !Contains(entry->filled, pos, pos + out.size()))
            return false;
        std::memcpy(out.data(), entry->data.data() + pos, out.size());
        return true;
    }

    // Уже полученные диапазоны файла (по возрастанию, слитые).
    std::vector<Range> GetReceivedRanges(uint32_t fileId) const {
        auto entry = Find(fileId);
        if (!entry)
            return {};
        std::lock_guard lock(entry->mtx);
        if (entry->state == State::Ready)
            return entry->size ? std::vector<Range>{{0, entry->size}} : std::vector<Range>{};
        if (entry->state != State::Collecting)
            return {};
        std::vector<Range> out;
        out.reserve(entry->filled.size());
        for (const auto& [b, e] : entry->filled)
            out.push_back({b, e});
        return out;
    }

    std::optional<Progress> GetProgress(uint32_t fileId) const {
        auto entry = Find(fileId);
        if (!entry)
            return std::nullopt;
        std::lock_guard lock(entry->mtx);
        switch (entry->state) {
        case State::Pending:
            return Progress{};
        case State::Taken:
            return std::nullopt;
        default:
            return Progress{entry->written, entry->size, entry->state == State::Ready};
        }
    }

private:
    enum class State { Pending, Collecting, Ready, Taken };
    using RangeMap = std::map<size_t, size_t>;  // begin -> end, без пересечений и стыков

    struct Entry {
        mutable std::mutex mtx;
        std::condition_variable cv;
        State state = State::Pending;
        size_t size = 0;
        size_t written = 0;
        Bytes data;
        RangeMap reserved;  // кому-то уже поручено записать
        RangeMap filled;    // фактически записано (для частичного чтения)
        std::vector<std::pair<size_t, Bytes>> pending;
    };

    std::shared_ptr<Entry> Find(uint32_t fileId) const {
        std::shared_lock lock(mapMtx_);
        auto it = files_.find(fileId);
        return it == files_.end() ? nullptr : it->second;
    }

    std::shared_ptr<Entry> FindOrCreate(uint32_t fileId) {
        if (auto e = Find(fileId))
            return e;
        auto fresh = std::make_shared<Entry>();
        std::unique_lock lock(mapMtx_);
        auto [it, inserted] = files_.try_emplace(fileId, std::move(fresh));
        return it->second;
    }

    static bool InBounds(size_t pos, size_t len, size_t fileSize) noexcept {
        return pos <= fileSize && len <= fileSize - pos;  // без переполнения
    }

    static size_t CopyRange(uint8_t* base, const Bytes& chunk, size_t chunkPos,
                            const Range& g) noexcept {
        const size_t len = g.end - g.begin;
        std::memcpy(base + g.begin, chunk.data() + (g.begin - chunkPos), len);
        return len;
    }

    static void MarkReady(Entry& e) {
        e.state = State::Ready;
        e.reserved.clear();
        e.filled.clear();
        e.cv.notify_all();
    }

    static Bytes TakeLocked(Entry& e) {
        Bytes out = std::move(e.data);
        e.data = Bytes{};
        // «Надгробие»: опоздавшие дубликаты чанков игнорируются, а не копятся.
        e.state = State::Taken;
        return out;
    }

    static bool Contains(const RangeMap& ranges, size_t b, size_t e) {
        auto it = ranges.upper_bound(b);
        if (it == ranges.begin())
            return false;
        --it;
        return it->first <= b && e <= it->second;
    }

    // Возвращает непокрытые участки [b, e) и помечает весь [b, e) покрытым,
    // сливая его с пересекающимися и соседними интервалами.
    // Строгая гарантия исключений: при bad_alloc map не меняется.
    static std::vector<Range> Reserve(RangeMap& covered, size_t b, size_t e) {
        std::vector<Range> gaps;

        auto first = covered.upper_bound(b);
        if (first != covered.begin()) {
            auto prev = std::prev(first);
            if (prev->second >= b)
                first = prev;
        }

        size_t cur = b, newB = b, newE = e;
        auto last = first;
        for (; last != covered.end() && last->first <= e; ++last) {
            if (last->first > cur)
                gaps.push_back({cur, last->first});
            cur = std::max(cur, last->second);
            newB = std::min(newB, last->first);
            newE = std::max(newE, last->second);
        }
        if (cur < e)
            gaps.push_back({cur, e});
        if (gaps.empty())
            return gaps;

        if (first == last) {
            covered.emplace_hint(last, newB, newE);
        } else {
            // Переиспользуем узел: слияние не требует аллокаций.
            auto next = std::next(first);
            auto node = covered.extract(first);
            covered.erase(next, last);
            node.key() = newB;
            node.mapped() = newE;
            covered.insert(last, std::move(node));
        }
        return gaps;
    }

    mutable std::shared_mutex mapMtx_;
    std::unordered_map<uint32_t, std::shared_ptr<Entry>> files_;
};
