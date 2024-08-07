/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#undef NDEBUG
#include <assert.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <memory>

#include "elfxx.h"
#include "mozilla/CheckedInt.h"

#define ver "1"
#define elfhack_data ".elfhack.data.v" ver
#define elfhack_text ".elfhack.text.v" ver

#ifndef R_ARM_V4BX
#  define R_ARM_V4BX 0x28
#endif
#ifndef R_ARM_CALL
#  define R_ARM_CALL 0x1c
#endif
#ifndef R_ARM_JUMP24
#  define R_ARM_JUMP24 0x1d
#endif
#ifndef R_ARM_THM_JUMP24
#  define R_ARM_THM_JUMP24 0x1e
#endif

char* rundir = nullptr;

template <typename T>
struct wrapped {
  T value;
};

class Elf_Addr_Traits {
 public:
  typedef wrapped<Elf32_Addr> Type32;
  typedef wrapped<Elf64_Addr> Type64;

  template <class endian, typename R, typename T>
  static inline void swap(T& t, R& r) {
    r.value = endian::swap(t.value);
  }
};

typedef serializable<Elf_Addr_Traits> Elf_Addr;

class ElfRelHack_Section : public ElfSection {
 public:
  ElfRelHack_Section(Elf_Shdr& s)
      : ElfSection(s, nullptr, nullptr),
        block_size((8 * s.sh_entsize - 1) * s.sh_entsize) {
    name = elfhack_data;
  };

  void serialize(std::ofstream& file, unsigned char ei_class,
                 unsigned char ei_data) {
    for (std::vector<Elf64_Addr>::iterator i = relr.begin(); i != relr.end();
         ++i) {
      Elf_Addr out;
      out.value = *i;
      out.serialize(file, ei_class, ei_data);
    }
  }

  bool isRelocatable() { return true; }

  void push_back(Elf64_Addr offset) {
    // The format used for the packed relocations is SHT_RELR, described in
    // https://groups.google.com/g/generic-abi/c/bX460iggiKg/m/Jnz1lgLJAgAJ
    // The gist of it is that an address is recorded, and the following words,
    // if their LSB is 1, represent a bitmap of word-size-spaced relocations
    // at the addresses that follow. There can be multiple such bitmaps, such
    // that very long streaks of (possibly spaced) relocations can be recorded
    // in a very compact way.
    for (;;) {
      // [block_start; block_start + block_size] represents the range of offsets
      // the current bitmap can record. If the offset doesn't fall in that
      // range, or if doesn't align properly to be recorded, we record the
      // bitmap, and slide the block corresponding to a new bitmap. If the
      // offset doesn't fall in the range for the new bitmap, or if there wasn't
      // an active bitmap in the first place, we record the offset and start a
      // new bitmap for the block that follows it.
      if (!block_start || offset < block_start ||
          offset >= block_start + block_size ||
          (offset - block_start) % shdr.sh_entsize) {
        if (bitmap) {
          relr.push_back((bitmap << 1) | 1);
          block_start += block_size;
          bitmap = 0;
          continue;
        }
        relr.push_back(offset);
        block_start = offset + shdr.sh_entsize;
        break;
      }
      bitmap |= 1ULL << ((offset - block_start) / shdr.sh_entsize);
      break;
    }
    shdr.sh_size = relr.size() * shdr.sh_entsize;
  }

 private:
  std::vector<Elf64_Addr> relr;
  size_t block_size;
  Elf64_Addr block_start = 0;
  Elf64_Addr bitmap = 0;
};

class ElfRelHackCode_Section : public ElfSection {
 public:
  ElfRelHackCode_Section(Elf_Shdr& s, Elf& e,
                         ElfRelHack_Section& relhack_section, unsigned int init,
                         unsigned int mprotect_cb, unsigned int sysconf_cb)
      : ElfSection(s, nullptr, nullptr),
        parent(e),
        relhack_section(relhack_section),
        init(init),
        init_trampoline(nullptr),
        mprotect_cb(mprotect_cb),
        sysconf_cb(sysconf_cb) {
    std::string file(rundir);
    file += "/inject/";
    switch (parent.getMachine()) {
      case EM_386:
        file += "x86";
        break;
      case EM_X86_64:
        file += "x86_64";
        break;
      case EM_ARM:
        file += "arm";
        break;
      case EM_AARCH64:
        file += "aarch64";
        break;
      default:
        throw std::runtime_error("unsupported architecture");
    }
    file += ".o";
    std::ifstream inject(file.c_str(), std::ios::in | std::ios::binary);
    elf = new Elf(inject);
    if (elf->getType() != ET_REL)
      throw std::runtime_error("object for injected code is not ET_REL");
    if (elf->getMachine() != parent.getMachine())
      throw std::runtime_error(
          "architecture of object for injected code doesn't match");

    ElfSymtab_Section* symtab = nullptr;

    // Find the symbol table.
    for (ElfSection* section = elf->getSection(1); section != nullptr;
         section = section->getNext()) {
      if (section->getType() == SHT_SYMTAB)
        symtab = (ElfSymtab_Section*)section;
    }
    if (symtab == nullptr)
      throw std::runtime_error(
          "Couldn't find a symbol table for the injected code");

    relro = parent.getSegmentByType(PT_GNU_RELRO);

    // Find the init symbol
    entry_point = -1;
    std::string symbol = "init";
    if (!init) symbol += "_noinit";
    if (relro) symbol += "_relro";
    Elf_SymValue* sym = symtab->lookup(symbol.c_str());
    if (!sym)
      throw std::runtime_error(
          "Couldn't find an 'init' symbol in the injected code");

    entry_point = sym->value.getValue();

    // Get all relevant sections from the injected code object.
    add_code_section(sym->value.getSection());

    // If the original init function is located too far away, we're going to
    // need to use a trampoline. See comment in inject.c.
    // Theoretically, we should check for (init - instr) > 0xffffff, where instr
    // is the virtual address of the instruction that calls the original init,
    // but we don't have it at this point, so punt to just init.
    if (init > 0xffffff && parent.getMachine() == EM_ARM) {
      Elf_SymValue* trampoline = symtab->lookup("init_trampoline");
      if (!trampoline) {
        throw std::runtime_error(
            "Couldn't find an 'init_trampoline' symbol in the injected code");
      }

      init_trampoline = trampoline->value.getSection();
      add_code_section(init_trampoline);
    }

    // Adjust code sections offsets according to their size
    std::vector<ElfSection*>::iterator c = code.begin();
    (*c)->getShdr().sh_addr = 0;
    for (ElfSection* last = *(c++); c != code.end(); ++c) {
      unsigned int addr = last->getShdr().sh_addr + last->getSize();
      if (addr & ((*c)->getAddrAlign() - 1))
        addr = (addr | ((*c)->getAddrAlign() - 1)) + 1;
      (*c)->getShdr().sh_addr = addr;
      // We need to align this section depending on the greater
      // alignment required by code sections.
      if (shdr.sh_addralign < (*c)->getAddrAlign())
        shdr.sh_addralign = (*c)->getAddrAlign();
      last = *c;
    }
    shdr.sh_size = code.back()->getAddr() + code.back()->getSize();
    data = static_cast<char*>(malloc(shdr.sh_size));
    if (!data) {
      throw std::runtime_error("Could not malloc ElfSection data");
    }
    char* buf = data;
    for (c = code.begin(); c != code.end(); ++c) {
      memcpy(buf, (*c)->getData(), (*c)->getSize());
      buf += (*c)->getSize();
    }
    name = elfhack_text;
  }

