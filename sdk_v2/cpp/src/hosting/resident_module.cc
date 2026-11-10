// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/resident_module.h"

#include "exception.h"
#include "util/sha256.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <span>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#ifdef __linux__
#include <elf.h>
#include <link.h>
#elif defined(__APPLE__)
#include <mach-o/loader.h>
#endif
#endif

namespace fl::hosting {
namespace {

using ReadBytes = std::function<bool(uint64_t, void*, size_t)>;

class ImageFile {
 public:
  explicit ImageFile(const std::filesystem::path& path)
      : size_(std::filesystem::file_size(path)), input_(path, std::ios::binary) {}

  ReadBytes Reader() {
    return [this](uint64_t offset, void* output, size_t count) {
      if (offset > size_ || count > size_ - offset || offset > static_cast<uint64_t>(INT64_MAX)) {
        return false;
      }

      input_.clear();
      input_.seekg(static_cast<std::streamoff>(offset));
      input_.read(static_cast<char*>(output), static_cast<std::streamsize>(count));
      return input_ && static_cast<size_t>(input_.gcount()) == count;
    };
  }

 private:
  uintmax_t size_;
  std::ifstream input_;
};

template <typename T>
bool Read(const ReadBytes& bytes, uint64_t offset, T& value) {
  return bytes(offset, &value, sizeof(value));
}

std::string Hex(std::span<const std::byte> bytes) {
  constexpr char digits[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    const auto value = std::to_integer<unsigned int>(byte);
    result.push_back(digits[value >> 4]);
    result.push_back(digits[value & 15]);
  }

  return result;
}

#if defined(_WIN32) || defined(__APPLE__)
ReadBytes MemoryReader(const void* memory, size_t size) {
  return [memory, size](uint64_t offset, void* output, size_t count) {
    if (offset > size || count > size - static_cast<size_t>(offset)) {
      return false;
    }
#ifdef _WIN32
    SIZE_T read{};
    return ReadProcessMemory(GetCurrentProcess(), static_cast<const std::byte*>(memory) + offset, output, count,
                             &read) &&
           read == count;
#else
    std::memcpy(output, static_cast<const std::byte*>(memory) + offset, count);
    return true;
#endif
  };
}
#endif

#ifdef _WIN32

// Some supported GenAI packages strip CodeView records. Compare immutable image sections instead, undoing the
// loader's base relocations and excluding loader-managed slots. Mutable sections are intentionally not fingerprinted.
std::string PeCodeIdentity(const ReadBytes& file, const ReadBytes* memory = nullptr, uintptr_t loaded_base = 0) {
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 nt{};
  if (!Read(file, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
      !Read(file, static_cast<uint64_t>(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.FileHeader.NumberOfSections > 128 ||
      nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IAT) {
    return {};
  }
  if (memory) {
    IMAGE_NT_HEADERS64 resident{};
    if (!Read(*memory, static_cast<uint64_t>(dos.e_lfanew), resident)) {
      return {};
    }
    // The mapped header's preferred image base may be rebased independently of this process's actual load address.
    resident.OptionalHeader.ImageBase = nt.OptionalHeader.ImageBase;
    if (std::memcmp(&nt, &resident, sizeof(nt)) != 0) {
      return {};
    }
  }

  std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
  const uint64_t table = static_cast<uint64_t>(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                         nt.FileHeader.SizeOfOptionalHeader;
  for (size_t i = 0; i < sections.size(); ++i) {
    if (!Read(file, table + i * sizeof(IMAGE_SECTION_HEADER), sections[i])) {
      return {};
    }
    if (memory) {
      IMAGE_SECTION_HEADER resident{};
      if (!Read(*memory, table + i * sizeof(IMAGE_SECTION_HEADER), resident) ||
          std::memcmp(&resident, &sections[i], sizeof(resident)) != 0) {
        return {};
      }
    }
  }

  const ReadBytes rva = [&](uint64_t offset, void* output, size_t count) {
    for (const auto& section : sections) {
      if (offset >= section.VirtualAddress) {
        const auto delta = offset - section.VirtualAddress;
        if (delta <= section.SizeOfRawData && count <= section.SizeOfRawData - delta) {
          return file(section.PointerToRawData + delta, output, count);
        }
      }
    }

    return false;
  };
  struct ImageRange {
    uint64_t rva;
    size_t width;
  };
  std::vector<ImageRange> relocations;
  const auto& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
  if (directory.Size > 16 * 1024 * 1024) {
    return {};
  }

  for (uint64_t offset = 0; offset < directory.Size;) {
    IMAGE_BASE_RELOCATION block{};
    if (directory.Size - offset < sizeof(block) || !Read(rva, directory.VirtualAddress + offset, block) ||
        block.SizeOfBlock < sizeof(block) || block.SizeOfBlock > directory.Size - offset ||
        (block.SizeOfBlock - sizeof(block)) % sizeof(uint16_t) != 0) {
      return {};
    }

    for (uint64_t entry = sizeof(block); entry < block.SizeOfBlock; entry += sizeof(uint16_t)) {
      uint16_t relocation{};
      if (!Read(rva, directory.VirtualAddress + offset + entry, relocation)) {
        return {};
      }

      const auto type = relocation >> 12;
      if (type == IMAGE_REL_BASED_ABSOLUTE) {
        continue;
      }
      if (type != IMAGE_REL_BASED_DIR64 && type != IMAGE_REL_BASED_HIGHLOW) {
        return {};
      }

      relocations.push_back({static_cast<uint64_t>(block.VirtualAddress) + (relocation & 0xFFF),
                             type == IMAGE_REL_BASED_DIR64 ? sizeof(uint64_t) : sizeof(uint32_t)});
    }

    offset += block.SizeOfBlock;
  }

  const auto& iat = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
  if (uint64_t{iat.VirtualAddress} + iat.Size > nt.OptionalHeader.SizeOfImage) {
    return {};
  }

  std::vector<ImageRange> loader_slots{{iat.VirtualAddress, iat.Size}};
  const auto& config = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
  if (config.Size) {
    DWORD config_size{};
    if (config.Size < sizeof(config_size) || !Read(rva, config.VirtualAddress, config_size) ||
        config_size < sizeof(config_size)) {
      return {};
    }

    constexpr size_t fields[]{offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie),
                              offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFCheckFunctionPointer),
                              offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFDispatchFunctionPointer),
                              offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardRFFailureRoutineFunctionPointer),
                              offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardRFVerifyStackPointerFunctionPointer),
                              offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardXFGCheckFunctionPointer),
                              offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardXFGDispatchFunctionPointer),
                              offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardXFGTableDispatchFunctionPointer),
                              offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, CastGuardOsDeterminedFailureMode),
                              offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardMemcpyFunctionPointer)};
    for (const auto offset : fields) {
      if (offset + sizeof(uint64_t) > std::min<DWORD>(config.Size, config_size)) {
        continue;
      }

      uint64_t address{};
      if (!Read(rva, uint64_t{config.VirtualAddress} + offset, address)) {
        return {};
      }
      if (!address) {
        continue;
      }
      if (address < nt.OptionalHeader.ImageBase ||
          address - nt.OptionalHeader.ImageBase + sizeof(uint64_t) > nt.OptionalHeader.SizeOfImage) {
        return {};
      }

      loader_slots.push_back({address - nt.OptionalHeader.ImageBase, sizeof(uint64_t)});
    }
  }

  std::string fingerprint(reinterpret_cast<const char*>(&nt), sizeof(nt));
  const uint64_t base_delta = loaded_base - nt.OptionalHeader.ImageBase;
  bool has_code = false;
  for (const auto& section : sections) {
    if (!(section.Characteristics & IMAGE_SCN_MEM_READ) ||
        (section.Characteristics & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_DISCARDABLE))) {
      continue;
    }

    const uint64_t size = section.Misc.VirtualSize;
    if (size > 128 * 1024 * 1024 || section.VirtualAddress > nt.OptionalHeader.SizeOfImage ||
        size > nt.OptionalHeader.SizeOfImage - section.VirtualAddress ||
        fingerprint.size() + size > 512 * 1024 * 1024) {
      return {};
    }

    std::vector<char> data(static_cast<size_t>(size), 0);
    if (size && (memory ? !(*memory)(section.VirtualAddress, data.data(), data.size())
                        : !file(section.PointerToRawData, data.data(),
                                static_cast<size_t>(std::min<uint64_t>(size, section.SizeOfRawData))))) {
      return {};
    }
    if (memory) {
      for (const auto& relocation : relocations) {
        if (relocation.rva < section.VirtualAddress || relocation.rva - section.VirtualAddress >= size) {
          continue;
        }

        const auto offset = static_cast<size_t>(relocation.rva - section.VirtualAddress);
        if (relocation.width > data.size() - offset) {
          return {};
        }
        if (relocation.width == sizeof(uint64_t)) {
          uint64_t value{};
          std::memcpy(&value, data.data() + offset, sizeof(value));
          value -= base_delta;
          std::memcpy(data.data() + offset, &value, sizeof(value));
        } else {
          uint32_t value{};
          std::memcpy(&value, data.data() + offset, sizeof(value));
          value -= static_cast<uint32_t>(base_delta);
          std::memcpy(data.data() + offset, &value, sizeof(value));
        }
      }
    }

    for (const auto& slot : loader_slots) {
      const auto begin = std::max<uint64_t>(section.VirtualAddress, slot.rva);
      const auto end = std::min<uint64_t>(section.VirtualAddress + size, slot.rva + slot.width);
      if (begin < end) {
        std::fill(data.begin() + static_cast<ptrdiff_t>(begin - section.VirtualAddress),
                  data.begin() + static_cast<ptrdiff_t>(end - section.VirtualAddress), 0);
      }
    }

    fingerprint.append(reinterpret_cast<const char*>(&section), sizeof(section));
    if (!data.empty()) {
      fingerprint.append(data.data(), data.size());
    }
    has_code |= (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
  }

  return has_code ? "pe-code:" + Sha256String(fingerprint) : std::string{};
}

