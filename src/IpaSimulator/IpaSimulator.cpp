#include "IpaSimulator.hpp"

#include "WrapperIndex.hpp"

#include <ffi.h>
#include <psapi.h> // for `GetModuleInformation`
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <winrt/base.h>

#include <filesystem>
#include <map>

using namespace std;
using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::Storage;

// Binary operators on enums. Taken from
// <https://stackoverflow.com/a/23152590/9080566>.
template <class T> inline T operator~(T a) { return (T) ~(int)a; }
template <class T> inline T operator|(T a, T b) { return (T)((int)a | (int)b); }
template <class T> inline T operator&(T a, T b) { return (T)((int)a & (int)b); }
template <class T> inline T operator^(T a, T b) { return (T)((int)a ^ (int)b); }
template <class T> inline T &operator|=(T &a, T b) {
  return (T &)((int &)a |= (int)b);
}
template <class T> inline T &operator&=(T &a, T b) {
  return (T &)((int &)a &= (int)b);
}
template <class T> inline T &operator^=(T &a, T b) {
  return (T &)((int &)a ^= (int)b);
}

static const uint8_t *bytes(const void *Ptr) {
  return reinterpret_cast<const uint8_t *>(Ptr);
}

template <typename T> static string to_hex_string(T Value) {
  std::stringstream SS;
  SS << std::hex << Value;
  return SS.str();
}

static void callUCSimple(uc_err Err) {
  // TODO: Throw better exceptions.
  if (Err != UC_ERR_OK)
    throw "unicorn error";
}
void DynamicLoader::callUC(uc_err Err) {
  if (Err != UC_ERR_OK) {
    OutputDebugStringA("Error: unicorn failed with ");
    OutputDebugStringA(to_string(Err).c_str());
    uint32_t Addr;
    callUCSimple(uc_reg_read(UC, UC_ARM_REG_PC, &Addr));
    OutputDebugStringA(" at ");
    dumpAddr(Addr);
    OutputDebugStringA(".\n");
    // TODO: Throw better exceptions.
    throw "unicorn error";
  }
}

bool LoadedLibrary::isInRange(uint64_t Addr) {
  return StartAddress <= Addr && Addr < StartAddress + Size;
}
void LoadedLibrary::checkInRange(uint64_t Addr) {
  // TODO: Do more flexible error reporting here.
  if (!isInRange(Addr))
    throw "address out of range";
}

uint64_t LoadedDylib::findSymbol(DynamicLoader &DL, const string &Name) {
  using namespace LIEF::MachO;

  if (!Bin.has_symbol(Name)) {
    // Try also re-exported libraries.
    for (DylibCommand &Lib : Bin.libraries()) {
      if (Lib.command() != LOAD_COMMAND_TYPES::LC_REEXPORT_DYLIB)
        continue;

      LoadedLibrary *LL = DL.load(Lib.name());
      if (!LL)
        continue;

      // If the target library is DLL, it doesn't have underscore prefixes, so
      // we need to remove it.
      uint64_t SymAddr;
      if (!LL->hasUnderscorePrefix() && Name[0] == '_')
        SymAddr = LL->findSymbol(DL, Name.substr(1));
      else
        SymAddr = LL->findSymbol(DL, Name);

      if (SymAddr)
        return SymAddr;
    }
    return 0;
  }
  return StartAddress + Bin.get_symbol(Name).value();
}

uint64_t LoadedDylib::getSection(const std::string &Name, uint64_t *Size) {
  using namespace LIEF::MachO;

  if (!Bin.has_section(Name))
    return 0;

  Section &Sect = Bin.get_section(Name);
  if (Size)
    *Size = Sect.size();
  return Sect.address() + StartAddress;
}

uint64_t LoadedDll::getSection(const std::string &Name, uint64_t *Size) {
  using namespace LIEF::MachO;

  // We only support DLLs with `_mh_dylib_header`.
  if (!MachOPoser)
    return 0;

  // Enumerate segments.
  uint64_t Slide, Addr;
  bool HasSlide = false, HasAddr = false;
  auto Header = reinterpret_cast<const mach_header *>(StartAddress);
  auto Cmd = reinterpret_cast<const load_command *>(Header + 1);
  for (size_t I = 0; I != Header->ncmds; ++I) {
    if (Cmd->cmd == (uint32_t)LOAD_COMMAND_TYPES::LC_SEGMENT) {
      auto Seg = reinterpret_cast<const segment_command_32 *>(Cmd);

      // Look for segment `__TEXT` to compute slide.
      if (!HasSlide && !strncmp(Seg->segname, "__TEXT", 16)) {
        Slide = StartAddress - Seg->vmaddr;
        HasSlide = true;
        if (HasAddr)
          break;
      }

      // Enumerate segment's sections.
      if (!HasAddr) {
        auto Sect = reinterpret_cast<const section_32 *>(
            bytes(Cmd) + sizeof(segment_command_32));
        for (size_t J = 0; J != Seg->nsects; ++J) {
          // Note that `Sect->sectname` is not necessarily null-terminated!
          if (!strncmp(Sect->sectname, Name.c_str(), 16)) {
            // We have found it.
            if (Size)
              *Size = Sect->size;
            Addr = Sect->addr;
            HasAddr = true;
            if (HasSlide)
              break;
          }

          // Move to the next `section`.
          Sect = reinterpret_cast<const section_32 *>(bytes(Sect) +
                                                      sizeof(section_32));
        }
      }
    }

    // Move to the next `load_command`.
    Cmd = reinterpret_cast<const load_command *>(bytes(Cmd) + Cmd->cmdsize);
  }

  if (HasSlide && HasAddr)
    return Addr + Slide;
  return 0;
}

uint64_t LoadedDll::findSymbol(DynamicLoader &DL, const string &Name) {
  return (uint64_t)GetProcAddress(Ptr, Name.c_str());
}

static bool isFileValid(const BinaryPath &BP) {
  if (BP.Relative) {
    return Package::Current()
               .InstalledLocation()
               .TryGetItemAsync(to_hstring(BP.Path))
               .get() != nullptr;
  }
  try {
    StorageFile File =
        StorageFile::GetFileFromPathAsync(to_hstring(BP.Path)).get();
    return true;
  } catch (...) {
    return false;
  }
}

static bool startsWith(const std::string &S, const std::string &Prefix) {
  return !S.compare(0, Prefix.length(), Prefix);
}
static bool endsWith(const std::string &S, const std::string &Suffix) {
  return !S.compare(S.length() - Suffix.length(), Suffix.length(), Suffix);
}

DynamicLoader::DynamicLoader(uc_engine *UC)
    : UC(UC), Running(false), Restart(false), Continue(false) {
  // Map "kernel" page.
  void *KernelPtr = _aligned_malloc(PageSize, PageSize);
  KernelAddr = reinterpret_cast<uint64_t>(KernelPtr);
  mapMemory(KernelAddr, PageSize, UC_PROT_NONE);
}

