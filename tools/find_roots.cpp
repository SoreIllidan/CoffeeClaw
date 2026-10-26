// Coffeeclaw - find_roots
// Localiza en el proceso vivo de ARK: FName pool, UWorld, slot de GWorld y los
// offsets de la cadena mundo -> nivel -> actores -> camara.
// Imprime un bloque listo para pegar en offsets.ini.
//
// Uso (consola elevada, con el juego en partida):
//   find_roots.exe              escaneo completo
//   find_roots.exe --names      solo el FName pool
//   find_roots.exe --verbose    detalle extra

#include "../src/proc.h"
#include "../src/config.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace claw;

namespace {

FILE* g_log = nullptr;

constexpr uintptr_t kOffClass = 0x10;
constexpr uintptr_t kOffName = 0x18;

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void Say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
    if (g_log) {
        va_list ap2;
        va_start(ap2, fmt);
        std::vfprintf(g_log, fmt, ap2);
        va_end(ap2);
        std::fflush(g_log);
    }
}

struct Region {
    uintptr_t base = 0;
    size_t size = 0;
    bool writable = false;
    DWORD type = 0;        // MEM_PRIVATE / MEM_MAPPED / MEM_IMAGE
    DWORD protect = 0;
    bool Heap() const { return type == MEM_PRIVATE; }
};

struct ArrayField {
    uintptr_t data = 0;
    int32_t num = 0;
    int32_t max = 0;
    size_t offset = 0;
};

class Finder {
public:
    bool Open() {
        if (!proc_.Attach(L"ArkAscended.exe")) {
            Say("[x] no pude abrir ArkAscended.exe.\n");
            Say("    consola COMO ADMINISTRADOR, juego en partida, sin BattlEye.\n");
            return false;
        }
        moduleBase_ = proc_.base();
        moduleSize_ = proc_.imageSize();
        Say("[+] ArkAscended.exe  pid=%lu  base=0x%llX  imagen=%zu MB\n",
            proc_.pid(), (unsigned long long)moduleBase_, moduleSize_ / (1024 * 1024));
        ReadSections();
        EnumerateRegions();
        size_t total = 0, rw = 0;
        for (const auto& r : regions_) {
            total += r.size;
            if (r.writable) rw += r.size;
        }
        Say("[+] memoria: %.1f GB  escribible: %.1f GB  regiones: %zu\n",
            total / 1073741824.0, rw / 1073741824.0, regions_.size());
        ClassifyRegions();
        return true;
    }

    uintptr_t poolAddress() const { return pool_; }