std::string PeIdentity(const ReadBytes& bytes, bool resident) {
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 nt{};
  if (!Read(bytes, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
      !Read(bytes, static_cast<uint64_t>(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG || nt.FileHeader.NumberOfSections > 128) {
    return {};
  }

  const auto& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
  if (!directory.VirtualAddress || directory.Size > 1024 * 1024 ||
      directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY) != 0) {
    return {};
  }

  uint64_t debug_offset = directory.VirtualAddress;
  if (!resident) {
    bool found = false;
    const uint64_t sections = static_cast<uint64_t>(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                              nt.FileHeader.SizeOfOptionalHeader;
    for (uint16_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
      IMAGE_SECTION_HEADER section{};
      if (!Read(bytes, sections + i * sizeof(section), section)) {
        return {};
      }
      if (directory.VirtualAddress >= section.VirtualAddress) {
        const uint64_t delta = static_cast<uint64_t>(directory.VirtualAddress) - section.VirtualAddress;
        if (delta <= section.SizeOfRawData && directory.Size <= section.SizeOfRawData - delta) {
          debug_offset = section.PointerToRawData + delta;
          found = true;
          break;
        }
      }
    }

    if (!found) {
      return {};
    }
  }

  for (uint64_t offset = 0; offset < directory.Size; offset += sizeof(IMAGE_DEBUG_DIRECTORY)) {
    IMAGE_DEBUG_DIRECTORY debug{};
    if (!Read(bytes, debug_offset + offset, debug)) {
      return {};
    }
    if (debug.Type != IMAGE_DEBUG_TYPE_CODEVIEW || debug.SizeOfData < 24) {
      continue;
    }

    std::array<std::byte, 24> record{};
    if (bytes(resident ? debug.AddressOfRawData : debug.PointerToRawData, record.data(), record.size()) &&
        std::memcmp(record.data(), "RSDS", 4) == 0) {
      return "pe:" + Hex(std::span{record}.subspan(4));
    }
  }

  return {};
}

#elif defined(__linux__)

std::string ElfNoteIdentity(std::span<const std::byte> notes) {
  size_t offset = 0;
  while (offset <= notes.size() && notes.size() - offset >= sizeof(Elf64_Nhdr)) {
    Elf64_Nhdr note{};
    std::memcpy(&note, notes.data() + offset, sizeof(note));
    offset += sizeof(note);
    const auto name_size = (static_cast<uint64_t>(note.n_namesz) + 3) & ~uint64_t{3};
    const auto value_size = (static_cast<uint64_t>(note.n_descsz) + 3) & ~uint64_t{3};
    if (name_size > notes.size() - offset || value_size > notes.size() - offset - name_size) {
      return {};
    }
    if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 && note.n_descsz > 0 && note.n_descsz <= 64 &&
        std::memcmp(notes.data() + offset, "GNU", 4) == 0) {
      return "elf:" + Hex(notes.subspan(offset + static_cast<size_t>(name_size), note.n_descsz));
    }

    offset += static_cast<size_t>(name_size + value_size);
  }

  return {};
}

std::string ElfFileIdentity(const ReadBytes& bytes) {
  Elf64_Ehdr header{};
  if (!Read(bytes, 0, header) || std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
      header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
      header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum > 4096 ||
      header.e_phoff > uint64_t{1024} * 1024 * 1024) {
    return {};
  }

  for (uint16_t i = 0; i < header.e_phnum; ++i) {
    Elf64_Phdr segment{};
    if (!Read(bytes, header.e_phoff + i * sizeof(segment), segment)) {
      return {};
    }
    if (segment.p_type != PT_NOTE || segment.p_filesz > 1024 * 1024) {
      continue;
    }

    std::vector<std::byte> notes(static_cast<size_t>(segment.p_filesz));
    if (!bytes(segment.p_offset, notes.data(), notes.size())) {
      return {};
    }
    if (auto identity = ElfNoteIdentity(notes); !identity.empty()) {
      return identity;
    }
  }

  return {};
}

std::string ElfResidentIdentity(const void* address) {
  struct Context {
    uintptr_t address;
    std::string identity;
  } context{reinterpret_cast<uintptr_t>(address), {}};
  dl_iterate_phdr(
      [](dl_phdr_info* image, size_t, void* data) {
        auto& value = *static_cast<Context*>(data);
        const auto base = image->dlpi_addr;
        const auto contains = [&](uintptr_t start, uint64_t size) {
          for (uint16_t i = 0; i < image->dlpi_phnum; ++i) {
            const auto& load = image->dlpi_phdr[i];
            const auto begin = base + load.p_vaddr;
            if (load.p_type == PT_LOAD && (load.p_flags & PF_R) && start >= begin && start - begin <= load.p_memsz &&
                size <= load.p_memsz - (start - begin)) {
              return true;
            }
          }

          return false;
        };
        if (!contains(value.address, 1)) {
          return 0;
        }

        for (uint16_t i = 0; i < image->dlpi_phnum; ++i) {
          const auto& note = image->dlpi_phdr[i];
          const auto start = base + note.p_vaddr;
          if (note.p_type == PT_NOTE && note.p_filesz <= 1024 * 1024 && contains(start, note.p_filesz)) {
            value.identity =
                ElfNoteIdentity({reinterpret_cast<const std::byte*>(start), static_cast<size_t>(note.p_filesz)});
            if (!value.identity.empty()) {
              break;
            }
          }
        }

        return 1;
      },
      &context);
  return context.identity;
}

#elif defined(__APPLE__)

std::string MachIdentity(const ReadBytes& bytes) {
  mach_header_64 header{};
  if (!Read(bytes, 0, header) || header.magic != MH_MAGIC_64 || header.sizeofcmds > 1024 * 1024 ||
      header.ncmds > 65536) {
    return {};
  }

  uint64_t offset = sizeof(header);
  const uint64_t end = offset + header.sizeofcmds;
  for (uint32_t i = 0; i < header.ncmds; ++i) {
    load_command command{};
    if (offset > end || end - offset < sizeof(command) || !Read(bytes, offset, command) ||
        command.cmdsize < sizeof(command) || command.cmdsize > end - offset) {
      return {};
    }
    if (command.cmd == LC_UUID && command.cmdsize >= sizeof(uuid_command)) {
      uuid_command uuid{};
      if (Read(bytes, offset, uuid)) {
        return "macho:" + Hex(std::as_bytes(std::span{uuid.uuid}));
      }

      return {};
    }

    offset += command.cmdsize;
  }

  return {};
}

#endif

}  // namespace

std::string FileModuleIdentity(const std::filesystem::path& path) {
  ImageFile file(path);
  const auto bytes = file.Reader();
  std::string identity;
#ifdef _WIN32
  identity = PeIdentity(bytes, false);
  if (identity.empty()) {
    identity = PeCodeIdentity(bytes);
  }
#elif defined(__linux__)
  identity = ElfFileIdentity(bytes);
#elif defined(__APPLE__)
  identity = MachIdentity(bytes);
#endif
  if (identity.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native hosting cannot verify module image identity: ", path);
  }

  return identity;
}

ResidentModule ResidentModuleAt(const void* address) {
  if (!address) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "resident module address must not be null");
  }

  ResidentModule module;