LoadedLibrary *DynamicLoader::load(const string &Path) {
  BinaryPath BP(resolvePath(Path));

  auto I = LIs.find(BP.Path);
  if (I != LIs.end())
    return I->second.get();

  // Check that file exists.
  if (!isFileValid(BP)) {
    error("invalid file: " + BP.Path);
    return nullptr;
  }

  OutputDebugStringA("Info: loading library ");
  OutputDebugStringA(BP.Path.c_str());
  OutputDebugStringA("...\n");

  LoadedLibrary *L;
  if (LIEF::MachO::is_macho(BP.Path))
    L = loadMachO(BP.Path);
  else if (LIEF::PE::is_pe(BP.Path))
    L = loadPE(BP.Path);
  else {
    error("invalid binary type: " + BP.Path);
    return nullptr;
  }

  // Recognize wrapper DLLs.
  if (L)
    L->IsWrapperDLL = BP.Relative && startsWith(BP.Path, "gen\\") &&
                      endsWith(BP.Path, ".wrapper.dll");

  return L;
}

// Reports non-fatal error to the user.
void DynamicLoader::error(const string &Msg, bool AppendLastError) {
  hstring HS(to_hstring("Error occurred: " + Msg + "."));
  if (AppendLastError) {
    hresult_error Err(HRESULT_FROM_WIN32(GetLastError()));
    HS = HS + L"\n" + Err.message();
  }

  // Output the error to debugging console.
  HS = HS + L"\n";
  OutputDebugStringW(HS.c_str());
}
// Inspired by `ImageLoaderMachO::segmentsCanSlide`.
bool DynamicLoader::canSegmentsSlide(LIEF::MachO::Binary &Bin) {
  using namespace LIEF::MachO;

  auto FType = Bin.header().file_type();
  return FType == FILE_TYPES::MH_DYLIB || FType == FILE_TYPES::MH_BUNDLE ||
         (FType == FILE_TYPES::MH_EXECUTE && Bin.is_pie());
}
// TODO: What if the mappings overlap?
void DynamicLoader::mapMemory(uint64_t Addr, uint64_t Size, uc_prot Perms) {
  if (uc_mem_map_ptr(UC, Addr, Size, Perms, reinterpret_cast<void *>(Addr)))
    error("couldn't map memory at 0x" + to_hex_string(Addr) + " of size 0x" +
          to_hex_string(Size));
}
BinaryPath DynamicLoader::resolvePath(const string &Path) {
  if (!Path.empty() && Path[0] == '/') {
    // This path is something like
    // `/System/Library/Frameworks/Foundation.framework/Foundation`.
    return BinaryPath{filesystem::path("gen" + Path).make_preferred().string(),
                      /* Relative */ true};
  }

  // TODO: Handle also `.ipa`-relative paths.
  return BinaryPath{Path, filesystem::path(Path).is_relative()};
}
static uint32_t getLow(uint64_t Val) {
  LARGE_INTEGER LI;
  LI.QuadPart = Val;
  return LI.LowPart;
}
static uint32_t getHigh(uint64_t Val) {
  LARGE_INTEGER LI;
  LI.QuadPart = Val;
  return LI.HighPart;
}
LoadedLibrary *DynamicLoader::loadMachO(const string &Path) {
  using namespace LIEF::MachO;

  unique_ptr<FatBinary> Fat(Parser::parse(Path, ParserConfig::quick()));
  Binary &Bin = Fat->at(0);

#if 0
  auto LL = make_unique<LoadedDylib>(Parser::parse(Path));
  LoadedDylib *LLP = LL.get();

  // TODO: Select the correct binary more intelligently.
  Binary &Bin = LL->Bin;

  LIs[Path] = move(LL);
#endif

  // Check header.
  Header &Hdr = Bin.header();
  if (Hdr.cpu_type() != CPU_TYPES::CPU_TYPE_ARM)
    error("expected ARM binary");
  // Ensure that segments are continuous (required by `relocateSegment`).
  if (Hdr.has(HEADER_FLAGS::MH_SPLIT_SEGS))
    error("MH_SPLIT_SEGS not supported");
  if (!canSegmentsSlide(Bin))
    error("the binary is not slideable");

  // Compute total size of all segments. Note that in Mach-O, segments must
  // slide together (see `ImageLoaderMachO::segmentsMustSlideTogether`).
  uint64_t VirtualSize = 0;
  uint64_t FileSize = 0;
  for (SegmentCommand &Seg : Bin.segments()) {
    VirtualSize += Seg.virtual_size();
    FileSize += Seg.file_size();
  }
  if (VirtualSize < FileSize) {
    error("virtual size is less than file size of " + Path);
    return nullptr;
  }

  // Retrieve system's page size.
  SYSTEM_INFO SI;
  GetSystemInfo(&SI);

  // Open the file for memory-mapping.
  HANDLE File =
      CreateFileA(Path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (File == INVALID_HANDLE_VALUE) {
    error("couldn't open file " + Path, /* AppendLastError */ true);
    return nullptr;
  }
  HANDLE FileMapping =
      CreateFileMappingA(File, nullptr, PAGE_WRITECOPY, getHigh(FileSize),
                         getLow(FileSize), nullptr);
  if (!FileMapping) {
    error("couldn't create mapping for " + Path, /* AppendLastError */ true);
    return nullptr;
  }

  // Also create section for virtual-only memory.
  uint64_t RestSize = VirtualSize - FileSize;
  HANDLE RestMapping;
  if (RestSize) {
    RestMapping =
        CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                           getHigh(RestSize), getLow(RestSize), nullptr);
    if (!RestMapping) {
      error("couldn't create virtual-only mapping for " + Path,
            /* AppendLastError */ true);
      return nullptr;
    }
  } else
    RestMapping = nullptr;
  uint64_t RestOffset = 0;

  // Allocate space for the segments.
  void *Ptr = VirtualAlloc2(nullptr, nullptr, VirtualSize,
                            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                            PAGE_NOACCESS, nullptr, 0);
  if (!Ptr) {
    error("couldn't allocate space for " + Path,
          /* AppendLastError */ true);
    return nullptr;
  }
  uint64_t Addr = reinterpret_cast<uint64_t>(Ptr);
  assert((Addr & (SI.dwPageSize - 1)) == 0 &&
         "Allocated memory is not aligned to page size.");

  // Map the segments.
  bool Error = false;
  for (SegmentCommand &Seg : Bin.segments()) {
    void *SuggestedAddr =
        reinterpret_cast<void *>(Addr + Seg.virtual_address());

    // First, release placeholder.
    if (!VirtualFree(SuggestedAddr, Seg.virtual_size(),
                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
      error("couldn't release placeholder for segment " + Seg.name() + " of " +
                Path,
            /* AppendLastError */ true);
      Error = true;
      continue;
    }

    // Then, do memory-mapping.
    if (Seg.file_size()) {
      void *SegAddr = MapViewOfFile3(
          FileMapping, nullptr, SuggestedAddr, Seg.file_offset(),
          Seg.file_size(), MEM_REPLACE_PLACEHOLDER, PAGE_WRITECOPY, nullptr, 0);
      if (!SegAddr) {
        error("couldn't map segment " + Seg.name() + " of " + Path,
              /* AppendLastError */ true);
        Error = true;
        continue;
      }
      assert(SegAddr == SuggestedAddr &&
             "MapViewOfFile3 didn't map to suggested address.");
    }

    // Finally, map the remaining virtual-only memory.
    if (Seg.virtual_size() < Seg.file_size()) {
      error("virtual size is less then file size of segment " + Seg.name() +
            " of " + Path);
      Error = true;
      continue;
    }
    uint64_t Rest = Seg.virtual_size() - Seg.file_size();
    if (Rest) {
      assert(RestSize && RestMapping &&
             "Unexpected virtual-only part of a segment.");
      void *SuggestedRestAddr = reinterpret_cast<void *>(
          Addr + Seg.virtual_address() + Seg.file_size());

      // Map the memory.
      void *RestAddr = MapViewOfFile3(RestMapping, nullptr, SuggestedRestAddr,
                                      RestOffset, Rest, MEM_REPLACE_PLACEHOLDER,
                                      PAGE_READWRITE, nullptr, 0);
      if (!RestAddr) {
        error("couldn't map virtual-only part of segment " + Seg.name() +
                  " of " + Path,
              /* AppendLastError */ true);
        Error = true;
        continue;
      }
      assert(RestAddr == SuggestedRestAddr &&
             "MapViewOfFile3 didn't map to suggested address.");
      RestOffset += Rest;

      // Clear the memory.
      memset(RestAddr, 0, Rest);
    }
  }
  assert((Error || RestOffset == RestSize) &&
         "Not all of virtual-only memory was used.");

#if 0
  uintptr_t Addr = (uintptr_t)_aligned_malloc(Size, PageSize);
  if (!Addr)
    error("couldn't allocate memory for segments");
  uint64_t Slide = Addr - LowAddr;
  LLP->StartAddress = Slide;
  LLP->Size = Size;

  // Load segments. Inspired by `ImageLoaderMachO::mapSegments`.
  for (SegmentCommand &Seg : Bin.segments()) {
    // Convert protection.
    uint32_t VMProt = Seg.init_protection();
    uc_prot Perms = UC_PROT_NONE;
    if (VMProt & (uint32_t)VM_PROTECTIONS::VM_PROT_READ) {
      Perms |= UC_PROT_READ;
    }
    if (VMProt & (uint32_t)VM_PROTECTIONS::VM_PROT_WRITE) {
      Perms |= UC_PROT_WRITE;
    }
    if (VMProt & (uint32_t)VM_PROTECTIONS::VM_PROT_EXECUTE) {
      Perms |= UC_PROT_EXEC;
    }

    uint64_t VAddr = Seg.virtual_address() + Slide;
    // Emulated virtual address is actually equal to the "real" virtual
    // address.
    uint8_t *Mem = reinterpret_cast<uint8_t *>(VAddr);
    uint64_t VSize = Seg.virtual_size();

    if (Perms == UC_PROT_NONE) {
      // No protection means we don't have to copy any data, we just map it.
      mapMemory(VAddr, VSize, Perms);
    } else {
      // TODO: Memory-map the segment instead of copying it.
      auto &Buff = Seg.content();
      // TODO: Copy to the end of the allocated space if flag `SG_HIGHVM` is
      // present.
      memcpy(Mem, Buff.data(), Buff.size());
      mapMemory(VAddr, VSize, Perms);

      // Clear the remaining memory.
      if (Buff.size() < VSize)
        memset(Mem + Buff.size(), 0, VSize - Buff.size());
    }

    // Relocate addresses. Inspired by `ImageLoaderMachOClassic::rebase`.
    if (Slide > 0) {
      for (Relocation &Rel : Seg.relocations()) {
        if (Rel.is_pc_relative() ||
            Rel.origin() != RELOCATION_ORIGINS::ORIGIN_DYLDINFO ||
            Rel.size() != 32 || (Rel.address() & R_SCATTERED) != 0)
          error("unsupported relocation");

        // Find base address for this relocation. Inspired by
        // `ImageLoaderMachOClassic::getRelocBase`.
        uint64_t RelBase = LowAddr + Slide;

        uint64_t RelAddr = RelBase + Rel.address();

        // TODO: Implement what `ImageLoader::containsAddress` does.
        if (RelAddr > VAddr + VSize || RelAddr < VAddr)
          error("relocation target out of range");

        uint32_t *Val = (uint32_t *)RelAddr;
        // We actively leave NULL pointers untouched. Technically it would be
        // correct to slide them because the PAGEZERO segment slid, too. But
        // programs probably wouldn't be happy if their NULLs were non-zero.
        // TODO: Solve this as the original dyld does. Maybe by always mapping
        // PAGEZERO to address 0 or something like that.
        if (*Val != 0)
          *Val = *Val + Slide;
      }
    }
  }

  // Load referenced libraries. See also #22.
  for (DylibCommand &Lib : Bin.libraries())
    load(Lib.name());

  // Bind external symbols.
  for (BindingInfo &BInfo : Bin.dyld_info().bindings()) {
    // Check binding's kind.
    if ((BInfo.binding_class() != BINDING_CLASS::BIND_CLASS_STANDARD &&
         BInfo.binding_class() != BINDING_CLASS::BIND_CLASS_LAZY) ||
        BInfo.binding_type() != BIND_TYPES::BIND_TYPE_POINTER ||
        BInfo.addend()) {
      error("unsupported binding info");
      continue;
    }
    if (!BInfo.has_library()) {
      error("flat-namespace symbols are not supported yet");
      continue;
    }

    // Find symbol's library.
    string LibName(BInfo.library().name());
    LoadedLibrary *Lib = load(LibName);
    if (!Lib) {
      error("symbol's library couldn't be loaded");
      continue;
    }

    // Find symbol's address.
    string SymName(BInfo.symbol().name());
    uint64_t SymAddr = Lib->findSymbol(*this, SymName);
    if (!SymAddr) {
      error("external symbol " + SymName + " from library " + LibName +
            " couldn't be resolved");
      continue;
    }

    // Bind it.
    uint64_t TargetAddr = BInfo.address() + Slide;
    LLP->checkInRange(TargetAddr);
    *reinterpret_cast<uint32_t *>(TargetAddr) = SymAddr;
  }

  return LLP;
#else
  return nullptr;
#endif
}
LoadedLibrary *DynamicLoader::loadPE(const string &Path) {
  using namespace LIEF::PE;

  // Mark the library as found.
  auto LL = make_unique<LoadedDll>();
  LoadedDll *LLP = LL.get();
  LIs[Path] = move(LL);

  // Load it into memory.
  HMODULE Lib = LoadPackagedLibrary(to_hstring(Path).c_str(), 0);
  if (!Lib) {
    error("couldn't load DLL: " + Path, /* AppendLastError */ true);
    LIs.erase(Path);
    return nullptr;
  }
  LLP->Ptr = Lib;

  // Find out where it lies in memory.
  MODULEINFO Info;
  if (!GetModuleInformation(GetCurrentProcess(), Lib, &Info, sizeof(Info))) {
    error("couldn't load module information", /* AppendLastError */ true);
    return nullptr;
  }
  if (uint64_t Hdr = LLP->findSymbol(*this, "_mh_dylib_header")) {
    // Map libraries that act as `.dylib`s without their PE headers.
    LLP->StartAddress = Hdr;
    LLP->Size =
        Info.SizeOfImage - (Hdr - reinterpret_cast<uint64_t>(Info.lpBaseOfDll));
    LLP->MachOPoser = true;
  } else {
    // Map other libraries in their entirety.
    LLP->StartAddress = reinterpret_cast<uint64_t>(Info.lpBaseOfDll);
    LLP->Size = Info.SizeOfImage;
    LLP->MachOPoser = false;
  }

  // Load the library into Unicorn engine.
  uint64_t StartAddr = alignToPageSize(LLP->StartAddress);
  uint64_t Size = roundToPageSize(LLP->Size);
  mapMemory(StartAddr, Size, UC_PROT_READ | UC_PROT_WRITE);

  return LLP;
}
void DynamicLoader::execute(LoadedLibrary *Lib) {
  auto *Dylib = dynamic_cast<LoadedDylib *>(Lib);
  if (!Dylib) {
    error("we can only execute Dylibs right now");
    return;
  }

  // Initialize the stack.
  size_t StackSize = 8 * 1024 * 1024; // 8 MiB
  void *StackPtr = _aligned_malloc(StackSize, PageSize);
  uint64_t StackAddr = reinterpret_cast<uint64_t>(StackPtr);
  mapMemory(StackAddr, StackSize, UC_PROT_READ | UC_PROT_WRITE);
  // Reserve 12 bytes on the stack, so that our instruction logger can read
  // them.
  uint32_t StackTop = StackAddr + StackSize - 12;
  callUC(uc_reg_write(UC, UC_ARM_REG_SP, &StackTop));

  // Install hooks. Hook `catchFetchProtMem` handles calls across platform
  // boundaries (iOS -> Windows). It works thanks to mapping Windows DLLs as
  // non-executable.
  uc_hook Hook;
  callUC(uc_hook_add(UC, &Hook, UC_HOOK_MEM_FETCH_PROT,
                     reinterpret_cast<void *>(catchFetchProtMem), this, 1, 0));
  // Hook `catchCode` logs execution for debugging purposes.
  callUC(uc_hook_add(UC, &Hook, UC_HOOK_CODE,
                     reinterpret_cast<void *>(catchCode), this, 1, 0));
  // Hook `catchMemWrite` logs all memory writes.
  callUC(uc_hook_add(UC, &Hook, UC_HOOK_MEM_WRITE,
                     reinterpret_cast<void *>(catchMemWrite), this, 1, 0));
  // Hook `catchMemUnmapped` allows through reading and writing to unmapped
  // memory (probably heap or other external objects).
  callUC(uc_hook_add(UC, &Hook,
                     UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED,
                     reinterpret_cast<void *>(catchMemUnmapped), this, 1, 0));

  // TODO: Do this also for all non-wrapper Dylibs (i.e., Dylibs that come with
  // the `.ipa` file).
  // TODO: Call also other (user) C++ initializers.
  // TODO: Catch callbacks into the emulated code.
  // Initialize the binary with our Objective-C runtime. This simulates what
  // `dyld_initializer.cpp` does.
  uint64_t Hdr = Dylib->findSymbol(*this, "__mh_execute_header");
  call("libdyld.dll", "_dyld_initialize", reinterpret_cast<void *>(Hdr));
  call("libobjc.dll", "_objc_init");

  // Start at entry point.
  execute(Dylib->Bin.entrypoint() + Dylib->StartAddress);
}
void DynamicLoader::execute(uint64_t Addr) {
  OutputDebugStringA("Info: starting emulation at ");
  dumpAddr(Addr);
  OutputDebugStringA(".\n");

  // Save LR.
  uint32_t LR;
  callUC(uc_reg_read(UC, UC_ARM_REG_LR, &LR));
  LRs.push(LR);

  // Point return address to kernel.
  uint32_t RetAddr = KernelAddr;
  callUC(uc_reg_write(UC, UC_ARM_REG_LR, &RetAddr));

  // Start execution.
  for (;;) {
    Running = true;
    callUC(uc_emu_start(UC, Addr, 0, 0, 0));
    assert(!Running && "Flag `Running` was not updated correctly.");

    if (Continue) {
      Continue = false;
      Continuation();
    }

    if (Restart) {
      // If restarting, continue where we left off.
      Restart = false;
      callUC(uc_reg_read(UC, UC_ARM_REG_LR, &LR));
      Addr = LR;
    } else
      break;
  }
}
void DynamicLoader::returnToKernel() {
  // Restore LR.
  uint32_t LR = LRs.top();
  LRs.pop();
  callUC(uc_reg_write(UC, UC_ARM_REG_LR, &LR));

  // Stop execution.
  callUC(uc_emu_stop(UC));
  Running = false;
}
void DynamicLoader::returnToEmulation() {
  // Move R14 (LR) to R15 (PC) to return.
  uint32_t LR;
  callUC(uc_reg_read(UC, UC_ARM_REG_LR, &LR));

  // Log details about the return.
  OutputDebugStringA("Info: returning to ");
  dumpAddr(LR);
  OutputDebugStringA(".\n");

  assert(!Running);
  Restart = true;
}

void DynamicLoader::DynamicCaller::loadArg(size_t Size) {
  for (size_t I = 0; I != Size; I += 4) {
    if (RegId <= UC_ARM_REG_R3) {
      // We have some registers left, use them.
      uint32_t I32;
      Dyld.callUC(uc_reg_read(Dyld.UC, RegId++, &I32));
      Args.push_back(I32);
    } else {
      // Otherwise, use stack.
      // TODO: Don't read SP every time.
      uint32_t SP;
      Dyld.callUC(uc_reg_read(Dyld.UC, UC_ARM_REG_SP, &SP));
      SP = SP + (Args.size() - 4) * 4;
      Args.push_back(*reinterpret_cast<uint32_t *>(SP));
    }
  }
}

bool DynamicLoader::DynamicCaller::call(bool Returns, uint32_t Addr) {
#define CASE(N)                                                                \
  case N:                                                                      \
    call0<N>(Returns, Addr);                                                   \
    break

  switch (Args.size()) {
    CASE(0);
    CASE(1);
    CASE(2);
    CASE(3);
    CASE(4);
    CASE(5);
    CASE(6);
  default:
    Dyld.error("function has too many arguments");
    return false;
  }
  return true;

#undef CASE
}

size_t DynamicLoader::TypeDecoder::getNextTypeSizeImpl() {
  switch (*T) {
  case 'v': // void
    return 0;
  case 'c': // char
  case '@': // id
  case ':': // SEL
  case 'i': // int
  case 'I': // unsigned int
  case 'f': // float
    return 4;
  case '^': // pointer to type
    ++T;
    getNextTypeSizeImpl(); // Skip the underlying type, it's not important.
    return 4;
  case '{': { // struct
    // Skip name of the struct.
    for (++T; *T != '='; ++T)
      if (!*T) {
        Dyld.error("struct type ended unexpectedly");
        return InvalidSize;
      }
    ++T;

    // Parse type recursively (note that the struct can be also empty).
    size_t TotalSize = 0;
    while (*T != '}') {
      size_t Size = getNextTypeSize();
      if (Size == InvalidSize)
        return InvalidSize;
      TotalSize += Size;
    }

    return TotalSize;
  }
  default:
    Dyld.error("unsupported type encoding");
    return InvalidSize;
  }
}
size_t DynamicLoader::TypeDecoder::getNextTypeSize() {
  size_t Result = getNextTypeSizeImpl();

  // Skip digits.
  for (++T; '0' <= *T && *T <= '9'; ++T)
    ;

  return Result;
}

bool DynamicLoader::catchFetchProtMem(uc_engine *UC, uc_mem_type Type,
                                      uint64_t Addr, int Size, int64_t Value,
                                      void *Data) {
  return reinterpret_cast<DynamicLoader *>(Data)->handleFetchProtMem(
      Type, Addr, Size, Value);
}
bool DynamicLoader::handleFetchProtMem(uc_mem_type Type, uint64_t Addr,
                                       int Size, int64_t Value) {
  // Check that the target address is in some loaded library.
  AddrInfo AI(lookup(Addr));
  if (!AI.Lib) {
    // Handle return to kernel.
    if (Addr == KernelAddr) {
      OutputDebugStringA("Info: executing kernel at 0x");
      OutputDebugStringA(to_hex_string(Addr).c_str());
      OutputDebugStringA(" (as protected).\n");
      returnToKernel();
      return true;
    }

    error("unmapped address fetched");
    return false;
  }

  // If the target is not a wrapper DLL, we must find and call the corresponding
  // wrapper instead.
  bool Wrapper = AI.Lib->IsWrapperDLL;
  if (!Wrapper) {
    filesystem::path WrapperPath(filesystem::path("gen") /
                                 filesystem::path(*AI.LibPath)
                                     .filename()
                                     .replace_extension(".wrapper.dll"));
    LoadedLibrary *WrapperLib = load(WrapperPath.string());
    if (!WrapperLib)
      return false;

    // Load `WrapperIndex`.
    uint64_t IdxAddr = WrapperLib->findSymbol(*this, "?Idx@@3UWrapperIndex@@A");
    auto *Idx = reinterpret_cast<WrapperIndex *>(IdxAddr);

    // TODO: Add real base address instead of hardcoded 0x1000.
    uint64_t RVA = Addr - AI.Lib->StartAddress + 0x1000;

    // Find Dylib with the corresponding wrapper.
    auto Entry = Idx->Map.find(RVA);
    if (Entry == Idx->Map.end()) {
      // If there's no corresponding wrapper, maybe this is a simple Objective-C
      // method and we can translate it dynamically.
      if (const char *T = AI.Lib->getMethodType(Addr)) {
        OutputDebugStringA("Info: dynamically handling method of type ");
        OutputDebugStringA(T);
        OutputDebugStringA(".\n");

        // Handle return value.
        TypeDecoder TD(*this, T);
        bool Returns;
        switch (TD.getNextTypeSize()) {
        case 0:
          Returns = false;
          break;
        case 4:
          Returns = true;
          break;
        default:
          error("unsupported return type");
          return false;
        }

        // Process function arguments.
        // TODO: Use `unique_ptr`.
        shared_ptr<DynamicCaller> DC(new DynamicCaller(*this));
        while (TD.hasNext()) {
          size_t Size = TD.getNextTypeSize();
          if (Size == TypeDecoder::InvalidSize)
            return false;
          DC->loadArg(Size);
        }

        continueOutsideEmulation([=]() {
          // Call the function.
          if (!DC->call(Returns, Addr))
            return;

          returnToEmulation();
        });
        return true;
      }

      error("cannot find RVA 0x" + to_hex_string(RVA) + " in WrapperIndex of " +
            WrapperPath.string());
      return false;
    }
    const string &Dylib = Idx->Dylibs[Entry->second];
    LoadedLibrary *WrapperDylib = load(Dylib);
    if (!WrapperDylib)
      return false;

    // Find the correct wrapper using its alias.
    Addr = WrapperDylib->findSymbol(*this, "$__ipaSim_wraps_" + to_string(RVA));
    if (!Addr) {
      error("cannot find wrapper for 0x" + to_hex_string(RVA) + " in " +
            *AI.LibPath);
      return false;
    }

    AI = lookup(Addr);
    assert(AI.Lib &&
           "Symbol found in library wasn't found there in reverse lookup.");
  }

  // Log details.
  OutputDebugStringA("Info: fetch prot. mem. at ");
  dumpAddr(Addr, AI);
  if (!Wrapper)
    OutputDebugStringA(" (not a wrapper)");
  OutputDebugStringA(".\n");

  // If the target is not a wrapper, we simply jump to it, no need to translate
  // anything.
  if (!Wrapper) {
    uint32_t PC = Addr;
    callUC(uc_reg_write(UC, UC_ARM_REG_PC, &PC));
    return true;
  }

  // Read register R0 containing address of our structure with function
  // arguments and return value.
  uint32_t R0;
  callUC(uc_reg_read(UC, UC_ARM_REG_R0, &R0));

  continueOutsideEmulation([=]() {
    // Call the target function.
    auto *Func = reinterpret_cast<void (*)(uint32_t)>(Addr);
    Func(R0);

    returnToEmulation();
  });
  return true;
}
void DynamicLoader::catchCode(uc_engine *UC, uint64_t Addr, uint32_t Size,
                              void *Data) {
  reinterpret_cast<DynamicLoader *>(Data)->handleCode(Addr, Size);
}
void DynamicLoader::handleCode(uint64_t Addr, uint32_t Size) {
  AddrInfo AI(inspect(Addr));
  if (!AI.Lib) {
    // Handle return to kernel.
    // TODO: This shouldn't happen since kernel is non-executable but it does.
    // It's the same bug as described below.
    if (Addr == KernelAddr) {
      OutputDebugStringA("Info: executing kernel at 0x");
      OutputDebugStringA(to_hex_string(Addr).c_str());
      OutputDebugStringA(".\n");
      returnToKernel();
      return;
    }

    error("unmapped address executed");
    return;
  }

  // There is a bug that sometimes protected memory accesses are not caught by
  // Unicorn Engine.
  // TODO: Fix that bug, maybe.
  // See also <https://github.com/unicorn-engine/unicorn/issues/888>.
  if (!dynamic_cast<LoadedDylib *>(load(*AI.LibPath))) {
    // TODO: Stop execution if this returns false.
    handleFetchProtMem(UC_MEM_FETCH_PROT, Addr, Size, 0);
    return;
  }

#if 0
  OutputDebugStringA("Info: executing at ");
  dumpAddr(Addr, AI);
  OutputDebugStringA(" [R0 = 0x");
  uint32_t Reg;
  callUC(uc_reg_read(UC, UC_ARM_REG_R0, &Reg));
  OutputDebugStringA(to_hex_string(Reg).c_str());
  OutputDebugStringA(", R1 = 0x");
  callUC(uc_reg_read(UC, UC_ARM_REG_R1, &Reg));
  OutputDebugStringA(to_hex_string(Reg).c_str());
  OutputDebugStringA(", R7 = 0x");
  callUC(uc_reg_read(UC, UC_ARM_REG_R7, &Reg));
  OutputDebugStringA(to_hex_string(Reg).c_str());
  OutputDebugStringA(", R12 = 0x");
  callUC(uc_reg_read(UC, UC_ARM_REG_R12, &Reg));
  OutputDebugStringA(to_hex_string(Reg).c_str());
  OutputDebugStringA(", R13 = 0x");
  callUC(uc_reg_read(UC, UC_ARM_REG_R13, &Reg));
  OutputDebugStringA(to_hex_string(Reg).c_str());
  OutputDebugStringA(", [R13] = 0x");
  uint32_t Word;
  callUC(uc_mem_read(UC, Reg, &Word, 4));
  OutputDebugStringA(to_hex_string(Word).c_str());
  OutputDebugStringA(", [R13+4] = 0x");
  callUC(uc_mem_read(UC, Reg + 4, &Word, 4));
  OutputDebugStringA(to_hex_string(Word).c_str());
  OutputDebugStringA(", [R13+8] = 0x");
  callUC(uc_mem_read(UC, Reg + 4, &Word, 4));
  OutputDebugStringA(to_hex_string(Word).c_str());
  OutputDebugStringA(", R14 = 0x");
  callUC(uc_reg_read(UC, UC_ARM_REG_R14, &Reg));
  OutputDebugStringA(to_hex_string(Reg).c_str());
  OutputDebugStringA("].\n");
#endif
}
bool DynamicLoader::catchMemWrite(uc_engine *UC, uc_mem_type Type,
                                  uint64_t Addr, int Size, int64_t Value,
                                  void *Data) {
  return reinterpret_cast<DynamicLoader *>(Data)->handleMemWrite(Type, Addr,
                                                                 Size, Value);
}
bool DynamicLoader::handleMemWrite(uc_mem_type Type, uint64_t Addr, int Size,
                                   int64_t Value) {
#if 0
  OutputDebugStringA("Info: writing [");
  dumpAddr(Addr);
  OutputDebugStringA("] := ");
  dumpAddr(Value);
  OutputDebugStringA(" (");
  OutputDebugStringA(to_string(Size).c_str());
  OutputDebugStringA(").\n");
#endif
  return true;
}
bool DynamicLoader::catchMemUnmapped(uc_engine *UC, uc_mem_type Type,
                                     uint64_t Addr, int Size, int64_t Value,
                                     void *Data) {
  return reinterpret_cast<DynamicLoader *>(Data)->handleMemUnmapped(
      Type, Addr, Size, Value);
}
// TODO: Maybe this happens when the emulated app accesses some non-directly
// dependent DLL and we should load it as a whole.
bool DynamicLoader::handleMemUnmapped(uc_mem_type Type, uint64_t Addr, int Size,
                                      int64_t Value) {
  OutputDebugStringA("Info: unmapped memory manipulation at ");
  dumpAddr(Addr);
  OutputDebugStringA(" (");
  OutputDebugStringA(to_string(Size).c_str());
  OutputDebugStringA(").\n");

  // Map the memory, so that emulation can continue.
  Addr = alignToPageSize(Addr);
  Size = roundToPageSize(Size);
  mapMemory(Addr, Size, UC_PROT_READ | UC_PROT_WRITE);

  return true;
}
AddrInfo DynamicLoader::lookup(uint64_t Addr) {
  for (auto &LI : LIs) {
    LoadedLibrary *LL = LI.second.get();
    if (LL->isInRange(Addr))
      return {&LI.first, LL, string()};
  }
  return {nullptr, nullptr, string()};
}
// TODO: Find symbol name and also use this function to implement
// `src/objc/dladdr.mm`.
AddrInfo DynamicLoader::inspect(uint64_t Addr) { return lookup(Addr); }
// Calling `uc_emu_start` inside `emu_start` (e.g., inside a hook) is not very
// good idea. Instead, we need to call it when emulation completely stops (i.e.,
// Unicorn returns from `uc_emu_start`). That's what this function is used for.
// All code that calls or could call `uc_emu_start` should be deferred using
// this function. See also
// <https://github.com/unicorn-engine/unicorn/issues/591>.
void DynamicLoader::continueOutsideEmulation(function<void()> Cont) {
  assert(!Continue && "Only one continuation is supported.");
  Continue = true;
  Continuation = Cont;

  callUC(uc_emu_stop(UC));
  Running = false;
}

struct IpaSimulator {
  IpaSimulator() : UC(initUC()), Dyld(UC) {}
  ~IpaSimulator() { callUCSimple(uc_close(UC)); }

  uc_engine *UC;
  DynamicLoader Dyld;
  string MainBinary;

private:
  uc_engine *initUC() {
    callUCSimple(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &UC));
    return UC;
  }
};