  ~ElfRelHackCode_Section() { delete elf; }

  void serialize(std::ofstream& file, unsigned char ei_class,
                 unsigned char ei_data) override {
    // Readjust code offsets
    for (std::vector<ElfSection*>::iterator c = code.begin(); c != code.end();
         ++c)
      (*c)->getShdr().sh_addr += getAddr();

    // Apply relocations
    for (std::vector<ElfSection*>::iterator c = code.begin(); c != code.end();
         ++c) {
      for (ElfSection* rel = elf->getSection(1); rel != nullptr;
           rel = rel->getNext())
        if (((rel->getType() == SHT_REL) || (rel->getType() == SHT_RELA)) &&
            (rel->getInfo().section == *c)) {
          if (rel->getType() == SHT_REL)
            apply_relocations((ElfRel_Section<Elf_Rel>*)rel, *c);
          else
            apply_relocations((ElfRel_Section<Elf_Rela>*)rel, *c);
        }
    }

    ElfSection::serialize(file, ei_class, ei_data);
  }

  bool isRelocatable() override { return false; }

  unsigned int getEntryPoint() { return entry_point; }

  void insertBefore(ElfSection* section, bool dirty = true) override {
    // Adjust the address so that this section is adjacent to the one it's
    // being inserted before. This avoids creating holes which subsequently
    // might lead the PHDR-adjusting code to create unnecessary additional
    // PT_LOADs.
    shdr.sh_addr =
        (section->getAddr() - shdr.sh_size) & ~(shdr.sh_addralign - 1);
    ElfSection::insertBefore(section, dirty);
  }

 private:
  void add_code_section(ElfSection* section) {
    if (section) {
      /* Don't add section if it's already been added in the past */
      for (auto s = code.begin(); s != code.end(); ++s) {
        if (section == *s) return;
      }
      code.push_back(section);
      find_code(section);
    }
  }

  /* Look at the relocations associated to the given section to find other
   * sections that it requires */
  void find_code(ElfSection* section) {
    for (ElfSection* s = elf->getSection(1); s != nullptr; s = s->getNext()) {
      if (((s->getType() == SHT_REL) || (s->getType() == SHT_RELA)) &&
          (s->getInfo().section == section)) {
        if (s->getType() == SHT_REL)
          scan_relocs_for_code((ElfRel_Section<Elf_Rel>*)s);
        else
          scan_relocs_for_code((ElfRel_Section<Elf_Rela>*)s);
      }
    }
  }

  template <typename Rel_Type>
  void scan_relocs_for_code(ElfRel_Section<Rel_Type>* rel) {
    ElfSymtab_Section* symtab = (ElfSymtab_Section*)rel->getLink();
    for (auto r = rel->rels.begin(); r != rel->rels.end(); ++r) {
      ElfSection* section =
          symtab->syms[ELF64_R_SYM(r->r_info)].value.getSection();
      add_code_section(section);
    }
  }

  // TODO: sort out which non-aarch64 relocation types should be using
  //  `value` (even though in practice it's either 0 or the same as addend)
  class pc32_relocation {
   public:
    Elf32_Addr operator()(unsigned int base_addr, Elf64_Off offset,
                          Elf64_Sxword addend, unsigned int addr,
                          Elf64_Word value) {
      return addr + addend - offset - base_addr;
    }
  };

  class arm_plt32_relocation {
   public:
    Elf32_Addr operator()(unsigned int base_addr, Elf64_Off offset,
                          Elf64_Sxword addend, unsigned int addr,
                          Elf64_Word value) {
      // We don't care about sign_extend because the only case where this is
      // going to be used only jumps forward.
      Elf32_Addr tmp = (Elf32_Addr)(addr - offset - base_addr) >> 2;
      tmp = (addend + tmp) & 0x00ffffff;
      return (addend & 0xff000000) | tmp;
    }
  };

  class arm_thm_jump24_relocation {
   public:
    Elf32_Addr operator()(unsigned int base_addr, Elf64_Off offset,
                          Elf64_Sxword addend, unsigned int addr,
                          Elf64_Word value) {
      /* Follows description of b.w and bl instructions as per
         ARM Architecture Reference Manual ARM® v7-A and ARM® v7-R edition,
         A8.6.16 We limit ourselves to Encoding T4 of b.w and Encoding T1 of bl.
         We don't care about sign_extend because the only case where this is
         going to be used only jumps forward. */
      Elf32_Addr tmp = (Elf32_Addr)(addr - offset - base_addr);
      unsigned int word0 = addend & 0xffff, word1 = addend >> 16;

      /* Encoding T4 of B.W is 10x1 ; Encoding T1 of BL is 11x1. */
      unsigned int type = (word1 & 0xd000) >> 12;
      if (((word0 & 0xf800) != 0xf000) || ((type & 0x9) != 0x9))
        throw std::runtime_error(
            "R_ARM_THM_JUMP24/R_ARM_THM_CALL relocation only supported for B.W "
            "<label> and BL <label>");

      /* When the target address points to ARM code, switch a BL to a
       * BLX. This however can't be done with a B.W without adding a
       * trampoline, which is not supported as of now. */
      if ((addr & 0x1) == 0) {
        if (type == 0x9)
          throw std::runtime_error(
              "R_ARM_THM_JUMP24/R_ARM_THM_CALL relocation only supported for "
              "BL <label> when label points to ARM code");
        /* The address of the target is always relative to a 4-bytes
         * aligned address, so if the address of the BL instruction is
         * not 4-bytes aligned, adjust for it. */
        if ((base_addr + offset) & 0x2) tmp += 2;
        /* Encoding T2 of BLX is 11x0. */
        type = 0xc;
      }

      unsigned int s = (word0 & (1 << 10)) >> 10;
      unsigned int j1 = (word1 & (1 << 13)) >> 13;
      unsigned int j2 = (word1 & (1 << 11)) >> 11;
      unsigned int i1 = j1 ^ s ? 0 : 1;
      unsigned int i2 = j2 ^ s ? 0 : 1;

      tmp += ((s << 24) | (i1 << 23) | (i2 << 22) | ((word0 & 0x3ff) << 12) |
              ((word1 & 0x7ff) << 1));

      s = (tmp & (1 << 24)) >> 24;
      j1 = ((tmp & (1 << 23)) >> 23) ^ !s;
      j2 = ((tmp & (1 << 22)) >> 22) ^ !s;

      return 0xf000 | (s << 10) | ((tmp & (0x3ff << 12)) >> 12) | (type << 28) |
             (j1 << 29) | (j2 << 27) | ((tmp & 0xffe) << 15);
    }
  };

  class gotoff_relocation {
   public:
    Elf32_Addr operator()(unsigned int base_addr, Elf64_Off offset,
                          Elf64_Sxword addend, unsigned int addr,
                          Elf64_Word value) {
      return addr + addend;
    }
  };

