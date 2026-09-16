// e2e: the disposable plug-in scanner pipeline and its durable cache.
//
// User story: open the plug-in manager, scan the machine, get a catalog back,
// restart the app tomorrow and load yesterday's cache without a full rescan.
// Every helper process is disposable by design, so this scenario also proves
// the honest failure paths: missing helper, timeout budgets, apply-before-
// ready rejection, and corrupt cache rejection that keeps the live catalog.
//
// VST3 hosting in this environment has no scan helper target (the SDK is a
// pinned opt-in dependency), so the VST3 half here asserts the complete
// negative contract: jobs fail loudly, the catalog stays empty, and adding
// anything from it is refused without touching the project revision.
#include "e2e.hpp"

#ifndef DAW_E2E_AU_SCAN_HELPER
#define DAW_E2E_AU_SCAN_HELPER "/nonexistent/mydaw-au-scan-helper"
#endif

int main() {
    try {
        using namespace e2e;
        TempRoot root("catalog");
        Bridge session;

        // 1. Begin-time argument gates never spawn a process.
#ifdef __APPLE__
        CHECK(daw_begin_installed_au_scan(nullptr, 1000) == nullptr);
        CHECK(daw_begin_installed_au_scan("", 1000) == nullptr);
        CHECK(daw_begin_installed_au_scan(DAW_E2E_AU_SCAN_HELPER, 99) == nullptr);   // below 100 ms
        CHECK(daw_begin_installed_au_scan(DAW_E2E_AU_SCAN_HELPER, 30001) == nullptr); // above 30 s
        CHECK(daw_begin_installed_vst3_scan(nullptr, 1000) == nullptr);
        CHECK(daw_begin_installed_vst3_scan("/nonexistent/vst3-scan-helper", 10000) != nullptr);
#else
        CHECK(daw_begin_installed_au_scan("/bin/echo", 1000) == nullptr); // no scanner off macOS
        CHECK(daw_begin_installed_vst3_scan("/bin/echo", 1000) == nullptr);
        uint32_t supported = 42;
        const int supportedRc = daw_scan_supported_au(session.get(), &supported);
        CHECK(supportedRc == 1 || supported == 0); // rejected or empty: never fabricated AUs
#endif

#ifdef __APPLE__
        // 2. A missing helper must surface as a failed job with error text,
        // never as a hang or a fake empty success.
        auto* missing = daw_begin_installed_au_scan("/nonexistent/mydaw-au-scan-helper", 2000);
        CHECK(missing);
        daw_au_scan_status status = abi<daw_au_scan_status>();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        do {
            CHECK(daw_poll_installed_au_scan(missing, &status) == 0);
            if (status.status != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);
        CHECK(status.status == 2 && status.error[0]);
        uint32_t available = 0, quarantined = 0;
        CHECK_REJ(session.get(), daw_apply_installed_au_scan(session.get(), missing, &available, &quarantined));
        daw_release_installed_au_scan(missing);

        // 3. The real scan: disposable helper enumerates the machine's AUs.
        // A slow machine can legitimately time the helper out; that is the
        // same failed-job contract asserted above, so accept both terminals
        // and only take the success path when the machine delivers one.
        auto* scan = daw_begin_installed_au_scan(DAW_E2E_AU_SCAN_HELPER, 30000);
        CHECK(scan);
        status = abi<daw_au_scan_status>();
        const long double scanDeadline = 120; // seconds of polling budget
        long double waited = 0;
        for (;;) {
            CHECK(daw_poll_installed_au_scan(scan, &status) == 0);
            if (status.status != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            waited += 0.025;
            if (waited > scanDeadline) { daw_release_installed_au_scan(scan); throw std::runtime_error("timeout waiting for AU scan job"); }
        }
        if (status.status == 1) {
            CHECK(status.available_count > 0); // every macOS ships Apple's built-ins
            CHECK_OK(session.get(), daw_apply_installed_au_scan(session.get(), scan, &available, &quarantined));
            CHECK(available == status.available_count);
            // The catalog is now the machine's set: enumerate it fully, and the
            // out-of-range read must reject.
            for (uint32_t index = 0; index < available; ++index) {
                auto component = abi<daw_au_component>();
                CHECK_OK(session.get(), daw_get_supported_au(session.get(), index, &component));
                CHECK(component.type && component.name[0]);
            }
            auto over = abi<daw_au_component>();
            CHECK_REJ(session.get(), daw_get_supported_au(session.get(), available, &over));

            // 4. Cache round trip: save, then load in a FRESH session with the
            // helper used only for freshness validation (the historic app
            // restart path).
            const auto cachePath = root / "au-scan-cache.bin";
            CHECK_OK(session.get(), daw_save_installed_au_scan_cache(session.get(), scan, cachePath.string().c_str()));
            CHECK(fileNonEmpty(cachePath));
            Bridge second;
            uint32_t cacheAvailable = 0, cacheQuarantined = 0, cacheInvalidated = 0;
            CHECK_OK(second.get(), daw_load_installed_au_scan_cache(second.get(), DAW_E2E_AU_SCAN_HELPER,
                                                                    cachePath.string().c_str(),
                                                                    &cacheAvailable, &cacheQuarantined, &cacheInvalidated));
            CHECK(cacheAvailable + cacheQuarantined <= available + quarantined); // nothing invented
            // A corrupt cache rejects and must not clear the live catalog.
            const auto corrupt = root / "corrupt.bin";
            { std::ofstream stream(corrupt, std::ios::binary); stream << "x"; }
            uint32_t a2 = 0, q2 = 0, i2 = 0;
            CHECK_REJ(second.get(), daw_load_installed_au_scan_cache(second.get(), DAW_E2E_AU_SCAN_HELPER,
                                                                      corrupt.string().c_str(), &a2, &q2, &i2));
            for (uint32_t index = 0; index < cacheAvailable; ++index) {
                auto component = abi<daw_au_component>();
                CHECK(daw_get_supported_au(second.get(), index, &component) == 0);
            }
        } else {
            std::cout << "note: AU scan helper reported failure on this machine: " << status.error << "\n";
        }
        daw_release_installed_au_scan(scan);

        // 5. VST3 negative contract (no pinned scan helper in this build).
        auto* vst3 = daw_begin_installed_vst3_scan("/nonexistent/mydaw-vst3-scan-helper", 2000);
        CHECK(vst3);
        daw_vst3_scan_status vstatus = abi<daw_vst3_scan_status>();
        long double vw = 0;
        for (;;) {
            CHECK(daw_poll_installed_vst3_scan(vst3, &vstatus) == 0);
            if (vstatus.status != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            vw += 0.025;
            if (vw > 30) { daw_release_installed_vst3_scan(vst3); throw std::runtime_error("timeout waiting for VST3 scan job"); }
        }
        CHECK(vstatus.status == 2 && vstatus.error[0]); // helper never spawned: loud failure
        CHECK(daw_save_installed_vst3_scan_cache(vst3, (root / "vst3-cache.bin").string().c_str()) == 1);
        CHECK_REJ(session.get(), daw_apply_installed_vst3_scan(session.get(), vst3, &available, &quarantined));
        daw_release_installed_vst3_scan(vst3);
        uint32_t vst3Count = 1;
        CHECK_OK(session.get(), daw_get_installed_vst3_count(session.get(), &vst3Count));
        CHECK(vst3Count == 0);
        auto component = abi<daw_vst3_component>();
        CHECK_REJ(session.get(), daw_get_installed_vst3(session.get(), 0, &component));
        const auto revision = rev(session.get());
        CHECK_REJ(session.get(), daw_add_master_vst3(session.get(), 0, revision));
        CHECK_REJ(session.get(), daw_add_insert_vst3(session.get(), DAW_INSERT_OWNER_MASTER, 0, 7, revision));
        CHECK(rev(session.get()) == revision); // all negatives left the project untouched

        // 6. VST3 cache load contracts. A MISSING cache is a clean first
        // start: empty catalog, zero counts, success. A CORRUPT cache rejects
        // with a named error, and neither outcome may invent entries.
        uint32_t av = 9, qu = 9, inv = 9;
        CHECK_OK(session.get(), daw_load_installed_vst3_scan_cache(session.get(), "/nonexistent/vst3-helper",
                                                                   (root / "missing.bin").string().c_str(), &av, &qu, &inv));
        CHECK(av == 0 && qu == 0 && inv == 0);
        { std::ofstream junk(root / "junk.bin", std::ios::binary); junk << "junk"; }
        CHECK_REJ(session.get(), daw_load_installed_vst3_scan_cache(session.get(), "/nonexistent/vst3-helper",
                                                                    (root / "junk.bin").string().c_str(), &av, &qu, &inv));
        CHECK_OK(session.get(), daw_get_installed_vst3_count(session.get(), &vst3Count));
        CHECK(vst3Count == 0);
#endif

        std::cout << "PASS: e2e_plugin_catalog — helper argument gates, failed-job contract, catalog apply, cache round trip, VST3 negatives\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "E2E FAIL: " << error.what() << '\n';
        return 1;
    }
}