static IpaSimulator IpaSim;

extern "C" __declspec(dllexport) void start(
    const hstring &Path, const LaunchActivatedEventArgs &LaunchArgs) {
  // Load the binary.
  IpaSim.MainBinary = to_string(Path);
  LoadedLibrary *App = IpaSim.Dyld.load(IpaSim.MainBinary);
  if (!App)
    return;

  // Execute it.
  IpaSim.Dyld.execute(App);

  // Call `UIApplicationLaunched`.
  LoadedLibrary *UIKit = IpaSim.Dyld.load("UIKit.dll");
  uint64_t LaunchAddr = UIKit->findSymbol(IpaSim.Dyld, "UIApplicationLaunched");
  auto *LaunchFunc = reinterpret_cast<void (*)(void *)>(LaunchAddr);
  // `get_abi` converts C++/WinRT object to its C++/CX equivalent.
  LaunchFunc(get_abi(LaunchArgs));
}

struct method_t {
  const char *name;
  const char *types;
  void *imp;
};

struct method_list_t {
  uint32_t entrysize;
  uint32_t count;
  method_t methods[0];
};

#define FAST_DATA_MASK 0xfffffffcUL
#define RW_REALIZED (1 << 31)

struct class_ro_t {
  uint32_t flags;
  uint32_t instanceStart;
  uint32_t instanceSize;

