#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>

namespace {

struct Counters {
    std::atomic<std::uint64_t> packets{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> note_on{0};
    std::atomic<std::uint64_t> note_off{0};
};

std::string cfString(CFStringRef value) {
    if (value == nullptr) return {};
    char buffer[1024]{};
    if (CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
        return buffer;
    }
    return {};
}

std::string endpointName(MIDIEndpointRef endpoint) {
    CFStringRef value = nullptr;
    if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &value) != noErr || value == nullptr) {
        if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyName, &value) != noErr || value == nullptr) {
            return "Unnamed MIDI source";
        }
    }
    const auto result = cfString(value);
    CFRelease(value);
    return result.empty() ? "Unnamed MIDI source" : result;
}

std::optional<MIDIUniqueID> endpointUniqueID(MIDIEndpointRef endpoint) {
    SInt32 value = 0;
    if (MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID, &value) != noErr) {
        return std::nullopt;
    }
    return static_cast<MIDIUniqueID>(value);
}

UInt32 umpWords(UInt32 messageType) {
    switch (messageType) {
    case 0x3U: // Data64 / SysEx7
    case 0x4U: // MIDI 2.0 Channel Voice
        return 2;
    case 0x5U: // Data128
    case 0xDU: // Flex Data
    case 0xFU: // Stream
        return 4;
    default:
        return 1;
    }
}

void receive(const MIDIEventList* eventList, Counters& counters) {
    if (eventList == nullptr) return;

    const MIDIEventPacket* packet = &eventList->packet[0];
    for (UInt32 packetIndex = 0; packetIndex < eventList->numPackets; ++packetIndex) {
        counters.packets.fetch_add(1, std::memory_order_relaxed);
        counters.bytes.fetch_add(static_cast<std::uint64_t>(packet->wordCount) * sizeof(UInt32),
                                 std::memory_order_relaxed);

        UInt32 wordIndex = 0;
        while (wordIndex < packet->wordCount) {
            const UInt32 word = packet->words[wordIndex];
            const UInt32 messageType = (word >> 28U) & 0x0fU;
            const UInt32 words = umpWords(messageType);
            if (words > packet->wordCount - wordIndex) break;

            // We request kMIDIProtocol_1_0 below. CoreMIDI therefore delivers
            // MIDI 1.0 Channel Voice UMP (message type 0x2) regardless of the
            // source's native protocol. Layout: type/group/status+channel/data1/data2.
            if (messageType == 0x2U) {
                const UInt32 command = (word >> 20U) & 0x0fU;
                const UInt32 velocity = word & 0xffU;
                if (command == 0x9U) {
                    if (velocity == 0U) counters.note_off.fetch_add(1, std::memory_order_relaxed);
                    else counters.note_on.fetch_add(1, std::memory_order_relaxed);
                } else if (command == 0x8U) {
                    counters.note_off.fetch_add(1, std::memory_order_relaxed);
                }
            }
            wordIndex += words;
        }
        packet = MIDIEventPacketNext(packet);
    }
}

void printSources() {
    const ItemCount count = MIDIGetNumberOfSources();
    std::cout << "CoreMIDI sources: " << count << '\n';
    for (ItemCount index = 0; index < count; ++index) {
        const MIDIEndpointRef source = MIDIGetSource(index);
        const auto uniqueID = endpointUniqueID(source);
        std::cout << index << "\t"
                  << (uniqueID ? std::to_string(*uniqueID) : std::string("no-id"))
                  << "\t" << endpointName(source) << '\n';
    }
}

std::optional<MIDIEndpointRef> findSource(MIDIUniqueID wanted) {
    const ItemCount count = MIDIGetNumberOfSources();
    for (ItemCount index = 0; index < count; ++index) {
        const MIDIEndpointRef source = MIDIGetSource(index);
        const auto uniqueID = endpointUniqueID(source);
        if (uniqueID && *uniqueID == wanted) return source;
    }
    return std::nullopt;
}

void usage(const char* argv0) {
    std::cerr << "Usage:\n"
              << "  " << argv0 << " --list\n"
              << "  " << argv0 << " --source <unique-id> [--seconds <n>]\n";
}

} // namespace

int main(int argc, char** argv) {
    bool listOnly = false;
    std::optional<MIDIUniqueID> sourceID;
    int seconds = 8;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0) {
            listOnly = true;
        } else if (std::strcmp(argv[i], "--source") == 0 && i + 1 < argc) {
            sourceID = static_cast<MIDIUniqueID>(std::strtol(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (listOnly && !sourceID) {
        printSources();
        return 0;
    }
    if (!sourceID || seconds <= 0 || seconds > 300) {
        usage(argv[0]);
        return 2;
    }

    const auto source = findSource(*sourceID);
    if (!source) {
        std::cerr << "MIDI source " << *sourceID << " not found.\n";
        printSources();
        return 3;
    }

    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    Counters counters;
    Counters* counterPtr = &counters;

    OSStatus status = MIDIClientCreate(CFSTR("My DAW MIDI hardware probe"), nullptr, nullptr, &client);
    if (status != noErr) {
        std::cerr << "MIDIClientCreate failed: " << status << '\n';
        return 4;
    }
    status = MIDIInputPortCreateWithProtocol(
        client, CFSTR("Probe input"), kMIDIProtocol_1_0, &port,
        ^(const MIDIEventList* eventList, void*) { receive(eventList, *counterPtr); });
    if (status != noErr) {
        std::cerr << "MIDIInputPortCreateWithProtocol failed: " << status << '\n';
        MIDIClientDispose(client);
        return 5;
    }
    status = MIDIPortConnectSource(port, *source, nullptr);
    if (status != noErr) {
        std::cerr << "MIDIPortConnectSource failed: " << status << '\n';
        MIDIPortDispose(port);
        MIDIClientDispose(client);
        return 6;
    }

    std::cout << "Listening to " << endpointName(*source) << " (" << *sourceID << ") for "
              << seconds << " s. Play several notes now.\n";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    }

    MIDIPortDisconnectSource(port, *source);
    MIDIPortDispose(port);
    MIDIClientDispose(client);

    const auto packets = counters.packets.load(std::memory_order_relaxed);
    const auto bytes = counters.bytes.load(std::memory_order_relaxed);
    const auto noteOn = counters.note_on.load(std::memory_order_relaxed);
    const auto noteOff = counters.note_off.load(std::memory_order_relaxed);
    std::cout << "packets=" << packets << " bytes=" << bytes
              << " note_on=" << noteOn << " note_off=" << noteOff << '\n';

    if (packets == 0) {
        std::cerr << "No MIDI packets received. Check the selected source, cable/Bluetooth session, and device power.\n";
        return 10;
    }
    if (noteOn == 0) {
        std::cerr << "MIDI traffic arrived, but no Note On messages were observed.\n";
        return 11;
    }
    return 0;
}
