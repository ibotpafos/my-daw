#include "platform/macos/vst3_editor_protocol.hpp"

#include "public.sdk/source/vst/hosting/module.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/base/ibstream.h"

#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

using namespace daw::vst3editor;

std::string encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (unsigned char byte : value) {
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '/') {
            result.push_back(static_cast<char>(byte));
        } else {
            result.push_back('%');
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 15]);
        }
    }
    return result;
}

bool validFuid(std::string_view value) {
    if (value.size() != 32) return false;
    for (const char byte : value) if (std::strchr("0123456789ABCDEFabcdef", byte) == nullptr) return false;
    return true;
}

bool validFingerprint(std::string_view value) {
    if (value.size() != 64) return false;
    for (const char byte : value) if (std::strchr("0123456789ABCDEFabcdef", byte) == nullptr) return false;
    return true;
}

std::string hexDigest(const unsigned char* digest, size_t count) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(count * 2);
    for (size_t index = 0; index < count; ++index) {
        result.push_back(hex[digest[index] >> 4]);
        result.push_back(hex[digest[index] & 15]);
    }
    return result;
}

std::string moduleFingerprint(const std::string& modulePath) {
    namespace fs = std::filesystem;
    std::vector<fs::path> executables;
    const fs::path directory = fs::path(modulePath) / "Contents" / "MacOS";
    std::error_code error;
    for (fs::directory_iterator iterator(directory, fs::directory_options::skip_permission_denied, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file(error) && !error) executables.push_back(iterator->path());
    }
    if (error || executables.empty()) return {};
    std::sort(executables.begin(), executables.end());
    const auto executable = executables.front();
    const auto size = fs::file_size(executable, error);
    if (error || size == 0 || size > 256U * 1024U * 1024U) return {};
    std::ifstream input(executable, std::ios::binary);
    if (!input) return {};
    CC_SHA256_CTX context{};
    CC_SHA256_Init(&context);
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(count));
    }
    if (!input.eof()) return {};
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256_Final(digest.data(), &context);
    return hexDigest(digest.data(), digest.size());
}

class EditorHost {
public:
    EditorHost() = default;
    ~EditorHost() { close(); }

    bool open(const std::string& modulePath, const std::string& classId) {
        std::string error;
        module = VST3::Hosting::Module::create(modulePath, error);
        if (!module) {
            lastError = "Module load failed: " + error;
            return false;
        }
        const auto wanted = VST3::UID::fromString(classId);
        if (!wanted) {
            lastError = "Invalid class ID";
            return false;
        }
        for (const auto& info : module->getFactory().classInfos()) {
            if (info.category() == kVstAudioEffectClass && info.ID() == *wanted) {
                component = module->getFactory().createInstance<Steinberg::Vst::IComponent>(info->ID());
                break;
            }
        }
        if (!component) {
            lastError = "Component creation failed";
            return false;
        }
        if (component->initialize(nullptr) != Steinberg::kResultOk) {
            lastError = "Component initialize failed";
            component.reset();
            return false;
        }
        controller = component->getControllerClassId();
        if (!controller) {
            lastError = "No controller class ID";
            return false;
        }
        editController = module->getFactory().createInstance<Steinberg::Vst::IEditController>(*controller);
        if (!editController) {
            lastError = "EditController creation failed";
            return false;
        }
        if (editController->initialize(nullptr) != Steinberg::kResultOk) {
            lastError = "EditController initialize failed";
            editController.reset();
            return false;
        }
        component->setControllerClassId(*controller);
        if (editController->setComponentState(nullptr) != Steinberg::kResultOk) {
            // Not fatal, some plugins don't need it
        }
        return true;
    }

    bool createView(const char* viewType, int32_t width, int32_t height) {
        if (!editController) {
            lastError = "No edit controller";
            return false;
        }
        view = editController->createView(viewType ? viewType : "editor");
        if (!view) {
            lastError = "createView returned null";
            return false;
        }
        Steinberg::ViewRect rect{0, 0, width, height};
        if (view->setSize(&rect) != Steinberg::kResultOk) {
            lastError = "view->setSize failed";
            view.reset();
            return false;
        }
        attached = false;
        return true;
    }

    bool attach(void* parentWindow) {
        if (!view || attached) return true;
        if (view->attached(parentWindow, Steinberg::kPlatformTypeHIView) != Steinberg::kResultOk) {
            lastError = "view->attached failed";
            return false;
        }
        attached = true;
        return true;
    }

    bool detach() {
        if (!view || !attached) return true;
        if (view->removed() != Steinberg::kResultOk) {
            lastError = "view->removed failed";
            return false;
        }
        attached = false;
        return true;
    }

    bool setSize(int32_t width, int32_t height) {
        if (!view) {
            lastError = "No view";
            return false;
        }
        Steinberg::ViewRect rect{0, 0, width, height};
        return view->setSize(&rect) == Steinberg::kResultOk;
    }