  const uint8_t *ivarLayout;

  const char *name;
  method_list_t *baseMethodList;
  void *baseProtocols;
  const void *ivars;

  const uint8_t *weakIvarLayout;
  void *baseProperties;
};

template <typename Element, typename List> class list_array_tt {
  struct array_t {
    uint32_t count;
    List *lists[0];
  };

private:
  union {
    List *list;
    uintptr_t arrayAndFlag;
  };

  bool hasArray() const { return arrayAndFlag & 1; }
  array_t *array() { return (array_t *)(arrayAndFlag & ~1); }

public:
  List **beginLists() {
    if (hasArray()) {
      return array()->lists;
    } else {
      return &list;
    }
  }

  List **endLists() {
    if (hasArray()) {
      return array()->lists + array()->count;
    } else if (list) {
      return &list + 1;
    } else {
      return &list;
    }
  }
};

using method_array_t = list_array_tt<method_t, method_list_t>;

struct class_rw_t {
  uint32_t flags;
  uint32_t version;

  const class_ro_t *ro;

  method_array_t methods;
  /*
  property_array_t properties;
  protocol_array_t protocols;

  Class firstSubclass;
  Class nextSiblingClass;

  char *demangledName;
  */
};

struct objc_class {
  objc_class *isa;
  void *superclass;
  void *cache;
  void *vtable;
  class_ro_t *info;