  template <int start, int end>
  class abs_lo12_nc_relocation {
   public:
    Elf32_Addr operator()(unsigned int base_addr, Elf64_Off offset,
                          Elf64_Sxword addend, unsigned int addr,
                          Elf64_Word value) {
      // Fill the bits [end:start] of the immediate value in an ADD, LDR or STR
      // instruction, at bits [21:10].
      // per ARM® Architecture Reference Manual ARMv8, for ARMv8-A architecture
      // profile C5.6.4, C5.6.83 or C5.6.178 and ELF for the ARM® 64-bit
      // Architecture (AArch64) 4.6.6, Table 4-9.
      Elf64_Word mask = (1 << (end + 1)) - 1;
      return value | (((((addr + addend) & mask) >> start) & 0xfff) << 10);
    }
  };

  class adr_prel_pg_hi21_relocation {
   public:
    Elf32_Addr operator()(unsigned int base_addr, Elf64_Off offset,
                          Elf64_Sxword addend, unsigned int addr,
                          Elf64_Word value) {
      // Fill the bits [32:12] of the immediate value in a ADRP instruction,
      // at bits [23:5]+[30:29].
      // per ARM® Architecture Reference Manual ARMv8, for ARMv8-A architecture
      // profile C5.6.10 and ELF for the ARM® 64-bit Architecture
      // (AArch64) 4.6.6, Table 4-9.
      Elf64_Word imm = ((addr + addend) >> 12) - ((base_addr + offset) >> 12);
      Elf64_Word immLo = (imm & 0x3) << 29;
      Elf64_Word immHi = (imm & 0x1ffffc) << 3;
      return value & 0x9f00001f | immLo | immHi;
    }
  };

  class call26_relocation {
   public:
    Elf32_Addr operator()(unsigned int base_addr, Elf64_Off offset,
                          Elf64_Sxword addend, unsigned int addr,
                          Elf64_Word value) {
      // Fill the bits [27:2] of the immediate value in a BL instruction,
      // at bits [25:0].
      // per ARM® Architecture Reference Manual ARMv8, for ARMv8-A architecture
      // profile C5.6.26 and ELF for the ARM® 64-bit Architecture
      // (AArch64) 4.6.6, Table 4-10.
      return value | (((addr + addend - offset - base_addr) & 0x0ffffffc) >> 2);
    }
  };

  template <class relocation_type>
  void apply_relocation(ElfSection* the_code, char* base, Elf_Rel* r,
                        unsigned int addr) {
    relocation_type relocation;
    Elf32_Addr value;
    memcpy(&value, base + r->r_offset, 4);
    value = relocation(the_code->getAddr(), r->r_offset, value, addr, value);
    memcpy(base + r->r_offset, &value, 4);
  }

  template <class relocation_type>
  void apply_relocation(ElfSection* the_code, char* base, Elf_Rela* r,
                        unsigned int addr) {
    relocation_type relocation;
    Elf64_Word value;
    memcpy(&value, base + r->r_offset, 4);
    Elf32_Addr new_value =
        relocation(the_code->getAddr(), r->r_offset, r->r_addend, addr, value);
    memcpy(base + r->r_offset, &new_value, 4);
  }

  template <typename Rel_Type>
  void apply_relocations(ElfRel_Section<Rel_Type>* rel, ElfSection* the_code) {
    assert(rel->getType() == Rel_Type::sh_type);
    char* buf = data + (the_code->getAddr() - code.front()->getAddr());
    // TODO: various checks on the sections
    ElfSymtab_Section* symtab = (ElfSymtab_Section*)rel->getLink();
    for (typename std::vector<Rel_Type>::iterator r = rel->rels.begin();
         r != rel->rels.end(); ++r) {
      // TODO: various checks on the symbol
      const char* name = symtab->syms[ELF64_R_SYM(r->r_info)].name;
      unsigned int addr;
      if (symtab->syms[ELF64_R_SYM(r->r_info)].value.getSection() == nullptr) {
        if (strcmp(name, "relhack") == 0) {
          addr = relhack_section.getAddr();
        } else if (strcmp(name, "elf_header") == 0) {
          // TODO: change this ungly hack to something better
          ElfSection* ehdr = parent.getSection(1)->getPrevious()->getPrevious();
          addr = ehdr->getAddr();
        } else if (strcmp(name, "original_init") == 0) {
          if (init_trampoline) {
            addr = init_trampoline->getAddr();
          } else {
            addr = init;
          }
        } else if (strcmp(name, "real_original_init") == 0) {
          addr = init;
        } else if (relro && strcmp(name, "mprotect_cb") == 0) {
          addr = mprotect_cb;
        } else if (relro && strcmp(name, "sysconf_cb") == 0) {
          addr = sysconf_cb;
        } else if (relro && strcmp(name, "relro_start") == 0) {
          addr = relro->getAddr();
        } else if (relro && strcmp(name, "relro_end") == 0) {
          addr = (relro->getAddr() + relro->getMemSize());
        } else if (strcmp(name, "_GLOBAL_OFFSET_TABLE_") == 0) {
          // We actually don't need a GOT, but need it as a reference for
          // GOTOFF relocations. We'll just use the start of the ELF file
          addr = 0;
        } else if (strcmp(name, "") == 0) {
          // This is for R_ARM_V4BX, until we find something better
          addr = -1;
        } else {
          throw std::runtime_error("Unsupported symbol in relocation");
        }
      } else {
        ElfSection* section =
            symtab->syms[ELF64_R_SYM(r->r_info)].value.getSection();
        assert((section->getType() == SHT_PROGBITS) &&
               (section->getFlags() & SHF_EXECINSTR));
        addr = symtab->syms[ELF64_R_SYM(r->r_info)].value.getValue();
      }
      // Do the relocation
#define REL(machine, type) (EM_##machine | (R_##machine##_##type << 8))
      switch (elf->getMachine() | (ELF64_R_TYPE(r->r_info) << 8)) {
        case REL(X86_64, PC32):
        case REL(X86_64, PLT32):
        case REL(386, PC32):
        case REL(386, GOTPC):
        case REL(ARM, GOTPC):
        case REL(ARM, REL32):
        case REL(AARCH64, PREL32):
          apply_relocation<pc32_relocation>(the_code, buf, &*r, addr);
          break;
        case REL(ARM, CALL):
        case REL(ARM, JUMP24):
        case REL(ARM, PLT32):
          apply_relocation<arm_plt32_relocation>(the_code, buf, &*r, addr);
          break;
        case REL(ARM, THM_PC22 /* THM_CALL */):
        case REL(ARM, THM_JUMP24):
          apply_relocation<arm_thm_jump24_relocation>(the_code, buf, &*r, addr);
          break;
        case REL(386, GOTOFF):
        case REL(ARM, GOTOFF):
          apply_relocation<gotoff_relocation>(the_code, buf, &*r, addr);
          break;
        case REL(AARCH64, ADD_ABS_LO12_NC):
          apply_relocation<abs_lo12_nc_relocation<0, 11>>(the_code, buf, &*r,
                                                          addr);
          break;
        case REL(AARCH64, ADR_PREL_PG_HI21):
          apply_relocation<adr_prel_pg_hi21_relocation>(the_code, buf, &*r,
                                                        addr);
          break;
        case REL(AARCH64, LDST32_ABS_LO12_NC):
          apply_relocation<abs_lo12_nc_relocation<2, 11>>(the_code, buf, &*r,
                                                          addr);
          break;
        case REL(AARCH64, LDST64_ABS_LO12_NC):
          apply_relocation<abs_lo12_nc_relocation<3, 11>>(the_code, buf, &*r,
                                                          addr);
          break;
        case REL(AARCH64, CALL26):
          apply_relocation<call26_relocation>(the_code, buf, &*r, addr);
          break;
        case REL(ARM, V4BX):
          // Ignore R_ARM_V4BX relocations
          break;
        default:
          throw std::runtime_error("Unsupported relocation type");
      }
    }
  }

