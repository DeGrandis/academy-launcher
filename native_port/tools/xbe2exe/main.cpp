// Converts an Xbox XBE into a 32-bit Windows PE that maps the game image at its original
// addresses and hands control to cw_runtime.dll!CwRun.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kXbeBase = 0x10000;
constexpr std::uint32_t kPageSize = 0x1000;
constexpr std::uint32_t kFileAlignment = 0x200;
constexpr std::uint32_t kNtHeadersOffset = 0xC00;
constexpr std::uint32_t kEntryRetailKey = 0xA8FC57AB;
constexpr std::uint32_t kEntryDebugKey = 0x94859D4B;

std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

std::uint32_t read32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 4 > data.size()) {
        throw std::runtime_error("read past end of XBE");
    }
    std::uint32_t value;
    std::memcpy(&value, data.data() + offset, 4);
    return value;
}

void write32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    std::memcpy(data.data() + offset, &value, 4);
}

std::vector<std::uint8_t> readFile(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error(std::string("cannot open ") + path);
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

struct XbeSection {
    std::uint32_t virtualAddress;
    std::uint32_t virtualSize;
    std::uint32_t rawAddress;
    std::uint32_t rawSize;
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: xbe2exe <default.xbe> <output.exe>\n");
        return 1;
    }

    try {
        const std::vector<std::uint8_t> xbe = readFile(argv[1]);
        if (xbe.size() < 0x178 || std::memcmp(xbe.data(), "XBEH", 4) != 0) {
            throw std::runtime_error("not an XBE file");
        }

        const std::uint32_t baseAddress = read32(xbe, 0x104);
        const std::uint32_t headersSize = read32(xbe, 0x108);
        const std::uint32_t sectionCount = read32(xbe, 0x11C);
        const std::uint32_t sectionHeadersAddress = read32(xbe, 0x120);
        if (baseAddress != kXbeBase) {
            throw std::runtime_error("unexpected XBE base address");
        }
        if (headersSize > kNtHeadersOffset) {
            throw std::runtime_error("XBE headers overlap the PE header area");
        }

        std::vector<XbeSection> sections;
        std::uint32_t imageEnd = headersSize;
        for (std::uint32_t index = 0; index < sectionCount; ++index) {
            const std::size_t header = sectionHeadersAddress - baseAddress + index * 56;
            XbeSection section{
                read32(xbe, header + 4) - baseAddress,
                read32(xbe, header + 8),
                read32(xbe, header + 12),
                read32(xbe, header + 16),
            };
            if (section.virtualAddress < kPageSize) {
                throw std::runtime_error("XBE section overlaps the PE header page");
            }
            imageEnd = std::max(imageEnd, section.virtualAddress + section.virtualSize);
            sections.push_back(section);
        }

        std::uint32_t entry = read32(xbe, 0x128) ^ kEntryRetailKey;
        if (entry < baseAddress || entry >= baseAddress + imageEnd) {
            entry = read32(xbe, 0x128) ^ kEntryDebugKey;
        }

        const std::uint32_t imageSectionRva = kPageSize;
        const std::uint32_t imageSectionVirtualSize = alignUp(imageEnd, kPageSize) - imageSectionRva;
        const std::uint32_t imageSectionRawSize = alignUp(imageSectionVirtualSize, kFileAlignment);
        const std::uint32_t runtimeSectionRva = alignUp(imageEnd, kPageSize);
        const std::uint32_t runtimeSectionRawSize = kFileAlignment;
        const std::uint32_t sizeOfImage = runtimeSectionRva + kPageSize;

        std::vector<std::uint8_t> pe(kPageSize + imageSectionRawSize + runtimeSectionRawSize, 0);

        // The original XBE header stays at 0x10000 because the game reads it at runtime.
        std::memcpy(pe.data(), xbe.data(), headersSize);
        for (const XbeSection& section : sections) {
            const std::uint32_t copySize = std::min(section.rawSize, section.virtualSize);
            if (section.rawAddress + copySize > xbe.size()) {
                throw std::runtime_error("XBE section data is truncated");
            }
            std::memcpy(pe.data() + section.virtualAddress, xbe.data() + section.rawAddress, copySize);
        }

        // DOS header fields only overwrite the unused XBE magic and signature bytes.
        pe[0] = 'M';
        pe[1] = 'Z';
        write32(pe, 0x3C, kNtHeadersOffset);

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(pe.data() + kNtHeadersOffset);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
        nt->FileHeader.NumberOfSections = 2;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
        nt->FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_RELOCS_STRIPPED | IMAGE_FILE_32BIT_MACHINE;

        IMAGE_OPTIONAL_HEADER32& optional = nt->OptionalHeader;
        optional.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        optional.MajorLinkerVersion = 14;
        optional.SizeOfCode = imageSectionVirtualSize;
        optional.AddressOfEntryPoint = runtimeSectionRva + 0x60;
        optional.BaseOfCode = imageSectionRva;
        optional.BaseOfData = imageSectionRva;
        optional.ImageBase = kXbeBase;
        optional.SectionAlignment = kPageSize;
        optional.FileAlignment = kFileAlignment;
        optional.MajorOperatingSystemVersion = 6;
        optional.MajorSubsystemVersion = 6;
        optional.SizeOfImage = sizeOfImage;
        optional.SizeOfHeaders = kPageSize;
        optional.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
        optional.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_TERMINAL_SERVER_AWARE;
        optional.SizeOfStackReserve = 0x400000;
        optional.SizeOfStackCommit = 0x10000;
        optional.SizeOfHeapReserve = 0x100000;
        optional.SizeOfHeapCommit = 0x1000;
        optional.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {runtimeSectionRva, 0x28};
        optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT] = {runtimeSectionRva + 0x30, 8};

        auto* sectionHeaders = reinterpret_cast<IMAGE_SECTION_HEADER*>(reinterpret_cast<std::uint8_t*>(&optional) + sizeof(IMAGE_OPTIONAL_HEADER32));
        std::memcpy(sectionHeaders[0].Name, ".xbe", 4);
        sectionHeaders[0].Misc.VirtualSize = imageSectionVirtualSize;
        sectionHeaders[0].VirtualAddress = imageSectionRva;
        sectionHeaders[0].SizeOfRawData = imageSectionRawSize;
        sectionHeaders[0].PointerToRawData = kPageSize;
        sectionHeaders[0].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_CNT_INITIALIZED_DATA |
            IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

        std::memcpy(sectionHeaders[1].Name, ".cwrt", 5);
        sectionHeaders[1].Misc.VirtualSize = kPageSize;
        sectionHeaders[1].VirtualAddress = runtimeSectionRva;
        sectionHeaders[1].SizeOfRawData = runtimeSectionRawSize;
        sectionHeaders[1].PointerToRawData = kPageSize + imageSectionRawSize;
        sectionHeaders[1].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_CNT_INITIALIZED_DATA |
            IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

        // .cwrt layout: import descriptors, lookup table, IAT, hint/name, dll name, entry stub.
        std::uint8_t* runtime = pe.data() + sectionHeaders[1].PointerToRawData;
        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(runtime);
        descriptor->OriginalFirstThunk = runtimeSectionRva + 0x28;
        descriptor->FirstThunk = runtimeSectionRva + 0x30;
        descriptor->Name = runtimeSectionRva + 0x48;
        std::uint32_t hintName = runtimeSectionRva + 0x38;
        std::memcpy(runtime + 0x28, &hintName, 4);
        std::memcpy(runtime + 0x30, &hintName, 4);
        std::memcpy(runtime + 0x3A, "CwRun", 6);
        std::memcpy(runtime + 0x48, "cw_runtime.dll", 15);

        // call dword ptr [IAT] ; push entry would be unnecessary because CwRun reads the XBE header.
        const std::uint32_t iatAddress = kXbeBase + runtimeSectionRva + 0x30;
        std::uint8_t* stub = runtime + 0x60;
        stub[0] = 0xFF;
        stub[1] = 0x15;
        std::memcpy(stub + 2, &iatAddress, 4);
        stub[6] = 0xC3;

        std::ofstream output(argv[2], std::ios::binary);
        if (!output) {
            throw std::runtime_error(std::string("cannot write ") + argv[2]);
        }
        output.write(reinterpret_cast<const char*>(pe.data()), static_cast<std::streamsize>(pe.size()));
        std::printf("xbe2exe: %s -> %s (image end 0x%X, entry 0x%X)\n", argv[1], argv[2], kXbeBase + imageEnd, entry);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "xbe2exe: %s\n", error.what());
        return 1;
    }
}