  class_rw_t *data() {
    return (class_rw_t *)((uintptr_t)info & FAST_DATA_MASK);
  }
  bool isRealized() { return data()->flags & RW_REALIZED; }
  const class_ro_t *getInfo() { return isRealized() ? data()->ro : info; }
};

struct category_t {
  const char *name;
  objc_class *cls;
  method_list_t *instanceMethods;
  method_list_t *classMethods;

  /*
  struct protocol_list_t *protocols;
  struct property_list_t *instanceProperties;
  // Fields below this point are not always present on disk.
  struct property_list_t *_classProperties;
  */
};

static const char *findMethod(method_list_t *Methods, uint64_t Addr) {
  if (!Methods)
    return nullptr;
  for (size_t J = 0; J != Methods->count; ++J) {
    method_t &Method = Methods->methods[J];
    if (reinterpret_cast<uint64_t>(Method.imp) == Addr)
      return Method.types;
  }
  return nullptr;
}
static const char *findMethod(objc_class *Class, uint64_t Addr) {
  // TODO: Isn't this first part redundant for realized classes?
  if (const char *T = findMethod(Class->getInfo()->baseMethodList, Addr))
    return T;
  if (Class->isRealized())
    for (auto *L = Class->data()->methods.beginLists(),
              *End = Class->data()->methods.endLists();
         L != End; ++L)
      if (const char *T = findMethod(*L, Addr))
        return T;
  return nullptr;
}
const char *LoadedLibrary::getClassOfMethod(uint64_t Addr) {
  // Enumerate classes in the image.
  uint64_t SecSize;
  uint64_t SecAddr = getSection("__objc_classlist", &SecSize);
  if (SecAddr) {
    auto *Classes = reinterpret_cast<objc_class **>(SecAddr);
    for (size_t I = 0, Count = SecSize / sizeof(void *); I != Count; ++I) {
      // Enumerate methods of every class and its meta-class.
      objc_class *Class = Classes[I];
      if (const char *T = findMethod(Class, Addr))
        return Class->getInfo()->name;
      if (const char *T = findMethod(Class->isa, Addr))
        return Class->isa->getInfo()->name;
    }
  }

  // Try also non-lazy classes.
  SecAddr = getSection("__objc_nlclslist", &SecSize);
  if (SecAddr) {
    auto *Classes = reinterpret_cast<objc_class **>(SecAddr);
    for (size_t I = 0, Count = SecSize / sizeof(void *); I != Count; ++I) {
      // Enumerate methods of every class and its meta-class.
      objc_class *Class = Classes[I];
      if (const char *T = findMethod(Class, Addr))
        return Class->getInfo()->name;
      if (const char *T = findMethod(Class->isa, Addr))
        return Class->isa->getInfo()->name;
    }
  }

  return nullptr;
}
const char *LoadedLibrary::getMethodType(uint64_t Addr) {
  // Enumerate classes in the image.
  uint64_t SecSize;
  uint64_t SecAddr = getSection("__objc_classlist", &SecSize);
  if (SecAddr) {
    auto *Classes = reinterpret_cast<objc_class **>(SecAddr);
    for (size_t I = 0, Count = SecSize / sizeof(void *); I != Count; ++I) {
      // Enumerate methods of every class and its meta-class.
      objc_class *Class = Classes[I];
      if (const char *T = findMethod(Class, Addr))
        return T;
      if (const char *T = findMethod(Class->isa, Addr))
        return T;
    }
  }

  // Try also categories.
  SecAddr = getSection("__objc_catlist", &SecSize);
  if (SecAddr) {
    auto *Categories = reinterpret_cast<category_t **>(SecAddr);
    for (size_t I = 0, Count = SecSize / sizeof(void *); I != Count; ++I) {
      // Enumerate methods of every category.
      category_t *Category = Categories[I];
      if (const char *T = findMethod(Category->classMethods, Addr))
        return T;
      if (const char *T = findMethod(Category->instanceMethods, Addr))
        return T;
    }
  }

  return nullptr;
}

