#pragma once

#include <cstdint>
#include <atomic>

namespace daw::vst3editor {

constexpr uint32_t kEditorMagic = 0x4d445645U; // MDVE
constexpr uint32_t kEditorVersion = 1;

enum class EditorOperation : uint32_t {
    Open = 1,
    Close = 2,
    Resize = 3,
    GetParameter = 4,
    SetParameter = 5,
    Idle = 6
};

struct EditorMapping {
    uint32_t magic = kEditorMagic;
    uint32_t version = kEditorVersion;
    uint32_t operation = 0;
    std::atomic<uint32_t> completion{0}; // 0 pending, 1 success, 2 failure
    uint32_t requestStateBytes = 0;
    uint32_t responseStateBytes = 0;
    uint32_t responseCapacityBytes = 0;
    uint32_t parameterID = 0;
    float normalizedValue = 0;
    int32_t width = 0;
    int32_t height = 0;
    uint32_t viewType = 0; // 0 = default, 1 = generic
    char error[256] = {0};
};

static_assert(sizeof(EditorMapping) <= 4096);

inline uint8_t* editorRequestPayload(EditorMapping* mapping) {
    return reinterpret_cast<uint8_t*>(mapping) + sizeof(EditorMapping);
}
inline const uint8_t* editorRequestPayload(const EditorMapping* mapping) {
    return reinterpret_cast<const uint8_t*>(mapping) + sizeof(EditorMapping);
}
inline uint8_t* editorResponsePayload(EditorMapping* mapping) {
    return editorRequestPayload(mapping) + mapping->requestStateBytes;
}
inline const uint8_t* editorResponsePayload(const EditorMapping* mapping) {
    return editorRequestPayload(mapping) + mapping->requestStateBytes;
}

constexpr size_t editorMappingBytes(uint32_t requestStateBytes, uint32_t responseCapacityBytes) {
    return sizeof(EditorMapping) + requestStateBytes + responseCapacityBytes;
}

} // namespace daw::vst3editor