  Elf *elf, &parent;
  ElfRelHack_Section& relhack_section;
  std::vector<ElfSection*> code;
  unsigned int init;
  ElfSection* init_trampoline;
  unsigned int mprotect_cb;
  unsigned int sysconf_cb;
  int entry_point;
  ElfSegment* relro;
};

unsigned int get_addend(Elf_Rel* rel, Elf* elf) {
  ElfLocation loc(rel->r_offset, elf);
  Elf_Addr addr(loc.getBuffer(), Elf_Addr::size(elf->getClass()),
                elf->getClass(), elf->getData());
  return addr.value;
}

unsigned int get_addend(Elf_Rela* rel, Elf* elf) { return rel->r_addend; }

void set_relative_reloc(Elf_Rel* rel, Elf* elf, unsigned int value) {
  ElfLocation loc(rel->r_offset, elf);
  Elf_Addr addr;
  addr.value = value;
  addr.serialize(const_cast<char*>(loc.getBuffer()),
                 Elf_Addr::size(elf->getClass()), elf->getClass(),
                 elf->getData());
}

void set_relative_reloc(Elf_Rela* rel, Elf* elf, unsigned int value) {
  // ld puts the value of relocated relocations both in the addend and
  // at r_offset. For consistency, keep it that way.
  set_relative_reloc((Elf_Rel*)rel, elf, value);
  rel->r_addend = value;
}

void maybe_split_segment(Elf* elf, ElfSegment* segment) {
  std::list<ElfSection*>::iterator it = segment->begin();
  for (ElfSection* last = *(it++); it != segment->end(); last = *(it++)) {
    // When two consecutive non-SHT_NOBITS sections are apart by more
    // than the alignment of the section, the second can be moved closer
    // to the first, but this requires the segment to be split.
    if (((*it)->getType() != SHT_NOBITS) && (last->getType() != SHT_NOBITS) &&
        ((*it)->getOffset() - last->getOffset() - last->getSize() >
         segment->getAlign())) {
      // Probably very wrong.
      Elf_Phdr phdr;
      phdr.p_type = PT_LOAD;
      phdr.p_vaddr = 0;
      phdr.p_paddr = phdr.p_vaddr + segment->getVPDiff();
      phdr.p_flags = segment->getFlags();
      phdr.p_align = segment->getAlign();
      phdr.p_filesz = (Elf64_Xword)-1LL;
      phdr.p_memsz = (Elf64_Xword)-1LL;
      ElfSegment* newSegment = new ElfSegment(&phdr);
      elf->insertSegmentAfter(segment, newSegment);
      for (; it != segment->end(); ++it) {
        newSegment->addSection(*it);
      }
      for (it = newSegment->begin(); it != newSegment->end(); ++it) {
        segment->removeSection(*it);
      }
      break;
    }
  }
}

// EH_FRAME constants
static const unsigned char DW_EH_PE_absptr = 0x00;
static const unsigned char DW_EH_PE_omit = 0xff;

// Data size
static const unsigned char DW_EH_PE_LEB128 = 0x01;
static const unsigned char DW_EH_PE_data2 = 0x02;
static const unsigned char DW_EH_PE_data4 = 0x03;
static const unsigned char DW_EH_PE_data8 = 0x04;

// Data signedness
static const unsigned char DW_EH_PE_signed = 0x08;

// Modifiers
static const unsigned char DW_EH_PE_pcrel = 0x10;

// Return the data size part of the encoding value
static unsigned char encoding_data_size(unsigned char encoding) {
  return encoding & 0x07;
}

// Advance `step` bytes in the buffer at `data` with size `size`, returning
// the advanced buffer pointer and remaining size.
// Returns true if step <= size.
static bool advance_buffer(char** data, size_t* size, size_t step) {
  if (step > *size) return false;

  *data += step;
  *size -= step;
  return true;
}

// Advance in the given buffer, skipping the full length of the variable-length
// encoded LEB128 type in CIE/FDE data.
static bool skip_LEB128(char** data, size_t* size) {
  if (!*size) return false;

  while (*size && (*(*data)++ & (char)0x80)) {
    (*size)--;
  }
  return true;
}

// Advance in the given buffer, skipping the full length of a pointer encoded
// with the given encoding.
static bool skip_eh_frame_pointer(char** data, size_t* size,
                                  unsigned char encoding) {
  switch (encoding_data_size(encoding)) {
    case DW_EH_PE_data2:
      return advance_buffer(data, size, 2);
    case DW_EH_PE_data4:
      return advance_buffer(data, size, 4);
    case DW_EH_PE_data8:
      return advance_buffer(data, size, 8);
    case DW_EH_PE_LEB128:
      return skip_LEB128(data, size);
  }
  throw std::runtime_error("unreachable");
}

// Specialized implementations for adjust_eh_frame_pointer().
template <typename T>
static bool adjust_eh_frame_sized_pointer(char** data, size_t* size,
                                          ElfSection* eh_frame,
                                          unsigned int origAddr, Elf* elf) {
  if (*size < sizeof(T)) return false;

  serializable<FixedSizeData<T>> pointer(*data, *size, elf->getClass(),
                                         elf->getData());
  mozilla::CheckedInt<T> value = pointer.value;
  if (origAddr < eh_frame->getAddr()) {
    unsigned int diff = eh_frame->getAddr() - origAddr;
    value -= diff;
  } else {
    unsigned int diff = origAddr - eh_frame->getAddr();
    value += diff;
  }
  if (!value.isValid())
    throw std::runtime_error("Overflow while adjusting eh_frame");
  pointer.value = value.value();
  pointer.serialize(*data, *size, elf->getClass(), elf->getData());
  return advance_buffer(data, size, sizeof(T));
}

// In the given eh_frame section, adjust the pointer with the given encoding,
// pointed to by the given buffer (`data`, `size`), considering the eh_frame
// section was originally at `origAddr`. Also advances in the buffer.
static bool adjust_eh_frame_pointer(char** data, size_t* size,
                                    unsigned char encoding,
                                    ElfSection* eh_frame, unsigned int origAddr,
                                    Elf* elf) {
  if ((encoding & 0x70) != DW_EH_PE_pcrel)
    return skip_eh_frame_pointer(data, size, encoding);

  if (encoding & DW_EH_PE_signed) {
    switch (encoding_data_size(encoding)) {
      case DW_EH_PE_data2:
        return adjust_eh_frame_sized_pointer<int16_t>(data, size, eh_frame,
                                                      origAddr, elf);
      case DW_EH_PE_data4:
        return adjust_eh_frame_sized_pointer<int32_t>(data, size, eh_frame,
                                                      origAddr, elf);
      case DW_EH_PE_data8:
        return adjust_eh_frame_sized_pointer<int64_t>(data, size, eh_frame,
                                                      origAddr, elf);
    }
  } else {
    switch (encoding_data_size(encoding)) {
      case DW_EH_PE_data2:
        return adjust_eh_frame_sized_pointer<uint16_t>(data, size, eh_frame,
                                                       origAddr, elf);
      case DW_EH_PE_data4:
        return adjust_eh_frame_sized_pointer<uint32_t>(data, size, eh_frame,
                                                       origAddr, elf);
      case DW_EH_PE_data8:
        return adjust_eh_frame_sized_pointer<uint64_t>(data, size, eh_frame,
                                                       origAddr, elf);
    }
  }

  throw std::runtime_error("Unsupported eh_frame pointer encoding");
}