    // ------------------------------------------------------------- name pool ---
    // Busca la entrada 0 del pool ("None" seguida de "ByteProperty") en paralelo.
    bool FindNamePool() {
        const double t0 = Now();
        std::vector<Region> targets;
        size_t totalBytes = 0;
        for (const auto& r : regions_) {
            if (!r.writable || !r.Heap() || r.size < 0x1000) continue;
            targets.push_back(r);
            totalBytes += r.size;
        }
        std::atomic<size_t> next{ 0 };
        std::atomic<uint64_t> scanned{ 0 };
        std::atomic<bool> done{ false };
        std::atomic<bool> hit{ false };
        std::atomic<uintptr_t> where{ 0 };

        std::thread monitor([&]() {
            while (!done.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                if (done.load()) break;
                Say("    buscando pool ... %.2f / %.2f GB (%.0f%%)  %.0fs\n",
                    scanned.load() / 1e9, totalBytes / 1e9,
                    totalBytes ? 100.0 * (double)scanned.load() / (double)totalBytes : 0.0,
                    Now() - t0);
            }
        });

        unsigned threads = std::max(2u, std::min(12u, std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([&]() {
                std::vector<uint8_t> buf(16 << 20);
                for (;;) {
                    if (hit.load()) break;
                    size_t ri = next.fetch_add(1);
                    if (ri >= targets.size()) break;
                    const Region& r = targets[ri];
                    size_t pos = 0;
                    while (pos < r.size) {
                        size_t len = std::min(buf.size(), r.size - pos);
                        if (!proc_.Read(r.base + pos, buf.data(), len)) {
                            scanned.fetch_add(len);
                            pos += len;
                            continue;
                        }
                        const uint8_t* base = buf.data();
                        const uint8_t* p = base + 2;
                        const uint8_t* end = base + len;
                        for (;;) {
                            const uint8_t* n = (const uint8_t*)std::memchr(p, 'N', (size_t)(end - p));
                            if (!n) break;
                            // Formato confirmado en este build:
                            //   uint16 header = bIsWide:1 | ProbeHash:5 | Len:10
                            //   texto SIN terminador; entradas alineadas a 2 bytes.
                            if (n + 40 <= end && std::memcmp(n, "None", 4) == 0) {
                                uint16_t hdr = 0;
                                std::memcpy(&hdr, n - 2, 2);
                                const bool wide = (hdr & 1) != 0;
                                const uint32_t len0 = (hdr >> 6) & 0x3FF;
                                if (!wide && len0 == 4) {
                                    // siguiente entrada: cabecera en T+4, texto en T+6
                                    if (std::memcmp(n + 6, "ByteProperty", 12) == 0) {
                                        uint16_t hdr2 = 0;
                                        std::memcpy(&hdr2, n + 4, 2);
                                        if (((hdr2 >> 6) & 0x3FF) == 12) {
                                            where.store(r.base + pos + (uintptr_t)(n - base) - 2);
                                            hit.store(true);
                                        }
                                    }
                                }
                            }
                            if (hit.load()) break;
                            p = n + 1;
                            if (p >= end) break;
                        }
                        scanned.fetch_add(len);
                        if (hit.load()) break;
                        pos += len;
                    }
                    if (hit.load()) break;
                }
            });
        }
        for (auto& th : pool) th.join();
        done.store(true);
        if (monitor.joinable()) monitor.join();

        block0_ = where.load();
        if (block0_)
            Say("[+] patron estricto: bloque de nombres en 0x%llX\n", (unsigned long long)block0_);
        if (block0_ && !AssignPoolFromBlock(block0_)) {
            Say("[!] el bloque estricto no cerro como pool; siguen las otras heuristicas.\n");
            block0_ = 0;
        }
        if (!pool_) {
            Say("[!] probando deteccion estructural sobre el .data del modulo...\n");
            StructuralFindPool();
        }
        if (!pool_) {
            Say("[!] patron estricto ausente; probando ancla 'None'...\n");
            AnchorFindPool();
        }
        if (!pool_) {
            Say("[!] probando deteccion flexible...\n");
            LooseFindNamePool();
        }
        if (!pool_) {
            Say("[x] no encontre el FName pool con ninguna heuristica.\n");
            DumpPoolContexts();
            return false;
        }
        if (block0_) Say("[+] FName block0: 0x%llX   (%.2fs)\n", (unsigned long long)block0_, Now() - t0);
        bool inModule = gnamesSlot_ >= moduleBase_ && gnamesSlot_ < moduleBase_ + moduleSize_;
        Say("[+] FNamePool:    0x%llX  slot=0x%llX ind=%d (%s)\n",
            (unsigned long long)pool_, (unsigned long long)gnamesSlot_, gnamesInd_,
            inModule ? "global del modulo: estable" : "heap: cambia cada arranque");
        {
            std::wstring a, b, c;
            Name(0, a); Name(1, b); Name(2, c);
            Say("    muestra: %ls | %ls | %ls\n", a.c_str(), b.c_str(), c.c_str());
        }
        return true;
    }

    uintptr_t gnamesSlot() const { return gnamesSlot_; }
    int gnamesIndirection() const { return gnamesInd_; }

    void ClearNames() {
        std::lock_guard<std::mutex> lock(nameMtx_);
        nameCache_.clear();
    }

    // Lectura por bloques con reintento fino: un tramo ilegible no tira el resto.
    template <typename Fn>
    void ScanRegion(const Region& r, size_t chunk, Fn&& fn) const {
        std::vector<uint8_t> buf(chunk);
        for (size_t pos = 0; pos < r.size; pos += chunk) {
            const size_t len = std::min(chunk, r.size - pos);
            if (proc_.Read(r.base + pos, buf.data(), len)) {
                stPrimaryOk.fetch_add(len);
                fn(r.base + pos, buf.data(), len);
                continue;
            }
            stPrimaryFail.fetch_add(len);
            for (size_t q = 0; q < len; q += 0x1000) {
                const size_t s = std::min<size_t>(0x1000, len - q);
                if (proc_.Read(r.base + pos + q, buf.data(), s)) {
                    stPageOk.fetch_add(s);
                    fn(r.base + pos + q, buf.data(), s);
                } else {
                    stPageFail.fetch_add(s);
                }
            }
        }
    }

    // Cuarta pasada: ancla en la entrada "None" (con cualquier convencion de
    // cabecera), busca quien la apunta y valida el pool resultante.
    bool AnchorFindPool() {
        const double t0 = Now();
        std::vector<Region> targets;
        size_t totalBytes = 0;
        for (const auto& r : regions_) {
            if (r.writable && r.type != MEM_IMAGE && r.size >= 0x1000) {
                targets.push_back(r);
                totalBytes += r.size;
            }
        }

        std::vector<uintptr_t> anchors;
        std::atomic<uint64_t> okBytes{ 0 };
        std::atomic<uint64_t> rawNone{ 0 };
        std::atomic<uint64_t> rawWide{ 0 };
        {
            std::atomic<size_t> next{ 0 };
            std::atomic<bool> enough{ false };
            std::mutex m;
            unsigned threads = std::max(2u, std::min(12u, std::thread::hardware_concurrency()));
            std::vector<std::thread> pool;
            for (unsigned t = 0; t < threads; ++t) {
                pool.emplace_back([&]() {
                    for (;;) {
                        size_t i = next.fetch_add(1);
                        if (i >= targets.size()) break;
                        ScanRegion(targets[i], 8 << 20, [&](uintptr_t addr, uint8_t* b, size_t len) {
                            okBytes.fetch_add(len);
                            const uint8_t* base = b;
                            const uint8_t* end = b + len;
                            const uint8_t* p = base + 2;
                            for (;;) {
                                const uint8_t* n = (const uint8_t*)std::memchr(p, 'N', (size_t)(end - p));
                                if (!n) break;
                                // ANSI: "None\0"
                                if (n + 40 <= end && std::memcmp(n, "None", 4) == 0 && n[4] == 0) {
                                    rawNone.fetch_add(1);
                                    uint16_t h = 0;
                                    std::memcpy(&h, n - 2, 2);
                                    int printable = 0;
                                    for (int k = 6; k < 40; ++k) {
                                        const uint8_t c = n[k];
                                        if (c == 0 || (c >= 0x20 && c < 0x7F)) ++printable;
                                    }
                                    const bool headerOk = (h & 0xFF00) == 0 && (h & 0xFF) <= 0x3F;
                                    if (headerOk && printable >= 28) {
                                        std::lock_guard<std::mutex> lock(m);
                                        if (anchors.size() < 20000) anchors.push_back(addr + (uintptr_t)(n - base) - 2);
                                    }
                                }
                                // UTF-16LE: "N\0o\0n\0e\0"
                                if (n + 60 <= end && n[1] == 0 && n[2] == 'o' && n[3] == 0 &&
                                    n[4] == 'n' && n[5] == 0 && n[6] == 'e' && n[7] == 0 &&
                                    n[8] == 0 && n[9] == 0) {
                                    rawWide.fetch_add(1);
                                    uint16_t h = 0;
                                    std::memcpy(&h, n - 2, 2);
                                    const bool headerOk = (h & 0xFF00) == 0 && (h & 0xFF) <= 0x3F;
                                    int printable = 0;
                                    for (int k = 10; k < 60; ++k) {
                                        const uint8_t c = n[k];
                                        if (c == 0 || (c >= 0x20 && c < 0x7F)) ++printable;
                                    }
                                    if (headerOk && printable >= 40) {
                                        std::lock_guard<std::mutex> lock(m);
                                        if (anchors.size() < 20000) anchors.push_back(addr + (uintptr_t)(n - base) - 2);
                                    }
                                }
                                p = n + 1;
                                if (p >= end) break;
                            }
                        });
                    }
                });
            }
            for (auto& th : pool) th.join();
        }
        Say("[+] anclas 'None': %zu  (%.2fs)\n", anchors.size(), Now() - t0);
        Say("    lectura OK: %.2f GB de %.2f GB   'None' vistos: %llu ascii / %llu wide\n",
            okBytes.load() / 1e9, totalBytes / 1e9,
            (unsigned long long)rawNone.load(), (unsigned long long)rawWide.load());
        Say("    detalle: primario ok=%.2f GB / fallo=%.2f GB | paginas ok=%.2f GB / fallo=%.2f GB\n",
            stPrimaryOk.load() / 1e9, stPrimaryFail.load() / 1e9,
            stPageOk.load() / 1e9, stPageFail.load() / 1e9);
        if (anchors.empty()) return false;
        std::sort(anchors.begin(), anchors.end());
        anchors.erase(std::unique(anchors.begin(), anchors.end()), anchors.end());
        Say("[+] anclas unicas: %zu\n", anchors.size());

        std::unordered_set<uintptr_t> wanted;
        for (uintptr_t a : anchors) {
            for (uintptr_t d = 0; d <= 6; d += 2) {
                if (a > d + 0x10000) wanted.insert(a - d);
            }
        }
        Say("[+] variantes de bloque a buscar: %zu\n", wanted.size());

        struct Pair { uintptr_t slot; uintptr_t block; };
        std::vector<Pair> pairs;
        {
            std::atomic<size_t> next{ 0 };
            std::mutex m;
            unsigned threads = std::max(2u, std::min(12u, std::thread::hardware_concurrency()));
            std::vector<std::thread> pool;
            for (unsigned t = 0; t < threads; ++t) {
                pool.emplace_back([&]() {
                    std::vector<Pair> local;
                    for (;;) {
                        size_t i = next.fetch_add(1);
                        if (i >= targets.size()) break;
                        ScanRegion(targets[i], 8 << 20, [&](uintptr_t addr, uint8_t* b, size_t len) {
                            for (size_t k = 0; k + 8 <= len; k += 8) {
                                uint64_t v = 0;
                                std::memcpy(&v, b + k, 8);
                                if (wanted.find((uintptr_t)v) == wanted.end()) continue;
                                local.push_back({ addr + k, (uintptr_t)v });
                                if (local.size() > 20000) {
                                    std::lock_guard<std::mutex> lock(m);
                                    pairs.insert(pairs.end(), local.begin(), local.end());
                                    local.clear();
                                }
                            }
                        });
                    }
                    std::lock_guard<std::mutex> lock(m);
                    pairs.insert(pairs.end(), local.begin(), local.end());
                });
            }
            for (auto& th : pool) th.join();
        }
        Say("[+] punteros a esas anclas: %zu  (%.2fs)\n", pairs.size(), Now() - t0);

        int shown = 0;
        size_t tried = 0;
        for (const Pair& p : pairs) {
            if (++tried > 300000) break;
            bool win = false;
            for (uintptr_t toff : { (uintptr_t)2, (uintptr_t)4, (uintptr_t)6 }) {
                textOff_ = toff;
                gnamesSlot_ = p.slot;
                gnamesInd_ = 0;
                pool_ = p.slot;
                block0_ = p.block;
                ClearNames();
                if (ValidatePool()) {
                    Say("[+] pool por ancla: 0x%llX  (slot 0x%llX, texto en +%llu)\n",
                        (unsigned long long)pool_, (unsigned long long)p.slot,
                        (unsigned long long)textOff_);
                    win = true;
                    break;
                }
            }
            if (win) return true;
            textOff_ = 2;
            if (shown < 3) {
                std::wstring a, b;
                Name(0, a);
                Name(1, b);
                if (!a.empty())
                    Say("    descartado slot=0x%llX -> '%ls' | '%ls'\n",
                        (unsigned long long)p.slot, a.c_str(), b.c_str());
                ++shown;
            }
            gnamesSlot_ = 0;
            pool_ = 0;
            block0_ = 0;
        }
        return false;
    }

    // A partir del bloque 0: encuentra el array Blocks y el global que lo apunta.
    bool AssignPoolFromBlock(uintptr_t block) {
        Say("    bloque 0x%llX: buscando el puntero global en .data ...\n", (unsigned long long)block);
        uintptr_t slot = FindGlobalPointerTo(block);
        Say("    .data: %s\n", slot ? "encontrado" : "sin coincidencia");
        if (slot) {
            gnamesSlot_ = slot;
            gnamesInd_ = 0;
            pool_ = slot;
        } else {
            uintptr_t heapSlot = FindPointerAnywhere(block);
            Say("    heap: %s\n", heapSlot ? "encontrado" : "sin coincidencia");
            if (!heapSlot) return false;
            pool_ = heapSlot;
            uintptr_t globalSlot = FindGlobalPointerTo(heapSlot);
            gnamesSlot_ = globalSlot ? globalSlot : heapSlot;
            gnamesInd_ = globalSlot ? 1 : 0;
            if (!globalSlot)
                Say("[!] el pool vive en heap sin global estable: GNamesSlot cambia por arranque.\n");
        }
        block0_ = block;
        ClearNames();
        Say("    validando pool 0x%llX ...\n", (unsigned long long)pool_);
        if (ValidatePool()) { Say("    pool valido\n"); return true; }
        Say("    validacion rechazada\n");
        pool_ = 0;
        block0_ = 0;
        gnamesSlot_ = 0;
        gnamesInd_ = 0;
        return false;
    }

    // Segunda pasada: no exige cabecera ni orden. Acepta nombres ANSI y UTF-16,
    // localiza "ByteProperty" con "IntProperty" cerca y deriva el bloque.
    struct LooseHit { uintptr_t text; bool wide; };

    void LooseFindNamePool() {
        const double t0 = Now();
        std::vector<Region> targets;
        for (const auto& r : regions_) {
            if (!r.writable || !r.Heap() || r.size < 0x1000) continue;
            targets.push_back(r);
        }
        std::atomic<size_t> next{ 0 };
        std::atomic<bool> score{ false };
        std::mutex outMtx;
        std::vector<LooseHit> cands;

        unsigned threads = std::max(2u, std::min(12u, std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([&]() {
                std::vector<uint8_t> buf(16 << 20);
                for (;;) {
                    size_t ri = next.fetch_add(1);
                    if (ri >= targets.size()) break;
                    const Region& r = targets[ri];
                    for (size_t pos = 0; pos < r.size; pos += buf.size()) {
                        if (score.load()) break;
                        size_t len = std::min(buf.size(), r.size - pos);
                        if (!proc_.Read(r.base + pos, buf.data(), len)) continue;
                        const uint8_t* base = buf.data();
                        const uint8_t* end = base + len;
                        const uint8_t* p = base;
                        for (;;) {
                            const uint8_t* n = (const uint8_t*)std::memchr(p, 'B', (size_t)(end - p));
                            if (!n) break;

                            // ANSI
                            if (n + 64 <= end && std::memcmp(n, "ByteProperty", 12) == 0 && n[12] == 0) {
                                bool hasInt = false;
                                for (const uint8_t* q = n + 13; q + 11 <= std::min(end, n + 64); ++q)
                                    if (std::memcmp(q, "IntProperty", 11) == 0) { hasInt = true; break; }
                                if (hasInt) {
                                    std::lock_guard<std::mutex> lock(outMtx);
                                    cands.push_back({ r.base + pos + (uintptr_t)(n - base), false });
                                    if (cands.size() > 400) score.store(true);
                                }
                            }

                            // UTF-16LE: B\0y\0t\0e\0P\0r\0o\0p\0e\0r\0t\0y\0
                            if (n + 96 <= end) {
                                static const char* kAscii = "ByteProperty";
                                bool ok = true;
                                for (int i = 0; i < 12 && ok; ++i) {
                                    if (n[i * 2] != (uint8_t)kAscii[i] || n[i * 2 + 1] != 0) ok = false;
                                }
                                if (ok && n[24] == 0 && n[25] == 0) {
                                    bool hasInt = false;
                                    for (const uint8_t* q = n + 26; q + 24 <= std::min(end, n + 96); q += 2) {
                                        static const char* kInt = "IntProperty";
                                        bool same = true;
                                        for (int i = 0; i < 11 && same; ++i)
                                            if (q[i * 2] != (uint8_t)kInt[i] || q[i * 2 + 1] != 0) same = false;
                                        if (same) { hasInt = true; break; }
                                    }
                                    if (hasInt) {
                                        std::lock_guard<std::mutex> lock(outMtx);
                                        cands.push_back({ r.base + pos + (uintptr_t)(n - base), true });
                                        if (cands.size() > 400) score.store(true);
                                    }
                                }
                            }
                            p = n + 1;
                            if (p >= end) break;
                        }
                    }
                }
            });
        }
        for (auto& th : pool) th.join();
        Say("[+] candidatos 'ByteProperty'+'IntProperty': %zu  (%.2fs)\n", cands.size(), Now() - t0);

        int tried = 0;
        for (const LooseHit& h : cands) {
            if (tried >= 8) break;
            ++tried;
            const size_t back = 64;
            uint8_t ctx[192]{};
            if (!proc_.Read(h.text - back, ctx, sizeof(ctx))) continue;

            uintptr_t block = 0;
            for (size_t k = 0; k + 6 <= back; ++k) {
                if (std::memcmp(ctx + k, "None", 4) == 0 && ctx[k + 4] == 0) {
                    block = h.text - back + k - 2;
                    break;
                }
                if (ctx[k] == 'N' && ctx[k + 1] == 0 && ctx[k + 2] == 'o' && ctx[k + 3] == 0 &&
                    ctx[k + 4] == 'n' && ctx[k + 5] == 0) {
                    block = h.text - back + k - 2;
                    break;
                }
            }
            if (!block) block = h.text - (h.wide ? 14 : 10);
            if (AssignPoolFromBlock(block)) {
                Say("[+] pool por deteccion flexible: 0x%llX\n", (unsigned long long)block);
                return;
            }
        }
        Say("[!] la deteccion flexible no cerro.\n");
    }

    // Tercera pasada: sin depender de cadenas conocidas. Recorre el .data del
    // modulo buscando un array cuyo primer puntero apunte a texto de nombre.
    bool StructuralFindPool() {
        const double t0 = Now();
        std::vector<Region> mods;
        size_t total = 0;
        for (const auto& r : regions_) {
            if (r.writable && r.base >= moduleBase_ && r.base < moduleBase_ + moduleSize_) {
                mods.push_back(r);
                total += r.size;
            }
        }
        Say("    .data escribible: %.1f MB\n", total / 1048576.0);

        struct Cand { uintptr_t slot; uintptr_t pool; int ind; };
        std::mutex outMtx;
        std::vector<Cand> cands;
        std::atomic<size_t> next{ 0 };
        std::atomic<bool> enough{ false };

        unsigned threads = std::max(2u, std::min(12u, std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([&]() {
                std::vector<uint8_t> buf(8 << 20);
                for (;;) {
                    if (enough.load()) break;
                    size_t i = next.fetch_add(1);
                    if (i >= mods.size()) break;
                    const Region& r = mods[i];
                    for (size_t pos = 0; pos < r.size; pos += buf.size()) {
                        if (enough.load()) break;
                        size_t len = std::min(buf.size(), r.size - pos);
                        if (!proc_.Read(r.base + pos, buf.data(), len)) continue;
                        for (size_t k = 0; k + 8 <= len; k += 8) {
                            uint64_t v = 0;
                            std::memcpy(&v, buf.data() + k, 8);
                            if (!InWritable((uintptr_t)v)) continue;
                            uint8_t t64[64]{};
                            if (!proc_.Read((uintptr_t)v, t64, sizeof(t64))) continue;
                            auto looksName = [](const uint8_t* b) {
                                if (!((b[2] >= 'A' && b[2] <= 'Z') || (b[2] >= 'a' && b[2] <= 'z') || b[2] == '_')) return false;
                                int run = 0;
                                while (run < 60 && b[2 + run] >= 0x20 && b[2 + run] < 0x7F) ++run;
                                return run >= 2 && run < 60 && b[2 + run] == 0;
                            };
                            uintptr_t slotAddr = r.base + pos + k;
                            if (looksName(t64)) {
                                std::lock_guard<std::mutex> lock(outMtx);
                                cands.push_back({ slotAddr, slotAddr, 0 });
                                if (cands.size() > 3000) enough.store(true);
                                continue;
                            }
                            // Caso B: el slot apunta a un objeto en heap (FNamePool).
                            // El array Blocks puede NO estar en el offset 0: hay builds
                            // con Lock/CurrentBlock/CurrentByteCursor delante.
                            uint8_t obj[0x80]{};
                            if (!proc_.Read((uintptr_t)v, obj, sizeof(obj))) continue;
                            for (size_t ko = 0; ko <= 0x70; ko += 8) {
                                uintptr_t p2 = 0;
                                std::memcpy(&p2, obj + ko, 8);
                                if (!InWritable(p2)) continue;
                                uint8_t t2[64]{};
                                if (!proc_.Read(p2, t2, sizeof(t2))) continue;
                                if (!looksName(t2)) continue;
                                std::lock_guard<std::mutex> lock(outMtx);
                                cands.push_back({ slotAddr, (uintptr_t)v + ko, 1 });
                                if (cands.size() > 3000) enough.store(true);
                                break;
                            }
                        }
                    }
                }
            });
        }
        for (auto& th : pool) th.join();
        Say("    candidatos estructurales: %zu  (%.2fs)\n", cands.size(), Now() - t0);

        int rejected = 0;
        for (const Cand& c : cands) {
            gnamesSlot_ = c.slot;
            gnamesInd_ = c.ind;
            pool_ = c.pool;
            block0_ = c.pool;   // marcador de exito
            bool win = false;
            for (uintptr_t toff : { (uintptr_t)2, (uintptr_t)4, (uintptr_t)6 }) {
                textOff_ = toff;
                ClearNames();
                if (ValidatePool()) {
                    Say("[+] pool estructural: 0x%llX (texto en +%llu)\n",
                        (unsigned long long)c.pool, (unsigned long long)textOff_);
                    win = true;
                    break;
                }
            }
            if (win) return true;
            textOff_ = 2;
            if (rejected < 4) {
                std::wstring a, b;
                Name(0, a);
                Name(1, b);
                if (!a.empty())
                    Say("    descartado slot=0x%llX pool=0x%llX -> '%ls' | '%ls'\n",
                        (unsigned long long)c.slot, (unsigned long long)c.pool, a.c_str(), b.c_str());
                ++rejected;
            }
            gnamesSlot_ = 0;
            gnamesInd_ = 0;
            pool_ = 0;
            block0_ = 0;
        }
        return false;
    }

    // Volcado de contexto para diagnostico cuando no hay pool.
    void DumpPoolContexts() {
        Say("\n--- contexto de 'ByteProperty' (32 bytes antes / 24 despues) ---\n");
        int shown = 0;
        for (const auto& r : regions_) {
            if (!r.writable || !r.Heap() || r.size < 0x1000) continue;
            std::vector<uint8_t> buf(8 << 20);
            for (size_t pos = 0; pos < r.size && shown < 5; pos += buf.size()) {
                size_t len = std::min(buf.size(), r.size - pos);
                if (!proc_.Read(r.base + pos, buf.data(), len)) continue;
                const uint8_t* base = buf.data();
                const uint8_t* end = base + len;
                const uint8_t* p = base;
                for (;;) {
                    const uint8_t* n = (const uint8_t*)std::memchr(p, 'B', (size_t)(end - p));
                    if (!n) break;
                    if (n + 20 <= end && std::memcmp(n, "ByteProperty", 12) == 0) {
                        const uintptr_t addr = r.base + pos + (uintptr_t)(n - base);
                        uint8_t ctx[64]{};
                        if (proc_.Read(addr - 24, ctx, sizeof(ctx))) {
                            Say("@0x%llX\n", (unsigned long long)addr);
                            for (int row = 0; row < 4; ++row) {
                                Say("  ");
                                for (int col = 0; col < 16; ++col) {
                                    Say("%02X ", ctx[row * 16 + col]);
                                }
                                Say(" |");
                                for (int col = 0; col < 16; ++col) {
                                    const uint8_t c = ctx[row * 16 + col];
                                    Say("%c", (c >= 32 && c < 127) ? c : '.');
                                }
                                Say("|\n");
                            }
                        }
                        ++shown;
                        if (shown >= 5) break;
                    }
                    p = n + 1;
                    if (p >= end) break;
                }
            }
            if (shown >= 5) break;
        }
        if (!shown) Say("  (sin ocurrencias ASCII)\n");
        Say("--- fin del volcado ---\n");
    }

    // Validacion estadistica: no depende de que "None" sea el indice 0 ni del
    // orden de insercion. Exige un bloque de nombres coherente.
    bool ValidatePool(bool verbose = false) {
        // En UE5 el FNameEntryId no es un indice secuencial: son offsets en
        // unidades de 2 bytes. Se valida recorriendo entradas desde el bloque 0.
        uintptr_t blk = block0_;
        if (!blk) return false;
        int valid = 0, property = 0;
        std::wstring first, second;
        uintptr_t cur = blk;
        for (int i = 0; i < 24; ++i) {
            uint16_t h = 0;
            if (!proc_.Read(cur, &h, sizeof(h))) break;
            const bool wide = (h & 1) != 0;
            int len = (int)((h >> 6) & 0x3FF);
            if (len <= 0 || len > 512) break;
            std::wstring n;
            if (wide) {
                wchar_t buf[520]{};
                if (!proc_.Read(cur + 2, buf, (size_t)len * 2)) break;
                for (int k = 0; k < len && buf[k]; ++k) n.push_back(buf[k]);
            } else {
                char buf[520]{};
                if (!proc_.Read(cur + 2, buf, (size_t)len)) break;
                for (int k = 0; k < len && buf[k]; ++k) n.push_back((wchar_t)(unsigned char)buf[k]);
            }
            if (n.empty()) break;
            bool ok = true;
            for (wchar_t c : n) {
                const bool good = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                                  (c >= L'0' && c <= L'9') || c == L'_' || c == L'/' ||
                                  c == L'.' || c == L'-' || c == L' ' || c == L':' || c > 127;
                if (!good) { ok = false; break; }
            }
            if (!ok) break;
            ++valid;
            if (first.empty()) first = n;
            else if (second.empty()) second = n;
            if (n.find(L"Property") != std::wstring::npos) ++property;
            if (verbose) Say("      %d: %ls\n", i, n.c_str());
            cur += (uintptr_t)(2 + len);
            cur = (cur + 1) & ~(uintptr_t)1;
        }
        if (valid < 6) return false;
        if (property < 1) return false;
        if (verbose) Say("    entradas validas: %d, *Property: %d\n", valid, property);
        return true;
    }

    bool Name(int32_t index, std::wstring& out) {
        out.clear();
        {
            std::lock_guard<std::mutex> lock(nameMtx_);
            auto it = nameCache_.find(index);
            if (it != nameCache_.end()) { out = it->second; return true; }
        }
        const uint32_t block = (uint32_t)index >> 16;
        const uint32_t offset = (uint32_t)index & 0xFFFF;
        if (block > 8191) return false;
        uintptr_t addr = 0;
        if (!proc_.ReadPtr(pool_ + (uintptr_t)block * 8, addr) || !addr) return false;
        const uintptr_t entry = addr + (uintptr_t)offset * 2;
        uint16_t h = 0;
        if (!proc_.Read(entry, &h, sizeof(h))) return false;
        const bool wide = (h & 1) != 0;
        int len = (int)((h >> 6) & 0x3FF);
        if (len <= 0 || len > 512) len = 255;
        if (wide) {
            wchar_t buf[520]{};
            if (!proc_.Read(entry + 2, buf, (size_t)len * 2)) return false;
            for (int i = 0; i < len && buf[i]; ++i) out.push_back(buf[i]);
        } else {
            char buf[520]{};
            if (!proc_.Read(entry + 2, buf, (size_t)len)) return false;
            for (int i = 0; i < len && buf[i]; ++i) out.push_back((wchar_t)(unsigned char)buf[i]);
        }
        if (out.empty()) return false;
        std::lock_guard<std::mutex> lock(nameMtx_);
        nameCache_[index] = out;
        return true;
    }

    std::wstring ObjName(uintptr_t obj) {
        int32_t idx = 0;
        if (!proc_.ReadT(obj + kOffName, idx)) return L"";
        std::wstring n;
        if (!Name(idx, n)) return L"";
        return n;
    }

    std::wstring ClassName(uintptr_t obj) {
        uintptr_t cls = 0;
        if (!proc_.ReadPtr(obj + kOffClass, cls) || !cls) return L"";
        if (cls < 0x10000) return L"";
        return ObjName(cls);
    }

    // --------------------------------------------------------------- objects ---
    // Objetos vivos: primer qword apunta a una vtable en .rdata del modulo.
    std::vector<uintptr_t> ScanObjects() {
        const double t0 = Now();
        std::vector<Region> targets;
        size_t totalBytes = 0;
        for (const auto& r : regions_) {
            if (!r.writable || !r.Heap() || r.size < 0x1000) continue;
            targets.push_back(r);
            totalBytes += r.size;
        }
        std::atomic<size_t> next{ 0 };
        std::atomic<uint64_t> scanned{ 0 };
        std::atomic<size_t> found{ 0 };
        std::atomic<bool> abort{ false };
        std::atomic<bool> done{ false };
        std::mutex outMtx;
        std::vector<uintptr_t> hits;
        hits.reserve(1 << 20);

        constexpr size_t kMaxCandidates = 40000000;

        std::thread monitor([&]() {
            while (!done.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                if (done.load()) break;
                const uint64_t cur = scanned.load();
                Say("    ... %.2f / %.2f GB (%.0f%%)  candidatos=%zu  %.0fs\n",
                    cur / 1e9, totalBytes / 1e9,
                    totalBytes ? 100.0 * (double)cur / (double)totalBytes : 0.0,
                    found.load(), Now() - t0);
            }
        });

        unsigned threads = std::max(2u, std::min(12u, std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([&]() {
                std::vector<uintptr_t> local;
                for (;;) {
                    if (abort.load()) break;
                    size_t i = next.fetch_add(1);
                    if (i >= targets.size()) break;
                    ScanRegion(targets[i], 8 << 20, [&](uintptr_t addr, uint8_t* b, size_t len) {
                        for (size_t k = 0; k + 8 <= len; k += 8) {
                            uint64_t v = 0;
                            std::memcpy(&v, b + k, 8);
                            if (v < vtableLo_ || v >= vtableHi_ || (v & 0x7)) continue;
                            local.push_back(addr + k);
                        }
                        scanned.fetch_add(len);
                        if (local.size() > 250000) {
                            std::lock_guard<std::mutex> lock(outMtx);
                            hits.insert(hits.end(), local.begin(), local.end());
                            found.fetch_add(local.size());
                            local.clear();
                            if (found.load() > kMaxCandidates) abort.store(true);
                        }
                    });
                }
                std::lock_guard<std::mutex> lock(outMtx);
                hits.insert(hits.end(), local.begin(), local.end());
                found.fetch_add(local.size());
            });
        }
        for (auto& th : pool) th.join();
        done.store(true);
        if (monitor.joinable()) monitor.join();
        Say("[+] candidatos a objeto: %zu  (%.2fs)%s\n", hits.size(), Now() - t0,
            abort.load() ? "  [tope alcanzado]" : "");
        return hits;
    }

    std::vector<uintptr_t> FilterByClass(const std::vector<uintptr_t>& candidates, const wchar_t* className) {
        const double t0 = Now();
        std::atomic<size_t> next{ 0 };
        std::atomic<size_t> processed{ 0 };
        std::atomic<bool> done{ false };
        std::mutex outMtx;
        std::vector<uintptr_t> hits;
        unsigned threads = std::max(2u, std::min(12u, std::thread::hardware_concurrency()));

        std::thread monitor([&]() {
            while (!done.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                if (done.load()) break;
                Say("    ... %zu / %zu candidatos  %.0fs\n", processed.load(), candidates.size(), Now() - t0);
            }
        });

        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([&]() {
                for (;;) {
                    size_t i = next.fetch_add(1);
                    if (i >= candidates.size()) break;
                    std::wstring cls = ClassName(candidates[i]);
                    processed.fetch_add(1);
                    if (cls == className) {
                        std::lock_guard<std::mutex> lock(outMtx);
                        hits.push_back(candidates[i]);
                    }
                }
            });
        }
        for (auto& th : pool) th.join();
        done.store(true);
        if (monitor.joinable()) monitor.join();
        Say("[+] '%ls': %zu objetos  (%.2fs)\n", className, hits.size(), Now() - t0);
        return hits;
    }

    uintptr_t FindGlobalPointerTo(uintptr_t target) {
        for (const auto& r : regions_) {
            if (!r.writable) continue;
            if (r.base < moduleBase_ || r.base >= moduleBase_ + moduleSize_) continue;
            std::vector<uint8_t> buf(std::min<size_t>(r.size, 64 << 20));
            for (size_t pos = 0; pos < r.size; pos += buf.size()) {
                size_t len = std::min(buf.size(), r.size - pos);
                if (!proc_.Read(r.base + pos, buf.data(), len)) continue;
                for (size_t k = 0; k + 8 <= len; k += 8) {
                    uint64_t v = 0;
                    std::memcpy(&v, buf.data() + k, 8);
                    if (v == target) return r.base + pos + k;
                }
            }
        }
        return 0;
    }

    // Busca el valor en toda la memoria escribible (para punteros en heap).
    uintptr_t FindPointerAnywhere(uintptr_t target) {
        std::vector<Region> targets2;
        size_t totalBytes = 0;
        for (const auto& r : regions_) {
            if (r.writable && r.type != MEM_IMAGE && r.size >= 0x1000) {
                targets2.push_back(r);
                totalBytes += r.size;
            }
        }
        Say("    buscando puntero a 0x%llX en %.2f GB ...\n",
            (unsigned long long)target, totalBytes / 1e9);
        const double t0 = Now();
        std::atomic<size_t> next{ 0 };
        std::atomic<uintptr_t> found{ 0 };
        std::atomic<uint64_t> st{ 0 };
        std::atomic<bool> fin{ false };
        std::thread mon([&]() {
            while (!fin.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                if (fin.load()) break;
                Say("    ... %.2f / %.2f GB  %.0fs\n", st.load() / 1e9, totalBytes / 1e9, Now() - t0);
            }
        });
        unsigned threads = std::max(2u, std::min(12u, std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([&]() {
                for (;;) {
                    if (found.load()) break;
                    size_t i = next.fetch_add(1);
                    if (i >= targets2.size()) break;
                    ScanRegion(targets2[i], 8 << 20, [&](uintptr_t addr, uint8_t* b, size_t len) {
                        st.fetch_add(len);
                        for (size_t k = 0; k + 8 <= len; k += 8) {
                            uint64_t v = 0;
                            std::memcpy(&v, b + k, 8);
                            if (v == target) { found.store(addr + k); return; }
                        }
                    });
                }
            });
        }
        for (auto& th : pool) th.join();
        fin.store(true);
        if (mon.joinable()) mon.join();
        return found.load();
    }

    uintptr_t FindPointerByClass(uintptr_t obj, size_t span, const wchar_t* className, size_t* offsetOut) {
        std::vector<uint8_t> buf(std::min<size_t>(span, 0x4000));
        if (!proc_.Read(obj, buf.data(), buf.size())) return 0;
        for (size_t off = 0; off + 8 <= buf.size(); off += 8) {
            uintptr_t ptr = 0;
            std::memcpy(&ptr, buf.data() + off, 8);
            if (ptr < 0x10000 || (ptr & 0x7)) continue;
            std::wstring cls = ClassName(ptr);
            if (cls.empty()) continue;
            if (cls.find(className) != std::wstring::npos) {
                if (offsetOut) *offsetOut = off;
                return ptr;
            }
        }
        return 0;
    }

    std::vector<ArrayField> FindArrays(uintptr_t obj, size_t span) {
        std::vector<ArrayField> out;
        std::vector<uint8_t> buf(std::min<size_t>(span, 0x4000));
        if (!proc_.Read(obj, buf.data(), buf.size())) return out;
        for (size_t off = 0; off + 16 <= buf.size(); off += 8) {
            uint64_t data = 0;
            int32_t num = 0, max = 0;
            std::memcpy(&data, buf.data() + off, 8);
            std::memcpy(&num, buf.data() + off + 8, 4);
            std::memcpy(&max, buf.data() + off + 12, 4);
            if (data < 0x10000 || (data & 0x7)) continue;
            if (num <= 0 || num > 500000 || max < num || max > 500000) continue;
            uintptr_t first = 0;
            if (!proc_.ReadPtr(data, first) || first < 0x10000) continue;
            if (ClassName(first).empty()) continue;
            out.push_back({ (uintptr_t)data, num, max, off });
        }
        return out;
    }

    // FTransform3d: FQuat4d (32B) + Translation (24B) + Scale3D (24B)
    // FTransform. Dos layouts posibles:
    //   A: FQuat4d (32B) + FVector3d + FVector3d  -> Translation en +0x20
    //   B: FQuat4f (16B) + FVector3d + FVector3d  -> Translation en +0x10
    size_t transOff_ = 0x20;

    bool FindTransform(uintptr_t obj, size_t span, size_t& offsetOut, Vec3& translationOut) {
        std::vector<uint8_t> buf(std::min<size_t>(span, 0x4000));
        if (!proc_.Read(obj, buf.data(), buf.size())) return false;
        for (size_t off = 0; off + 64 <= buf.size(); off += 8) {
            // layout A: cuaternion en doubles
            {
                double q[4];
                std::memcpy(q, buf.data() + off, 32);
                const double norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                if (norm > 0.99 && norm < 1.01) {
                    double tr[3], sc[3];
                    std::memcpy(tr, buf.data() + off + 32, 24);
                    std::memcpy(sc, buf.data() + off + 56, 24);
                    bool ok = true;
                    for (int i = 0; i < 3; ++i) {
                        if (!std::isfinite(tr[i]) || std::fabs(tr[i]) > 1e8) ok = false;
                        if (!std::isfinite(sc[i]) || sc[i] < 1e-3 || sc[i] > 1e3) ok = false;
                    }
                    if (ok && !(std::fabs(tr[0]) < 100.0 && std::fabs(tr[1]) < 100.0)) {
                        offsetOut = off;
                        transOff_ = 0x20;
                        translationOut = Vec3(tr[0], tr[1], tr[2]);
                        return true;
                    }
                }
            }
            // layout B: cuaternion en floats, vectores en doubles
            {
                float q[4];
                std::memcpy(q, buf.data() + off, 16);
                const double norm = std::sqrt((double)q[0] * q[0] + (double)q[1] * q[1] +
                                              (double)q[2] * q[2] + (double)q[3] * q[3]);
                if (!(norm > 0.99 && norm < 1.01)) continue;
                double tr[3], sc[3];
                std::memcpy(tr, buf.data() + off + 16, 24);
                std::memcpy(sc, buf.data() + off + 40, 24);
                bool ok = true;
                for (int i = 0; i < 3; ++i) {
                    if (!std::isfinite(tr[i]) || std::fabs(tr[i]) > 1e8) ok = false;
                    if (!std::isfinite(sc[i]) || sc[i] < 1e-3 || sc[i] > 1e3) ok = false;
                }
                if (ok && !(std::fabs(tr[0]) < 100.0 && std::fabs(tr[1]) < 100.0)) {
                    offsetOut = off;
                    transOff_ = 0x10;
                    translationOut = Vec3(tr[0], tr[1], tr[2]);
                    return true;
                }
            }
        }
        return false;
    }

    // FMinimalViewInfo: Location (24B) + Rotation (24B) + FOV (float)
    bool FindPov(uintptr_t obj, size_t span, size_t& offsetOut, Vec3& loc, Vec3& rot, double& fov) {
        std::vector<uint8_t> buf(std::min<size_t>(span, 0x10000));
        if (!proc_.Read(obj, buf.data(), buf.size())) return false;
        for (size_t off = 0; off + 56 <= buf.size(); off += 8) {
            double v[6];
            std::memcpy(v, buf.data() + off, 48);
            float f = 0;
            std::memcpy(&f, buf.data() + off + 48, 4);
            if (!(f > 30.0f && f < 150.0f)) continue;
            bool ok = true;
            for (int i = 0; i < 3; ++i) if (!std::isfinite(v[i]) || std::fabs(v[i]) > 1e8) ok = false;
            if (!ok) continue;
            if (std::fabs(v[0]) < 100.0 && std::fabs(v[1]) < 100.0) continue;
            if (!(std::fabs(v[3]) <= 90.0 && std::fabs(v[4]) <= 360.0 && std::fabs(v[5]) <= 360.0)) continue;
            offsetOut = off;
            loc = Vec3(v[0], v[1], v[2]);
            rot = Vec3(v[3], v[4], v[5]);
            fov = f;
            return true;
        }
        return false;
    }

    Process& proc() { return proc_; }
    size_t translationOffset() const { return transOff_; }

    // Descubre por fuerza bruta el campo del actor que lleva a un componente con
    // posicion de mundo valida, y el offset de esa posicion en el componente.
    // Ancla en la camara del jugador: busca (campo del actor, offset dentro del
    // componente) tal que ALGUN actor quede a menos de 300 cm de la camara.
    // Ese actor es el pawn local: su posicion es la que se mueve de verdad.
    bool DiscoverByCamera(const Vec3& cam, const std::vector<uintptr_t>& sampleActors,
                          size_t& actorOffOut, size_t& transOffOut, Vec3& posOut, uintptr_t& actorOut) {
        int bestScore = 0;
        size_t bestA = 0, bestT = 0;
        Vec3 bestP;
        uintptr_t bestActor = 0;
        for (size_t aoff = 0; aoff <= 0x600; aoff += 8) {
            std::vector<uintptr_t> comps, owners;
            for (uintptr_t a : sampleActors) {
                uintptr_t c = 0;
                if (!proc_.ReadPtr(a + aoff, c) || c < 0x10000) continue;
                if (ClassName(c).empty()) continue;
                comps.push_back(c);
                owners.push_back(a);
                if (comps.size() >= 48) break;
            }
            if (comps.size() < 4) continue;
            std::vector<std::vector<uint8_t>> blobs(comps.size());
            for (size_t i = 0; i < comps.size(); ++i) {
                blobs[i].resize(0x800);
                if (!proc_.Read(comps[i], blobs[i].data(), blobs[i].size())) blobs[i].clear();
            }
            for (size_t t = 0; t + 24 <= 0x800; t += 8) {
                for (size_t i = 0; i < blobs.size(); ++i) {
                    if (blobs[i].empty()) continue;
                    double v[3];
                    std::memcpy(v, blobs[i].data() + t, 24);
                    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
                    if (std::fabs(v[0]) > 1e6 || std::fabs(v[1]) > 1e6 || std::fabs(v[2]) > 1e6) continue;
                    const double dx = v[0] - cam.x, dy = v[1] - cam.y, dz = v[2] - cam.z;
                    const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (d > 300.0) continue;
                    const int score = (int)(1000.0 - d);
                    if (score > bestScore) {
                        bestScore = score;
                        bestA = aoff;
                        bestT = t;
                        bestP = Vec3(v[0], v[1], v[2]);
                        bestActor = owners[i];
                    }
                    break;
                }
            }
        }
        if (!bestScore) return false;
        actorOffOut = bestA;
        transOffOut = bestT;
        posOut = bestP;
        actorOut = bestActor;
        return true;
    }

    struct PovCand { size_t mgrOff = 0; size_t povOff = 0; Vec3 loc, rot; double fov = 0; uintptr_t mgr = 0; };

    // Ancla en el pawn del jugador: busca (offset en el actor, offset dentro del
    // componente) que dejan al pawn a menos de 300 cm de la camara.
    bool DiscoverFromPawns(const std::vector<uintptr_t>& pawns, const std::vector<PovCand>& povs,
                           size_t& povIndexOut, size_t& actorOffOut, size_t& transOffOut,
                           Vec3& posOut, uintptr_t& pawnOut, double& distOut) {
        int bestScore = 0;
        size_t bestPov = 0, bestA = 0, bestT = 0;
        Vec3 bestPos;
        uintptr_t bestPawn = 0;
        double bestDist = 1e9;
        for (uintptr_t pawn : pawns) {
            for (size_t aoff = 0; aoff <= 0x600; aoff += 8) {
                uintptr_t comp = 0;
                if (!proc_.ReadPtr(pawn + aoff, comp) || comp < 0x10000) continue;
                if (ClassName(comp).empty()) continue;
                std::vector<uint8_t> blob(0x800);
                if (!proc_.Read(comp, blob.data(), blob.size())) continue;
                for (size_t t = 0; t + 24 <= blob.size(); t += 8) {
                    double v[3];
                    std::memcpy(v, blob.data() + t, 24);
                    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
                    if (std::fabs(v[0]) > 1e6 || std::fabs(v[1]) > 1e6 || std::fabs(v[2]) > 1e6) continue;
                    for (size_t pi = 0; pi < povs.size(); ++pi) {
                        const Vec3& c = povs[pi].loc;
                        const double dx = v[0] - c.x, dy = v[1] - c.y, dz = v[2] - c.z;
                        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (d > 300.0) continue;
                        const int score = (int)(1000.0 - d);
                        if (score > bestScore) {
                            bestScore = score;
                            bestPov = pi;
                            bestA = aoff;
                            bestT = t;
                            bestPos = Vec3(v[0], v[1], v[2]);
                            bestPawn = pawn;
                            bestDist = d;
                        }
                    }
                }
            }
        }
        if (!bestScore) return false;
        povIndexOut = bestPov;
        actorOffOut = bestA;
        transOffOut = bestT;
        posOut = bestPos;
        pawnOut = bestPawn;
        distOut = bestDist;
        return true;
    }

    bool CollectPovs(uintptr_t pc, std::vector<PovCand>& out) {
        std::vector<uint8_t> buf(0x8000);
        if (!proc_.Read(pc, buf.data(), buf.size())) return false;
        for (size_t off = 0; off + 8 <= buf.size() && out.size() < 10; off += 8) {
            uintptr_t cand = 0;
            std::memcpy(&cand, buf.data() + off, 8);
            if (cand < 0x10000 || (cand & 7)) continue;
            const std::wstring cls = ClassName(cand);
            if (cls.find(L"Camera") == std::wstring::npos) continue;
            size_t povOff = 0;
            Vec3 l, r;
            double f = 0;
            if (!FindPov(cand, 0x20000, povOff, l, r, f)) continue;
            PovCand c;
            c.mgrOff = off;
            c.povOff = povOff;
            c.loc = l;
            c.rot = r;
            c.fov = f;
            c.mgr = cand;
            out.push_back(c);
        }
        return !out.empty();
    }

    // Busqueda conjunta: elige la pareja (camara, offsets de posicion) que hace
    // que algun actor quede a menos de 300 cm de la camara.
    bool JointDiscover(const std::vector<PovCand>& povs, const std::vector<uintptr_t>& actors,
                       size_t& povIndexOut, size_t& actorOffOut, size_t& transOffOut,
                       Vec3& posOut, uintptr_t& pawnOut, double& distOut) {
        int bestScore = 0;
        size_t bestPov = 0, bestA = 0, bestT = 0;
        Vec3 bestP;
        uintptr_t bestPawn = 0;
        double bestDist = 1e9;
        std::vector<uintptr_t> sample(actors.begin(), actors.begin() + std::min<size_t>(actors.size(), 32));

        for (size_t aoff = 0; aoff <= 0x600; aoff += 8) {
            std::vector<uintptr_t> comps, owners;
            for (uintptr_t a : sample) {
                uintptr_t c = 0;
                if (!proc_.ReadPtr(a + aoff, c) || c < 0x10000) continue;
                if (ClassName(c).empty()) continue;
                comps.push_back(c);
                owners.push_back(a);
            }
            if (comps.size() < 4) continue;
            std::vector<std::vector<uint8_t>> blobs(comps.size());
            for (size_t i = 0; i < comps.size(); ++i) {
                blobs[i].resize(0x800);
                if (!proc_.Read(comps[i], blobs[i].data(), blobs[i].size())) blobs[i].clear();
            }
            for (size_t t = 0; t + 24 <= 0x800; t += 8) {
                for (size_t i = 0; i < blobs.size(); ++i) {
                    if (blobs[i].empty()) continue;
                    double v[3];
                    std::memcpy(v, blobs[i].data() + t, 24);
                    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
                    if (std::fabs(v[0]) > 1e6 || std::fabs(v[1]) > 1e6 || std::fabs(v[2]) > 1e6) continue;
                    for (size_t pi = 0; pi < povs.size(); ++pi) {
                        const Vec3& c = povs[pi].loc;
                        const double dx = v[0] - c.x, dy = v[1] - c.y, dz = v[2] - c.z;
                        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (d > 300.0) continue;
                        const int score = (int)(1000.0 - d);
                        if (score > bestScore) {
                            bestScore = score;
                            bestPov = pi;
                            bestA = aoff;
                            bestT = t;
                            bestP = Vec3(v[0], v[1], v[2]);
                            bestPawn = owners[i];
                            bestDist = d;
                        }
                    }
                }
            }
        }
        if (!bestScore) return false;
        povIndexOut = bestPov;
        actorOffOut = bestA;
        transOffOut = bestT;
        posOut = bestP;
        pawnOut = bestPawn;
        distOut = bestDist;
        return true;
    }

    bool DiscoverActorTransform(const std::vector<uintptr_t>& peers, size_t& actorOffOut,
                                size_t& transOffOut, Vec3& sampleOut, int& countOut,
                                const Vec3& camHint) {
        // Una posicion de mundo real: finita, de magnitud de mapa, sin ceros
        // exactos, y a una distancia razonable de la camara del jugador.
        auto plausible = [&](double x, double y, double z) {
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
            if (std::fabs(x) > 1e6 || std::fabs(y) > 1e6 || std::fabs(z) > 1e6) return false;
            if (std::fabs(x) < 1.0 || std::fabs(y) < 1.0 || std::fabs(z) < 1.0) return false;
            const double dx = x - camHint.x, dy = y - camHint.y, dz = z - camHint.z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > 2.0e6) return false;
            return true;
        };
        // Preferencia por clase de componente: la malla es la que lleva la
        // transformada del actor; un SphereComponent cualquiera no.
        auto pref = [](const std::wstring& cls) -> int {
            if (cls.find(L"SkeletalMeshComponent") != std::wstring::npos) return 4;
            if (cls.find(L"StaticMeshComponent") != std::wstring::npos) return 3;
            if (cls.find(L"SceneComponent") != std::wstring::npos) return 2;
            if (cls.find(L"Component") != std::wstring::npos) return 1;
            return 0;
        };
        int bestScore = 0;
        size_t bestActorOff = 0, bestTransOff = 0;
        Vec3 bestPos;
        for (size_t aoff = 0; aoff <= 0x600; aoff += 8) {
            std::vector<uintptr_t> comps;
            int classPref = 0;
            for (uintptr_t p : peers) {
                uintptr_t c = 0;
                if (!proc_.ReadPtr(p + aoff, c) || c < 0x10000) continue;
                const std::wstring cls = ClassName(c);
                if (cls.empty()) continue;
                const int pr = pref(cls);
                if (pr == 0) continue;
                if (classPref == 0) classPref = pr;
                else if (pr < classPref) continue;   // mezcla de clases: descarta
                comps.push_back(c);
                if (comps.size() >= 8) break;
            }
            if (comps.size() < 5) continue;
            std::vector<std::vector<uint8_t>> blobs(comps.size());
            for (size_t i = 0; i < comps.size(); ++i) {
                blobs[i].resize(0x800);
                if (!proc_.Read(comps[i], blobs[i].data(), blobs[i].size())) blobs[i].clear();
            }
            for (size_t t = 0; t + 24 <= 0x800; t += 8) {
                int ok = 0;
                Vec3 first;
                bool distinct = false;
                for (size_t i = 0; i < blobs.size(); ++i) {
                    if (blobs[i].empty()) continue;
                    double v[3];
                    std::memcpy(v, blobs[i].data() + t, 24);
                    if (!plausible(v[0], v[1], v[2])) continue;
                    const Vec3 p(v[0], v[1], v[2]);
                    if (ok > 0 && (std::fabs(p.x - first.x) > 1.0 || std::fabs(p.y - first.y) > 1.0)) distinct = true;
                    if (ok == 0) first = p;
                    ++ok;
                }
                if (ok < 5 || ok != (int)blobs.size() || !distinct) continue;
                const int score = classPref * 100 + ok;
                if (score > bestScore) {
                    bestScore = score;
                    bestActorOff = aoff;
                    bestTransOff = t;
                    bestPos = first;
                }
            }
        }
        if (bestScore == 0) return false;
        actorOffOut = bestActorOff;
        transOffOut = bestTransOff;
        sampleOut = bestPos;
        countOut = bestScore % 100;
        return true;
    }

private:
    void ReadSections() {
        uint8_t hdr[0x1000]{};
        if (!proc_.Read(moduleBase_, hdr, sizeof(hdr))) return;
        const uint32_t e_lfanew = *(uint32_t*)(hdr + 0x3C);
        if (e_lfanew + 0x108 > sizeof(hdr)) return;
        const uint16_t numSections = *(uint16_t*)(hdr + e_lfanew + 6);
        const uint16_t optSize = *(uint16_t*)(hdr + e_lfanew + 20);
        const uint32_t sizeOfImage = *(uint32_t*)(hdr + e_lfanew + 24 + 56);

        uintptr_t imageEnd = moduleBase_ + (sizeOfImage ? sizeOfImage : (uint32_t)moduleSize_);
        if (sizeOfImage > 0x1000 && sizeOfImage < 0x80000000u) moduleSize_ = sizeOfImage;

        size_t secOff = e_lfanew + 24 + optSize;
        uintptr_t lo = 0, hi = 0;
        for (uint16_t i = 0; i < numSections; ++i, secOff += 40) {
            if (secOff + 40 > sizeof(hdr)) break;
            char name[9]{};
            std::memcpy(name, hdr + secOff, 8);
            const uint32_t vsize = *(uint32_t*)(hdr + secOff + 8);
            const uint32_t vaddr = *(uint32_t*)(hdr + secOff + 12);
            if (!vsize) continue;
            const uintptr_t start = moduleBase_ + vaddr;
            uintptr_t end = start + vsize;
            if (end > imageEnd) end = imageEnd;
            if (end <= start) continue;
            if (std::strcmp(name, ".rdata") == 0) { lo = start; hi = end; }
        }
        if (lo && hi && hi > lo && lo >= moduleBase_ && hi <= imageEnd) {
            vtableLo_ = lo;
            vtableHi_ = hi;
        } else {
            vtableLo_ = moduleBase_;
            vtableHi_ = imageEnd;
        }
        Say("[+] vtables: 0x%llX - 0x%llX  (imagen hasta 0x%llX)\n",
            (unsigned long long)vtableLo_, (unsigned long long)vtableHi_, (unsigned long long)imageEnd);
    }

    void ClassifyRegions() {
        struct Agg { uint64_t total = 0, readable = 0; int count = 0; };
        Agg priv, mapped, image;
        int e299 = 0, e5 = 0, e487 = 0, eOther = 0;
        int shownFail = 0;
        for (const auto& r : regions_) {
            Agg& a = (r.type == MEM_PRIVATE) ? priv : (r.type == MEM_MAPPED ? mapped : image);
            a.total += r.size;
            a.count += 1;
            uint8_t probe[16]{};
            bool ok = proc_.Read(r.base, probe, sizeof(probe));
            if (!ok && r.size > 0x1000) ok = proc_.Read(r.base + r.size - 0x1000, probe, sizeof(probe));
            if (ok) {
                a.readable += r.size;
            } else {
                const DWORD e = ::GetLastError();
                if (e == 299) ++e299;
                else if (e == 5) ++e5;
                else if (e == 487) ++e487;
                else ++eOther;
                if (shownFail < 6) {
                    Say("    falla: base=0x%llX size=%.1f MB tipo=%s prot=0x%X err=%lu\n",
                        (unsigned long long)r.base, r.size / 1048576.0,
                        r.type == MEM_PRIVATE ? "PRIVATE" : (r.type == MEM_MAPPED ? "MAPPED" : "IMAGE"),
                        r.protect, (unsigned long)e);
                    uint8_t big[4096]{};
                    const size_t sizes[] = { 16, 64, 256, 1024, 4096 };
                    Say("      escalera:");
                    for (size_t s : sizes) {
                        Say(" %zu=%s", s, proc_.Read(r.base, big, s) ? "ok" : "no");
                    }
                    Say("   (offset+0x1000: 4096=%s)\n",
                        r.size > 0x2000 && proc_.Read(r.base + 0x1000, big, 4096) ? "ok" : "no");
                    ++shownFail;
                }
            }
        }
        Say("[+] mapa por tipo:\n");
        Say("    PRIVATE regiones=%-6d total=%6.2f GB  legible~%6.2f GB\n",
            priv.count, priv.total / 1073741824.0, priv.readable / 1073741824.0);
        Say("    MAPPED  regiones=%-6d total=%6.2f GB  legible~%6.2f GB\n",
            mapped.count, mapped.total / 1073741824.0, mapped.readable / 1073741824.0);
        Say("    IMAGE   regiones=%-6d total=%6.2f GB  legible~%6.2f GB\n",
            image.count, image.total / 1073741824.0, image.readable / 1073741824.0);
        Say("    errores: 299(parcial)=%d  5(denegado)=%d  487(dir)=%d  otros=%d\n",
            e299, e5, e487, eOther);
    }

    void EnumerateRegions() {
        uintptr_t addr = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        while (::VirtualQueryEx(proc_.handle(), (LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            const uintptr_t base = (uintptr_t)mbi.BaseAddress;
            const size_t size = mbi.RegionSize;
            if (!size) break;
            const DWORD p = mbi.Protect & 0xFF;
            if (mbi.State == MEM_COMMIT) {
                const bool writable = (p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
                                       p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY);
                if (writable) {
                    regions_.push_back({ base, size, true, mbi.Type, mbi.Protect });
                    ranges_.push_back({ base, base + size });
                }
            }
            addr = base + size;
            if (addr < base) break;
        }
        std::sort(ranges_.begin(), ranges_.end());
    }

    bool InWritable(uintptr_t addr) const {
        if (ranges_.empty() || addr < ranges_.front().first) return false;
        auto it = std::upper_bound(ranges_.begin(), ranges_.end(), addr,
                                   [](uintptr_t a, const std::pair<uintptr_t, uintptr_t>& r) { return a < r.first; });
        if (it == ranges_.begin()) return false;
        --it;
        return addr >= it->first && addr < it->second;
    }

    Process proc_;
    uintptr_t moduleBase_ = 0;
    size_t moduleSize_ = 0;
    uintptr_t vtableLo_ = 0, vtableHi_ = 0;
    uintptr_t block0_ = 0;
    uintptr_t poolSlot_ = 0;
    uintptr_t pool_ = 0;
    uintptr_t textOff_ = 2;
    uintptr_t gnamesSlot_ = 0;
    int gnamesInd_ = 0;
    std::vector<Region> regions_;
    std::vector<std::pair<uintptr_t, uintptr_t>> ranges_;
    mutable std::atomic<uint64_t> stPrimaryOk{ 0 };
    mutable std::atomic<uint64_t> stPrimaryFail{ 0 };
    mutable std::atomic<uint64_t> stPageOk{ 0 };
    mutable std::atomic<uint64_t> stPageFail{ 0 };
    std::unordered_map<int32_t, std::wstring> nameCache_;
    std::mutex nameMtx_;
};

} // namespace

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    if (g_log) {
        std::fprintf(g_log, "[!] EXCEPCION 0x%08lX en %p\n", 
                     (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
        std::fflush(g_log);
    }
    std::printf("[!] EXCEPCION 0x%08lX en %p\n", (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char** argv) {
    ::SetUnhandledExceptionFilter(&CrashFilter);
    bool namesOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--names") == 0) namesOnly = true;
    }
    ::SetConsoleOutputCP(CP_UTF8);
    g_log = std::fopen("found_roots.txt", "w");
    if (g_log) std::setvbuf(g_log, nullptr, _IOLBF, 0);

    Finder f;
    Say("[+] find_roots build %s %s\n", __DATE__, __TIME__);
    if (!f.Open()) return 2;
    if (!f.FindNamePool()) return 3;
    if (namesOnly) return 0;

    std::vector<uintptr_t> candidates = f.ScanObjects();
    std::vector<uintptr_t> worlds = f.FilterByClass(candidates, L"World");
    if (worlds.empty()) {
        Say("[x] sin UWorld. Confirma que estas en partida.\n");
        return 4;
    }

    uintptr_t world = 0, level = 0;
    size_t levelOffset = 0, actorsOffset = 0;
    int32_t actorCount = 0;
    uintptr_t actorsData = 0;
    int32_t bestScore = -1;

    // Eligir el nivel persistente real: el de mayor cantidad de actores.
    // Los subniveles de streaming tienen cientos; el persistente tiene miles.
    int tried = 0;
    for (uintptr_t w : worlds) {
        if (++tried > 60) break;
        std::vector<uint8_t> wbuf(0x2000);
        if (!f.proc().Read(w, wbuf.data(), wbuf.size())) continue;
        for (size_t off = 0; off + 8 <= wbuf.size(); off += 8) {
            uintptr_t p = 0;
            std::memcpy(&p, wbuf.data() + off, 8);
            if (p < 0x10000 || (p & 7)) continue;
            if (f.ClassName(p) != L"Level") continue;
            for (const auto& a : f.FindArrays(p, 0x2000)) {
                if (a.num < 300) continue;
                // Puntua por actores de tipo Character: el nivel persistente los
                // tiene a cientos; un nivel de HLOD no tiene ninguno.
                int charHits = 0;
                {
                    std::vector<uint64_t> probe(64, 0);
                    if (f.proc().Read(a.data, probe.data(), probe.size() * 8)) {
                        for (uint64_t e2 : probe) {
                            if (!e2) continue;
                            if (f.ClassName((uintptr_t)e2).find(L"Character") != std::wstring::npos) ++charHits;
                        }
                    }
                }
                const int score = charHits * 100000 + a.num;
                if (score <= bestScore) continue;
                std::vector<uint64_t> first(8, 0);
                if (!f.proc().Read(a.data, first.data(), first.size() * 8)) continue;
                int okEl = 0;
                for (uint64_t e : first) {
                    if (e && !f.ClassName((uintptr_t)e).empty()) ++okEl;
                }
                if (okEl < 6) continue;
                bestScore = score;
                world = w;
                level = p;
                levelOffset = off;
                actorsOffset = a.offset;
                actorCount = a.num;
                actorsData = a.data;
            }
        }
    }
    if (!world) {
        Say("[x] ningun World con Level y actores validos. Mundos vistos: %zu\n", worlds.size());
        for (size_t i = 0; i < worlds.size() && i < 10; ++i)
            Say("    world 0x%llX  %ls\n", (unsigned long long)worlds[i], f.ObjName(worlds[i]).c_str());
        return 5;
    }

    Say("[+] UWorld: 0x%llX (%ls)\n", (unsigned long long)world, f.ObjName(world).c_str());
    Say("[+] World.PersistentLevel = 0x%zX  -> Level 0x%llX\n", levelOffset, (unsigned long long)level);
    Say("[+] Level.Actors = 0x%zX  (%d entradas)\n", actorsOffset, actorCount);

    uintptr_t gworldSlot = f.FindGlobalPointerTo(world);
    if (!gworldSlot) {
        Say("    global no esta en .data; barriendo memoria ...\n");
        uintptr_t anySlot = f.FindPointerAnywhere(world);
        if (anySlot) {
            uintptr_t modSlot = f.FindGlobalPointerTo(anySlot);
            gworldSlot = modSlot ? modSlot : anySlot;
            if (!modSlot) Say("[!] GWorld slot en heap: cambia por arranque.\n");
        }
    }
    if (gworldSlot) Say("[+] GWorldSlot = 0x%llX\n", (unsigned long long)gworldSlot);
    else Say("[!] sin global apuntando a UWorld.\n");

    // --- histograma de clases --------------------------------------------
    std::vector<uint64_t> ptrs((size_t)std::min(actorCount, 6000));
    f.proc().Read(actorsData, ptrs.data(), ptrs.size() * 8);
    std::map<std::wstring, int> hist;
    for (uint64_t p : ptrs) {
        if (!p) continue;
        std::wstring cls = f.ClassName((uintptr_t)p);
        if (!cls.empty()) hist[cls] += 1;
    }
    std::vector<std::pair<std::wstring, int>> sorted(hist.begin(), hist.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    Say("[+] clases mas frecuentes:\n");
    for (size_t i = 0; i < sorted.size() && i < 15; ++i)
        Say("      %-56ls x%d\n", sorted[i].first.c_str(), sorted[i].second);

    // --- muestra: preferir un dino de verdad (excluye buffs, items, volumenes) ---
    uintptr_t sampleActor = 0;
    std::wstring sampleClass;
    auto looksDino = [](const std::wstring& s) {
        if (s.find(L"Buff") != std::wstring::npos) return false;
        if (s.find(L"Weapon") != std::wstring::npos) return false;
        if (s.find(L"Item") != std::wstring::npos) return false;
        if (s.find(L"Structure") != std::wstring::npos) return false;
        if (s.find(L"Volume") != std::wstring::npos) return false;
        if (s.find(L"Zone") != std::wstring::npos) return false;
        return s.find(L"Character_BP") != std::wstring::npos ||
               s.find(L"Character") != std::wstring::npos;
    };
    for (const auto& kv : sorted) {
        if (!looksDino(kv.first)) continue;
        for (uint64_t p : ptrs) {
            if (!p) continue;
            if (f.ClassName((uintptr_t)p) == kv.first) { sampleActor = (uintptr_t)p; sampleClass = kv.first; break; }
        }
        if (sampleActor) break;
    }

    size_t rootOffset = 0, transformOffset = 0;
    Vec3 samplePos;
    if (sampleActor) {
        Say("[+] muestra: 0x%llX  %ls\n", (unsigned long long)sampleActor, sampleClass.c_str());

        // Recolecta hasta 24 actores de la misma clase para puntuar offsets.
        std::vector<uintptr_t> peers;
        for (uint64_t p : ptrs) {
            if (!p) continue;
            if (f.ClassName((uintptr_t)p) == sampleClass) peers.push_back((uintptr_t)p);
            if (peers.size() >= 24) break;
        }

        size_t aoff = 0, toff2 = 0;
        Vec3 pos;
        int hits = 0;
        if (f.DiscoverActorTransform(peers, aoff, toff2, pos, hits, Vec3())) {
            rootOffset = aoff;
            transformOffset = toff2;      // el offset del componente ya ES la posicion
            samplePos = pos;
            uintptr_t comp = 0;
            f.proc().ReadPtr(sampleActor + aoff, comp);
            Say("[+] Actor.RootComponent = 0x%zX  (%ls)  valido en %d actores\n",
                aoff, f.ClassName(comp).c_str(), hits);
            Say("[+] Scene.ComponentToWorld = 0x%zX  Translation=+0x0  pos %.1f %.1f %.1f\n",
                toff2, pos.x, pos.y, pos.z);
        } else {
            Say("[!] no pude descubrir la posicion de los actores.\n");
        }
    } else {
        Say("[!] no hay dinosaurios en el nivel para muestrear.\n");
    }

    // --- camara -----------------------------------------------------------
    size_t giOffset = 0, lpOffset = 0, pcOffset = 0, camOffset = 0, povOffset = 0;
    Vec3 camPos;
    uintptr_t pcFound = 0;
    bool haveCam = false, byCamera = false;
    uintptr_t gi = f.FindPointerByClass(world, 0x2000, L"GameInstance", &giOffset);
    Say("[%c] World.OwningGameInstance = 0x%zX  (%ls)\n", gi ? '+' : '!', giOffset, f.ClassName(gi).c_str());
    if (gi) {
        uintptr_t lp = 0;
        for (const auto& a : f.FindArrays(gi, 0x400)) {
            if (a.num != 1) continue;
            uintptr_t elem = 0;
            if (!f.proc().ReadPtr(a.data, elem)) continue;
            std::wstring cls = f.ClassName(elem);
            if (cls.find(L"LocalPlayer") != std::wstring::npos) { lp = elem; lpOffset = a.offset; break; }
        }
        Say("[%c] GameInstance.LocalPlayers = 0x%zX  (%ls)\n", lp ? '+' : '!', lpOffset, f.ClassName(lp).c_str());
        if (lp) {
            uintptr_t pc = f.FindPointerByClass(lp, 0x800, L"PlayerController", &pcOffset);
            pcFound = pc;
            Say("    pc clase: %ls\n", f.ClassName(pc).c_str());
            {
                std::vector<uint8_t> pbuf(0x8000);
                if (f.proc().Read(pc, pbuf.data(), pbuf.size())) {
                    Say("    --- campos de pc con Controller/Camera/Pawn ---\n");
                    int shown = 0;
                    for (size_t off = 0; off + 8 <= pbuf.size() && shown < 40; off += 8) {
                        uintptr_t cand = 0;
                        std::memcpy(&cand, pbuf.data() + off, 8);
                        if (cand < 0x10000 || (cand & 7)) continue;
                        const std::wstring cls = f.ClassName(cand);
                        if (cls.empty()) continue;
                        if (cls.find(L"Camera") == std::wstring::npos &&
                            cls.find(L"Controller") == std::wstring::npos &&
                            cls.find(L"Pawn") == std::wstring::npos) continue;
                        Say("      0x%zX -> %ls\n", off, cls.c_str());
                        ++shown;
                    }
                }
            }
            Say("[%c] LocalPlayer.PlayerController = 0x%zX\n", pc ? '+' : '!', pcOffset);
            if (pc) {
                uintptr_t cam = f.FindPointerByClass(pc, 0x8000, L"PlayerCameraManager", &camOffset);
                Vec3 loc, rot;
                double fov = 0;
                // Plan B: si el nombre no aparece, buscar un objeto del PC que
                // contenga un FMinimalViewInfo con pinta de camara.
                if (!cam) {
                    std::vector<uint8_t> pbuf(0x8000);
                    if (f.proc().Read(pc, pbuf.data(), pbuf.size())) {
                        for (size_t off = 0; off + 8 <= pbuf.size() && !cam; off += 8) {
                            uintptr_t cand = 0;
                            std::memcpy(&cand, pbuf.data() + off, 8);
                            if (cand < 0x10000 || (cand & 7)) continue;
                            if (f.ClassName(cand).empty()) continue;
                            size_t povOff = 0;
                            Vec3 l2, r2;
                            double f2 = 0;
                            if (f.FindPov(cand, 0x20000, povOff, l2, r2, f2)) {
                                cam = cand;
                                camOffset = off;
                                povOffset = povOff;
                                loc = l2;
                                rot = r2;
                                fov = f2;
                                Say("    camera manager hallado por contenido, no por nombre\n");
                            }
                        }
                    }
                }
                Say("[%c] PlayerController.CameraManager = 0x%zX\n", cam ? '+' : '!', camOffset);
                if (cam) {
                    if (!povOffset && !f.FindPov(cam, 0x20000, povOffset, loc, rot, fov)) {
                        Say("[!] sin FMinimalViewInfo en el camera manager.\n");
                    } else if (povOffset) {
                        Say("[+] CameraManager.CameraCache = 0x%zX  fov %.1f\n", povOffset, fov);
                        Say("    camara %.1f %.1f %.1f  pitch %.1f yaw %.1f roll %.1f\n",
                            loc.x, loc.y, loc.z, rot.x, rot.y, rot.z);
                        camPos = loc;
                        haveCam = true;
                    }
                }
            }
        }
    }

    // --- ancla en el pawn del jugador (campo del PlayerController) ---
    if (pcFound && actorCount > 0 && actorsData) {
        std::vector<Finder::PovCand> povs;
        f.CollectPovs(pcFound, povs);
        for (size_t fixedOff : { (size_t)0x560, (size_t)0x540, (size_t)0x390 }) {
            uintptr_t mgr = 0;
            if (!f.proc().ReadPtr(pcFound + fixedOff, mgr) || mgr < 0x10000) continue;
            if (f.ClassName(mgr).empty()) continue;
            size_t povOff = 0;
            Vec3 l, r;
            double fv = 0;
            if (!f.FindPov(mgr, 0x20000, povOff, l, r, fv)) continue;
            bool dup = false;
            for (const auto& c : povs) if (c.mgrOff == fixedOff && c.povOff == povOff) dup = true;
            if (dup) continue;
            Finder::PovCand c;
            c.mgrOff = fixedOff; c.povOff = povOff; c.loc = l; c.rot = r; c.fov = fv; c.mgr = mgr;
            povs.push_back(c);
        }
        Say("[+] candidatos de camara: %zu\n", povs.size());
        for (size_t i = 0; i < povs.size(); ++i)
            Say("    #%zu mgrOff=0x%zX (%ls) povOff=0x%zX  pos %.0f %.0f %.0f  fov %.1f\n",
                i, povs[i].mgrOff, f.ClassName(povs[i].mgr).c_str(), povs[i].povOff,
                povs[i].loc.x, povs[i].loc.y, povs[i].loc.z, povs[i].fov);

        std::vector<uintptr_t> pawns;
        for (size_t pOff : { (size_t)0x4D0, (size_t)0x4E0, (size_t)0x548 }) {
            uintptr_t p = 0;
            if (!f.proc().ReadPtr(pcFound + pOff, p) || p < 0x10000) continue;
            const std::wstring cls = f.ClassName(p);
            if (cls.find(L"Pawn") == std::wstring::npos) continue;
            pawns.push_back(p);
        }
        Say("[+] pawns candidatos: %zu\n", pawns.size());
        for (uintptr_t p : pawns) Say("    pawn 0x%llX  %ls\n", (unsigned long long)p, f.ClassName(p).c_str());

        size_t pi = 0, aoff = 0, toff3 = 0;
        Vec3 pos;
        uintptr_t pawnHit = 0;
        double dist = 0;
        if (f.DiscoverFromPawns(pawns, povs, pi, aoff, toff3, pos, pawnHit, dist)) {
            Say("[+] cerrado: camara #%zu (mgr 0x%zX, pov 0x%zX), actor 0x%llX, dist %.1f cm\n",
                pi, povs[pi].mgrOff, povs[pi].povOff, (unsigned long long)pawnHit, dist);
            Say("[+] Actor.RootComponent = 0x%zX   Scene.ComponentToWorld = 0x%zX\n", aoff, toff3);
            Say("    posicion del pawn: %.1f %.1f %.1f\n", pos.x, pos.y, pos.z);
            camOffset = povs[pi].mgrOff;
            povOffset = povs[pi].povOff;
            rootOffset = aoff;
            transformOffset = toff3;
        } else {
            Say("[!] el pawn no cerro contra ninguna camara.\n");
        }
    }

    // --- malla esqueletica: bounds exactos y array de huesos ---
    size_t meshOff = 0, boundsOff = 0, bonesOff = 0, meshCtwOff = 0;
    int32_t boneCount = 0;
    Vec3 meshExtent;
    if (sampleActor) {
        std::vector<uintptr_t> peers;
        for (uint64_t p : ptrs) {
            if (!p) continue;
            if (f.ClassName((uintptr_t)p) == sampleClass) peers.push_back((uintptr_t)p);
            if (peers.size() >= 8) break;
        }
        uintptr_t mesh = 0;
        int bestOk = 0;
        size_t bestAoff = 0;
        for (size_t aoff = 0; aoff <= 0x8000; aoff += 8) {
            int ok = 0;
            uintptr_t first = 0;
            for (uintptr_t a : peers) {
                uintptr_t c = 0;
                if (!f.proc().ReadPtr(a + aoff, c) || c < 0x10000) continue;
                if (f.ClassName(c).find(L"MeshComponent") == std::wstring::npos) continue;
                if (!first) first = c;
                ++ok;
            }
            if (ok > bestOk) { bestOk = ok; bestAoff = aoff; mesh = first; }
            if (ok >= 5) { meshOff = aoff; break; }
        }
        if (!meshOff && bestOk > 0) Say("[!] SkeletalMesh solo en %d/%zu actores, mejor offset 0x%zX\n", bestOk, peers.size(), bestAoff);
        {
            std::vector<uint8_t> abuf(0x2000);
            if (f.proc().Read(sampleActor, abuf.data(), abuf.size())) {
                Say("    --- componentes del actor muestra ---\n");
                int shown = 0;
                for (size_t off = 0; off + 8 <= abuf.size() && shown < 30; off += 8) {
                    uintptr_t c = 0;
                    std::memcpy(&c, abuf.data() + off, 8);
                    if (c < 0x10000 || (c & 7)) continue;
                    const std::wstring cls = f.ClassName(c);
                    if (cls.find(L"Component") == std::wstring::npos) continue;
                    Say("      0x%zX -> %ls\n", off, cls.c_str());
                    ++shown;
                }
            }
        }
        if (mesh) {
            Say("[+] Actor.SkeletalMesh = 0x%zX  (%ls)\n", meshOff, f.ClassName(mesh).c_str());

            // posicion del actor con los offsets ya validados
            uintptr_t comp = 0;
            Vec3 actorPos;
            if (f.proc().ReadPtr(sampleActor + 0x120, comp) && comp)
                f.proc().Read(comp + 0x120, &actorPos.x, 24);

            // ComponentToWorld del mesh: 3 doubles cerca de la posicion del actor
            std::vector<uint8_t> mbuf(0x4000);
            if (f.proc().Read(mesh, mbuf.data(), mbuf.size())) {
                for (size_t off = 0; off + 24 <= mbuf.size(); off += 8) {
                    double v[3];
                    std::memcpy(v, mbuf.data() + off, 24);
                    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
                    if (std::fabs(v[0] - actorPos.x) > 50 || std::fabs(v[1] - actorPos.y) > 50) continue;
                    meshCtwOff = off;
                    break;
                }
                // Candidatos de bounds: se listan varios para elegir con datos.
                {
                    int shownCand = 0;
                    for (size_t off = 0; off + 48 <= mbuf.size() && shownCand < 6; off += 8) {
                        double o[3];
                        std::memcpy(o, mbuf.data() + off, 24);
                        if (!std::isfinite(o[0]) || !std::isfinite(o[1]) || !std::isfinite(o[2])) continue;
                        if (std::fabs(o[0] - actorPos.x) > 300 || std::fabs(o[1] - actorPos.y) > 300) continue;
                        double e[3];
                        std::memcpy(e, mbuf.data() + off + 24, 24);
                        bool ok = true;
                        for (int k = 0; k < 3; ++k) {
                            if (!std::isfinite(e[k]) || std::fabs(e[k]) < 5 || std::fabs(e[k]) > 3000) ok = false;
                        }
                        if (!ok) continue;
                        Say("      bounds? off 0x%zX -> %.0f %.0f %.0f\n", off, e[0], e[1], e[2]);
                        ++shownCand;
                    }
                }
                // FBoxSphereBounds: Origin (24) + BoxExtent (24). Se busca un
                // origen pegado a la posicion del actor cuyo extent sea de criatura.
                for (size_t off = 0; off + 48 <= mbuf.size(); off += 8) {
                    double o[3];
                    std::memcpy(o, mbuf.data() + off, 24);
                    if (!std::isfinite(o[0]) || !std::isfinite(o[1]) || !std::isfinite(o[2])) continue;
                    if (std::fabs(o[0] - actorPos.x) > 200 || std::fabs(o[1] - actorPos.y) > 200) continue;
                    double e[3];
                    std::memcpy(e, mbuf.data() + off + 24, 24);
                    bool ok = true;
                    for (int k = 0; k < 3; ++k) {
                        if (!std::isfinite(e[k]) || std::fabs(e[k]) < 10 || std::fabs(e[k]) > 3000) ok = false;
                    }
                    if (!ok) continue;
                    boundsOff = off + 24;
                    meshExtent = Vec3(e[0], e[1], e[2]);
                    break;
                }
            }
            Say("[%c] Mesh.ComponentToWorld = 0x%zX   [%c] Mesh.BoxExtent = 0x%zX (%.0f %.0f %.0f)\n",
                meshCtwOff ? '+' : '!', meshCtwOff, boundsOff ? '+' : '!', boundsOff,
                meshExtent.x, meshExtent.y, meshExtent.z);

            // array de huesos: TArray<FTransform> con num razonable
            for (size_t off = 0; off + 16 <= 0x4000; off += 8) {
                struct TArr { uint64_t data; int32_t num; int32_t max; };
                TArr arr{};
                if (!f.proc().Read(mesh + off, &arr, sizeof(arr))) continue;
                if (arr.num < 20 || arr.num > 400 || arr.max < arr.num || !arr.data) continue;
                uint8_t q[96]{};
                if (!f.proc().Read((uintptr_t)arr.data, q, sizeof(q))) continue;
                double d[4];
                std::memcpy(d, q, 32);
                float fl[4];
                std::memcpy(fl, q, 16);
                const double nd = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + d[3] * d[3]);
                const double nf = std::sqrt((double)fl[0] * fl[0] + (double)fl[1] * fl[1] +
                                            (double)fl[2] * fl[2] + (double)fl[3] * fl[3]);
                if (!((nd > 0.9 && nd < 1.1) || (nf > 0.9 && nf < 1.1))) continue;
                bonesOff = off;
                boneCount = arr.num;
                // Arrays con pinta de huesos: se validan tres layouts de FTransform.
                for (size_t coff = 0; coff + 16 <= 0x8000; coff += 8) {
                    struct TA2 { uint64_t data; int32_t num; int32_t max; };
                    TA2 a2{};
                    if (!f.proc().Read(mesh + coff, &a2, sizeof(a2))) continue;
                    if (a2.num < 20 || a2.num > 400 || a2.max < a2.num || !a2.data) continue;
                    for (int layout = 0; layout < 3; ++layout) {
                        const size_t stride = (layout == 0) ? 40 : (layout == 1 ? 64 : 80);
                        const size_t tOff = (layout == 0) ? 16 : (layout == 1 ? 16 : 32);
                        std::vector<uint8_t> tb((size_t)a2.num * stride);
                        if (!f.proc().Read((uintptr_t)a2.data, tb.data(), tb.size())) continue;
                        const int probe = (a2.num < 12) ? a2.num : 12;
                        int good = 0;
                        double maxAbs = 0;
                        for (int bi = 0; bi < probe; ++bi) {
                            const uint8_t* pb = tb.data() + (size_t)bi * stride;
                            float q4[4];
                            std::memcpy(q4, pb, 16);
                            const double n = std::sqrt((double)q4[0] * q4[0] + (double)q4[1] * q4[1] +
                                                       (double)q4[2] * q4[2] + (double)q4[3] * q4[3]);
                            if (!(n > 0.9 && n < 1.1)) continue;
                            double tx = 0, ty = 0, tz = 0;
                            if (layout == 0) {
                                float tf[3];
                                std::memcpy(tf, pb + tOff, 12);
                                tx = tf[0]; ty = tf[1]; tz = tf[2];
                            } else {
                                double td[3];
                                std::memcpy(td, pb + tOff, 24);
                                tx = td[0]; ty = td[1]; tz = td[2];
                            }
                            if (!std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tz)) continue;
                            if (std::fabs(tx) > 1e5 || std::fabs(ty) > 1e5 || std::fabs(tz) > 1e5) continue;
                            ++good;
                            maxAbs = std::max(maxAbs, std::max(std::fabs(tx), std::max(std::fabs(ty), std::fabs(tz))));
                        }
                        if (good >= 2)
                            Say("      huesos? 0x%zX stride=%zu maxAbs=%.1f (%d/%d)\n", coff, stride, maxAbs, good, probe);
                    }
                }                // El asset SkeletalMesh guarda el RefSkeleton: pose de referencia
                // y arbol de huesos. Ahi si hay FTransform en espacio de componente.
                uintptr_t asset = 0;
                size_t assetOff = 0;
                {
                    std::vector<uint8_t> mb(0x8000);
                    if (f.proc().Read(mesh, mb.data(), mb.size())) {
                        for (size_t off = 0; off + 8 <= mb.size(); off += 8) {
                            uintptr_t c = 0;
                            std::memcpy(&c, mb.data() + off, 8);
                            if (c < 0x10000 || (c & 7)) continue;
                            const std::wstring cls = f.ClassName(c);
                            if (cls.find(L"SkeletalMesh") == std::wstring::npos) continue;
                            if (cls.find(L"Component") != std::wstring::npos) continue;
                            asset = c;
                            assetOff = off;
                            break;
                        }
                    }
                }
                if (asset) {
                    Say("[+] Mesh.Asset = 0x%zX  (%ls)\n", assetOff, f.ClassName(asset).c_str());
                    std::vector<uint8_t> ab(0x4000);
                    if (f.proc().Read(asset, ab.data(), ab.size())) {
                        for (size_t off = 0; off + 16 <= ab.size(); off += 8) {
                            struct TA3 { uint64_t data; int32_t num; int32_t max; };
                            TA3 a3{};
                            std::memcpy(&a3, ab.data() + off, sizeof(a3));
                            if (a3.num < 20 || a3.num > 400 || a3.max < a3.num || !a3.data) continue;
                            // hueso: FName(8) + ParentIndex(4)
                            int32_t pi[8]{};
                            if (!f.proc().Read((uintptr_t)a3.data + 8, pi, 4)) continue;
                            if (pi[0] < -1 || pi[0] > 500) continue;
                            Say("      asset array 0x%zX num=%d parent0=%d\n", off, a3.num, pi[0]);
                            // y como transformadas
                            for (int layout = 0; layout < 3; ++layout) {
                                const size_t stride = (layout == 0) ? 40 : (layout == 1 ? 64 : 80);
                                const size_t tOff = (layout == 0) ? 16 : (layout == 1 ? 16 : 32);
                                std::vector<uint8_t> tb((size_t)a3.num * stride);
                                if (!f.proc().Read((uintptr_t)a3.data, tb.data(), tb.size())) continue;
                                int good = 0;
                                double maxAbs = 0;
                                const int probe = (a3.num < 8) ? a3.num : 8;
                                for (int bi = 0; bi < probe; ++bi) {
                                    const uint8_t* pb = tb.data() + (size_t)bi * stride;
                                    float q4[4];
                                    std::memcpy(q4, pb, 16);
                                    const double n = std::sqrt((double)q4[0] * q4[0] + (double)q4[1] * q4[1] +
                                                               (double)q4[2] * q4[2] + (double)q4[3] * q4[3]);
                                    if (!(n > 0.9 && n < 1.1)) continue;
                                    double tx = 0, ty = 0, tz = 0;
                                    if (layout == 0) {
                                        float tf[3];
                                        std::memcpy(tf, pb + tOff, 12);
                                        tx = tf[0]; ty = tf[1]; tz = tf[2];
                                    } else {
                                        double td[3];
                                        std::memcpy(td, pb + tOff, 24);
                                        tx = td[0]; ty = td[1]; tz = td[2];
                                    }
                                    if (!std::isfinite(tx) || std::isfinite(tx) == false) continue;
                                    if (std::fabs(tx) > 1e5 || std::fabs(ty) > 1e5 || std::fabs(tz) > 1e5) continue;
                                    ++good;
                                    maxAbs = std::max(maxAbs, std::max(std::fabs(tx), std::max(std::fabs(ty), std::fabs(tz))));
                                }
                                if (good >= 2)
                                    Say("        como T? stride=%zu maxAbs=%.1f (%d/%d)\n", stride, maxAbs, good, probe);
                            }
                        }
                    }
                } else {
                    Say("[!] no encontre el asset SkeletalMesh.\n");
                }                // Volcado crudo: primeras traslaciones de cada array candidato.
                {
                    auto dump = [&](uintptr_t base, size_t span, const char* tag) {
                        std::vector<uint8_t> bb(span);
                        if (!f.proc().Read(base, bb.data(), bb.size())) return;
                        for (size_t off = 0; off + 16 <= bb.size(); off += 8) {
                            struct TA4 { uint64_t data; int32_t num; int32_t max; };
                            TA4 a4{};
                            std::memcpy(&a4, bb.data() + off, sizeof(a4));
                            if (a4.num < 20 || a4.num > 400 || a4.max < a4.num || !a4.data) continue;
                            int32_t p0 = 0;
                            f.proc().Read((uintptr_t)a4.data + 8, &p0, 4);
                            std::vector<uint8_t> tb((size_t)a4.num * 64);
                            if (!f.proc().Read((uintptr_t)a4.data, tb.data(), tb.size())) continue;
                            int quatOk = 0;
                            double tr[5][3]{};
                            for (int bi = 0; bi < 8; ++bi) {
                                const uint8_t* pb = tb.data() + (size_t)bi * 40;
                                float q4[4];
                                std::memcpy(q4, pb, 16);
                                const double n = std::sqrt((double)q4[0] * q4[0] + (double)q4[1] * q4[1] +
                                                           (double)q4[2] * q4[2] + (double)q4[3] * q4[3]);
                                if (n > 0.9 && n < 1.1) ++quatOk;
                                if (bi < 5) {
                                    float tf[3];
                                    std::memcpy(tf, pb + 16, 12);
                                    tr[bi][0] = tf[0]; tr[bi][1] = tf[1]; tr[bi][2] = tf[2];
                                }
                            }
                            Say("      [%s] 0x%zX num=%d par0=%d quatOk=%d/8  t: (%.0f %.0f %.0f) (%.0f %.0f %.0f) (%.0f %.0f %.0f) (%.0f %.0f %.0f)\n",
                                tag, off, a4.num, p0, quatOk,
                                tr[0][0], tr[0][1], tr[0][2], tr[1][0], tr[1][1], tr[1][2],
                                tr[2][0], tr[2][1], tr[2][2], tr[3][0], tr[3][1], tr[3][2]);
                        }
                    };
                    dump(mesh, 0x8000, "mesh");
                    if (asset) dump(asset, 0x2000, "asset");
                }                // Hex crudo de los arrays candidatos: esto se decodifica a ojo.
                {
                    auto hexdump = [&](uintptr_t base, size_t off, const char* tag) {
                        struct TA5 { uint64_t data; int32_t num; int32_t max; };
                        TA5 a5{};
                        if (!f.proc().Read(base + off, &a5, sizeof(a5))) return;
                        if (a5.num < 20 || a5.num > 400 || !a5.data) return;
                        uint8_t hx[160]{};
                        if (!f.proc().Read((uintptr_t)a5.data, hx, sizeof(hx))) return;
                        Say("      HEX[%s 0x%zX] num=%d\n", tag, off, a5.num);
                        for (int row = 0; row < 5; ++row) {
                            Say("        ");
                            for (int col = 0; col < 32; ++col) Say("%02X ", hx[row * 32 + col]);
                            Say("\n");
                        }
                    };
                    hexdump(mesh, 0x9E0, "mesh");
                    if (asset) {
                        hexdump(asset, 0x380, "asset");
                        hexdump(asset, 0x390, "asset");
                        hexdump(asset, 0x4E8, "asset");
                        hexdump(asset, 0x568, "asset");
                    }
                }                // Busqueda por patron de TRASLACION: tres valores crecientes de
                // decenas de cm, coherentes entre elementos. Esto huele a huesos.
                {
                    auto probe = [&](uintptr_t base, size_t span, const char* tag) {
                        std::vector<uint8_t> bb(span);
                        if (!f.proc().Read(base, bb.data(), bb.size())) return;
                        for (size_t off = 0; off + 16 <= bb.size(); off += 8) {
                            struct TA6 { uint64_t data; int32_t num; int32_t max; };
                            TA6 a6{};
                            std::memcpy(&a6, bb.data() + off, sizeof(a6));
                            if (a6.num < 40 || a6.num > 400 || a6.max < a6.num || !a6.data) continue;
                            const size_t strides[4] = { 12, 24, 40, 80 };
                            const size_t toffs[6] = { 0, 16, 24, 32, 48, 64 };
                            for (size_t si = 0; si < 4; ++si) {
                                const size_t stride = strides[si];
                                std::vector<uint8_t> tb((size_t)a6.num * stride);
                                if (!f.proc().Read((uintptr_t)a6.data, tb.data(), tb.size())) continue;
                                for (size_t ti = 0; ti < 6; ++ti) {
                                    const size_t tOff = toffs[ti];
                                    if (tOff + stride > (size_t)a6.num * stride) continue;
                                    int good = 0;
                                    double maxAbs = 0, minAbs = 1e18;
                                    for (int bi = 0; bi < 16; ++bi) {
                                        const uint8_t* pb = tb.data() + (size_t)bi * stride + tOff;
                                        if ((size_t)bi * stride + tOff + 24 > tb.size()) break;
                                        double v[3];
                                        if (stride == 12) {
                                            float tf[3];
                                            std::memcpy(tf, pb, 12);
                                            v[0] = tf[0]; v[1] = tf[1]; v[2] = tf[2];
                                        } else {
                                            std::memcpy(v, pb, 24);
                                        }
                                        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
                                        const double a = std::max(std::fabs(v[0]), std::max(std::fabs(v[1]), std::fabs(v[2])));
                                        if (a > 5000) continue;
                                        ++good;
                                        maxAbs = std::max(maxAbs, a);
                                        minAbs = std::min(minAbs, a);
                                    }
                                    if (good >= 12 && maxAbs > 40 && maxAbs < 2000)
                                        Say("      POS? [%s] 0x%zX num=%d stride=%zu tOff=%zu good=%d max=%.1f\n",
                                            tag, off, a6.num, stride, tOff, good, maxAbs);
                                }
                            }
                        }
                    };
                    probe(mesh, 0x8000, "mesh");
                    if (asset) probe(asset, 0x2000, "asset");
                }                Say("[+] Mesh.Bones = 0x%zX  (%d huesos, cuaternion %s)\n",
                    off, arr.num, (nd > 0.9 && nd < 1.1) ? "double" : "float");
                break;
            }
            if (!bonesOff) Say("[!] no encontre el array de huesos.\n");
        } else {
            Say("[!] no encontre SkeletalMeshComponent en el actor.\n");
        }
    }

    // --- salida -----------------------------------------------------------
    std::wstring hint = L"PrimalDinoCharacter";   // clase nativa: el filtro sube la cadena SuperStruct

    Say("\n================ offsets.ini ================\n");
    Say("[roots]\n");
    if (gworldSlot) Say("GWorldSlot=0x%llX\n", (unsigned long long)gworldSlot);
    Say("GNamesSlot=0x%llX\n", (unsigned long long)f.gnamesSlot());
    Say("GNamesIndirection=%d\n\n", f.gnamesIndirection());
    Say("[offsets]\n");
    Say("World.PersistentLevel=0x%zX\n", levelOffset);
    if (giOffset) Say("World.OwningGameInstance=0x%zX\n", giOffset);
    if (lpOffset) Say("GameInstance.LocalPlayers=0x%zX\n", lpOffset);
    if (pcOffset) Say("LocalPlayer.PlayerController=0x%zX\n", pcOffset);
    if (camOffset) Say("PlayerController.CameraManager=0x%zX\n", camOffset);
    if (povOffset) Say("CameraManager.CameraCache=0x%zX\n", povOffset);
    Say("Level.Actors=0x%zX\n", actorsOffset);
    if (rootOffset) Say("Actor.RootComponent=0x%zX\n", rootOffset);
    if (transformOffset) Say("Scene.ComponentToWorld=0x%zX\n", transformOffset);
    Say("Transform.Translation=0x0\n");
    Say("Camera.FOV=0x30\n\n");
    Say("[filter]\n");
    Say("DinoClassHint=%s\n", Narrow(hint).c_str());
    Say("=============================================\n");
    return 0;
}









































