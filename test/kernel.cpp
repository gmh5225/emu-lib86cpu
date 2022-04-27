/*
 * lib86cpu cxbxrkrnl app test generator (https://github.com/ergo720/cxbxrkrnl)
 * Demonstrates running a kernel inside lib86cpu
 *
 * ergo720                Copyright (c) 2022
 */

#include <fstream>
#include "lib86cpu.h"
#include "run.h"

#define CONTIGUOUS_MEMORY_BASE 0x80000000

#define DBG_STR_PORT 0x200

// Windows PE format definitions
#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550
#define IMAGE_FILE_MACHINE_I386 0x14c
#define IMAGE_NT_OPTIONAL_HDR32_MAGIC 0x10b
#define IMAGE_SUBSYSTEM_NATIVE 1
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16
#define IMAGE_SIZEOF_SHORT_NAME 8

typedef struct _IMAGE_DOS_HEADER {
	uint16_t e_magic;
	uint16_t e_cblp;
	uint16_t e_cp;
	uint16_t e_crlc;
	uint16_t e_cparhdr;
	uint16_t e_minalloc;
	uint16_t e_maxalloc;
	uint16_t e_ss;
	uint16_t e_sp;
	uint16_t e_csum;
	uint16_t e_ip;
	uint16_t e_cs;
	uint16_t e_lfarlc;
	uint16_t e_ovno;
	uint16_t e_res[4];
	uint16_t e_oemid;
	uint16_t e_oeminfo;
	uint16_t e_res2[10];
	int32_t e_lfanew;
} IMAGE_DOS_HEADER, *PIMAGE_DOS_HEADER;

typedef struct _IMAGE_FILE_HEADER {
	uint16_t Machine;
	uint16_t NumberOfSections;
	uint32_t TimeDateStamp;
	uint32_t PointerToSymbolTable;
	uint32_t NumberOfSymbols;
	uint16_t SizeOfOptionalHeader;
	uint16_t Characteristics;
} IMAGE_FILE_HEADER, *PIMAGE_FILE_HEADER;

typedef struct _IMAGE_DATA_DIRECTORY {
	uint32_t VirtualAddress;
	uint32_t Size;
} IMAGE_DATA_DIRECTORY, *PIMAGE_DATA_DIRECTORY;