// The eh_frame section may contain "PC"-relative pointers. If we move the
// section, those need to be adjusted. Other type of pointers are relative to
// sections we don't touch.
static void adjust_eh_frame(ElfSection* eh_frame, unsigned int origAddr,
                            Elf* elf) {
  if (eh_frame->getAddr() == origAddr)  // nothing to do;
    return;

  char* data = const_cast<char*>(eh_frame->getData());
  size_t size = eh_frame->getSize();
  unsigned char LSDAencoding = DW_EH_PE_omit;
  unsigned char FDEencoding = DW_EH_PE_absptr;
  bool hasZ = false;

  // Decoding of eh_frame based on https://www.airs.com/blog/archives/460
  while (size) {
    if (size < sizeof(uint32_t)) goto malformed;

    serializable<FixedSizeData<uint32_t>> entryLength(
        data, size, elf->getClass(), elf->getData());
    if (!advance_buffer(&data, &size, sizeof(uint32_t))) goto malformed;

    char* cursor = data;
    size_t length = entryLength.value;

    if (length == 0) {
      continue;
    }

    if (size < sizeof(uint32_t)) goto malformed;

    serializable<FixedSizeData<uint32_t>> id(data, size, elf->getClass(),
                                             elf->getData());
    if (!advance_buffer(&cursor, &length, sizeof(uint32_t))) goto malformed;

    if (id.value == 0) {
      // This is a Common Information Entry
      if (length < 2) goto malformed;
      // Reset LSDA and FDE encodings, and hasZ for subsequent FDEs.
      LSDAencoding = DW_EH_PE_omit;
      FDEencoding = DW_EH_PE_absptr;
      hasZ = false;
      // CIE version. Should only be 1 or 3.
      char version = *cursor++;
      length--;
      if (version != 1 && version != 3) {
        throw std::runtime_error("Unsupported eh_frame version");
      }
      // NUL terminated string.
      const char* augmentationString = cursor;
      size_t l = strnlen(augmentationString, length - 1);
      if (l == length - 1) goto malformed;
      if (!advance_buffer(&cursor, &length, l + 1)) goto malformed;
      // Skip code alignment factor (LEB128)
      if (!skip_LEB128(&cursor, &length)) goto malformed;
      // Skip data alignment factor (LEB128)
      if (!skip_LEB128(&cursor, &length)) goto malformed;
      // Skip return address register (single byte in CIE version 1, LEB128
      // in CIE version 3)
      if (version == 1) {
        if (!advance_buffer(&cursor, &length, 1)) goto malformed;
      } else {
        if (!skip_LEB128(&cursor, &length)) goto malformed;
      }
      // Past this, it's data driven by the contents of the augmentation string.
      for (size_t i = 0; i < l; i++) {
        if (!length) goto malformed;
        switch (augmentationString[i]) {
          case 'z':
            if (!skip_LEB128(&cursor, &length)) goto malformed;
            hasZ = true;
            break;
          case 'L':
            LSDAencoding = *cursor++;
            length--;
            break;
          case 'R':
            FDEencoding = *cursor++;
            length--;
            break;
          case 'P': {
            unsigned char encoding = (unsigned char)*cursor++;
            length--;
            if (!adjust_eh_frame_pointer(&cursor, &length, encoding, eh_frame,
                                         origAddr, elf))
              goto malformed;
          } break;
          default:
            goto malformed;
        }
      }
    } else {
      // This is a Frame Description Entry
      // Starting address
      if (!adjust_eh_frame_pointer(&cursor, &length, FDEencoding, eh_frame,
                                   origAddr, elf))
        goto malformed;

      if (LSDAencoding != DW_EH_PE_omit) {
        // Skip number of bytes, same size as the starting address.
        if (!skip_eh_frame_pointer(&cursor, &length, FDEencoding))
          goto malformed;
        if (hasZ) {
          if (!skip_LEB128(&cursor, &length)) goto malformed;
        }
        // pointer to the LSDA.
        if (!adjust_eh_frame_pointer(&cursor, &length, LSDAencoding, eh_frame,
                                     origAddr, elf))
          goto malformed;
      }
    }

    data += entryLength.value;
    size -= entryLength.value;
  }
  return;

malformed:
  throw std::runtime_error("malformed .eh_frame");
}

