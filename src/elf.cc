// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <string>
#include <iostream>
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "re2/re2.h"
#include "third_party/freebsd_elf/elf.h"
#include "bloaty.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>

using absl::string_view;

ABSL_ATTRIBUTE_NORETURN
static void Throw(const char *str, int line) {
  throw bloaty::Error(str, __FILE__, line);
}

#define THROW(msg) Throw(msg, __LINE__)
#define THROWF(...) Throw(absl::Substitute(__VA_ARGS__).c_str(), __LINE__)
#define WARN(x) fprintf(stderr, "bloaty: %s\n", x);

namespace {

size_t CheckedAdd(size_t a, size_t b) {
  if (SIZE_MAX - b < a) {
    THROW("integer overflow");
  }
  return a + b;
}

}

namespace bloaty {

namespace {

struct ByteSwapFunc {
  template <class T>
  T operator()(T val) {
    return ByteSwap(val);
  }
};

struct NullFunc {
  template <class T>
  T operator()(T val) { return val; }
};

size_t StringViewToSize(string_view str) {
  size_t ret;
  if (!absl::SimpleAtoi(str, &ret)) {
    THROWF("couldn't convert string '$0' to integer.", str);
  }
  return ret;
}

// ElfFile /////////////////////////////////////////////////////////////////////

// For parsing the pieces we need out of an ELF file (.o, .so, and binaries).

class ElfFile {
 public:
  ElfFile(string_view data) : data_(data) {
    ok_ = Initialize();
  }

  bool IsOpen() { return ok_; }

  // Regions of the file where different headers live.
  string_view entire_file() const { return data_; }
  string_view header_region() const { return header_region_; }
  string_view section_headers() const { return section_headers_; }
  string_view segment_headers() const { return segment_headers_; }

  const Elf64_Ehdr& header() const { return header_; }
  Elf64_Xword section_count() const { return section_count_; }
  Elf64_Xword section_string_index() const { return section_string_index_; }

  // Represents an ELF segment (data used by the loader / dynamic linker).
  class Segment {
   public:
    const Elf64_Phdr& header() const { return header_; }
    string_view contents() const { return contents_; }

   private:
    friend class ElfFile;
    Elf64_Phdr header_;
    string_view contents_;
  };

  // Represents an ELF section (.text, .data, .bss, etc.)
  class Section {
   public:
    const Elf64_Shdr& header() const { return header_; }
    string_view contents() const { return contents_; }

    // If header().sh_type == SHT_STRTAB.
    string_view ReadName(Elf64_Word index) const;

    // If header().sh_type == SHT_SYMTAB
    Elf64_Word GetSymbolCount() const;
    void ReadSymbol(Elf64_Word index, Elf64_Sym* sym) const;

   private:
    friend class ElfFile;
    const ElfFile* elf_;
    Elf64_Shdr header_;
    string_view contents_;
  };

  bool ReadSegment(Elf64_Word index, Segment* segment) const;
  bool ReadSection(Elf64_Word index, Section* section) const;

  bool is_64bit() const { return is_64bit_; }
  bool is_native_endian() const { return is_native_endian_; }

 private:
  friend class Section;

  bool Initialize();

  string_view GetRegion(size_t start, size_t n) const {
    if (SIZE_MAX - n <= start || start + n > data_.size()) {
      THROW("ELF region out-of-bounds");
    }
    return data_.substr(start, n);
  }

  // Shared code for reading various ELF structures.  Handles endianness
  // conversion and 32->64 bit conversion, when necessary.
  class StructReader {
   public:
    StructReader(const ElfFile& elf, string_view data)
        : elf_(elf), data_(data) {}

    template <class T32, class T64, class Munger>
    void Read(size_t offset, Munger /*munger*/, T64* out) const {
      if (elf_.is_64bit() && elf_.is_native_endian()) {
        return Memcpy(offset, out);
      } else {
        return ReadFallback<T32, T64, Munger>(offset, out);
      }
    }

   private:
    const ElfFile& elf_;
    string_view data_;

    template <class T32, class T64, class Munger>
    void ReadFallback(size_t offset, T64* out) const;