    bool getParameter(uint32_t id, float& outValue) {
        if (!editController) {
            lastError = "No edit controller";
            return false;
        }
        Steinberg::Vst::ParamValue value = 0;
        if (editController->getParamNormalized(id, value) != Steinberg::kResultOk) {
            lastError = "getParamNormalized failed";
            return false;
        }
        outValue = static_cast<float>(value);
        return true;
    }

    bool setParameter(uint32_t id, float normalizedValue) {
        if (!editController) {
            lastError = "No edit controller";
            return false;
        }
        return editController->setParamNormalized(id, normalizedValue) == Steinberg::kResultOk;
    }

    void idle() {
        if (editController) {
            editController->performEdit(nullptr);
        }
    }

    void close() {
        if (view) {
            if (attached) {
                view->removed();
                attached = false;
            }
            view.reset();
        }
        if (editController) {
            editController->terminate();
            editController.reset();
        }
        if (component) {
            component->terminate();
            component.reset();
        }
        module.reset();
    }

    const std::string& getLastError() const { return lastError; }

private:
    VST3::Hosting::ModulePtr module;
    Steinberg::Vst::IComponentPtr component;
    Steinberg::Vst::IEditControllerPtr editController;
    Steinberg::Vst::IPlugViewPtr view;
    Steinberg::FUID controller;
    bool attached = false;
    std::string lastError;
};

int runEditor(const char* name) {
    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) return 10;
    struct stat info{};
    if (fstat(fd, &info) != 0 || info.st_size < static_cast<off_t>(sizeof(EditorMapping))) {
        close(fd);
        return 11;
    }
    const size_t mapBytes = static_cast<size_t>(info.st_size);
    auto* mapping = static_cast<EditorMapping*>(mmap(nullptr, mapBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (mapping == MAP_FAILED) {
        close(fd);
        return 12;
    }
    const auto cleanup = [&] { munmap(mapping, mapBytes); close(fd); };

    if (mapping->magic != kEditorMagic || mapping->version != kEditorVersion) {
        cleanup();
        return 13;
    }

    static EditorHost host;
    const auto operation = static_cast<EditorOperation>(mapping->operation);
    try {
        switch (operation) {
            case EditorOperation::Open: {
                const auto* payload = editorRequestPayload(mapping);
                const std::string modulePath(reinterpret_cast<const char*>(payload), mapping->requestStateBytes);
                const std::string classId(reinterpret_cast<const char*>(payload + modulePath.size() + 1));
                if (!host.open(modulePath, classId)) {
                    std::strncpy(mapping->error, host.getLastError().c_str(), sizeof(mapping->error) - 1);
                    mapping->completion.store(2, std::memory_order_release);
                    cleanup();
                    return 14;
                }
                mapping->completion.store(1, std::memory_order_release);
                cleanup();
                return 0;
            }
            case EditorOperation::Close: {
                host.close();
                mapping->completion.store(1, std::memory_order_release);
                cleanup();
                return 0;
            }
            case EditorOperation::Resize: {
                if (!host.setSize(mapping->width, mapping->height)) {
                    std::strncpy(mapping->error, host.getLastError().c_str(), sizeof(mapping->error) - 1);
                    mapping->completion.store(2, std::memory_order_release);
                    cleanup();
                    return 15;
                }
                mapping->completion.store(1, std::memory_order_release);
                cleanup();
                return 0;
            }
            case EditorOperation::GetParameter: {
                float value = 0;
                if (!host.getParameter(mapping->parameterID, value)) {
                    std::strncpy(mapping->error, host.getLastError().c_str(), sizeof(mapping->error) - 1);
                    mapping->completion.store(2, std::memory_order_release);
                    cleanup();
                    return 16;
                }
                auto* response = editorResponsePayload(mapping);
                *reinterpret_cast<float*>(response) = value;
                mapping->responsePayloadBytes = sizeof(float);
                mapping->completion.store(1, std::memory_order_release);
                cleanup();
                return 0;
            }
            case EditorOperation::SetParameter: {
                if (!host.setParameter(mapping->parameterID, mapping->normalizedValue)) {
                    std::strncpy(mapping->error, host.getLastError().c_str(), sizeof(mapping->error) - 1);
                    mapping->completion.store(2, std::memory_order_release);
                    cleanup();
                    return 17;
                }
                mapping->completion.store(1, std::memory_order_release);
                cleanup();
                return 0;
            }
            case EditorOperation::Idle: {
                host.idle();
                mapping->completion.store(1, std::memory_order_release);
                cleanup();
                return 0;
            }
            default: {
                std::strncpy(mapping->error, "Unknown editor operation", sizeof(mapping->error) - 1);
                mapping->completion.store(2, std::memory_order_release);
                cleanup();
                return 18;
            }
        }
    } catch (...) {
        std::strncpy(mapping->error, "Exception in editor helper", sizeof(mapping->error) - 1);
        mapping->completion.store(2, std::memory_order_release);
        cleanup();
        return 19;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--control-shared-memory") == 0) return runEditor(argv[2]);
    return 2;
}