template <typename Rel_Type>
int do_relocation_section(Elf* elf, unsigned int rel_type,
                          unsigned int rel_type2, bool force) {
  ElfDynamic_Section* dyn = elf->getDynSection();
  if (dyn == nullptr) {
    fprintf(stderr, "Couldn't find SHT_DYNAMIC section\n");
    return -1;
  }

  ElfRel_Section<Rel_Type>* section =
      (ElfRel_Section<Rel_Type>*)dyn->getSectionForType(Rel_Type::d_tag);
  if (section == nullptr) {
    fprintf(stderr, "No relocations\n");
    return -1;
  }
  assert(section->getType() == Rel_Type::sh_type);

  Elf64_Shdr relhack64_section = {0,
                                  SHT_PROGBITS,
                                  SHF_ALLOC,
                                  0,
                                  (Elf64_Off)-1LL,
                                  0,
                                  SHN_UNDEF,
                                  0,
                                  Elf_Addr::size(elf->getClass()),
                                  Elf_Addr::size(elf->getClass())};
  Elf64_Shdr relhackcode64_section = {0,
                                      SHT_PROGBITS,
                                      SHF_ALLOC | SHF_EXECINSTR,
                                      0,
                                      (Elf64_Off)-1LL,
                                      0,
                                      SHN_UNDEF,
                                      0,
                                      1,
                                      0};

  unsigned int entry_sz = Elf_Addr::size(elf->getClass());

  // The injected code needs to be executed before any init code in the
  // binary. There are three possible cases:
  // - The binary has no init code at all. In this case, we will add a
  //   DT_INIT entry pointing to the injected code.
  // - The binary has a DT_INIT entry. In this case, we will interpose:
  //   we change DT_INIT to point to the injected code, and have the
  //   injected code call the original DT_INIT entry point.
  // - The binary has no DT_INIT entry, but has a DT_INIT_ARRAY. In this
  //   case, we interpose as well, by replacing the first entry in the
  //   array to point to the injected code, and have the injected code
  //   call the original first entry.
  // The binary may have .ctors instead of DT_INIT_ARRAY, for its init
  // functions, but this falls into the second case above, since .ctors
  // are actually run by DT_INIT code.
  ElfValue* value = dyn->getValueForType(DT_INIT);
  unsigned int original_init = value ? value->getValue() : 0;
  ElfSection* init_array = nullptr;
  if (!value || !value->getValue()) {
    value = dyn->getValueForType(DT_INIT_ARRAYSZ);
    if (value && value->getValue() >= entry_sz)
      init_array = dyn->getSectionForType(DT_INIT_ARRAY);
  }

  Elf_Shdr relhack_section(relhack64_section);
  Elf_Shdr relhackcode_section(relhackcode64_section);
  auto relhack_ptr = std::make_unique<ElfRelHack_Section>(relhack_section);
  auto relhack = relhack_ptr.get();

  ElfSymtab_Section* symtab = (ElfSymtab_Section*)section->getLink();
  Elf_SymValue* sym = symtab->lookup("__cxa_pure_virtual");

  std::vector<Rel_Type> new_rels;
  std::vector<Rel_Type> init_array_relocs;
  size_t init_array_insert = 0;
  for (typename std::vector<Rel_Type>::iterator i = section->rels.begin();
       i != section->rels.end(); ++i) {
    // We don't need to keep R_*_NONE relocations
    if (!ELF64_R_TYPE(i->r_info)) continue;
    ElfLocation loc(i->r_offset, elf);
    // __cxa_pure_virtual is a function used in vtables to point at pure
    // virtual methods. The __cxa_pure_virtual function usually abort()s.
    // These functions are however normally never called. In the case
    // where they would, jumping to the null address instead of calling
    // __cxa_pure_virtual is going to work just as well. So we can remove
    // relocations for the __cxa_pure_virtual symbol and null out the
    // content at the offset pointed by the relocation.
    if (sym) {
      if (sym->defined) {
        // If we are statically linked to libstdc++, the
        // __cxa_pure_virtual symbol is defined in our lib, and we
        // have relative relocations (rel_type) for it.
        if (ELF64_R_TYPE(i->r_info) == rel_type) {
          Elf_Addr addr(loc.getBuffer(), entry_sz, elf->getClass(),
                        elf->getData());
          if (addr.value == sym->value.getValue()) {
            memset((char*)loc.getBuffer(), 0, entry_sz);
            continue;
          }
        }
      } else {
        // If we are dynamically linked to libstdc++, the
        // __cxa_pure_virtual symbol is undefined in our lib, and we
        // have absolute relocations (rel_type2) for it.
        if ((ELF64_R_TYPE(i->r_info) == rel_type2) &&
            (sym == &symtab->syms[ELF64_R_SYM(i->r_info)])) {
          memset((char*)loc.getBuffer(), 0, entry_sz);
          continue;
        }
      }
    }
    // Keep track of the relocations associated with the init_array section.
    if (init_array && i->r_offset >= init_array->getAddr() &&
        i->r_offset < init_array->getAddr() + init_array->getSize()) {
      init_array_relocs.push_back(*i);
      init_array_insert = new_rels.size();
    } else if (!(loc.getSection()->getFlags() & SHF_WRITE) ||
               (ELF64_R_TYPE(i->r_info) != rel_type)) {
      // Don't pack relocations happening in non writable sections.
      // Our injected code is likely not to be allowed to write there.
      new_rels.push_back(*i);
    } else if (i->r_offset & 1) {
      // RELR packing doesn't support relocations at an odd address, but
      // there shouldn't be any.
      new_rels.push_back(*i);
    } else {
      // With Elf_Rel, the value pointed by the relocation offset is the addend.
      // With Elf_Rela, the addend is in the relocation entry, but the elfhacked
      // relocation info doesn't contain it. Elfhack relies on the value pointed
      // by the relocation offset to also contain the addend. Which is true with
      // BFD ld and gold, but not lld, which leaves that nulled out. So if that
      // value is nulled out, we update it to the addend.
      Elf_Addr addr(loc.getBuffer(), entry_sz, elf->getClass(), elf->getData());
      unsigned int addend = get_addend(&*i, elf);
      if (addr.value == 0) {
        addr.value = addend;
        addr.serialize(const_cast<char*>(loc.getBuffer()), entry_sz,
                       elf->getClass(), elf->getData());
      } else if (addr.value != addend) {
        fprintf(stderr,
                "Relocation addend inconsistent with content. Skipping\n");
        return -1;
      }
      relhack->push_back(i->r_offset);
    }
  }
  // Last entry must be a nullptr
  relhack->push_back(0);

  if (init_array) {
    // Some linkers create a DT_INIT_ARRAY section that, for all purposes,
    // is empty: it only contains 0x0 or 0xffffffff pointers with no
    // relocations. In some other cases, there can be null pointers with no
    // relocations in the middle of the section. Example: crtend_so.o in the
    // Android NDK contains a sized .init_array with a null pointer and no
    // relocation, which ends up in all Android libraries, and in some cases it
    // ends up in the middle of the final .init_array section. If we have such a
    // reusable slot at the beginning of .init_array, we just use it. It we have
    // one in the middle of .init_array, we slide its content to move the "hole"
    // at the beginning and use it there (we need our injected code to run
    // before any other). Otherwise, replace the first entry and keep the
    // original pointer.
    std::sort(init_array_relocs.begin(), init_array_relocs.end(),
              [](Rel_Type& a, Rel_Type& b) { return a.r_offset < b.r_offset; });
    size_t expected = init_array->getAddr();
    const size_t zero = 0;
    const size_t all = SIZE_MAX;
    const char* data = init_array->getData();
    size_t length = Elf_Addr::size(elf->getClass());
    size_t off = 0;
    for (; off < init_array_relocs.size(); off++) {
      auto& r = init_array_relocs[off];
      if (r.r_offset >= expected + length &&
          (memcmp(data + off * length, &zero, length) == 0 ||
           memcmp(data + off * length, &all, length) == 0)) {
        // We found a hole, move the preceding entries.
        while (off) {
          auto& p = init_array_relocs[--off];
          if (ELF64_R_TYPE(p.r_info) == rel_type) {
            unsigned int addend = get_addend(&p, elf);
            p.r_offset += length;
            set_relative_reloc(&p, elf, addend);
          } else {
            fprintf(stderr,
                    "Unsupported relocation type in DT_INIT_ARRAY. Skipping\n");
            return -1;
          }
        }
        break;
      }
      expected = r.r_offset + length;
    }

    if (off == 0) {
      // We either found a hole above, and can now use the first entry,
      // or the init_array section is effectively empty (see further above)
      // and we also can use the first entry.
      // Either way, code further below will take care of actually setting
      // the right r_info and r_added for the relocation.
      Rel_Type rel;
      rel.r_offset = init_array->getAddr();
      init_array_relocs.insert(init_array_relocs.begin(), rel);
    } else {
      // Use relocated value of DT_INIT_ARRAY's first entry for the
      // function to be called by the injected code.
      auto& rel = init_array_relocs[0];
      unsigned int addend = get_addend(&rel, elf);
      if (ELF64_R_TYPE(rel.r_info) == rel_type) {
        original_init = addend;
      } else if (ELF64_R_TYPE(rel.r_info) == rel_type2) {
        ElfSymtab_Section* symtab = (ElfSymtab_Section*)section->getLink();
        original_init =
            symtab->syms[ELF64_R_SYM(rel.r_info)].value.getValue() + addend;
      } else {
        fprintf(stderr,
                "Unsupported relocation type for DT_INIT_ARRAY's first entry. "
                "Skipping\n");
        return -1;
      }
    }

    new_rels.insert(std::next(new_rels.begin(), init_array_insert),
                    init_array_relocs.begin(), init_array_relocs.end());
  }

  unsigned int mprotect_cb = 0;
  unsigned int sysconf_cb = 0;
  // If there is a relro segment, our injected code will run after the linker
  // sets the corresponding pages read-only. We need to make our code change
  // that to read-write before applying relocations, which means it needs to
  // call mprotect. To do that, we need to find a reference to the mprotect
  // symbol. In case the library already has one, we use that, but otherwise, we
  // add the symbol. Then the injected code needs to be able to call the
  // corresponding function, which means it needs access to a pointer to it. We
  // get such a pointer by making the linker apply a relocation for the symbol
  // at an address our code can read. The problem here is that there is not much
  // relocated space where we can put such a pointer, so we abuse the bss
  // section temporarily (it will be restored to a null value before any code
  // can actually use it)
  if (elf->getSegmentByType(PT_GNU_RELRO)) {
    ElfSection* gnu_versym = dyn->getSectionForType(DT_VERSYM);
    auto ensure_symbol = [&symtab, &gnu_versym](const char* symbol) {
      Elf_SymValue* sym_value = symtab->lookup(symbol, STT(FUNC));
      if (!sym_value) {
        symtab->syms.emplace_back();
        sym_value = &symtab->syms.back();
        symtab->grow(symtab->syms.size() * symtab->getEntSize());
        sym_value->name =
            ((ElfStrtab_Section*)symtab->getLink())->getStr(symbol);
        sym_value->info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
        sym_value->other = STV_DEFAULT;
        new (&sym_value->value) ElfLocation(nullptr, 0, ElfLocation::ABSOLUTE);
        sym_value->size = 0;
        sym_value->defined = false;

        // The DT_VERSYM data (in the .gnu.version section) has the same number
        // of entries as the symbols table. Since we added one entry there, we
        // need to add one entry here. Zeroes in the extra data means no version
        // for that symbol, which is the simplest thing to do.
        if (gnu_versym) {
          gnu_versym->grow(gnu_versym->getSize() + gnu_versym->getEntSize());
        }
      }
    };
    // ensure_symbol may trigger a symbol table vector resize, so only lookup
    // the symbols after we're done touching the symbol table.
    ensure_symbol("mprotect");
    ensure_symbol("sysconf");
    Elf_SymValue* mprotect = symtab->lookup("mprotect", STT(FUNC));
    Elf_SymValue* sysconf = symtab->lookup("sysconf", STT(FUNC));

    // Add relocations for the mprotect and sysconf symbols.
    auto add_relocation_to = [&new_rels, &symtab, rel_type2](
                                 Elf_SymValue* symbol, unsigned int location) {
      new_rels.emplace_back();
      Rel_Type& rel = new_rels.back();
      memset(&rel, 0, sizeof(rel));
      rel.r_info = ELF64_R_INFO(
          std::distance(symtab->syms.begin(),
                        std::vector<Elf_SymValue>::iterator(symbol)),
          rel_type2);
      rel.r_offset = location;
      return location;
    };

    // Find the beginning of the bss section, and use an aligned location in
    // there for the relocation.
    for (ElfSection* s = elf->getSection(1); s != nullptr; s = s->getNext()) {
      if (s->getType() != SHT_NOBITS ||
          (s->getFlags() & (SHF_TLS | SHF_WRITE)) != SHF_WRITE) {
        continue;
      }
      size_t ptr_size = Elf_Addr::size(elf->getClass());
      size_t usable_start = (s->getAddr() + ptr_size - 1) & ~(ptr_size - 1);
      size_t usable_end = (s->getAddr() + s->getSize()) & ~(ptr_size - 1);
      if (usable_end - usable_start >= 2 * ptr_size) {
        mprotect_cb = add_relocation_to(mprotect, usable_start);
        sysconf_cb = add_relocation_to(sysconf, usable_start + ptr_size);
        break;
      }
    }

    if (mprotect_cb == 0 || sysconf_cb == 0) {
      fprintf(stderr, "Couldn't find .bss. Skipping\n");
      return -1;
    }
  }

  size_t old_size = section->getSize();

  section->rels.assign(new_rels.begin(), new_rels.end());
  section->shrink(new_rels.size() * section->getEntSize());

  auto relhackcode_ptr = std::make_unique<ElfRelHackCode_Section>(
      relhackcode_section, *elf, *relhack, original_init, mprotect_cb,
      sysconf_cb);
  auto relhackcode = relhackcode_ptr.get();
  // Find the first executable section, and insert the relhack code before
  // that. The relhack data is inserted between .rel.dyn and .rel.plt.
  ElfSection* first_executable = nullptr;
  for (ElfSection* s = elf->getSection(1); s != nullptr; s = s->getNext()) {
    if (s->getFlags() & SHF_EXECINSTR) {
      first_executable = s;
      break;
    }
  }

  if (!first_executable) {
    fprintf(stderr, "Couldn't find executable section. Skipping\n");
    return -1;
  }

  // Once the pointers for relhack, relhackcode, and init are inserted,
  // their ownership is transferred to the Elf object, which will free
  // them when itself is freed. Hence the .release() calls here (and
  // the init.release() call later on). Please note that the raw
  // pointers will continue to be used after .release(), which is why
  // we are caching them (since .release() will end up setting the
  // smart pointer's internal raw pointer to nullptr).

  relhack->insertBefore(section);
  relhack_ptr.release();

  relhackcode->insertBefore(first_executable);
  relhackcode_ptr.release();

  // Don't try further if we can't gain from the relocation section size change.
  // We account for the fact we're going to split the PT_LOAD before the
  // injected code section, so the overhead of the page alignment for section
  // needs to be accounted for.
  size_t align = first_executable->getSegmentByType(PT_LOAD)->getAlign();
  size_t new_size = relhack->getSize() + section->getSize() +
                    relhackcode->getSize() +
                    (relhackcode->getAddr() & (align - 1));
  if (!force && (new_size >= old_size || old_size - new_size < align)) {
    fprintf(stderr, "No gain. Skipping\n");
    return -1;
  }

  // .eh_frame/.eh_frame_hdr may be between the relocation sections and the
  // executable sections. When that happens, we may end up creating a separate
  // PT_LOAD for just both of them because they are not considered relocatable.
  // But they are, in fact, kind of relocatable, albeit with some manual work.
  // Which we'll do here.
  ElfSegment* eh_frame_segment = elf->getSegmentByType(PT_GNU_EH_FRAME);
  ElfSection* eh_frame_hdr =
      eh_frame_segment ? eh_frame_segment->getFirstSection() : nullptr;
  // The .eh_frame section usually follows the eh_frame_hdr section.
  ElfSection* eh_frame = eh_frame_hdr ? eh_frame_hdr->getNext() : nullptr;
  ElfSection* first = eh_frame_hdr;
  ElfSection* second = eh_frame;
  if (eh_frame && strcmp(eh_frame->getName(), ".eh_frame")) {
    // But sometimes it appears *before* the eh_frame_hdr section.
    eh_frame = eh_frame_hdr->getPrevious();
    first = eh_frame;
    second = eh_frame_hdr;
  }
  if (eh_frame_hdr && (!eh_frame || strcmp(eh_frame->getName(), ".eh_frame"))) {
    throw std::runtime_error(
        "Expected to find an .eh_frame section adjacent to .eh_frame_hdr");
  }
  if (eh_frame && first->getAddr() > relhack->getAddr() &&
      second->getAddr() < first_executable->getAddr()) {
    // The distance between both sections needs to be preserved because
    // eh_frame_hdr contains relative offsets to eh_frame. Well, they could be
    // relocated too, but it's not worth the effort for the few number of bytes
    // this would save.
    Elf64_Off distance = second->getAddr() - first->getAddr();
    Elf64_Addr origAddr = eh_frame->getAddr();
    ElfSection* previous = first->getPrevious();
    first->getShdr().sh_addr = (previous->getAddr() + previous->getSize() +
                                first->getAddrAlign() - 1) &
                               ~(first->getAddrAlign() - 1);
    second->getShdr().sh_addr =
        (first->getAddr() + std::min(first->getSize(), distance) +
         second->getAddrAlign() - 1) &
        ~(second->getAddrAlign() - 1);
    // Re-adjust to keep the original distance.
    // If the first section has a smaller alignment requirement than the second,
    // the second will be farther away, so we need to adjust the first.
    // If the second section has a smaller alignment requirement than the first,
    // it will already be at the right distance.
    first->getShdr().sh_addr = second->getAddr() - distance;
    assert(distance == second->getAddr() - first->getAddr());
    first->markDirty();
    adjust_eh_frame(eh_frame, origAddr, elf);
  }

  // Adjust PT_LOAD segments
  for (ElfSegment* segment = elf->getSegmentByType(PT_LOAD); segment;
       segment = elf->getSegmentByType(PT_LOAD, segment)) {
    maybe_split_segment(elf, segment);
  }

  // Ensure Elf sections will be at their final location.
  elf->normalize();
  auto init =
      std::make_unique<ElfLocation>(relhackcode, relhackcode->getEntryPoint());
  if (init_array) {
    // Adjust the first DT_INIT_ARRAY entry to point at the injected code
    // by transforming its relocation into a relative one pointing to the
    // address of the injected code.
    Rel_Type* rel = &section->rels[init_array_insert];
    rel->r_info = ELF64_R_INFO(0, rel_type);  // Set as a relative relocation
    set_relative_reloc(rel, elf, init->getValue());
  } else {
    if (dyn->setValueForType(DT_INIT, init.get())) {
      init.release();
    } else {
      fprintf(stderr, "Can't grow .dynamic section to set DT_INIT. Skipping\n");
      return -1;
    }
  }

  // TODO: adjust the value according to the remaining number of relative
  // relocations
  if (dyn->getValueForType(Rel_Type::d_tag_count))
    dyn->setValueForType(Rel_Type::d_tag_count, new ElfPlainValue(0));

  return 0;
}