    template <class T>
    void Memcpy(size_t offset, T* out) const {
      size_t end = CheckedAdd(offset, sizeof(T));
      if (end > data_.size()) {
        THROW("can't memcpy that data from ELF file");
      }
      memcpy(out, data_.data() + offset, sizeof(*out));
    }
  };

  template <class T32, class T64, class Munger>
  void ReadStruct(size_t offset, Munger munger, T64* out) const {
    StructReader(*this, data_).Read<T32>(offset, munger, out);
  }

  bool ok_;
  bool is_64bit_;
  bool is_native_endian_;
  string_view data_;
  Elf64_Ehdr header_;
  Elf64_Xword section_count_;
  Elf64_Xword section_string_index_;
  string_view header_region_;
  string_view section_headers_;
  string_view segment_headers_;
};

// ELF uses different structure definitions for 32/64 bit files.  The sizes of
// members are different, and members are even in a different order!
//
// These mungers can convert 32 bit structures to 64-bit ones.  They can also
// handle converting endianness.  We use templates so a single template function
// can handle all three patterns:
//
//   32 native  -> 64 native
//   32 swapped -> 64 native
//   64 swapped -> 64 native

struct EhdrMunger {
  template <class From, class Func>
  void operator()(const From& from, Elf64_Ehdr* to, Func func) {
    memcpy(&to->e_ident[0], &from.e_ident[0], EI_NIDENT);
    to->e_type       = func(from.e_type);
    to->e_machine    = func(from.e_machine);
    to->e_version    = func(from.e_version);
    to->e_entry      = func(from.e_entry);
    to->e_phoff      = func(from.e_phoff);
    to->e_shoff      = func(from.e_shoff);
    to->e_flags      = func(from.e_flags);
    to->e_ehsize     = func(from.e_ehsize);
    to->e_phentsize  = func(from.e_phentsize);
    to->e_phnum      = func(from.e_phnum);
    to->e_shentsize  = func(from.e_shentsize);
    to->e_shnum      = func(from.e_shnum);
    to->e_shstrndx   = func(from.e_shstrndx);
  }
};

struct ShdrMunger {
  template <class From, class Func>
  void operator()(const From& from, Elf64_Shdr* to, Func func) {
    to->sh_name       = func(from.sh_name);
    to->sh_type       = func(from.sh_type);
    to->sh_flags      = func(from.sh_flags);
    to->sh_addr       = func(from.sh_addr);
    to->sh_offset     = func(from.sh_offset);
    to->sh_size       = func(from.sh_size);
    to->sh_link       = func(from.sh_link);
    to->sh_info       = func(from.sh_info);
    to->sh_addralign  = func(from.sh_addralign);
    to->sh_entsize    = func(from.sh_entsize);
  }
};

struct PhdrMunger {
  template <class From, class Func>
  void operator()(const From& from, Elf64_Phdr* to, Func func) {
    to->p_type   = func(from.p_type);
    to->p_flags  = func(from.p_flags);
    to->p_offset = func(from.p_offset);
    to->p_vaddr  = func(from.p_vaddr);
    to->p_paddr  = func(from.p_paddr);
    to->p_filesz = func(from.p_filesz);
    to->p_memsz  = func(from.p_memsz);
    to->p_align  = func(from.p_align);
  }
};

struct SymMunger {
  template <class From, class Func>
  void operator()(const From& from, Elf64_Sym* to, Func func) {
    to->st_name   = func(from.st_name);
    to->st_info   = func(from.st_info);
    to->st_other  = func(from.st_other);
    to->st_shndx  = func(from.st_shndx);
    to->st_value  = func(from.st_value);
    to->st_size   = func(from.st_size);
  }
};

template <class T32, class T64, class Munger>
void ElfFile::StructReader::ReadFallback(size_t offset, T64* out) const {
  if (elf_.is_64bit()) {
    assert(!elf_.is_native_endian());
    Memcpy(offset, out);
    Munger()(*out, out, ByteSwapFunc());
  } else {
    T32 data32;
    Memcpy(offset, &data32);
    if (elf_.is_native_endian()) {
      Munger()(data32, out, NullFunc());
    } else {
      Munger()(data32, out, ByteSwapFunc());
    }
  }
}

string_view ElfFile::Section::ReadName(Elf64_Word index) const {
  assert(header().sh_type == SHT_STRTAB);

  if (index == SHN_UNDEF || index >= contents_.size()) {
    THROWF("can't read index $0 from strtab, total size is $1", index,
           contents_.size());
  }

  string_view ret = contents_.substr(index);

  const char* null_pos =
      static_cast<const char*>(memchr(ret.data(), '\0', ret.size()));

  if (null_pos == NULL) {
    THROW("no NULL terminator found");
  }

  size_t len = null_pos - ret.data();
  ret = ret.substr(0, len);
  return ret;
}

Elf64_Word ElfFile::Section::GetSymbolCount() const {
  assert(header().sh_type == SHT_SYMTAB);
  if (header_.sh_entsize == 0) {
    THROW("sh_entsize is zero");
  }
  return contents_.size() / header_.sh_entsize;
}

void ElfFile::Section::ReadSymbol(Elf64_Word index, Elf64_Sym* sym) const {
  assert(header().sh_type == SHT_SYMTAB);
  ElfFile::StructReader reader(*elf_, contents());
  reader.Read<Elf32_Sym>(header_.sh_entsize * index, SymMunger(), sym);
}

bool ElfFile::Initialize() {
  if (data_.size() < EI_NIDENT) {
    return false;
  }

  unsigned char ident[EI_NIDENT];
  memcpy(ident, data_.data(), EI_NIDENT);

  if (memcmp(ident, "\177ELF", 4) != 0) {
    // Not an ELF file.
    return false;
  }

  switch (ident[EI_CLASS]) {
    case ELFCLASS32:
      is_64bit_ = false;
      break;
    case ELFCLASS64:
      is_64bit_ = true;
      break;
    default:
      THROWF("unexpected ELF class: $0", ident[EI_CLASS]);
  }

  switch (ident[EI_DATA]) {
    case ELFDATA2LSB:
      is_native_endian_ = IsLittleEndian();
      break;
    case ELFDATA2MSB:
      is_native_endian_ = !IsLittleEndian();
      break;
    default:
      THROWF("unexpected ELF data: $0", ident[EI_DATA]);
  }

  ReadStruct<Elf32_Ehdr>(0, EhdrMunger(), &header_);

  Section section0;
  bool has_section0 = 0;

  // ELF extensions: if certain fields overflow, we have to find their true data
  // from elsewhere.  For more info see:
  // https://docs.oracle.com/cd/E19683-01/817-3677/chapter6-94076/index.html
  if (header_.e_shoff > 0 &&
      data_.size() > (header_.e_shoff + header_.e_shentsize)) {
    section_count_ = 1;
    ReadSection(0, &section0);
    has_section0 = true;
  }

  section_count_ = header_.e_shnum;
  section_string_index_ = header_.e_shstrndx;

  if (section_count_ == 0 && has_section0) {
    section_count_ = section0.header().sh_size;
  }

  if (section_string_index_ == SHN_XINDEX && has_section0) {
    section_string_index_ = section0.header().sh_link;
  }

  header_region_ = GetRegion(0, header_.e_ehsize);
  section_headers_ =
      GetRegion(header_.e_shoff, header_.e_shentsize * section_count_);
  segment_headers_ =
      GetRegion(header_.e_phoff, header_.e_phentsize * header_.e_phnum);

  return true;
}

bool ElfFile::ReadSegment(Elf64_Word index, Segment* segment) const {
  if (index >= header_.e_phnum) {
    THROWF("segment $0 doesn't exist, only $1 segments", index,
           header_.e_phnum);
  }

  Elf64_Phdr* header = &segment->header_;
  ReadStruct<Elf32_Phdr>(header_.e_phoff + header_.e_phentsize * index,
                         PhdrMunger(), header);
  segment->contents_ = GetRegion(header->p_offset, header->p_filesz);
  return true;
}

bool ElfFile::ReadSection(Elf64_Word index, Section* section) const {
  if (index >= section_count_) {
    THROWF("tried to read section $0, but there are only $1", index,
           section_count_);
  }

  Elf64_Shdr* header = &section->header_;
  ReadStruct<Elf32_Shdr>(header_.e_shoff + header_.e_shentsize * index,
                         ShdrMunger(), header);

  if (header->sh_type == SHT_NOBITS) {
    section->contents_ = string_view();
  } else {
    section->contents_ = GetRegion(header->sh_offset, header->sh_size);
  }

  section->elf_ = this;
  return true;
}


// ArFile //////////////////////////////////////////////////////////////////////

// For parsing .a files (static libraries).
//
// The best documentation I've been able to find for this file format is
// Wikipedia: https://en.wikipedia.org/wiki/Ar_(Unix)
//
// So far we only parse the System V / GNU variant.

class ArFile {
 public:
  ArFile(string_view data)
      : magic_(data.substr(0, kMagicSize)),
        contents_(data.substr(std::min<size_t>(data.size(), kMagicSize))) {}