void DynamicLoader::dumpAddr(uint64_t Addr, const AddrInfo &AI) {
  if (!AI.Lib) {
    OutputDebugStringA("0x");
    OutputDebugStringA(to_hex_string(Addr).c_str());
  } else {
    OutputDebugStringA(AI.LibPath->c_str());
    OutputDebugStringA("+0x");
    uint64_t RVA = Addr - AI.Lib->StartAddress;
    OutputDebugStringA(to_hex_string(RVA).c_str());
  }
}
void DynamicLoader::dumpAddr(uint64_t Addr) {
  if (Addr == KernelAddr) {
    OutputDebugStringA("kernel!0x");
    OutputDebugStringA(to_hex_string(Addr).c_str());
  } else {
    AddrInfo AI(lookup(Addr));
    dumpAddr(Addr, AI);
  }
}

struct Trampoline {
  ffi_cif CIF;
  bool Returns;
  size_t ArgC;
  uint64_t Addr;
};
void DynamicLoader::handleTrampoline(void *Ret, void **Args, void *Data) {
  auto *Tr = reinterpret_cast<Trampoline *>(Data);
  OutputDebugStringA("Info: handling trampoline (arguments: ");
  OutputDebugStringA(to_string(Tr->ArgC).c_str());
  if (Tr->Returns)
    OutputDebugStringA(", returns).\n");
  else
    OutputDebugStringA(", void).\n");

  // Pass arguments.
  int RegId = UC_ARM_REG_R0;
  for (size_t I = 0, ArgC = Tr->ArgC; I != ArgC; ++I) {
    uint32_t I32 = *reinterpret_cast<uint32_t *>(Args[I]);
    callUC(uc_reg_write(UC, RegId++, &I32));
  }

  // Call the function.
  execute(Tr->Addr);

  // Extract return value.
  if (Tr->Returns) {
    uint32_t Reg;
    callUC(uc_reg_read(UC, UC_ARM_REG_R0, &Reg));
    *reinterpret_cast<ffi_arg *>(Ret) = Reg;
  }
}
static void ipaSim_handleTrampoline(ffi_cif *, void *Ret, void **Args,
                                    void *Data) {
  IpaSim.Dyld.handleTrampoline(Ret, Args, Data);
}