static inline int backup_file(const char* name) {
  std::string fname(name);
  fname += ".bak";
  return rename(name, fname.c_str());
}

void do_file(const char* name, bool backup = false, bool force = false) {
  std::ifstream file(name, std::ios::in | std::ios::binary);
  Elf elf(file);
  unsigned int size = elf.getSize();
  fprintf(stderr, "%s: ", name);
  if (elf.getType() != ET_DYN) {
    fprintf(stderr, "Not a shared object. Skipping\n");
    return;
  }

  for (ElfSection* section = elf.getSection(1); section != nullptr;
       section = section->getNext()) {
    if (section->getName() &&
        (strncmp(section->getName(), ".elfhack.", 9) == 0)) {
      fprintf(stderr, "Already elfhacked. Skipping\n");
      return;
    }
  }

  int exit = -1;
  switch (elf.getMachine()) {
    case EM_386:
      exit =
          do_relocation_section<Elf_Rel>(&elf, R_386_RELATIVE, R_386_32, force);
      break;
    case EM_X86_64:
      exit = do_relocation_section<Elf_Rela>(&elf, R_X86_64_RELATIVE,
                                             R_X86_64_64, force);
      break;
    case EM_ARM:
      exit = do_relocation_section<Elf_Rel>(&elf, R_ARM_RELATIVE, R_ARM_ABS32,
                                            force);
      break;
    case EM_AARCH64:
      exit = do_relocation_section<Elf_Rela>(&elf, R_AARCH64_RELATIVE,
                                             R_AARCH64_ABS64, force);
      break;
    default:
      throw std::runtime_error("unsupported architecture");
  }
  if (exit == 0) {
    if (!force && (elf.getSize() >= size)) {
      fprintf(stderr, "No gain. Skipping\n");
    } else if (backup && backup_file(name) != 0) {
      fprintf(stderr, "Couln't create backup file\n");
    } else {
      std::ofstream ofile(name,
                          std::ios::out | std::ios::binary | std::ios::trunc);
      elf.write(ofile);
      fprintf(stderr, "Reduced by %d bytes\n", size - elf.getSize());
    }
  }
}

