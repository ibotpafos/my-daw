#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
namespace daw {
struct ZipPackageEntry { std::string name; std::vector<uint8_t> bytes; };
// Writes a deterministic ZIP32 archive with uncompressed entries only. The
// destination is atomically replaced after file and directory fsync succeed.
bool writeZipPackage(const std::vector<ZipPackageEntry>& entries,const std::string& destination,std::string& error,const std::atomic<bool>* cancel=nullptr);
}