#ifdef _WIN32
  HMODULE handle{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &handle)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "cannot locate resident native module");
  }

  std::vector<wchar_t> path(512);
  while (path.size() <= 32768) {
    const auto length = GetModuleFileNameW(handle, path.data(), static_cast<DWORD>(path.size()));
    if (!length) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "cannot resolve resident native module path");
    }
    if (length < path.size()) {
      module.path = std::filesystem::path(path.data(), path.data() + length);
      break;
    }

    path.resize(path.size() * 2);
  }

  const auto* base = reinterpret_cast<const std::byte*>(handle);
  IMAGE_DOS_HEADER dos{};
  std::memcpy(&dos, base, sizeof(dos));
  IMAGE_NT_HEADERS64 nt{};
  if (dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= 0) {
    std::memcpy(&nt, base + dos.e_lfanew, sizeof(nt));
    const auto memory = MemoryReader(base, nt.OptionalHeader.SizeOfImage);
    module.image_identity = PeIdentity(memory, true);
    if (module.image_identity.empty()) {
      ImageFile file(module.path);
      const auto bytes = file.Reader();
      const auto fingerprint = PeCodeIdentity(bytes, &memory, reinterpret_cast<uintptr_t>(handle));
      if (!fingerprint.empty() && fingerprint == PeCodeIdentity(bytes)) {
        module.image_identity = PeIdentity(bytes, false);
        if (module.image_identity.empty()) {
          module.image_identity = fingerprint;
        }
      }
    }
  }
#else
  Dl_info info{};
  if (!dladdr(address, &info) || !info.dli_fname || !info.dli_fbase) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "cannot locate resident native module");
  }

  module.path = info.dli_fname;
#ifdef __linux__
  module.image_identity = ElfResidentIdentity(address);
#elif defined(__APPLE__)
  mach_header_64 header{};
  std::memcpy(&header, info.dli_fbase, sizeof(header));
  if (header.magic == MH_MAGIC_64 && header.sizeofcmds <= 1024 * 1024) {
    module.image_identity = MachIdentity(MemoryReader(info.dli_fbase, sizeof(header) + header.sizeofcmds));
  }
#endif
#endif
  if (module.path.empty() || module.image_identity.empty()) {
#ifdef __linux__
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "native hosting requires GNU build IDs in Foundry, ORT and GenAI runtime modules; "
             "cannot identify resident module: ",
             module.path);
#else
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "native hosting cannot identify the resident runtime module: ", module.path);
#endif
  }

  module.path = std::filesystem::canonical(module.path);
  return module;
}

}  // namespace fl::hosting