// If `Addr` points to emulated code, returns address of wrapper that should be
// called instead. Otherwise, returns `Addr` unchanged.
void *DynamicLoader::translate(void *Addr) {
  uint64_t AddrVal = reinterpret_cast<uint64_t>(Addr);
  AddrInfo AI(lookup(AddrVal));
  if (auto *Dylib = dynamic_cast<LoadedDylib *>(AI.Lib)) {
    // The address points to Dylib.

    if (const char *T = Dylib->getMethodType(AddrVal)) {
      // We have found metadata of the callback method. Now, for simple methods,
      // it's actually quite simple to translate i386 -> ARM calls dynamically,
      // so that's what we do here.
      // TODO: Generate wrappers for callbacks, too (see README of
      // `HeadersAnalyzer` for more details).
      OutputDebugStringA("Info: dynamically handling callback of type ");
      OutputDebugStringA(T);
      OutputDebugStringA(".\n");

      // First, handle the return value.
      TypeDecoder TD(*this, T);
      auto *Tr = new Trampoline;
      switch (TD.getNextTypeSize()) {
      case 0:
        Tr->Returns = false;
        break;
      case 4:
        Tr->Returns = true;
        break;
      default:
        error("unsupported return type of callback");
        return nullptr;
      }

      // Next, process function arguments.
      Tr->ArgC = 0;
      while (TD.hasNext()) {
        switch (TD.getNextTypeSize()) {
        case TypeDecoder::InvalidSize:
          return nullptr;
        case 4: {
          if (Tr->ArgC > 3) {
            error("callback has too many arguments");
            return nullptr;
          }
          ++Tr->ArgC;
          break;
        }
        default:
          error("unsupported callback argument type");
          return nullptr;
        }
      }

      // Now, create trampoline.
      Tr->Addr = AddrVal;
      void *Ptr;
      // TODO: `Closure` nor `Tr` are never deallocated.
      auto *Closure = reinterpret_cast<ffi_closure *>(
          ffi_closure_alloc(sizeof(ffi_closure), &Ptr));
      if (!Closure) {
        error("couldn't allocate closure");
        return nullptr;
      }
      static ffi_type *ArgTypes[4] = {&ffi_type_uint32, &ffi_type_uint32,
                                      &ffi_type_uint32, &ffi_type_uint32};
      if (ffi_prep_cif(&Tr->CIF, FFI_MS_CDECL, Tr->ArgC,
                       Tr->Returns ? &ffi_type_uint32 : &ffi_type_void,
                       ArgTypes) != FFI_OK) {
        error("couldn't prepare CIF");
        return nullptr;
      }
      if (ffi_prep_closure_loc(Closure, &Tr->CIF, ipaSim_handleTrampoline, Tr,
                               Ptr) != FFI_OK) {
        error("couldn't prepare closure");
        return nullptr;
      }
      return Ptr;
    }

    error("callback not found");
    return nullptr;
  }

  return Addr;
}