void undo_file(const char* name, bool backup = false) {
  std::ifstream file(name, std::ios::in | std::ios::binary);
  Elf elf(file);
  unsigned int size = elf.getSize();
  fprintf(stderr, "%s: ", name);
  if (elf.getType() != ET_DYN) {
    fprintf(stderr, "Not a shared object. Skipping\n");
    return;
  }

  ElfSection *data = nullptr, *text = nullptr;
  for (ElfSection* section = elf.getSection(1); section != nullptr;
       section = section->getNext()) {
    if (section->getName() && (strcmp(section->getName(), elfhack_data) == 0))
      data = section;
    if (section->getName() && (strcmp(section->getName(), elfhack_text) == 0))
      text = section;
  }

  if (!data || !text) {
    fprintf(stderr, "Not elfhacked. Skipping\n");
    return;
  }

  // When both elfhack sections are in the same segment, try to merge
  // the segment that contains them both and the following segment.
  // When the elfhack sections are in separate segments, try to merge
  // those segments.
  ElfSegment* first = data->getSegmentByType(PT_LOAD);
  ElfSegment* second = text->getSegmentByType(PT_LOAD);
  if (first == second) {
    second = elf.getSegmentByType(PT_LOAD, first);
  }

  // Only merge the segments when their flags match.
  if (second->getFlags() != first->getFlags()) {
    fprintf(stderr, "Couldn't merge PT_LOAD segments. Skipping\n");
    return;
  }
  // Move sections from the second PT_LOAD to the first, and remove the
  // second PT_LOAD segment.
  for (std::list<ElfSection*>::iterator section = second->begin();
       section != second->end(); ++section)
    first->addSection(*section);

  elf.removeSegment(second);
  elf.normalize();

  if (backup && backup_file(name) != 0) {
    fprintf(stderr, "Couln't create backup file\n");
  } else {
    std::ofstream ofile(name,
                        std::ios::out | std::ios::binary | std::ios::trunc);
    elf.write(ofile);
    fprintf(stderr, "Grown by %d bytes\n", elf.getSize() - size);
  }
}

int main(int argc, char* argv[]) {
  int arg;
  bool backup = false;
  bool force = false;
  bool revert = false;
  char* lastSlash = rindex(argv[0], '/');
  if (lastSlash != nullptr) rundir = strndup(argv[0], lastSlash - argv[0]);
  for (arg = 1; arg < argc; arg++) {
    if (strcmp(argv[arg], "-f") == 0)
      force = true;
    else if (strcmp(argv[arg], "-b") == 0)
      backup = true;
    else if (strcmp(argv[arg], "-r") == 0)
      revert = true;
    else if (revert) {
      undo_file(argv[arg], backup);
    } else
      do_file(argv[arg], backup, force);
  }

  free(rundir);
  return 0;
}