  bool IsOpen() const { return magic() == string_view(kMagic); }

  string_view magic() const { return magic_; }
  string_view contents() const { return contents_; }

  struct MemberFile {
    enum {
      kSymbolTable,        // Stores a symbol table.
      kLongFilenameTable,  // Stores long filenames, users should ignore.
      kNormal,             // Regular data file.
    } file_type;
    string_view filename;  // Only when file_type == kNormal
    size_t size;
    string_view header;
    string_view contents;
  };

  class MemberReader {
   public:
    MemberReader(const ArFile& ar) : remaining_(ar.contents()) {}
    bool ReadMember(MemberFile* file);
    bool IsEof() const { return remaining_.size() == 0; }

   private:
    string_view Consume(size_t n) {
      if (remaining_.size() < n) {
        THROW("premature end of file");
      }
      string_view ret = remaining_.substr(0, n);
      remaining_.remove_prefix(n);
      return ret;
    }

    string_view long_filenames_;
    string_view remaining_;
  };

 private:
  const string_view magic_;
  const string_view contents_;

  static constexpr const char* kMagic = "!<arch>\n";
  static constexpr int kMagicSize = 8;
};

bool ArFile::MemberReader::ReadMember(MemberFile* file) {
  struct Header {
    char file_id[16];
    char modified_timestamp[12];
    char owner_id[6];
    char group_id[6];
    char mode[8];
    char size[10];
    char end[2];
  };

  if (remaining_.size() < sizeof(Header)) {
    return false;
  }

  const Header* header = reinterpret_cast<const Header*>(remaining_.data());
  file->header = Consume(sizeof(Header));

  string_view file_id(&header->file_id[0], sizeof(header->file_id));
  string_view size_str(&header->size[0], sizeof(header->size));
  file->size = StringViewToSize(size_str);
  file->contents = Consume(file->size);
  file->file_type = MemberFile::kNormal;

  if (file_id[0] == '/') {
    // Special filename, internal to the format.
    if (file_id[1] == ' ') {
      file->file_type = MemberFile::kSymbolTable;
    } else if (file_id[1] == '/') {
      file->file_type = MemberFile::kLongFilenameTable;
      long_filenames_ = file->contents;
    } else if (isdigit(file_id[1])) {
      size_t offset = StringViewToSize(file_id.substr(1));
      size_t end = long_filenames_.find('/', offset);

      if (end == std::string::npos) {
        return false;
      }

      file->filename = long_filenames_.substr(offset, end - offset);
    } else {
      return false;  // Unexpected special filename.
    }
  } else {
    // Normal filename, slash-terminated.
    size_t slash = file_id.find('/');

    if (slash == std::string::npos) {
      fprintf(stderr, "BSD-style AR not yet implemented.\n");
      return false;
    }

    file->filename = file_id.substr(0, slash);
  }

  return true;
}

void MaybeAddFileRange(RangeSink* sink, string_view label, string_view range) {
  if (sink) {
    sink->AddFileRange(label, range);
  }
}

template <class Func>
void OnElfFile(const ElfFile& elf, string_view filename,
               unsigned long index_base, RangeSink* sink, Func func) {
  func(elf, filename, index_base);

  // Add these *after* running the user callback.  That way if there is
  // overlap, the user's annotations will take precedence.
  MaybeAddFileRange(sink, "[ELF Headers]", elf.header_region());
  MaybeAddFileRange(sink, "[ELF Headers]", elf.section_headers());
  MaybeAddFileRange(sink, "[ELF Headers]", elf.segment_headers());

  // Any sections of the file not covered by any segments/sections/symbols/etc.
  MaybeAddFileRange(sink, "[Unmapped]", elf.entire_file());
}

template <class Func>
bool ForEachElf(const InputFile& file, RangeSink* sink, Func func) {
  ArFile ar_file(file.data());
  unsigned long index_base = 0;

  if (ar_file.IsOpen()) {
    ArFile::MemberFile member;
    ArFile::MemberReader reader(ar_file);

    MaybeAddFileRange(sink, "[AR Headers]", ar_file.magic());

    while (reader.ReadMember(&member)) {
      MaybeAddFileRange(sink, "[AR Headers]", member.header);
      switch (member.file_type) {
        case ArFile::MemberFile::kNormal: {
          ElfFile elf(member.contents);
          if (elf.IsOpen()) {
            OnElfFile(elf, member.filename, index_base, sink, func);
            index_base += elf.section_count();
          } else {
            MaybeAddFileRange(sink, "[AR Non-ELF Member File]",
                              member.contents);
          }
          break;
        }
        case ArFile::MemberFile::kSymbolTable:
          MaybeAddFileRange(sink, "[AR Symbol Table]", member.contents);
          break;
        case ArFile::MemberFile::kLongFilenameTable:
          MaybeAddFileRange(sink, "[AR Headers]", member.contents);
          break;
      }
    }
  } else {
    ElfFile elf(file.data());
    if (!elf.IsOpen()) {
      fprintf(stderr, "Not an ELF or Archive file: %s\n",
              file.filename().c_str());
      return false;
    }

    OnElfFile(elf, file.filename(), index_base, sink, func);
  }

  return true;
}


// There are several programs that offer useful information about
// binaries:
//
// - objdump: display object file headers and contents (including disassembly)
// - readelf: more ELF-specific objdump (no disassembly though)
// - nm: display symbols
// - size: display binary size

// For object files, addresses are relative to the section they live in, which
// is indicated by ndx.  We split this into:
//
// - 24 bits for index (up to 16M symbols with -ffunction-sections)
// - 40 bits for address (up to 1TB section)
static uint64_t ToVMAddr(size_t addr, long ndx, bool is_object) {
  if (is_object) {
    return (ndx << 40) | addr;
  } else {
    return addr;
  }
}

static bool IsArchiveFile(string_view data) {
  ArFile ar(data);
  return ar.IsOpen();
}

static bool IsObjectFile(string_view data) {
  ElfFile elf(data);
  return IsArchiveFile(data) || (elf.IsOpen() && elf.header().e_type == ET_REL);
}

static void CheckNotObject(const char* source, RangeSink* sink) {
  if (IsObjectFile(sink->input_file().data())) {
    THROWF(
        "can't use data source '$0' on object files (only binaries and shared "
        "libraries)",
        source);
  }
}

static void ReadELFSymbols(const InputFile& file, RangeSink* sink,
                           SymbolTable* table, Demangler* demangler) {
  bool is_object = IsObjectFile(file.data());

  ForEachElf(
      file, sink,
      [=](const ElfFile& elf, string_view /*filename*/, uint32_t index_base) {
        for (Elf64_Xword i = 1; i < elf.section_count(); i++) {
          ElfFile::Section section;
          elf.ReadSection(i, &section);

          if (section.header().sh_type != SHT_SYMTAB) {
            continue;
          }

          Elf64_Word symbol_count = section.GetSymbolCount();

          // Find the corresponding section where the strings for the symbol
          // table can be found.
          ElfFile::Section strtab_section;
          elf.ReadSection(section.header().sh_link, &strtab_section);
          if (strtab_section.header().sh_type != SHT_STRTAB) {
            THROW("symtab section pointed to non-strtab section");
          }

          for (Elf64_Word i = 1; i < symbol_count; i++) {
            Elf64_Sym sym;

            section.ReadSymbol(i, &sym);
            int type = ELF64_ST_TYPE(sym.st_info);

            if (type != STT_OBJECT && type != STT_FUNC) {
              continue;
            }

            if (sym.st_size == 0) {
              // Maybe try to refine?  See ReadELFSectionsRefineSymbols below.
              continue;
            }

            string_view name = strtab_section.ReadName(sym.st_name);
            uint64_t full_addr =
                ToVMAddr(sym.st_value, index_base + sym.st_shndx, is_object);
            if (sink) {
              std::string namestr(name);
              if (sink->data_source() == DataSource::kCppSymbols ||
                  sink->data_source() == DataSource::kCppSymbolsStripped) {
                namestr = demangler->Demangle(namestr);
                if (sink->data_source() == DataSource::kCppSymbolsStripped) {
                  namestr = std::string(StripName(namestr));
                }
              }
              sink->AddVMRangeAllowAlias(full_addr, sym.st_size, namestr);
            }
            if (table) {
              table->insert(
                  std::make_pair(name, std::make_pair(full_addr, sym.st_size)));
            }
          }
        }
      });
}

enum ReportSectionsBy {
  kReportBySectionName,
  kReportByFlags,
  kReportByFilename,
};

static bool DoReadELFSections(RangeSink* sink, enum ReportSectionsBy report_by) {
  bool is_object = IsObjectFile(sink->input_file().data());
  return ForEachElf(
      sink->input_file(), sink,
      [=](const ElfFile& elf, string_view filename, uint32_t index_base) {
        if (elf.section_count() == 0) {
          return;
        }

        std::string name_from_flags;
        ElfFile::Section section_names;
        elf.ReadSection(elf.section_string_index(), &section_names);
        if (section_names.header().sh_type != SHT_STRTAB) {
          THROW("section string index pointed to non-strtab");
        }

        for (Elf64_Xword i = 1; i < elf.section_count(); i++) {
          ElfFile::Section section;
          elf.ReadSection(i, &section);
          const auto& header = section.header();

          if (header.sh_name == SHN_UNDEF) {
            return;
          }

          string_view name = section_names.ReadName(header.sh_name);

          auto addr = header.sh_addr;
          auto size = header.sh_size;
          auto filesize = (header.sh_type == SHT_NOBITS) ? 0 : size;
          auto vmsize = (header.sh_flags & SHF_ALLOC) ? size : 0;

          string_view contents = section.contents().substr(0, filesize);

          uint64_t full_addr = ToVMAddr(addr, index_base + i, is_object);

          if (report_by == kReportByFlags) {
            name_from_flags = std::string(name);

            name_from_flags = "Section [";

            if (header.sh_flags & SHF_ALLOC) {
              name_from_flags += 'A';
            }

            if (header.sh_flags & SHF_WRITE) {
              name_from_flags += 'W';
            }

            if (header.sh_flags & SHF_EXECINSTR) {
              name_from_flags += 'X';
            }

            name_from_flags += ']';
            sink->AddRange(name_from_flags, full_addr, vmsize, contents);
          } else if (report_by == kReportBySectionName) {
            sink->AddRange(name, full_addr, vmsize, contents);
          } else if (report_by == kReportByFilename) {
            sink->AddRange(filename, full_addr, vmsize, contents);
          }
        }

        if (report_by == kReportByFilename) {
          // Cover unmapped parts of the file.
          sink->AddFileRange(filename, elf.entire_file());
        }
      });
}

static void ReadELFSegments(RangeSink* sink) {
  if (IsObjectFile(sink->input_file().data())) {
    // Object files don't actually have segments.  But we can cheat a little bit
    // and make up "segments" based on section flags.  This can be really useful
    // when you are compiling with -ffunction-sections and -fdata-sections,
    // because in those cases the actual "sections" report becomes pretty
    // useless (since every function/data has its own section, it's like the
    // "symbols" report except less readable).
    DoReadELFSections(sink, kReportByFlags);
    return;
  }

  ForEachElf(sink->input_file(), sink,
             [=](const ElfFile& elf, string_view /*filename*/,
                 uint32_t /*index_base*/) {
               for (Elf64_Xword i = 0; i < elf.header().e_phnum; i++) {
                 ElfFile::Segment segment;
                 elf.ReadSegment(i, &segment);
                 const auto& header = segment.header();

                 if (header.p_type != PT_LOAD) {
                   continue;
                 }

                 std::string name = "LOAD [";

                 if (header.p_flags & PF_R) {
                   name += 'R';
                 }

                 if (header.p_flags & PF_W) {
                   name += 'W';
                 }

                 if (header.p_flags & PF_X) {
                   name += 'X';
                 }

                 name += ']';

                 sink->AddRange(name, header.p_vaddr, header.p_memsz,
                                segment.contents());
               }
             });
}

// ELF files put debug info directly into the binary, so we call the DWARF
// reader directly on them.  At the moment we don't attempt to make these
// work with object files.

static void ReadDWARFSections(const ElfFile& elf, dwarf::File* dwarf) {
  ElfFile::Section section_names;
  elf.ReadSection(elf.section_string_index(), &section_names);
  if (section_names.header().sh_type != SHT_STRTAB) {
    THROW("section string index pointed to non-strtab");
  }

  for (Elf64_Xword i = 1; i < elf.section_count(); i++) {
    ElfFile::Section section;
    elf.ReadSection(i, &section);
    const auto& header = section.header();

    if (header.sh_name == SHN_UNDEF) {
      return;
    }

    string_view name = section_names.ReadName(header.sh_name);

    if (name == ".debug_aranges") {
      dwarf->debug_aranges = section.contents();
    } else if (name == ".debug_str") {
      dwarf->debug_str = section.contents();
    } else if (name == ".debug_info") {
      dwarf->debug_info = section.contents();
    } else if (name == ".debug_abbrev") {
      dwarf->debug_abbrev = section.contents();
    } else if (name == ".debug_line") {
      dwarf->debug_line = section.contents();
    }
  }
}

}  // namespace

class ElfFileHandler : public FileHandler {
  void ProcessBaseMap(RangeSink* sink) override {
    if (IsObjectFile(sink->input_file().data())) {
      DoReadELFSections(sink, kReportBySectionName);
    } else {
      // Slightly more complete for executables, but not present in object
      // files.
      ReadELFSegments(sink);
    }
  }

