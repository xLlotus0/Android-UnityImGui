#include "ElfScannerManager.h"

#include <chrono>
#include <string_view>
#include <thread>
#include <vector>

#include "Foundation/KittyEx.h"
#include "Foundation/Logger.h"

// 由 X-macro 自动生成查找表
static constexpr struct { std::string_view soName; int index; } kLibTable[] = {
#define ELF_LIB_ENTRY(ENUM, FUNC, SO) { SO, ElfScannerManager::LIB_##ENUM },
    ELF_LIB_LIST
#undef ELF_LIB_ENTRY
};

static bool HasLibTextSeg(const ElfScanner& elf, const std::string& name)
{
    for (const auto& s : elf.segments()) {
        if (!s.readable || !s.executable) continue;
        if (s.pathname.size() >= name.size() &&
            s.pathname.compare(s.pathname.size() - name.size(), name.size(), name) == 0)
            return true;
    }
    return false;
}

static ElfScanner SelectByName(const std::string& name)
{
    auto& mgr = KT::detail::g_mgr;
    ElfScanner picked = mgr.elfScanner.createWithSoInfo(mgr.linkerScanner.findSoInfo(name));
    if (!picked.isValid()) picked = mgr.elfScanner.findElf(name);
    if (picked.isValid() && HasLibTextSeg(picked, name)) return picked;

    auto maps = KT::GetAllMaps();
    uintptr_t bestBase = 0, bestSize = 0;
    for (const auto& m : maps) {
        if (m.offset != 0 || !m.readable) continue;
        if (m.pathname.size() < name.size()) continue;
        if (m.pathname.compare(m.pathname.size() - name.size(), name.size(), name) != 0) continue;
        const size_t sz = m.endAddress - m.startAddress;
        if (sz > bestSize) { bestSize = sz; bestBase = m.startAddress; }
    }
    if (bestBase && bestBase != picked.base()) {
        ElfScanner rebuilt(mgr.memOp(), bestBase, maps);
        if (rebuilt.isValid() && HasLibTextSeg(rebuilt, name)) {
            LOGI("[ElfScannerManager] %s: residual at 0x%lx; rebuilt at real base 0x%lx (sz=0x%lx)",
                 name.c_str(), (unsigned long)picked.base(),
                 (unsigned long)bestBase, (unsigned long)bestSize);
            return rebuilt;
        }
    }
    return picked;  // best-effort
}

bool ElfScannerManager::Scan(const std::set<std::string>& libraries) {
    if (libraries.empty())
        return true;

    LOGI("[ElfScannerManager] Starting scan for %zu libraries...", libraries.size());
    auto start = std::chrono::high_resolution_clock::now();

    struct ScanTask { int index; std::string name; };
    std::vector<ScanTask> tasks;
    tasks.reserve(libraries.size());
    for (const auto& libName : libraries) {
        int idx = -1;
        for (const auto& entry : kLibTable) {
            if (entry.soName == libName) { idx = entry.index; break; }
        }
        if (idx < 0) {
            LOGE("[ElfScannerManager] Unknown library: %s (not in predefined list)", libName.c_str());
            continue;
        }
        if (m_scanners[idx].isValid()) {
            LOGW("[ElfScannerManager] Library already scanned: %s", libName.c_str());
            continue;
        }
        tasks.push_back({ idx, libName });
    }
    if (tasks.empty()) {
        LOGI("[ElfScannerManager] Nothing to scan");
        return true;
    }

    bool allSuccess = true;
    for (const auto& task : tasks) {
        ElfScanner picked = SelectByName(task.name);
        for (int retry = 0; !picked.isValid() && retry < 50; ++retry) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            picked = SelectByName(task.name);
        }

        if (!picked.isValid()) {
            LOGE("[ElfScannerManager] Failed to find library: %s", task.name.c_str());
            allSuccess = false;
            continue;
        }
        m_scanners[task.index] = std::move(picked);
        LOGI("[ElfScannerManager] %s base: 0x%llX",
             task.name.c_str(), (unsigned long long)m_scanners[task.index].base());
    }

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double, std::milli>(end - start).count();
    LOGI("[ElfScannerManager] Scan completed in %f ms", elapsed);

    return allSuccess;
}