typedef struct _IMAGE_OPTIONAL_HEADER {
	uint16_t Magic;
	uint8_t MajorLinkerVersion;
	uint8_t MinorLinkerVersion;
	uint32_t SizeOfCode;
	uint32_t SizeOfInitializedData;
	uint32_t SizeOfUninitializedData;
	uint32_t AddressOfEntryPoint;
	uint32_t BaseOfCode;
	uint32_t BaseOfData;
	uint32_t ImageBase;
	uint32_t SectionAlignment;
	uint32_t FileAlignment;
	uint16_t MajorOperatingSystemVersion;
	uint16_t MinorOperatingSystemVersion;
	uint16_t MajorImageVersion;
	uint16_t MinorImageVersion;
	uint16_t MajorSubsystemVersion;
	uint16_t MinorSubsystemVersion;
	uint32_t Win32VersionValue;
	uint32_t SizeOfImage;
	uint32_t SizeOfHeaders;
	uint32_t CheckSum;
	uint16_t Subsystem;
	uint16_t DllCharacteristics;
	uint32_t SizeOfStackReserve;
	uint32_t SizeOfStackCommit;
	uint32_t SizeOfHeapReserve;
	uint32_t SizeOfHeapCommit;
	uint32_t LoaderFlags;
	uint32_t NumberOfRvaAndSizes;
	IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER32, *PIMAGE_OPTIONAL_HEADER32;

typedef struct _IMAGE_NT_HEADERS {
	uint32_t Signature;
	IMAGE_FILE_HEADER FileHeader;
	IMAGE_OPTIONAL_HEADER32 OptionalHeader;
} IMAGE_NT_HEADERS32;
typedef struct _IMAGE_NT_HEADERS *PIMAGE_NT_HEADERS32;

typedef struct _IMAGE_SECTION_HEADER {
	uint8_t Name[IMAGE_SIZEOF_SHORT_NAME];
	union {
		uint32_t PhysicalAddress;
		uint32_t VirtualSize;
	} Misc;
	uint32_t VirtualAddress;
	uint32_t SizeOfRawData;
	uint32_t PointerToRawData;
	uint32_t PointerToRelocations;
	uint32_t PointerToLinenumbers;
	uint16_t NumberOfRelocations;
	uint16_t NumberOfLinenumbers;
	uint32_t Characteristics;
} IMAGE_SECTION_HEADER, *PIMAGE_SECTION_HEADER;


static void
dbg_write_handler(addr_t addr, size_t size, const uint64_t value, void* opaque)
{
	switch (addr)
	{
	case DBG_STR_PORT: {
		if (size == 4) {
			// NOTE: get_host_ptr will only work if the string is allocated with a contiguous allocation in physical memory, to avoid
			// issues with allocations spanning pages; cxbxrkrnl should guarantee this
			std::printf("Received a new debug string from kernel:\n%s", get_host_ptr(static_cast<cpu_t *>(opaque), static_cast<addr_t>(value)));
		}
		else {
			std::printf("%s: unexpected i/o write at port %d with size %d\n", __func__, addr, size);
		}
	}
	break;

	default:
		std::printf("%s: unexpected i/o write at port %d\n", __func__, addr);
	}
}

bool
gen_cxbxrkrnl_test(const std::string &executable)
{
	size_t ramsize = 64 * 1024 * 1024;

	// load kernel exe file
	std::ifstream ifs(executable.c_str(), std::ios_base::in | std::ios_base::binary);
	if (!ifs.is_open()) {
		std::printf("Could not open binary file \"%s\"!\n", executable.c_str());
		return false;
	}
	ifs.seekg(0, ifs.end);
	std::streampos length = ifs.tellg();
	ifs.seekg(0, ifs.beg);

	// Sanity checks on the kernel exe size
	if (length == 0) {
		std::printf("Size of binary file \"%s\" detected as zero!\n", executable.c_str());
		return false;
	}
	else if (length > ramsize) {
		std::printf("Binary file \"%s\" doesn't fit inside RAM!\n", executable.c_str());
		return false;
	}

	std::unique_ptr<char[]> krnl_buff { new char[static_cast<unsigned>(length)] };
	if (!krnl_buff) {
		std::printf("Could not allocate kernel buffer!\n");
		return false;
	}
	ifs.read(krnl_buff.get(), length);
	ifs.close();

	// Sanity checks on the kernel exe file
	PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(krnl_buff.get());
	if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
		std::printf("Kernel image has an invalid dos header signature!\n");
		return false;
	}

	PIMAGE_NT_HEADERS32 peHeader = reinterpret_cast<PIMAGE_NT_HEADERS32>(reinterpret_cast<uint8_t*>(dosHeader) + dosHeader->e_lfanew);
	if (peHeader->Signature != IMAGE_NT_SIGNATURE ||
		peHeader->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
		peHeader->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
		peHeader->OptionalHeader.Subsystem != IMAGE_SUBSYSTEM_NATIVE) {
		std::printf("Kernel image has an invalid nt header signature!\n");
		return false;
	}

	if (peHeader->OptionalHeader.ImageBase != 0x80010000) {
		std::printf("Kernel image has an incorrect image base address!\n");
		return false;
	}

	// Init lib86cpu
	if (!LIB86CPU_CHECK_SUCCESS(cpu_new(ramsize, cpu))) {
		std::printf("Failed to create cpu!\n");
		return false;
	}

	if (!LIB86CPU_CHECK_SUCCESS(mem_init_region_ram(cpu, 0, ramsize, 1))) {
		std::printf("Failed to initialize ram memory!\n");
		return false;
	}

	if (!LIB86CPU_CHECK_SUCCESS(mem_init_region_ram(cpu, CONTIGUOUS_MEMORY_BASE, ramsize, 1))) {
		std::printf("Failed to initialize contiguous memory!\n");
		return false;
	}

	if (!LIB86CPU_CHECK_SUCCESS(mem_init_region_io(cpu, DBG_STR_PORT, 4, true, nullptr, dbg_write_handler, cpu, 1))) {
		std::printf("Failed to initialize debug port!\n");
		return false;
	}

	// Load kernel exe into ram
	uint8_t *ram = get_ram_ptr(cpu);
	std::memcpy(&ram[peHeader->OptionalHeader.ImageBase - CONTIGUOUS_MEMORY_BASE], dosHeader, peHeader->OptionalHeader.SizeOfHeaders);

	PIMAGE_SECTION_HEADER sections = reinterpret_cast<PIMAGE_SECTION_HEADER>(reinterpret_cast<uint8_t *>(peHeader) + sizeof(IMAGE_NT_HEADERS32));
	for (uint16_t i = 0; i < peHeader->FileHeader.NumberOfSections; ++i) {
		uint8_t* dest = &ram[peHeader->OptionalHeader.ImageBase - CONTIGUOUS_MEMORY_BASE] + sections[i].VirtualAddress;
		if (sections[i].SizeOfRawData) {
			std::memcpy(dest, reinterpret_cast<uint8_t*>(dosHeader) + sections[i].PointerToRawData, sections[i].SizeOfRawData);
		}
		else {
			std::memset(dest, 0, sections[i].Misc.VirtualSize);
		}
	}

	mem_fill_block(cpu, 0xF000, 0x1000, 0);
	uint32_t pde = 0xE3; // large, dirty, accessed, r/w, present
	mem_write_block(cpu, 0xFF000, 4, &pde);
	for (int i = 0; i < 16; ++i) {
		mem_write_block(cpu, 0xF000 + (i * 4), 4, &pde); // this identity maps all physical memory
		pde += 0x400000;
	}
	pde = 0x800000E3;
	for (int i = 0; i < 16; ++i) {
		mem_write_block(cpu, 0xF800 + (i * 4), 4, &pde); // this identity maps all contiguous memory
		pde += 0x400000;
	}
	pde = 0x0000F063; // dirty, accessed, r/w, present
	mem_write_block(cpu, 0xFC00, 4, &pde); // this maps the pd at 0xC0000000

	write_gpr(cpu, 0x0, REG_CS, SEG_BASE);
	write_gpr(cpu, 0x0, REG_ES, SEG_BASE);
	write_gpr(cpu, 0x0, REG_DS, SEG_BASE);
	write_gpr(cpu, 0x0, REG_SS, SEG_BASE);
	write_gpr(cpu, 0x0, REG_FS, SEG_BASE);
	write_gpr(cpu, 0x0, REG_GS, SEG_BASE);

	write_gpr(cpu, 0xCF9F00, REG_CS, SEG_FLG);
	write_gpr(cpu, 0xCF9700, REG_ES, SEG_FLG);
	write_gpr(cpu, 0xCF9700, REG_DS, SEG_FLG);
	write_gpr(cpu, 0xCF9700, REG_SS, SEG_FLG);
	write_gpr(cpu, 0xCF9700, REG_FS, SEG_FLG);
	write_gpr(cpu, 0xCF9700, REG_GS, SEG_FLG);

	write_gpr(cpu, 0x80000001, REG_CR0); // protected, paging
	write_gpr(cpu, 0xF000, REG_CR3); // pd addr
	write_gpr(cpu, 0x10, REG_CR4); // pse

	write_gpr(cpu, 0x80400000, REG_ESP);
	write_gpr(cpu, 0x80400000, REG_EBP);
	write_gpr(cpu, peHeader->OptionalHeader.ImageBase + peHeader->OptionalHeader.AddressOfEntryPoint, REG_EIP);

	return true;
}