void DynamicLoader::callLoad(void *load, void *self, void *sel) {
  uint64_t Addr = reinterpret_cast<uint64_t>(load);
  AddrInfo AI(lookup(Addr));
  if (!dynamic_cast<LoadedDylib *>(AI.Lib)) {
    // Target load method is not inside any emulated Dylib, so it must be native
    // executable code and we can simply call it.
    reinterpret_cast<void (*)(void *, void *)>(load)(self, sel);
  } else {
    // Target load method is inside some emulated library.

    // HACK: Don't load `__ARCLite__` as it's buggy.
    if (string("__ARCLite__") == AI.Lib->getClassOfMethod(Addr))
      return;

    // Pass arguments.
    uint32_t I32 = reinterpret_cast<uint32_t>(self);
    callUC(uc_reg_write(UC, UC_ARM_REG_R0, &I32));
    I32 = reinterpret_cast<uint32_t>(sel);
    callUC(uc_reg_write(UC, UC_ARM_REG_R1, &I32));

    // And call it.
    execute(Addr);
  }
}

extern "C" __declspec(dllexport) void *ipaSim_translate(void *Addr) {
  return IpaSim.Dyld.translate(Addr);
}
extern "C" __declspec(dllexport) void ipaSim_translate4(uint32_t *Addr) {
  Addr[1] = reinterpret_cast<uint32_t>(
      IpaSim.Dyld.translate(reinterpret_cast<void *>(Addr[1])));
}
extern "C" __declspec(dllexport) const char *ipaSim_processPath() {
  return IpaSim.MainBinary.c_str();
}
extern "C" __declspec(dllexport) void ipaSim_callLoad(void *load, void *self,
                                                      void *sel) {
  IpaSim.Dyld.callLoad(load, self, sel);
}
extern "C" __declspec(dllexport) void ipaSim_callBack1(void *FP, void *arg0) {
  IpaSim.Dyld.callBack(FP, arg0);
}
extern "C" __declspec(dllexport) void ipaSim_callBack2(void *FP, void *arg0,
                                                       void *arg1) {
  IpaSim.Dyld.callBack(FP, arg0, arg1);
}
extern "C" __declspec(dllexport) void *ipaSim_callBack1r(void *FP, void *arg0) {
  return IpaSim.Dyld.callBackR(FP, arg0);
}
extern "C" __declspec(dllexport) void *ipaSim_callBack3r(void *FP, void *arg0,
                                                         void *arg1,
                                                         void *arg2) {
  return IpaSim.Dyld.callBackR(FP, arg0, arg1, arg2);
}