  void ProcessFile(const std::vector<RangeSink*>& sinks) override {
    for (auto sink : sinks) {
      switch (sink->data_source()) {
        case DataSource::kSegments:
          ReadELFSegments(sink);
          break;
        case DataSource::kSections:
          DoReadELFSections(sink, kReportBySectionName);
          break;
        case DataSource::kSymbols:
        case DataSource::kCppSymbols:
        case DataSource::kCppSymbolsStripped:
          ReadELFSymbols(sink->input_file(), sink, nullptr, &demangler_);
          break;
        case DataSource::kArchiveMembers:
          DoReadELFSections(sink, kReportByFilename);
          break;
        case DataSource::kCompileUnits: {
          CheckNotObject("compileunits", sink);
          SymbolTable symtab;
          ElfFile elf(sink->input_file().data());
          ReadELFSymbols(sink->input_file(), nullptr, &symtab, &demangler_);
          dwarf::File dwarf;
          ReadDWARFSections(elf, &dwarf);
          ReadDWARFCompileUnits(dwarf, symtab, sink);
          break;
        }
        case DataSource::kInlines: {
          CheckNotObject("lineinfo", sink);
          ElfFile elf(sink->input_file().data());
          dwarf::File dwarf;
          ReadDWARFSections(elf, &dwarf);
          ReadDWARFInlines(dwarf, sink, true);
          break;
        }
        default:
          THROW("unknown data source");
      }
    }
  }

 private:
  Demangler demangler_;
};

std::unique_ptr<FileHandler> TryOpenELFFile(const InputFile& file) {
  ElfFile elf(file.data());
  ArFile ar(file.data());
  if (elf.IsOpen() || ar.IsOpen()) {
    return std::unique_ptr<FileHandler>(new ElfFileHandler());
  } else {
    return nullptr;
  }
}

}  // namespace bloaty
